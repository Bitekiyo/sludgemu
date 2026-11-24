#include "loader/stubs.h"

#include <windows.h>
#include <intrin.h>
#include <stdio.h>

#include "common/log.h"
#include "common/str.h"
#include "loader/crash.h"

#define MAX_STUBS 8192
#define STUB_CODE_SIZE 32
#define STUB_LOG_LIMIT 8

struct StubInfo {
    char module[64];
    char name[128];
    volatile long calls;
};

static StubInfo g_infos[MAX_STUBS];
static u8* g_code = nullptr;
static u32 g_count = 0;
static volatile long g_hit = 0;
static bool g_break_on_call = false;
static SRWLOCK g_lock = SRWLOCK_INIT;

// Reached by a jmp from the thunk, so our return address is the title's call site.
// RCX was overwritten with the StubInfo pointer; the title's first argument is lost, which is fine
// for a function that only logs.
static u64 stub_handler(StubInfo* info) {
    void* caller = _ReturnAddress();
    long n = InterlockedIncrement(&info->calls);
    if (n == 1)
        InterlockedIncrement(&g_hit);
    if (n <= STUB_LOG_LIMIT) {
        char where[160];
        describe_address(caller, where, sizeof(where));
        LOG_W("stub", "unimplemented %s!%s called from %s%s", info->module, info->name, where,
              n == STUB_LOG_LIMIT ? " (further calls not logged)" : "");
    }
    if (g_break_on_call && IsDebuggerPresent())
        __debugbreak();
    return 0;
}

void stubs_init(bool break_on_call) {
    g_break_on_call = break_on_call;
    g_code = (u8*)VirtualAlloc(nullptr, MAX_STUBS * STUB_CODE_SIZE, MEM_RESERVE | MEM_COMMIT,
                               PAGE_EXECUTE_READWRITE);
    if (!g_code)
        FATAL("cannot allocate stub code pool");
}

void* stub_create(const char* module, const char* name, u16 ordinal) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_count >= MAX_STUBS) {
        ReleaseSRWLockExclusive(&g_lock);
        FATAL("out of import stubs");
    }
    u32 index = g_count++;
    ReleaseSRWLockExclusive(&g_lock);

    StubInfo* info = &g_infos[index];
    str_copy(info->module, sizeof(info->module), module);
    if (name)
        str_copy(info->name, sizeof(info->name), name);
    else
        snprintf(info->name, sizeof(info->name), "#%u", ordinal);

    // mov rcx, imm64 ; mov rax, imm64 ; jmp rax
    u8* c = g_code + (usize)index * STUB_CODE_SIZE;
    u64 info_ptr = (u64)info;
    u64 handler = (u64)&stub_handler;
    c[0] = 0x48;
    c[1] = 0xB9;
    memcpy(c + 2, &info_ptr, 8);
    c[10] = 0x48;
    c[11] = 0xB8;
    memcpy(c + 12, &handler, 8);
    c[20] = 0xFF;
    c[21] = 0xE0;
    for (u32 i = 22; i < STUB_CODE_SIZE; i++)
        c[i] = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), c, STUB_CODE_SIZE);

    LOG_D("stub", "created stub for %s!%s", info->module, info->name);
    return c;
}

u32 stubs_hit_count() { return (u32)g_hit; }
