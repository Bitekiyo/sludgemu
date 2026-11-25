#include "gpu/shader_programs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <ole2.h> // dxcapi.h needs COM types the lean Windows headers leave out
#include <dxcapi.h>

#include "common/log.h"
#include "common/thread_slot.h"
#include "shader/rdna.h"

// --- jobs --------------------------------------------------------------------------------------------

// A job is a program to build or a function to run.
struct Job {
    ShaderProgram* program;
    void (*run)(void* context);
    void* context;
};

#define JOB_CAPACITY 65536

static Job* g_jobs;
static u32 g_job_head, g_job_count;
static SRWLOCK g_job_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_job_ready = CONDITION_VARIABLE_INIT;
static bool g_ready;

static void queue(const Job& job) {
    AcquireSRWLockExclusive(&g_job_lock);
    if (g_job_count == JOB_CAPACITY) {
        ReleaseSRWLockExclusive(&g_job_lock);
        LOG_E("gpu", "GPU job queue full; job dropped");
        return;
    }
    g_jobs[(g_job_head + g_job_count) % JOB_CAPACITY] = job;
    g_job_count++;
    ReleaseSRWLockExclusive(&g_job_lock);
    WakeConditionVariable(&g_job_ready);
}

void gpu_job_queue(void (*run)(void* context), void* context) {
    queue({nullptr, run, context});
}

// --- compiler ----------------------------------------------------------------------------------------

static DxcCreateInstanceProc g_dxc_create;

// Each worker has its own compiler: DXC compiler objects are not thread safe.
struct Worker {
    IDxcCompiler3* compiler;
    u32 index;
};

static const wchar_t* profile_of(ShaderStage stage) {
    switch (stage) {
    case SHADER_VERTEX:
        return L"vs_6_6";
    case SHADER_PIXEL:
        return L"ps_6_6";
    default:
        return L"cs_6_6";
    }
}

// Programs (code and HLSL, with the error for failures) are written to SLUDGE_SHADER_DUMP=<directory>.
static void dump(const ShaderProgram* p, const char* hlsl, const char* reason) {
    char dir[MAX_PATH];
    if (!GetEnvironmentVariableA("SLUDGE_SHADER_DUMP", dir, sizeof(dir)))
        return;
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%s\\%016llx.bin", dir, (unsigned long long)p->key);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(p->code, 1, p->code_size, f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s\\%016llx.hlsl", dir, (unsigned long long)p->key);
    f = fopen(path, "w");
    if (f) {
        if (reason)
            fprintf(f, "// %s\n", reason);
        if (hlsl)
            fputs(hlsl, f);
        fclose(f);
    }
}

static void build_program(Worker& w, ShaderProgram* p) {
    TranslatedShader t = {};
    u32 state = PROGRAM_FAILED;
    if (!shader_translate(p->code, p->code_size, p->info, &t)) {
        LOG_W("gpu", "shader %016llx: translation failed: %s", (unsigned long long)p->key, t.error);
        dump(p, t.hlsl.data, t.error);
    } else {
        DxcBuffer source = {t.hlsl.data, t.hlsl.length, DXC_CP_UTF8};
        const wchar_t* args[] = {L"-E",
                                 L"main",
                                 L"-T",
                                 profile_of(p->stage),
                                 L"-O3",
                                 L"-HV",
                                 L"2021",
                                 L"-Qstrip_debug",
                                 L"-Qstrip_reflect"};
        IDxcResult* result = nullptr;
        HRESULT status = E_FAIL;
        if (SUCCEEDED(w.compiler->Compile(&source, args, ARRAY_COUNT(args), nullptr, IID_PPV_ARGS(&result))))
            result->GetStatus(&status);
        IDxcBlob* object = nullptr;
        if (SUCCEEDED(status))
            result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
        if (object && object->GetBufferSize()) {
            p->dxil = malloc(object->GetBufferSize());
            if (p->dxil) {
                memcpy(p->dxil, object->GetBufferPointer(), object->GetBufferSize());
                p->dxil_size = object->GetBufferSize();
                p->vertex_element_mask = t.vertex_element_mask;
                memcpy(p->element_format, t.element_format, sizeof(p->element_format));
                p->ps_target_mask = t.ps_target_mask;
                state = PROGRAM_READY;
                dump(p, t.hlsl.data, nullptr);
            }
        } else {
            IDxcBlobUtf8* errors = nullptr;
            if (result)
                result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
            const char* text =
                errors && errors->GetStringLength() ? errors->GetStringPointer() : "unknown error";
            LOG_W("gpu", "shader %016llx: compiling the translation failed: %.300s",
                  (unsigned long long)p->key, text);
            dump(p, t.hlsl.data, text);
            if (errors)
                errors->Release();
        }
        if (object)
            object->Release();
        if (result)
            result->Release();
    }
    t.hlsl.release();
    free(p->code);
    p->code = nullptr;
    InterlockedExchange((volatile LONG*)&p->state, (LONG)state);
}

static DWORD WINAPI worker_main(void* param) {
    Worker w = {};
    w.index = (u32)(usize)param;
    if (FAILED(g_dxc_create(CLSID_DxcCompiler, IID_PPV_ARGS(&w.compiler)))) {
        LOG_E("gpu", "shader worker %u: cannot create a compiler", w.index);
        return 1;
    }
    for (;;) {
        AcquireSRWLockExclusive(&g_job_lock);
        while (g_job_count == 0)
            SleepConditionVariableSRW(&g_job_ready, &g_job_lock, INFINITE, 0);
        Job job = g_jobs[g_job_head];
        g_job_head = (g_job_head + 1) % JOB_CAPACITY;
        g_job_count--;
        ReleaseSRWLockExclusive(&g_job_lock);
        if (job.program)
            build_program(w, job.program);
        else
            job.run(job.context);
    }
}

// --- the program table -------------------------------------------------------------------------------

#define PROGRAM_TABLE_SIZE 65536 // power of two

static ShaderProgram** g_programs;
static SRWLOCK g_program_lock = SRWLOCK_INIT;
static u32 g_program_count;

static u64 hash_bytes(const void* data, usize size, u64 h) {
    const u8* p = (const u8*)data;
    for (usize i = 0; i < size; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

bool shader_programs_init() {
    rdna_init();
    HMODULE dxc = LoadLibraryW(L"dxcompiler.dll");
    g_dxc_create = dxc ? (DxcCreateInstanceProc)GetProcAddress(dxc, "DxcCreateInstance") : nullptr;
    if (!g_dxc_create) {
        LOG_E("gpu", "dxcompiler.dll is missing: shaders cannot be translated");
        return false;
    }
    g_jobs = (Job*)calloc(JOB_CAPACITY, sizeof(Job));
    g_programs = (ShaderProgram**)calloc(PROGRAM_TABLE_SIZE, sizeof(ShaderProgram*));
    if (!g_jobs || !g_programs)
        return false;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    u32 workers = si.dwNumberOfProcessors > 4 ? si.dwNumberOfProcessors - 2 : 2;
    if (workers > 8)
        workers = 8;
    for (u32 i = 0; i < workers; i++) {
        HANDLE thread = CreateThread(nullptr, 1 << 20, worker_main, (void*)(usize)i, 0, nullptr);
        if (!thread)
            return false;
        SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
        SetThreadDescription(thread, L"sludgemu shader worker");
        CloseHandle(thread);
    }
    g_ready = true;
    LOG_I("gpu", "shader translation: %u workers", workers);
    return true;
}

ShaderProgram* shader_program_get(const void* code, u32 code_size, const ShaderInfo& info) {
    if (!g_ready)
        return nullptr;
    u64 key = hash_bytes(code, code_size, 0xcbf29ce484222325ull);
    key = hash_bytes(&info, sizeof(info), key);
    u32 slot = (u32)key & (PROGRAM_TABLE_SIZE - 1);
    AcquireSRWLockExclusive(&g_program_lock);
    while (g_programs[slot]) {
        if (g_programs[slot]->key == key) {
            ShaderProgram* found = g_programs[slot];
            ReleaseSRWLockExclusive(&g_program_lock);
            return found;
        }
        slot = (slot + 1) & (PROGRAM_TABLE_SIZE - 1);
    }
    ShaderProgram* p = nullptr;
    if (g_program_count < PROGRAM_TABLE_SIZE / 2) {
        p = (ShaderProgram*)calloc(1, sizeof(ShaderProgram));
        u8* copy = p ? (u8*)malloc(code_size) : nullptr;
        if (p && copy) {
            memcpy(copy, code, code_size);
            p->code = copy;
            p->code_size = code_size;
            p->info = info;
            p->stage = info.stage;
            p->key = key;
            g_programs[slot] = p;
            g_program_count++;
        } else {
            free(p);
            p = nullptr;
        }
    } else {
        LOG_W_ONCE("gpu", "shader program table full; more programs are not translated");
    }
    ReleaseSRWLockExclusive(&g_program_lock);
    if (p)
        queue({p, nullptr, nullptr});
    return p;
}
