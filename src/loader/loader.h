#pragma once

#include <windows.h>

#include "common/types.h"
#include "loader/pe_image.h"

struct HleModule;

enum ModuleKind {
    MODULE_MISSING,
    MODULE_HLE,   // Xbox system DLL implemented by us
    MODULE_TITLE, // DLL shipped in the title package, mapped by us
    MODULE_HOST,  // regular Windows DLL loaded by the native loader
};

struct ModuleRef {
    ModuleKind kind = MODULE_MISSING;
    const HleModule* hle = nullptr;
    PeImage* image = nullptr;
    HMODULE host = nullptr;
};

// Maps the title executable and its package DLLs by hand and binds their imports.
//
// Import resolution order for a DLL name:
//   1. HLE module claiming the name (xmem.dll, d3d12_xs.dll, ...)
//   2. title-local DLL from the package directory (mapped recursively)
//   3. host DLL (System32 / API sets); individual functions may be replaced by host overrides
// Anything unresolved becomes a logging stub.
class Loader {
public:
    void init(const wchar_t* title_dir);

    PeImage* load_main(const wchar_t* exe_path);

    // Runs TLS callbacks and the title entry point on the calling thread. Does not return.
    [[noreturn]] void run_main();

    // Opens (loading if needed) a module by DLL name, using the resolution order above.
    ModuleRef open_module(const char* dll_name);

    // Looks up an export in an opened module. Unresolved exports return a logging stub when
    // stub_missing is set, otherwise nullptr.
    void* resolve(const ModuleRef& module, const char* dll_name, const char* fn_name, u16 ordinal,
                  bool stub_missing);

    PeImage* find_by_name(const char* dll_name);
    PeImage* find_by_handle(HMODULE handle);
    PeImage* find_containing(const void* addr); // lock-free; safe from exception handlers

    PeImage* main_image() const { return m_main; }
    const wchar_t* title_dir() const { return m_title_dir; }

    // Title-visible command line ("<exe path>" args...). Also patched into the PEB.
    void set_command_line(const wchar_t* args);
    const wchar_t* command_line_w() const { return m_cmdline_w; }
    const char* command_line_a() const { return m_cmdline_a; }

private:
    PeImage* load_image(const wchar_t* path, bool is_main);
    bool bind_imports(PeImage* image);
    bool setup_main_tls(PeImage* image);
    void init_thread_tls();
    void call_tls_callbacks(PeImage* image, DWORD reason);
    void notify_thread(DWORD reason);
    bool title_local_path(const char* dll_name, wchar_t* out, usize cap) const;

    static void thread_callback(u32 reason);

    static const u32 k_max_images = 64;
    PeImage m_images[k_max_images];
    volatile u32 m_count = 0;
    PeImage* m_main = nullptr;
    wchar_t m_title_dir[MAX_PATH] = {};
    CRITICAL_SECTION m_lock;

    const u8* m_tls_template = nullptr;
    u32 m_tls_template_size = 0;
    u32 m_tls_zero_fill = 0;
    bool m_tls_ready = false;

    wchar_t* m_cmdline_w = nullptr;
    char* m_cmdline_a = nullptr;
};

extern Loader g_loader;
