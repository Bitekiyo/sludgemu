#include "core/memory.h"

#include <windows.h>
#include <stdlib.h>

#include "common/log.h"

// One region of a placeholder range: a placeholder, or a view of consecutive page frames.
struct MemRegion {
    u32 first_page; // page index within the range; the region ends where the next one starts
    u32 pfn;        // frame mapped at first_page, or REGION_PLACEHOLDER
};
#define REGION_PLACEHOLDER 0xFFFFFFFFu

struct RangeSlot {
    MemRange info;
    MemRegion* regions; // placeholder ranges only; sorted by first_page, regions[0].first_page == 0
    u32 region_count;
    u32 region_capacity;
};

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

    RangeSlot ranges[MAX_RANGES];
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
//
// A placeholder range keeps a sorted list of regions that mirrors the address space exactly:
// every region is either one Win32 placeholder or one mapped view of consecutive page frames.
// All changes go through split_at / make_placeholder, which update Windows and the list together,
// so the two never disagree. A Win32 failure in there means they would, which is not recoverable.

static u32 range_pages(const RangeSlot* s) {
    return (u32)(s->info.size >> MEM_PAGE_SHIFT);
}

static u8* page_va(const RangeSlot* s, u32 page) {
    return s->info.base + ((u64)page << MEM_PAGE_SHIFT);
}

// Index of the region containing `page`.
static u32 region_find(const RangeSlot* s, u32 page) {
    u32 lo = 0, hi = s->region_count; // regions[0].first_page == 0
    while (hi - lo > 1) {
        u32 mid = lo + (hi - lo) / 2;
        if (s->regions[mid].first_page <= page)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

static u32 region_end(const RangeSlot* s, u32 index) {
    return index + 1 < s->region_count ? s->regions[index + 1].first_page : range_pages(s);
}

static void region_insert(RangeSlot* s, u32 at, MemRegion region) {
    if (s->region_count == s->region_capacity) {
        u32 capacity = s->region_capacity ? s->region_capacity * 2 : 16;
        MemRegion* grown = (MemRegion*)realloc(s->regions, capacity * sizeof(MemRegion));
        if (!grown)
            FATAL("out of memory tracking mappings");
        s->regions = grown;
        s->region_capacity = capacity;
    }
    memmove(&s->regions[at + 1], &s->regions[at], (s->region_count - at) * sizeof(MemRegion));
    s->regions[at] = region;
    s->region_count++;
}

// Replaces regions [first, last] with one region.
static void region_replace(RangeSlot* s, u32 first, u32 last, MemRegion region) {
    s->regions[first] = region;
    u32 removed = last - first;
    if (removed) {
        memmove(&s->regions[first + 1], &s->regions[last + 1],
                (s->region_count - last - 1) * sizeof(MemRegion));
        s->region_count -= removed;
    }
}

static u32 mapping_protect(const RangeSlot* s) {
    // GPU-only ranges are still mapped CPU-accessible on our side; the title never touches them on
    // the CPU, but we need to read them for uploads.
    u32 prot = s->info.protect;
    if ((prot & 0xFF) == PAGE_NOACCESS || !(prot & 0xFF))
        prot = PAGE_READWRITE;
    return prot;
}

static void win_split(u8* va, u64 bytes) {
    if (!VirtualFree(va, bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        FATAL("splitting placeholder at %p+0x%llx failed (%lu)", va, bytes, GetLastError());
}

static void win_coalesce(u8* va, u64 bytes) {
    if (!VirtualFree(va, bytes, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
        FATAL("coalescing placeholders at %p (0x%llx bytes) failed (%lu)", va, bytes, GetLastError());
}

static void win_unmap_to_placeholder(u8* va) {
    if (!g_mem.unmap_view2(GetCurrentProcess(), va, MEM_PRESERVE_PLACEHOLDER))
        FATAL("unmapping view at %p failed (%lu)", va, GetLastError());
}

// Maps `pages` consecutive frames starting at `pfn` (all in one chunk) over a placeholder that
// spans exactly [va, va + pages * MEM_PAGE_SIZE).
static void win_map(u8* va, u32 pfn, u32 pages, u32 protect) {
    HANDLE chunk = g_mem.chunks[pfn / MEM_CHUNK_PAGES];
    u64 offset = (u64)(pfn % MEM_CHUNK_PAGES) << MEM_PAGE_SHIFT;
    void* v = g_mem.map_view3(chunk, GetCurrentProcess(), va, offset, (u64)pages << MEM_PAGE_SHIFT,
                              MEM_REPLACE_PLACEHOLDER, protect, nullptr, 0);
    if (v != va)
        FATAL("mapping frames 0x%x+%u at %p failed (%lu)", pfn, pages, va, GetLastError());
}

// Ensures a region boundary at `page`. A view that straddles it is unmapped, split and mapped
// again as two views.
static void split_at(RangeSlot* s, u32 page) {
    if (page == 0 || page >= range_pages(s))
        return;
    u32 i = region_find(s, page);
    MemRegion r = s->regions[i];
    if (r.first_page == page)
        return;
    u32 end = region_end(s, i);
    u32 head = page - r.first_page;
    bool view = r.pfn != REGION_PLACEHOLDER;
    if (view)
        win_unmap_to_placeholder(page_va(s, r.first_page));
    win_split(page_va(s, r.first_page), (u64)head << MEM_PAGE_SHIFT);
    if (view) {
        u32 protect = mapping_protect(s);
        win_map(page_va(s, r.first_page), r.pfn, head, protect);
        win_map(page_va(s, page), r.pfn + head, end - page, protect);
    }
    MemRegion tail = {page, view ? r.pfn + head : REGION_PLACEHOLDER};
    region_insert(s, i + 1, tail);
}

// Turns [first, end) into exactly one placeholder and returns its region index.
static u32 make_placeholder(RangeSlot* s, u32 first, u32 end) {
    split_at(s, first);
    split_at(s, end);
    u32 i0 = region_find(s, first);
    u32 i1 = region_find(s, end - 1);
    for (u32 i = i0; i <= i1; i++) {
        if (s->regions[i].pfn != REGION_PLACEHOLDER)
            win_unmap_to_placeholder(page_va(s, s->regions[i].first_page));
    }
    if (i1 > i0)
        win_coalesce(page_va(s, first), (u64)(end - first) << MEM_PAGE_SHIFT);
    MemRegion merged = {first, REGION_PLACEHOLDER};
    region_replace(s, i0, i1, merged);
    return i0;
}

static void map_run(RangeSlot* s, u32 first, u32 count, u32 pfn) {
    u32 i = region_find(s, first);
    const MemRegion& r = s->regions[i];
    if (r.first_page == first && region_end(s, i) == first + count && r.pfn == pfn)
        return; // already mapped exactly like this
    i = make_placeholder(s, first, first + count);
    win_map(page_va(s, first), pfn, count, mapping_protect(s));
    s->regions[i].pfn = pfn;
}

static void unmap_run(RangeSlot* s, u32 first, u32 count) {
    make_placeholder(s, first, first + count);
}

static RangeSlot* find_range_locked(const void* addr) {
    for (u32 i = 0; i < g_mem.range_count; i++) {
        RangeSlot& s = g_mem.ranges[i];
        if ((const u8*)addr >= s.info.base && (const u8*)addr < s.info.base + s.info.size)
            return &s;
    }
    return nullptr;
}

bool mem_query_range(const void* addr, MemRange* out) {
    AcquireSRWLockShared(&g_mem.range_lock);
    RangeSlot* s = find_range_locked(addr);
    if (s)
        *out = s->info;
    ReleaseSRWLockShared(&g_mem.range_lock);
    return s != nullptr;
}

bool mem_is_tracked(const void* addr) {
    MemRange r;
    return mem_query_range(addr, &r);
}

void mem_set_range_protect(const void* addr, u32 protect) {
    AcquireSRWLockExclusive(&g_mem.range_lock);
    if (RangeSlot* s = find_range_locked(addr))
        s->info.protect = protect;
    ReleaseSRWLockExclusive(&g_mem.range_lock);
}

// Publishes a fully initialized range. `regions` (placeholder ranges only) is owned by the range
// from here on.
static void add_range(u8* base, u64 size, u32 flags, u32 protect, u64 xflags, MemRegion* regions) {
    AcquireSRWLockExclusive(&g_mem.range_lock);
    if (g_mem.range_count >= MAX_RANGES) {
        ReleaseSRWLockExclusive(&g_mem.range_lock);
        FATAL("too many memory ranges");
    }
    RangeSlot& s = g_mem.ranges[g_mem.range_count++];
    s.info.base = base;
    s.info.size = size;
    s.info.flags = flags;
    s.info.protect = protect;
    s.info.xflags = xflags;
    s.regions = regions;
    s.region_count = regions ? 1 : 0;
    s.region_capacity = regions ? 1 : 0;
    ReleaseSRWLockExclusive(&g_mem.range_lock);
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
    if (size == 0 || (size >> MEM_PAGE_SHIFT) > 0xFFFFFFFFull) {
        LOG_E("memory", "placeholder reservation of 0x%llx bytes is not supported", size);
        return nullptr;
    }
    MemRegion* regions = (MemRegion*)malloc(sizeof(MemRegion));
    if (!regions)
        FATAL("out of memory tracking mappings");
    regions[0].first_page = 0;
    regions[0].pfn = REGION_PLACEHOLDER;

    MEM_ADDRESS_REQUIREMENTS req;
    fill_address_requirements(limits, &req);
    MEM_EXTENDED_PARAMETER param = {};
    param.Type = MemExtendedParameterAddressRequirements;
    param.Pointer = &req;
    u8* base = (u8*)g_mem.virtual_alloc2(GetCurrentProcess(), nullptr, size,
                                         MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &param, 1);
    if (!base) {
        LOG_E("memory", "placeholder reservation of %llu MiB failed (%lu)", size >> 20, GetLastError());
        free(regions);
        return nullptr;
    }
    add_range(base, size, range_flags | MEM_RANGE_PLACEHOLDER, protect, xflags, regions);
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
            add_range((u8*)p, ALIGN_UP(size, MEM_PAGE_SIZE), range_flags, protect, xflags, nullptr);
    }
    return p;
}

// An allocated frame always has its chunk (both are set up together under the physical lock).
static bool frame_mappable(u64 pfn) {
    if (pfn >= g_mem.total_pages)
        return false;
    AcquireSRWLockShared(&g_mem.phys_lock);
    bool ok = bit_get(g_mem.used_bitmap, pfn) && g_mem.chunks[pfn / MEM_CHUNK_PAGES] != nullptr;
    ReleaseSRWLockShared(&g_mem.phys_lock);
    return ok;
}

// Maps (or unmaps, for MEM_INVALID_PFN entries or pfns == nullptr) `count` consecutive pages at
// `va`, one view per run of consecutive frames within a chunk. Range lock held exclusively.
static bool map_span_locked(void* va, u64 count, const u64* pfns) {
    RangeSlot* s = find_range_locked(va);
    if (!s || !(s->info.flags & MEM_RANGE_PLACEHOLDER) || ((uptr)va & (MEM_PAGE_SIZE - 1))) {
        LOG_E("memory", "physical map target %p is not a page in a placeholder range", va);
        return false;
    }
    u64 first = (u64)((u8*)va - s->info.base) >> MEM_PAGE_SHIFT;
    if (count > range_pages(s) - first) {
        LOG_E("memory", "physical map of %llu pages at %p runs past its range", count, va);
        return false;
    }
    // Validate everything before changing anything.
    for (u64 i = 0; pfns && i < count; i++) {
        if (pfns[i] != MEM_INVALID_PFN && !frame_mappable(pfns[i])) {
            LOG_E("memory", "frame 0x%llx mapped at %p is not an allocated page frame", pfns[i],
                  (u8*)va + (i << MEM_PAGE_SHIFT));
            return false;
        }
    }
    for (u64 i = 0; i < count;) {
        u64 pfn = pfns ? pfns[i] : MEM_INVALID_PFN;
        u64 run = 1;
        if (pfn == MEM_INVALID_PFN) {
            while (i + run < count && (!pfns || pfns[i + run] == MEM_INVALID_PFN))
                run++;
            unmap_run(s, (u32)(first + i), (u32)run);
        } else {
            while (i + run < count && pfns[i + run] == pfn + run && (pfn + run) % MEM_CHUNK_PAGES != 0)
                run++;
            map_run(s, (u32)(first + i), (u32)run, (u32)pfn);
        }
        i += run;
    }
    return true;
}

bool mem_map_physical(void* va, u64 count, const u64* pfns) {
    AcquireSRWLockExclusive(&g_mem.range_lock);
    bool ok = map_span_locked(va, count, pfns);
    ReleaseSRWLockExclusive(&g_mem.range_lock);
    return ok;
}

bool mem_map_physical_scatter(void* const* vas, u64 count, const u64* pfns) {
    AcquireSRWLockExclusive(&g_mem.range_lock);
    bool ok = true;
    // Entries at consecutive addresses form spans, which map_span_locked turns into runs.
    for (u64 i = 0; ok && i < count;) {
        u64 span = 1;
        while (i + span < count && (u8*)vas[i + span] == (u8*)vas[i] + (span << MEM_PAGE_SHIFT))
            span++;
        ok = map_span_locked(vas[i], span, pfns ? pfns + i : nullptr);
        i += span;
    }
    ReleaseSRWLockExclusive(&g_mem.range_lock);
    return ok;
}

void* mem_alloc_physical_backed(u64 size, const MemAddressLimits& limits, u32 range_flags, u32 protect,
                                u64 xflags) {
    size = ALIGN_UP(size, MEM_PAGE_SIZE);
    u64 pages = size >> MEM_PAGE_SHIFT;
    u64* pfns = (u64*)VirtualAlloc(nullptr, pages * sizeof(u64), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!pfns)
        return nullptr;
    if (!mem_physical_alloc(pages, pfns)) {
        VirtualFree(pfns, 0, MEM_RELEASE);
        return nullptr;
    }
    void* base = mem_reserve_placeholder(size, limits, range_flags | MEM_RANGE_OWNS_PAGES, protect, xflags);
    if (!base) {
        mem_physical_free(pages, pfns);
    } else if (!mem_map_physical(base, pages, pfns)) {
        // A failed map changes nothing, so none of the frames is mapped (owned) yet.
        mem_release(base);
        mem_physical_free(pages, pfns);
        base = nullptr;
    }
    VirtualFree(pfns, 0, MEM_RELEASE);
    return base;
}

bool mem_release(void* base) {
    AcquireSRWLockExclusive(&g_mem.range_lock);
    RangeSlot* s = find_range_locked(base);
    if (!s || s->info.base != base) {
        ReleaseSRWLockExclusive(&g_mem.range_lock);
        return false;
    }
    if (s->info.flags & MEM_RANGE_PLACEHOLDER) {
        // Each region is one placeholder or one view, so each is released exactly once. Unmapping
        // a view without MEM_PRESERVE_PLACEHOLDER frees its address space as well.
        bool owns = (s->info.flags & MEM_RANGE_OWNS_PAGES) != 0;
        for (u32 i = 0; i < s->region_count; i++) {
            const MemRegion& r = s->regions[i];
            u8* va = page_va(s, r.first_page);
            bool ok;
            if (r.pfn == REGION_PLACEHOLDER) {
                ok = VirtualFree(va, 0, MEM_RELEASE) != FALSE;
            } else {
                ok = g_mem.unmap_view2(GetCurrentProcess(), va, 0) != FALSE;
                if (owns) {
                    for (u32 p = 0; p < region_end(s, i) - r.first_page; p++) {
                        u64 pfn = r.pfn + p;
                        mem_physical_free(1, &pfn);
                    }
                }
            }
            if (!ok)
                LOG_E("memory", "releasing %p failed (%lu)", va, GetLastError());
        }
        free(s->regions);
    } else {
        VirtualFree(s->info.base, 0, MEM_RELEASE);
    }
    *s = g_mem.ranges[--g_mem.range_count];
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
