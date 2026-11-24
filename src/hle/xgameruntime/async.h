#pragma once

#include <windows.h>

#include "common/types.h"
#include "hle/xgameruntime/runtime_api.h"

// XAsync: the runtime's asynchronous operation model. The title owns an XAsyncBlock; a provider
// (runtime code or the title's own) implements the operation in steps driven by XAsyncOp.

struct TaskQueue;
struct XAsyncBlock;

typedef void CALLBACK XAsyncCompletionRoutine(XAsyncBlock* block);
typedef HRESULT CALLBACK XAsyncWork(XAsyncBlock* block);

struct XAsyncBlock {
    TaskQueue* queue; // nullptr: process task queue
    void* context;
    XAsyncCompletionRoutine* callback;
    u8 internal[32]; // ours; see async.cpp
};

enum XAsyncOp : u32 {
    XASYNC_OP_BEGIN = 0,
    XASYNC_OP_DO_WORK = 1,
    XASYNC_OP_GET_RESULT = 2,
    XASYNC_OP_CANCEL = 3,
    XASYNC_OP_CLEANUP = 4,
};

struct XAsyncProviderData {
    XAsyncBlock* async;
    usize buffer_size;
    void* buffer;
    void* context;
};

typedef HRESULT CALLBACK XAsyncProvider(XAsyncOp op, const XAsyncProviderData* data);

HRESULT WINAPI XAsyncGetStatus(RuntimeSelf, XAsyncBlock* block, bool wait);
HRESULT WINAPI XAsyncGetResultSize(RuntimeSelf, XAsyncBlock* block, usize* size);
void WINAPI XAsyncCancel(RuntimeSelf, XAsyncBlock* block);
HRESULT WINAPI XAsyncRun(RuntimeSelf, XAsyncBlock* block, XAsyncWork* work);
HRESULT WINAPI XAsyncBegin(RuntimeSelf, XAsyncBlock* block, void* context, const void* identity,
                           const char* identity_name, XAsyncProvider* provider);
HRESULT WINAPI XAsyncSchedule(RuntimeSelf, XAsyncBlock* block, u32 delay_ms);
void WINAPI XAsyncComplete(RuntimeSelf, XAsyncBlock* block, HRESULT result, usize required_size);
HRESULT WINAPI XAsyncGetResult(RuntimeSelf, XAsyncBlock* block, const void* identity, usize buffer_size,
                               void* buffer, usize* used);
