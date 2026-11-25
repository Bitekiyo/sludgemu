#pragma once

#include "common/types.h"

// Console-style memory model on top of Win32.
//
// "Physical" memory is divided into 64 KiB page frames, backed by committed pagefile sections
// ("chunks") created as frames are allocated. Titles reserve placeholder ranges and map page
// frames into them themselves, so the same physical page can be visible at several virtual
// addresses, exactly as on console. Every chunk is also mapped once into a linear view, which
// gives us (and the GPU layer) a stable view of every page frame.
//
// Mappings are 64 KiB granular because console pages are (and Windows view granularity matches),
// but runs of consecutive frames are mapped as one view.
//
// Every range the title marks as GPU-visible is tracked, so the graphics layer can find the host
// memory behind title GPU addresses (mem_backing_block) and hear about mapping changes.

#define MEM_PAGE_SIZE  KiB(64)
#define MEM_PAGE_SHIFT 16

// Physical memory is committed in chunks of this size (each one a section; see memory.cpp).
#define MEM_CHUNK_SIZE  MiB(256)
#define MEM_CHUNK_PAGES (MEM_CHUNK_SIZE / MEM_PAGE_SIZE)
#define MEM_INVALID_PFN 0xFFFFFFFFu

// Page numbers exchanged with titles count 4 KiB units, like console page numbers: a page frame's
// number is its index shifted left by this, so it is a multiple of 16 and the next frame's number
// is 16 higher. Titles depend on that (some store page numbers without their low four bits).
#define MEM_PAGE_NUMBER_SHIFT 4

enum MemRangeFlags : u32 {
    MEM_RANGE_GPU = 1u << 0,         // GPU visible
    MEM_RANGE_PLACEHOLDER = 1u << 1, // reserved for physical page mapping
    MEM_RANGE_CPU_ACCESS = 1u << 2,  // CPU may access (not GPU-only)
    MEM_RANGE_OWNS_PAGES = 1u << 3,  // physical pages were allocated with the range; freed on release
    MEM_RANGE_LAZY = 1u << 4,        // committed by the title but reserved here, committed when touched
    MEM_RANGE_SECTION = 1u << 5, // a section the GPU sees through a view of its own (see mem_alloc_virtual)
};

// Description of a tracked range. Ranges can be released by another thread at any time, so they
// are only ever handed out by copy.
struct MemRange {
    u8* base;
    u64 size;
    u32 flags;   // MemRangeFlags
    u32 protect; // host page protection used for mappings
    u64 xflags;  // console allocation flags as passed by the title (for logging / later use)
};

bool mem_init(u64 physical_bytes);

// Physical page frames ------------------------------------------------------------------------

u64 mem_physical_total_pages();
u64 mem_physical_used_pages();

// Allocates `count` page frames (not necessarily contiguous) and stores their page numbers in
// `pfns`. Pages are committed and zeroed. Returns false (allocating nothing) if not enough are free.
bool mem_physical_alloc(u64 count, u64* pfns);
void mem_physical_free(u64 count, const u64* pfns);

// Host pointer to the page frame with page number `pfn`, through the linear view; nullptr if it
// isn't an allocated frame.
u8* mem_physical_ptr(u64 pfn);

// Physical memory chunks (committed sections, nullptr until first used). Chunk i holds frames
// [i * MEM_CHUNK_PAGES, (i + 1) * MEM_CHUNK_PAGES). Allocations that fit in a chunk get consecutive
// frames of one chunk where possible, so the GPU can see them as one block.
u32 mem_physical_chunk_count();
void* mem_physical_chunk(u32 index); // section HANDLE

// Where a host pointer into the linear view lies: its chunk and the offset in it. False for any
// other memory.
bool mem_linear_location(const void* host, u32* chunk, u64* offset);

// Virtual ranges ------------------------------------------------------------------------------

// Address constraints for new reservations (0 = no constraint).
struct MemAddressLimits {
    u64 lowest;
    u64 highest; // inclusive upper bound of the whole range
    u64 alignment;
};

// Reserves a placeholder range for later physical page mapping.
void* mem_reserve_placeholder(u64 size, const MemAddressLimits& limits, u32 range_flags, u32 protect,
                              u64 xflags);

// Reserves a placeholder range, allocates physical pages for all of it and maps them: committed
// memory that is explicitly backed by physical page frames.
void* mem_alloc_physical_backed(u64 size, const MemAddressLimits& limits, u32 range_flags, u32 protect,
                                u64 xflags);

// Reserves (+ optionally commits) ordinary memory with address constraints. Returns nullptr on
// failure. Ranges with MEM_RANGE_GPU are registered. Large commits are only reserved and get
// committed as they are touched, like mapped physical frames (see mem_map_physical).
//
// New GPU-visible ranges are views of a section, which the GPU sees through a second view of ours
// (MemBlock::host): heaps opened over memory lock its pages, and pages locked that way can't have
// their protection changed, so the title's own addresses must never be the ones locked. Sections
// can't decommit: decommitting part of such a range keeps it committed.
void* mem_alloc_virtual(void* base, u64 size, u32 alloc_type, u32 protect, const MemAddressLimits& limits,
                        u32 range_flags, u64 xflags);

// Maps page frames (by page number) into a placeholder range. pfns == nullptr unmaps. Each page
// may be at a different address (scatter) when `vas` is given; otherwise pages are consecutive from
// `va`.
//
// Mapped frames are backed by memory only when their pages are first touched: CPU accesses are
// caught and handled transparently, but anything that reaches title memory without faulting in user
// mode (kernel I/O, the GPU) must call mem_prepare first.
bool mem_map_physical(void* va, u64 count, const u64* pfns);
bool mem_map_physical_scatter(void* const* vas, u64 count, const u64* pfns);

// Backs anything committed or mapped by the title but not backed yet under [addr, addr + size) (see
// mem_alloc_virtual and mem_map_physical).
void mem_prepare(const void* addr, u64 size);

// Releases a range created by the functions above. Returns false if `base` isn't one of ours.
bool mem_release(void* base);

// Copies the description of the range containing `addr`; false if it isn't tracked.
bool mem_query_range(const void* addr, MemRange* out);

// A block of title memory backed by one contiguous block of a single host allocation, which is what
// a GPU heap can be opened over: a whole ordinary range, or in a placeholder range the longest run
// of pages around an address that map consecutive frames of one chunk (seen through the linear
// view).
struct MemBlock {
    u8* base; // title address
    u64 size;
    u8* host; // host address of the same memory
};

// The block holding `addr`. False if `addr` isn't tracked or isn't mapped.
bool mem_backing_block(const void* addr, MemBlock* out);

// The frames mapped at `count` pages from `va` in a placeholder range, by page number
// (MEM_INVALID_PFN where nothing is mapped). False if the pages aren't all in one placeholder range.
bool mem_frames_at(const void* va, u64 count, u64* pfns);

// Whether every page of [addr, addr + size) is tracked and has memory: committed, or mapped frames.
bool mem_is_mapped(const void* addr, u64 size);

// Called after page mappings change in [base, base + size) of a tracked range, or when the range is
// released. Called without memory locks held, possibly from any thread; must be cheap. Listeners are
// added once each, from one thread at a time.
typedef void (*MemMappingListener)(const u8* base, u64 size);
void mem_add_mapping_listener(MemMappingListener listener);

// Changes the host protection used for future mappings in the range containing `addr`.
void mem_set_range_protect(const void* addr, u32 protect);
bool mem_is_tracked(const void* addr);

// Graphics address window reported to the title (D3D12 virtual memory configuration).
void mem_set_gpu_window(u64 lowest, u64 highest);
void mem_gpu_window(u64* lowest, u64* highest);
