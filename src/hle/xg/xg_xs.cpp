// xg_xs.dll: the console's texture layout library, on gpu/console_layout.
//
// Titles ask it which swizzle mode a texture gets and where every mip and texel lives, to place
// textures in their memory and stream data into them.

#include <windows.h>
#include <d3d12.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "gpu/console_layout.h"
#include "gpu/formats.h"
#include "hle/hle.h"

// The texture computer is a C++ object: a virtual function table and the computed layout. Its
// methods take the object first, like the console library's.
struct TextureComputer {
    const void* const* vtable;
    volatile long refs;
    ConsoleTextureDesc desc;
    ConsoleTextureLayout layout;
};

static ConsoleTextureDesc desc_from_d3d12(const D3D12_RESOURCE_DESC& d) {
    ConsoleTextureDesc t = {};
    t.dimension = d.Dimension;
    t.width = d.Width;
    t.height = d.Height;
    t.depth_or_array_size = d.DepthOrArraySize;
    t.mip_levels = d.MipLevels;
    t.format = d.Format;
    t.sample_count = d.SampleDesc.Count ? d.SampleDesc.Count : 1;
    t.layout = d.Layout;
    t.flags = d.Flags;
    return t;
}

// --- computer methods ------------------------------------------------------------------------------

static ULONG computer_add_ref(TextureComputer* self) {
    return (ULONG)InterlockedIncrement(&self->refs);
}

static ULONG computer_release(TextureComputer* self) {
    long refs = InterlockedDecrement(&self->refs);
    if (refs == 0)
        free(self);
    return (ULONG)refs;
}

static HRESULT computer_get_resource_layout(TextureComputer* self, ConsoleTextureLayout* out) {
    if (!out)
        return E_INVALIDARG;
    *out = self->layout;
    return S_OK;
}

static UINT64 computer_get_resource_size_bytes(TextureComputer* self) {
    return self->layout.size;
}

static UINT64 computer_get_resource_base_alignment_bytes(TextureComputer* self) {
    return self->layout.base_alignment;
}

static UINT64 computer_get_mip_level_offset_bytes(TextureComputer* self, UINT32 plane, UINT32 mip) {
    if (plane >= self->layout.plane_count || mip >= self->layout.mip_levels)
        return 0;
    return self->layout.planes[plane].base_offset + self->layout.planes[plane].mips[mip].offset;
}

static UINT64 computer_get_texel_element_offset_bytes(TextureComputer* self, UINT32 plane, UINT32 mip,
                                                      UINT64 x, UINT32 y, UINT32 z_or_slice, UINT32 sample,
                                                      UINT32* bit_position) {
    return console_texel_offset(self->desc, self->layout, plane, mip, (u32)x, y, z_or_slice, sample,
                                bit_position);
}

// Subresource indices count mips first, then array slices; a volume has one per mip.
static bool split_subresource(const TextureComputer* self, UINT32 subresource, u32* mip, u32* slice) {
    u32 mips = self->layout.mip_levels;
    *mip = subresource % mips;
    *slice = subresource / mips;
    u32 slices =
        self->desc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : self->desc.depth_or_array_size;
    return *slice < slices;
}

static HRESULT computer_copy_into_subresource(TextureComputer* self, void* texture, UINT32 plane,
                                              UINT32 subresource, const void* linear, UINT32 row_pitch,
                                              UINT32 slice_pitch) {
    u32 mip, slice;
    if (!texture || !linear || !split_subresource(self, subresource, &mip, &slice))
        return E_INVALIDARG;
    return console_copy_subresource(self->desc, self->layout, plane, mip, slice, texture, (void*)linear,
                                    row_pitch, slice_pitch, true)
               ? S_OK
               : E_INVALIDARG;
}

static HRESULT computer_copy_from_subresource(TextureComputer* self, void* linear, UINT32 plane,
                                              UINT32 subresource, UINT32 sample, const void* texture,
                                              UINT32 row_pitch, UINT32 slice_pitch) {
    u32 mip, slice;
    if (!texture || !linear || !split_subresource(self, subresource, &mip, &slice))
        return E_INVALIDARG;
    if (sample != 0) {
        LOG_W("xg", "CopyFromSubresource: sample %u of an MSAA texture is not supported", sample);
        return E_NOTIMPL;
    }
    return console_copy_subresource(self->desc, self->layout, plane, mip, slice, (void*)texture, linear,
                                    row_pitch, slice_pitch, false)
               ? S_OK
               : E_INVALIDARG;
}

static HRESULT computer_get_texel_coordinate(TextureComputer*, UINT64, UINT32*, UINT32*, UINT64*, UINT32*,
                                             UINT32*, UINT32*) {
    LOG_W("xg", "XGTextureAddressComputer::GetTexelCoordinate is not implemented");
    return E_NOTIMPL;
}

// Tiled resources: titles stream textures in 64 KiB tiles, mapping page i of a texture's memory for
// tile i, so tiles follow the layout in memory: a standard mip's tiles start where the mip does, and
// the mips in the mip tail share the tail's tile(s) ("packed" mips).
struct PackedMips {
    u8 standard_mips;
    u8 packed_mips;
    u32 packed_tiles; // per array slice
    u32 first_packed_tile;
};

struct TileShape {
    u32 width, height, depth; // in texels
};

struct SubresourceTiling {
    u32 width_in_tiles;
    u16 height_in_tiles;
    u16 depth_in_tiles;
    u32 first_tile; // PACKED_TILE for mips in the tail
};

#define TILE_SIZE   0x10000u
#define PACKED_TILE 0xFFFFFFFFu

static HRESULT computer_get_resource_tiling(TextureComputer* self, UINT32* tile_count, PackedMips* packed,
                                            TileShape* shape, UINT32* tiling_count, UINT32 first_tiling,
                                            SubresourceTiling* tilings) {
    const ConsolePlaneLayout& p = self->layout.planes[0];
    u32 mips = self->layout.mip_levels;
    u32 slices =
        self->desc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : self->desc.depth_or_array_size;
    // The tile shape is the swizzle block of the standard mips, which must be 64 KiB.
    ConsoleSubresourceAddressing a = {};
    bool tiled = p.standard_mip_count > 0 &&
                 console_subresource_addressing(self->desc, self->layout, 0, 0, 0, &a, false) && !a.linear &&
                 a.block_bytes_shift == 16;
    if (!tiled && p.standard_mip_count > 0) {
        LOG_W("xg", "GetResourceTiling of a texture without 64 KiB tiles (format %d, swizzle mode %u)",
              self->desc.format, p.mips[0].swizzle_mode);
        return E_INVALIDARG;
    }
    if (tile_count)
        *tile_count = (UINT32)((self->layout.size + TILE_SIZE - 1) / TILE_SIZE);
    u32 tail_tiles = 0;
    u64 tail_offset = 0;
    if (p.tail_mip_count) {
        const ConsoleMipLayout& first_tail = p.mips[p.standard_mip_count];
        tail_offset = p.base_offset + first_tail.offset;
        tail_tiles = (u32)((first_tail.size + TILE_SIZE - 1) / TILE_SIZE);
        tail_tiles = tail_tiles ? tail_tiles : 1;
    }
    if (packed) {
        packed->standard_mips = (u8)p.standard_mip_count;
        packed->packed_mips = (u8)p.tail_mip_count;
        packed->packed_tiles = tail_tiles;
        packed->first_packed_tile =
            p.tail_mip_count ? (u32)(ALIGN_DOWN(tail_offset, (u64)TILE_SIZE) / TILE_SIZE) : 0;
    }
    FormatInfo f = format_info(self->desc.format);
    u32 block_w = f.block_width ? f.block_width : 1, block_h = f.block_height ? f.block_height : 1;
    if (shape) {
        if (tiled) {
            shape->width = (1u << a.block_shift[0]) * block_w;
            shape->height = (1u << a.block_shift[1]) * block_h;
            shape->depth = 1u << a.block_shift[2];
        } else {
            *shape = {0, 0, 0}; // everything is packed
        }
    }
    if (tiling_count) {
        u32 total = mips * slices;
        u32 count = first_tiling < total ? total - first_tiling : 0;
        count = count < *tiling_count ? count : *tiling_count;
        for (u32 i = 0; i < count && tilings; i++) {
            u32 sub = first_tiling + i, mip = sub % mips, slice = sub / mips;
            const ConsoleMipLayout& m = p.mips[mip];
            SubresourceTiling& t = tilings[i];
            if (mip >= p.standard_mip_count) {
                t = {0, 0, 0, PACKED_TILE};
                continue;
            }
            u32 tw = 1u << a.block_shift[0], th = 1u << a.block_shift[1], td = 1u << a.block_shift[2];
            u32 depth =
                self->desc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? m.depth_or_array_size : 1;
            t.width_in_tiles = (m.width + tw - 1) / tw;
            t.height_in_tiles = (u16)((m.height + th - 1) / th);
            t.depth_in_tiles = (u16)((depth + td - 1) / td);
            t.first_tile = (u32)((slice * p.slice_size + p.base_offset + m.offset) / TILE_SIZE);
        }
        *tiling_count = count;
    }
    return S_OK;
}

static HRESULT computer_get_texture_view_descriptor(TextureComputer*, UINT32, void*) {
    LOG_W("xg", "XGTextureAddressComputer::GetTextureViewDescriptor is not implemented");
    return E_NOTIMPL;
}

// Tiled (reserved) resources use the _T swizzle modes.
static bool computer_is_tiled_resource(TextureComputer* self) {
    u32 mode = self->layout.planes[0].mips[0].swizzle_mode;
    return mode >= 16 && mode <= 19;
}

static HRESULT computer_get_swizzle_pattern_info(TextureComputer*, UINT32, void*) {
    LOG_W("xg", "XGTextureAddressComputer::GetSwizzlePatternInfo is not implemented");
    return E_NOTIMPL;
}

static const void* const k_computer_vtable[] = {
    (void*)&computer_add_ref,
    (void*)&computer_release,
    (void*)&computer_get_resource_layout,
    (void*)&computer_get_resource_size_bytes,
    (void*)&computer_get_resource_base_alignment_bytes,
    (void*)&computer_get_mip_level_offset_bytes,
    (void*)&computer_get_texel_element_offset_bytes,
    (void*)&computer_get_texel_coordinate,
    (void*)&computer_copy_into_subresource,
    (void*)&computer_copy_from_subresource,
    (void*)&computer_get_resource_tiling,
    (void*)&computer_get_texture_view_descriptor,
    (void*)&computer_is_tiled_resource,
    (void*)&computer_get_swizzle_pattern_info,
};

// --- exports ---------------------------------------------------------------------------------------

static HRESULT __cdecl XGSuggestSwizzleMode(const D3D12_RESOURCE_DESC* desc, UINT32 flags, UINT32* layout) {
    if (!desc || !layout)
        return E_INVALIDARG;
    u32 suggested = console_suggest_layout(desc_from_d3d12(*desc));
    if (!suggested) {
        LOG_W("xg", "XGSuggestSwizzleMode: no swizzle mode for format %d", desc->Format);
        return E_INVALIDARG;
    }
    *layout = suggested;
    return S_OK;
}

static HRESULT __cdecl XGCreateTextureComputer(const D3D12_RESOURCE_DESC* desc, TextureComputer** out) {
    if (!desc || !out)
        return E_INVALIDARG;
    *out = nullptr;
    TextureComputer* c = (TextureComputer*)calloc(1, sizeof(TextureComputer));
    if (!c)
        return E_OUTOFMEMORY;
    c->vtable = k_computer_vtable;
    c->refs = 1;
    c->desc = desc_from_d3d12(*desc);
    if (!console_texture_layout(c->desc, &c->layout)) {
        LOG_W("xg",
              "XGCreateTextureComputer: no layout for dim %d, %llux%ux%u, mips %u, format %d, layout 0x%x",
              desc->Dimension, desc->Width, desc->Height, desc->DepthOrArraySize, desc->MipLevels,
              desc->Format, desc->Layout);
        free(c);
        return E_INVALIDARG;
    }
    *out = c;
    return S_OK;
}

static const HleExport k_exports[] = {
    HLE_EXPORT(XGSuggestSwizzleMode),
    HLE_EXPORT(XGCreateTextureComputer),
};

extern const HleModule g_hle_module_xg_xs = {"xg_xs.dll", k_exports, ARRAY_COUNT(k_exports)};
