// Console shader containers: the shader bytecode titles pass in pipeline state streams.
//
// The container is a DXBC file whose DXIL part carries, compressed, the console compiler's shader
// object: machine code plus the hardware register values and link-time patches the console runtime
// applies when it makes a pipeline. Pixel shaders leave their color exports as patch points, with
// one code variant per export format.

#pragma once

#include "common/types.h"
#include "shader/translate.h"

// A vertex shader input from the container's input signature.
struct ContainerInput {
    const char* semantic; // in the container, NUL terminated
    u32 semantic_index;
    u8 reg;  // input register
    u8 mask; // components the shader reads
};

#define CONTAINER_MAX_INPUTS 32

struct ConsoleContainerShader {
    u8* object; // the decompressed shader object (owned: console_container_close)
    u32 object_size;
    u8* code; // machine code, inside `object`; pixel shader exports patched
    u32 code_size;
    u8 user_sgprs;
    u32 ps_input_addr;   // pixel shaders: SPI_PS_INPUT_ADDR layout of the input VGPRs
    u32 ps_color_format; // pixel shaders: export format per render target (4 bits each)
    // Vertex shaders: the inputs the input layout feeds (system values left out).
    u8 input_count;
    ContainerInput inputs[CONTAINER_MAX_INPUTS];
};

// Where the driver's fetch subroutine leaves vertex shader inputs: four VGPRs per input register,
// after the system values (vertex ID, two others, instance ID).
#define CONTAINER_FETCH_FIRST_VGPR 4

// Opens a container for `stage` (vertex or pixel). Pixel shader exports are patched to export all
// four components as 32-bit values, which desktop render target writes convert. False, with the
// reason in `error`, if the container is not in a known form.
bool console_container_open(const void* bytecode, usize size, ShaderStage stage, ConsoleContainerShader* out,
                            const char** error);
void console_container_close(ConsoleContainerShader* shader);
