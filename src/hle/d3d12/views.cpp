// Descriptor writes: console view creation and descriptor copies, mirrored into desktop
// descriptors (see ConsoleDescriptorHeap).

#include <stddef.h>
#include <string.h>

#include "common/log.h"
#include "gpu/formats.h"
#include "gpu/gpu_window.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/views.h"

// Console slots get a marker naming the desktop descriptor they mirror, which makes a title's own
// raw copies of descriptor memory recognizable when debugging.
struct SlotMarker {
    u32 magic;
    u32 type;
    u64 desktop_cpu;
};
#define SLOT_MARKER_MAGIC 0x47444C53u // "SLDG"

static void finish_write(const DescriptorSlot& slot) {
    SlotMarker marker = {SLOT_MARKER_MAGIC, (u32)slot.type, slot.cpu.ptr};
    memcpy(slot.console, &marker, sizeof(marker)); // every console descriptor is at least 16 bytes
    console_descriptor_publish(slot, 1);
}

// Bytes per element of the formats typed buffer views use; 0 for others.
static u32 buffer_format_bytes(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 16;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 12;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
        return 8;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return 4;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
        return 2;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
        return 1;
    default:
        return 0;
    }
}

static u32 buffer_element_bytes(DXGI_FORMAT format, u32 structure_stride, bool raw) {
    if (raw)
        return 4;
    if (structure_stride)
        return structure_stride;
    return buffer_format_bytes(format);
}

// Where the elements a buffer view covers live on desktop: the window holding those bytes and the
// index of the view's first element in it. nullptr (logged) if they aren't GPU-visible as one
// block, or the window doesn't start on an element boundary, which desktop views can't express.
static ID3D12Resource* buffer_view_target(u64 buffer_address, u64 first_element, u32 num_elements,
                                          u32 element_bytes, u64* desktop_first_element) {
    if (element_bytes == 0) {
        LOG_E("d3d12", "buffer view of 0x%llx with an unsupported format", buffer_address);
        return nullptr;
    }
    GpuAddress a;
    if (!gpu_translate(buffer_address + first_element * element_bytes, (u64)num_elements * element_bytes, &a))
        return nullptr;
    if (a.offset % element_bytes != 0) {
        LOG_E("d3d12", "buffer view of 0x%llx: its window starts mid-element (%u-byte elements)",
              buffer_address, element_bytes);
        return nullptr;
    }
    *desktop_first_element = a.offset / element_bytes;
    return a.buffer;
}

// --- texture views -----------------------------------------------------------------------------------
//
// Texture views go through their full description: desktop formats can differ from the console's
// (depth textures are typeless, console-only formats have stand-ins), so what a null description
// means is spelled out, and a view desktop can't make becomes a null view of the same kind rather
// than leaving the slot stale.

// The desktop format of a texture view the title made in console format `view` (UNKNOWN: the
// texture's own); false if desktop can't show the texture that way.
static bool texture_view_format(const ConsoleResource* r, DXGI_FORMAT view, bool shader_read,
                                DXGI_FORMAT* out) {
    if (!format_view_compatible(r->desc.Format, view)) {
        LOG_W_ONCE("d3d12",
                   "texture of format %d viewed as format %d: no desktop equivalent, writing null views",
                   r->desc.Format, view);
        return false;
    }
    DXGI_FORMAT f = view != DXGI_FORMAT_UNKNOWN ? view : r->desc.Format;
    *out = shader_read ? format_desktop_shader_read(f) : format_desktop(f);
    return *out != DXGI_FORMAT_UNKNOWN;
}

// The texture's desktop counterpart; nullptr (the view becomes null) for a null resource or one with
// no desktop texture.
static ID3D12Resource* texture_view_target(const ConsoleResource* r) {
    u64 offset;
    return r ? console_resource_desktop((ConsoleResource*)r, &offset) : nullptr;
}

// Null views still need a format of their kind; reads through them return zero whatever it is.
#define NULL_VIEW_FORMAT       DXGI_FORMAT_R8G8B8A8_UNORM
#define NULL_DEPTH_VIEW_FORMAT DXGI_FORMAT_D32_FLOAT

static bool is_array(const D3D12_RESOURCE_DESC1& r) {
    return r.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D && r.DepthOrArraySize > 1;
}

// Whole-texture descriptions, in the texture's own format (UNKNOWN until texture_view_format).
static D3D12_SHADER_RESOURCE_VIEW_DESC whole_texture_srv(const D3D12_RESOURCE_DESC1& r) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    const UINT all = (UINT)-1;
    if (r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        d.ViewDimension = is_array(r) ? D3D12_SRV_DIMENSION_TEXTURE1DARRAY : D3D12_SRV_DIMENSION_TEXTURE1D;
        d.Texture1DArray.MipLevels = all;
        d.Texture1DArray.ArraySize = r.DepthOrArraySize;
    } else if (r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        d.Texture3D.MipLevels = all;
    } else if (r.SampleDesc.Count > 1) {
        d.ViewDimension =
            is_array(r) ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D12_SRV_DIMENSION_TEXTURE2DMS;
        d.Texture2DMSArray.ArraySize = r.DepthOrArraySize;
    } else {
        d.ViewDimension = is_array(r) ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Texture2DArray.MipLevels = all;
        d.Texture2DArray.ArraySize = r.DepthOrArraySize;
    }
    return d;
}

static D3D12_UNORDERED_ACCESS_VIEW_DESC whole_texture_uav(const D3D12_RESOURCE_DESC1& r) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    if (r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        d.ViewDimension = is_array(r) ? D3D12_UAV_DIMENSION_TEXTURE1DARRAY : D3D12_UAV_DIMENSION_TEXTURE1D;
        d.Texture1DArray.ArraySize = r.DepthOrArraySize;
    } else if (r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        d.Texture3D.WSize = (UINT)-1;
    } else {
        d.ViewDimension = is_array(r) ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
        d.Texture2DArray.ArraySize = r.DepthOrArraySize;
    }
    return d;
}

static D3D12_RENDER_TARGET_VIEW_DESC whole_texture_rtv(const D3D12_RESOURCE_DESC1& r) {
    D3D12_RENDER_TARGET_VIEW_DESC d = {};
    if (r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        d.ViewDimension = is_array(r) ? D3D12_RTV_DIMENSION_TEXTURE1DARRAY : D3D12_RTV_DIMENSION_TEXTURE1D;
        d.Texture1DArray.ArraySize = r.DepthOrArraySize;
    } else if (r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        d.Texture3D.WSize = (UINT)-1;
    } else if (r.SampleDesc.Count > 1) {
        d.ViewDimension =
            is_array(r) ? D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D12_RTV_DIMENSION_TEXTURE2DMS;
        d.Texture2DMSArray.ArraySize = r.DepthOrArraySize;
    } else {
        d.ViewDimension = is_array(r) ? D3D12_RTV_DIMENSION_TEXTURE2DARRAY : D3D12_RTV_DIMENSION_TEXTURE2D;
        d.Texture2DArray.ArraySize = r.DepthOrArraySize;
    }
    return d;
}

static D3D12_DEPTH_STENCIL_VIEW_DESC whole_texture_dsv(const D3D12_RESOURCE_DESC1& r) {
    D3D12_DEPTH_STENCIL_VIEW_DESC d = {};
    if (r.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        d.ViewDimension = is_array(r) ? D3D12_DSV_DIMENSION_TEXTURE1DARRAY : D3D12_DSV_DIMENSION_TEXTURE1D;
        d.Texture1DArray.ArraySize = r.DepthOrArraySize;
    } else if (r.SampleDesc.Count > 1) {
        d.ViewDimension =
            is_array(r) ? D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D12_DSV_DIMENSION_TEXTURE2DMS;
        d.Texture2DMSArray.ArraySize = r.DepthOrArraySize;
    } else {
        d.ViewDimension = is_array(r) ? D3D12_DSV_DIMENSION_TEXTURE2DARRAY : D3D12_DSV_DIMENSION_TEXTURE2D;
        d.Texture2DArray.ArraySize = r.DepthOrArraySize;
    }
    return d;
}

static bool is_buffer(void* resource) {
    return resource && ((ConsoleResource*)resource)->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
}

void WINAPI device_create_constant_buffer_view(ConsoleDevice* self,
                                               const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                               D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    DescriptorSlot slot;
    if (!console_descriptor_slots(dest.ptr, 1, &slot))
        return;
    D3D12_CONSTANT_BUFFER_VIEW_DESC d = {};
    GpuAddress a = {};
    bool valid = desc && desc->BufferLocation && gpu_translate(desc->BufferLocation, desc->SizeInBytes, &a);
    if (valid && (a.desktop_address & (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1))) {
        LOG_E("d3d12", "constant buffer view at 0x%llx is not 256-byte aligned", desc->BufferLocation);
        valid = false;
    }
    if (valid) {
        // Console views can end anywhere; desktop sizes are whole 256-byte units. Round up, but
        // never past the window, where the GPU would read unmapped memory.
        u64 size = ALIGN_UP((u64)desc->SizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        if (size > a.available)
            size = ALIGN_DOWN(a.available, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        d.BufferLocation = a.desktop_address;
        d.SizeInBytes = (u32)size;
        valid = size != 0;
    }
    self->desktop->CreateConstantBufferView(valid ? &d : nullptr, slot.cpu);
    finish_write(slot);
}

// The console UAV description is the desktop one without its later union members, so it is shorter:
// copy only what the title passed.
#define CONSOLE_UAV_DESC_SIZE 40

static D3D12_UNORDERED_ACCESS_VIEW_DESC desktop_uav_desc(const void* console) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    memcpy(&d, console, CONSOLE_UAV_DESC_SIZE);
    return d;
}

// A buffer view without a description covers the whole buffer as raw 32-bit words.
static D3D12_SHADER_RESOURCE_VIEW_DESC whole_buffer_srv(const ConsoleResource* r) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.NumElements = (u32)(r->desc.Width / 4);
    d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    return d;
}

static D3D12_UNORDERED_ACCESS_VIEW_DESC whole_buffer_uav(const ConsoleResource* r) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = (u32)(r->desc.Width / 4);
    d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    return d;
}

// Failed views leave a null descriptor of the same kind rather than a stale one.
void WINAPI device_create_shader_resource_view(ConsoleDevice* self, void* resource,
                                               const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                               D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    DescriptorSlot slot;
    if (!console_descriptor_slots(dest.ptr, 1, &slot))
        return;
    ID3D12Resource* desktop;
    D3D12_SHADER_RESOURCE_VIEW_DESC d;
    if (is_buffer(resource)) {
        const ConsoleResource* r = (const ConsoleResource*)resource;
        d = desc ? *desc : whole_buffer_srv(r);
        u32 element = buffer_element_bytes(d.Format, d.Buffer.StructureByteStride,
                                           (d.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0);
        desktop = buffer_view_target(r->gpu_address, d.Buffer.FirstElement, d.Buffer.NumElements, element,
                                     &d.Buffer.FirstElement);
    } else {
        const ConsoleResource* r = (const ConsoleResource*)resource;
        if (!r && !desc)
            return; // the console API requires one or the other
        d = desc ? *desc : whole_texture_srv(r->desc);
        desktop = texture_view_target(r);
        if (!desktop || !texture_view_format(r, d.Format, true, &d.Format)) {
            desktop = nullptr;
            d.Format = NULL_VIEW_FORMAT;
        }
    }
    self->desktop->CreateShaderResourceView(desktop, &d, slot.cpu);
    finish_write(slot);
}

void WINAPI device_create_unordered_access_view(ConsoleDevice* self, void* resource, void* counter,
                                                const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    DescriptorSlot slot;
    if (!console_descriptor_slots(dest.ptr, 1, &slot))
        return;
    ID3D12Resource* desktop;
    ID3D12Resource* desktop_counter = nullptr;
    D3D12_UNORDERED_ACCESS_VIEW_DESC d;
    if (is_buffer(resource)) {
        const ConsoleResource* r = (const ConsoleResource*)resource;
        d = desc ? desktop_uav_desc(desc) : whole_buffer_uav(r);
        u32 element = buffer_element_bytes(d.Format, d.Buffer.StructureByteStride,
                                           (d.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0);
        desktop = buffer_view_target(r->gpu_address, d.Buffer.FirstElement, d.Buffer.NumElements, element,
                                     &d.Buffer.FirstElement);
        if (desktop && counter) {
            GpuAddress c;
            u64 at = ((const ConsoleResource*)counter)->gpu_address + d.Buffer.CounterOffsetInBytes;
            if (gpu_translate(at, 4, &c)) {
                desktop_counter = c.buffer;
                d.Buffer.CounterOffsetInBytes = c.offset;
            } else {
                desktop = nullptr;
            }
        }
    } else {
        const ConsoleResource* r = (const ConsoleResource*)resource;
        if (!r && !desc)
            return;
        d = desc ? desktop_uav_desc(desc) : whole_texture_uav(r->desc);
        desktop = texture_view_target(r);
        if (!desktop || !texture_view_format(r, d.Format, false, &d.Format)) {
            desktop = nullptr;
            d.Format = NULL_VIEW_FORMAT;
        }
    }
    self->desktop->CreateUnorderedAccessView(desktop, desktop_counter, &d, slot.cpu);
    finish_write(slot);
}

void WINAPI device_create_render_target_view(ConsoleDevice* self, void* resource,
                                             const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                             D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    DescriptorSlot slot;
    if (!console_descriptor_slots(dest.ptr, 1, &slot))
        return;
    const ConsoleResource* r = (const ConsoleResource*)resource;
    if (!r && !desc)
        return;
    D3D12_RENDER_TARGET_VIEW_DESC d = desc ? *desc : whole_texture_rtv(r->desc);
    ID3D12Resource* desktop = texture_view_target(r);
    if (!desktop || !texture_view_format(r, d.Format, false, &d.Format)) {
        desktop = nullptr;
        d.Format = NULL_VIEW_FORMAT;
    }
    self->desktop->CreateRenderTargetView(desktop, &d, slot.cpu);
    finish_write(slot);
}

void WINAPI device_create_depth_stencil_view(ConsoleDevice* self, void* resource,
                                             const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                             D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    DescriptorSlot slot;
    if (!console_descriptor_slots(dest.ptr, 1, &slot))
        return;
    const ConsoleResource* r = (const ConsoleResource*)resource;
    if (!r && !desc)
        return;
    D3D12_DEPTH_STENCIL_VIEW_DESC d = desc ? *desc : whole_texture_dsv(r->desc);
    ID3D12Resource* desktop = texture_view_target(r);
    if (!desktop || !texture_view_format(r, d.Format, false, &d.Format)) {
        desktop = nullptr;
        d.Format = NULL_DEPTH_VIEW_FORMAT;
    }
    self->desktop->CreateDepthStencilView(desktop, &d, slot.cpu);
    finish_write(slot);
}

void WINAPI device_create_sampler(ConsoleDevice* self, const D3D12_SAMPLER_DESC* desc,
                                  D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    DescriptorSlot slot;
    if (!desc || !console_descriptor_slots(dest.ptr, 1, &slot))
        return;
    self->desktop->CreateSampler(desc, slot.cpu);
    finish_write(slot);
}

// Copies `count` descriptors between two ranges, each inside one heap.
static void copy_range(ConsoleDevice* self, u64 dest, u64 src, u32 count, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    if (!console_descriptor_size(type))
        return;
    DescriptorSlot d, s;
    if (!console_descriptor_slots(dest, count, &d) || !console_descriptor_slots(src, count, &s))
        return;
    if (d.type != s.type) {
        LOG_E("d3d12", "descriptor copy between heap types %d and %d", s.type, d.type);
        return;
    }
    self->desktop->CopyDescriptorsSimple(count, d.cpu, s.cpu, d.type);
    console_descriptor_publish(d, count);
    memmove(d.console, s.console, (usize)count * console_descriptor_size(type));
}

void WINAPI device_copy_descriptors_simple(ConsoleDevice* self, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest,
                                           D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    if (count)
        copy_range(self, dest.ptr, src.ptr, count, type);
}

// Walks the destination and source range lists in step, copying the overlap of the current pair.
// A missing size array means ranges of one descriptor each.
void WINAPI device_copy_descriptors(ConsoleDevice* self, UINT dest_range_count,
                                    const D3D12_CPU_DESCRIPTOR_HANDLE* dest_starts, const UINT* dest_sizes,
                                    UINT src_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE* src_starts,
                                    const UINT* src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    u32 console_stride = console_descriptor_size(type);
    if (!console_stride)
        return;
    u32 di = 0, si = 0, d_done = 0, s_done = 0;
    while (di < dest_range_count && si < src_range_count) {
        u32 d_size = dest_sizes ? dest_sizes[di] : 1;
        u32 s_size = src_sizes ? src_sizes[si] : 1;
        if (d_done == d_size) {
            di++, d_done = 0;
            continue;
        }
        if (s_done == s_size) {
            si++, s_done = 0;
            continue;
        }
        u32 n = d_size - d_done < s_size - s_done ? d_size - d_done : s_size - s_done;
        copy_range(self, dest_starts[di].ptr + (u64)d_done * console_stride,
                   src_starts[si].ptr + (u64)s_done * console_stride, n, type);
        d_done += n;
        s_done += n;
    }
}

// --- views of memory without a resource object -------------------------------------------------------

// Console view descriptions that also carry the memory's GPU address.
struct ConsoleSrvDescX {
    D3D12_SHADER_RESOURCE_VIEW_DESC view;
    u64 location;
    u32 image_format;
    u32 memory_type;
    f32 warn_lod;
    s32 perf_modulation;
};
static_assert(offsetof(ConsoleSrvDescX, location) == 40, "console SRV description layout");

struct ConsoleUavDescX {
    u8 view[CONSOLE_UAV_DESC_SIZE];
    u64 location;
    u64 counter_location;
    u32 memory_type;
};

static void unsupported_placed_texture_view(const char* kind) {
    static volatile long once = 0;
    if (log_once_flag(&once))
        LOG_W("d3d12",
              "%s views of placed textures without a resource are not supported yet; writing null views",
              kind);
}

void WINAPI device_create_placed_raw_srv_x(ConsoleDevice* self, const D3D12_RESOURCE_DESC* resource_desc,
                                           const ConsoleSrvDescX* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    DescriptorSlot slot;
    if (!resource_desc || !desc || !console_descriptor_slots(dest.ptr, 1, &slot))
        return;
    D3D12_SHADER_RESOURCE_VIEW_DESC d = desc->view;
    ID3D12Resource* desktop = nullptr;
    if (resource_desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        d.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
        u32 element = buffer_element_bytes(d.Format, d.Buffer.StructureByteStride,
                                           (d.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0);
        desktop = buffer_view_target(desc->location, d.Buffer.FirstElement, d.Buffer.NumElements, element,
                                     &d.Buffer.FirstElement);
    } else {
        unsupported_placed_texture_view("shader resource");
    }
    self->desktop->CreateShaderResourceView(desktop, &d, slot.cpu);
    finish_write(slot);
}

void WINAPI device_create_placed_raw_uav_x(ConsoleDevice* self, const D3D12_RESOURCE_DESC* resource_desc,
                                           const ConsoleUavDescX* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    DescriptorSlot slot;
    if (!resource_desc || !desc || !console_descriptor_slots(dest.ptr, 1, &slot))
        return;
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = desktop_uav_desc(desc->view);
    ID3D12Resource* desktop = nullptr;
    ID3D12Resource* desktop_counter = nullptr;
    if (resource_desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        d.ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
        u32 element = buffer_element_bytes(d.Format, d.Buffer.StructureByteStride,
                                           (d.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0);
        desktop = buffer_view_target(desc->location, d.Buffer.FirstElement, d.Buffer.NumElements, element,
                                     &d.Buffer.FirstElement);
        if (desktop && desc->counter_location) {
            GpuAddress c;
            if (gpu_translate(desc->counter_location + d.Buffer.CounterOffsetInBytes, 4, &c)) {
                desktop_counter = c.buffer;
                d.Buffer.CounterOffsetInBytes = c.offset;
            } else {
                desktop = nullptr;
            }
        }
    } else {
        unsupported_placed_texture_view("unordered access");
    }
    if (!desktop_counter)
        d.Buffer.CounterOffsetInBytes = 0;
    self->desktop->CreateUnorderedAccessView(desktop, desktop_counter, &d, slot.cpu);
    finish_write(slot);
}

// --- console sampler descriptions --------------------------------------------------------------------

// The console sampler description sets each filter stage separately, as the hardware does.
struct ConsoleSamplerDescX {
    u32 flags;
    u32 filter_mag; // CONSOLE_XY_FILTER_*
    u32 filter_min;
    u32 filter_mip; // CONSOLE_MIP_FILTER_*
    u32 filter_z;
    u32 filter_mode; // CONSOLE_FILTER_MODE_*
    D3D12_TEXTURE_ADDRESS_MODE address_u;
    D3D12_TEXTURE_ADDRESS_MODE address_v;
    D3D12_TEXTURE_ADDRESS_MODE address_w;
    f32 min_lod;
    f32 max_lod;
    f32 mip_lod_bias;
    f32 mip_lod_bias_secondary;
    u32 max_anisotropy;
    f32 anisotropy_bias;
    u32 anisotropy_threshold;
    D3D12_COMPARISON_FUNC comparison_func;
    f32 border_color[4];
    u32 performance_mip;
    u32 performance_z;
    u32 min_mip_slope_u;
    u32 min_mip_offset_u;
    u32 min_mip_slope_v_or_w;
    u32 min_mip_offset_v_or_w;
};
static_assert(sizeof(ConsoleSamplerDescX) == 108, "console sampler description layout");

#define CONSOLE_SAMPLER_FLAG_UNNORMALIZED_COORDINATES 0x1
#define CONSOLE_XY_FILTER_BILINEAR                    1
#define CONSOLE_XY_FILTER_ANISO_POINT                 2
#define CONSOLE_XY_FILTER_ANISO_BILINEAR              3
#define CONSOLE_MIP_FILTER_NONE                       0
#define CONSOLE_MIP_FILTER_LINEAR                     2
#define CONSOLE_FILTER_MODE_MIN                       1
#define CONSOLE_FILTER_MODE_MAX                       2

static D3D12_SAMPLER_DESC2 desktop_sampler_desc(const ConsoleSamplerDescX& c) {
    D3D12_SAMPLER_DESC2 d = {};
    bool aniso =
        c.filter_min >= CONSOLE_XY_FILTER_ANISO_POINT || c.filter_mag >= CONSOLE_XY_FILTER_ANISO_POINT;
    bool min_linear =
        c.filter_min == CONSOLE_XY_FILTER_BILINEAR || c.filter_min == CONSOLE_XY_FILTER_ANISO_BILINEAR;
    bool mag_linear =
        c.filter_mag == CONSOLE_XY_FILTER_BILINEAR || c.filter_mag == CONSOLE_XY_FILTER_ANISO_BILINEAR;
    bool mip_linear = c.filter_mip == CONSOLE_MIP_FILTER_LINEAR;
    D3D12_FILTER_REDUCTION_TYPE reduction = D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    if (c.filter_mode == CONSOLE_FILTER_MODE_MIN)
        reduction = D3D12_FILTER_REDUCTION_TYPE_MINIMUM;
    else if (c.filter_mode == CONSOLE_FILTER_MODE_MAX)
        reduction = D3D12_FILTER_REDUCTION_TYPE_MAXIMUM;
    else if (c.comparison_func > D3D12_COMPARISON_FUNC_NEVER)
        reduction = D3D12_FILTER_REDUCTION_TYPE_COMPARISON;
    if (aniso)
        d.Filter = mip_linear ? D3D12_ENCODE_ANISOTROPIC_FILTER(reduction)
                              : D3D12_ENCODE_MIN_MAG_ANISOTROPIC_MIP_POINT_FILTER(reduction);
    else
        d.Filter = D3D12_ENCODE_BASIC_FILTER(min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                             mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                             mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                             reduction);
    d.AddressU = c.address_u;
    d.AddressV = c.address_v;
    d.AddressW = c.address_w;
    d.MipLODBias = c.mip_lod_bias;
    d.MaxAnisotropy = c.max_anisotropy ? (c.max_anisotropy > 16 ? 16 : c.max_anisotropy) : 1;
    d.ComparisonFunc = c.comparison_func ? c.comparison_func : D3D12_COMPARISON_FUNC_NEVER;
    for (u32 i = 0; i < 4; i++)
        d.FloatBorderColor[i] = c.border_color[i];
    d.MinLOD = c.min_lod;
    d.MaxLOD =
        c.filter_mip == CONSOLE_MIP_FILTER_NONE ? c.min_lod : c.max_lod; // no mipmapping: stay at MinLOD
    if (c.flags & CONSOLE_SAMPLER_FLAG_UNNORMALIZED_COORDINATES)
        d.Flags = D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES;
    return d;
}

void WINAPI device_create_sampler_x(ConsoleDevice* self, const ConsoleSamplerDescX* desc,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    DescriptorSlot slot;
    if (!desc || !console_descriptor_slots(dest.ptr, 1, &slot))
        return;
    D3D12_SAMPLER_DESC2 d = desktop_sampler_desc(*desc);
    self->desktop->CreateSampler2(&d, slot.cpu);
    finish_write(slot);
}
