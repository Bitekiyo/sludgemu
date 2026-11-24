#pragma once

#include <windows.h>

#include "common/types.h"

// High-level emulation of Xbox system DLLs.
//
// An HleModule "claims" a DLL name: imports from it never go to the host. Exports we haven't
// written yet resolve to logging stubs, so a missing function is always visible in the log.

struct HleExport {
    const char* name; // nullptr for ordinal-only exports
    u16 ordinal;      // 0 if exported by name only
    void* fn;
};

struct HleModule {
    const char* name; // e.g. "xmem.dll"
    const HleExport* exports;
    u32 export_count;
};

#define HLE_EXPORT(fn) {#fn, 0, (void*)&fn}
#define HLE_EXPORT_AS(fn, name) {name, 0, (void*)&fn}
#define HLE_EXPORT_ORDINAL(fn, name, ord) {name, ord, (void*)&fn}

// Declares an HLE module whose exports are all still logging stubs.
#define HLE_EMPTY_MODULE(var, dll_name) extern const HleModule var = {dll_name, nullptr, 0}

void hle_init();

const HleModule* hle_find_module(const char* dll_name);
void* hle_find_export(const HleModule* module, const char* name, u16 ordinal);

// Replacements for host functions as seen by the title (LoadLibrary, GetModuleHandle, ...).
// Looked up by function name for imports that resolve to host DLLs.
void* hle_find_host_override(const char* fn_name);

// Fake HMODULE values handed to the title for HLE modules.
HMODULE hle_module_handle(const HleModule* module);
const HleModule* hle_module_from_handle(HMODULE handle);
