#pragma once

#include <windows.h>

#include "common/types.h"

// What we know about the title being launched, mostly from its MicrosoftGame.Config.
struct TitleInfo {
    wchar_t dir[MAX_PATH];      // package root
    wchar_t exe_path[MAX_PATH]; // full path to the executable
    wchar_t display_name[256];  // window caption
    char title_id[16];          // hex string, e.g. "1234ABCD" (empty if unknown)
};

// The title being run (filled in by main before anything else starts).
extern TitleInfo g_title;

// `path` may be the package directory (executable picked from MicrosoftGame.Config) or the
// executable itself.
bool title_resolve(const wchar_t* path, TitleInfo* out, wchar_t* error, usize error_cap);
