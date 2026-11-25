#pragma once

#include "common/types.h"

// Console memory-API constants and structures. Values are the ABI the title was compiled
// against; names are our own.

// --- XMem allocation flags (64-bit "XMemFlags" argument) ---
#define XMEMF_CPU               0x1ull  // CPU accessible
#define XMEMF_GPU               0x2ull  // GPU accessible (graphics = CPU | GPU)
#define XMEMF_GPU_32BIT         0x4ull  // GPU address must fit in 32 bits
#define XMEMF_TOOL              0x10ull // tools partition
#define XMEMF_MAPPABLE          0x20ull // range will receive physical page mappings
#define XMEMF_2MB_ALIGN         0x40ull // 2 MiB aligned
#define XMEMF_BANDWIDTH_MASK    0x380ull
#define XMEMF_ADDRESS_BITS_MASK 0x7000ull // 0 = default, 1..7 = 46..40 address bits

// --- console additions to Win32 allocation types / protections ---
#define XMEM_TYPE_2MB_PAGES     0x20000000u // same bit as MEM_LARGE_PAGES on desktop
#define XMEM_TYPE_PHYSICAL      0x00400000u // same bit as MEM_PHYSICAL on desktop
#define XMEM_PROT_GRAPHICS_MASK 0x0007F800u // all PAGE_GRAPHICS_* bits (incl. coherent / nocache)

// --- XMemAlloc attribute word ---
// bits  0..13 object type, 14..15 page size, 16..23 allocator id, 24..28 alignment (log2),
// 29..32 memory type, 33 32-bit GPU address mapping.
static inline u32 xalloc_page_size_code(u64 a) {
    return (u32)((a >> 14) & 0x3);
}
static inline u32 xalloc_allocator_id(u64 a) {
    return (u32)((a >> 16) & 0xFF);
}
static inline u32 xalloc_alignment_log2(u64 a) {
    return (u32)((a >> 24) & 0x1F);
}
static inline u32 xalloc_memory_type(u64 a) {
    return (u32)((a >> 29) & 0xF);
}
static inline bool xalloc_32bit_gpu(u64 a) {
    return ((a >> 33) & 1) != 0;
}

#define XALLOC_PAGE_4K  0
#define XALLOC_PAGE_64K 1
#define XALLOC_PAGE_2M  2

#define XALLOC_TYPE_HEAP 0x0 // plain cacheable CPU heap; every other type is graphics/physical

#define XALLOC_ALLOCATOR_D3D 128

static inline u64 xalloc_make(u32 object_type, u32 page_size_code, u32 allocator_id, u32 alignment_log2,
                              u32 memory_type, bool gpu_32bit) {
    return (u64)(object_type & 0x3FFF) | ((u64)page_size_code << 14) | ((u64)allocator_id << 16) |
           ((u64)alignment_log2 << 24) | ((u64)memory_type << 29) | ((u64)gpu_32bit << 33);
}

// --- working-set statistics (layout must match the title's expectations) ---
struct XMemWorkingSetStats {
    u64 game_limit;
    u64 game_used;
    u64 tools_limit;
    u64 tools_used;
    u32 stack_used;
    u32 game_physical_pages;
    u64 gpu_optimal_limit;
    u64 gpu_optimal_used;
    u64 tools_gpu_optimal_limit;
    u64 tools_gpu_optimal_used;
    u32 kernel_pool_used;
    u32 kernel_pool_limit;
    u32 direct_storage_used;
    u32 frame_preserve_size;
    u64 game_used_peak;
    u32 gpu_page_table_size;
    u32 cpu_page_table_size;
    u64 reserved[2];
};
static_assert(sizeof(XMemWorkingSetStats) == 0x78, "XMemWorkingSetStats layout");

// XMemAlloc / XMemFree hooks a title may export as "__XMemAllocationHooks__".
typedef void*(__stdcall* XMemAllocHookFn)(usize size, u64 attributes);
typedef void(__stdcall* XMemFreeHookFn)(void* p, u64 attributes);
struct XMemAllocationHooks {
    XMemAllocHookFn alloc;
    XMemFreeHookFn free;
};
