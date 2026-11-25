#pragma once

#include "common/types.h"

void xmem_init();

// Console-semantics VirtualAlloc (accepts console allocation types, XMem flags and graphics page
// protections). Shared by XMemVirtualAlloc and the title's VirtualAlloc override.
void* xmem_virtual_alloc(void* base, usize size, u32 alloc_type, u64 xflags, u32 protect);

// XMemAlloc / XMemFree as the console runtime calls them: through the title's allocation hooks,
// which may hand out memory the title has set aside for that very request.
void* xmem_alloc(usize size, u64 attributes);
void xmem_free(void* p, u64 attributes);

// Memory types of graphics allocations (how the CPU side is cached).
#define XMEM_TYPE_GRAPHICS_WRITECOMBINE 0x1
#define XMEM_TYPE_GRAPHICS_CACHEABLE    0x3

// Attributes of graphics memory the console D3D12 runtime allocates for its objects (resources,
// descriptor heaps): D3D allocator, the given alignment (at least 64 KiB), and like the console
// runtime, 2 MiB pages for allocations of 2 MiB or more (titles' allocation hooks look at that).
u64 xmem_d3d_attributes(u32 object_type, u32 memory_type, u64 size, u64 alignment, bool gpu_32bit);
