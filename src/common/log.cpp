#include "common/log.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

namespace {

struct LogState {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE console = INVALID_HANDLE_VALUE;
    bool console_vt = false;
    LogLevel min_level = LOG_LEVEL_INFO;
    SRWLOCK lock = SRWLOCK_INIT;
    LARGE_INTEGER qpc_start = {};
    LARGE_INTEGER qpc_freq = {};
};

LogState g_log;

const char k_level_chars[] = {'T', 'D', 'I', 'W', 'E', 'F'};
const char* const k_level_colors[] = {
    "\x1b[90m", "\x1b[37m", "\x1b[0m", "\x1b[33m", "\x1b[31m", "\x1b[41;97m",
};

void ensure_timer() {
    if (g_log.qpc_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&g_log.qpc_freq);
        QueryPerformanceCounter(&g_log.qpc_start);
    }
}

void ensure_console() {
    if (g_log.console != INVALID_HANDLE_VALUE)
        return;
    g_log.console = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (g_log.console && GetConsoleMode(g_log.console, &mode)) {
        g_log.console_vt = SetConsoleMode(g_log.console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }
}

} // namespace

void log_init(const wchar_t* file_path, LogLevel min_level) {
    ensure_timer();
    ensure_console();
    g_log.min_level = min_level;
    if (file_path) {
        g_log.file = CreateFileW(file_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    }
}

void log_shutdown() {
    AcquireSRWLockExclusive(&g_log.lock);
    if (g_log.file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log.file);
        CloseHandle(g_log.file);
        g_log.file = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&g_log.lock);
}

void log_flush() {
    AcquireSRWLockExclusive(&g_log.lock);
    if (g_log.file != INVALID_HANDLE_VALUE)
        FlushFileBuffers(g_log.file);
    ReleaseSRWLockExclusive(&g_log.lock);
}

void log_set_level(LogLevel min_level) { g_log.min_level = min_level; }

bool log_enabled(LogLevel level) { return level >= g_log.min_level; }

bool log_once_flag(volatile long* flag) { return InterlockedExchange(flag, 1) == 0; }

static void log_vprintf(LogLevel level, const char* channel, const char* fmt, va_list args) {
    if (level < g_log.min_level)
        return;
    ensure_timer();
    ensure_console();

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    f64 t = (f64)(now.QuadPart - g_log.qpc_start.QuadPart) / (f64)g_log.qpc_freq.QuadPart;

    char line[4096];
    int prefix = snprintf(line, sizeof(line), "[%10.6f] [%5lu] %c %-8s ", t, GetCurrentThreadId(),
                          k_level_chars[level], channel);
    if (prefix < 0)
        prefix = 0;
    int body = vsnprintf(line + prefix, sizeof(line) - (usize)prefix - 2, fmt, args);
    usize len = (usize)prefix + (body < 0 ? 0 : (usize)body);
    if (len > sizeof(line) - 2)
        len = sizeof(line) - 2;
    line[len++] = '\n';
    line[len] = 0;

    AcquireSRWLockExclusive(&g_log.lock);
    DWORD written;
    if (g_log.console != INVALID_HANDLE_VALUE && g_log.console) {
        if (g_log.console_vt) {
            const char* color = k_level_colors[level];
            WriteFile(g_log.console, color, (DWORD)strlen(color), &written, nullptr);
            WriteFile(g_log.console, line, (DWORD)len - 1, &written, nullptr);
            WriteFile(g_log.console, "\x1b[0m\n", 5, &written, nullptr);
        } else {
            WriteFile(g_log.console, line, (DWORD)len, &written, nullptr);
        }
    }
    if (g_log.file != INVALID_HANDLE_VALUE)
        WriteFile(g_log.file, line, (DWORD)len, &written, nullptr);
    ReleaseSRWLockExclusive(&g_log.lock);

    if (IsDebuggerPresent())
        OutputDebugStringA(line);
}

void log_printf(LogLevel level, const char* channel, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_vprintf(level, channel, fmt, args);
    va_end(args);
}

void sludge_fatal(const char* file, int line, const char* fmt, ...) {
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    log_printf(LOG_LEVEL_FATAL, "fatal", "%s (%s:%d)", msg, file, line);
    log_flush();
    if (IsDebuggerPresent())
        __debugbreak();
    TerminateProcess(GetCurrentProcess(), 0xDEAD);
    for (;;) {
    }
}
