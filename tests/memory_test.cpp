// Tests for the console memory model (core/memory), and the entry point of the unit tests. Run:
// sludgemu_tests.exe (exit code = failures).

#include <windows.h>
#include <stdio.h>

#include "common/log.h"
#include "core/memory.h"
#include "test.h"

int g_failures = 0;

#define PAGE MEM_PAGE_SIZE

static u32 g_notifications = 0;
static const u8* g_last_notified = nullptr;

static void on_mapping_changed(const u8* base, u64) {
    g_notifications++;
    g_last_notified = base;
}

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

// Writes a recognizable value into each frame through the linear view.
static void tag_frames(const u64* pfns, u32 count) {
    for (u32 i = 0; i < count; i++)
        *(volatile u64*)mem_physical_ptr(pfns[i]) = 0x5100000000ull + pfns[i];
}

static bool shows_frame(const u8* va, u64 pfn) {
    return *(const volatile u64*)va == 0x5100000000ull + pfn;
}

// The allocation (view or placeholder) that contains `va`, and its size.
static void query_allocation(const u8* va, void** allocation_base, u64* size, u32* state) {
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(va, &mbi, sizeof(mbi));
    *allocation_base = mbi.AllocationBase;
    *size = mbi.RegionSize;
    *state = mbi.State;
}

static void test_runs_and_remaps() {
    const u32 pages = 32;
    MemAddressLimits limits = {};
    u8* base = (u8*)mem_reserve_placeholder(pages * PAGE, limits, 0, PAGE_READWRITE, 0);
    CHECK(base != nullptr);
    if (!base)
        return;

    // Frames come from a fresh area of physical memory, so they are consecutive (page numbers count
    // 4 KiB units, so consecutive frames are 16 apart).
    u64 run[8];
    CHECK(mem_physical_alloc(8, run));
    for (u32 i = 1; i < 8; i++)
        CHECK(run[i] == run[0] + ((u64)i << MEM_PAGE_NUMBER_SHIFT));
    u64 other[2];
    CHECK(mem_physical_alloc(2, other));
    tag_frames(run, 8);
    tag_frames(other, 2);
    u32 errors = log_error_count();

    // A run of consecutive frames is one view, once backed.
    CHECK(mem_map_physical(base + 4 * PAGE, 8, run));
    mem_prepare(base + 4 * PAGE, 8 * PAGE);
    void* alloc;
    u64 size;
    u32 state;
    query_allocation(base + 4 * PAGE, &alloc, &size, &state);
    CHECK(alloc == base + 4 * PAGE);
    CHECK(size == 8 * PAGE);
    for (u32 i = 0; i < 8; i++)
        CHECK(shows_frame(base + (4 + i) * PAGE, run[i]));
    // The run is one block, seen through the linear view.
    MemBlock block;
    CHECK(mem_backing_block(base + 6 * PAGE + 100, &block));
    CHECK(block.base == base + 4 * PAGE && block.size == 8 * PAGE && block.host == mem_physical_ptr(run[0]));
    CHECK(!mem_backing_block(base + 3 * PAGE, &block)); // page 3 isn't mapped

    // Remapping one page in the middle splits the view; every page still shows the right frame.
    u32 notified = g_notifications;
    CHECK(mem_map_physical(base + 7 * PAGE, 1, &other[0]));
    CHECK(g_notifications == notified + 1);
    CHECK(g_last_notified == base + 7 * PAGE);
    for (u32 i = 0; i < 8; i++)
        CHECK(shows_frame(base + (4 + i) * PAGE, i == 3 ? other[0] : run[i]));
    query_allocation(base + 4 * PAGE, &alloc, &size, &state);
    CHECK(size == 3 * PAGE);
    // The frames are no longer consecutive at page 7: three blocks.
    CHECK(mem_backing_block(base + 5 * PAGE, &block));
    CHECK(block.base == base + 4 * PAGE && block.size == 3 * PAGE && block.host == mem_physical_ptr(run[0]));
    CHECK(mem_backing_block(base + 7 * PAGE, &block));
    CHECK(block.base == base + 7 * PAGE && block.size == PAGE && block.host == mem_physical_ptr(other[0]));
    CHECK(mem_backing_block(base + 11 * PAGE, &block));
    CHECK(block.base == base + 8 * PAGE && block.size == 4 * PAGE && block.host == mem_physical_ptr(run[4]));

    // Separately mapped views whose frames continue the sequence form one block; a view whose
    // frames don't starts a new one.
    CHECK(mem_map_physical(base + 12 * PAGE, 2, &run[0]));
    CHECK(mem_map_physical(base + 14 * PAGE, 1, &run[2]));
    mem_prepare(base + 12 * PAGE, 3 * PAGE);
    CHECK(mem_backing_block(base + 14 * PAGE, &block));
    CHECK(block.base == base + 12 * PAGE && block.size == 3 * PAGE && block.host == mem_physical_ptr(run[0]));
    CHECK(mem_backing_block(base + 11 * PAGE, &block) && block.base == base + 8 * PAGE); // run[7], run[0]: no
    u64 invalid3[3] = {MEM_INVALID_PFN, MEM_INVALID_PFN, MEM_INVALID_PFN};
    CHECK(mem_map_physical(base + 12 * PAGE, 3, invalid3));

    // Aliasing: the same frame at a second address, writes visible through both.
    CHECK(mem_map_physical(base + 20 * PAGE, 1, &other[0]));
    base[20 * PAGE + 100] = 0x77;
    CHECK(base[7 * PAGE + 100] == 0x77);

    // Unmapping returns a page to a placeholder.
    u64 invalid = MEM_INVALID_PFN;
    CHECK(mem_map_physical(base + 5 * PAGE, 1, &invalid));
    query_allocation(base + 5 * PAGE, &alloc, &size, &state);
    CHECK(state == MEM_RESERVE);
    CHECK(shows_frame(base + 4 * PAGE, run[0]));
    CHECK(shows_frame(base + 6 * PAGE, run[2]));

    // Scatter: non-consecutive addresses.
    void* vas[3] = {base + 0 * PAGE, base + 30 * PAGE, base + 31 * PAGE};
    u64 scatter[3] = {other[1], run[6], run[7]};
    CHECK(mem_map_physical_scatter(vas, 3, scatter));
    CHECK(shows_frame(base, other[1]));
    CHECK(shows_frame(base + 30 * PAGE, run[6]));
    CHECK(shows_frame(base + 31 * PAGE, run[7]));

    // Mapping a frame that isn't allocated is refused and changes nothing.
    u64 bogus = mem_physical_total_pages() - 1;
    CHECK(!mem_map_physical(base + 4 * PAGE, 1, &bogus));
    CHECK(shows_frame(base + 4 * PAGE, run[0]));
    CHECK(!mem_map_physical(base + 31 * PAGE, 2, run)); // runs past the range
    errors += 2;                                        // both refusals are logged

    CHECK(log_error_count() == errors);
    CHECK(mem_release(base));
    CHECK(log_error_count() == errors);
    CHECK(range_is_free(base, pages * PAGE));
    mem_physical_free(8, run);
    mem_physical_free(2, other);
}

// Titles allocate page frames first and map slices of them later, not necessarily in address order.
// Frames are placed in memory when first mapped, following the range's layout, so the whole range
// still ends up as one block the GPU can see.
static void test_out_of_order_slices_form_one_block() {
    const u32 pages = 40;
    MemAddressLimits limits = {};
    u8* base = (u8*)mem_reserve_placeholder(pages * PAGE, limits, 0, PAGE_READWRITE, 0);
    CHECK(base != nullptr);
    if (!base)
        return;
    u64 ids[pages];
    CHECK(mem_physical_alloc(pages, ids));
    CHECK(mem_map_physical(base + 20 * PAGE, 10, &ids[0]));
    CHECK(mem_map_physical(base + 30 * PAGE, 10, &ids[10]));
    CHECK(mem_map_physical(base + 0 * PAGE, 10, &ids[20]));
    CHECK(mem_map_physical(base + 10 * PAGE, 10, &ids[30]));

    MemBlock block;
    mem_prepare(base, pages * PAGE);
    CHECK(mem_backing_block(base + 25 * PAGE, &block));
    CHECK(block.base == base && block.size == pages * PAGE);
    base[3 * PAGE + 5] = 0x42; // ids[23], seen through the linear view
    CHECK(mem_physical_ptr(ids[23])[5] == 0x42);
    CHECK(block.host + 3 * PAGE == mem_physical_ptr(ids[23]));

    CHECK(mem_release(base));
    mem_physical_free(pages, ids);
}

// Mapped frames stay placeholders until touched; a CPU access backs them transparently, and
// mem_prepare backs them for those that can't fault (the GPU, kernel I/O).
static void test_lazy_backing() {
    const u32 pages = 8;
    MemAddressLimits limits = {};
    u8* base = (u8*)mem_reserve_placeholder(pages * PAGE, limits, 0, PAGE_READWRITE, 0);
    CHECK(base != nullptr);
    if (!base)
        return;
    u64 frames[pages];
    CHECK(mem_physical_alloc(pages, frames));
    tag_frames(frames, pages);
    CHECK(mem_map_physical(base, pages, frames));

    void* alloc;
    u64 size;
    u32 state;
    query_allocation(base, &alloc, &size, &state);
    CHECK(state == MEM_RESERVE); // pending
    MemBlock block;
    CHECK(!mem_backing_block(base, &block));
    CHECK(shows_frame(base + 5 * PAGE, frames[5])); // the access backs the pages and retries
    query_allocation(base, &alloc, &size, &state);
    CHECK(state == MEM_COMMIT);

    // Remapped pages are pending again; mem_prepare backs them.
    CHECK(mem_map_physical(base + 2 * PAGE, 2, &frames[6]));
    query_allocation(base + 2 * PAGE, &alloc, &size, &state);
    CHECK(state == MEM_RESERVE);
    mem_prepare(base + 2 * PAGE, 2 * PAGE);
    CHECK(mem_backing_block(base + 2 * PAGE, &block));
    CHECK(shows_frame(base + 3 * PAGE, frames[7]));

    // Page number 0 is how titles release a slot: it unmaps.
    u64 none = 0;
    u32 errors = log_error_count();
    CHECK(mem_map_physical(base + 3 * PAGE, 1, &none));
    CHECK(log_error_count() == errors);
    mem_prepare(base + 3 * PAGE, PAGE);
    query_allocation(base + 3 * PAGE, &alloc, &size, &state);
    CHECK(state == MEM_RESERVE);

    CHECK(mem_release(base));
    CHECK(range_is_free(base, pages * PAGE));
    mem_physical_free(pages, frames);
}

// Large commits are reserved and committed as they're touched, a granule at a time.
static void test_lazy_commit() {
    MemAddressLimits limits = {};
    const u64 size = MiB(64);
    u8* base = (u8*)mem_alloc_virtual(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, limits, 0, 0);
    CHECK(base != nullptr);
    if (!base)
        return;
    void* alloc;
    u64 region;
    u32 state;
    query_allocation(base + MiB(40), &alloc, &region, &state);
    CHECK(state == MEM_RESERVE);
    base[MiB(40) + 12] = 7; // faults, gets committed, retries
    CHECK(base[MiB(40) + 12] == 7);
    query_allocation(base + MiB(40), &alloc, &region, &state);
    CHECK(state == MEM_COMMIT);
    query_allocation(base + MiB(20), &alloc, &region, &state);
    CHECK(state == MEM_RESERVE); // untouched parts stay reserved
    mem_prepare(base + MiB(20), 16);
    query_allocation(base + MiB(20), &alloc, &region, &state);
    CHECK(state == MEM_COMMIT);
    CHECK(mem_release(base));
    CHECK(range_is_free(base, size));
}

void test_xapu_decode();
void test_xapu_convert();
void test_xdsp();
void test_xma();

int main() {
    log_init(nullptr, LOG_LEVEL_WARN);
    if (!mem_init(GiB(1))) {
        printf("FAIL: mem_init\n");
        return 1;
    }
    mem_set_mapping_listener(on_mapping_changed);
    test_placeholder_release();
    test_runs_and_remaps();
    test_out_of_order_slices_form_one_block();
    test_backed_release();
    test_frames_zeroed_on_reuse();
    test_lazy_backing();
    test_lazy_commit();
    test_xapu_decode();
    test_xapu_convert();
    test_xdsp();
    test_xma();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures;
}
