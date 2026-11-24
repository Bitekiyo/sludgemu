// Console ID3D12CommandQueue.

#include <stddef.h>

#include "common/log.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"

static_assert(offsetof(ConsoleCommandQueue, type) == 0x10, "command queue layout");

static const GUID* const k_queue_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                           &__uuidof(ID3D12Pageable), &__uuidof(ID3D12CommandQueue)};

static const ConsoleClass k_queue_class = {"ID3D12CommandQueue", k_queue_iids, ARRAY_COUNT(k_queue_iids),
                                           nullptr};

static void* const* g_queue_table = nullptr;

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
    return console_return(queue, iid, out);
}
