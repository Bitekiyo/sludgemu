// Translated console shader programs: translation to HLSL and compilation to DXIL on worker
// threads, cached by program code and translation inputs.
//
// Programs are requested when pipelines are created and built in the background; draws using a
// pipeline whose programs are not ready yet are skipped. The workers also run other GPU jobs that
// must wait for programs (desktop pipeline creation).

#pragma once

#include "common/types.h"
#include "shader/translate.h"

enum ShaderProgramState : u32 {
    PROGRAM_PENDING,
    PROGRAM_READY,
    PROGRAM_FAILED,
};

struct ShaderProgram {
    volatile u32 state; // ShaderProgramState; READY/FAILED are final
    ShaderStage stage;
    u64 key;

    // Ready programs: DXIL and what the pipeline needs about the program.
    void* dxil;
    usize dxil_size;
    u32 vertex_element_mask;
    u32 element_format[SHADER_MAX_VERTEX_ELEMENTS];
    u32 ps_target_mask;

    // The translation inputs, kept until the program is built.
    u8* code;
    u32 code_size;
    ShaderInfo info;
};

// Starts the workers. Returns false if the shader compiler is missing.
bool shader_programs_init();

// The program for `code` translated with `info`: an existing one, or a new one queued for building.
// Programs live until shutdown. Returns nullptr only when out of memory.
ShaderProgram* shader_program_get(const void* code, u32 code_size, const ShaderInfo& info);

// Runs `job(context)` on a worker thread.
void gpu_job_queue(void (*job)(void* context), void* context);
