// VirtualAlloc / VirtualFree as seen by the title. On console these accept graphics page
// protections and console allocation types, and must interoperate with XMem ranges.

#include <windows.h>

#include "common/log.h"
#include "core/memory.h"
#include "hle/kernel/overrides.h"
#include "hle/xmem/xmem.h"
#include "hle/xmem/xmem_defs.h"

static bool is_console_specific(DWORD type, DWORD protect) {
    return (type & (XMEM_TYPE_2MB_PAGES | XMEM_TYPE_PHYSICAL)) || (protect & XMEM_PROT_GRAPHICS_MASK);
}

static LPVOID WINAPI hk_VirtualAlloc(LPVOID base, SIZE_T size, DWORD type, DWORD protect) {
    if (is_console_specific(type, protect) || (base && mem_is_tracked(base)))
        return xmem_virtual_alloc(base, size, type, XMEMF_CPU, protect);
    return VirtualAlloc(base, size, type, protect);
}

static BOOL WINAPI hk_VirtualFree(LPVOID base, SIZE_T size, DWORD type) {
    if (base && mem_is_tracked(base)) {
        MemRange r;
        bool placeholder = mem_query_range(base, &r) && (r.flags & MEM_RANGE_PLACEHOLDER);
        if ((type & MEM_PRESERVE_PLACEHOLDER) && placeholder) {
            // Splitting a placeholder: our placeholder ranges split per page when mapped.
            return TRUE;
        }
        if (type & MEM_RELEASE) {
            bool ok = mem_release(base);
            LOG_D("kernel", "VirtualFree(%p, MEM_RELEASE) on tracked range -> %d", base, ok);
            return ok;
        }
        if (placeholder) {
            LOG_W("kernel", "VirtualFree(%p, 0x%llx, 0x%lx) on a physical-mapping range ignored", base,
                  (u64)size, type);
            return TRUE;
        }
    }
    return VirtualFree(base, size, type);
}

const HleExport g_memory_overrides[] = {
    HLE_EXPORT_AS(hk_VirtualAlloc, "VirtualAlloc"),
    HLE_EXPORT_AS(hk_VirtualFree, "VirtualFree"),
};
const u32 g_memory_override_count = ARRAY_COUNT(g_memory_overrides);
