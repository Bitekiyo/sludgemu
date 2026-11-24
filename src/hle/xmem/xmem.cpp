// xmem.dll: title memory management (virtual ranges, physical pages, default allocator).

#include <windows.h>

#include "common/log.h"
#include "core/memory.h"
#include "hle/hle.h"
#include "hle/xmem/xmem.h"
#include "hle/xmem/xmem_defs.h"
#include "loader/loader.h"

// Title-visible memory budget. Matches a typical console title partition; the backing section is
// sized a bit larger so we can hand out everything we report.
#define TITLE_MEMORY_LIMIT (GiB(13) + MiB(512))

static volatile u64 g_peak_used = 0;

// ---------------------------------------------------------------------------------------------
// Helpers shared with the VirtualAlloc override

static u32 cpu_protect_from_console(u32 protect) {
    u32 prot = protect & ~XMEM_PROT_GRAPHICS_MASK;
    // Write-combined / uncached CPU mappings only matter for the console memory controller.
    prot &= ~(PAGE_WRITECOMBINE | PAGE_NOCACHE);
    if ((prot & 0xFF) == 0) {
        // Graphics-only protection: derive the CPU side.
        if (protect & (PAGE_GRAPHICS_READWRITE | PAGE_GRAPHICS_EXECUTE_READWRITE | PAGE_GRAPHICS_COHERENT))
            prot |= PAGE_READWRITE;
        else if (protect & (PAGE_GRAPHICS_READONLY | PAGE_GRAPHICS_EXECUTE | PAGE_GRAPHICS_EXECUTE_READ))
            prot |= PAGE_READONLY;
        else
            prot |= PAGE_NOACCESS;
    }
    return prot;
}

static MemAddressLimits limits_from_flags(u64 xflags, u32 alloc_type) {
    MemAddressLimits l = {};
    bool gpu = (xflags & XMEMF_GPU) != 0;
    if (gpu)
        mem_gpu_window(&l.lowest, &l.highest);
    u64 bits_code = (xflags & XMEMF_ADDRESS_BITS_MASK) >> 12;
    if (bits_code) {
        u64 bits = 47 - bits_code; // 1 -> 46 bits ... 7 -> 40 bits
        u64 hi = (1ull << bits) - 1;
        if (!l.highest || hi < l.highest)
            l.highest = hi;
    }
    if (xflags & XMEMF_GPU_32BIT)
        l.highest = 0xFFFFFFFFull;
    if ((alloc_type & XMEM_TYPE_2MB_PAGES) || (xflags & XMEMF_2MB_ALIGN))
        l.alignment = MiB(2);
    return l;
}

void* xmem_virtual_alloc(void* base, usize size, u32 alloc_type, u64 xflags, u32 protect) {
    u32 cpu_prot = cpu_protect_from_console(protect);
    bool gpu = (xflags & XMEMF_GPU) || (protect & XMEM_PROT_GRAPHICS_MASK);
    u32 range_flags =
        (gpu ? MEM_RANGE_GPU : 0) | ((cpu_prot & 0xFF) != PAGE_NOACCESS ? MEM_RANGE_CPU_ACCESS : 0);
    MemAddressLimits limits = limits_from_flags(gpu ? (xflags | XMEMF_GPU) : xflags, alloc_type);

    void* p = nullptr;
    if (alloc_type & XMEM_TYPE_PHYSICAL) {
        // As with plain VirtualAlloc, MEM_COMMIT without a base address implies MEM_RESERVE.
        if (base || !(alloc_type & (MEM_RESERVE | MEM_COMMIT))) {
            LOG_E("xmem", "unsupported physical allocation (base=%p type=0x%x xflags=0x%llx prot=0x%x)", base,
                  alloc_type, xflags, protect);
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        if (alloc_type & MEM_COMMIT)
            p = mem_alloc_physical_backed(size, limits, range_flags, cpu_prot, xflags);
        else
            p = mem_reserve_placeholder(size, limits, range_flags, cpu_prot, xflags);
    } else {
        u32 type = alloc_type & (MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN | MEM_RESET | MEM_RESET_UNDO);
        p = mem_alloc_virtual(base, size, type, cpu_prot, limits, range_flags, xflags);
    }
    LogLevel lvl = p ? LOG_LEVEL_DEBUG : LOG_LEVEL_WARN;
    log_printf(lvl, "xmem", "VirtualAlloc(base=%p size=0x%llx type=0x%x xflags=0x%llx prot=0x%x) -> %p", base,
               (u64)size, alloc_type, xflags, protect, p);
    if (!p)
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}

// ---------------------------------------------------------------------------------------------
// Exports

static PVOID WINAPI XMemVirtualAlloc(PVOID base, SIZE_T size, DWORD alloc_type, ULONGLONG xflags,
                                     DWORD protect) {
    return xmem_virtual_alloc(base, size, alloc_type, xflags, protect);
}

static BOOL WINAPI XMemAllocatePhysicalPages(ULONGLONG xflags, PULONG_PTR count, PULONG_PTR pages) {
    static_assert(sizeof(ULONG_PTR) == sizeof(u64), "pfn array element size");
    u64 n = *count;
    if (!mem_physical_alloc(n, (u64*)pages)) {
        *count = 0;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    u64 used = mem_physical_used_pages() << MEM_PAGE_SHIFT;
    if (used > g_peak_used)
        g_peak_used = used;
    LOG_D("xmem", "AllocatePhysicalPages(xflags=0x%llx, %llu pages) -> first pfn 0x%llx, %llu MiB in use",
          xflags, n, n ? (u64)pages[0] : 0ull, used >> 20);
    return TRUE;
}

static BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR count, PULONG_PTR pages) {
    mem_physical_free(count, (const u64*)pages);
    LOG_D("xmem", "FreePhysicalPages(%llu pages)", (u64)count);
    return TRUE;
}

static PVOID WINAPI XMemMapPhysicalPages(PVOID va, ULONG_PTR count, PULONG_PTR pages) {
    if (!mem_map_physical(va, count, (const u64*)pages)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    LOG_T("xmem", "MapPhysicalPages(%p, %llu pages, %s)", va, (u64)count, pages ? "map" : "unmap");
    return va;
}

static BOOL WINAPI XMemMapPhysicalPagesScatter(PVOID* vas, ULONG_PTR count, PULONG_PTR pages) {
    if (!mem_map_physical_scatter(vas, count, (const u64*)pages)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    LOG_T("xmem", "MapPhysicalPagesScatter(%llu pages, %s)", (u64)count, pages ? "map" : "unmap");
    return TRUE;
}

static BOOL WINAPI XMemGetWorkingSetStatistics(DWORD flags, XMemWorkingSetStats* stats) {
    memset(stats, 0, sizeof(*stats));
    u64 phys_used = mem_physical_used_pages() << MEM_PAGE_SHIFT;
    stats->game_limit = TITLE_MEMORY_LIMIT;
    stats->game_used = phys_used;
    stats->game_physical_pages = (u32)mem_physical_used_pages();
    stats->gpu_optimal_limit = GiB(10);
    stats->gpu_optimal_used = phys_used < GiB(10) ? phys_used : GiB(10);
    stats->game_used_peak = g_peak_used;
    LOG_D("xmem", "GetWorkingSetStatistics(0x%lx): %llu MiB used", flags, phys_used >> 20);
    return TRUE;
}

static BOOL WINAPI XMemIsToolsMemAvailable() {
    return FALSE;
}

static BOOL WINAPI XMemTransferMemory(DWORD type, SIZE_T size) {
    LOG_I("xmem", "TransferMemory(type=%lu, %llu MiB): accepted, no-op", type, (u64)size >> 20);
    return TRUE;
}

// --- default allocator -------------------------------------------------------------------------
//
// Heap memory goes to the process heap with an alignment header; graphics memory types get their
// own page-granular GPU-visible ranges for now (sub-allocation can come later if it matters).

struct HeapBlockHeader {
    void* raw;
};

static void* WINAPI XMemAllocDefault(SIZE_T size, ULONGLONG attributes) {
    u32 type = xalloc_memory_type(attributes);
    u32 align_log2 = xalloc_alignment_log2(attributes);
    u64 align = align_log2 >= 2 ? (1ull << align_log2) : 16;

    if (type == XALLOC_TYPE_HEAP) {
        if (align < 16)
            align = 16;
        u8* raw = (u8*)HeapAlloc(GetProcessHeap(), 0, size + align + sizeof(HeapBlockHeader));
        if (!raw)
            return nullptr;
        u8* p = (u8*)ALIGN_UP((uptr)(raw + sizeof(HeapBlockHeader)), align);
        ((HeapBlockHeader*)p)[-1].raw = raw;
        return p;
    }

    u64 xflags = XMEMF_CPU | XMEMF_GPU | (xalloc_32bit_gpu(attributes) ? XMEMF_GPU_32BIT : 0);
    u32 alloc_type = MEM_RESERVE | MEM_COMMIT;
    if (xalloc_page_size_code(attributes) == XALLOC_PAGE_2M || align > MEM_PAGE_SIZE)
        xflags |= XMEMF_2MB_ALIGN;
    void* p = xmem_virtual_alloc(nullptr, ALIGN_UP(size, MEM_PAGE_SIZE), alloc_type, xflags, PAGE_READWRITE);
    LOG_D("xmem", "AllocDefault(0x%llx, attr=0x%llx type=%u) -> %p", (u64)size, attributes, type, p);
    return p;
}

static void WINAPI XMemFreeDefault(PVOID p, ULONGLONG attributes) {
    if (!p)
        return;
    if (xalloc_memory_type(attributes) == XALLOC_TYPE_HEAP) {
        HeapFree(GetProcessHeap(), 0, ((HeapBlockHeader*)p)[-1].raw);
        return;
    }
    if (!mem_release(p))
        LOG_W("xmem", "FreeDefault(%p): not a range we allocated", p);
}

static const XMemAllocationHooks* title_hooks() {
    static const XMemAllocationHooks* s_hooks = nullptr;
    static bool s_looked = false;
    if (!s_looked) {
        PeImage* main = g_loader.main_image();
        s_hooks = main ? (const XMemAllocationHooks*)main->find_export("__XMemAllocationHooks__") : nullptr;
        s_looked = true;
        LOG_I("xmem", "title allocation hooks: %s", s_hooks ? "present" : "none (using default allocator)");
    }
    return s_hooks;
}

static void* WINAPI XMemAlloc(SIZE_T size, ULONGLONG attributes) {
    const XMemAllocationHooks* h = title_hooks();
    if (h && h->alloc)
        return h->alloc(size, attributes);
    return XMemAllocDefault(size, attributes);
}

static void WINAPI XMemFree(PVOID p, ULONGLONG attributes) {
    const XMemAllocationHooks* h = title_hooks();
    if (h && h->free) {
        h->free(p, attributes);
        return;
    }
    XMemFreeDefault(p, attributes);
}

void xmem_init() {
    if (!mem_init(GiB(16)))
        FATAL("memory subsystem initialization failed");
}

static const HleExport k_exports[] = {
    HLE_EXPORT(XMemVirtualAlloc),
    HLE_EXPORT(XMemAllocatePhysicalPages),
    HLE_EXPORT(XMemFreePhysicalPages),
    HLE_EXPORT(XMemMapPhysicalPages),
    HLE_EXPORT(XMemMapPhysicalPagesScatter),
    HLE_EXPORT(XMemGetWorkingSetStatistics),
    HLE_EXPORT(XMemIsToolsMemAvailable),
    HLE_EXPORT(XMemTransferMemory),
    HLE_EXPORT(XMemAllocDefault),
    HLE_EXPORT(XMemFreeDefault),
    HLE_EXPORT(XMemAlloc),
    HLE_EXPORT(XMemFree),
};

extern const HleModule g_hle_module_xmem = {"xmem.dll", k_exports, ARRAY_COUNT(k_exports)};
