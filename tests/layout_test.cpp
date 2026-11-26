// Texture addressing for shaders (console_subresource_addressing) against the layout library's own
// addressing, across formats, swizzle modes, sizes, mips, array slices and volumes.

#include <windows.h>
#include <d3d12.h>
#include <stdio.h>

#include "addrinterface.h"
#include "gpu/console_layout.h"
#include "test.h"

static u32 g_compared = 0;

// Every element of small subresources, a spread of elements of large ones.
static void check_texture(const ConsoleTextureDesc& d) {
    ConsoleTextureLayout layout;
    if (!console_texture_layout(d, &layout)) {
        printf("FAIL layout: format %d %llux%ux%u layout 0x%x\n", d.format, d.width, d.height,
               d.depth_or_array_size, d.layout);
        g_failures++;
        return;
    }
    bool volume = d.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    u32 slices = volume ? 1 : d.depth_or_array_size;
    for (u32 slice = 0; slice < slices; slice++) {
        for (u32 mip = 0; mip < layout.mip_levels; mip++) {
            ConsoleSubresourceAddressing a;
            if (!console_subresource_addressing(d, layout, 0, mip, slice, &a, true)) {
                printf("FAIL addressing: format %d %llux%ux%u layout 0x%x mip %u slice %u\n", d.format,
                       d.width, d.height, d.depth_or_array_size, d.layout, mip, slice);
                g_failures++;
                continue;
            }
            u64 elements = (u64)a.width * a.height * a.depth;
            u64 step = elements <= 8192 ? 1 : elements / 4096;
            for (u64 e = 0; e < elements; e += step) {
                u32 x = (u32)(e % a.width), y = (u32)(e / a.width % a.height),
                    z = (u32)(e / a.width / a.height);
                u64 expect = console_texel_offset(d, layout, 0, mip, x, y, volume ? z : slice, 0, nullptr);
                u64 got = console_subresource_offset(a, x, y, z);
                g_compared++;
                if (expect != got) {
                    printf(
                        "FAIL offset: format %d %llux%ux%u layout 0x%x mip %u slice %u (%u,%u,%u): 0x%llx, "
                        "expected 0x%llx\n",
                        d.format, d.width, d.height, d.depth_or_array_size, d.layout, mip, slice, x, y, z,
                        got, expect);
                    g_failures++;
                    return;
                }
            }
        }
    }
}

void test_layout_addressing() {
    if (!console_layout_init(CONSOLE_SERIES_X)) {
        printf("FAIL console_layout_init\
");
        g_failures++;
        return;
    }
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8_UNORM,           DXGI_FORMAT_R8G8_UNORM,
                                   DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_BC1_UNORM,
                                   DXGI_FORMAT_BC3_UNORM,          DXGI_FORMAT_BC7_UNORM};
    const u32 modes[] = {ADDR_SW_LINEAR,   ADDR_SW_4KB_S,    ADDR_SW_4KB_S_X, ADDR_SW_64KB_S,
                         ADDR_SW_64KB_S_X, ADDR_SW_64KB_R_X, ADDR_SW_64KB_D_X};
    const u32 sizes[][2] = {{1, 1}, {7, 5}, {64, 64}, {300, 200}, {1024, 512}, {4096, 4096}};
    for (DXGI_FORMAT format : formats) {
        for (u32 mode : modes) {
            for (u32 si = 0; si < ARRAY_COUNT(sizes); si++) {
                for (u32 arrays = 1; arrays <= 3; arrays += 2) {
                    ConsoleTextureDesc d = {};
                    d.dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    d.width = sizes[si][0];
                    d.height = sizes[si][1];
                    d.depth_or_array_size = arrays;
                    d.mip_levels = 0;
                    d.format = format;
                    d.sample_count = 1;
                    d.layout = CONSOLE_LAYOUT_SWIZZLE | mode;
                    if (mode == ADDR_SW_64KB_R_X)
                        d.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                    bool compressed =
                        format >= DXGI_FORMAT_BC1_TYPELESS && format <= DXGI_FORMAT_BC7_UNORM_SRGB;
                    if (compressed && d.flags) // block compressed formats can't be render targets
                        continue;
                    check_texture(d);
                }
            }
        }
    }
    // Volumes: thick and thin swizzles. (The layout library has no addressing for 64 KiB display
    // swizzled volumes of 8 and 16-byte elements, so there is nothing to compare those with.)
    const u32 volume_modes[] = {ADDR_SW_LINEAR, ADDR_SW_4KB_S, ADDR_SW_64KB_S, ADDR_SW_64KB_S_X};
    const u32 volume_sizes[][3] = {{8, 8, 8}, {64, 32, 16}, {33, 17, 9}};
    for (DXGI_FORMAT format : formats) {
        for (u32 mode : volume_modes) {
            for (u32 si = 0; si < ARRAY_COUNT(volume_sizes); si++) {
                ConsoleTextureDesc d = {};
                d.dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
                d.width = volume_sizes[si][0];
                d.height = volume_sizes[si][1];
                d.depth_or_array_size = volume_sizes[si][2];
                d.format = format;
                d.sample_count = 1;
                d.layout = CONSOLE_LAYOUT_SWIZZLE | mode;
                check_texture(d);
            }
        }
    }
    printf("texture addressing: %u elements compared\n", g_compared);
}
