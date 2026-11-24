// Console ID3D12Device on top of the desktop device. One object and one function table serve
// every device version, as one vtable does on desktop.

#include "hle/d3d12/device.h"

#include <stddef.h>

#include "common/log.h"
#include "gpu/gpu.h"
#include "hle/d3d12/console_iids.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"

static_assert(offsetof(ConsoleDevice, null_constant_buffer_address) == 0x10, "device layout");
static_assert(offsetof(ConsoleDevice, out_of_line_flags) == 0x18, "device layout");
static_assert(offsetof(ConsoleDevice, desktop) == 88, "device state follows the public members");

static const GUID* const k_device_iids[] = {
    &__uuidof(ID3D12Object),   &__uuidof(ID3D12Device),  &__uuidof(ID3D12Device1), &__uuidof(ID3D12Device2),
    &__uuidof(ID3D12Device3),  &__uuidof(ID3D12Device4), &__uuidof(ID3D12Device5), &__uuidof(ID3D12Device6),
    &__uuidof(ID3D12Device7),  &IID_ConsoleDevice8,      &__uuidof(ID3D12Device9), &__uuidof(ID3D12Device10),
    &__uuidof(ID3D12Device11),
};

static const ConsoleClass k_device_class = {"ID3D12Device", k_device_iids, ARRAY_COUNT(k_device_iids),
                                            nullptr};

// Created once; our own reference keeps it alive for the lifetime of the process.
static ConsoleDevice* g_device = nullptr;

static UINT WINAPI device_get_node_count(ConsoleDevice*) {
    return 1;
}

static HRESULT WINAPI device_create_command_queue(ConsoleDevice* self, const D3D12_COMMAND_QUEUE_DESC* desc,
                                                  REFIID iid, void** out) {
    return console_create_command_queue(self, desc, iid, out);
}

// Console queues address hardware engines and pipes directly and carry a priority level; the
// desktop scheduler decides those, so only the type and the desktop flags carry over.
static HRESULT WINAPI device_create_command_queue_x(ConsoleDevice* self, const ConsoleQueueDescX* desc,
                                                    REFIID iid, void** out) {
    if (!desc)
        return E_INVALIDARG;
    LOG_D("d3d12", "CreateCommandQueueX(type %d, flags 0x%x, engine/pipe %u, queue %u)", desc->type,
          desc->flags, desc->engine_or_pipe_index, desc->queue_index);
    D3D12_COMMAND_QUEUE_DESC d = {};
    d.Type = desc->type;
    d.Flags = (D3D12_COMMAND_QUEUE_FLAGS)(desc->flags & 0x00FFFFFF);
    return console_create_command_queue(self, &d, iid, out);
}

static HRESULT WINAPI device_create_command_allocator(ConsoleDevice* self, D3D12_COMMAND_LIST_TYPE type,
                                                      REFIID iid, void** out) {
    return console_create_command_allocator(self, type, iid, out);
}

static HRESULT WINAPI device_create_command_list_x(ConsoleDevice* self, const ConsoleCommandListDesc* desc,
                                                   ConsoleCommandAllocator* allocator, void* initial_state,
                                                   REFIID iid, void** out) {
    return console_create_command_list(self, desc, allocator, initial_state, iid, out);
}

static HRESULT WINAPI device_create_fence(ConsoleDevice* self, UINT64 initial_value, D3D12_FENCE_FLAGS flags,
                                          REFIID iid, void** out) {
    return console_create_fence(self, initial_value, flags, iid, out);
}

// Struct return through a hidden pointer: a C entry receives it before the object.
static D3D12_HEAP_PROPERTIES* WINAPI device_get_custom_heap_properties(D3D12_HEAP_PROPERTIES* ret,
                                                                       ConsoleDevice* self, UINT node_mask,
                                                                       D3D12_HEAP_TYPE type) {
    *ret = self->desktop->GetCustomHeapProperties(node_mask, type);
    return ret;
}

static const ConsoleEntry k_device_impl[] = {
    CONSOLE_ENTRY(7, device_get_node_count),
    CONSOLE_ENTRY(8, device_create_command_queue),
    CONSOLE_ENTRY(9, device_create_command_allocator),
    CONSOLE_ENTRY(26, device_get_custom_heap_properties),
    CONSOLE_ENTRY(36, device_create_fence),
    CONSOLE_ENTRY(48, device_create_command_queue_x),
    CONSOLE_ENTRY(73, device_create_command_list_x),
};

HRESULT console_create_device(void* adapter, const ConsoleDeviceParams* params, REFIID iid, void** out) {
    if (!params)
        return E_INVALIDARG;
    LOG_I("d3d12", "D3D12XboxCreateDevice(version 0x%x, debug flags 0x%x, create flags 0x%x, queue ring %u)",
          params->version, params->process_debug_flags, params->create_flags,
          params->graphics_queue_ring_bytes);

    if (!g_device) {
        ID3D12Device10* desktop = gpu_device();
        if (!desktop)
            return DXGI_ERROR_UNSUPPORTED;
        void* const* table =
            console_build_table("ID3D12Device", k_device_entries, ARRAY_COUNT(k_device_entries),
                                k_device_impl, ARRAY_COUNT(k_device_impl));
        desktop->AddRef(); // one reference for the console object, one held by the gpu module
        g_device = (ConsoleDevice*)console_alloc(sizeof(ConsoleDevice), &k_device_class, table, desktop);
        g_device->out_of_line_flags[0] = 1;
        g_device->desktop = desktop;
        console_set_device(g_device);
        console_command_queue_init();
        console_fence_init();
        console_command_allocator_init();
        console_command_list_init();
    }
    if (!out)
        return S_FALSE; // capability check only
    return console_query(g_device, iid, out);
}
