#include "core/memory.h"

#include <windows.h>

#include "common/log.h"

// Placeholder-page states stored in MemRange::page_pfn alongside real PFNs.
#define PAGE_STATE_UNSPLIT 0xFFFFFFFFu // still part of a larger placeholder
#define PAGE_STATE_SPLIT   0xFFFFFFFEu // its own 64 KiB placeholder, nothing mapped

#define MAX_RANGES 8192
#define MAX_CHUNKS 256

// Win32 functions from newer API sets, resolved at runtime.
typedef PVOID(WINAPI* VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
typedef PVOID(WINAPI* MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                        MEM_EXTENDED_PARAMETER*, ULONG);
typedef BOOL(WINAPI* UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);

struct MemoryState {
    VirtualAlloc2Fn virtual_alloc2;
    MapViewOfFile3Fn map_view3;
    UnmapViewOfFile2Fn unmap_view2;

    // Physical memory is a set of committed sections ("chunks") created as pages get allocated, so
    // the commit charge follows actual use. Committed sections can also be opened as D3D12 heaps,
    // which makes every page GPU-visible in place.
    HANDLE chunks[MAX_CHUNKS];
    u32 chunk_count; // chunks covering all of physical memory (created or not)
    u8* linear;      // every chunk is mapped here, at pfn * MEM_PAGE_SIZE
    u64 total_pages;
    u64 used_pages;
    u64* used_bitmap;  // 1 = allocated
    u64* dirty_bitmap; // 1 = was used before, must be zeroed on next allocation
    u64 search_hint;
    SRWLOCK phys_lock;

    MemRange ranges[MAX_RANGES];
    u32 range_count;
    SRWLOCK range_lock;

    u64 gpu_lowest;
    u64 gpu_highest;
};

static MemoryState g_mem;

static inline bool bit_get(const u64* bm, u64 i) {
    return (bm[i >> 6] >> (i & 63)) & 1;
}
static inline void bit_set(u64* bm, u64 i) {
    bm[i >> 6] |= 1ull << (i & 63);
}
static inline void bit_clear(u64* bm, u64 i) {
    bm[i >> 6] &= ~(1ull << (i & 63));
}

bool mem_init(u64 physical_bytes) {
    InitializeSRWLock(&g_mem.phys_lock);
    InitializeSRWLock(&g_mem.range_lock);

    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    g_mem.virtual_alloc2 = (VirtualAlloc2Fn)GetProcAddress(kb, "VirtualAlloc2");
    g_mem.map_view3 = (MapViewOfFile3Fn)GetProcAddress(kb, "MapViewOfFile3");
    g_mem.unmap_view2 = (UnmapViewOfFile2Fn)GetProcAddress(kb, "UnmapViewOfFile2");
    if (!g_mem.virtual_alloc2 || !g_mem.map_view3 || !g_mem.unmap_view2) {
        LOG_E("memory", "placeholder APIs unavailable (Windows 10 1803+ required)");
        return false;
    }

    physical_bytes = ALIGN_UP(physical_bytes, MEM_CHUNK_SIZE);
    g_mem.total_pages = physical_bytes >> MEM_PAGE_SHIFT;
    g_mem.chunk_count = (u32)(physical_bytes / MEM_CHUNK_SIZE);
    if (g_mem.chunk_count > MAX_CHUNKS) {
        LOG_E("memory", "physical memory size %llu MiB exceeds %u chunks", physical_bytes >> 20, MAX_CHUNKS);
        return false;
    }
    // The linear view is one placeholder per chunk; a chunk's section replaces its placeholder when
    // the chunk is created.
    g_mem.linear =
        (u8*)g_mem.virtual_alloc2(GetCurrentProcess(), nullptr, physical_bytes,
                                  MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (!g_mem.linear) {
        LOG_E("memory", "cannot reserve the physical memory view (%lu)", GetLastError());
        return false;
    }
    for (u32 i = 0; i + 1 < g_mem.chunk_count; i++) {
        if (!VirtualFree(g_mem.linear + (u64)i * MEM_CHUNK_SIZE, MEM_CHUNK_SIZE,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            LOG_E("memory", "cannot split the physical memory view (%lu)", GetLastError());
            return false;
        }
    }
    u64 words = (g_mem.total_pages + 63) / 64;
    g_mem.used_bitmap = (u64*)VirtualAlloc(nullptr, words * 8 * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_mem.dirty_bitmap = g_mem.used_bitmap + words;

    g_mem.gpu_lowest = GiB(2);
    g_mem.gpu_highest = (1ull << 40) - 1;

    LOG_I("memory", "physical memory: %llu MiB (%llu pages), linear view at %p", physical_bytes >> 20,
          g_mem.total_pages, g_mem.linear);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Physical pages

u64 mem_physical_total_pages() {
    return g_mem.total_pages;
}
u64 mem_physical_used_pages() {
    return g_mem.used_pages;
}
u8* mem_physical_ptr(u64 pfn) {
    return g_mem.linear + (pfn << MEM_PAGE_SHIFT);
}

u32 mem_physical_chunk_count() {
    return g_mem.chunk_count;
}
void* mem_physical_chunk(u32 index) {
    return index < g_mem.chunk_count ? g_mem.chunks[index] : nullptr;
}

// Creates the section for a chunk and maps it into its slot of the linear view. Called with
// phys_lock held.
static void create_chunk(u32 index) {
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                        (DWORD)(MEM_CHUNK_SIZE >> 32), (DWORD)MEM_CHUNK_SIZE, nullptr);
    if (!section)
        FATAL("cannot commit %llu MiB of physical memory (%lu)", MEM_CHUNK_SIZE >> 20, GetLastError());
    u8* slot = g_mem.linear + (u64)index * MEM_CHUNK_SIZE;
    if (!g_mem.map_view3(section, GetCurrentProcess(), slot, 0, MEM_CHUNK_SIZE, MEM_REPLACE_PLACEHOLDER,
                         PAGE_READWRITE, nullptr, 0))
        FATAL("cannot map physical memory chunk %u (%lu)", index, GetLastError());
    g_mem.chunks[index] = section;
    LOG_D("memory", "physical chunk %u committed (%llu MiB)", index, MEM_CHUNK_SIZE >> 20);
}

bool mem_physical_alloc(u64 count, u64* pfns) {
    AcquireSRWLockExclusive(&g_mem.phys_lock);
    if (g_mem.used_pages + count > g_mem.total_pages) {
        ReleaseSRWLockExclusive(&g_mem.phys_lock);
        LOG_W("memory", "out of physical memory: want %llu pages, %llu free", count,
              g_mem.total_pages - g_mem.used_pages);
        return false;
    }
    u64 found = 0;
    u64 i = g_mem.search_hint;
    for (u64 scanned = 0; found < count && scanned < g_mem.total_pages; scanned++, i++) {
        if (i >= g_mem.total_pages)
            i = 0;
        if (!bit_get(g_mem.used_bitmap, i)) {
            bit_set(g_mem.used_bitmap, i);
            pfns[found++] = i;
        }
    }
    SLUDGE_ASSERT(found == count);
    g_mem.search_hint = i;
    g_mem.used_pages += count;

    // Make sure the frames' chunks exist; recycled frames are zeroed (new chunks come zeroed).
    for (u64 k = 0; k < count; k++) {
        u32 chunk = (u32)(pfns[k] / MEM_CHUNK_PAGES);
        if (!g_mem.chunks[chunk])
            create_chunk(chunk);
        if (bit_get(g_mem.dirty_bitmap, pfns[k])) {
            memset(mem_physical_ptr(pfns[k]), 0, MEM_PAGE_SIZE);
            bit_clear(g_mem.dirty_bitmap, pfns[k]);
        }
    }
    ReleaseSRWLockExclusive(&g_mem.phys_lock);
    return true;
}

void mem_physical_free(u64 count, const u64* pfns) {
    AcquireSRWLockExclusive(&g_mem.phys_lock);
    for (u64 k = 0; k < count; k++) {
        u64 pfn = pfns[k];
        if (pfn >= g_mem.total_pages || !bit_get(g_mem.used_bitmap, pfn)) {
            LOG_W("memory", "freeing invalid/unallocated page frame 0x%llx", pfn);
            continue;
        }
        bit_clear(g_mem.used_bitmap, pfn);
        bit_set(g_mem.dirty_bitmap, pfn);
        g_mem.used_pages--;
        // Hand the RAM back; the frame is zeroed when it's allocated again.
        DiscardVirtualMemory(mem_physical_ptr(pfn), MEM_PAGE_SIZE);
    }
    ReleaseSRWLockExclusive(&g_mem.phys_lock);
}

// ---------------------------------------------------------------------------------------------
// Ranges

static MemRange* find_range_locked(const void* addr) {
    for (u32 i = 0; i < g_mem.range_count; i++) {
        MemRange& r = g_mem.ranges[i];
        if ((const u8*)addr >= r.base && (const u8*)addr < r.base + r.size)
            return &r;
    }
    return nullptr;
}

const MemRange* mem_find_range(const void* addr) {
    AcquireSRWLockShared(&g_mem.range_lock);
    MemRange* r = find_range_locked(addr);
    ReleaseSRWLockShared(&g_mem.range_lock);
    return r;
}

bool mem_is_tracked(const void* addr) {
    return mem_find_range(addr) != nullptr;
}

void mem_set_range_protect(const void* addr, u32 protect) {
    AcquireSRWLockExclusive(&g_mem.range_lock);
    if (MemRange* r = find_range_locked(addr))
        r->protect = protect;
    ReleaseSRWLockExclusive(&g_mem.range_lock);
}

static MemRange* add_range(u8* base, u64 size, u32 flags, u32 protect, u64 xflags) {
    AcquireSRWLockExclusive(&g_mem.range_lock);
    if (g_mem.range_count >= MAX_RANGES) {
        ReleaseSRWLockExclusive(&g_mem.range_lock);
        FATAL("too many memory ranges");
    }
    MemRange* r = &g_mem.ranges[g_mem.range_count++];
    r->base = base;
    r->size = size;
    r->flags = flags;
    r->protect = protect;
    r->xflags = xflags;
    r->page_pfn = nullptr;
    ReleaseSRWLockExclusive(&g_mem.range_lock);
    return r;
}

static void fill_address_requirements(const MemAddressLimits& limits, MEM_ADDRESS_REQUIREMENTS* req) {
    memset(req, 0, sizeof(*req));
    req->LowestStartingAddress = (PVOID)ALIGN_UP(limits.lowest, MEM_PAGE_SIZE);
    if (limits.highest)
        req->HighestEndingAddress = (PVOID)(ALIGN_DOWN(limits.highest + 1, MEM_PAGE_SIZE) - 1);
    req->Alignment = limits.alignment > MEM_PAGE_SIZE ? limits.alignment : 0;
}

void* mem_reserve_placeholder(u64 size, const MemAddressLimits& limits, u32 range_flags, u32 protect,
                              u64 xflags) {
    size = ALIGN_UP(size, MEM_PAGE_SIZE);
    MEM_ADDRESS_REQUIREMENTS req;
    fill_address_requirements(limits, &req);
    MEM_EXTENDED_PARAMETER param = {};
    param.Type = MemExtendedParameterAddressRequirements;
    param.Pointer = &req;
    u8* base = (u8*)g_mem.virtual_alloc2(GetCurrentProcess(), nullptr, size,
                                         MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &param, 1);
    if (!base) {
        LOG_E("memory", "placeholder reservation of %llu MiB failed (%lu)", size >> 20, GetLastError());
        return nullptr;
    }
    MemRange* r = add_range(base, size, range_flags | MEM_RANGE_PLACEHOLDER, protect, xflags);
    u64 pages = size >> MEM_PAGE_SHIFT;
    r->page_pfn = (u32*)VirtualAlloc(nullptr, pages * sizeof(u32), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    memset(r->page_pfn, 0xFF, pages * sizeof(u32)); // PAGE_STATE_UNSPLIT
    LOG_I("memory", "placeholder range %p..%p (%llu MiB)%s", base, base + size, size >> 20,
          (range_flags & MEM_RANGE_GPU) ? " [gpu]" : "");
    return base;
}

void* mem_alloc_virtual(void* base, u64 size, u32 alloc_type, u32 protect, const MemAddressLimits& limits,
                        u32 range_flags, u64 xflags) {
    void* p;
    if (base) {
        // Committing inside an existing reservation (or a fixed-address request): no constraints.
        p = VirtualAlloc(base, size, alloc_type, protect);
    } else {
        MEM_ADDRESS_REQUIREMENTS req;
        fill_address_requirements(limits, &req);
        MEM_EXTENDED_PARAMETER param = {};
        param.Type = MemExtendedParameterAddressRequirements;
        param.Pointer = &req;
        p = g_mem.virtual_alloc2(GetCurrentProcess(), nullptr, size, alloc_type, protect, &param, 1);
    }
    if (!p)
        return nullptr;
    // Fresh reservations are tracked; commits into an existing tracked range are not new ranges.
    if (!base || !mem_is_tracked(base)) {
        if (range_flags & MEM_RANGE_GPU)
            add_range((u8*)p, ALIGN_UP(size, MEM_PAGE_SIZE), range_flags, protect, xflags);
    }
    return p;
}

// Maps (or unmaps, pfn == MEM_INVALID_PFN) one 64 KiB page of a placeholder range.
static bool map_page(MemRange* r, u64 page, u32 pfn) {
    u8* va = r->base + (page << MEM_PAGE_SHIFT);
    u32 state = r->page_pfn[page];
    HANDLE proc = GetCurrentProcess();

    if (state != PAGE_STATE_UNSPLIT && state != PAGE_STATE_SPLIT) {
        if (!g_mem.unmap_view2(proc, va, MEM_PRESERVE_PLACEHOLDER)) {
            LOG_E("memory", "unmap of %p failed (%lu)", va, GetLastError());
            return false;
        }
        state = PAGE_STATE_SPLIT;
    } else if (state == PAGE_STATE_UNSPLIT && pfn != MEM_INVALID_PFN) {
        // Carve this page out as its own placeholder. Fails harmlessly if it already is the last
        // remaining page of the original placeholder.
        VirtualFree(va, MEM_PAGE_SIZE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        state = PAGE_STATE_SPLIT;
    }
    r->page_pfn[page] = state;
    if (pfn == MEM_INVALID_PFN)
        return true;

    // GPU-only ranges are still mapped CPU-accessible on our side; the title never touches them on
    // the CPU, but we need to read them for uploads.
    u32 prot = r->protect;
    if ((prot & 0xFF) == PAGE_NOACCESS || !(prot & 0xFF))
        prot = PAGE_READWRITE;
    void* v = g_mem.map_view3(g_mem.chunks[pfn / MEM_CHUNK_PAGES], proc, va,
                              (pfn % MEM_CHUNK_PAGES) << MEM_PAGE_SHIFT, MEM_PAGE_SIZE,
                              MEM_REPLACE_PLACEHOLDER, prot, nullptr, 0);
    if (v != va) {
        LOG_E("memory", "mapping pfn 0x%x at %p failed (%lu)", pfn, va, GetLastError());
        return false;
    }
    r->page_pfn[page] = pfn;
    return true;
}

static bool map_one(void* va, u64 pfn_or_invalid) {
    AcquireSRWLockExclusive(&g_mem.range_lock);
    MemRange* r = find_range_locked(va);
    bool ok = false;
    if (!r || !(r->flags & MEM_RANGE_PLACEHOLDER) || ((uptr)va & (MEM_PAGE_SIZE - 1))) {
        LOG_E("memory", "physical map target %p is not a page in a placeholder range", va);
    } else {
        ok = map_page(r, (u64)((u8*)va - r->base) >> MEM_PAGE_SHIFT, (u32)pfn_or_invalid);
    }
    ReleaseSRWLockExclusive(&g_mem.range_lock);
    return ok;
}

bool mem_map_physical(void* va, u64 count, const u64* pfns) {
    for (u64 i = 0; i < count; i++) {
        if (!map_one((u8*)va + (i << MEM_PAGE_SHIFT), pfns ? pfns[i] : MEM_INVALID_PFN))
            return false;
    }
    return true;
}

void* mem_alloc_physical_backed(u64 size, const MemAddressLimits& limits, u32 range_flags, u32 protect,
                                u64 xflags) {
    size = ALIGN_UP(size, MEM_PAGE_SIZE);
    u64 pages = size >> MEM_PAGE_SHIFT;
    u64* pfns = (u64*)VirtualAlloc(nullptr, pages * sizeof(u64), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!pfns || !mem_physical_alloc(pages, pfns)) {
        if (pfns)
            VirtualFree(pfns, 0, MEM_RELEASE);
        return nullptr;
    }
    void* base = mem_reserve_placeholder(size, limits, range_flags | MEM_RANGE_OWNS_PAGES, protect, xflags);
    if (!base || !mem_map_physical(base, pages, pfns)) {
        if (base)
            mem_release(base);
        else
            mem_physical_free(pages, pfns);
        VirtualFree(pfns, 0, MEM_RELEASE);
        return nullptr;
    }
    VirtualFree(pfns, 0, MEM_RELEASE);
    return base;
}

bool mem_map_physical_scatter(void* const* vas, u64 count, const u64* pfns) {
    for (u64 i = 0; i < count; i++) {
        if (!map_one(vas[i], pfns ? pfns[i] : MEM_INVALID_PFN))
            return false;
    }
    return true;
}

bool mem_release(void* base) {
    AcquireSRWLockExclusive(&g_mem.range_lock);
    MemRange* r = find_range_locked(base);
    if (!r || r->base != base) {
        ReleaseSRWLockExclusive(&g_mem.range_lock);
        return false;
    }
    if (r->flags & MEM_RANGE_PLACEHOLDER) {
        // Every piece of address space is released exactly once:
        //  - a mapped page: unmapping without MEM_PRESERVE_PLACEHOLDER frees its address space,
        //  - a split, unmapped page: it is its own placeholder,
        //  - a run of unsplit pages: one placeholder (pages are only ever carved out of the run
        //    around them, so a maximal run of unsplit pages is exactly one placeholder).
        u64 pages = r->size >> MEM_PAGE_SHIFT;
        bool owns = (r->flags & MEM_RANGE_OWNS_PAGES) != 0;
        HANDLE proc = GetCurrentProcess();
        for (u64 p = 0; p < pages;) {
            u8* va = r->base + (p << MEM_PAGE_SHIFT);
            u32 state = r->page_pfn[p];
            u64 run = 1;
            bool ok;
            if (state == PAGE_STATE_UNSPLIT) {
                while (p + run < pages && r->page_pfn[p + run] == PAGE_STATE_UNSPLIT)
                    run++;
                ok = VirtualFree(va, 0, MEM_RELEASE) != FALSE;
            } else if (state == PAGE_STATE_SPLIT) {
                ok = VirtualFree(va, 0, MEM_RELEASE) != FALSE;
            } else {
                ok = g_mem.unmap_view2(proc, va, 0) != FALSE;
                if (owns) {
                    u64 pfn = state;
                    mem_physical_free(1, &pfn);
                }
            }
            if (!ok)
                LOG_E("memory", "releasing %p (%llu pages) failed (%lu)", va, run, GetLastError());
            p += run;
        }
        VirtualFree(r->page_pfn, 0, MEM_RELEASE);
    } else {
        VirtualFree(r->base, 0, MEM_RELEASE);
    }
    *r = g_mem.ranges[--g_mem.range_count];
    ReleaseSRWLockExclusive(&g_mem.range_lock);
    return true;
}

void mem_set_gpu_window(u64 lowest, u64 highest) {
    g_mem.gpu_lowest = lowest;
    g_mem.gpu_highest = highest;
}

void mem_gpu_window(u64* lowest, u64* highest) {
    *lowest = g_mem.gpu_lowest;
    *highest = g_mem.gpu_highest;
}
