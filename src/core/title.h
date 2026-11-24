#pragma once

#include <windows.h>

#include "common/types.h"

// What we know about the title being launched, mostly from its MicrosoftGame.Config.
struct TitleInfo {
    wchar_t dir[MAX_PATH];         // package root
    wchar_t exe_path[MAX_PATH];    // full path to the executable
    wchar_t display_name[256];     // window caption
    char title_id[16];             // hex string, e.g. "1234ABCD" (empty if unknown)
    u64 pls_quota;                 // persistent local storage size in bytes (0 if none declared)
    bool advanced_memory_series_x; // title memory mode "Advanced" (default Standard)
    bool advanced_memory_series_s;
};

// The title being run (filled in by main before anything else starts).
extern TitleInfo g_title;

// `path` may be the package directory (executable picked from MicrosoftGame.Config) or the
// executable itself.
bool title_resolve(const wchar_t* path, TitleInfo* out, wchar_t* error, usize error_cap);

// Host directory for per-title user data (%LOCALAPPDATA%\sludgemu\<title id>\<name>), created on
// demand. Written with a trailing backslash. Returns false if it can't be created.
bool title_data_dir(const wchar_t* name, wchar_t* out, usize cap);
