#include "loader/crash.h"

#include <windows.h>
#include <winternl.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/log.h"
#include "common/str.h"
#include "loader/loader.h"

#define FIRST_CHANCE_LOG_LIMIT 64

static u32 capture_frames(CONTEXT* ctx, u64* frames, u32 max_frames);

static wchar_t g_dump_path[MAX_PATH];
static volatile long g_first_chance_logged = 0;
static volatile long g_dumped = 0;

void describe_address(const void* addr, char* out, usize cap) {
    PeImage* img = g_loader.find_containing(addr);
    if (img) {
        snprintf(out, cap, "%s+0x%x [0x%llx]", img->name, img->rva_of(addr),
                 (unsigned long long)img->ida_address(addr));
        return;
    }
    // Host module. Must not take the loader lock (this runs while diagnosing deadlocks), so no
    // GetModuleFileName: find the image base via the inverted function table and read the DLL
    // name from its export directory.
    void* base = nullptr;
    if (RtlPcToFileHeader((PVOID)addr, &base) && base) {
        const u8* b = (const u8*)base;
        const IMAGE_NT_HEADERS64* nt =
            (const IMAGE_NT_HEADERS64*)(b + ((const IMAGE_DOS_HEADER*)b)->e_lfanew);
        const IMAGE_DATA_DIRECTORY& ed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        const char* mod_name = (HMODULE)base == GetModuleHandleW(nullptr) ? "sludgemu.exe" : "?";
        if (ed.Size)
            mod_name = (const char*)(b + ((const IMAGE_EXPORT_DIRECTORY*)(b + ed.VirtualAddress))->Name);
        snprintf(out, cap, "%s+0x%llx", mod_name, (unsigned long long)((const u8*)addr - b));
        return;
    }
    snprintf(out, cap, "%p", addr);
}

static const char* exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW:
        return "STACK_OVERFLOW";
    case EXCEPTION_BREAKPOINT:
        return "BREAKPOINT";
    case EXCEPTION_IN_PAGE_ERROR:
        return "IN_PAGE_ERROR";
    case 0xE06D7363:
        return "C++ EXCEPTION";
    case 0xC0000409:
        return "STACK_BUFFER_OVERRUN (fastfail)";
    default:
        return "?";
    }
}

// Diagnostic for loader-data corruption: ntdll keeps an index of loaded modules by base address,
// a red-black tree whose nodes live inside each LDR_DATA_TABLE_ENTRY. Checks that every node's
// child links point at another module's node (or are null) and reports the ones that don't.
// Offsets are those of the 64-bit LDR_DATA_TABLE_ENTRY on Windows 8+.
#define LDR_ENTRY_DLLBASE         0x30
#define LDR_ENTRY_BASENAME        0x58
#define LDR_ENTRY_BASE_INDEX_NODE 0xC8

static bool readable(const void* p, usize bytes) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    return (const u8*)p + bytes <= (const u8*)mbi.BaseAddress + mbi.RegionSize;
}

static void validate_loader_index() {
    static volatile long s_running = 0;
    if (InterlockedExchange(&s_running, 1))
        return;
    const u8* peb = (const u8*)__readgsqword(0x60);
    const u8* ldr = *(const u8* const*)(peb + 0x18);
    const LIST_ENTRY* head = (const LIST_ENTRY*)(ldr + 0x10); // InLoadOrderModuleList
    const u8* nodes[512];
    const u8* entries[512];
    u32 n = 0;
    const LIST_ENTRY* prev = head;
    for (const LIST_ENTRY* e = head->Flink; e != head && n < ARRAY_COUNT(nodes); prev = e, e = e->Flink) {
        if (!readable(e, LDR_ENTRY_BASE_INDEX_NODE + 0x18)) {
            LOG_E("crash", "  loader module list broken: entry %p has Flink %p (unreadable)", prev, e);
            break;
        }
        entries[n] = (const u8*)e;
        nodes[n] = (const u8*)e + LDR_ENTRY_BASE_INDEX_NODE;
        n++;
    }
    u32 bad = 0;
    for (u32 i = 0; i < n; i++) {
        const u64* node = (const u64*)nodes[i];
        for (u32 side = 0; side < 2; side++) {
            u64 child = node[side];
            bool ok = child == 0;
            for (u32 j = 0; j < n && !ok; j++)
                ok = child == (u64)nodes[j];
            if (ok)
                continue;
            const UNICODE_STRING* name = (const UNICODE_STRING*)(entries[i] + LDR_ENTRY_BASENAME);
            char narrow[128] = "?";
            if (name->Buffer && readable(name->Buffer, name->Length))
                WideCharToMultiByte(CP_UTF8, 0, name->Buffer, name->Length / 2, narrow, sizeof(narrow) - 1,
                                    nullptr, nullptr);
            LOG_E("crash", "  loader index corrupt: %s entry %p, %s child at %p = 0x%llx", narrow, entries[i],
                  side ? "right" : "left", &node[side], child);
            bad++;
        }
    }
    LOG_E("crash", "  loader index check: %u modules, %u bad links", n, bad);
    s_running = 0;
}

static void log_exception(const char* kind, EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    char where[200];
    describe_address(er->ExceptionAddress, where, sizeof(where));
    LOG_E("crash", "%s exception 0x%08lx %s at %s", kind, er->ExceptionCode,
          exception_name(er->ExceptionCode), where);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char* op = er->ExceptionInformation[0] == 0
                             ? "read"
                             : (er->ExceptionInformation[0] == 1 ? "write" : "execute");
        LOG_E("crash", "  %s of address 0x%llx", op, (unsigned long long)er->ExceptionInformation[1]);
    }
    validate_loader_index();
    LOG_E("crash", "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    LOG_E("crash", "  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    LOG_E("crash", "  r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx", c->R8, c->R9, c->R10, c->R11);
    LOG_E("crash", "  r12=%016llx r13=%016llx r14=%016llx r15=%016llx", c->R12, c->R13, c->R14, c->R15);

    // Unwind-based backtrace (works through title code because .pdata is registered).
    CONTEXT ctx = *c;
    for (int frame = 0; frame < 24 && ctx.Rip; frame++) {
        char sym[200];
        describe_address((void*)ctx.Rip, sym, sizeof(sym));
        LOG_E("crash", "  #%02d %s", frame, sym);
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
        if (!fn) {
            // Leaf function: return address is at [rsp].
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
        } else {
            void* handler_data;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher,
                             nullptr);
        }
    }
    log_flush();
}

static void write_minidump(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_dumped, 1) != 0)
        return;
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelp)
        return;
    typedef BOOL(WINAPI *
                 MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                      PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    MiniDumpWriteDumpFn write = (MiniDumpWriteDumpFn)GetProcAddress(dbghelp, "MiniDumpWriteDump");
    if (!write)
        return;
    HANDLE f =
        CreateFileW(g_dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), ep, FALSE};
    MINIDUMP_TYPE type =
        (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                        MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);
    if (write(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, nullptr, nullptr))
        LOG_E("crash", "minidump written");
    CloseHandle(f);
}

// ---------------------------------------------------------------------------------------------
// Write watchpoint

static volatile u64 g_watch_addr = 0;
static volatile long g_watch_hits = 0;
#define WATCH_LOG_LIMIT 32

// Dr7: local enable for Dr0 (bit 0), RW0 = 01 (break on write, bits 16-17), LEN0 = 10 (8 bytes,
// bits 18-19).
#define DR7_WATCH0_WRITE8 ((1ull << 0) | (1ull << 16) | (2ull << 18))

static bool set_thread_watch(HANDLE thread, bool self) {
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!self && SuspendThread(thread) == (DWORD)-1)
        return false;
    bool ok = GetThreadContext(thread, &ctx) != 0;
    if (ok) {
        ctx.Dr0 = g_watch_addr;
        ctx.Dr7 = (ctx.Dr7 & ~0xF0003ull) | DR7_WATCH0_WRITE8;
        ok = SetThreadContext(thread, &ctx) != 0;
    }
    if (!self)
        ResumeThread(thread);
    return ok;
}

void crash_watch_current_thread() {
    if (g_watch_addr)
        set_thread_watch(GetCurrentThread(), true);
}

bool crash_arm_write_watch(const char* spec) {
    u64 addr = 0;
    const char* plus = strchr(spec, '+');
    if (strncmp(spec, "ldrentry:", 9) == 0) {
        // Flink of the Nth (1-based) entry of the native loader's load-order module list.
        u32 want = (u32)strtoul(spec + 9, nullptr, 10);
        const u8* peb = (const u8*)__readgsqword(0x60);
        const LIST_ENTRY* head = (const LIST_ENTRY*)(*(const u8* const*)(peb + 0x18) + 0x10);
        const LIST_ENTRY* e = head->Flink;
        for (u32 i = 1; i < want && e != head; i++)
            e = e->Flink;
        if (!want || e == head) {
            LOG_E("watch", "no loader entry %u", want);
            return false;
        }
        const UNICODE_STRING* name = (const UNICODE_STRING*)((const u8*)e + LDR_ENTRY_BASENAME);
        char narrow[128] = "?";
        WideCharToMultiByte(CP_UTF8, 0, name->Buffer, name->Length / 2, narrow, sizeof(narrow) - 1, nullptr,
                            nullptr);
        LOG_I("watch", "loader entry %u is %s at %p", want, narrow, e);
        addr = (u64)e;
    } else if (plus) {
        char mod[MAX_PATH];
        usize n = (usize)(plus - spec);
        if (n >= sizeof(mod))
            return false;
        memcpy(mod, spec, n);
        mod[n] = 0;
        HMODULE h = GetModuleHandleA(mod);
        PeImage* img = h ? nullptr : g_loader.find_by_name(mod);
        u8* base = h ? (u8*)h : (img ? img->base : nullptr);
        if (!base) {
            LOG_E("watch", "module %s is not loaded", mod);
            return false;
        }
        addr = (u64)base + strtoull(plus + 1, nullptr, 16);
    } else {
        addr = strtoull(spec, nullptr, 16);
    }
    if (!addr || (addr & 7)) {
        LOG_E("watch", "invalid watch address 0x%llx (must be 8-byte aligned)", addr);
        return false;
    }
    g_watch_addr = addr;

    // Every existing thread, then new ones via crash_watch_current_thread().
    u32 armed = 0;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te = {sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid)
                continue;
            if (te.th32ThreadID == self) {
                armed += set_thread_watch(GetCurrentThread(), true);
                continue;
            }
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
                                   te.th32ThreadID);
            if (th) {
                armed += set_thread_watch(th, false);
                CloseHandle(th);
            }
        }
        CloseHandle(snap);
    }
    LOG_I("watch", "write watch on 0x%llx (%s), armed on %u threads, current value 0x%llx", addr, spec, armed,
          *(volatile u64*)addr);
    return armed != 0;
}

static void log_watch_hit(EXCEPTION_POINTERS* ep) {
    long n = InterlockedIncrement(&g_watch_hits);
    if (n > WATCH_LOG_LIMIT)
        return;
    u64 value = *(volatile u64*)g_watch_addr;
    char where[200];
    describe_address((void*)ep->ContextRecord->Rip, where, sizeof(where));
    LOG_W("watch", "write #%ld to 0x%llx: value now 0x%llx, after instruction before %s", n, g_watch_addr,
          value, where);
    CONTEXT ctx = *ep->ContextRecord;
    u64 frames[16];
    u32 count = capture_frames(&ctx, frames, ARRAY_COUNT(frames));
    for (u32 i = 0; i < count; i++) {
        describe_address((void*)frames[i], where, sizeof(where));
        LOG_W("watch", "  #%02u %s", i, where);
    }
}

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && g_watch_addr && (ep->ContextRecord->Dr6 & 1)) {
        log_watch_hit(ep);
        ep->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Breakpoints are title asserts; the title usually installs its own unhandled-exception
    // filter, so this is the only place they get reported.
    bool interesting = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                       code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                       code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_IN_PAGE_ERROR ||
                       code == EXCEPTION_BREAKPOINT;
    if (interesting && InterlockedIncrement(&g_first_chance_logged) <= FIRST_CHANCE_LOG_LIMIT)
        log_exception("first-chance", ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
    log_exception("UNHANDLED", ep);
    write_minidump(ep);
    log_flush();
    return EXCEPTION_EXECUTE_HANDLER;
}

// Unwinds one stack into raw return addresses. Must not log or touch the loader lock: other
// threads are suspended while this runs.
static u32 capture_frames(CONTEXT* ctx, u64* frames, u32 max_frames) {
    u32 n = 0;
    while (n < max_frames && ctx->Rip) {
        frames[n++] = ctx->Rip;
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx->Rip, &image_base, nullptr);
        if (!fn) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)ctx->Rsp, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
                break;
            ctx->Rip = *(DWORD64*)ctx->Rsp;
            ctx->Rsp += 8;
        } else {
            void* handler_data;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx->Rip, fn, ctx, &handler_data, &establisher,
                             nullptr);
        }
    }
    return n;
}

#define DUMP_MAX_THREADS 512
#define DUMP_MAX_FRAMES  32

struct ThreadDump {
    DWORD tid;
    u32 frame_count;
    u64 frames[DUMP_MAX_FRAMES];
};

void crash_dump_all_threads(const char* reason) {
    static ThreadDump s_dumps[DUMP_MAX_THREADS];
    static SRWLOCK s_lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&s_lock);

    DWORD self = GetCurrentThreadId();
    DWORD pid = GetCurrentProcessId();
    u32 count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te = {sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok && count < DUMP_MAX_THREADS;
             ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                   FALSE, te.th32ThreadID);
            if (!th)
                continue;
            if (SuspendThread(th) != (DWORD)-1) {
                CONTEXT ctx = {};
                ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                ThreadDump& d = s_dumps[count];
                d.tid = te.th32ThreadID;
                d.frame_count =
                    GetThreadContext(th, &ctx) ? capture_frames(&ctx, d.frames, DUMP_MAX_FRAMES) : 0;
                ResumeThread(th);
                count++;
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
    }

    LOG_I("threads", "---- thread dump (%s): %u threads ----", reason, count);
    for (u32 i = 0; i < count; i++) {
        const ThreadDump& d = s_dumps[i];
        LOG_I("threads", "thread %lu:", d.tid);
        for (u32 f = 0; f < d.frame_count; f++) {
            char sym[200];
            describe_address((void*)d.frames[f], sym, sizeof(sym));
            LOG_I("threads", "  #%02u %s", f, sym);
        }
    }
    LOG_I("threads", "---- end of thread dump ----");
    log_flush();
    ReleaseSRWLockExclusive(&s_lock);
}

static DWORD WINAPI thread_dump_timer(void* param) {
    u32 seconds = (u32)(uptr)param;
    for (;;) {
        Sleep(seconds * 1000);
        crash_dump_all_threads("timer");
    }
}

void crash_start_thread_dump_timer(u32 seconds) {
    if (!seconds)
        return;
    HANDLE t = CreateThread(nullptr, 0, thread_dump_timer, (void*)(uptr)seconds, 0, nullptr);
    if (t)
        CloseHandle(t);
}

void crash_install(const wchar_t* dump_path) {
    str_copy_w(g_dump_path, MAX_PATH, dump_path);
    AddVectoredExceptionHandler(1, vectored_handler);
    SetUnhandledExceptionFilter(unhandled_filter);
}
