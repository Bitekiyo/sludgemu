// Page pools: title memory registered as the source of GPU page mapping copies
// (ID3D12CommandQueue::CopyPageMappingsX), which titles use to stream resources in and out without
// touching CPU mappings.

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
