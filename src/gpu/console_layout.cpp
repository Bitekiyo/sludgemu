#include "gpu/console_layout.h"

#include <d3d12.h>
#include <stdlib.h>
#include <string.h>

#include "addrinterface.h"
#include "common/log.h"
#include "gpu/formats.h"

// addrlib chip identification: the console GPUs are RDNA2 (gfx10.3, with RB+), the same addressing
// as the first desktop RDNA2 chip.
#define ADDR_ENGINE_GFX9_AND_LATER 0x0D
#define ADDR_FAMILY_NAVI           0x8F
#define ADDR_REVISION_NAVI21       0x28

// GB_ADDR_CONFIG of the console GPUs: log2 pipes in bits 0-2, 256-byte pipe interleave (0) in bits
// 3-5, log2 max compressed fragments in bits 6-7, log2 packers in bits 8-10. Pipes and packers
// decide the XOR swizzle modes; they were matched against the console's layouts.
#define GB_ADDR_CONFIG(pipes_log2, packers_log2, frags_log2)                                                 \
    ((pipes_log2) | ((frags_log2) << 6) | ((packers_log2) << 8))

static ADDR_HANDLE g_addrlib = nullptr;

static VOID* ADDR_API addrlib_alloc(const ADDR_ALLOCSYSMEM_INPUT* in) {
    return malloc(in->sizeInBytes);
}

static ADDR_E_RETURNCODE ADDR_API addrlib_free(const ADDR_FREESYSMEM_INPUT* in) {
    free(in->pVirtAddr);
    return ADDR_OK;
}

bool console_layout_init(ConsoleModel model) {
    ADDR_CREATE_INPUT in = {};
    ADDR_CREATE_OUTPUT out = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.chipEngine = ADDR_ENGINE_GFX9_AND_LATER;
    in.chipFamily = ADDR_FAMILY_NAVI;
    in.chipRevision = ADDR_REVISION_NAVI21;
    in.callbacks.allocSysMem = addrlib_alloc;
    in.callbacks.freeSysMem = addrlib_free;
    in.regValue.gbAddrConfig = model == CONSOLE_SERIES_S ? GB_ADDR_CONFIG(3, 2, 1) : GB_ADDR_CONFIG(5, 3, 1);
    if (AddrCreate(&in, &out) != ADDR_OK) {
        LOG_E("gpu", "cannot set up texture layout computation");
        return false;
    }
    g_addrlib = out.hLib;
    return true;
}

// --- descriptions ------------------------------------------------------------------------------------

static AddrResourceType resource_type(u32 dimension) {
    switch (dimension) {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
        return ADDR_RSRC_TEX_1D;
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
        return ADDR_RSRC_TEX_3D;
    default:
        return ADDR_RSRC_TEX_2D;
    }
}

// addrlib needs the format only where elements aren't plain texels.
static AddrFormat addr_format(DXGI_FORMAT format, const FormatInfo& info) {
    switch (format) {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        return ADDR_FMT_BC1;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
        return ADDR_FMT_BC2;
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
        return ADDR_FMT_BC3;
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return ADDR_FMT_BC4;
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
        return ADDR_FMT_BC5;
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
        return ADDR_FMT_BC6;
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return ADDR_FMT_BC7;
    default:
        break;
    }
    switch (info.bytes_per_element) {
    case 1:
        return ADDR_FMT_8;
    case 2:
        return ADDR_FMT_16;
    case 4:
        return ADDR_FMT_32;
    case 8:
        return ADDR_FMT_32_32;
    case 12:
        return ADDR_FMT_32_32_32;
    case 16:
        return ADDR_FMT_32_32_32_32;
    default:
        return ADDR_FMT_INVALID;
    }
}

static u32 mip_count(const ConsoleTextureDesc& d) {
    if (d.mip_levels)
        return d.mip_levels;
    u64 largest = d.width > d.height ? d.width : d.height;
    if (d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && d.depth_or_array_size > largest)
        largest = d.depth_or_array_size;
    u32 levels = 1;
    while (largest > 1) {
        largest >>= 1;
        levels++;
    }
    return levels;
}

static ADDR2_SURFACE_FLAGS surface_flags(const ConsoleTextureDesc& d, const FormatInfo& info, bool stencil) {
    ADDR2_SURFACE_FLAGS f = {};
    bool depth_format = info.kind == FORMAT_KIND_DEPTH || info.kind == FORMAT_KIND_DEPTH_STENCIL;
    f.color = (d.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    f.depth = !stencil && depth_format && (d.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    f.stencil = stencil;
    f.texture = !(d.flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    f.unordered = (d.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    return f;
}

// Console resource flag: the texture may use delta color compression.
#define CONSOLE_RESOURCE_FLAG_ALLOW_DCC 0x400000u

// The console runtime's choice, which differs from addrlib's own preference: small standard
// swizzled pages for textures that are only sampled, 64 KiB render-target swizzles for anything the
// GPU renders to or compresses, depth swizzles for depth, and for UAVs whichever page size fits the
// texture (larger than 64 KiB: 64 KiB pages).
u32 console_suggest_layout(const ConsoleTextureDesc& d) {
    FormatInfo info = format_info(d.format);
    if (info.kind == FORMAT_KIND_UNKNOWN)
        return 0;
    AddrSwizzleMode mode;
    if (d.flags & CONSOLE_RESOURCE_FLAG_ALLOW_DCC) {
        mode = ADDR_SW_64KB_R_X;
    } else if (d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        mode = ADDR_SW_LINEAR;
    } else if (d.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
        mode = ADDR_SW_64KB_Z_X;
    } else if (d.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
        mode = ADDR_SW_64KB_R_X;
    } else if ((d.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
               d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        u64 width = (d.width + info.block_width - 1) / info.block_width;
        u64 height = (d.height + info.block_height - 1) / info.block_height;
        mode = width * height * info.bytes_per_element > KiB(64) ? ADDR_SW_64KB_R_X : ADDR_SW_4KB_S_X;
    } else {
        mode = ADDR_SW_4KB_S;
    }
    return CONSOLE_LAYOUT_SWIZZLE | mode;
}

// --- layouts -----------------------------------------------------------------------------------------

// Computes one image plane (the color image, or the depth or stencil part of a depth format).
static bool image_plane(const ConsoleTextureDesc& d, const FormatInfo& info, AddrSwizzleMode swizzle,
                        bool stencil, ConsolePlaneUsage usage, ConsolePlaneLayout* p) {
    u32 mips = mip_count(d);
    u32 bpe = stencil ? 1 : info.bytes_per_element;
    ADDR2_COMPUTE_SURFACE_INFO_INPUT in = {};
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT out = {};
    ADDR2_MIP_INFO mip_info[CONSOLE_MAX_MIPS] = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.flags = surface_flags(d, info, stencil);
    in.swizzleMode = swizzle;
    in.resourceType = resource_type(d.dimension);
    in.format = stencil ? ADDR_FMT_8 : addr_format(d.format, info);
    in.bpp = bpe * 8;
    in.width = (u32)d.width;
    in.height = d.height;
    in.numSlices = d.depth_or_array_size;
    in.numMipLevels = mips;
    in.numSamples = d.sample_count ? d.sample_count : 1;
    in.numFrags = in.numSamples;
    out.pMipInfo = mip_info;
    if (Addr2ComputeSurfaceInfo(g_addrlib, &in, &out) != ADDR_OK)
        return false;

    // Linear surfaces have no mip tail. Volumes (thick swizzles) count a slice as one block deep.
    bool volume = d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    u32 first_in_tail = swizzle == ADDR_SW_LINEAR ? mips : out.firstMipIdInTail;
    u32 block_slice_bytes = out.blockWidth * out.blockHeight * bpe;
    p->usage = usage;
    p->size = out.surfSize;
    p->slice_size = volume ? out.sliceSize * out.blockSlices : out.sliceSize;
    p->base_alignment = out.baseAlign;
    p->bytes_per_element = bpe;
    p->standard_mip_count = first_in_tail;
    p->tail_mip_count = mips - first_in_tail;
    for (u32 m = 0; m < mips; m++) {
        const ADDR2_MIP_INFO& mi = mip_info[m];
        ConsoleMipLayout& ml = p->mips[m];
        u32 width_pixels = (u32)(d.width >> m) ? (u32)(d.width >> m) : 1;
        u32 height_pixels = (d.height >> m) ? (d.height >> m) : 1;
        u32 depth = volume ? ((d.depth_or_array_size >> m) ? (d.depth_or_array_size >> m) : 1)
                           : d.depth_or_array_size;
        bool in_tail = m >= first_in_tail;
        ml.slice_2d_size = (u64)mi.pitch * mi.height * bpe * in.numSamples;
        if (!volume) {
            ml.offset = mi.offset;
            ml.padded_depth_or_array_size = out.numSlices;
            ml.size = ml.slice_2d_size;
        } else {
            // Volume mips are padded from the depth halved and rounded up at each level, to whole
            // block slices; mips in the mip tail to half block slices. Mips follow each other block
            // slice by block slice, and a mip in the tail is placed by its tail coordinates.
            u32 rounded_depth = (d.depth_or_array_size + (1u << m) - 1) >> m;
            u32 slices = in_tail ? out.blockSlices / 2 : out.blockSlices;
            ml.padded_depth_or_array_size = (rounded_depth + slices - 1) & ~(slices - 1);
            if (in_tail)
                ml.slice_2d_size = block_slice_bytes;
            ml.offset = in_tail ? 0 : mi.macroBlockOffset;
            ml.size = ml.slice_2d_size * ml.padded_depth_or_array_size;
        }
        ml.pitch_pixels = mi.pitch * info.block_width;
        ml.pitch_bytes = mi.pitch * bpe;
        ml.padded_width = mi.pitch;
        ml.padded_height = mi.height;
        ml.width = (width_pixels + info.block_width - 1) / info.block_width;
        ml.height = (height_pixels + info.block_height - 1) / info.block_height;
        ml.width_pixels = width_pixels;
        ml.height_pixels = height_pixels;
        ml.depth_or_array_size = depth;
        ml.sample_count = in.numSamples;
        ml.swizzle_mode = swizzle;
        ml.slice_depth = volume ? out.blockSlices : 1;
        ml.in_mip_tail = in_tail;
        ml.tail_x = mi.mipTailCoordX;
        ml.tail_y = mi.mipTailCoordY;
        ml.tail_z = mi.mipTailCoordZ;
        ml.alignment = in_tail ? 256 : out.baseAlign;
    }
    return true;
}

// Console resource flags that keep compression metadata away from a texture.
#define CONSOLE_RESOURCE_FLAG_DENY_COLOR_COMPRESSION_DATA 0x100000u
#define CONSOLE_RESOURCE_FLAG_DENY_DEPTH_STENCIL_HTILE    0x80000000u

// A metadata plane: its size and alignment, recorded on the first mip as well, with the swizzle
// mode the metadata itself is laid out in.
static void metadata_plane(ConsolePlaneUsage usage, u64 size, u64 slice_size, u64 alignment, u32 swizzle,
                           ConsolePlaneLayout* p) {
    p->usage = usage;
    p->size = size;
    p->slice_size = slice_size;
    p->base_alignment = alignment;
    p->mips[0].size = size;
    p->mips[0].alignment = (u32)alignment;
    p->mips[0].swizzle_mode = swizzle;
}

// CMask: per-tile fast clear state of a render target. RDNA2 defines it against the fragment mask
// layout (64KB_Z_X), whatever the image's own swizzle.
static bool cmask_plane(const ConsoleTextureDesc& d, const FormatInfo& info, const ConsolePlaneLayout& image,
                        ConsolePlaneLayout* p) {
    ADDR2_COMPUTE_CMASK_INFO_INPUT in = {};
    ADDR2_COMPUTE_CMASK_INFO_OUTPUT out = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.cMaskFlags.pipeAligned = 1;
    in.cMaskFlags.rbAligned = 1;
    in.colorFlags = surface_flags(d, info, false);
    in.resourceType = ADDR_RSRC_TEX_2D;
    in.swizzleMode = ADDR_SW_64KB_Z_X;
    in.unalignedWidth = (u32)d.width;
    in.unalignedHeight = d.height;
    in.numSlices = d.depth_or_array_size;
    in.numMipLevels = mip_count(d);
    in.firstMipIdInTail = image.standard_mip_count;
    if (Addr2ComputeCmaskInfo(g_addrlib, &in, &out) != ADDR_OK)
        return false;
    metadata_plane(CONSOLE_PLANE_COLOR_MASK, out.cmaskBytes, out.sliceSize, out.baseAlign, ADDR_SW_LINEAR, p);
    return true;
}

// FMask: which fragment each sample of an MSAA render target uses.
static bool fmask_plane(const ConsoleTextureDesc& d, ConsolePlaneLayout* p) {
    ADDR2_COMPUTE_FMASK_INFO_INPUT in = {};
    ADDR2_COMPUTE_FMASK_INFO_OUTPUT out = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.swizzleMode = ADDR_SW_64KB_Z_X;
    in.unalignedWidth = (u32)d.width;
    in.unalignedHeight = d.height;
    in.numSlices = d.depth_or_array_size;
    in.numSamples = d.sample_count;
    in.numFrags = d.sample_count;
    if (Addr2ComputeFmaskInfo(g_addrlib, &in, &out) != ADDR_OK)
        return false;
    metadata_plane(CONSOLE_PLANE_FRAGMENT_MASK, out.fmaskBytes, out.sliceSize, out.baseAlign,
                   ADDR_SW_64KB_Z_X, p);
    return true;
}

// DCC: delta color compression keys, laid out in the image's own swizzle.
static bool dcc_plane(const ConsoleTextureDesc& d, const FormatInfo& info, const ConsolePlaneLayout& image,
                      ConsolePlaneLayout* p) {
    ADDR2_COMPUTE_DCCINFO_INPUT in = {};
    ADDR2_COMPUTE_DCCINFO_OUTPUT out = {};
    ADDR2_META_MIP_INFO mip_info[CONSOLE_MAX_MIPS] = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.dccKeyFlags.pipeAligned = 1;
    in.dccKeyFlags.rbAligned = 1;
    in.colorFlags = surface_flags(d, info, false);
    in.resourceType = resource_type(d.dimension);
    in.swizzleMode = (AddrSwizzleMode)image.mips[0].swizzle_mode;
    in.bpp = image.bytes_per_element * 8;
    in.unalignedWidth = (u32)d.width;
    in.unalignedHeight = d.height;
    in.numSlices = d.depth_or_array_size;
    in.numFrags = d.sample_count ? d.sample_count : 1;
    in.numMipLevels = mip_count(d);
    in.dataSurfaceSize = (u32)image.size;
    in.firstMipIdInTail = image.standard_mip_count;
    out.pMipInfo = mip_info;
    if (Addr2ComputeDccInfo(g_addrlib, &in, &out) != ADDR_OK)
        return false;
    // Aligned like the image, one slice's worth per slice.
    u64 alignment = out.dccRamBaseAlign > image.base_alignment ? out.dccRamBaseAlign : image.base_alignment;
    metadata_plane(CONSOLE_PLANE_DCC, out.dccRamSize, out.dccRamSliceSize, alignment, in.swizzleMode, p);
    p->bytes_per_element = 1;
    p->mips[0].size = in.numMipLevels > 1 ? mip_info[0].sliceSize : out.dccRamSliceSize;
    return true;
}

// HTile: depth compression metadata, per 8x8 pixel tile.
static bool htile_plane(const ConsoleTextureDesc& d, const FormatInfo& info, const ConsolePlaneLayout& depth,
                        ConsolePlaneLayout* p) {
    ADDR2_COMPUTE_HTILE_INFO_INPUT in = {};
    ADDR2_COMPUTE_HTILE_INFO_OUTPUT out = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.hTileFlags.pipeAligned = 1;
    in.hTileFlags.rbAligned = 1;
    in.depthFlags = surface_flags(d, info, false);
    in.swizzleMode = (AddrSwizzleMode)depth.mips[0].swizzle_mode;
    in.unalignedWidth = (u32)d.width;
    in.unalignedHeight = d.height;
    in.numSlices = d.depth_or_array_size;
    in.numMipLevels = mip_count(d);
    in.firstMipIdInTail = depth.standard_mip_count;
    if (Addr2ComputeHtileInfo(g_addrlib, &in, &out) != ADDR_OK)
        return false;
    metadata_plane(CONSOLE_PLANE_HTILE, out.htileBytes, 0, out.baseAlign, ADDR_SW_LINEAR, p);
    return true;
}

// Appends a plane after the ones laid out so far, at its alignment.
static void place_plane(ConsoleTextureLayout* out, const ConsolePlaneLayout& plane) {
    ConsolePlaneLayout& p = out->planes[out->plane_count++];
    p = plane;
    p.base_offset = (out->size + plane.base_alignment - 1) & ~(plane.base_alignment - 1);
    out->size = p.base_offset + p.size;
    if (plane.base_alignment > out->base_alignment)
        out->base_alignment = plane.base_alignment;
}

bool console_texture_layout(const ConsoleTextureDesc& d, ConsoleTextureLayout* out) {
    memset(out, 0, sizeof(*out));
    FormatInfo info = format_info(d.format);
    if (!g_addrlib || info.kind == FORMAT_KIND_UNKNOWN || d.width == 0 || d.height == 0)
        return false;
    u32 layout = d.layout;
    if (layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
        layout = CONSOLE_LAYOUT_SWIZZLE | ADDR_SW_LINEAR;
    else if ((layout & ~0xF000u) < CONSOLE_LAYOUT_SWIZZLE)
        layout = console_suggest_layout(d);
    AddrSwizzleMode swizzle = (AddrSwizzleMode)(layout & 0x1F);

    out->mip_levels = mip_count(d);
    out->dimension = d.dimension;
    if (d.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
        // Depth buffers (typeless color formats included): HTile first, then depth, then stencil,
        // each its own surface. Only single-mip depth buffers get HTile, and a depth-only buffer
        // with mips is laid out as a plain texture.
        bool stencil_planes = info.kind == FORMAT_KIND_DEPTH_STENCIL;
        bool single_mip = out->mip_levels == 1;
        ConsolePlaneUsage usage = stencil_planes || single_mip ? CONSOLE_PLANE_DEPTH : CONSOLE_PLANE_DEFAULT;
        ConsolePlaneLayout depth = {}, stencil = {}, htile = {};
        if (!image_plane(d, info, swizzle, false, usage, &depth))
            return false;
        if (single_mip && !(d.flags & CONSOLE_RESOURCE_FLAG_DENY_DEPTH_STENCIL_HTILE)) {
            if (!htile_plane(d, info, depth, &htile))
                return false;
            place_plane(out, htile);
        }
        place_plane(out, depth);
        if (stencil_planes) {
            if (!image_plane(d, info, swizzle, true, CONSOLE_PLANE_STENCIL, &stencil))
                return false;
            place_plane(out, stencil);
        }
        return true;
    }

    ConsolePlaneLayout image = {};
    if (!image_plane(d, info, swizzle, false, CONSOLE_PLANE_DEFAULT, &image))
        return false;
    place_plane(out, image);

    // Render targets get compression metadata unless the title keeps it away: FMask with MSAA,
    // CMask (fast clear) for single-mip targets, except single-sampled ones that use DCC or have
    // 128-bit elements, and DCC where the title allows it.
    bool render_target = (d.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    if (!render_target || (d.flags & CONSOLE_RESOURCE_FLAG_DENY_COLOR_COMPRESSION_DATA))
        return true;
    bool msaa = d.sample_count > 1;
    bool dcc = (d.flags & CONSOLE_RESOURCE_FLAG_ALLOW_DCC) != 0;
    if (msaa) {
        ConsolePlaneLayout fmask = {};
        if (!fmask_plane(d, &fmask))
            return false;
        place_plane(out, fmask);
    }
    if (out->mip_levels == 1 && (msaa || (!dcc && info.bytes_per_element <= 8))) {
        ConsolePlaneLayout cmask = {};
        if (!cmask_plane(d, info, image, &cmask))
            return false;
        place_plane(out, cmask);
    }
    if (dcc) {
        ConsolePlaneLayout keys = {};
        if (!dcc_plane(d, info, image, &keys))
            return false;
        place_plane(out, keys);
    }
    return true;
}

u64 console_texel_offset(const ConsoleTextureDesc& d, const ConsoleTextureLayout& layout, u32 plane, u32 mip,
                         u32 x, u32 y, u32 z_or_slice, u32 sample, u32* bit_position) {
    if (plane >= layout.plane_count || mip >= layout.mip_levels)
        return ~0ull;
    const ConsolePlaneLayout& p = layout.planes[plane];
    FormatInfo info = format_info(d.format);
    bool stencil = p.usage == CONSOLE_PLANE_STENCIL;
    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT in = {};
    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT out = {};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.x = x;
    in.y = y;
    in.slice = z_or_slice;
    in.sample = sample;
    in.mipId = mip;
    in.swizzleMode = (AddrSwizzleMode)p.mips[0].swizzle_mode;
    in.flags = surface_flags(d, info, stencil);
    in.resourceType = resource_type(d.dimension);
    in.bpp = p.bytes_per_element * 8;
    in.unalignedWidth = (u32)((d.width + info.block_width - 1) / info.block_width);
    in.unalignedHeight = (d.height + info.block_height - 1) / info.block_height;
    in.numSlices = d.depth_or_array_size;
    in.numMipLevels = layout.mip_levels;
    in.numSamples = d.sample_count ? d.sample_count : 1;
    in.numFrags = in.numSamples;
    if (Addr2ComputeSurfaceAddrFromCoord(g_addrlib, &in, &out) != ADDR_OK)
        return ~0ull;
    if (bit_position)
        *bit_position = out.bitPosition;
    return p.base_offset + out.addr;
}

bool console_copy_subresource(const ConsoleTextureDesc& d, const ConsoleTextureLayout& layout, u32 plane,
                              u32 mip, u32 slice, void* texture, void* linear, u64 row_pitch, u64 slice_pitch,
                              bool into_texture) {
    if (plane >= layout.plane_count || mip >= layout.mip_levels)
        return false;
    const ConsolePlaneLayout& p = layout.planes[plane];
    if (p.bytes_per_element == 0)
        return false; // metadata planes have no texels
    const ConsoleMipLayout& m = p.mips[mip];
    FormatInfo info = format_info(d.format);
    bool stencil = p.usage == CONSOLE_PLANE_STENCIL;
    bool volume = d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    if (!volume && slice >= d.depth_or_array_size)
        return false;

    ADDR2_COPY_MEMSURFACE_INPUT in = {};
    in.size = sizeof(in);
    in.swizzleMode = (AddrSwizzleMode)m.swizzle_mode;
    in.format = stencil ? ADDR_FMT_8 : addr_format(d.format, info);
    in.flags = surface_flags(d, info, stencil);
    in.resourceType = resource_type(d.dimension);
    in.bpp = p.bytes_per_element * 8;
    in.unAlignedDims.width = (u32)d.width;
    in.unAlignedDims.height = d.height;
    in.unAlignedDims.depth = d.depth_or_array_size;
    in.numMipLevels = layout.mip_levels;
    in.numSamples = d.sample_count ? d.sample_count : 1;
    in.pMappedSurface = (u8*)texture + p.base_offset;

    ADDR2_COPY_MEMSURFACE_REGION region = {};
    region.size = sizeof(region);
    region.slice = volume ? 0 : slice;
    region.mipId = mip;
    region.copyDims.width = m.width;
    region.copyDims.height = m.height;
    region.copyDims.depth = volume ? m.depth_or_array_size : 1;
    region.pMem = linear;
    region.memRowPitch = row_pitch;
    region.memSlicePitch = slice_pitch;
    ADDR_E_RETURNCODE r = into_texture ? Addr2CopyMemToSurface(g_addrlib, &in, &region, 1)
                                       : Addr2CopySurfaceToMem(g_addrlib, &in, &region, 1);
    return r == ADDR_OK;
}
