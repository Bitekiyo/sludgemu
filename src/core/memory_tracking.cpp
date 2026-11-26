// Access tracking of title memory: watches, store traps, taken pages and device writes (see memory.h).
//
// Tracking keys memory by its canonical address, the same for every address showing it: physical
// memory by frame ID (a frame can show at several addresses), other title memory by its address.
// Protection is per host page and counted: a page is read-only, at every address showing it, while
// any watch or trap covers it, and it only changes when a count goes from or to zero. So arming a
// watch protects only pages nobody protected yet, and ending one releases only pages nobody else
// needs; a write ends exactly the watches covering the page written. A taken page allows no access
// at all whatever its count, until it is given back.
//
// Lock order: range lock, then phys lock (briefly, to find frames), then the tracking lock. Listeners
// are called with no locks held.

#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "common/thread_slot.h"
#include "core/memory_internal.h"

#define CANON_FRAME      (1ull << 62) // canonical addresses of physical memory: frame ID and offset
#define HOST_PAGE        4096u
#define FRAME_HOST_PAGES (MEM_PAGE_SIZE / HOST_PAGE)
#define GRANULE_SHIFT    16 // granules of the watch index: one frame, or 64 KiB of other memory
#define NO_NODE          0xFFFFFFFFu
#define MAX_REPORTS      16

// A host page's count: the watches and traps covering it, and above them whether it is taken.
#define TAKEN      0x8000u
#define COUNT_MASK 0x7FFFu

// What the CPU may do on a page.
enum Level { LEVEL_WRITE, LEVEL_READ, LEVEL_NONE };

static u32 level_of(u16 count) {
    return (count & TAKEN) ? LEVEL_NONE : (count & COUNT_MASK) ? LEVEL_READ : LEVEL_WRITE;
}

// A canonical range, host page aligned.
struct Span {
    u64 start;
    u64 end;
};

struct Watch {
    u64 base, size; // what is reported; size 0: a free slot
    Span* spans;    // sorted, disjoint
    u32 span_count;
    bool trap; // a store trap: stays, and reports every store
    u32 next_free;
};

// Which watches cover a granule: chained hashing, nodes from a pool.
struct GranuleNode {
    u64 granule;
    u32 watch;
    u32 next;
};

struct Report {
    u64 base, size;
};

static struct {
    SRWLOCK lock;
    volatile long live;  // watches and traps there are: none means no fault can be ours
    volatile long taken; // host pages taken
    MemWriteListener write_listeners[4];
    MemStoreListener store_listener;
    MemTakenListener taken_listener;
    u16* frame_counts; // per host page of every frame ID: what protects it
    Watch* watches;
    u32 watch_slots, watch_capacity, free_watch;
    u32* by_base; // live watches by reported base, for exact and overlap lookups
    u32 by_base_count;
    u64 max_size; // of a live watch: bounds the overlap search
    u32* buckets;
    u32 bucket_mask;
    GranuleNode* nodes;
    u32 node_slots, node_capacity, free_node, node_count;
} g_track = {SRWLOCK_INIT};

// The trapped pages the current thread's store runs on with write access (a store can straddle two),
// their canonical addresses, and the store's address: a repeated string store steps until done.
static ThreadSlot g_step_page[2];
static ThreadSlot g_step_key[2];
static ThreadSlot g_step_ip;
#define TRAP_FLAG 0x100u // EFLAGS.TF: single-step

void tracking_init() {
    u64 bytes = g_mem.total_pages * FRAME_HOST_PAGES * sizeof(u16);
    g_track.frame_counts = (u16*)VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_track.frame_counts)
        FATAL("out of memory for write tracking");
    g_track.bucket_mask = 4095;
    g_track.buckets = (u32*)malloc((g_track.bucket_mask + 1) * sizeof(u32));
    if (!g_track.buckets)
        FATAL("out of memory for write tracking");
    memset(g_track.buckets, 0xFF, (g_track.bucket_mask + 1) * sizeof(u32));
    g_track.free_watch = g_track.free_node = NO_NODE;
    for (u32 i = 0; i < 2; i++) {
        g_step_page[i].init();
        g_step_key[i].init();
    }
    g_step_ip.init();
}

void mem_add_write_listener(MemWriteListener listener) {
    for (u32 i = 0; i < ARRAY_COUNT(g_track.write_listeners); i++) {
        if (!g_track.write_listeners[i]) {
            g_track.write_listeners[i] = listener;
            return;
        }
    }
    FATAL("too many write listeners");
}

void mem_remove_write_listener(MemWriteListener listener) {
    for (u32 i = 0; i < ARRAY_COUNT(g_track.write_listeners); i++)
        if (g_track.write_listeners[i] == listener)
            g_track.write_listeners[i] = nullptr;
}

static void notify_write(u64 base, u64 size, MemWriteKind kind) {
    for (u32 i = 0; i < ARRAY_COUNT(g_track.write_listeners); i++)
        if (MemWriteListener listener = g_track.write_listeners[i])
            listener(base, size, kind);
}

void mem_set_store_listener(MemStoreListener listener) {
    g_track.store_listener = listener;
}

void mem_set_taken_listener(MemTakenListener listener) {
    g_track.taken_listener = listener;
}

// Whether any fault can be tracking's.
static bool tracking_active() {
    return g_track.live || g_track.taken;
}

// --- canonical addresses -----------------------------------------------------------------------------

// The canonical address of title address `va` in range `s`, or 0 if no memory is there (an unmapped
// page; with `backed`, also a frame not backed yet). Range lock held, phys lock not held.
static u64 canonical(const RangeSlot* s, u64 va, bool backed) {
    if (!(s->info.flags & MEM_RANGE_PLACEHOLDER))
        return va;
    u32 page = (u32)((va - (u64)s->info.base) >> MEM_PAGE_SHIFT);
    if (backed && s->pending && s->pending[page] != MEM_INVALID_PFN)
        return 0;
    AcquireSRWLockShared(&g_mem.phys_lock);
    u32 id = frame_at_locked(s, page);
    ReleaseSRWLockShared(&g_mem.phys_lock);
    if (id == MEM_INVALID_PFN)
        return 0;
    return CANON_FRAME | ((u64)id << MEM_PAGE_SHIFT) | (va & (MEM_PAGE_SIZE - 1));
}

// The canonical spans of [start, end) in range `s`, host page aligned, appended to `out` (growable;
// `*capacity` entries). Range lock held, phys lock not held.
static void canonical_spans(const RangeSlot* s, u64 start, u64 end, Span** out, u32* count, u32* capacity) {
    start &= ~(u64)(HOST_PAGE - 1);
    end = ALIGN_UP(end, (u64)HOST_PAGE);
    for (u64 va = start; va < end;) {
        u64 stop =
            (s->info.flags & MEM_RANGE_PLACEHOLDER) ? ALIGN_DOWN(va, MEM_PAGE_SIZE) + MEM_PAGE_SIZE : end;
        stop = stop < end ? stop : end;
        u64 key = canonical(s, va, false);
        if (key) {
            if (*count && (*out)[*count - 1].end == key) {
                (*out)[*count - 1].end = key + (stop - va);
            } else {
                if (*count == *capacity) {
                    *capacity = *capacity ? *capacity * 2 : 16;
                    Span* grown = (Span*)realloc(*out, *capacity * sizeof(Span));
                    if (!grown)
                        FATAL("out of memory for write tracking");
                    *out = grown;
                }
                (*out)[(*count)++] = {key, key + (stop - va)};
            }
        }
        va = stop;
    }
}

// --- protection --------------------------------------------------------------------------------------

// The protection count of the host page at canonical address `key`; nullptr if it has none and
// `create` is false. Range lock held; tracking lock held (exclusively to change counts or create).
static u16* page_count(u64 key, bool create) {
    if (key & CANON_FRAME)
        return &g_track.frame_counts[(key & ~CANON_FRAME) / HOST_PAGE];
    RangeSlot* s = find_range_locked((const void*)key);
    if (!s)
        return nullptr;
    if (!s->watch_counts) {
        if (!create)
            return nullptr;
        s->watch_counts = (u16*)calloc(s->info.size / HOST_PAGE, sizeof(u16));
        if (!s->watch_counts)
            FATAL("out of memory for write tracking");
    }
    return &s->watch_counts[(key - (u64)s->info.base) / HOST_PAGE];
}

// Title memory the CPU can't write needs no write protection: GPU-only memory, and read-only memory.
static bool protectable(const RangeSlot* s) {
    u32 p = s->info.protect & 0xFF;
    if (p == PAGE_NOACCESS || !p)
        return false; // GPU only
    u32 m = mapping_protect(s) & 0xFF;
    return m == PAGE_READWRITE || m == PAGE_EXECUTE_READWRITE;
}

// Memory the CPU can't access isn't taken from it either.
static bool takeable(const RangeSlot* s) {
    u32 p = s->info.protect & 0xFF;
    return p != PAGE_NOACCESS && p != 0;
}

// Whether `s` takes protection for pages at `level`: taking needs the CPU to have access, write
// protection to be able to write.
static bool protects(const RangeSlot* s, u32 level) {
    return level == LEVEL_NONE ? takeable(s) : protectable(s) || (takeable(s) && level == LEVEL_WRITE);
}

// Changes the protection of [start, end) in range `s` to `level`. Views are separate allocations to
// VirtualProtect, so placeholder ranges go view by view; other ranges may have pages not committed yet
// (lazy commits), so they go by what is committed. Range lock held.
static void protect_va(const RangeSlot* s, u64 start, u64 end, u32 level) {
    const u32 modifiers = PAGE_WRITECOMBINE | PAGE_NOCACHE;
    u32 writable = mapping_protect(s) & 0xFF;
    u32 to = level == LEVEL_NONE ? PAGE_NOACCESS : level == LEVEL_READ ? PAGE_READONLY : writable;
    DWORD old;
    if (s->info.flags & MEM_RANGE_PLACEHOLDER) {
        for (u64 va = start; va < end;) {
            u32 page = (u32)((va - (u64)s->info.base) >> MEM_PAGE_SHIFT);
            u32 r = region_find(s, page);
            u64 stop = (u64)page_va(s, region_end(s, r));
            stop = stop < end ? stop : end;
            if (s->regions[r].pfn != REGION_PLACEHOLDER && !VirtualProtect((void*)va, stop - va, to, &old))
                LOG_W_ONCE("memory", "cannot change the protection of %p (+0x%llx) for write tracking (%lu)",
                           (void*)va, stop - va, GetLastError());
            va = stop;
        }
        return;
    }
    for (u64 va = start; va < end;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void*)va, &mbi, sizeof(mbi)))
            break;
        u64 stop = (u64)mbi.BaseAddress + mbi.RegionSize;
        stop = stop < end ? stop : end;
        // Only protections tracking gives: what the title set itself stays.
        u32 now = mbi.Protect & ~modifiers;
        bool ours = now == writable || now == PAGE_READONLY || now == PAGE_NOACCESS;
        if (mbi.State == MEM_COMMIT && ours && now != to && !VirtualProtect((void*)va, stop - va, to, &old))
            LOG_W_ONCE("memory", "cannot change the protection of %p (+0x%llx) for write tracking (%lu)",
                       (void*)va, stop - va, GetLastError());
        va = stop;
    }
}

// Address ranges whose protection changes, collected and merged before applying them.
struct VaRun {
    u64 start, end;
    const RangeSlot* range;
};

static VaRun* g_runs; // tracking lock held exclusively
static u32 g_run_count, g_run_capacity;

static void add_run(u64 start, u64 end, const RangeSlot* s) {
    if (g_run_count == g_run_capacity) {
        g_run_capacity = g_run_capacity ? g_run_capacity * 2 : 256;
        VaRun* grown = (VaRun*)realloc(g_runs, g_run_capacity * sizeof(VaRun));
        if (!grown)
            FATAL("out of memory for write tracking");
        g_runs = grown;
    }
    g_runs[g_run_count++] = {start, end, s};
}

// The range holding `va`; consecutive lookups mostly hit the same range. Range lock held, tracking lock
// held exclusively.
static const RangeSlot* range_of(u64 va) {
    static const RangeSlot* last;
    if (last && last >= g_mem.ranges && last < g_mem.ranges + g_mem.range_count &&
        va >= (u64)last->info.base && va < (u64)last->info.base + last->info.size)
        return last;
    last = find_range_locked((const void*)va);
    return last;
}

// The addresses showing canonical pages [key, key + size), as runs, where ranges protect pages at
// `level`. Range lock held, tracking lock held exclusively.
static void add_addresses(u64 key, u64 size, u32 level) {
    if (!(key & CANON_FRAME)) {
        const RangeSlot* s = range_of(key);
        if (s && protects(s, level))
            add_run(key, key + size, s);
        return;
    }
    u64 frame = (key & ~CANON_FRAME) >> MEM_PAGE_SHIFT;
    u64 offset = key & (MEM_PAGE_SIZE - 1);
    u64 va = g_mem.frame_va[frame];
    if (!va)
        return; // not mapped anywhere
    const RangeSlot* s = range_of(va);
    if (s && protects(s, level))
        add_run(va + offset, va + offset + size, s);
    for (u32 a = alias_lower_bound((u32)frame); a < g_mem.alias_count && g_mem.aliases[a].id == frame; a++) {
        va = g_mem.aliases[a].va;
        s = range_of(va);
        if (s && protects(s, level))
            add_run(va + offset, va + offset + size, s);
    }
}

static int compare_runs(const void* a, const void* b) {
    u64 x = ((const VaRun*)a)->start, y = ((const VaRun*)b)->start;
    return x < y ? -1 : x > y ? 1 : 0;
}

// Applies `level` to the collected runs, merged where they continue each other. Range lock held.
static void apply_runs(u32 level) {
    if (!g_run_count)
        return;
    qsort(g_runs, g_run_count, sizeof(VaRun), compare_runs);
    VaRun cur = g_runs[0];
    for (u32 i = 1; i < g_run_count; i++) {
        if (g_runs[i].range == cur.range && g_runs[i].start <= cur.end) {
            cur.end = g_runs[i].end > cur.end ? g_runs[i].end : cur.end;
            continue;
        }
        protect_va(cur.range, cur.start, cur.end, level);
        cur = g_runs[i];
    }
    protect_va(cur.range, cur.start, cur.end, level);
    g_run_count = 0;
}

// Adds `delta` (+1 or -1) to the counts of a span's pages; pages whose count leaves or reaches zero
// are collected as runs (taken ones stay as they are). A span lies in one range. Range lock held,
// tracking lock held exclusively.
static void count_span(const Span& sp, int delta) {
    u16* counts = page_count(sp.start, true);
    if (!counts)
        return;
    u32 level = delta > 0 ? LEVEL_READ : LEVEL_WRITE;
    u64 run_start = 0, run_end = 0;
    for (u64 key = sp.start; key < sp.end; key += HOST_PAGE) {
        u16* c = &counts[(key - sp.start) / HOST_PAGE];
        u16 before = *c;
        *c = (u16)(delta > 0 ? before + 1 : before - 1);
        if (level_of(before) == level_of(*c))
            continue;
        // Runs stay within a frame: the next frame can show elsewhere.
        bool continues = run_end == key && ((key & CANON_FRAME) == 0 || (key & (MEM_PAGE_SIZE - 1)) != 0);
        if (continues) {
            run_end = key + HOST_PAGE;
            continue;
        }
        if (run_end)
            add_addresses(run_start, run_end - run_start, level);
        run_start = key;
        run_end = key + HOST_PAGE;
    }
    if (run_end)
        add_addresses(run_start, run_end - run_start, level);
}

// Takes a span's pages from the CPU or gives them back; pages whose level changes to `level` are
// collected as runs (giving back collects those going to `level` only: watched pages go back to read
// only, others to writable). A span lies in one range. Range lock held, tracking lock held exclusively.
static void take_span(const Span& sp, bool take, u32 level) {
    u16* counts = page_count(sp.start, true);
    if (!counts)
        return;
    u64 run_start = 0, run_end = 0;
    for (u64 key = sp.start; key < sp.end; key += HOST_PAGE) {
        u16* c = &counts[(key - sp.start) / HOST_PAGE];
        if (((*c & TAKEN) != 0) == take)
            continue;
        if (!take && level_of(*c & COUNT_MASK) != level)
            continue; // the other pass gives it back
        *c = (u16)(take ? *c | TAKEN : *c & ~TAKEN);
        take ? InterlockedIncrement(&g_track.taken) : InterlockedDecrement(&g_track.taken);
        bool continues = run_end == key && ((key & CANON_FRAME) == 0 || (key & (MEM_PAGE_SIZE - 1)) != 0);
        if (continues) {
            run_end = key + HOST_PAGE;
            continue;
        }
        if (run_end)
            add_addresses(run_start, run_end - run_start, level);
        run_start = key;
        run_end = key + HOST_PAGE;
    }
    if (run_end)
        add_addresses(run_start, run_end - run_start, level);
}

// --- the watch index ---------------------------------------------------------------------------------

static u32 granule_hash(u64 granule) {
    return (u32)((granule * 0x9E3779B97F4A7C15ull) >> 40) & g_track.bucket_mask;
}

static void index_rehash() {
    u32 mask = g_track.bucket_mask * 2 + 1;
    u32* buckets = (u32*)malloc((mask + 1) * sizeof(u32));
    if (!buckets)
        FATAL("out of memory for write tracking");
    memset(buckets, 0xFF, (mask + 1) * sizeof(u32));
    free(g_track.buckets);
    g_track.buckets = buckets;
    g_track.bucket_mask = mask;
    for (u32 i = 0; i < g_track.node_slots; i++) {
        GranuleNode& n = g_track.nodes[i];
        if (n.watch == NO_NODE)
            continue;
        u32 b = granule_hash(n.granule);
        n.next = g_track.buckets[b];
        g_track.buckets[b] = i;
    }
}

static void index_add(u64 granule, u32 watch) {
    if (g_track.node_count >= (g_track.bucket_mask + 1) * 2)
        index_rehash();
    u32 n = g_track.free_node;
    if (n != NO_NODE) {
        g_track.free_node = g_track.nodes[n].next;
    } else {
        if (g_track.node_slots == g_track.node_capacity) {
            g_track.node_capacity = g_track.node_capacity ? g_track.node_capacity * 2 : 4096;
            GranuleNode* grown =
                (GranuleNode*)realloc(g_track.nodes, g_track.node_capacity * sizeof(GranuleNode));
            if (!grown)
                FATAL("out of memory for write tracking");
            g_track.nodes = grown;
        }
        n = g_track.node_slots++;
    }
    u32 b = granule_hash(granule);
    g_track.nodes[n] = {granule, watch, g_track.buckets[b]};
    g_track.buckets[b] = n;
    g_track.node_count++;
}

static void index_remove(u64 granule, u32 watch) {
    u32* link = &g_track.buckets[granule_hash(granule)];
    while (*link != NO_NODE) {
        GranuleNode& n = g_track.nodes[*link];
        if (n.granule == granule && n.watch == watch) {
            u32 freed = *link;
            *link = n.next;
            n.watch = NO_NODE;
            n.next = g_track.free_node;
            g_track.free_node = freed;
            g_track.node_count--;
            return;
        }
        link = &n.next;
    }
}

// Calls index_add or index_remove for every granule a watch's spans touch.
static void index_watch(u32 id, bool add) {
    const Watch& w = g_track.watches[id];
    u64 last = ~0ull;
    for (u32 i = 0; i < w.span_count; i++) {
        for (u64 g = w.spans[i].start >> GRANULE_SHIFT; g <= (w.spans[i].end - 1) >> GRANULE_SHIFT; g++) {
            if (g == last)
                continue;
            if (add)
                index_add(g, id);
            else
                index_remove(g, id);
            last = g;
        }
    }
}

static bool watch_covers(const Watch& w, u64 key) {
    u32 lo = 0, hi = w.span_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (w.spans[mid].end <= key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < w.span_count && w.spans[lo].start <= key;
}

// --- watches -----------------------------------------------------------------------------------------

static u32 by_base_lower_bound(u64 base) {
    u32 lo = 0, hi = g_track.by_base_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_track.watches[g_track.by_base[mid]].base < base)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// The live watch reporting exactly [base, base + size) (a trap, or not), or NO_NODE. Tracking lock held.
static u32 find_watch(u64 base, u64 size, bool trap) {
    for (u32 i = by_base_lower_bound(base); i < g_track.by_base_count; i++) {
        const Watch& w = g_track.watches[g_track.by_base[i]];
        if (w.base != base)
            break;
        if (w.size == size && w.trap == trap)
            return g_track.by_base[i];
    }
    return NO_NODE;
}

// Makes a watch over `spans` (taken over) and protects what it covers. Range lock held, tracking lock
// held exclusively.
static void add_watch(u64 base, u64 size, Span* spans, u32 span_count, bool trap) {
    u32 id = g_track.free_watch;
    if (id != NO_NODE) {
        g_track.free_watch = g_track.watches[id].next_free;
    } else {
        if (g_track.watch_slots == g_track.watch_capacity) {
            g_track.watch_capacity = g_track.watch_capacity ? g_track.watch_capacity * 2 : 1024;
            Watch* grown = (Watch*)realloc(g_track.watches, g_track.watch_capacity * sizeof(Watch));
            u32* order =
                grown ? (u32*)realloc(g_track.by_base, g_track.watch_capacity * sizeof(u32)) : nullptr;
            if (grown)
                g_track.watches = grown;
            if (!order)
                FATAL("out of memory for write tracking");
            g_track.by_base = order;
        }
        id = g_track.watch_slots++;
    }
    g_track.watches[id] = {base, size, spans, span_count, trap, NO_NODE};
    u32 at = by_base_lower_bound(base);
    memmove(&g_track.by_base[at + 1], &g_track.by_base[at], (g_track.by_base_count - at) * sizeof(u32));
    g_track.by_base[at] = id;
    g_track.by_base_count++;
    g_track.max_size = size > g_track.max_size ? size : g_track.max_size;
    index_watch(id, true);
    for (u32 i = 0; i < span_count; i++)
        count_span(spans[i], +1);
    apply_runs(LEVEL_READ);
    InterlockedIncrement(&g_track.live);
}

// Ends watch `id`, releasing what only it protected, and adds it to `reports` unless it is null.
// Range lock held, tracking lock held exclusively.
static void end_watch(u32 id, Report* reports, u32* report_count) {
    Watch& w = g_track.watches[id];
    index_watch(id, false);
    for (u32 i = 0; i < w.span_count; i++)
        count_span(w.spans[i], -1);
    apply_runs(LEVEL_WRITE);
    u32 at = by_base_lower_bound(w.base);
    while (g_track.by_base[at] != id)
        at++;
    memmove(&g_track.by_base[at], &g_track.by_base[at + 1], (g_track.by_base_count - at - 1) * sizeof(u32));
    g_track.by_base_count--;
    if (!g_track.by_base_count)
        g_track.max_size = 0;
    if (reports && *report_count < MAX_REPORTS) {
        reports[(*report_count)++] = {w.base, w.size};
    } else if (reports) { // too many to list: the last one grows to cover the rest
        Report& last = reports[MAX_REPORTS - 1];
        u64 end = last.base + last.size > w.base + w.size ? last.base + last.size : w.base + w.size;
        last.base = w.base < last.base ? w.base : last.base;
        last.size = end - last.base;
    }
    free(w.spans);
    w = {};
    w.next_free = g_track.free_watch;
    g_track.free_watch = id;
    InterlockedDecrement(&g_track.live);
}

// Ends the watches (not traps) covering canonical address `key`. Range lock held, tracking lock held
// exclusively.
static void end_watches_at(u64 key, Report* reports, u32* report_count) {
    u64 granule = key >> GRANULE_SHIFT;
    for (u32 n = g_track.buckets[granule_hash(granule)]; n != NO_NODE;) {
        const GranuleNode node = g_track.nodes[n];
        n = node.next;
        if (node.granule != granule)
            continue;
        const Watch& w = g_track.watches[node.watch];
        if (!w.trap && watch_covers(w, key)) {
            end_watch(node.watch, reports, report_count);
            n = g_track.buckets[granule_hash(granule)]; // the chain changed: look again
        }
    }
}

// Ends the watches (traps too, with `traps`) reporting into [start, end), unreported. Range lock held,
// tracking lock held exclusively.
static void end_watches_reporting_in(u64 start, u64 end, bool traps) {
    u64 from = start > g_track.max_size ? start - g_track.max_size : 0;
    for (u32 i = by_base_lower_bound(from); i < g_track.by_base_count;) {
        u32 id = g_track.by_base[i];
        const Watch& w = g_track.watches[id];
        if (w.base >= end)
            break;
        if (w.base + w.size > start && (traps || !w.trap)) {
            end_watch(id, nullptr, nullptr);
            continue; // the next one moved here
        }
        i++;
    }
}

static void report_writes(const Report* reports, u32 count) {
    for (u32 i = 0; i < count; i++)
        notify_write(reports[i].base, reports[i].size, MEM_WATCH_ENDED);
}

static void watch_or_trap(const void* base, u64 size, bool trap) {
    u64 start = (u64)base, end = start + size;
    if (!size)
        return;
    AcquireSRWLockShared(&g_mem.range_lock);
    const RangeSlot* s = find_range_locked(base);
    if (!s || (const u8*)end > s->info.base + s->info.size) {
        ReleaseSRWLockShared(&g_mem.range_lock);
        if (trap)
            LOG_W("memory", "store trap at %p (+0x%llx) outside title memory; stores there aren't seen", base,
                  size);
        return; // not title memory we track
    }
    Span* spans = nullptr;
    u32 count = 0, capacity = 0;
    canonical_spans(s, start, end, &spans, &count, &capacity);
    AcquireSRWLockExclusive(&g_track.lock);
    if (find_watch(start, size, trap) == NO_NODE && count)
        add_watch(start, size, spans, count, trap);
    else
        free(spans);
    ReleaseSRWLockExclusive(&g_track.lock);
    ReleaseSRWLockShared(&g_mem.range_lock);
}

void mem_watch_writes(const void* base, u64 size) {
    watch_or_trap(base, size, false);
}

void mem_trap_stores(const void* base, u64 size) {
    watch_or_trap(base, size, true);
}

static void end_armed(const void* base, u64 size, bool trap) {
    if (!g_track.live)
        return;
    AcquireSRWLockShared(&g_mem.range_lock);
    AcquireSRWLockExclusive(&g_track.lock);
    u32 id = find_watch((u64)base, size, trap);
    if (id != NO_NODE)
        end_watch(id, nullptr, nullptr);
    ReleaseSRWLockExclusive(&g_track.lock);
    ReleaseSRWLockShared(&g_mem.range_lock);
}

void mem_unwatch_writes(const void* base, u64 size) {
    end_armed(base, size, false);
}

void mem_untrap_stores(const void* base, u64 size) {
    end_armed(base, size, true);
}

static void take_or_give(const void* base, u64 size, bool take) {
    u64 start = (u64)base, end = start + size;
    if (!size)
        return;
    AcquireSRWLockShared(&g_mem.range_lock);
    const RangeSlot* s = find_range_locked(base);
    if (!s || (const u8*)end > s->info.base + s->info.size) {
        ReleaseSRWLockShared(&g_mem.range_lock);
        LOG_W("memory", "pages at %p (+0x%llx) to %s are outside title memory", base, size,
              take ? "take" : "give back");
        return;
    }
    Span* spans = nullptr;
    u32 count = 0, capacity = 0;
    canonical_spans(s, start, end, &spans, &count, &capacity);
    AcquireSRWLockExclusive(&g_track.lock);
    if (take) {
        for (u32 i = 0; i < count; i++)
            take_span(spans[i], true, LEVEL_NONE);
        apply_runs(LEVEL_NONE);
    } else {
        for (u32 level = LEVEL_WRITE; level <= LEVEL_READ; level++) {
            for (u32 i = 0; i < count; i++)
                take_span(spans[i], false, level);
            apply_runs(level);
        }
    }
    ReleaseSRWLockExclusive(&g_track.lock);
    ReleaseSRWLockShared(&g_mem.range_lock);
    free(spans);
}

void mem_take_pages(const void* base, u64 size) {
    take_or_give(base, size, true);
}

void mem_give_pages(const void* base, u64 size) {
    take_or_give(base, size, false);
}

void mem_end_write_watches(const void* base, u64 size) {
    if (!g_track.live || !size)
        return;
    Report reports[MAX_REPORTS];
    u32 report_count = 0;
    u64 start = (u64)base & ~(u64)(HOST_PAGE - 1), end = (u64)base + size;
    AcquireSRWLockShared(&g_mem.range_lock);
    const RangeSlot* s = find_range_locked(base);
    if (s) {
        end = end < (u64)s->info.base + s->info.size ? end : (u64)s->info.base + s->info.size;
        Span* spans = nullptr;
        u32 count = 0, capacity = 0;
        canonical_spans(s, start, end, &spans, &count, &capacity);
        AcquireSRWLockExclusive(&g_track.lock);
        for (u32 i = 0; i < count; i++)
            for (u64 key = spans[i].start; key < spans[i].end; key += HOST_PAGE)
                end_watches_at(key, reports, &report_count);
        ReleaseSRWLockExclusive(&g_track.lock);
        free(spans);
    }
    ReleaseSRWLockShared(&g_mem.range_lock);
    report_writes(reports, report_count);
}

// --- hooks from the rest of the memory system --------------------------------------------------------

// Gives [start, end) of a view just mapped `level` (read-only or no access). (The range's region list
// catches up after the mapping, so protect_va can't go by it yet.)
static void protect_new_view(u64 start, u64 end, u32 level) {
    DWORD old;
    if (!VirtualProtect((void*)start, end - start, level == LEVEL_NONE ? PAGE_NOACCESS : PAGE_READONLY, &old))
        LOG_W_ONCE("memory", "cannot change the protection of %p (+0x%llx) for access tracking (%lu)",
                   (void*)start, end - start, GetLastError());
}

void tracking_view_mapped(const RangeSlot* s, u32 first_page, u32 count, u32 real) {
    if (!tracking_active() || !takeable(s))
        return;
    u32* ids = (u32*)malloc(count * sizeof(u32));
    if (!ids)
        FATAL("out of memory for write tracking");
    AcquireSRWLockShared(&g_mem.phys_lock);
    for (u32 i = 0; i < count; i++)
        ids[i] = g_mem.real_to_id[real + i];
    ReleaseSRWLockShared(&g_mem.phys_lock);
    AcquireSRWLockShared(&g_track.lock);
    u64 run_start = 0, run_end = 0;
    u32 run_level = LEVEL_WRITE;
    for (u32 i = 0; i < count; i++) {
        const u16* c = &g_track.frame_counts[(u64)ids[i] * FRAME_HOST_PAGES];
        u64 va = (u64)page_va(s, first_page + i);
        for (u32 p = 0; p < FRAME_HOST_PAGES; p++) {
            u32 level = level_of(c[p]);
            if (level == LEVEL_WRITE || !protects(s, level))
                continue;
            u64 at = va + (u64)p * HOST_PAGE;
            if (run_end == at && level == run_level) {
                run_end += HOST_PAGE;
                continue;
            }
            if (run_end)
                protect_new_view(run_start, run_end, run_level);
            run_start = at;
            run_end = at + HOST_PAGE;
            run_level = level;
        }
    }
    if (run_end)
        protect_new_view(run_start, run_end, run_level);
    ReleaseSRWLockShared(&g_track.lock);
    free(ids);
}

void tracking_committed(const RangeSlot* s, u64 lo, u64 hi) {
    if (!tracking_active() || !s->watch_counts || !takeable(s))
        return;
    AcquireSRWLockShared(&g_track.lock);
    u64 run_start = 0, run_end = 0;
    u32 run_level = LEVEL_WRITE;
    for (u64 va = lo; va < hi; va += HOST_PAGE) {
        u32 level = level_of(s->watch_counts[(va - (u64)s->info.base) / HOST_PAGE]);
        if (level == LEVEL_WRITE || !protects(s, level))
            continue;
        if (run_end == va && level == run_level) {
            run_end += HOST_PAGE;
            continue;
        }
        if (run_end)
            protect_va(s, run_start, run_end, run_level);
        run_start = va;
        run_end = va + HOST_PAGE;
        run_level = level;
    }
    if (run_end)
        protect_va(s, run_start, run_end, run_level);
    ReleaseSRWLockShared(&g_track.lock);
}

void tracking_mapping_changed(const u8* base, u64 size) {
    if (!g_track.live)
        return;
    AcquireSRWLockShared(&g_mem.range_lock);
    AcquireSRWLockExclusive(&g_track.lock);
    end_watches_reporting_in((u64)base, (u64)base + size, false);
    ReleaseSRWLockExclusive(&g_track.lock);
    ReleaseSRWLockShared(&g_mem.range_lock);
}

void tracking_range_released(RangeSlot* s) {
    if (g_track.live) {
        AcquireSRWLockExclusive(&g_track.lock);
        u64 base = (u64)s->info.base, end = base + s->info.size;
        end_watches_reporting_in(base, end, true);
        // Memory of other ranges keyed by address here (none should be): gone with the range.
        if (!(s->info.flags & MEM_RANGE_PLACEHOLDER)) {
            for (u32 i = 0; i < g_track.by_base_count;) {
                u32 id = g_track.by_base[i];
                const Watch& w = g_track.watches[id];
                bool inside = false;
                for (u32 k = 0; k < w.span_count && !inside; k++)
                    inside =
                        !(w.spans[k].start & CANON_FRAME) && w.spans[k].start < end && w.spans[k].end > base;
                if (inside) {
                    end_watch(id, nullptr, nullptr);
                    continue;
                }
                i++;
            }
        }
        ReleaseSRWLockExclusive(&g_track.lock);
    }
    if (s->watch_counts) {
        long taken = 0;
        for (u64 i = 0; i < s->info.size / HOST_PAGE; i++)
            taken += (s->watch_counts[i] & TAKEN) != 0;
        InterlockedAdd(&g_track.taken, -taken);
    }
    free(s->watch_counts);
    s->watch_counts = nullptr;
}

void tracking_frame_freed(u32 id) {
    if (!g_track.taken)
        return;
    u16* c = &g_track.frame_counts[(u64)id * FRAME_HOST_PAGES];
    for (u32 p = 0; p < FRAME_HOST_PAGES; p++) {
        if (c[p] & TAKEN) {
            c[p] &= ~TAKEN;
            InterlockedDecrement(&g_track.taken);
        }
    }
}

// --- faults ------------------------------------------------------------------------------------------

// Whether watches or traps protect the host page at canonical address `key`. Range lock and tracking
// lock held.
static bool page_protected(u64 key) {
    const u16* c = page_count(key, false);
    return c && (*c & COUNT_MASK);
}

static bool page_taken(u64 key) {
    const u16* c = page_count(key, false);
    return c && (*c & TAKEN);
}

bool tracking_access_fault(u64 addr) {
    if (!g_track.taken)
        return false;
    u64 page = addr & ~(u64)(HOST_PAGE - 1);
    AcquireSRWLockShared(&g_mem.range_lock);
    const RangeSlot* s = find_range_locked((const void*)addr);
    u64 key = s ? canonical(s, page, true) : 0;
    bool taken = false;
    if (key) {
        AcquireSRWLockShared(&g_track.lock);
        taken = page_taken(key);
        ReleaseSRWLockShared(&g_track.lock);
    }
    ReleaseSRWLockShared(&g_mem.range_lock);
    if (!taken)
        return false;
    if (!g_track.taken_listener) {
        LOG_E("memory", "access to a taken page at %p with nobody to give it back", (void*)addr);
        return false;
    }
    g_track.taken_listener((const void*)addr); // gives the page back; the access is retried
    return true;
}

static void finish_trapped_pages(const u64* pages, const u64* keys, Report* reports, u32* report_count);

bool tracking_write_fault(EXCEPTION_POINTERS* ep, u64 addr) {
    if (!tracking_active())
        return false;
    u64 page = addr & ~(u64)(HOST_PAGE - 1);
    Report reports[MAX_REPORTS];
    u32 report_count = 0;
    bool handled = false, trapped = false;
    AcquireSRWLockShared(&g_mem.range_lock);
    const RangeSlot* s = find_range_locked((const void*)addr);
    u64 key = s ? canonical(s, page, true) : 0;
    if (key) {
        AcquireSRWLockExclusive(&g_track.lock);
        if (page_protected(key)) {
            end_watches_at(key, reports, &report_count);
            handled = true;
            trapped = page_protected(key); // only traps left
        }
        ReleaseSRWLockExclusive(&g_track.lock);
    }
    Report stores[MAX_REPORTS];
    u32 store_count = 0;
    if (trapped) {
        // The store runs alone with the page writable (single-stepped); tracking_single_step
        // protects it again and reports.
        u64 first = g_step_page[0].get();
        bool known = first == page || g_step_page[1].get() == page;
        if (!known && first && g_step_page[1].get()) {
            if (ep->ContextRecord->Rip == g_step_ip.get()) {
                // A repeated string store (memcpy's rep movsb) moving on: it is done with the pages
                // before, which are protected again now.
                u64 done[2] = {first, g_step_page[1].get()};
                u64 done_keys[2] = {g_step_key[0].get(), g_step_key[1].get()};
                for (u32 k = 0; k < 2; k++) {
                    g_step_page[k].set(0);
                    g_step_key[k].set(0);
                }
                AcquireSRWLockShared(&g_track.lock);
                finish_trapped_pages(done, done_keys, stores, &store_count);
                ReleaseSRWLockShared(&g_track.lock);
                first = 0;
            } else {
                LOG_E("memory", "a store at %p reaches a third trapped page", (void*)ep->ContextRecord->Rip);
                handled = false;
            }
        }
        if (handled) {
            DWORD old;
            if (!VirtualProtect((void*)page, HOST_PAGE, mapping_protect(s) & 0xFF, &old)) {
                handled = false;
            } else if (!known) {
                u32 k = first ? 1 : 0;
                g_step_page[k].set(page);
                g_step_key[k].set(key);
                if (!first)
                    g_step_ip.set(ep->ContextRecord->Rip);
            }
            ep->ContextRecord->EFlags |= TRAP_FLAG;
        }
    } else if (!handled && s) {
        // Another thread may have just released the page; a write it allows now is retried.
        handled = access_allowed((const void*)addr, 1) && key;
    }
    ReleaseSRWLockShared(&g_mem.range_lock);
    report_writes(reports, report_count);
    for (u32 i = 0; i < store_count && g_track.store_listener; i++)
        g_track.store_listener(stores[i].base, stores[i].size);
    return handled;
}

void tracking_describe_write_fault(u64 addr) {
    u64 page = addr & ~(u64)(HOST_PAGE - 1);
    AcquireSRWLockShared(&g_mem.range_lock);
    const RangeSlot* s = find_range_locked((const void*)addr);
    if (!s) {
        ReleaseSRWLockShared(&g_mem.range_lock);
        LOG_E("memory", "unclaimed write fault at 0x%llx: not title memory", addr);
        return;
    }
    u32 pending = MEM_INVALID_PFN, id = MEM_INVALID_PFN, region_first = 0, region_real = MEM_INVALID_PFN;
    if (s->info.flags & MEM_RANGE_PLACEHOLDER) {
        u32 p = (u32)((page - (u64)s->info.base) >> MEM_PAGE_SHIFT);
        pending = s->pending ? s->pending[p] : MEM_INVALID_PFN;
        AcquireSRWLockShared(&g_mem.phys_lock);
        id = frame_at_locked(s, p);
        const MemRegion& r = s->regions[region_find(s, p)];
        region_first = r.first_page;
        region_real = r.pfn;
        ReleaseSRWLockShared(&g_mem.phys_lock);
    }
    u64 key = canonical(s, page, false);
    u64 backed_key = canonical(s, page, true);
    AcquireSRWLockShared(&g_track.lock);
    const u16* count = key ? page_count(key, false) : nullptr;
    u32 n = count ? *count : 0;
    ReleaseSRWLockShared(&g_track.lock);
    LOG_E(
        "memory",
        "unclaimed write fault at 0x%llx: range %p+0x%llx flags 0x%x protect 0x%x; pending frame 0x%x, frame "
        "0x%x, region from page %u real 0x%x; key 0x%llx (backed 0x%llx), protection count %u",
        addr, s->info.base, s->info.size, s->info.flags, s->info.protect, pending, id, region_first,
        region_real, key, backed_key, n);
    ReleaseSRWLockShared(&g_mem.range_lock);
}

// Protects trapped pages a store has finished with again (those still trapped), noting the traps over
// them for the store listener. Range lock and tracking lock held shared.
static void finish_trapped_pages(const u64* pages, const u64* keys, Report* reports, u32* report_count) {
    for (u32 k = 0; k < 2 && pages[k]; k++) {
        const RangeSlot* s = find_range_locked((const void*)pages[k]);
        if (!s || !page_protected(keys[k]))
            continue;
        protect_va(s, pages[k], pages[k] + HOST_PAGE, true);
        u64 granule = keys[k] >> GRANULE_SHIFT;
        for (u32 n = g_track.buckets[granule_hash(granule)]; n != NO_NODE; n = g_track.nodes[n].next) {
            const GranuleNode& node = g_track.nodes[n];
            const Watch& w = g_track.watches[node.watch];
            if (node.granule != granule || !w.trap || *report_count == MAX_REPORTS)
                continue;
            if (watch_covers(w, keys[k])) // spans are host page aligned
                reports[(*report_count)++] = {w.base, w.size};
        }
    }
}

bool tracking_single_step(EXCEPTION_POINTERS* ep) {
    u64 pages[2] = {g_step_page[0].get(), g_step_page[1].get()};
    if (!pages[0])
        return false;
    if (ep->ContextRecord->Rip == g_step_ip.get()) { // a repeated string store isn't done yet
        ep->ContextRecord->EFlags |= TRAP_FLAG;
        return true;
    }
    u64 keys[2] = {g_step_key[0].get(), g_step_key[1].get()};
    ep->ContextRecord->EFlags &= ~TRAP_FLAG;
    for (u32 k = 0; k < 2; k++) {
        g_step_page[k].set(0);
        g_step_key[k].set(0);
    }
    // Protected again before the report, so a store the report doesn't see faults.
    Report reports[MAX_REPORTS];
    u32 report_count = 0;
    AcquireSRWLockShared(&g_mem.range_lock);
    AcquireSRWLockShared(&g_track.lock);
    finish_trapped_pages(pages, keys, reports, &report_count);
    ReleaseSRWLockShared(&g_track.lock);
    ReleaseSRWLockShared(&g_mem.range_lock);
    for (u32 i = 0; i < report_count && g_track.store_listener; i++)
        g_track.store_listener(reports[i].base, reports[i].size);
    return true;
}

// --- device writes -----------------------------------------------------------------------------------

struct DeviceWrites {
    HANDLE done;
    MemSpan* spans;
    u32 count;
    u64 lo, hi; // bounds of the spans
};

static struct {
    DeviceWrites* groups;
    u32 count;
    u32 capacity;
    SRWLOCK lock;
} g_device = {nullptr, 0, 0, SRWLOCK_INIT};

void mem_expect_device_writes(void* done, const MemSpan* spans, u32 count) {
    DeviceWrites g = {done, (MemSpan*)malloc((usize)count * sizeof(MemSpan)), count, UINT64_MAX, 0};
    if (!g.spans)
        FATAL("out of memory for device writes");
    memcpy(g.spans, spans, (usize)count * sizeof(MemSpan));
    for (u32 i = 0; i < count; i++) {
        g.lo = spans[i].base < g.lo ? spans[i].base : g.lo;
        g.hi = spans[i].base + spans[i].size > g.hi ? spans[i].base + spans[i].size : g.hi;
    }
    AcquireSRWLockExclusive(&g_device.lock);
    if (g_device.count == g_device.capacity) {
        u32 capacity = g_device.capacity ? g_device.capacity * 2 : 64;
        DeviceWrites* grown = (DeviceWrites*)realloc(g_device.groups, capacity * sizeof(DeviceWrites));
        if (!grown)
            FATAL("out of memory for device writes");
        g_device.groups = grown;
        g_device.capacity = capacity;
    }
    g_device.groups[g_device.count++] = g;
    ReleaseSRWLockExclusive(&g_device.lock);
}

bool mem_device_writes_outstanding() {
    return *(volatile u32*)&g_device.count != 0;
}

bool mem_device_writes_pending(u64 base, u64 size) {
    u64 end = base + size;
    bool pending = false;
    AcquireSRWLockShared(&g_device.lock);
    for (u32 i = 0; i < g_device.count && !pending; i++) {
        const DeviceWrites& g = g_device.groups[i];
        if (g.lo >= end || g.hi <= base)
            continue;
        for (u32 k = 0; k < g.count && !pending; k++)
            pending = g.spans[k].base < end && g.spans[k].base + g.spans[k].size > base;
    }
    ReleaseSRWLockShared(&g_device.lock);
    return pending;
}

void mem_poll_device_writes() {
    if (!mem_device_writes_outstanding())
        return;
    for (;;) {
        // One finished group at a time, reported without the lock held.
        DeviceWrites finished = {};
        AcquireSRWLockExclusive(&g_device.lock);
        for (u32 i = 0; i < g_device.count; i++) {
            if (WaitForSingleObject(g_device.groups[i].done, 0) != WAIT_OBJECT_0)
                continue;
            finished = g_device.groups[i];
            memmove(&g_device.groups[i], &g_device.groups[i + 1],
                    (g_device.count - i - 1) * sizeof(DeviceWrites));
            g_device.count--;
            break;
        }
        ReleaseSRWLockExclusive(&g_device.lock);
        if (!finished.done)
            return;
        CloseHandle(finished.done);
        for (u32 k = 0; k < finished.count; k++)
            notify_write(finished.spans[k].base, finished.spans[k].size, MEM_DEVICE_WROTE);
        free(finished.spans);
    }
}

// --- where frames show -------------------------------------------------------------------------------

u32 mem_frame_addresses(u64 pfn, u64* vas, u32 max) {
    u32 n = 0;
    AcquireSRWLockShared(&g_mem.range_lock);
    u64 id = pfn >> MEM_PAGE_NUMBER_SHIFT;
    bool valid = !(pfn & ((1u << MEM_PAGE_NUMBER_SHIFT) - 1)) && id && id < g_mem.total_pages;
    if (valid && g_mem.frame_va[id]) {
        if (n < max)
            vas[n] = g_mem.frame_va[id];
        n++;
        for (u32 a = alias_lower_bound((u32)id); a < g_mem.alias_count && g_mem.aliases[a].id == id; a++) {
            if (g_mem.aliases[a].va == g_mem.frame_va[id])
                continue;
            if (n < max)
                vas[n] = g_mem.aliases[a].va;
            n++;
        }
    }
    ReleaseSRWLockShared(&g_mem.range_lock);
    return n;
}
