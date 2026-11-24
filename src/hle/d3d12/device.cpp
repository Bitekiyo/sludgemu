// Console ID3D12Device on top of the desktop device.

#include "hle/d3d12/device.h"

#include <stddef.h>

#include "common/log.h"
#include "gpu/gpu.h"
#include "hle/d3d12/d3d12_tables.h"

static_assert(offsetof(ConsoleDevice, null_constant_buffer_address) == 0x10, "device layout");
static_assert(offsetof(ConsoleDevice, out_of_line_flags) == 0x18, "device layout");
static_assert(offsetof(ConsoleDevice, desktop) == 88, "device private state follows the public members");

static ConsoleDevice g_device;

// Interface IDs are shared with desktop D3D12.
static const GUID* const k_device_iids[] = {
    &__uuidof(IUnknown),       &__uuidof(ID3D12Object),   &__uuidof(ID3D12Device),  &__uuidof(ID3D12Device1),
    &__uuidof(ID3D12Device2),  &__uuidof(ID3D12Device3),  &__uuidof(ID3D12Device4), &__uuidof(ID3D12Device5),
    &__uuidof(ID3D12Device6),  &__uuidof(ID3D12Device7),  &__uuidof(ID3D12Device8), &__uuidof(ID3D12Device9),
    &__uuidof(ID3D12Device10), &__uuidof(ID3D12Device11),
};

// --- IUnknown / ID3D12Object ---------------------------------------------------------------------

static HRESULT WINAPI device_query_interface(ConsoleDevice* self, REFIID iid, void** out) {
    for (u32 i = 0; i < ARRAY_COUNT(k_device_iids); i++) {
        if (iid == *k_device_iids[i]) {
            InterlockedIncrement((volatile long*)&self->header.ref_count);
            *out = self;
            return S_OK;
        }
    }
    *out = nullptr;
    LOG_W("d3d12", "ID3D12Device::QueryInterface: unknown interface {%08lx-...}", iid.Data1);
    return E_NOINTERFACE;
}

// The device lives as long as the process.
static ULONG WINAPI device_add_ref(ConsoleDevice* self) {
    return (ULONG)InterlockedIncrement((volatile long*)&self->header.ref_count);
}
static ULONG WINAPI device_release(ConsoleDevice* self) {
    return (ULONG)InterlockedDecrement((volatile long*)&self->header.ref_count);
}

static HRESULT WINAPI device_get_private_data(ConsoleDevice* self, REFGUID guid, UINT* size, void* data) {
    return self->desktop->GetPrivateData(guid, size, data);
}
static HRESULT WINAPI device_set_private_data(ConsoleDevice* self, REFGUID guid, UINT size,
                                              const void* data) {
    return self->desktop->SetPrivateData(guid, size, data);
}
static HRESULT WINAPI device_set_name(ConsoleDevice* self, LPCWSTR name) {
    return self->desktop->SetName(name);
}

// --- ID3D12Device --------------------------------------------------------------------------------

static UINT WINAPI device_get_node_count(ConsoleDevice*) {
    return 1;
}

static const ConsoleEntry k_device_impl[] = {
    CONSOLE_ENTRY(0, device_query_interface),  CONSOLE_ENTRY(1, device_add_ref),
    CONSOLE_ENTRY(2, device_release),          CONSOLE_ENTRY(3, device_get_private_data),
    CONSOLE_ENTRY(4, device_set_private_data), CONSOLE_ENTRY(6, device_set_name),
    CONSOLE_ENTRY(7, device_get_node_count),
};

HRESULT console_create_device(void* adapter, const ConsoleDeviceParams* params, REFIID iid, void** out) {
    if (!params)
        return E_INVALIDARG;
    LOG_I("d3d12", "D3D12XboxCreateDevice(version 0x%x, debug flags 0x%x, create flags 0x%x, queue ring %u)",
          params->version, params->process_debug_flags, params->create_flags,
          params->graphics_queue_ring_bytes);

    if (!g_device.desktop) {
        ID3D12Device10* desktop = gpu_device();
        if (!desktop)
            return DXGI_ERROR_UNSUPPORTED;
        g_device.header.function_table =
            console_build_table("ID3D12Device", k_device_entries, ARRAY_COUNT(k_device_entries),
                                k_device_impl, ARRAY_COUNT(k_device_impl));
        g_device.header.ref_count = 0;
        g_device.out_of_line_flags[0] = 1;
        g_device.desktop = desktop;
    }
    if (!out)
        return S_FALSE; // capability check only
    return device_query_interface(&g_device, iid, out);
}
