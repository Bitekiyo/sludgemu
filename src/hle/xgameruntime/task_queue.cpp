// XTaskQueue: callback queues with per-port dispatch modes, built on the Windows thread pool.
//
// Lifetime: a queue is reference counted by its handles and by every callback submitted through
// it, so it stays alive until its last callback has run. Ports are reference counted by the
// queues using them (composite queues share ports) and by pending thread-pool callbacks.
// Delayed callbacks wait in one runtime-wide list driven by a single thread-pool timer.

#include "hle/xgameruntime/task_queue.h"

#include <stdlib.h>

#include "common/log.h"
#include "common/thread_slot.h"
#include "hle/xgameruntime/async.h"
#include "hle/xgameruntime/runtime_api.h"

#define QUEUE_MAGIC  0x51545851u // "QXTQ"
#define PORT_MAGIC   0x50545851u // "QXTP"
#define MAX_MONITORS 8

typedef void CALLBACK TaskMonitorCallback(void* context, TaskQueue* queue, TaskPortId port);

struct TaskPort;

struct TaskEntry {
    TaskEntry* next;
    TaskCallback* callback;
    void* context;
    TaskQueue* owner; // queue the callback was submitted through; holds a reference to it
    TaskPort* port;
    TaskPortId port_id;
    u64 due; // GetTickCount64() time, for delayed entries
};

struct TaskPort {
    u32 magic;
    TaskDispatchMode mode;
    volatile long refs;
    SRWLOCK lock;
    TaskEntry* head;
    TaskEntry* tail;
    volatile long ready_seq; // bumped on every enqueue; XTaskQueueDispatch waits on it
    bool draining;           // serialized mode: a thread-pool callback is draining the port
};

struct TaskMonitor {
    u64 token;
    void* context;
    TaskMonitorCallback* callback;
};

struct TaskQueue {
    u32 magic;
    volatile long refs;
    volatile long active; // callbacks submitted and not yet finished
    volatile long terminated;
    TaskPort* ports[2];
    SRWLOCK monitor_lock;
    TaskMonitor monitors[MAX_MONITORS];
    u32 monitor_count;
};

static TaskQueue* g_process_queue = nullptr;
static SRWLOCK g_process_queue_lock = SRWLOCK_INIT;

static SRWLOCK g_delayed_lock = SRWLOCK_INIT;
static TaskEntry* g_delayed = nullptr; // sorted by due time
static PTP_TIMER g_delayed_timer = nullptr;

static volatile s64 g_next_token = 1;

// --- ports ---------------------------------------------------------------------------------------

static TaskPort* port_create(TaskDispatchMode mode) {
    TaskPort* p = (TaskPort*)calloc(1, sizeof(TaskPort));
    p->magic = PORT_MAGIC;
    p->mode = mode;
    p->refs = 1;
    InitializeSRWLock(&p->lock);
    return p;
}

static void port_add_ref(TaskPort* p) {
    InterlockedIncrement(&p->refs);
}

static void port_release(TaskPort* p) {
    if (InterlockedDecrement(&p->refs) == 0) {
        p->magic = 0;
        free(p);
    }
}

static TaskEntry* port_pop(TaskPort* p) {
    AcquireSRWLockExclusive(&p->lock);
    TaskEntry* e = p->head;
    if (e) {
        p->head = e->next;
        if (!p->head)
            p->tail = nullptr;
    }
    ReleaseSRWLockExclusive(&p->lock);
    return e;
}

// --- running callbacks ---------------------------------------------------------------------------

static void queue_add_ref(TaskQueue* q) {
    InterlockedIncrement(&q->refs);
}

static void queue_release(TaskQueue* q) {
    if (InterlockedDecrement(&q->refs) == 0) {
        port_release(q->ports[TASK_PORT_WORK]);
        port_release(q->ports[TASK_PORT_COMPLETION]);
        q->magic = 0;
        free(q);
    }
}

static void run_entry(TaskEntry* e, bool canceled) {
    TaskQueue* q = e->owner;
    e->callback(e->context, canceled);
    free(e);
    if (InterlockedDecrement(&q->active) == 0)
        WakeByAddressAll((void*)&q->active);
    queue_release(q);
}

static void CALLBACK pool_run_one(PTP_CALLBACK_INSTANCE, void* context) {
    TaskPort* p = (TaskPort*)context;
    if (TaskEntry* e = port_pop(p))
        run_entry(e, false);
    port_release(p);
}

static void CALLBACK pool_drain(PTP_CALLBACK_INSTANCE, void* context) {
    TaskPort* p = (TaskPort*)context;
    for (;;) {
        AcquireSRWLockExclusive(&p->lock);
        TaskEntry* e = p->head;
        if (!e) {
            p->draining = false;
            ReleaseSRWLockExclusive(&p->lock);
            break;
        }
        p->head = e->next;
        if (!p->head)
            p->tail = nullptr;
        ReleaseSRWLockExclusive(&p->lock);
        run_entry(e, false);
    }
    port_release(p);
}

static void submit_to_pool(TaskPort* p, PTP_SIMPLE_CALLBACK fn) {
    port_add_ref(p);
    if (!TrySubmitThreadpoolCallback(fn, p, nullptr))
        FATAL("TrySubmitThreadpoolCallback failed (%lu)", GetLastError());
}

static void notify_monitors(TaskQueue* q, TaskPortId port) {
    TaskMonitor monitors[MAX_MONITORS];
    AcquireSRWLockShared(&q->monitor_lock);
    u32 count = q->monitor_count;
    memcpy(monitors, q->monitors, count * sizeof(TaskMonitor));
    ReleaseSRWLockShared(&q->monitor_lock);
    for (u32 i = 0; i < count; i++)
        monitors[i].callback(monitors[i].context, q, port);
}

// Makes an entry runnable on its port according to the port's dispatch mode.
static void schedule_entry(TaskEntry* e) {
    TaskPort* p = e->port;
    if (p->mode == TASK_DISPATCH_IMMEDIATE) {
        TaskQueue* q = e->owner;
        TaskPortId port_id = e->port_id;
        queue_add_ref(q); // keep q alive for the monitors; run_entry drops the entry's reference
        run_entry(e, false);
        notify_monitors(q, port_id);
        queue_release(q);
        return;
    }

    TaskQueue* q = e->owner;
    TaskPortId port_id = e->port_id;
    queue_add_ref(q);

    bool start_drain = false;
    AcquireSRWLockExclusive(&p->lock);
    e->next = nullptr;
    if (p->tail)
        p->tail->next = e;
    else
        p->head = e;
    p->tail = e;
    InterlockedIncrement(&p->ready_seq);
    if (p->mode == TASK_DISPATCH_SERIALIZED_THREAD_POOL && !p->draining) {
        p->draining = true;
        start_drain = true;
    }
    ReleaseSRWLockExclusive(&p->lock);
    WakeByAddressAll((void*)&p->ready_seq);

    if (p->mode == TASK_DISPATCH_THREAD_POOL)
        submit_to_pool(p, pool_run_one);
    else if (start_drain)
        submit_to_pool(p, pool_drain);

    notify_monitors(q, port_id);
    queue_release(q);
}

// --- delayed callbacks ---------------------------------------------------------------------------

static void arm_delayed_timer_locked() {
    if (!g_delayed) {
        SetThreadpoolTimer(g_delayed_timer, nullptr, 0, 0);
        return;
    }
    u64 now = GetTickCount64();
    u64 wait_ms = g_delayed->due > now ? g_delayed->due - now : 0;
    LARGE_INTEGER due;
    due.QuadPart = -(s64)(wait_ms * 10000); // relative, 100 ns units
    FILETIME ft;
    ft.dwLowDateTime = due.LowPart;
    ft.dwHighDateTime = (DWORD)due.HighPart;
    SetThreadpoolTimer(g_delayed_timer, &ft, 0, 0);
}

static void CALLBACK delayed_timer_fired(PTP_CALLBACK_INSTANCE, void*, PTP_TIMER) {
    AcquireSRWLockExclusive(&g_delayed_lock);
    u64 now = GetTickCount64();
    TaskEntry* due = nullptr;
    TaskEntry** due_tail = &due;
    while (g_delayed && g_delayed->due <= now) {
        TaskEntry* e = g_delayed;
        g_delayed = e->next;
        e->next = nullptr;
        *due_tail = e;
        due_tail = &e->next;
    }
    arm_delayed_timer_locked();
    ReleaseSRWLockExclusive(&g_delayed_lock);

    while (due) {
        TaskEntry* e = due;
        due = e->next;
        schedule_entry(e);
    }
}

static void add_delayed(TaskEntry* e) {
    AcquireSRWLockExclusive(&g_delayed_lock);
    TaskEntry** link = &g_delayed;
    while (*link && (*link)->due <= e->due)
        link = &(*link)->next;
    e->next = *link;
    *link = e;
    if (g_delayed == e)
        arm_delayed_timer_locked();
    ReleaseSRWLockExclusive(&g_delayed_lock);
}

// --- queues --------------------------------------------------------------------------------------

static TaskQueue* queue_create(TaskPort* work, TaskPort* completion) {
    TaskQueue* q = (TaskQueue*)calloc(1, sizeof(TaskQueue));
    q->magic = QUEUE_MAGIC;
    q->refs = 1;
    q->ports[TASK_PORT_WORK] = work;
    q->ports[TASK_PORT_COMPLETION] = completion;
    InitializeSRWLock(&q->monitor_lock);
    return q;
}

static bool valid_queue(const TaskQueue* q) {
    return q && q->magic == QUEUE_MAGIC;
}
static bool valid_port(const TaskPort* p) {
    return p && p->magic == PORT_MAGIC;
}
static bool valid_mode(u32 mode) {
    return mode <= TASK_DISPATCH_IMMEDIATE;
}

static ThreadSlot g_time_sensitive;

void task_queue_init() {
    g_time_sensitive.init();
    g_delayed_timer = CreateThreadpoolTimer(delayed_timer_fired, nullptr, nullptr);
    if (!g_delayed_timer)
        FATAL("CreateThreadpoolTimer failed (%lu)", GetLastError());
    g_process_queue =
        queue_create(port_create(TASK_DISPATCH_THREAD_POOL), port_create(TASK_DISPATCH_THREAD_POOL));
}

TaskQueue* task_queue_acquire(TaskQueue* handle) {
    if (handle) {
        if (!valid_queue(handle))
            return nullptr;
        queue_add_ref(handle);
        return handle;
    }
    AcquireSRWLockShared(&g_process_queue_lock);
    TaskQueue* q = g_process_queue;
    if (q)
        queue_add_ref(q);
    ReleaseSRWLockShared(&g_process_queue_lock);
    return q;
}

void task_queue_release(TaskQueue* queue) {
    queue_release(queue);
}

HRESULT task_queue_submit(TaskQueue* q, TaskPortId port, u32 delay_ms, void* context,
                          TaskCallback* callback) {
    if (!valid_queue(q) || port > TASK_PORT_COMPLETION || !callback)
        return E_INVALIDARG;
    if (q->terminated)
        return E_ABORT;

    TaskEntry* e = (TaskEntry*)calloc(1, sizeof(TaskEntry));
    e->callback = callback;
    e->context = context;
    e->owner = q;
    e->port = q->ports[port];
    e->port_id = port;
    queue_add_ref(q);
    InterlockedIncrement(&q->active);

    if (delay_ms) {
        e->due = GetTickCount64() + delay_ms;
        add_delayed(e);
    } else {
        schedule_entry(e);
    }
    return S_OK;
}

// Removes every pending callback submitted through q, from its ports and the delayed list.
static TaskEntry* take_pending(TaskQueue* q) {
    TaskEntry* taken = nullptr;
    for (u32 i = 0; i < 2; i++) {
        TaskPort* p = q->ports[i];
        AcquireSRWLockExclusive(&p->lock);
        TaskEntry** link = &p->head;
        p->tail = nullptr;
        while (*link) {
            TaskEntry* e = *link;
            if (e->owner == q) {
                *link = e->next;
                e->next = taken;
                taken = e;
            } else {
                p->tail = e;
                link = &e->next;
            }
        }
        ReleaseSRWLockExclusive(&p->lock);
    }
    AcquireSRWLockExclusive(&g_delayed_lock);
    TaskEntry** link = &g_delayed;
    while (*link) {
        TaskEntry* e = *link;
        if (e->owner == q) {
            *link = e->next;
            e->next = taken;
            taken = e;
        } else {
            link = &e->next;
        }
    }
    arm_delayed_timer_locked();
    ReleaseSRWLockExclusive(&g_delayed_lock);
    return taken;
}

static void wait_until_idle(TaskQueue* q) {
    for (;;) {
        long active = q->active;
        if (active == 0)
            return;
        WaitOnAddress(&q->active, &active, sizeof(active), INFINITE);
    }
}

struct TerminateRequest {
    TaskQueue* queue;
    void* context;
    void(CALLBACK* callback)(void* context);
};

static void finish_terminate(TaskQueue* q, void* context, void(CALLBACK* callback)(void*)) {
    wait_until_idle(q);
    if (callback)
        callback(context);
}

static void CALLBACK pool_finish_terminate(PTP_CALLBACK_INSTANCE, void* param) {
    TerminateRequest* req = (TerminateRequest*)param;
    finish_terminate(req->queue, req->context, req->callback);
    queue_release(req->queue);
    free(req);
}

// --- XTaskQueue API ------------------------------------------------------------------------------

static HRESULT WINAPI XTaskQueueCreate(RuntimeSelf, u32 work_mode, u32 completion_mode, TaskQueue** out) {
    if (!out || !valid_mode(work_mode) || !valid_mode(completion_mode))
        return E_INVALIDARG;
    *out = queue_create(port_create((TaskDispatchMode)work_mode),
                        port_create((TaskDispatchMode)completion_mode));
    return S_OK;
}

static HRESULT WINAPI XTaskQueueCreateComposite(RuntimeSelf, TaskPort* work, TaskPort* completion,
                                                TaskQueue** out) {
    if (!out || !valid_port(work) || !valid_port(completion))
        return E_INVALIDARG;
    port_add_ref(work);
    port_add_ref(completion);
    *out = queue_create(work, completion);
    return S_OK;
}

static HRESULT WINAPI XTaskQueueGetPort(RuntimeSelf, TaskQueue* q, u32 port, TaskPort** out) {
    if (!out || !valid_queue(q) || port > TASK_PORT_COMPLETION)
        return E_INVALIDARG;
    *out = q->ports[port];
    return S_OK;
}

static HRESULT WINAPI XTaskQueueDuplicateHandle(RuntimeSelf, TaskQueue* q, TaskQueue** out) {
    if (!out || !valid_queue(q))
        return E_INVALIDARG;
    queue_add_ref(q);
    *out = q;
    return S_OK;
}

static bool WINAPI XTaskQueueDispatch(RuntimeSelf, TaskQueue* q, u32 port, u32 timeout_ms) {
    if (!valid_queue(q) || port > TASK_PORT_COMPLETION)
        return false;
    TaskPort* p = q->ports[port];
    u64 start = GetTickCount64();
    for (;;) {
        long seq = p->ready_seq;
        if (TaskEntry* e = port_pop(p)) {
            run_entry(e, false);
            return true;
        }
        if (timeout_ms == 0)
            return false;
        DWORD wait_ms = INFINITE;
        if (timeout_ms != INFINITE) {
            u64 elapsed = GetTickCount64() - start;
            if (elapsed >= timeout_ms)
                return false;
            wait_ms = (DWORD)(timeout_ms - elapsed);
        }
        WaitOnAddress(&p->ready_seq, &seq, sizeof(seq), wait_ms);
    }
}

static void WINAPI XTaskQueueCloseHandle(RuntimeSelf, TaskQueue* q) {
    if (valid_queue(q))
        queue_release(q);
}

static HRESULT WINAPI XTaskQueueSubmitCallback(RuntimeSelf, TaskQueue* q, u32 port, void* context,
                                               TaskCallback* callback) {
    return task_queue_submit(q, (TaskPortId)port, 0, context, callback);
}

static HRESULT WINAPI XTaskQueueSubmitDelayedCallback(RuntimeSelf, TaskQueue* q, u32 port, u32 delay_ms,
                                                      void* context, TaskCallback* callback) {
    return task_queue_submit(q, (TaskPortId)port, delay_ms, context, callback);
}

// Cancels pending callbacks (invoking them with canceled = true), waits for running ones, then
// invokes the terminated callback. With wait = false the waiting happens on the thread pool.
static HRESULT WINAPI XTaskQueueTerminate(RuntimeSelf, TaskQueue* q, bool wait, void* context,
                                          void(CALLBACK* callback)(void*)) {
    if (!valid_queue(q))
        return E_INVALIDARG;
    if (InterlockedExchange(&q->terminated, 1))
        return E_ABORT;

    queue_add_ref(q);
    TaskEntry* canceled = take_pending(q);
    while (canceled) {
        TaskEntry* e = canceled;
        canceled = e->next;
        run_entry(e, true);
    }

    if (wait) {
        finish_terminate(q, context, callback);
        queue_release(q);
        return S_OK;
    }
    TerminateRequest* req = (TerminateRequest*)calloc(1, sizeof(TerminateRequest));
    req->queue = q;
    req->context = context;
    req->callback = callback;
    if (!TrySubmitThreadpoolCallback(pool_finish_terminate, req, nullptr))
        FATAL("TrySubmitThreadpoolCallback failed (%lu)", GetLastError());
    return S_OK;
}

static HRESULT WINAPI XTaskQueueRegisterMonitor(RuntimeSelf, TaskQueue* q, void* context,
                                                TaskMonitorCallback* callback, u64* token) {
    if (!valid_queue(q) || !callback || !token)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&q->monitor_lock);
    if (q->monitor_count == MAX_MONITORS) {
        ReleaseSRWLockExclusive(&q->monitor_lock);
        LOG_E("xgr", "XTaskQueueRegisterMonitor: more than %u monitors on one queue", MAX_MONITORS);
        return E_OUTOFMEMORY;
    }
    TaskMonitor& m = q->monitors[q->monitor_count++];
    m.token = (u64)InterlockedIncrement64(&g_next_token);
    m.context = context;
    m.callback = callback;
    *token = m.token;
    ReleaseSRWLockExclusive(&q->monitor_lock);
    return S_OK;
}

static void WINAPI XTaskQueueUnregisterMonitor(RuntimeSelf, TaskQueue* q, u64 token) {
    if (!valid_queue(q))
        return;
    AcquireSRWLockExclusive(&q->monitor_lock);
    for (u32 i = 0; i < q->monitor_count; i++) {
        if (q->monitors[i].token == token) {
            q->monitors[i] = q->monitors[--q->monitor_count];
            break;
        }
    }
    ReleaseSRWLockExclusive(&q->monitor_lock);
}

// Returns a new handle to the process queue, which the caller closes.
static bool WINAPI XTaskQueueGetCurrentProcessTaskQueue(RuntimeSelf, TaskQueue** out) {
    TaskQueue* q = task_queue_acquire(nullptr);
    *out = q;
    return q != nullptr;
}

static void WINAPI XTaskQueueSetCurrentProcessTaskQueue(RuntimeSelf, TaskQueue* q) {
    if (q && !valid_queue(q))
        return;
    if (q)
        queue_add_ref(q);
    AcquireSRWLockExclusive(&g_process_queue_lock);
    TaskQueue* old = g_process_queue;
    g_process_queue = q;
    ReleaseSRWLockExclusive(&g_process_queue_lock);
    if (old)
        queue_release(old);
}

// --- XThread -------------------------------------------------------------------------------------
// "Time sensitive" threads must not call runtime APIs that may block. The flag is only used for
// the title's own diagnostics, so we track it and never complain.

static HRESULT WINAPI XThreadSetTimeSensitive(RuntimeSelf, bool value) {
    g_time_sensitive.set(value ? 1 : 0);
    return S_OK;
}

static bool WINAPI XThreadIsTimeSensitive(RuntimeSelf) {
    return g_time_sensitive.get() != 0;
}
static HRESULT WINAPI XThreadVerifyNotTimeSensitive(RuntimeSelf) {
    return S_OK;
}
static void WINAPI XThreadAssertNotTimeSensitive(RuntimeSelf) {}

// --- interface table -----------------------------------------------------------------------------

static const GUID k_XTaskQueue_iids[] = {
    {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}},
};

static const RuntimeMethod k_XTaskQueue_methods[] = {
    {3, "XAsyncGetStatus", (void*)&XAsyncGetStatus},
    {4, "XAsyncGetResultSize", (void*)&XAsyncGetResultSize},
    {5, "XAsyncCancel", (void*)&XAsyncCancel},
    {6, "XAsyncRun", (void*)&XAsyncRun},
    {7, "XAsyncBegin", (void*)&XAsyncBegin},
    {9, "XAsyncSchedule", (void*)&XAsyncSchedule},
    {10, "XAsyncComplete", (void*)&XAsyncComplete},
    {11, "XAsyncGetResult", (void*)&XAsyncGetResult},
    {12, "XTaskQueueCreate", (void*)&XTaskQueueCreate},
    {13, "XTaskQueueCreateComposite", (void*)&XTaskQueueCreateComposite},
    {14, "XTaskQueueGetPort", (void*)&XTaskQueueGetPort},
    {15, "XTaskQueueDuplicateHandle", (void*)&XTaskQueueDuplicateHandle},
    {16, "XTaskQueueDispatch", (void*)&XTaskQueueDispatch},
    {17, "XTaskQueueCloseHandle", (void*)&XTaskQueueCloseHandle},
    {18, "XTaskQueueSubmitCallback", (void*)&XTaskQueueSubmitCallback},
    {19, "XTaskQueueSubmitDelayedCallback", (void*)&XTaskQueueSubmitDelayedCallback},
    {20, "XTaskQueueRegisterWaiter", nullptr},
    {21, "XTaskQueueUnregisterWaiter", nullptr},
    {22, "XTaskQueueTerminate", (void*)&XTaskQueueTerminate},
    {23, "XTaskQueueRegisterMonitor", (void*)&XTaskQueueRegisterMonitor},
    {24, "XTaskQueueUnregisterMonitor", (void*)&XTaskQueueUnregisterMonitor},
    {25, "XTaskQueueGetCurrentProcessTaskQueue", (void*)&XTaskQueueGetCurrentProcessTaskQueue},
    {26, "XTaskQueueSetCurrentProcessTaskQueue", (void*)&XTaskQueueSetCurrentProcessTaskQueue},
    {27, "XThreadSetTimeSensitive", (void*)&XThreadSetTimeSensitive},
    {28, "XThreadVerifyNotTimeSensitive", (void*)&XThreadVerifyNotTimeSensitive},
    {29, "XThreadAssertNotTimeSensitive", (void*)&XThreadAssertNotTimeSensitive},
    {30, "XThreadIsTimeSensitive", (void*)&XThreadIsTimeSensitive},
};

extern const RuntimeApi g_runtime_api_XTaskQueue =
    RUNTIME_API("XTaskQueue", &k_XTaskQueue_iids[0], k_XTaskQueue_iids, k_XTaskQueue_methods);
