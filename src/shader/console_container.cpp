#include "shader/console_container.h"

#include <stdlib.h>
#include <string.h>

#include <libdeflate.h>

// The shader object, after decompression. Version 0x101 lays out:
//   +0x00 "FSSM", +0x04 version, +0x08 size
//   +0xC0 code offset (from +0x10), +0xC8 code size
//   vertex shaders: +0x2A8 SPI_SHADER_PGM_RSRC2 of the hardware vertex stage
//   pixel shaders:  +0x23C SPI_SHADER_PGM_RSRC2, +0x248 SPI_PS_INPUT_ADDR, +0x250 CB_SHADER_MASK,
//                   +0x25C export patch size (dwords), +0x260 the export patches: per exported
//                   render target, the patch's code offset (dwords), then one variant per export
//                   format (SPI_SHADER_COL_FORMAT order), each EXPORT_VARIANT_DWORDS long.
#define OBJECT_MAGIC          0x4D535346u // "FSSM"
#define OBJECT_VERSION        0x101u
#define OBJECT_CODE_OFFSET    0xC0
#define OBJECT_CODE_SIZE      0xC8
#define OBJECT_CODE_BASE      0x10
#define VS_RSRC2              0x2A8
#define PS_RSRC2              0x23C
#define PS_INPUT_ADDR         0x248
#define PS_TARGET_MASK        0x250
#define PS_PATCH_DWORDS       0x25C
#define PS_PATCHES            0x260
#define EXPORT_FORMATS        10
#define EXPORT_VARIANT_DWORDS 10
#define EXPORT_32_ABGR        9

static u32 read_u32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

// The DXIL part of a DXBC container, or nullptr.
static const u8* dxil_part(const u8* b, usize size, u32* part_size) {
    if (size < 32 || read_u32(b) != 0x43425844u) // "DXBC"
        return nullptr;
    u32 count = read_u32(b + 28);
    if (count > 64 || 32 + (usize)count * 4 > size)
        return nullptr;
    for (u32 i = 0; i < count; i++) {
        u32 at = read_u32(b + 32 + i * 4);
        if ((usize)at + 8 > size)
            return nullptr;
        u32 n = read_u32(b + at + 4);
        if ((usize)at + 8 + n > size)
            return nullptr;
        if (read_u32(b + at) == 0x4C495844u) { // "DXIL"
            *part_size = n;
            return b + at + 8;
        }
    }
    return nullptr;
}

// The compressed shader object inside the DXIL part: "XBCH", version, compressed size, 0,
// decompressed size, 0, then zlib data.
static const u8* find_object(const u8* dxil, u32 size, u32* compressed, u32* decompressed) {
    for (u32 i = 0; i + 24 <= size; i += 4) {
        if (read_u32(dxil + i) != 0x48434258u) // "XBCH"
            continue;
        *compressed = read_u32(dxil + i + 8);
        *decompressed = read_u32(dxil + i + 16);
        if ((u64)i + 24 + *compressed > size)
            return nullptr;
        return dxil + i + 24;
    }
    return nullptr;
}

bool console_container_open(const void* bytecode, usize size, ShaderStage stage, ConsoleContainerShader* out,
                            const char** error) {
    memset(out, 0, sizeof(*out));
    u32 dxil_size = 0;
    const u8* dxil = bytecode ? dxil_part((const u8*)bytecode, size, &dxil_size) : nullptr;
    if (!dxil) {
        *error = "not a shader container with a DXIL part";
        return false;
    }
    u32 compressed = 0, decompressed = 0;
    const u8* packed = find_object(dxil, dxil_size, &compressed, &decompressed);
    if (!packed || decompressed < 0x308 || decompressed > (64u << 20)) {
        *error = "no console shader object in the container";
        return false;
    }
    u8* object = (u8*)malloc(decompressed);
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    size_t actual = 0;
    bool inflated = object && d &&
                    libdeflate_zlib_decompress(d, packed, compressed, object, decompressed, &actual) ==
                        LIBDEFLATE_SUCCESS &&
                    actual == decompressed;
    if (d)
        libdeflate_free_decompressor(d);
    if (!inflated) {
        free(object);
        *error = "the console shader object does not decompress";
        return false;
    }
    out->object = object;
    out->object_size = decompressed;
    if (read_u32(object) != OBJECT_MAGIC || read_u32(object + 4) != OBJECT_VERSION) {
        *error = "console shader object of an unknown version";
        console_container_close(out);
        return false;
    }
    u32 code_at = read_u32(object + OBJECT_CODE_OFFSET) + OBJECT_CODE_BASE;
    u32 code_size = read_u32(object + OBJECT_CODE_SIZE);
    if (code_at > decompressed || code_size > decompressed - code_at || code_size % 4) {
        *error = "console shader object with code out of bounds";
        console_container_close(out);
        return false;
    }
    out->code = object + code_at;
    out->code_size = code_size;

    if (stage == SHADER_VERTEX) {
        out->user_sgprs = (u8)((read_u32(object + VS_RSRC2) >> 1) & 31);
        return true;
    }
    if (stage != SHADER_PIXEL) {
        *error = "containers are read for vertex and pixel shaders only";
        console_container_close(out);
        return false;
    }
    out->user_sgprs = (u8)((read_u32(object + PS_RSRC2) >> 1) & 31);
    out->ps_input_addr = read_u32(object + PS_INPUT_ADDR);
    u32 target_mask = read_u32(object + PS_TARGET_MASK);
    u32 patch_dwords = read_u32(object + PS_PATCH_DWORDS);
    if (patch_dwords > EXPORT_VARIANT_DWORDS) {
        *error = "export patches longer than their variants";
        console_container_close(out);
        return false;
    }
    // One patch per render target written, in target order; each gets the 32-bit RGBA variant.
    const u32 patch_bytes = 4 + EXPORT_FORMATS * EXPORT_VARIANT_DWORDS * 4;
    u32 at = PS_PATCHES;
    for (u32 target = 0; target < 8; target++) {
        if (!((target_mask >> (target * 4)) & 0xF))
            continue;
        if ((u64)at + patch_bytes > decompressed) {
            *error = "export patches out of bounds";
            console_container_close(out);
            return false;
        }
        u32 offset = read_u32(object + at) * 4;
        if (offset > code_size || patch_dwords * 4 > code_size - offset) {
            *error = "export patch outside the code";
            console_container_close(out);
            return false;
        }
        const u8* variant = object + at + 4 + EXPORT_32_ABGR * EXPORT_VARIANT_DWORDS * 4;
        memcpy(out->code + offset, variant, patch_dwords * 4);
        out->ps_color_format |= (u32)EXPORT_32_ABGR << (target * 4);
        at += patch_bytes;
    }
    return true;
}

void console_container_close(ConsoleContainerShader* shader) {
    free(shader->object);
    memset(shader, 0, sizeof(*shader));
}
