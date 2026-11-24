// CPU topology and memory status as a console title expects to see them.
//
// Titles size job systems and per-thread tables from the processor topology. The console gives a
// title 7 SMT cores (14 hardware threads) in two core complexes; reporting the host's topology
// (e.g. 20 threads on a hybrid CPU) overflows those tables. Affinity requests use console
// processor numbers, so they are recorded but not applied: the host scheduler places threads
// better on hybrid CPUs than a fixed console-to-host mapping would.

#include <windows.h>

#include "common/log.h"
#include "common/thread_slot.h"
#include "core/memory.h"
#include "hle/kernel/overrides.h"

#define CONSOLE_CORES 7
#define CONSOLE_THREADS_PER_CORE 2
#define CONSOLE_LOGICAL (CONSOLE_CORES * CONSOLE_THREADS_PER_CORE)
#define CONSOLE_CORES_PER_CCX 4
#define CONSOLE_ALL_MASK ((1ull << CONSOLE_LOGICAL) - 1)

// Reported to the title; matches what the xmem working-set statistics use.
#define CONSOLE_TOTAL_PHYS GiB(16)
#define CONSOLE_TITLE_PHYS (GiB(13) + MiB(512))

static u32 build_topology(SYSTEM_LOGICAL_PROCESSOR_INFORMATION* out, u32 cap) {
    u32 n = 0;
#define EMIT(mask_, rel_)                                                                                    \
    if (out && n < cap) {                                                                                    \
        memset(&out[n], 0, sizeof(out[n]));                                                                  \
        out[n].ProcessorMask = (ULONG_PTR)(mask_);                                                           \
        out[n].Relationship = rel_;                                                                          \
    }
    for (u32 core = 0; core < CONSOLE_CORES; core++) {
        u64 mask = 3ull << (core * CONSOLE_THREADS_PER_CORE);
        EMIT(mask, RelationProcessorCore)
        if (out && n < cap)
            out[n].ProcessorCore.Flags = 1; // SMT siblings share the core
        n++;

        struct CacheDesc {
            BYTE level;
            BYTE assoc;
            DWORD size;
            PROCESSOR_CACHE_TYPE type;
        };
        static const CacheDesc k_core_caches[] = {
            {1, 8, (DWORD)KiB(32), CacheData},
            {1, 8, (DWORD)KiB(32), CacheInstruction},
            {2, 8, (DWORD)KiB(512), CacheUnified},
        };
        for (u32 c = 0; c < ARRAY_COUNT(k_core_caches); c++) {
            EMIT(mask, RelationCache)
            if (out && n < cap) {
                out[n].Cache.Level = k_core_caches[c].level;
                out[n].Cache.Associativity = k_core_caches[c].assoc;
                out[n].Cache.LineSize = 64;
                out[n].Cache.Size = k_core_caches[c].size;
                out[n].Cache.Type = k_core_caches[c].type;
            }
            n++;
        }
    }
    // L3 per core complex.
    for (u32 first = 0; first < CONSOLE_CORES; first += CONSOLE_CORES_PER_CCX) {
        u32 last = first + CONSOLE_CORES_PER_CCX;
        if (last > CONSOLE_CORES)
            last = CONSOLE_CORES;
        u64 mask = 0;
        for (u32 core = first; core < last; core++)
            mask |= 3ull << (core * CONSOLE_THREADS_PER_CORE);
        EMIT(mask, RelationCache)
        if (out && n < cap) {
            out[n].Cache.Level = 3;
            out[n].Cache.Associativity = 16;
            out[n].Cache.LineSize = 64;
            out[n].Cache.Size = (DWORD)MiB(4);
            out[n].Cache.Type = CacheUnified;
        }
        n++;
    }
    EMIT(CONSOLE_ALL_MASK, RelationProcessorPackage)
    n++;
    EMIT(CONSOLE_ALL_MASK, RelationNumaNode)
    if (out && n < cap)
        out[n].NumaNode.NodeNumber = 0;
    n++;
#undef EMIT
    return n;
}

static BOOL WINAPI hk_GetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer, PDWORD length) {
    if (!length) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    u32 count = build_topology(nullptr, 0);
    DWORD needed = count * (DWORD)sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    if (!buffer || *length < needed) {
        *length = needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    build_topology(buffer, count);
    *length = needed;
    static volatile long s_logged = 0;
    if (log_once_flag(&s_logged))
        LOG_I("kernel", "GetLogicalProcessorInformation: reporting console topology (%u cores, %u threads)",
              CONSOLE_CORES, CONSOLE_LOGICAL);
    return TRUE;
}

// Last affinity the title asked for, per thread (only for reporting it back). Stored as
// mask + 1 so an untouched slot (0) means "all processors".
static ThreadSlot g_title_affinity;

void cpu_overrides_init() { g_title_affinity.init(); }

static u64 title_affinity() {
    uptr v = g_title_affinity.get();
    return v ? (u64)(v - 1) : CONSOLE_ALL_MASK;
}

static BOOL WINAPI hk_SetThreadGroupAffinity(HANDLE thread, const GROUP_AFFINITY* affinity, PGROUP_AFFINITY previous) {
    if (!affinity || affinity->Group != 0 || !(affinity->Mask & CONSOLE_ALL_MASK)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    bool self = thread == GetCurrentThread() || GetThreadId(thread) == GetCurrentThreadId();
    if (previous) {
        memset(previous, 0, sizeof(*previous));
        previous->Mask = (KAFFINITY)(self ? title_affinity() : CONSOLE_ALL_MASK);
    }
    // Only tracked for the calling thread; other-thread requests just succeed.
    if (self)
        g_title_affinity.set((uptr)(affinity->Mask & CONSOLE_ALL_MASK) + 1);
    LOG_T("kernel", "SetThreadGroupAffinity(tid %lu, mask 0x%llx) recorded, not applied", GetThreadId(thread),
          (u64)affinity->Mask);
    return TRUE;
}

static BOOL WINAPI hk_GetThreadGroupAffinity(HANDLE thread, PGROUP_AFFINITY affinity) {
    if (!affinity) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(affinity, 0, sizeof(*affinity));
    bool self = thread == GetCurrentThread() || GetThreadId(thread) == GetCurrentThreadId();
    affinity->Mask = (KAFFINITY)(self ? title_affinity() : CONSOLE_ALL_MASK);
    return TRUE;
}

static BOOL WINAPI hk_GlobalMemoryStatusEx(LPMEMORYSTATUSEX status) {
    if (!status || status->dwLength != sizeof(MEMORYSTATUSEX)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    u64 used = mem_physical_used_pages() << MEM_PAGE_SHIFT;
    u64 avail = used < CONSOLE_TITLE_PHYS ? CONSOLE_TITLE_PHYS - used : 0;
    status->dwMemoryLoad = (DWORD)((used * 100) / CONSOLE_TOTAL_PHYS);
    status->ullTotalPhys = CONSOLE_TOTAL_PHYS;
    status->ullAvailPhys = avail;
    status->ullTotalPageFile = CONSOLE_TOTAL_PHYS; // no paging on console: commit == physical
    status->ullAvailPageFile = avail;
    status->ullTotalVirtual = 1ull << 47;
    status->ullAvailVirtual = (1ull << 47) - GiB(64);
    status->ullAvailExtendedVirtual = 0;
    return TRUE;
}

const HleExport g_cpu_overrides[] = {
    HLE_EXPORT_AS(hk_GetLogicalProcessorInformation, "GetLogicalProcessorInformation"),
    HLE_EXPORT_AS(hk_SetThreadGroupAffinity, "SetThreadGroupAffinity"),
    HLE_EXPORT_AS(hk_GetThreadGroupAffinity, "GetThreadGroupAffinity"),
    HLE_EXPORT_AS(hk_GlobalMemoryStatusEx, "GlobalMemoryStatusEx"),
};
const u32 g_cpu_override_count = ARRAY_COUNT(g_cpu_overrides);
