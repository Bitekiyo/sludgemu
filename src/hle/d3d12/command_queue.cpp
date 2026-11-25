// Console ID3D12CommandQueue.

#include <stddef.h>

#include "common/log.h"
#include "gpu/gpu.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"

static_assert(offsetof(ConsoleCommandQueue, type) == 0x10, "command queue layout");

static const GUID* const k_queue_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                           &__uuidof(ID3D12Pageable), &__uuidof(ID3D12CommandQueue)};

static void queue_destroy(void* object);

static const ConsoleClass k_queue_class = {"ID3D12CommandQueue", k_queue_iids, ARRAY_COUNT(k_queue_iids),
                                           queue_destroy};

static void* const* g_queue_table = nullptr;

static void queue_destroy(void* object) {
    gpu_queue_unregister(((ConsoleCommandQueue*)object)->slot);
}

// Lists submitted per desktop call; longer submissions are split, which keeps their order.
#define SUBMIT_BATCH 64

static void WINAPI queue_execute_command_lists(ConsoleCommandQueue* self, UINT count,
                                               ConsoleCommandList* const* lists) {
    ID3D12CommandList* desktop[SUBMIT_BATCH];
    u32 n = 0;
    for (UINT i = 0; i < count; i++) {
        if (lists[i])
            desktop[n++] = lists[i]->desktop;
        if (n == SUBMIT_BATCH || (i + 1 == count && n)) {
            self->desktop->ExecuteCommandLists(n, desktop);
            n = 0;
        }
    }
    gpu_queue_submitted(self->slot);
}

static HRESULT WINAPI queue_signal(ConsoleCommandQueue* self, ConsoleFence* fence, UINT64 value) {
    return fence ? self->desktop->Signal(fence->desktop, value) : E_INVALIDARG;
}

// The flush flags pick which console caches to write back first; desktop signals see all
// earlier writes.
static HRESULT WINAPI queue_signal_x(ConsoleCommandQueue* self, ConsoleFence* fence, UINT64 value, UINT) {
    return queue_signal(self, fence, value);
}

static HRESULT WINAPI queue_wait(ConsoleCommandQueue* self, ConsoleFence* fence, UINT64 value) {
    return fence ? self->desktop->Wait(fence->desktop, value) : E_INVALIDARG;
}

static void WINAPI queue_set_marker(ConsoleCommandQueue* self, UINT metadata, const void* data, UINT size) {
    self->desktop->SetMarker(metadata, data, size);
}

static void WINAPI queue_begin_event(ConsoleCommandQueue* self, UINT metadata, const void* data, UINT size) {
    self->desktop->BeginEvent(metadata, data, size);
}

static void WINAPI queue_end_event(ConsoleCommandQueue* self) {
    self->desktop->EndEvent();
}

// Work is submitted as it arrives, so there is nothing to kick off.
static void WINAPI queue_kickoff_x(ConsoleCommandQueue*) {}

static HRESULT WINAPI queue_get_timestamp_frequency(ConsoleCommandQueue* self, UINT64* frequency) {
    return self->desktop->GetTimestampFrequency(frequency);
}

static HRESULT WINAPI queue_get_clock_calibration(ConsoleCommandQueue* self, UINT64* gpu, UINT64* cpu) {
    return self->desktop->GetClockCalibration(gpu, cpu);
}

static void WINAPI queue_get_desc(ConsoleCommandQueue* self, D3D12_COMMAND_QUEUE_DESC* desc) {
    *desc = self->desktop->GetDesc();
}

static const ConsoleEntry k_queue_impl[] = {
    CONSOLE_ENTRY(10, queue_execute_command_lists),
    CONSOLE_ENTRY(11, queue_set_marker),
    CONSOLE_ENTRY(12, queue_begin_event),
    CONSOLE_ENTRY(13, queue_end_event),
    CONSOLE_ENTRY(16, queue_signal),
    CONSOLE_ENTRY(17, queue_wait),
    CONSOLE_ENTRY(25, queue_signal_x),
    CONSOLE_ENTRY(27, queue_kickoff_x),
    CONSOLE_ENTRY(18, queue_get_timestamp_frequency),
    CONSOLE_ENTRY(19, queue_get_clock_calibration),
    CONSOLE_ENTRY(28, queue_get_desc),
};

void console_command_queue_init() {
    g_queue_table =
        console_build_table("ID3D12CommandQueue", k_command_queue_entries,
                            ARRAY_COUNT(k_command_queue_entries), k_queue_impl, ARRAY_COUNT(k_queue_impl));
}

HRESULT console_create_command_queue(ConsoleDevice* device, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID iid,
                                     void** out) {
    if (!desc)
        return E_INVALIDARG;

    // Console-only queue flags have no desktop meaning.
    D3D12_COMMAND_QUEUE_DESC d = *desc;
    d.Flags &= D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;
    ID3D12CommandQueue* desktop = nullptr;
    HRESULT hr = device->desktop->CreateCommandQueue(&d, IID_PPV_ARGS(&desktop));
    LOG_I("d3d12", "CreateCommandQueue(type %d, priority %d, flags 0x%x) -> 0x%08lx", desc->Type,
          desc->Priority, desc->Flags, (unsigned long)hr);
    if (FAILED(hr))
        return hr;

    ConsoleCommandQueue* queue = (ConsoleCommandQueue*)console_alloc(sizeof(ConsoleCommandQueue),
                                                                     &k_queue_class, g_queue_table, desktop);
    queue->type = desc->Type;
    queue->desktop = desktop;
    queue->slot = gpu_queue_register(desktop);
    return console_return(queue, iid, out);
}
