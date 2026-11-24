// Console ID3D12Fence / ID3D12Fence1.

#include "common/log.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"

static const GUID* const k_fence_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                           &__uuidof(ID3D12Pageable), &__uuidof(ID3D12Fence),
                                           &__uuidof(ID3D12Fence1)};

static const ConsoleClass k_fence_class = {"ID3D12Fence", k_fence_iids, ARRAY_COUNT(k_fence_iids), nullptr};

static void* const* g_fence_table = nullptr;

static UINT64 WINAPI fence_get_completed_value(ConsoleFence* self) {
    return self->desktop->GetCompletedValue();
}

static HRESULT WINAPI fence_set_event_on_completion(ConsoleFence* self, UINT64 value, HANDLE event) {
    return self->desktop->SetEventOnCompletion(value, event);
}

static HRESULT WINAPI fence_signal(ConsoleFence* self, UINT64 value) {
    return self->desktop->Signal(value);
}

static D3D12_FENCE_FLAGS WINAPI fence_get_creation_flags(ConsoleFence* self) {
    return self->desktop->GetCreationFlags();
}

static const ConsoleEntry k_fence_impl[] = {
    CONSOLE_ENTRY(8, fence_get_completed_value),
    CONSOLE_ENTRY(9, fence_set_event_on_completion),
    CONSOLE_ENTRY(10, fence_signal),
    CONSOLE_ENTRY(11, fence_get_creation_flags),
};

void console_fence_init() {
    g_fence_table = console_build_table("ID3D12Fence", k_fence_entries, ARRAY_COUNT(k_fence_entries),
                                        k_fence_impl, ARRAY_COUNT(k_fence_impl));
}

HRESULT console_create_fence(ConsoleDevice* device, UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID iid,
                             void** out) {
    const D3D12_FENCE_FLAGS desktop_flags =
        D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER | D3D12_FENCE_FLAG_NON_MONITORED;
    if (flags & ~desktop_flags)
        LOG_W("d3d12", "CreateFence: ignoring console fence flags 0x%x", flags & ~desktop_flags);

    ID3D12Fence1* desktop = nullptr;
    HRESULT hr = device->desktop->CreateFence(initial_value, flags & desktop_flags, IID_PPV_ARGS(&desktop));
    if (FAILED(hr)) {
        LOG_E("d3d12", "CreateFence(%llu, 0x%x) failed (0x%08lx)", initial_value, flags, (unsigned long)hr);
        return hr;
    }
    ConsoleFence* fence =
        (ConsoleFence*)console_alloc(sizeof(ConsoleFence), &k_fence_class, g_fence_table, desktop);
    fence->desktop = desktop;
    return console_return(fence, iid, out);
}
