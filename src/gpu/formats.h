#pragma once

#include <windows.h>
#include <dxgiformat.h>

#include "common/types.h"

// Storage properties of formats in the console's numbering: the DXGI formats plus console-only
// ones, some of which reuse values desktop DXGI later gave other formats.

// Console-only formats.
#define CONSOLE_FORMAT_R10G10B10_7E3_A2_FLOAT   ((DXGI_FORMAT)116)
#define CONSOLE_FORMAT_R10G10B10_6E4_A2_FLOAT   ((DXGI_FORMAT)117)
#define CONSOLE_FORMAT_D16_UNORM_S8_UINT        ((DXGI_FORMAT)118)
#define CONSOLE_FORMAT_R16_UNORM_X8_TYPELESS    ((DXGI_FORMAT)119)
#define CONSOLE_FORMAT_X16_TYPELESS_G8_UINT     ((DXGI_FORMAT)120)
#define CONSOLE_FORMAT_R10G10B10_SNORM_A2_UNORM ((DXGI_FORMAT)189)
#define CONSOLE_FORMAT_R4G4_UNORM               ((DXGI_FORMAT)190)
#define CONSOLE_FORMAT_R24G8_UNORM              ((DXGI_FORMAT)191)

enum FormatKind : u8 {
    FORMAT_KIND_UNKNOWN,
    FORMAT_KIND_COLOR,
    FORMAT_KIND_BLOCK_COMPRESSED, // 4x4 texel blocks
    FORMAT_KIND_DEPTH,            // depth only
    FORMAT_KIND_DEPTH_STENCIL,    // depth and stencil, stored as separate planes on the GPU
    FORMAT_KIND_STENCIL,          // stencil only (a view of a depth-stencil format)
    FORMAT_KIND_PACKED_422,       // two texels per element
    FORMAT_KIND_VIDEO,            // planar YUV
};

struct FormatInfo {
    FormatKind kind;
    u8 bytes_per_element; // of the first plane; a block for compressed formats
    u8 block_width;       // texels per element horizontally (4 for BC, 2 for packed 4:2:2)
    u8 block_height;
};

// Properties of `format`; kind is FORMAT_KIND_UNKNOWN for formats with no storage.
FormatInfo format_info(DXGI_FORMAT format);
