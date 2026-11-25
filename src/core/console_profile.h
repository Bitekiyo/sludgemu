#pragma once

#include "common/types.h"

// Which console sludgemu presents itself as. Titles adapt to the hardware they detect (memory
// budget, render resolution, quality settings), so emulating a Series S is the lighter option for
// machines with less memory or a smaller GPU.

enum ConsoleModel {
    CONSOLE_SERIES_X,
    CONSOLE_SERIES_S,
};

struct ConsoleProfile {
    ConsoleModel model;
    const char* name;
    u64 physical_memory;       // installed memory
    u64 title_memory_standard; // title budget in the Standard memory mode
    u64 title_memory_advanced; // title budget in the Advanced memory mode
    u32 console_mode;          // hardware identification value the runtime reads from the registry
};

void console_profile_select(ConsoleModel model);
const ConsoleProfile& console_profile();

// Memory budget of the running title: its memory mode for this console comes from
// MicrosoftGame.Config (Standard unless it asks for Advanced).
u64 console_title_memory();

// The console a host can run titles for: a Series X title fills about 13 GB of memory and 10 GB of
// it at full GPU bandwidth, so smaller machines get a Series S, whose titles budget about 8 GB in all.
ConsoleModel console_model_for_host();

// Accepts "series-x"/"x" and "series-s"/"s" (case-insensitive).
bool console_model_parse(const wchar_t* name, ConsoleModel* out);
