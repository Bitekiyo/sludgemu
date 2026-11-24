#pragma once

#include <windows.h>

#include "common/types.h"

// Task queues: the runtime's callback scheduling primitive. A queue has two ports (work and
// completion), each with a dispatch mode. Async operations (async.cpp) run their work on the work
// port and their completion callback on the completion port.

enum TaskDispatchMode : u32 {
    TASK_DISPATCH_MANUAL = 0,                 // runs only when the title calls XTaskQueueDispatch
    TASK_DISPATCH_THREAD_POOL = 1,            // runs on the thread pool, concurrently
    TASK_DISPATCH_SERIALIZED_THREAD_POOL = 2, // runs on the thread pool, one at a time
    TASK_DISPATCH_IMMEDIATE = 3,              // runs on the submitting thread
};

enum TaskPortId : u32 {
    TASK_PORT_WORK = 0,
    TASK_PORT_COMPLETION = 1,
};

struct TaskQueue; // XTaskQueueHandle

typedef void CALLBACK TaskCallback(void* context, bool canceled);

void task_queue_init();

// Resolves a title-provided queue handle: nullptr means the process task queue. Returns a new
// reference (release with task_queue_release), or nullptr if the handle is invalid or no process
// queue is set.
TaskQueue* task_queue_acquire(TaskQueue* handle);
void task_queue_release(TaskQueue* queue);

HRESULT task_queue_submit(TaskQueue* queue, TaskPortId port, u32 delay_ms, void* context,
                          TaskCallback* callback);
