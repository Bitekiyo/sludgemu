#pragma once

#include "hle/hle.h"

// Host-function override tables, merged by hle_find_host_override().

extern const HleExport g_module_overrides[];
extern const u32 g_module_override_count;

extern const HleExport g_window_overrides[];
extern const u32 g_window_override_count;

extern const HleExport g_memory_overrides[];
extern const u32 g_memory_override_count;

extern const HleExport g_cpu_overrides[];
extern const u32 g_cpu_override_count;
void cpu_overrides_init();
