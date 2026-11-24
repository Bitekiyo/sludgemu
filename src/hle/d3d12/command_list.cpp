// Console ID3D12GraphicsCommandList (direct, compute, copy and bundle lists share the class).
//
// The console class can write command packets inline, straight into its command buffer, when the
// put buffer's limit pointers leave room. We keep every limit null, so the title's inlined methods
// always take their fallback path through the function table, and the whole recording arrives
// here at API level, where it's replayed onto a desktop command list.

#include <stddef.h>

#include "common/log.h"
#include "hle/d3d12/console_iids.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"

static_assert(offsetof(ConsoleCommandList, type) == 0x10, "command list layout");
static_assert(offsetof(ConsoleCommandList, putter) == 0x18, "command list layout");
static_assert(sizeof(ConsolePutBuffer) == 296, "put buffer layout");
static_assert(offsetof(ConsoleCommandList, cpu_state) == 0x140, "command list layout");
static_assert(offsetof(ConsoleCommandList, graphics_root_packet_header) == 0x144, "command list layout");
static_assert(offsetof(ConsoleCommandList, compute_root_packet_header) == 0x344, "command list layout");
static_assert(offsetof(ConsoleCommandList, graphics_pso_shadow) == 0x544, "command list layout");
static_assert(offsetof(ConsoleCommandList, desktop) == 0x568,
              "command list state follows the public members");

static const GUID* const k_list_iids[] = {
    &__uuidof(ID3D12Object),
    &__uuidof(ID3D12DeviceChild),
    &__uuidof(ID3D12CommandList),
    &__uuidof(ID3D12GraphicsCommandList),
    &__uuidof(ID3D12GraphicsCommandList1),
    &__uuidof(ID3D12GraphicsCommandList2),
    &__uuidof(ID3D12GraphicsCommandList3),
    &__uuidof(ID3D12GraphicsCommandList4),
    &IID_ConsoleGraphicsCommandList5,
    &IID_ConsoleGraphicsCommandList6,
    &__uuidof(ID3D12GraphicsCommandList7),
    &__uuidof(ID3D12GraphicsCommandList8),
};

static const ConsoleClass k_list_class = {"ID3D12GraphicsCommandList", k_list_iids, ARRAY_COUNT(k_list_iids),
                                          nullptr};

static void* const* g_list_table = nullptr;

static ID3D12PipelineState* desktop_pso(void* console_pso) {
    if (console_pso)
        LOG_W("d3d12", "command list: initial pipeline state not supported yet, ignored");
    return nullptr;
}

static D3D12_COMMAND_LIST_TYPE WINAPI list_get_type(ConsoleCommandList* self) {
    return self->type;
}

static HRESULT WINAPI list_close(ConsoleCommandList* self) {
    return self->desktop->Close();
}

static HRESULT WINAPI list_reset(ConsoleCommandList* self, ConsoleCommandAllocator* allocator,
                                 void* initial_state) {
    return self->desktop->Reset(allocator->desktop, desktop_pso(initial_state));
}

static const ConsoleEntry k_list_impl[] = {
    CONSOLE_ENTRY(8, list_get_type),
    CONSOLE_ENTRY(9, list_close),
    CONSOLE_ENTRY(10, list_reset),
};

void console_command_list_init() {
    g_list_table =
        console_build_table("ID3D12GraphicsCommandList", k_command_list_entries,
                            ARRAY_COUNT(k_command_list_entries), k_list_impl, ARRAY_COUNT(k_list_impl));
}

HRESULT console_create_command_list(ConsoleDevice* device, const ConsoleCommandListDesc* desc,
                                    ConsoleCommandAllocator* allocator, void* initial_state, REFIID iid,
                                    void** out) {
    if (!desc || !allocator)
        return E_INVALIDARG;
    if (desc->type > D3D12_COMMAND_LIST_TYPE_COPY) {
        LOG_E("d3d12", "CreateCommandListX: list type %d not supported yet", desc->type);
        return E_NOTIMPL;
    }

    ID3D12GraphicsCommandList7* desktop = nullptr;
    HRESULT hr = device->desktop->CreateCommandList(0, desc->type, allocator->desktop,
                                                    desktop_pso(initial_state), IID_PPV_ARGS(&desktop));
    LOG_D("d3d12", "CreateCommandListX(type %d, flags 0x%x, barrier batch %u) -> 0x%08lx", desc->type,
          desc->flags, desc->resource_barrier_batch_size, (unsigned long)hr);
    if (FAILED(hr))
        return hr;

    ConsoleCommandList* list =
        (ConsoleCommandList*)console_alloc(sizeof(ConsoleCommandList), &k_list_class, g_list_table, desktop);
    list->type = desc->type;
    list->desktop = desktop;
    return console_return(list, iid, out);
}
