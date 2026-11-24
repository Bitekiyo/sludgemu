// Library-loader functions as seen by the title. The title's modules are not known to the native
// loader, so LoadLibrary / GetProcAddress / GetModuleHandle / GetModuleFileName must go through
// our loader to find HLE modules and mapped title DLLs.

#include <windows.h>
#include <stdio.h>

#include "common/log.h"
#include "common/str.h"
#include "hle/hle.h"
#include "hle/kernel/overrides.h"
#include "loader/loader.h"

static const char* const k_kind_names[] = {"FAILED", "hle", "title", "host"};

static bool is_resource_only(DWORD flags) {
    return (flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) != 0;
}

static HMODULE module_ref_handle(const ModuleRef& ref) {
    switch (ref.kind) {
    case MODULE_HLE: return hle_module_handle(ref.hle);
    case MODULE_TITLE: return (HMODULE)ref.image->base;
    case MODULE_HOST: return ref.host;
    default: return nullptr;
    }
}

static HMODULE load_library(const wchar_t* wname, DWORD flags) {
    if (!wname) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (is_resource_only(flags))
        return LoadLibraryExW(wname, nullptr, flags);

    char name[MAX_PATH];
    str_to_narrow(name, sizeof(name), path_file_name_w(wname));
    if (!strchr(name, '.')) {
        usize len = strlen(name);
        if (len + 4 < sizeof(name))
            memcpy(name + len, ".dll", 5);
    }

    ModuleRef ref = g_loader.open_module(name);
    HMODULE h = module_ref_handle(ref);
    if (!h && wname[0] && wcschr(wname, L'\\')) {
        // Absolute path outside System32 that isn't a title DLL: let the host try it as given.
        h = LoadLibraryExW(wname, nullptr, flags);
        if (h)
            ref.kind = MODULE_HOST;
    }
    LogLevel level = h ? LOG_LEVEL_INFO : LOG_LEVEL_WARN;
    log_printf(level, "kernel", "LoadLibrary(%s, flags=0x%lx) -> %s %p", name, flags, k_kind_names[h ? ref.kind : 0], h);
    if (!h)
        SetLastError(ERROR_MOD_NOT_FOUND);
    return h;
}

static HMODULE WINAPI hk_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) { return load_library(name, flags); }
static HMODULE WINAPI hk_LoadLibraryW(LPCWSTR name) { return load_library(name, 0); }

static HMODULE WINAPI hk_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
    if (!name)
        return load_library(nullptr, flags);
    wchar_t wname[MAX_PATH];
    str_to_wide(wname, MAX_PATH, name);
    return load_library(wname, flags);
}

static HMODULE WINAPI hk_LoadLibraryA(LPCSTR name) { return hk_LoadLibraryExA(name, nullptr, 0); }

static BOOL WINAPI hk_FreeLibrary(HMODULE h) {
    if (hle_module_from_handle(h) || g_loader.find_by_handle(h))
        return TRUE;
    return FreeLibrary(h);
}

static FARPROC WINAPI hk_GetProcAddress(HMODULE h, LPCSTR name) {
    bool by_ordinal = IS_INTRESOURCE(name);
    const char* fn_name = by_ordinal ? nullptr : name;
    u16 ordinal = by_ordinal ? (u16)(uptr)name : 0;

    ModuleRef ref;
    char dll[MAX_PATH] = "?";
    if ((ref.hle = hle_module_from_handle(h)) != nullptr) {
        ref.kind = MODULE_HLE;
        str_copy(dll, sizeof(dll), ref.hle->name);
    } else if ((ref.image = g_loader.find_by_handle(h)) != nullptr) {
        ref.kind = MODULE_TITLE;
        str_copy(dll, sizeof(dll), ref.image->name);
    } else {
        // No GetModuleFileNameW here: it takes the loader lock, and titles call GetProcAddress
        // while holding their own locks (deadlocks against thread-attach TLS callbacks).
        ref.kind = MODULE_HOST;
        ref.host = h;
        const char* known = g_loader.host_module_name(h);
        if (known)
            str_copy(dll, sizeof(dll), known);
        else
            snprintf(dll, sizeof(dll), "host module %p", (void*)h);
    }

    // HLE modules always hand out something callable, so missing functions show up as stub
    // calls in the log rather than as mysterious "feature not available" paths.
    void* fn = g_loader.resolve(ref, dll, fn_name, ordinal, ref.kind == MODULE_HLE);
    if (fn_name)
        log_printf(fn ? LOG_LEVEL_DEBUG : LOG_LEVEL_WARN, "kernel", "GetProcAddress(%s, %s) -> %p", dll, fn_name, fn);
    else
        log_printf(fn ? LOG_LEVEL_DEBUG : LOG_LEVEL_WARN, "kernel", "GetProcAddress(%s, #%u) -> %p", dll, ordinal, fn);
    if (!fn)
        SetLastError(ERROR_PROC_NOT_FOUND);
    return (FARPROC)fn;
}

static HMODULE get_module_handle(const wchar_t* wname) {
    if (!wname)
        return (HMODULE)g_loader.main_image()->base;
    char name[MAX_PATH];
    str_to_narrow(name, sizeof(name), path_file_name_w(wname));
    if (!strchr(name, '.')) {
        usize len = strlen(name);
        if (len + 4 < sizeof(name))
            memcpy(name + len, ".dll", 5);
    }
    if (const HleModule* m = hle_find_module(name))
        return hle_module_handle(m);
    if (PeImage* img = g_loader.find_by_name(name))
        return (HMODULE)img->base;
    return GetModuleHandleW(wname);
}

static HMODULE WINAPI hk_GetModuleHandleW(LPCWSTR name) { return get_module_handle(name); }

static HMODULE WINAPI hk_GetModuleHandleA(LPCSTR name) {
    if (!name)
        return get_module_handle(nullptr);
    wchar_t wname[MAX_PATH];
    str_to_wide(wname, MAX_PATH, name);
    return get_module_handle(wname);
}

// Returns false if `h` is a host module the caller should forward.
static bool title_module_path(HMODULE h, wchar_t* out, usize cap) {
    if (!h || h == (HMODULE)g_loader.main_image()->base) {
        str_copy_w(out, cap, g_loader.main_image()->path);
        return true;
    }
    if (PeImage* img = g_loader.find_by_handle(h)) {
        str_copy_w(out, cap, img->path);
        return true;
    }
    if (const HleModule* m = hle_module_from_handle(h)) {
        wchar_t wname[64];
        str_to_wide(wname, 64, m->name);
        _snwprintf(out, cap, L"%s\\%s", g_loader.title_dir(), wname);
        out[cap - 1] = 0;
        return true;
    }
    return false;
}

static DWORD copy_module_path(const wchar_t* src, wchar_t* dst, DWORD size) {
    if (!size) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    usize len = wcslen(src);
    if (len >= size) {
        memcpy(dst, src, (size - 1) * sizeof(wchar_t));
        dst[size - 1] = 0;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    memcpy(dst, src, (len + 1) * sizeof(wchar_t));
    SetLastError(ERROR_SUCCESS);
    return (DWORD)len;
}

static DWORD WINAPI hk_GetModuleFileNameW(HMODULE h, LPWSTR out, DWORD size) {
    wchar_t path[MAX_PATH];
    if (!title_module_path(h, path, MAX_PATH))
        return GetModuleFileNameW(h, out, size);
    return copy_module_path(path, out, size);
}

static DWORD WINAPI hk_GetModuleFileNameA(HMODULE h, LPSTR out, DWORD size) {
    wchar_t path[MAX_PATH];
    if (!title_module_path(h, path, MAX_PATH))
        return GetModuleFileNameA(h, out, size);
    wchar_t tmp[MAX_PATH];
    DWORD n = copy_module_path(path, tmp, size < MAX_PATH ? size : MAX_PATH);
    if (size)
        WideCharToMultiByte(CP_ACP, 0, tmp, -1, out, (int)size, nullptr, nullptr);
    return n;
}

static LPSTR WINAPI hk_GetCommandLineA() { return (LPSTR)g_loader.command_line_a(); }
static LPWSTR WINAPI hk_GetCommandLineW() { return (LPWSTR)g_loader.command_line_w(); }

const HleExport g_module_overrides[] = {
    HLE_EXPORT_AS(hk_LoadLibraryExW, "LoadLibraryExW"),
    HLE_EXPORT_AS(hk_LoadLibraryExA, "LoadLibraryExA"),
    HLE_EXPORT_AS(hk_LoadLibraryW, "LoadLibraryW"),
    HLE_EXPORT_AS(hk_LoadLibraryA, "LoadLibraryA"),
    HLE_EXPORT_AS(hk_FreeLibrary, "FreeLibrary"),
    HLE_EXPORT_AS(hk_GetProcAddress, "GetProcAddress"),
    HLE_EXPORT_AS(hk_GetModuleHandleW, "GetModuleHandleW"),
    HLE_EXPORT_AS(hk_GetModuleHandleA, "GetModuleHandleA"),
    HLE_EXPORT_AS(hk_GetModuleFileNameW, "GetModuleFileNameW"),
    HLE_EXPORT_AS(hk_GetModuleFileNameA, "GetModuleFileNameA"),
    HLE_EXPORT_AS(hk_GetCommandLineA, "GetCommandLineA"),
    HLE_EXPORT_AS(hk_GetCommandLineW, "GetCommandLineW"),
};
const u32 g_module_override_count = ARRAY_COUNT(g_module_overrides);
