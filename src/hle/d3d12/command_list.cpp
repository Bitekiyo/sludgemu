// Console ID3D12GraphicsCommandList (direct, compute, copy and bundle lists share the class):
// lifetime, barriers and pipeline state.
//
// The console class can write command packets inline, straight into its command buffer, when the
// put buffer's limit pointers leave room. We keep every limit null, so the title's inlined methods
// always take their fallback path through the function table, and the whole recording arrives
// here at API level, where it's recorded onto a desktop command list as it comes.

#include "hle/d3d12/command_list.h"

#include <stddef.h>

#include "common/log.h"
#include "hle/d3d12/barriers.h"
#include "hle/d3d12/console_iids.h"
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

// --- shared helpers ----------------------------------------------------------------------------------

bool list_gpu_descriptor(const ConsoleCommandList* list, u64 handle, D3D12_GPU_DESCRIPTOR_HANDLE* out) {
    for (u32 i = 0; i < ARRAY_COUNT(list->heaps); i++) {
        const ConsoleDescriptorHeap* heap = list->heaps[i];
        if (!heap || handle < (u64)heap->memory)
            continue;
        u64 index = (handle - (u64)heap->memory) / heap->stride;
        if (index < heap->desktop_count) {
            out->ptr = heap->visible_gpu_start + index * heap->desktop_stride;
            return true;
        }
    }
    LOG_W_ONCE("d3d12", "descriptor table 0x%llx is not in a bound descriptor heap", handle);
    return false;
}

bool list_cpu_descriptor(u64 handle, u32 count, D3D12_CPU_DESCRIPTOR_HANDLE* out) {
    DescriptorSlot slot;
    if (!console_descriptor_slots(handle, count, &slot))
        return false;
    *out = slot.cpu;
    return true;
}

ID3D12Resource* list_resource(void* console_resource, u64* offset) {
    *offset = 0;
    if (!console_resource)
        return nullptr;
    return console_resource_desktop((ConsoleResource*)console_resource, offset);
}

// Root descriptors and buffer views: title GPU addresses become desktop ones. Address 0 stays 0.
static u64 desktop_address(u64 address, u64 size) {
    u64 out = 0;
    if (address && !console_translate_gpu_address(address, size ? size : 1, &out))
        LOG_W_ONCE("d3d12", "GPU address 0x%llx is outside GPU-visible memory", address);
    return out;
}

// --- lifetime ----------------------------------------------------------------------------------------

static void reset_state(ConsoleCommandList* self) {
    self->recording = true;
    self->pipeline_bound = false;
    self->heaps[0] = nullptr;
    self->heaps[1] = nullptr;
}

static ID3D12PipelineState* desktop_pso(void* console_pso) {
    if (console_pso)
        LOG_W_ONCE("d3d12", "command list: pipeline states are not translated yet, initial state ignored");
    return nullptr;
}

static D3D12_COMMAND_LIST_TYPE WINAPI list_get_type(ConsoleCommandList* self) {
    return self->type;
}

static HRESULT WINAPI list_close(ConsoleCommandList* self) {
    self->recording = false;
    HRESULT hr = self->desktop->Close();
    if (FAILED(hr))
        LOG_E("d3d12", "command list Close failed (0x%08lx)", (unsigned long)hr);
    return hr;
}

static HRESULT WINAPI list_reset_x(ConsoleCommandList* self, ConsoleCommandAllocator* allocator,
                                   void* initial_state, UINT flags) {
    if (!allocator)
        return E_INVALIDARG;
    // Lists are created recording. Titles may reset one straight away, which desktop D3D12 only
    // allows once it's closed.
    if (self->recording)
        self->desktop->Close();
    HRESULT hr = self->desktop->Reset(allocator->desktop, desktop_pso(initial_state));
    if (FAILED(hr)) {
        LOG_E("d3d12", "command list Reset failed (0x%08lx)", (unsigned long)hr);
        return hr;
    }
    reset_state(self);
    (void)flags; // sync and state-minimizing hints for the console command processor
    return S_OK;
}

static HRESULT WINAPI list_reset(ConsoleCommandList* self, ConsoleCommandAllocator* allocator,
                                 void* initial_state) {
    return list_reset_x(self, allocator, initial_state, 0);
}

static void WINAPI list_clear_state(ConsoleCommandList* self, void* pso) {
    self->desktop->ClearState(desktop_pso(pso));
    self->pipeline_bound = false;
    self->heaps[0] = nullptr;
    self->heaps[1] = nullptr;
}

// --- barriers and synchronization --------------------------------------------------------------------

static void WINAPI list_resource_barrier(ConsoleCommandList* self, UINT count,
                                         const D3D12_RESOURCE_BARRIER* barriers) {
    console_record_resource_barriers(self->desktop, count, barriers);
}

// The flags only control how the console splits the barrier into cache and pipeline operations.
static void WINAPI list_resource_barrier_x(ConsoleCommandList* self, UINT count,
                                           const D3D12_RESOURCE_BARRIER* barriers, UINT, void* actions) {
    if (actions)
        LOG_W_ONCE("d3d12", "ResourceBarrierX: summary actions are not reported");
    console_record_resource_barriers(self->desktop, count, barriers);
}

static void WINAPI list_barrier(ConsoleCommandList* self, UINT count, const D3D12_BARRIER_GROUP* groups) {
    console_record_barrier_groups(self->desktop, count, groups);
}

// Console cache flushes and pipeline waits; on desktop, a barrier covering everything.
static void WINAPI list_sync_x(ConsoleCommandList* self, UINT, UINT, UINT64, SIZE_T) {
    console_record_full_barrier(self->desktop);
}

static void WINAPI list_flush_pipeline_x(ConsoleCommandList* self, UINT, UINT64, SIZE_T) {
    console_record_full_barrier(self->desktop);
}

// --- pipeline state and root arguments ---------------------------------------------------------------

static void WINAPI list_set_pipeline_state(ConsoleCommandList* self, void* pso) {
    if (pso)
        LOG_W_ONCE("d3d12", "SetPipelineState: pipeline states are not translated yet; draws and dispatches "
                            "are skipped");
    self->pipeline_bound = false;
}

static void WINAPI list_set_descriptor_heaps(ConsoleCommandList* self, UINT count,
                                             ConsoleDescriptorHeap** heaps) {
    ID3D12DescriptorHeap* desktop[2];
    u32 n = 0;
    self->heaps[0] = nullptr;
    self->heaps[1] = nullptr;
    for (UINT i = 0; i < count && n < 2; i++) {
        ConsoleDescriptorHeap* heap = heaps[i];
        if (!heap || !heap->visible)
            continue;
        self->heaps[heap->desktop_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0] = heap;
        desktop[n++] = heap->visible;
    }
    self->desktop->SetDescriptorHeaps(n, desktop);
}

static ID3D12RootSignature* desktop_root_signature(void* root_signature) {
    return root_signature ? ((ConsoleRootSignature*)root_signature)->desktop : nullptr;
}

static void WINAPI list_set_compute_root_signature(ConsoleCommandList* self, void* root_signature) {
    self->desktop->SetComputeRootSignature(desktop_root_signature(root_signature));
}

static void WINAPI list_set_graphics_root_signature(ConsoleCommandList* self, void* root_signature) {
    self->desktop->SetGraphicsRootSignature(desktop_root_signature(root_signature));
}

static void WINAPI list_set_compute_root_descriptor_table(ConsoleCommandList* self, UINT index,
                                                          D3D12_GPU_DESCRIPTOR_HANDLE table) {
    D3D12_GPU_DESCRIPTOR_HANDLE d;
    if (list_gpu_descriptor(self, table.ptr, &d))
        self->desktop->SetComputeRootDescriptorTable(index, d);
}

static void WINAPI list_set_graphics_root_descriptor_table(ConsoleCommandList* self, UINT index,
                                                           D3D12_GPU_DESCRIPTOR_HANDLE table) {
    D3D12_GPU_DESCRIPTOR_HANDLE d;
    if (list_gpu_descriptor(self, table.ptr, &d))
        self->desktop->SetGraphicsRootDescriptorTable(index, d);
}

static void WINAPI list_set_compute_root_32bit_constant(ConsoleCommandList* self, UINT index, UINT value,
                                                        UINT offset) {
    self->desktop->SetComputeRoot32BitConstant(index, value, offset);
}

static void WINAPI list_set_graphics_root_32bit_constant(ConsoleCommandList* self, UINT index, UINT value,
                                                         UINT offset) {
    self->desktop->SetGraphicsRoot32BitConstant(index, value, offset);
}

static void WINAPI list_set_compute_root_32bit_constants(ConsoleCommandList* self, UINT index, UINT count,
                                                         const void* values, UINT offset) {
    self->desktop->SetComputeRoot32BitConstants(index, count, values, offset);
}

static void WINAPI list_set_graphics_root_32bit_constants(ConsoleCommandList* self, UINT index, UINT count,
                                                          const void* values, UINT offset) {
    self->desktop->SetGraphicsRoot32BitConstants(index, count, values, offset);
}

static void WINAPI list_set_compute_root_cbv(ConsoleCommandList* self, UINT index, UINT64 address) {
    self->desktop->SetComputeRootConstantBufferView(index, desktop_address(address, 0));
}

static void WINAPI list_set_graphics_root_cbv(ConsoleCommandList* self, UINT index, UINT64 address) {
    self->desktop->SetGraphicsRootConstantBufferView(index, desktop_address(address, 0));
}

static void WINAPI list_set_compute_root_srv(ConsoleCommandList* self, UINT index, UINT64 address) {
    self->desktop->SetComputeRootShaderResourceView(index, desktop_address(address, 0));
}

static void WINAPI list_set_graphics_root_srv(ConsoleCommandList* self, UINT index, UINT64 address) {
    self->desktop->SetGraphicsRootShaderResourceView(index, desktop_address(address, 0));
}

static void WINAPI list_set_compute_root_uav(ConsoleCommandList* self, UINT index, UINT64 address) {
    self->desktop->SetComputeRootUnorderedAccessView(index, desktop_address(address, 0));
}

static void WINAPI list_set_graphics_root_uav(ConsoleCommandList* self, UINT index, UINT64 address) {
    self->desktop->SetGraphicsRootUnorderedAccessView(index, desktop_address(address, 0));
}

// --- input assembly, rasterizer and output merger ----------------------------------------------------

static void WINAPI list_ia_set_primitive_topology(ConsoleCommandList* self,
                                                  D3D12_PRIMITIVE_TOPOLOGY topology) {
    self->desktop->IASetPrimitiveTopology(topology);
}

static void WINAPI list_ia_set_index_buffer(ConsoleCommandList* self, const D3D12_INDEX_BUFFER_VIEW* view) {
    if (!view) {
        self->desktop->IASetIndexBuffer(nullptr);
        return;
    }
    D3D12_INDEX_BUFFER_VIEW v = *view;
    v.BufferLocation = desktop_address(v.BufferLocation, v.SizeInBytes);
    self->desktop->IASetIndexBuffer(&v);
}

static void WINAPI list_ia_set_vertex_buffers(ConsoleCommandList* self, UINT start, UINT count,
                                              const D3D12_VERTEX_BUFFER_VIEW* views) {
    D3D12_VERTEX_BUFFER_VIEW v[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    if (!views || start >= ARRAY_COUNT(v)) {
        self->desktop->IASetVertexBuffers(start, count, nullptr);
        return;
    }
    if (count > ARRAY_COUNT(v) - start)
        count = ARRAY_COUNT(v) - start;
    for (UINT i = 0; i < count; i++) {
        v[i] = views[i];
        v[i].BufferLocation = desktop_address(v[i].BufferLocation, v[i].SizeInBytes);
    }
    self->desktop->IASetVertexBuffers(start, count, v);
}

static void WINAPI list_rs_set_viewports(ConsoleCommandList* self, UINT count,
                                         const D3D12_VIEWPORT* viewports) {
    self->desktop->RSSetViewports(count, viewports);
}

static void WINAPI list_rs_set_scissor_rects(ConsoleCommandList* self, UINT count, const D3D12_RECT* rects) {
    self->desktop->RSSetScissorRects(count, rects);
}

static void WINAPI list_om_set_blend_factor(ConsoleCommandList* self, const FLOAT factor[4]) {
    self->desktop->OMSetBlendFactor(factor);
}

static void WINAPI list_om_set_stencil_ref(ConsoleCommandList* self, UINT ref) {
    self->desktop->OMSetStencilRef(ref);
}

static void WINAPI list_om_set_front_and_back_stencil_ref(ConsoleCommandList* self, UINT front, UINT back) {
    self->desktop->OMSetFrontAndBackStencilRef(front, back);
}

static void WINAPI list_om_set_depth_bounds(ConsoleCommandList* self, FLOAT min, FLOAT max) {
    self->desktop->OMSetDepthBounds(min, max);
}

static void WINAPI list_om_set_render_targets(ConsoleCommandList* self, UINT count,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE* targets, BOOL single_range,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    D3D12_CPU_DESCRIPTOR_HANDLE rt[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
    D3D12_CPU_DESCRIPTOR_HANDLE ds;
    if (count > ARRAY_COUNT(rt))
        count = ARRAY_COUNT(rt);
    if (count && single_range) {
        if (!list_cpu_descriptor(targets[0].ptr, count, &rt[0]))
            return;
    } else {
        for (UINT i = 0; i < count; i++)
            if (!list_cpu_descriptor(targets[i].ptr, 1, &rt[i]))
                return;
    }
    if (depth && !list_cpu_descriptor(depth->ptr, 1, &ds))
        return;
    self->desktop->OMSetRenderTargets(count, count ? rt : nullptr, single_range, depth ? &ds : nullptr);
}

// --- debug markers -----------------------------------------------------------------------------------

// PIX event data passes through as is, so graphics debuggers show the title's own markers.
static void WINAPI list_set_marker(ConsoleCommandList* self, UINT metadata, const void* data, UINT size) {
    self->desktop->SetMarker(metadata, data, size);
}

static void WINAPI list_begin_event(ConsoleCommandList* self, UINT metadata, const void* data, UINT size) {
    self->desktop->BeginEvent(metadata, data, size);
}

static void WINAPI list_end_event(ConsoleCommandList* self) {
    self->desktop->EndEvent();
}

static const ConsoleEntry k_list_impl[] = {
    CONSOLE_ENTRY(8, list_get_type),
    CONSOLE_ENTRY(9, list_close),
    CONSOLE_ENTRY(10, list_reset),
    CONSOLE_ENTRY(11, list_clear_state),
    CONSOLE_ENTRY(20, list_ia_set_primitive_topology),
    CONSOLE_ENTRY(21, list_rs_set_viewports),
    CONSOLE_ENTRY(22, list_rs_set_scissor_rects),
    CONSOLE_ENTRY(23, list_om_set_blend_factor),
    CONSOLE_ENTRY(24, list_om_set_stencil_ref),
    CONSOLE_ENTRY(25, list_set_pipeline_state),
    CONSOLE_ENTRY(26, list_resource_barrier),
    CONSOLE_ENTRY(32, list_set_descriptor_heaps),
    CONSOLE_ENTRY(33, list_set_compute_root_signature),
    CONSOLE_ENTRY(34, list_set_graphics_root_signature),
    CONSOLE_ENTRY(35, list_set_compute_root_descriptor_table),
    CONSOLE_ENTRY(36, list_set_graphics_root_descriptor_table),
    CONSOLE_ENTRY(37, list_set_compute_root_32bit_constant),
    CONSOLE_ENTRY(38, list_set_graphics_root_32bit_constant),
    CONSOLE_ENTRY(39, list_set_compute_root_32bit_constants),
    CONSOLE_ENTRY(40, list_set_graphics_root_32bit_constants),
    CONSOLE_ENTRY(41, list_set_compute_root_cbv),
    CONSOLE_ENTRY(42, list_set_graphics_root_cbv),
    CONSOLE_ENTRY(43, list_set_compute_root_srv),
    CONSOLE_ENTRY(44, list_set_graphics_root_srv),
    CONSOLE_ENTRY(45, list_set_compute_root_uav),
    CONSOLE_ENTRY(46, list_set_graphics_root_uav),
    CONSOLE_ENTRY(47, list_ia_set_index_buffer),
    CONSOLE_ENTRY(48, list_ia_set_vertex_buffers),
    CONSOLE_ENTRY(50, list_om_set_render_targets),
    CONSOLE_ENTRY(56, list_set_marker),
    CONSOLE_ENTRY(57, list_begin_event),
    CONSOLE_ENTRY(58, list_end_event),
    CONSOLE_ENTRY(69, list_flush_pipeline_x),
    CONSOLE_ENTRY(70, list_om_set_depth_bounds),
    CONSOLE_ENTRY(109, list_om_set_depth_bounds),
    CONSOLE_ENTRY(131, list_sync_x),
    CONSOLE_ENTRY(132, list_sync_x),
    CONSOLE_ENTRY(139, list_reset_x),
    CONSOLE_ENTRY(140, list_resource_barrier_x),
    CONSOLE_ENTRY(146, list_barrier),
    CONSOLE_ENTRY(147, list_om_set_front_and_back_stencil_ref),
};

void console_command_list_init() {
    // Both sources' entries in one table.
    static ConsoleEntry s_entries[ARRAY_COUNT(k_list_impl) + 64];
    u32 n = 0;
    for (u32 i = 0; i < ARRAY_COUNT(k_list_impl); i++)
        s_entries[n++] = k_list_impl[i];
    SLUDGE_ASSERT(n + k_list_work_entry_count <= ARRAY_COUNT(s_entries));
    for (u32 i = 0; i < k_list_work_entry_count; i++)
        s_entries[n++] = k_list_work_entries[i];
    g_list_table = console_build_table("ID3D12GraphicsCommandList", k_command_list_entries,
                                       ARRAY_COUNT(k_command_list_entries), s_entries, n);
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

    ID3D12GraphicsCommandList8* desktop = nullptr;
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
    reset_state(list);
    return console_return(list, iid, out);
}
