#include "loader/crash.h"

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>

#include "common/log.h"
#include "common/str.h"
#include "loader/loader.h"

#define FIRST_CHANCE_LOG_LIMIT 64

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
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) &&
        mod) {
        wchar_t wpath[MAX_PATH];
        char narrow[MAX_PATH];
        GetModuleFileNameW(mod, wpath, MAX_PATH);
        str_to_narrow(narrow, sizeof(narrow), path_file_name_w(wpath));
        snprintf(out, cap, "%s+0x%llx", narrow, (unsigned long long)((const u8*)addr - (const u8*)mod));
        return;
    }
    snprintf(out, cap, "%p", addr);
}

static const char* exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    case 0xE06D7363: return "C++ EXCEPTION";
    case 0xC0000409: return "STACK_BUFFER_OVERRUN (fastfail)";
    default: return "?";
    }
}

static void log_exception(const char* kind, EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    char where[200];
    describe_address(er->ExceptionAddress, where, sizeof(where));
    LOG_E("crash", "%s exception 0x%08lx %s at %s", kind, er->ExceptionCode, exception_name(er->ExceptionCode),
          where);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char* op = er->ExceptionInformation[0] == 0 ? "read" : (er->ExceptionInformation[0] == 1 ? "write" : "execute");
        LOG_E("crash", "  %s of address 0x%llx", op, (unsigned long long)er->ExceptionInformation[1]);
    }
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
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
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
    typedef BOOL(WINAPI * MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                               PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    MiniDumpWriteDumpFn write = (MiniDumpWriteDumpFn)GetProcAddress(dbghelp, "MiniDumpWriteDump");
    if (!write)
        return;
    HANDLE f = CreateFileW(g_dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), ep, FALSE};
    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                         MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);
    if (write(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, nullptr, nullptr))
        LOG_E("crash", "minidump written");
    CloseHandle(f);
}

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    bool interesting = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                       code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                       code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_IN_PAGE_ERROR;
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

void crash_install(const wchar_t* dump_path) {
    str_copy_w(g_dump_path, MAX_PATH, dump_path);
    AddVectoredExceptionHandler(1, vectored_handler);
    SetUnhandledExceptionFilter(unhandled_filter);
}
