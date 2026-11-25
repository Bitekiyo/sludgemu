#pragma once

#include <windows.h>
#include <dxgiformat.h>

#include "common/types.h"
#include "core/console_profile.h"

// Console GPU texture layouts.
//
// Console textures live in title memory in the GPU's own layout: RDNA2 swizzle modes, mip chains
// laid out smallest first, compression metadata planes next to the image. Titles size and place
// textures from these layouts, stream data into them, and read texels back, so they have to come
// out exactly as on console. They are computed with AMD's address library (addrlib), configured
// as the console GPU (the two console models differ in memory pipes and packers).
//
// The layout structs below follow the console layout library's ABI, so they can be handed to
// titles as they are.

// Swizzle mode as stored in a texture layout value: CONSOLE_LAYOUT_SWIZZLE | mode, with the bank
// rotation in bits 12-15.
#define CONSOLE_LAYOUT_SWIZZLE      0x100u
#define CONSOLE_LAYOUT_SWIZZLE_MASK 0x11Fu

enum ConsolePlaneUsage : u32 {
    CONSOLE_PLANE_UNUSED = 0,
    CONSOLE_PLANE_DEFAULT = 1,
    CONSOLE_PLANE_COLOR_MASK = 2,    // CMask: fast clear state per tile
    CONSOLE_PLANE_FRAGMENT_MASK = 3, // FMask: MSAA fragment indices
    CONSOLE_PLANE_HTILE = 4,         // depth compression metadata
    CONSOLE_PLANE_LUMA = 5,
    CONSOLE_PLANE_CHROMA = 6,
    CONSOLE_PLANE_DEPTH = 7,
    CONSOLE_PLANE_STENCIL = 8,
    CONSOLE_PLANE_DCC = 9, // delta color compression metadata
};

struct ConsoleMipLayout {
    u64 size;
    u64 offset; // from the start of the plane
    u64 slice_2d_size;
    u32 pitch_pixels;
    u32 pitch_bytes;
    u32 alignment;
    u32 padded_width; // in elements
    u32 padded_height;
    u32 padded_depth_or_array_size;
    u32 width; // in elements
    u32 height;
    u32 width_pixels;
    u32 height_pixels;
    u32 depth_or_array_size;
    u32 sample_count;
    u32 swizzle_mode;
    u64 bank_rotation_address_mask;
    u64 bank_rotation_bytes_per_slice;
    u32 slice_depth; // in elements
    u32 in_mip_tail;
    u32 tail_x : 11; // position inside the mip tail, in elements
    u32 tail_y : 11;
    u32 tail_z : 10;
};
static_assert(sizeof(ConsoleMipLayout) == 112, "console mip layout ABI");

#define CONSOLE_MAX_MIPS   15
#define CONSOLE_MAX_PLANES 4

struct ConsolePlaneLayout {
    ConsolePlaneUsage usage;
    u64 size;
    u64 slice_size; // one array slice: a whole mip chain
    u64 base_offset;
    u64 base_alignment;
    u32 bytes_per_element;
    u32 standard_mip_count; // mips before the mip tail
    u32 tail_mip_count;
    ConsoleMipLayout mips[CONSOLE_MAX_MIPS];
};

struct ConsoleTextureLayout {
    u64 size;
    u64 base_alignment;
    u32 mip_levels;
    u32 plane_count;
    ConsolePlaneLayout planes[CONSOLE_MAX_PLANES];
    u32 dimension; // D3D12_RESOURCE_DIMENSION
};

// What a layout is computed from (a D3D12 resource description).
struct ConsoleTextureDesc {
    u32 dimension; // D3D12_RESOURCE_DIMENSION
    u64 width;
    u32 height;
    u32 depth_or_array_size;
    u32 mip_levels; // 0 = full chain
    DXGI_FORMAT format;
    u32 sample_count;
    u32 layout; // D3D12_TEXTURE_LAYOUT, or a console swizzle mode (CONSOLE_LAYOUT_SWIZZLE | mode)
    u32 flags;  // D3D12 resource flags, console flags included
};

// Sets up layout computation for a console model. Call once, before any layout is computed.
bool console_layout_init(ConsoleModel model);

// The swizzle mode the console runtime picks for a texture (a CONSOLE_LAYOUT_SWIZZLE value), or 0.
u32 console_suggest_layout(const ConsoleTextureDesc& desc);

// Computes the layout of a texture. Fails for descriptions the console can't create.
bool console_texture_layout(const ConsoleTextureDesc& desc, ConsoleTextureLayout* out);

// Byte offset of a texel element from the start of the texture, or ~0 if out of range.
u64 console_texel_offset(const ConsoleTextureDesc& desc, const ConsoleTextureLayout& layout, u32 plane,
                         u32 mip, u32 x, u32 y, u32 z_or_slice, u32 sample, u32* bit_position);

// Copies one subresource (mip `mip` of array slice `slice`; a whole volume mip for 3D) of a plane
// between linear memory and texture memory in console layout.
bool console_copy_subresource(const ConsoleTextureDesc& desc, const ConsoleTextureLayout& layout, u32 plane,
                              u32 mip, u32 slice, void* texture, void* linear, u64 row_pitch, u64 slice_pitch,
                              bool into_texture);
