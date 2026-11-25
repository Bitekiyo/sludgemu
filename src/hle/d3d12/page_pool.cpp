// Page pools: title memory registered as the source of GPU page mapping copies
// (ID3D12CommandQueue::CopyPageMappingsX), which titles use to stream resources in and out without
// touching CPU mappings.

#include <stdlib.h>

#include "common/log.h"
#include "core/memory.h"
#include "hle/d3d12/d3d12_objects.h"

#define MAX_PAGE_POOLS 64

struct PagePool {
    u64 base; // 0 = free slot
    u32 page_count;
    u32 flags;
};

static struct {
    PagePool pools[MAX_PAGE_POOLS];
    SRWLOCK lock;
} g_page_pools = {{}, SRWLOCK_INIT};

#define REGISTER_PAGE_POOL_FLAG_2MB_PAGES 0x1

HRESULT console_register_page_pool(u64 address, u32 page_count, u32 flags, HANDLE* handle) {
    if (!handle || page_count == 0 || (address & (MEM_PAGE_SIZE - 1)))
        return E_INVALIDARG;
    MemRange range;
    if (!mem_query_range((const void*)address, &range) ||
        (u64)page_count * MEM_PAGE_SIZE > (u64)(range.base + range.size) - address) {
        LOG_E("d3d12", "page pool 0x%llx (%u pages) is not inside one range of title memory", address,
              page_count);
        return E_INVALIDARG;
    }
    AcquireSRWLockExclusive(&g_page_pools.lock);
    u32 slot = 0;
    while (slot < MAX_PAGE_POOLS && g_page_pools.pools[slot].base)
        slot++;
    if (slot < MAX_PAGE_POOLS)
        g_page_pools.pools[slot] = {address, page_count, flags};
    ReleaseSRWLockExclusive(&g_page_pools.lock);
    if (slot == MAX_PAGE_POOLS)
        return E_OUTOFMEMORY;
    *handle = (HANDLE)(uptr)(slot + 1);
    LOG_I("d3d12", "page pool %u: 0x%llx, %u pages (%llu MiB)%s", slot, address, page_count,
          ((u64)page_count * MEM_PAGE_SIZE) >> 20,
          (flags & REGISTER_PAGE_POOL_FLAG_2MB_PAGES) ? ", 2 MiB pages" : "");
    return S_OK;
}

HRESULT console_unregister_page_pool(HANDLE handle) {
    uptr slot = (uptr)handle - 1;
    if (slot >= MAX_PAGE_POOLS)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&g_page_pools.lock);
    bool registered = g_page_pools.pools[slot].base != 0;
    g_page_pools.pools[slot] = {};
    ReleaseSRWLockExclusive(&g_page_pools.lock);
    return registered ? S_OK : E_INVALIDARG;
}

// Maps pool frames at the destination right away: titles change mappings of memory the GPU is done
// with (they wait on fences first), so doing it on the CPU timeline keeps the order that matters.
HRESULT console_copy_page_mappings(u64 destination, u32 range_count, const ConsolePageRange* ranges, u64 pool,
                                   u32 pool_pages, u32 flags) {
    if (!ranges && range_count)
        return E_INVALIDARG;
    const u64 unit = (flags & PAGE_MAPPING_FLAG_2MB_PAGES) ? MiB(2) / MEM_PAGE_SIZE : 1; // our pages per page
    u64 dest = destination;
    u64* frames = nullptr;
    u64 frames_capacity = 0;
    HRESULT result = S_OK;
    for (u32 i = 0; i < range_count && SUCCEEDED(result); i++) {
        const ConsolePageRange& r = ranges[i];
        u64 pages = (u64)r.page_count * unit;
        u64 bytes = pages << MEM_PAGE_SHIFT;
        switch (r.type) {
        case PAGE_RANGE_SKIP:
            break;
        case PAGE_RANGE_NULL:
            if (!mem_map_physical((void*)dest, pages, nullptr))
                result = E_INVALIDARG;
            break;
        case PAGE_RANGE_INCREMENTING:
        case PAGE_RANGE_CONSTANT: {
            u64 needed = r.type == PAGE_RANGE_CONSTANT ? 1 : r.page_count;
            if ((u64)r.first_page + needed > pool_pages) {
                result = E_INVALIDARG;
                break;
            }
            if (pages > frames_capacity) {
                u64* grown = (u64*)realloc(frames, pages * sizeof(u64));
                if (!grown) {
                    result = E_OUTOFMEMORY;
                    break;
                }
                frames = grown;
                frames_capacity = pages;
            }
            const void* source = (const void*)(pool + (((u64)r.first_page * unit) << MEM_PAGE_SHIFT));
            if (!mem_frames_at(source, needed * unit, frames)) {
                result = E_INVALIDARG;
                break;
            }
            for (u64 k = unit; r.type == PAGE_RANGE_CONSTANT && k < pages; k++)
                frames[k] = frames[k % unit]; // the one page, again and again
            if (!mem_map_physical((void*)dest, pages, frames))
                result = E_INVALIDARG;
            break;
        }
        default:
            result = E_INVALIDARG;
            break;
        }
        dest += bytes;
    }
    free(frames);
    if (FAILED(result))
        LOG_W("d3d12", "page mapping copy to 0x%llx from pool 0x%llx (%u pages) failed (0x%08lx)",
              destination, pool, pool_pages, (unsigned long)result);
    return result;
}
