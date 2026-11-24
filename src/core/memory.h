#pragma once

#include "common/types.h"

// Console-style memory model on top of Win32.
//
// "Physical" memory is a pagefile-backed section (reserved, committed on allocation) divided into
// 64 KiB page frames. Titles reserve placeholder ranges and map page frames into them themselves,
// so the same physical page can be visible at several virtual addresses, exactly as on console.
// The whole section is also mapped once linearly, which gives us (and later the GPU layer) a
// stable view of every page frame.
//
// Every range the title marks as GPU-visible is recorded in a registry that the graphics layer
// uses to translate title GPU addresses.

#define MEM_PAGE_SIZE  KiB(64)
#define MEM_PAGE_SHIFT 16

// Physical memory is committed in chunks of this size (each one a section; see memory.cpp).
#define MEM_CHUNK_SIZE  MiB(256)
#define MEM_CHUNK_PAGES (MEM_CHUNK_SIZE / MEM_PAGE_SIZE)
#define MEM_INVALID_PFN 0xFFFFFFFFu

enum MemRangeFlags : u32 {
    MEM_RANGE_GPU = 1u << 0,         // GPU visible
    MEM_RANGE_PLACEHOLDER = 1u << 1, // reserved for physical page mapping
    MEM_RANGE_CPU_ACCESS = 1u << 2,  // CPU may access (not GPU-only)
    MEM_RANGE_OWNS_PAGES = 1u << 3,  // physical pages were allocated with the range; freed on release
};

struct MemRange {
    u8* base;
    u64 size;
    u32 flags;     // MemRangeFlags
    u32 protect;   // host page protection used for mappings
    u64 xflags;    // console allocation flags as passed by the title (for logging / later use)
    u32* page_pfn; // placeholder ranges: physical page mapped at each 64 KiB page, or a PAGE_STATE_*
};

bool mem_init(u64 physical_bytes);

// Physical page frames ------------------------------------------------------------------------

u64 mem_physical_total_pages();
u64 mem_physical_used_pages();

// Allocates `count` page frames (not necessarily contiguous) into `pfns`. Pages are committed and
// zeroed. Returns false (allocating nothing) if not enough are free.
bool mem_physical_alloc(u64 count, u64* pfns);
void mem_physical_free(u64 count, const u64* pfns);

// Host pointer to a page frame through the linear view.
u8* mem_physical_ptr(u64 pfn);

// Physical memory chunks (committed sections, nullptr until first used), for opening them as GPU
// heaps. Chunk i holds frames [i * MEM_CHUNK_PAGES, (i + 1) * MEM_CHUNK_PAGES).
u32 mem_physical_chunk_count();
void* mem_physical_chunk(u32 index); // section HANDLE

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
// failure. Ranges with MEM_RANGE_GPU are registered.
void* mem_alloc_virtual(void* base, u64 size, u32 alloc_type, u32 protect, const MemAddressLimits& limits,
                        u32 range_flags, u64 xflags);

// Maps page frames into a placeholder range. pfns == nullptr unmaps. Each page may be at a
// different address (scatter) when `vas` is given; otherwise pages are consecutive from `va`.
bool mem_map_physical(void* va, u64 count, const u64* pfns);
bool mem_map_physical_scatter(void* const* vas, u64 count, const u64* pfns);

// Releases a range created by the functions above. Returns false if `base` isn't one of ours.
bool mem_release(void* base);

const MemRange* mem_find_range(const void* addr);

// Changes the host protection used for future mappings in the range containing `addr`.
void mem_set_range_protect(const void* addr, u32 protect);
bool mem_is_tracked(const void* addr);

// Graphics address window reported to the title (D3D12 virtual memory configuration).
void mem_set_gpu_window(u64 lowest, u64 highest);
void mem_gpu_window(u64* lowest, u64* highest);
