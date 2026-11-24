// Console ID3D12CommandAllocator.

#include "common/log.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"

static const GUID* const k_allocator_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                               &__uuidof(ID3D12Pageable), &__uuidof(ID3D12CommandAllocator)};

static const ConsoleClass k_allocator_class = {"ID3D12CommandAllocator", k_allocator_iids,
                                               ARRAY_COUNT(k_allocator_iids), nullptr};

static void* const* g_allocator_table = nullptr;

static HRESULT WINAPI allocator_reset(ConsoleCommandAllocator* self) {
    return self->desktop->Reset();
}

static const ConsoleEntry k_allocator_impl[] = {
    CONSOLE_ENTRY(8, allocator_reset),
};

void console_command_allocator_init() {
    g_allocator_table = console_build_table("ID3D12CommandAllocator", k_command_allocator_entries,
                                            ARRAY_COUNT(k_command_allocator_entries), k_allocator_impl,
                                            ARRAY_COUNT(k_allocator_impl));
}

HRESULT console_create_command_allocator(ConsoleDevice* device, D3D12_COMMAND_LIST_TYPE type, REFIID iid,
                                         void** out) {
    ID3D12CommandAllocator* desktop = nullptr;
    HRESULT hr = device->desktop->CreateCommandAllocator(type, IID_PPV_ARGS(&desktop));
    if (FAILED(hr)) {
        LOG_E("d3d12", "CreateCommandAllocator(type %d) failed (0x%08lx)", type, (unsigned long)hr);
        return hr;
    }
    ConsoleCommandAllocator* allocator = (ConsoleCommandAllocator*)console_alloc(
        sizeof(ConsoleCommandAllocator), &k_allocator_class, g_allocator_table, desktop);
    allocator->type = type;
    allocator->desktop = desktop;
    return console_return(allocator, iid, out);
}
