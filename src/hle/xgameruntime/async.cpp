// XAsync on top of task queues.
//
// The title owns the XAsyncBlock and may free or reuse it as soon as it sees the operation
// complete (by polling XAsyncGetStatus, or in its completion callback). So the block is never
// touched after its final status is published, except as a WakeByAddressAll key, which doesn't
// dereference it. Waiters in XAsyncGetStatus block on the status word inside the block, which
// they own.
//
// Per-operation state is heap allocated and reference counted: one reference for the operation
// itself (dropped at cleanup) and one per queued work or completion callback. Cleanup (the
// provider's XASYNC_OP_CLEANUP) happens at completion for operations without a result payload,
// otherwise when the title retrieves the result.

#include "hle/xgameruntime/async.h"

#include <stdlib.h>

#include "common/log.h"
#include "hle/xgameruntime/task_queue.h"

#define ASYNC_SIGNATURE 0x43535941u // "AYSC"

struct AsyncState;

// Our view of XAsyncBlock::internal.
struct AsyncInternal {
    AsyncState* state;    // nullptr once cleaned up
    volatile long status; // E_PENDING until the operation completes
    u32 signature;
    u64 reserved[2];
};
static_assert(sizeof(AsyncInternal) == sizeof(((XAsyncBlock*)nullptr)->internal), "internal size");

struct AsyncState {
    volatile long refs;
    volatile long completing;
    volatile long cleaned_up;
    XAsyncBlock* block;
    XAsyncProvider* provider;
    void* context;
    const void* identity;
    const char* identity_name;
    TaskQueue* queue;
    XAsyncCompletionRoutine* completion; // captured at completion, before the status is published
    usize result_size;
};

// Guards AsyncInternal::state against concurrent cleanup in XAsyncCancel, the only API that may
// race with completion on another thread.
static SRWLOCK g_state_lock = SRWLOCK_INIT;

static AsyncInternal* internal_of(XAsyncBlock* block) {
    return (AsyncInternal*)block->internal;
}

static void state_add_ref(AsyncState* s) {
    InterlockedIncrement(&s->refs);
}

static void state_release(AsyncState* s) {
    if (InterlockedDecrement(&s->refs) == 0) {
        task_queue_release(s->queue);
        free(s);
    }
}

static void call_provider(AsyncState* s, XAsyncOp op, usize buffer_size, void* buffer, HRESULT* result) {
    XAsyncProviderData data;
    data.async = s->block;
    data.buffer_size = buffer_size;
    data.buffer = buffer;
    data.context = s->context;
    HRESULT hr = s->provider(op, &data);
    if (result)
        *result = hr;
}

// Runs the provider's cleanup and drops the operation's own reference. Called at most once.
static void cleanup(AsyncState* s) {
    if (InterlockedExchange(&s->cleaned_up, 1))
        return;
    call_provider(s, XASYNC_OP_CLEANUP, 0, nullptr, nullptr);
    AcquireSRWLockExclusive(&g_state_lock);
    internal_of(s->block)->state = nullptr;
    ReleaseSRWLockExclusive(&g_state_lock);
    state_release(s);
}

static void CALLBACK completion_callback(void* context, bool) {
    AsyncState* s = (AsyncState*)context;
    s->completion(s->block);
    state_release(s);
}

static void complete(AsyncState* s, HRESULT result, usize required_size) {
    if (result == E_PENDING || InterlockedExchange(&s->completing, 1))
        return;
    state_add_ref(s);

    XAsyncBlock* block = s->block;
    AsyncInternal* in = internal_of(block);
    s->result_size = SUCCEEDED(result) ? required_size : 0;
    s->completion = block->callback;
    if (s->result_size == 0)
        cleanup(s);

    // Publishing the status hands the block back to the title.
    InterlockedExchange(&in->status, result);
    WakeByAddressAll((void*)&in->status);

    if (s->completion) {
        state_add_ref(s);
        if (FAILED(task_queue_submit(s->queue, TASK_PORT_COMPLETION, 0, s, completion_callback)))
            completion_callback(s, true); // queue terminated: the callback still has to run
    }
    state_release(s);
}

static void CALLBACK do_work_callback(void* context, bool canceled) {
    AsyncState* s = (AsyncState*)context;
    if (!s->completing) {
        if (canceled) {
            complete(s, E_ABORT, 0);
        } else {
            HRESULT hr;
            call_provider(s, XASYNC_OP_DO_WORK, 0, nullptr, &hr);
            if (hr != E_PENDING)
                complete(s, hr, 0);
        }
    }
    state_release(s);
}

// The state of a pending operation, or nullptr. Only for callers that own the block while the
// operation is pending (the provider, or the title before completion).
static AsyncState* pending_state(XAsyncBlock* block) {
    if (!block)
        return nullptr;
    AsyncInternal* in = internal_of(block);
    if (in->signature != ASYNC_SIGNATURE || in->status != E_PENDING)
        return nullptr;
    return in->state;
}

// --- API -----------------------------------------------------------------------------------------

HRESULT WINAPI XAsyncBegin(RuntimeSelf, XAsyncBlock* block, void* context, const void* identity,
                           const char* identity_name, XAsyncProvider* provider) {
    if (!block || !provider)
        return E_INVALIDARG;
    AsyncInternal* in = internal_of(block);
    if (in->signature == ASYNC_SIGNATURE && in->status == E_PENDING && in->state) {
        LOG_E("xgr", "XAsyncBegin(%s): async block %p is still in use", identity_name ? identity_name : "?",
              block);
        return E_INVALIDARG;
    }
    TaskQueue* queue = task_queue_acquire(block->queue);
    if (!queue)
        return E_INVALIDARG;

    AsyncState* s = (AsyncState*)calloc(1, sizeof(AsyncState));
    s->refs = 1;
    s->block = block;
    s->provider = provider;
    s->context = context;
    s->identity = identity;
    s->identity_name = identity_name;
    s->queue = queue;

    in->state = s;
    in->status = E_PENDING;
    in->signature = ASYNC_SIGNATURE;

    HRESULT hr;
    call_provider(s, XASYNC_OP_BEGIN, 0, nullptr, &hr);
    if (FAILED(hr)) {
        // The operation never started: no cleanup, no completion callback.
        in->state = nullptr;
        in->status = hr;
        state_release(s);
        return hr;
    }
    return S_OK;
}

HRESULT WINAPI XAsyncSchedule(RuntimeSelf, XAsyncBlock* block, u32 delay_ms) {
    AsyncState* s = pending_state(block);
    if (!s)
        return E_INVALIDARG;
    state_add_ref(s);
    HRESULT hr = task_queue_submit(s->queue, TASK_PORT_WORK, delay_ms, s, do_work_callback);
    if (FAILED(hr))
        state_release(s);
    return hr;
}

void WINAPI XAsyncComplete(RuntimeSelf, XAsyncBlock* block, HRESULT result, usize required_size) {
    AsyncState* s = pending_state(block);
    if (s)
        complete(s, result, required_size);
}

HRESULT WINAPI XAsyncGetStatus(RuntimeSelf, XAsyncBlock* block, bool wait) {
    if (!block)
        return E_INVALIDARG;
    AsyncInternal* in = internal_of(block);
    if (in->signature != ASYNC_SIGNATURE)
        return E_INVALIDARG;
    long status = in->status;
    while (wait && status == E_PENDING) {
        WaitOnAddress(&in->status, &status, sizeof(status), INFINITE);
        status = in->status;
    }
    return status;
}

HRESULT WINAPI XAsyncGetResultSize(RuntimeSelf, XAsyncBlock* block, usize* size) {
    if (!block || !size)
        return E_INVALIDARG;
    AsyncInternal* in = internal_of(block);
    if (in->signature != ASYNC_SIGNATURE)
        return E_INVALIDARG;
    HRESULT status = in->status;
    *size = 0;
    if (SUCCEEDED(status)) {
        if (AsyncState* s = in->state)
            *size = s->result_size;
    }
    return status;
}

HRESULT WINAPI XAsyncGetResult(RuntimeSelf, XAsyncBlock* block, const void* identity, usize buffer_size,
                               void* buffer, usize* used) {
    if (used)
        *used = 0;
    if (!block)
        return E_INVALIDARG;
    AsyncInternal* in = internal_of(block);
    if (in->signature != ASYNC_SIGNATURE)
        return E_INVALIDARG;
    HRESULT status = in->status;
    if (status == E_PENDING || FAILED(status))
        return status;

    AsyncState* s = in->state;
    if (!s)
        return status; // no payload, or already retrieved
    if (identity && s->identity != identity) {
        LOG_E("xgr", "XAsyncGetResult: block %p belongs to %s", block,
              s->identity_name ? s->identity_name : "?");
        return E_INVALIDARG;
    }
    if (buffer_size < s->result_size)
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

    HRESULT hr;
    call_provider(s, XASYNC_OP_GET_RESULT, buffer_size, buffer, &hr);
    if (SUCCEEDED(hr) && used)
        *used = s->result_size;
    cleanup(s);
    return FAILED(hr) ? hr : status;
}

void WINAPI XAsyncCancel(RuntimeSelf, XAsyncBlock* block) {
    if (!block)
        return;
    AsyncInternal* in = internal_of(block);
    AcquireSRWLockShared(&g_state_lock);
    AsyncState* s = (in->signature == ASYNC_SIGNATURE && in->status == E_PENDING) ? in->state : nullptr;
    if (s)
        state_add_ref(s);
    ReleaseSRWLockShared(&g_state_lock);
    if (!s)
        return;
    if (!s->completing)
        call_provider(s, XASYNC_OP_CANCEL, 0, nullptr, nullptr);
    complete(s, E_ABORT, 0); // no-op if the provider completed it already
    state_release(s);
}

static HRESULT CALLBACK run_provider(XAsyncOp op, const XAsyncProviderData* data) {
    switch (op) {
    case XASYNC_OP_BEGIN:
        return XAsyncSchedule(nullptr, data->async, 0);
    case XASYNC_OP_DO_WORK:
        return ((XAsyncWork*)data->context)(data->async);
    default:
        return S_OK;
    }
}

HRESULT WINAPI XAsyncRun(RuntimeSelf, XAsyncBlock* block, XAsyncWork* work) {
    if (!work)
        return E_INVALIDARG;
    return XAsyncBegin(nullptr, block, (void*)work, (void*)&XAsyncRun, "XAsyncRun", run_provider);
}
