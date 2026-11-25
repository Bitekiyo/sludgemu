// Console shader translation: RDNA 2 machine code to HLSL for the desktop compiler.
//
// The model, in short:
//
// - Every register becomes a 32-bit HLSL variable of the same name. Each desktop thread runs the
//   console program for one console lane; scalar registers hold the same value in every thread.
// - Lane masks (EXEC, VCC, compare results) hold, in each thread, all ones or zero in both halves:
//   that lane's bit, broadcast. Bitwise mask operations then work unchanged, and the operations
//   that look at the whole mask (branches on EXECZ/VCCZ, bit counts) use wave intrinsics.
// - Scalar branches are uniform, so desktop waves stay converged; vector instructions only write
//   lanes whose EXEC bit is set.
// - User data (the console's user SGPRs) comes from a desktop root constant block holding the same
//   values, except that descriptor tables hold byte offsets into the desktop descriptor heaps:
//   console code computes descriptor addresses with its own arithmetic, and the address of a loaded
//   descriptor, divided by the slot size, is its desktop heap index. Registers loaded from the heap
//   hold the byte address of each dword instead of descriptor bits.
// - Root descriptors (buffer addresses in user data) become desktop root SRVs/UAVs read as raw
//   buffers; registers holding their high address dword are tracked statically.
// - Vertex fetches (loads through the vertex buffer table) become desktop input assembler inputs.

#pragma once

#include "common/types.h"
#include "shader/text.h"

enum ShaderStage : u8 {
    SHADER_VERTEX,
    SHADER_PIXEL,
    SHADER_COMPUTE,
};

// What a root parameter places in user SGPRs.
enum UserDataKind : u8 {
    USER_DATA_NONE,
    USER_DATA_RESOURCE_TABLE, // byte offset of the table in the desktop CBV/SRV/UAV heap
    USER_DATA_SAMPLER_TABLE,  // byte offset of the table in the desktop sampler heap
    USER_DATA_CONSTANTS,
    USER_DATA_ROOT_CBV, // root descriptors: 2 SGPRs, read through desktop root SRVs/UAVs
    USER_DATA_ROOT_SRV,
    USER_DATA_ROOT_UAV,
};

// The kind of descriptor a resource table range holds.
enum TableRangeKind : u8 {
    RANGE_SRV,
    RANGE_UAV,
    RANGE_CBV,
};

struct TableRange {
    TableRangeKind kind;
    u32 offset; // in descriptors from the table start
    u32 count;  // UINT32_MAX: unbounded
};

#define SHADER_MAX_ROOT_PARAMETERS 64
#define SHADER_MAX_TABLE_RANGES    16
#define SHADER_MAX_VERTEX_ELEMENTS 32
#define NO_HEAP_SGPR               0xFF

struct UserDataParameter {
    UserDataKind kind;
    u8 sgpr;          // first user SGPR
    u8 dwords;        // SGPRs used
    u8 root_register; // root descriptors: desktop register (t or u) in the translated shader's space
    u8 range_count;   // resource tables: the table's ranges
    TableRange ranges[SHADER_MAX_TABLE_RANGES];
};

struct VertexElement {
    u8 slot;
    u32 offset; // byte offset in the vertex
    u32 format; // DXGI_FORMAT
};

// Everything about the program that is not in its code.
struct ShaderInfo {
    ShaderStage stage;
    u8 user_sgprs; // user SGPR count; system SGPRs follow
    u8 parameter_count;
    UserDataParameter parameters[SHADER_MAX_ROOT_PARAMETERS];
    // Directly indexed heaps: the user SGPR holding the heap's base (the translated program's value
    // is 0, the start of the desktop heap), or NO_HEAP_SGPR.
    u8 resource_heap_sgpr;
    u8 sampler_heap_sgpr;

    // Vertex shaders: the input layout; fetches are matched to elements by slot and offset.
    u8 element_count;
    VertexElement elements[SHADER_MAX_VERTEX_ELEMENTS];

    // Pixel shaders: which input VGPRs exist (SPI_PS_INPUT_ADDR layout) and the export formats.
    u32 ps_input_addr;
    u32 ps_color_format; // SPI_SHADER_COL_FORMAT: 4 bits per render target

    // Compute shaders.
    u16 threads[3];
    u8 group_id_mask;   // which of x/y/z workgroup IDs follow the user SGPRs
    u8 thread_id_count; // thread ID VGPRs: 1..3
    u32 lds_bytes;
};

// The translation's result: HLSL source plus what the pipeline needs to know about it.
struct TranslatedShader {
    Text hlsl;
    u32 vertex_element_mask; // vertex shaders: input layout elements the shader reads (by index)
    u32 vs_param_mask;       // vertex shaders: exported parameters
    u32 ps_attribute_mask;   // pixel shaders: attributes read
    u32 ps_target_mask;      // pixel shaders: render targets written
    bool ps_writes_depth;
    bool ps_discards;
    // Vertex shaders: the desktop input format of each element read (raw fetches read dwords).
    u32 element_format[SHADER_MAX_VERTEX_ELEMENTS];
    char error[160]; // why translation failed
};

// Translates `code` (RDNA 2 instructions, `size` bytes). Returns false with `out->error` set if the
// program uses something the translator does not handle.
bool shader_translate(const void* code, u32 size, const ShaderInfo& info, TranslatedShader* out);
