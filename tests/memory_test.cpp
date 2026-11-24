// Tests for the console memory model (core/memory). Run: sludgemu_tests.exe (exit code = failures).

#include <windows.h>
#include <stdio.h>

#include "common/log.h"
#include "core/memory.h"

static int g_failures = 0;

#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                           \
            g_failures++;                                                                                    \
        }                                                                                                    \
    } while (0)

#define PAGE MEM_PAGE_SIZE

static bool range_is_free(const u8* base, u64 size) {
    const u8* p = base;
    while (p < base + size) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_FREE)
            return false;
        p = (const u8*)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

// A placeholder range with every page state mixed in: mapped pages, a split-but-unmapped page and
// several unsplit runs. Releasing it must free all of it, with every release succeeding once.
static void test_placeholder_release() {
    const u64 pages = 16;
    MemAddressLimits limits = {};
    u8* base = (u8*)mem_reserve_placeholder(pages * PAGE, limits, 0, PAGE_READWRITE, 0);
    CHECK(base != nullptr);
    if (!base)
        return;

    u64 pfns[3];
    CHECK(mem_physical_alloc(3, pfns));
    void* vas[3] = {base + 3 * PAGE, base + 4 * PAGE, base + 10 * PAGE};
    CHECK(mem_map_physical_scatter(vas, 3, pfns));
    ((volatile u8*)vas[0])[0] = 1; // mapped and writable

    u64 unmap = MEM_INVALID_PFN;
    CHECK(mem_map_physical(base + 4 * PAGE, 1, &unmap)); // page 4: split, nothing mapped

    u32 errors = log_error_count();
    CHECK(mem_release(base));
    CHECK(log_error_count() == errors);
    CHECK(range_is_free(base, pages * PAGE));
    CHECK(!mem_is_tracked(base));
    mem_physical_free(3, pfns);
}

// A fully backed range owns its pages: releasing it returns them to the physical allocator.
static void test_backed_release() {
    MemAddressLimits limits = {};
    u64 used = mem_physical_used_pages();
    u8* base = (u8*)mem_alloc_physical_backed(8 * PAGE, limits, 0, PAGE_READWRITE, 0);
    CHECK(base != nullptr);
    if (!base)
        return;
    CHECK(mem_physical_used_pages() == used + 8);
    base[0] = 1;
    base[8 * PAGE - 1] = 1;

    u32 errors = log_error_count();
    CHECK(mem_release(base));
    CHECK(log_error_count() == errors);
    CHECK(range_is_free(base, 8 * PAGE));
    CHECK(mem_physical_used_pages() == used);
}

// Recycled frames come back zeroed. With every other frame taken, the reallocation has to return
// the frame that was just freed.
static void test_frames_zeroed_on_reuse() {
    u64 count = mem_physical_total_pages() - mem_physical_used_pages();
    u64* all = (u64*)VirtualAlloc(nullptr, count * sizeof(u64), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(mem_physical_alloc(count, all));
    u64 victim = all[count / 2];
    mem_physical_ptr(victim)[123] = 0xAB;
    mem_physical_free(1, &victim);

    u64 again;
    CHECK(mem_physical_alloc(1, &again));
    CHECK(again == victim);
    CHECK(mem_physical_ptr(again)[123] == 0);

    all[count / 2] = again;
    mem_physical_free(count, all);
    VirtualFree(all, 0, MEM_RELEASE);
}

int main() {
    log_init(nullptr, LOG_LEVEL_WARN);
    if (!mem_init(GiB(1))) {
        printf("FAIL: mem_init\n");
        return 1;
    }
    test_placeholder_release();
    test_backed_release();
    test_frames_zeroed_on_reuse();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures;
}
