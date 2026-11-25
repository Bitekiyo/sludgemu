#include "hle/d3d12/frame_pacing.h"

#include "common/log.h"
#include "gpu/presenter.h"

#define MAX_SIGNALS      7
#define MAX_WAIT_HANDLES 63
#define MAX_PENDING      16 // presented frames waiting to flip; titles keep at most a period's worth
#define MAX_FLIP_WAITERS 8  // threads blocked on flips at once

#define SCHEDULE_FLAG_DISABLE_SIGNAL       0x1
#define WAIT_FLAG_ALL                      0x1
#define WAIT_FLAG_PROCESS_TOKEN_ON_TIMEOUT 0x2

struct EventSchedule {
    bool enabled;
    u32 signal_count;
    HANDLE signals[MAX_SIGNALS];
};

struct PendingFlip {
    FrameToken token;
    u64 present_done; // presenter_completed() value at which the GPU is done with it
};

// A thread waiting for a frame to reach the display (or, without a token, for the next flip).
struct FlipWaiter {
    HANDLE event; // auto-reset, owned by the pacer
    bool used;
    FrameToken token;
    u64 flips_before; // frames_flipped when the wait began
};

struct FramePacer {
    SRWLOCK lock;
    HANDLE thread;
    HANDLE timer;  // high-resolution waitable timer driving the timeline
    HANDLE wakeup; // auto-reset: settings changed or a frame flipped

    u32 length_us; // 0 until SetFrameIntervalX
    u32 period;
    u32 origin_offset_us;
    EventSchedule events[FRAME_EVENT_COUNT];

    // Manual-reset events the waits block on. origin_ready stays set while an origin has fired
    // and its token hasn't been handed out yet.
    HANDLE ready[FRAME_EVENT_COUNT];

    u64 qpc_frequency;
    bool timeline_running;
    u64 timeline_start;       // QPC time of interval 0
    u64 intervals_done;       // index of the next interval boundary (= current interval + 1)
    u64 last_origin_interval; // intervals_done at the last origin, so one origin per interval

    u64 origins_fired;
    u64 tokens_issued;
    u64 frames_flipped;
    FrameToken last_flipped; // the frame on the display

    PendingFlip pending[MAX_PENDING];
    u32 pending_first;
    u32 pending_count;
    FlipWaiter flip_waiters[MAX_FLIP_WAITERS];
};

static FramePacer g_pacer;
static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;

static u64 qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (u64)t.QuadPart;
}

static u64 us_to_qpc(u64 us) {
    return us * g_pacer.qpc_frequency / 1000000;
}

static void signal_event(FrameEventType type) {
    EventSchedule& e = g_pacer.events[type];
    for (u32 i = 0; i < e.signal_count; i++)
        SetEvent(e.signals[i]);
}

static bool flip_wait_satisfied(const FlipWaiter& w) {
    return w.token ? g_pacer.last_flipped >= w.token : g_pacer.frames_flipped > w.flips_before;
}

// Puts a frame on the display (lock held).
static void flip_locked(FrameToken token) {
    g_pacer.frames_flipped++;
    if (token > g_pacer.last_flipped)
        g_pacer.last_flipped = token;
    for (u32 i = 0; i < MAX_FLIP_WAITERS; i++) {
        FlipWaiter& w = g_pacer.flip_waiters[i];
        if (w.used && flip_wait_satisfied(w))
            SetEvent(w.event);
    }
    if (g_pacer.events[FRAME_EVENT_FLIP].enabled)
        signal_event(FRAME_EVENT_FLIP);
}

// Takes the oldest presented frame off the queue (lock held).
static FrameToken pop_pending_locked() {
    FrameToken token = g_pacer.pending[g_pacer.pending_first].token;
    g_pacer.pending_first = (g_pacer.pending_first + 1) % MAX_PENDING;
    g_pacer.pending_count--;
    return token;
}

// At a vblank: the oldest presented frame flips if the GPU has finished it (lock held).
static void flip_pending_locked() {
    if (!g_pacer.pending_count)
        return;
    const PendingFlip& f = g_pacer.pending[g_pacer.pending_first];
    if (f.present_done && presenter_completed() < f.present_done)
        return;
    flip_locked(pop_pending_locked());
}

// Origins may fire while fewer than `period` frames are between their start and the display.
static bool origin_allowed() {
    return g_pacer.tokens_issued - g_pacer.frames_flipped < g_pacer.period;
}

static DWORD WINAPI scheduler_thread(void*) {
    for (;;) {
        AcquireSRWLockExclusive(&g_pacer.lock);
        if (!g_pacer.length_us || !g_pacer.events[FRAME_EVENT_ORIGIN].enabled) {
            ReleaseSRWLockExclusive(&g_pacer.lock);
            WaitForSingleObject(g_pacer.wakeup, INFINITE);
            continue;
        }

        // Next point of interest: the coming interval boundary (vblank) or the origin inside the
        // current interval, whichever is earlier.
        u64 length = us_to_qpc(g_pacer.length_us);
        u64 boundary = g_pacer.timeline_start + g_pacer.intervals_done * length;
        u64 origin = boundary - length + us_to_qpc(g_pacer.origin_offset_us);
        u64 now = qpc_now();

        if (now >= boundary) {
            g_pacer.intervals_done++;
            SetEvent(g_pacer.ready[FRAME_EVENT_VBLANK]);
            if (g_pacer.events[FRAME_EVENT_VBLANK].enabled)
                signal_event(FRAME_EVENT_VBLANK);
            flip_pending_locked();
            // The origin of the new interval is due at boundary + offset; a pending one from the
            // previous interval that was withheld stays pending.
        } else if (now >= origin && g_pacer.last_origin_interval < g_pacer.intervals_done &&
                   g_pacer.origins_fired == g_pacer.tokens_issued && origin_allowed()) {
            // A withheld origin is not made up later: the next one comes at its regular offset.
            g_pacer.last_origin_interval = g_pacer.intervals_done;
            g_pacer.origins_fired++;
            SetEvent(g_pacer.ready[FRAME_EVENT_ORIGIN]);
            signal_event(FRAME_EVENT_ORIGIN);
        }

        u64 next = boundary;
        if (now < origin)
            next = origin;
        ReleaseSRWLockExclusive(&g_pacer.lock);

        if (next > now) {
            LARGE_INTEGER due;
            due.QuadPart = -(s64)((next - now) * 10000000 / g_pacer.qpc_frequency); // relative, 100 ns
            SetWaitableTimer(g_pacer.timer, &due, 0, nullptr, nullptr, FALSE);
            HANDLE handles[2] = {g_pacer.timer, g_pacer.wakeup};
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        }
    }
}

static BOOL CALLBACK init_pacer(PINIT_ONCE, void*, void**) {
    InitializeSRWLock(&g_pacer.lock);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_pacer.qpc_frequency = (u64)f.QuadPart;
    g_pacer.wakeup = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_pacer.timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    bool ok = g_pacer.wakeup && g_pacer.timer;
    for (u32 i = 0; i < FRAME_EVENT_COUNT; i++) {
        g_pacer.ready[i] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ok = ok && g_pacer.ready[i];
    }
    for (u32 i = 0; i < MAX_FLIP_WAITERS; i++) {
        g_pacer.flip_waiters[i].event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ok = ok && g_pacer.flip_waiters[i].event;
    }
    if (!ok)
        FATAL("cannot create frame pacing objects (%lu)", GetLastError());
    g_pacer.thread = CreateThread(nullptr, 0, scheduler_thread, nullptr, 0, nullptr);
    if (!g_pacer.thread)
        FATAL("cannot create the frame pacing thread (%lu)", GetLastError());
    SetThreadPriority(g_pacer.thread, THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadDescription(g_pacer.thread, L"sludgemu frame pacing");
    return TRUE;
}

static void ensure_init() {
    InitOnceExecuteOnce(&g_init_once, init_pacer, nullptr, nullptr);
}

HRESULT frame_set_interval(u32 length_us, u32 period, u32 flags) {
    ensure_init();
    if (length_us == 0 || period < 1 || period > 4)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&g_pacer.lock);
    g_pacer.length_us = length_us;
    g_pacer.period = period;
    ReleaseSRWLockExclusive(&g_pacer.lock);
    SetEvent(g_pacer.wakeup);
    LOG_I("d3d12", "frame interval %u us (%.2f Hz), period %u, flags 0x%x", length_us, 1e6 / length_us,
          period, flags);
    return S_OK;
}

HRESULT frame_schedule_event(FrameEventType type, u32 offset_us, const FrameSignalList* signals, u32 flags) {
    ensure_init();
    if (type >= FRAME_EVENT_COUNT || (signals && signals->count > MAX_SIGNALS))
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&g_pacer.lock);
    if (!g_pacer.length_us) {
        ReleaseSRWLockExclusive(&g_pacer.lock);
        return E_INVALIDARG; // the interval has to be set first
    }
    EventSchedule& e = g_pacer.events[type];
    e.enabled = type == FRAME_EVENT_ORIGIN || !(flags & SCHEDULE_FLAG_DISABLE_SIGNAL);
    e.signal_count = signals ? signals->count : 0;
    for (u32 i = 0; i < e.signal_count; i++)
        e.signals[i] = signals->objects[i];
    if (type == FRAME_EVENT_ORIGIN) {
        g_pacer.origin_offset_us = offset_us % g_pacer.length_us;
        if (!g_pacer.timeline_running) {
            g_pacer.timeline_running = true;
            // Start the timeline so that the first origin is due right away.
            g_pacer.timeline_start = qpc_now() - us_to_qpc(g_pacer.origin_offset_us);
            g_pacer.intervals_done = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_pacer.lock);
    SetEvent(g_pacer.wakeup);
    LOG_D("d3d12", "ScheduleFrameEventX(type %u, offset %u us, %u signals, flags 0x%x)", type, offset_us,
          signals ? signals->count : 0, flags);
    return S_OK;
}

// Hands out the token for a fired origin (lock held).
static FrameToken take_origin_locked() {
    g_pacer.tokens_issued++;
    if (g_pacer.tokens_issued >= g_pacer.origins_fired)
        ResetEvent(g_pacer.ready[FRAME_EVENT_ORIGIN]);
    return g_pacer.tokens_issued; // tokens are 1-based frame numbers
}

// Flip waits are for the caller's frame: they return at once if it's already on the display.
static HRESULT wait_flip(u32 timeout_ms, const FrameWaitList* extra, u32 flags, FrameToken token) {
    AcquireSRWLockExclusive(&g_pacer.lock);
    FlipWaiter probe = {nullptr, true, token, g_pacer.frames_flipped};
    if (flip_wait_satisfied(probe)) {
        ReleaseSRWLockExclusive(&g_pacer.lock);
        if (extra && extra->signaled_index)
            *extra->signaled_index = FRAME_SIGNALED_PRIMARY;
        return S_OK;
    }
    FlipWaiter* w = nullptr;
    for (u32 i = 0; i < MAX_FLIP_WAITERS && !w; i++)
        if (!g_pacer.flip_waiters[i].used)
            w = &g_pacer.flip_waiters[i];
    if (!w) {
        ReleaseSRWLockExclusive(&g_pacer.lock);
        LOG_E("d3d12", "more than %u threads wait for flips at once", MAX_FLIP_WAITERS);
        return E_FAIL;
    }
    ResetEvent(w->event);
    w->used = true;
    w->token = token;
    w->flips_before = probe.flips_before;
    ReleaseSRWLockExclusive(&g_pacer.lock);

    HANDLE handles[1 + MAX_WAIT_HANDLES];
    u32 count = 0;
    handles[count++] = w->event;
    for (u32 i = 0; extra && i < extra->count; i++)
        handles[count++] = extra->objects[i];
    DWORD r = WaitForMultipleObjects(count, handles, (flags & WAIT_FLAG_ALL) != 0, timeout_ms);

    AcquireSRWLockExclusive(&g_pacer.lock);
    w->used = false;
    ReleaseSRWLockExclusive(&g_pacer.lock);
    if (r == WAIT_FAILED)
        return HRESULT_FROM_WIN32(GetLastError());
    u32 signaled = FRAME_SIGNALED_NONE;
    if (r < WAIT_OBJECT_0 + count)
        signaled = r == WAIT_OBJECT_0 ? FRAME_SIGNALED_PRIMARY : (u32)(r - WAIT_OBJECT_0 - 1);
    if (extra && extra->signaled_index)
        *extra->signaled_index = signaled;
    return r == WAIT_TIMEOUT ? S_FALSE : S_OK;
}

HRESULT frame_wait_event(FrameEventType type, u32 timeout_ms, const FrameWaitList* extra, u32 flags,
                         FrameToken* token) {
    ensure_init();
    if (type >= FRAME_EVENT_COUNT || (extra && extra->count > MAX_WAIT_HANDLES))
        return E_INVALIDARG;
    if (type == FRAME_EVENT_ORIGIN && !g_pacer.events[FRAME_EVENT_ORIGIN].enabled)
        return E_INVALIDARG;
    if (type == FRAME_EVENT_FLIP)
        return wait_flip(timeout_ms, extra, flags, token ? *token : 0);

    HANDLE handles[1 + MAX_WAIT_HANDLES];
    u32 count = 0;
    handles[count++] = g_pacer.ready[type];
    for (u32 i = 0; extra && i < extra->count; i++)
        handles[count++] = extra->objects[i];

    // VBLANK waits are for the next one.
    if (type == FRAME_EVENT_VBLANK)
        ResetEvent(g_pacer.ready[type]);

    for (;;) {
        DWORD r = WaitForMultipleObjects(count, handles, (flags & WAIT_FLAG_ALL) != 0, timeout_ms);
        u32 signaled = FRAME_SIGNALED_NONE;
        if (r < WAIT_OBJECT_0 + count)
            signaled = (r == WAIT_OBJECT_0) ? FRAME_SIGNALED_PRIMARY : (u32)(r - WAIT_OBJECT_0 - 1);

        bool timed_out = r == WAIT_TIMEOUT;
        if (timed_out && !(flags & WAIT_FLAG_PROCESS_TOKEN_ON_TIMEOUT)) {
            if (extra && extra->signaled_index)
                *extra->signaled_index = FRAME_SIGNALED_NONE;
            return S_FALSE;
        }
        if (r == WAIT_FAILED)
            return HRESULT_FROM_WIN32(GetLastError());

        if (type == FRAME_EVENT_ORIGIN && token) {
            AcquireSRWLockExclusive(&g_pacer.lock);
            bool available = g_pacer.origins_fired > g_pacer.tokens_issued;
            if (available || timed_out)
                *token = take_origin_locked();
            ReleaseSRWLockExclusive(&g_pacer.lock);
            if (!available && !timed_out && signaled == FRAME_SIGNALED_PRIMARY)
                continue; // another thread took this origin
        }
        if (extra && extra->signaled_index)
            *extra->signaled_index = signaled;
        return timed_out ? S_FALSE : S_OK;
    }
}

void frame_on_present(FrameToken token, u64 present_done) {
    ensure_init();
    AcquireSRWLockExclusive(&g_pacer.lock);
    if (!g_pacer.timeline_running) {
        flip_locked(token); // no display timeline to wait for
    } else {
        if (g_pacer.pending_count == MAX_PENDING) {
            LOG_W_ONCE("d3d12", "more than %u presented frames wait to flip; flipping the oldest early",
                       MAX_PENDING);
            flip_locked(pop_pending_locked());
        }
        PendingFlip& f = g_pacer.pending[(g_pacer.pending_first + g_pacer.pending_count) % MAX_PENDING];
        f.token = token;
        f.present_done = present_done;
        g_pacer.pending_count++;
    }
    ReleaseSRWLockExclusive(&g_pacer.lock);
}
