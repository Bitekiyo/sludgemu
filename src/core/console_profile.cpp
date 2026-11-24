#include "core/console_profile.h"

#include <windows.h>

#include "core/title.h"

static const ConsoleProfile k_profiles[] = {
    // Standard: 10 GB at full GPU bandwidth; Advanced adds 3 GB of standard-bandwidth memory.
    {CONSOLE_SERIES_X, "Xbox Series X", GiB(16), GiB(10), GiB(13), 0x64},
    // Standard: 7904 MiB GPU-optimal + 94 MiB standard; Advanced adds 574 MiB.
    {CONSOLE_SERIES_S, "Xbox Series S", GiB(10), MiB(7998), MiB(7998 + 574), 0x60},
};

static const ConsoleProfile* g_profile = &k_profiles[0];

void console_profile_select(ConsoleModel model) {
    g_profile = &k_profiles[model == CONSOLE_SERIES_S ? 1 : 0];
}

const ConsoleProfile& console_profile() {
    return *g_profile;
}

u64 console_title_memory() {
    bool advanced = g_profile->model == CONSOLE_SERIES_S ? g_title.advanced_memory_series_s
                                                         : g_title.advanced_memory_series_x;
    return advanced ? g_profile->title_memory_advanced : g_profile->title_memory_standard;
}

bool console_model_parse(const wchar_t* name, ConsoleModel* out) {
    if (!_wcsicmp(name, L"series-x") || !_wcsicmp(name, L"x")) {
        *out = CONSOLE_SERIES_X;
        return true;
    }
    if (!_wcsicmp(name, L"series-s") || !_wcsicmp(name, L"s")) {
        *out = CONSOLE_SERIES_S;
        return true;
    }
    return false;
}
