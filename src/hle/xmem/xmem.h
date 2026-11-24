#pragma once

#include "common/types.h"

void xmem_init();

// Console-semantics VirtualAlloc (accepts console allocation types, XMem flags and graphics page
// protections). Shared by XMemVirtualAlloc and the title's VirtualAlloc override.
void* xmem_virtual_alloc(void* base, usize size, u32 alloc_type, u64 xflags, u32 protect);
