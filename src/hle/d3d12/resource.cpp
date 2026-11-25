// Console ID3D12Resource / ID3D12Resource1 / ID3D12Resource2.

#include "common/log.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"
#include "gpu/console_layout.h"
#include "hle/xmem/xmem.h"

static const GUID* const k_resource_iids[] = {&__uuidof(ID3D12Object),    &__uuidof(ID3D12DeviceChild),
                                              &__uuidof(ID3D12Pageable),  &__uuidof(ID3D12Resource),
                                              &__uuidof(ID3D12Resource1), &__uuidof(ID3D12Resource2)};

static void resource_destroy(void* object);

static const ConsoleClass k_resource_class = {"ID3D12Resource", k_resource_iids, ARRAY_COUNT(k_resource_iids),
                                              resource_destroy};

static void* const* g_resource_table = nullptr;

bool console_translate_gpu_address(u64 address, u64 size, u64* desktop_address) {
    GpuAddress a;
    if (!gpu_translate(address, size, &a))
        return false;
    *desktop_address = a.desktop_address;
    return true;
}

// --- desktop backing ---------------------------------------------------------------------------------

// Console resource flags that desktop D3D12 knows; the rest describe console hardware (compression,
// tiling preferences) or are implied on desktop.
static const u32 k_desktop_resource_flags =
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
    D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;

// Console layouts are hardware swizzle modes; desktop picks its own.
static D3D12_TEXTURE_LAYOUT desktop_texture_layout(D3D12_TEXTURE_LAYOUT layout) {
    return layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR ? layout : D3D12_TEXTURE_LAYOUT_UNKNOWN;
}

// The desktop description of a console texture.
static D3D12_RESOURCE_DESC1 desktop_texture_desc(const D3D12_RESOURCE_DESC1& console) {
    D3D12_RESOURCE_DESC1 desc = console;
    desc.Alignment = 0;
    desc.Layout = desktop_texture_layout(desc.Layout);
    desc.Flags = (D3D12_RESOURCE_FLAGS)(desc.Flags & k_desktop_resource_flags);
    return desc;
}

static ID3D12Resource* create_texture(ConsoleResource* r) {
    D3D12_RESOURCE_DESC1 desc = desktop_texture_desc(r->desc);
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    bool clear_allowed =
        desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    const D3D12_CLEAR_VALUE* clear = r->has_clear_value && clear_allowed ? &r->clear_value : nullptr;

    ID3D12Resource* texture = nullptr;
    HRESULT hr = gpu_device()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_BARRIER_LAYOUT_COMMON, clear, nullptr, 0,
                                                        nullptr, IID_PPV_ARGS(&texture));
    if (FAILED(hr)) {
        LOG_E("d3d12",
              "cannot create the desktop texture for 0x%llx (dim %d, %llux%ux%u, mips %u, format %d, "
              "flags 0x%x): 0x%08lx",
              r->gpu_address, r->desc.Dimension, r->desc.Width, r->desc.Height, r->desc.DepthOrArraySize,
              r->desc.MipLevels, r->desc.Format, r->desc.Flags, (unsigned long)hr);
        return nullptr;
    }
    return texture;
}

// Textures take what their console layout takes: titles place them in their own memory and stream
// data into them in that layout.
D3D12_RESOURCE_ALLOCATION_INFO console_resource_allocation_info(const D3D12_RESOURCE_DESC1& desc) {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        u64 alignment = desc.Alignment ? desc.Alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        return {ALIGN_UP(desc.Width, alignment), alignment};
    }
    ConsoleTextureDesc t = {};
    t.dimension = desc.Dimension;
    t.width = desc.Width;
    t.height = desc.Height;
    t.depth_or_array_size = desc.DepthOrArraySize;
    t.mip_levels = desc.MipLevels;
    t.format = desc.Format;
    t.sample_count = desc.SampleDesc.Count;
    t.layout = desc.Layout;
    t.flags = desc.Flags;
    ConsoleTextureLayout layout;
    if (!console_texture_layout(t, &layout)) {
        LOG_E("d3d12",
              "no console layout for texture dim %d, %llux%ux%u, mips %u, format %d, layout 0x%x, flags 0x%x",
              desc.Dimension, desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels, desc.Format,
              desc.Layout, desc.Flags);
        return {UINT64_MAX, 0};
    }
    return {layout.size, layout.base_alignment};
}

ID3D12Resource* console_resource_desktop(ConsoleResource* r, u64* offset) {
    if (r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        GpuAddress a;
        if (!gpu_translate(r->gpu_address, r->desc.Width, &a))
            return nullptr;
        *offset = a.offset;
        return a.buffer;
    }
    *offset = 0;
    AcquireSRWLockExclusive(&r->lock);
    if (!r->texture)
        r->texture = create_texture(r);
    ID3D12Resource* texture = r->texture;
    ReleaseSRWLockExclusive(&r->lock);
    return texture;
}

static void resource_destroy(void* object) {
    ConsoleResource* r = (ConsoleResource*)object;
    gpu_retire(r->texture);
    if (r->owned_memory)
        xmem_free(r->owned_memory, r->owned_attributes);
}

// --- entries -----------------------------------------------------------------------------------------

static D3D12_RESOURCE_DESC desc0_from_desc1(const D3D12_RESOURCE_DESC1& d) {
    D3D12_RESOURCE_DESC r;
    r.Dimension = d.Dimension;
    r.Alignment = d.Alignment;
    r.Width = d.Width;
    r.Height = d.Height;
    r.DepthOrArraySize = d.DepthOrArraySize;
    r.MipLevels = d.MipLevels;
    r.Format = d.Format;
    r.SampleDesc = d.SampleDesc;
    r.Layout = d.Layout;
    r.Flags = d.Flags;
    return r;
}

// Field by field: the version-1 description's extra member starts inside the padding at the end of
// the version-0 one, so the two can't be copied as blocks.
D3D12_RESOURCE_DESC1 console_desc1_from_desc(const D3D12_RESOURCE_DESC& d) {
    D3D12_RESOURCE_DESC1 r = {};
    r.Dimension = d.Dimension;
    r.Alignment = d.Alignment;
    r.Width = d.Width;
    r.Height = d.Height;
    r.DepthOrArraySize = d.DepthOrArraySize;
    r.MipLevels = d.MipLevels;
    r.Format = d.Format;
    r.SampleDesc = d.SampleDesc;
    r.Layout = d.Layout;
    r.Flags = d.Flags;
    return r;
}

// The title's memory is the resource's memory, mapped for the CPU at the GPU address.
static HRESULT WINAPI resource_map(ConsoleResource* self, UINT subresource, const D3D12_RANGE*, void** data) {
    if (subresource != 0) {
        LOG_W("d3d12", "ID3D12Resource::Map: subresource %u not supported", subresource);
        return E_INVALIDARG;
    }
    if (data)
        *data = (void*)self->gpu_address;
    return S_OK;
}

static void WINAPI resource_unmap(ConsoleResource*, UINT, const D3D12_RANGE*) {}

static void WINAPI resource_get_desc(ConsoleResource* self, D3D12_RESOURCE_DESC* out) {
    *out = desc0_from_desc1(self->desc);
}

static D3D12_GPU_VIRTUAL_ADDRESS WINAPI resource_get_gpu_virtual_address(ConsoleResource* self) {
    return self->gpu_address;
}

static HRESULT WINAPI resource_get_heap_properties(ConsoleResource* self, D3D12_HEAP_PROPERTIES* properties,
                                                   D3D12_HEAP_FLAGS* flags) {
    if (properties)
        *properties = self->heap_properties;
    if (flags)
        *flags = self->heap_flags;
    return S_OK;
}

// Struct returns go through a hidden pointer, received before the object.
static D3D12_RESOURCE_DESC* WINAPI resource_get_placement_desc_x(D3D12_RESOURCE_DESC* ret,
                                                                 ConsoleResource* self) {
    *ret = desc0_from_desc1(self->desc);
    return ret;
}

static D3D12_RESOURCE_DESC1* WINAPI resource_get_desc1(D3D12_RESOURCE_DESC1* ret, ConsoleResource* self) {
    *ret = self->desc;
    return ret;
}

static const ConsoleEntry k_resource_impl[] = {
    CONSOLE_ENTRY(8, resource_map),
    CONSOLE_ENTRY(9, resource_unmap),
    CONSOLE_ENTRY(10, resource_get_desc),
    CONSOLE_ENTRY(11, resource_get_gpu_virtual_address),
    CONSOLE_ENTRY(14, resource_get_heap_properties),
    CONSOLE_ENTRY(15, resource_get_placement_desc_x),
    CONSOLE_ENTRY(19, resource_get_desc1),
};

void console_resource_init() {
    g_resource_table =
        console_build_table("ID3D12Resource", k_resource_entries, ARRAY_COUNT(k_resource_entries),
                            k_resource_impl, ARRAY_COUNT(k_resource_impl));
}

// Creates the object for a resource at `gpu_address`; the caller hands it to the title.
static ConsoleResource* new_resource(u64 gpu_address, const D3D12_RESOURCE_DESC1* desc,
                                     const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS heap_flags,
                                     D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE* clear) {
    ConsoleResource* r = (ConsoleResource*)console_alloc(sizeof(ConsoleResource), &k_resource_class,
                                                         g_resource_table, nullptr);
    r->desc = *desc;
    r->gpu_address = gpu_address;
    r->heap_properties = heap;
    r->heap_flags = heap_flags;
    r->initial_layout = initial_layout;
    if (clear) {
        r->has_clear_value = true;
        r->clear_value = *clear;
    }
    InitializeSRWLock(&r->lock);
    return r;
}

HRESULT console_create_committed_resource(ConsoleDevice* device, const D3D12_HEAP_PROPERTIES* heap,
                                          D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1* desc,
                                          D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE* clear,
                                          REFIID iid, void** out) {
    if (!heap || !desc)
        return E_INVALIDARG;
    D3D12_RESOURCE_ALLOCATION_INFO info = console_resource_allocation_info(*desc);
    if (info.SizeInBytes == UINT64_MAX)
        return E_INVALIDARG;
    // CPU caching of the memory follows what the heap is for.
    bool write_combined = heap->Type == D3D12_HEAP_TYPE_UPLOAD ||
                          (heap->Type == D3D12_HEAP_TYPE_CUSTOM &&
                           heap->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE);
    u32 object =
        (heap_flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY) ? XMEM_D3D_OBJECT_PRESENTABLE : XMEM_D3D_OBJECT_RESOURCE;
    u32 memory_type = write_combined ? XMEM_TYPE_GRAPHICS_WRITECOMBINE : XMEM_TYPE_GRAPHICS_CACHEABLE;
    u64 attributes = xmem_d3d_attributes(object, memory_type, info.SizeInBytes, info.Alignment, false);
    void* memory = xmem_alloc(info.SizeInBytes, attributes);
    if (!memory) {
        LOG_E("d3d12", "no memory for a committed resource of %llu KiB", info.SizeInBytes >> 10);
        return E_OUTOFMEMORY;
    }
    LOG_D("d3d12",
          "committed resource at %p: heap %d, heap flags 0x%x, dim %d, %llux%ux%u, mips %u, format %d, "
          "flags 0x%x",
          memory, heap->Type, heap_flags, desc->Dimension, desc->Width, desc->Height, desc->DepthOrArraySize,
          desc->MipLevels, desc->Format, desc->Flags);
    ConsoleResource* r = new_resource((u64)memory, desc, *heap, heap_flags, initial_layout, clear);
    r->owned_memory = memory;
    r->owned_attributes = attributes;
    return console_return(r, iid, out);
}

HRESULT console_create_placed_resource(ConsoleDevice* device, u64 gpu_address,
                                       const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT initial_layout,
                                       const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out) {
    if (!desc || !gpu_address)
        return E_INVALIDARG;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && desc->Width == 0)
        return E_INVALIDARG;
    LOG_D("d3d12",
          "placed resource at 0x%llx: dim %d, %llux%ux%u, mips %u, format %d, layout 0x%x, flags 0x%x",
          gpu_address, desc->Dimension, desc->Width, desc->Height, desc->DepthOrArraySize, desc->MipLevels,
          desc->Format, desc->Layout, desc->Flags);

    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                  D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    ConsoleResource* r = new_resource(gpu_address, desc, heap, D3D12_HEAP_FLAG_NONE, initial_layout, clear);
    return console_return(r, iid, out);
}
