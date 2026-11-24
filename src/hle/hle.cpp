#include "hle/hle.h"

#include "common/log.h"
#include "common/str.h"
#include "hle/kernel/overrides.h"
#include "hle/xgameruntime/xgameruntime.h"

// Module tables live next to their implementations.
extern const HleModule g_hle_module_xmem;
extern const HleModule g_hle_module_d3d12_xs;
extern const HleModule g_hle_module_xg_xs;
extern const HleModule g_hle_module_chad;
extern const HleModule g_hle_module_xcrdapi;
extern const HleModule g_hle_module_xapu;
extern const HleModule g_hle_module_acphal;
extern const HleModule g_hle_module_gameinput;
extern const HleModule g_hle_module_xboxbvhbuilder;
extern const HleModule g_hle_module_ffx_loader;
extern const HleModule g_hle_module_xgameruntime;

struct OverrideTable {
    const HleExport* exports;
    const u32* count;
};

static const OverrideTable k_override_tables[] = {
    {g_module_overrides, &g_module_override_count},
    {g_window_overrides, &g_window_override_count},
    {g_memory_overrides, &g_memory_override_count},
    {g_cpu_overrides, &g_cpu_override_count},
};

static const HleModule* const k_modules[] = {
    &g_hle_module_xmem,      &g_hle_module_d3d12_xs,  &g_hle_module_xg_xs,
    &g_hle_module_chad,      &g_hle_module_xcrdapi,   &g_hle_module_xapu,
    &g_hle_module_acphal,    &g_hle_module_gameinput, &g_hle_module_xboxbvhbuilder,
    &g_hle_module_ffx_loader, &g_hle_module_xgameruntime,
};

// Pseudo module handles: pointers into this array. Never dereferenced by the title as a PE image
// in practice; if one is, the magic makes it obvious in a debugger.
struct HleHandle {
    u64 magic;
    const HleModule* module;
};
#define HLE_HANDLE_MAGIC 0x45444F4D454C4855ull // "UHLEMODE"

static HleHandle g_handles[ARRAY_COUNT(k_modules)];

void hle_init() {
    cpu_overrides_init();
    xgameruntime_init();
    for (u32 i = 0; i < ARRAY_COUNT(k_modules); i++) {
        g_handles[i].magic = HLE_HANDLE_MAGIC;
        g_handles[i].module = k_modules[i];
        LOG_D("hle", "module %s: %u exports implemented", k_modules[i]->name, k_modules[i]->export_count);
    }
}

const HleModule* hle_find_module(const char* dll_name) {
    for (u32 i = 0; i < ARRAY_COUNT(k_modules); i++) {
        if (str_ieq(k_modules[i]->name, dll_name))
            return k_modules[i];
    }
    return nullptr;
}

void* hle_find_export(const HleModule* module, const char* name, u16 ordinal) {
    for (u32 i = 0; i < module->export_count; i++) {
        const HleExport& e = module->exports[i];
        if (name && e.name && strcmp(e.name, name) == 0)
            return e.fn;
        if (!name && ordinal && e.ordinal == ordinal)
            return e.fn;
    }
    return nullptr;
}

void* hle_find_host_override(const char* fn_name) {
    if (!fn_name)
        return nullptr;
    for (u32 t = 0; t < ARRAY_COUNT(k_override_tables); t++) {
        const OverrideTable& table = k_override_tables[t];
        for (u32 i = 0; i < *table.count; i++) {
            if (strcmp(table.exports[i].name, fn_name) == 0)
                return table.exports[i].fn;
        }
    }
    return nullptr;
}

HMODULE hle_module_handle(const HleModule* module) {
    for (u32 i = 0; i < ARRAY_COUNT(k_modules); i++) {
        if (k_modules[i] == module)
            return (HMODULE)&g_handles[i];
    }
    return nullptr;
}

const HleModule* hle_module_from_handle(HMODULE handle) {
    const HleHandle* h = (const HleHandle*)handle;
    if (h < g_handles || h >= g_handles + ARRAY_COUNT(g_handles))
        return nullptr;
    return h->module;
}
