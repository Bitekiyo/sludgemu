#pragma once

#include "common/types.h"

enum LogLevel {
    LOG_LEVEL_TRACE,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL,
};

// Opens the log file (truncating it). log_printf works before this too; output then only goes to
// the console.
void log_init(const wchar_t* file_path, LogLevel min_level);

// Number of messages logged at error level or above (counted even when filtered out).
u32 log_error_count();
void log_shutdown();
void log_flush();
void log_set_level(LogLevel min_level);
bool log_enabled(LogLevel level);

void log_printf(LogLevel level, const char* channel, const char* fmt, ...);

#define LOG_T(ch, ...) log_printf(LOG_LEVEL_TRACE, ch, __VA_ARGS__)
#define LOG_D(ch, ...) log_printf(LOG_LEVEL_DEBUG, ch, __VA_ARGS__)
#define LOG_I(ch, ...) log_printf(LOG_LEVEL_INFO, ch, __VA_ARGS__)
#define LOG_W(ch, ...) log_printf(LOG_LEVEL_WARN, ch, __VA_ARGS__)
#define LOG_E(ch, ...) log_printf(LOG_LEVEL_ERROR, ch, __VA_ARGS__)

[[noreturn]] void sludge_fatal(const char* file, int line, const char* fmt, ...);

#define FATAL(...) sludge_fatal(__FILE__, __LINE__, __VA_ARGS__)

#define SLUDGE_ASSERT(cond)                                                                                  \
    do {                                                                                                     \
        if (!(cond))                                                                                         \
            sludge_fatal(__FILE__, __LINE__, "assertion failed: %s", #cond);                                 \
    } while (0)

bool log_once_flag(volatile long* flag);

// Logs a warning the first time this call site is reached, for paths that can repeat every frame.
#define LOG_W_ONCE(ch, ...)                                                                                  \
    do {                                                                                                     \
        static volatile long s_logged_ = 0;                                                                  \
        if (log_once_flag(&s_logged_))                                                                       \
            LOG_W(ch, __VA_ARGS__);                                                                          \
    } while (0)

// For HLE functions that are knowingly incomplete. Logs once per call site, never silent.
#define UNIMPLEMENTED(ch)                                                                                    \
    do {                                                                                                     \
        static volatile long s_unimpl_hit_ = 0;                                                              \
        if (log_once_flag(&s_unimpl_hit_))                                                                   \
            LOG_W(ch, "UNIMPLEMENTED: %s (%s:%d)", __FUNCTION__, __FILE__, __LINE__);                        \
    } while (0)
