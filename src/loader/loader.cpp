#include "loader/loader.h"

#include <winternl.h>
#include <stdio.h>

#include "common/log.h"
#include "common/str.h"
#include "common/thread_slot.h"
#include "hle/hle.h"
#include "loader/crash.h"
#include "loader/stubs.h"
#include "loader/tls_reserve.h"

Loader g_loader;

typedef LONG(NTAPI* NtQueryInformationThreadFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);

static NtQueryInformationThreadFn g_nt_query_information_thread = nullptr;
static ThreadSlot g_title_thread; // nonzero on threads the title created

// The title package ships Xbox builds of the VC++ runtime. The host's builds are ABI compatible
// and actually work on desktop, so these always come from System32.
static const char* const k_host_runtime_dlls[] = {
    "vcruntime140.dll",   "vcruntime140_1.dll",         "msvcp140.dll",     "msvcp140_1.dll",
    "msvcp140_2.dll",     "msvcp140_atomic_wait.dll",   "msvcp140_codecvt_ids.dll",
    "concrt140.dll",      "vcomp140.dll",               "vccorlib140.dll",  "ucrtbase.dll",
};

static bool is_host_runtime_dll(const char* name) {
    for (u32 i = 0; i < ARRAY_COUNT(k_host_runtime_dlls); i++)
        if (str_ieq(name, k_host_runtime_dlls[i]))
            return true;
    return false;
}

void Loader::init(const wchar_t* title_dir) {
    InitializeCriticalSection(&m_lock);
    str_copy_w(m_title_dir, MAX_PATH, title_dir);
    g_title_thread.init();
    g_nt_query_information_thread =
        (NtQueryInformationThreadFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    if (!tls_reserve_init(&Loader::thread_callback))
        FATAL("TLS slot 0 is not available for the title (see log)");
}

void Loader::set_command_line(const wchar_t* args) {
    SLUDGE_ASSERT(m_main);
    usize cap = wcslen(m_main->path) + (args ? wcslen(args) : 0) + 8;
    m_cmdline_w = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(wchar_t));
    _snwprintf(m_cmdline_w, cap, L"\"%s\"%s%s", m_main->path, (args && *args) ? L" " : L"", args ? args : L"");
    m_cmdline_w[cap - 1] = 0;

    int acap = WideCharToMultiByte(CP_ACP, 0, m_cmdline_w, -1, nullptr, 0, nullptr, nullptr);
    m_cmdline_a = (char*)HeapAlloc(GetProcessHeap(), 0, (usize)acap);
    WideCharToMultiByte(CP_ACP, 0, m_cmdline_w, -1, m_cmdline_a, acap, nullptr, nullptr);

    // Host DLLs (ucrtbase builds argv from this) read the command line from the PEB.
    PEB* peb = (PEB*)__readgsqword(0x60);
    UNICODE_STRING& cl = peb->ProcessParameters->CommandLine;
    cl.Buffer = m_cmdline_w;
    cl.Length = (USHORT)(wcslen(m_cmdline_w) * sizeof(wchar_t));
    cl.MaximumLength = (USHORT)(cl.Length + sizeof(wchar_t));
    LOG_I("loader", "title command line: %s", m_cmdline_a);
}

bool Loader::title_local_path(const char* dll_name, wchar_t* out, usize cap) const {
    wchar_t wname[MAX_PATH];
    str_to_wide(wname, MAX_PATH, dll_name);
    _snwprintf(out, cap, L"%s\\%s", m_title_dir, wname);
    out[cap - 1] = 0;
    DWORD attr = GetFileAttributesW(out);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

PeImage* Loader::find_by_name(const char* dll_name) {
    u32 n = m_count;
    for (u32 i = 0; i < n; i++)
        if (str_ieq(m_images[i].name, dll_name))
            return &m_images[i];
    return nullptr;
}

PeImage* Loader::find_by_handle(HMODULE handle) {
    u32 n = m_count;
    for (u32 i = 0; i < n; i++)
        if (m_images[i].base == (u8*)handle)
            return &m_images[i];
    return nullptr;
}

PeImage* Loader::find_containing(const void* addr) {
    u32 n = m_count;
    for (u32 i = 0; i < n; i++)
        if (m_images[i].contains(addr))
            return &m_images[i];
    return nullptr;
}

void Loader::remember_host_module(HMODULE handle, const char* name) {
    EnterCriticalSection(&m_lock);
    u32 n = m_host_module_count;
    bool known = false;
    for (u32 i = 0; i < n; i++)
        if (m_host_modules[i].handle == handle)
            known = true;
    if (!known && n < k_max_host_modules) {
        m_host_modules[n].handle = handle;
        str_copy(m_host_modules[n].name, sizeof(m_host_modules[n].name), name);
        m_host_module_count = n + 1; // publish after the entry is complete
    }
    LeaveCriticalSection(&m_lock);
}

const char* Loader::host_module_name(HMODULE handle) {
    u32 n = m_host_module_count;
    for (u32 i = 0; i < n; i++)
        if (m_host_modules[i].handle == handle)
            return m_host_modules[i].name;
    return nullptr;
}

ModuleRef Loader::open_module(const char* dll_name) {
    ModuleRef ref;
    const char* name = path_file_name(dll_name);

    ref.hle = hle_find_module(name);
    if (ref.hle) {
        ref.kind = MODULE_HLE;
        return ref;
    }

    wchar_t native_path[MAX_PATH] = {};
    EnterCriticalSection(&m_lock);
    ref.image = find_by_name(name);
    if (!ref.image && !is_host_runtime_dll(name)) {
        wchar_t path[MAX_PATH];
        if (title_local_path(name, path, MAX_PATH)) {
            if (can_load_natively(path))
                str_copy_w(native_path, MAX_PATH, path);
            else
                ref.image = load_image(path, false);
        }
    }
    LeaveCriticalSection(&m_lock);
    if (ref.image) {
        ref.kind = MODULE_TITLE;
        return ref;
    }

    if (native_path[0]) {
        // Outside m_lock: the native loader takes the loader lock, and code running under the
        // loader lock (thread attach) may call back into us. Dependencies are searched in
        // System32 only, so the package's console builds of the VC++ runtime are never picked up.
        ref.host = LoadLibraryExW(native_path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (ref.host) {
            ref.kind = MODULE_HOST;
            remember_host_module(ref.host, name);
            LOG_I("loader", "%s: no console imports, loaded natively at %p", name, (void*)ref.host);
            return ref;
        }
        LOG_E("loader", "%s: native load failed (%lu)", name, GetLastError());
        return ref;
    }

    ref.host = LoadLibraryExA(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ref.host) {
        ref.kind = MODULE_HOST;
        remember_host_module(ref.host, name);
        return ref;
    }
    LOG_E("loader", "module %s not found anywhere (error %lu)", name, GetLastError());
    return ref;
}

void* Loader::resolve(const ModuleRef& module, const char* dll_name, const char* fn_name, u16 ordinal,
                      bool stub_missing) {
    void* fn = nullptr;
    switch (module.kind) {
    case MODULE_HLE:
        fn = hle_find_export(module.hle, fn_name, ordinal);
        break;
    case MODULE_TITLE:
        fn = fn_name ? module.image->find_export(fn_name) : module.image->find_export_ordinal(ordinal);
        break;
    case MODULE_HOST:
        fn = hle_find_host_override(fn_name);
        if (!fn)
            fn = (void*)GetProcAddress(module.host, fn_name ? fn_name : (LPCSTR)(uptr)ordinal);
        break;
    case MODULE_MISSING:
        break;
    }
    if (!fn && stub_missing)
        fn = stub_create(path_file_name(dll_name), fn_name, ordinal);
    return fn;
}

bool Loader::bind_imports(PeImage* image) {
    const IMAGE_DATA_DIRECTORY& d = image->dir(IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (!d.Size)
        return true;
    u32 total = 0, total_stubbed = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* desc = image->at_rva<IMAGE_IMPORT_DESCRIPTOR>(d.VirtualAddress); desc->Name;
         desc++) {
        const char* dll = image->at_rva<const char>(desc->Name);
        ModuleRef module = open_module(dll);

        u32 lookup_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        IMAGE_THUNK_DATA64* lookup = image->at_rva<IMAGE_THUNK_DATA64>(lookup_rva);
        IMAGE_THUNK_DATA64* iat = image->at_rva<IMAGE_THUNK_DATA64>(desc->FirstThunk);
        u32 count = 0, stubbed = 0;
        for (; lookup->u1.AddressOfData; lookup++, iat++, count++) {
            const char* fn_name = nullptr;
            u16 ordinal = 0;
            if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal))
                ordinal = (u16)IMAGE_ORDINAL64(lookup->u1.Ordinal);
            else
                fn_name = (const char*)image->at_rva<IMAGE_IMPORT_BY_NAME>((u32)lookup->u1.AddressOfData)->Name;

            void* fn = resolve(module, dll, fn_name, ordinal, false);
            if (!fn) {
                fn = stub_create(dll, fn_name, ordinal);
                stubbed++;
            }
            iat->u1.Function = (u64)fn;
        }
        static const char* const k_kind_names[] = {"MISSING", "hle", "title", "host"};
        if (stubbed)
            LOG_I("loader", "%s: %-48s -> %-7s %3u imports, %u stubbed", image->name, dll,
                  k_kind_names[module.kind], count, stubbed);
        else
            LOG_D("loader", "%s: %-48s -> %-7s %3u imports", image->name, dll, k_kind_names[module.kind], count);
        total += count;
        total_stubbed += stubbed;
    }
    LOG_I("loader", "%s: bound %u imports, %u stubbed", image->name, total, total_stubbed);
    return true;
}

// The title exe's implicit TLS lives in slot 0, which sludgemu.exe reserves for it (see
// loader/tls_reserve.h), exactly where a native launch would put it. Hand-mapped DLLs can't have
// implicit TLS; title DLLs that don't import console modules are loaded natively and get it from
// the OS.
bool Loader::setup_image_tls(PeImage* image) {
    const IMAGE_DATA_DIRECTORY& d = image->dir(IMAGE_DIRECTORY_ENTRY_TLS);
    if (!d.Size)
        return true;
    if (image->is_dll) {
        LOG_E("loader", "%s has implicit TLS and imports console modules; that combination isn't supported",
              image->name);
        return false;
    }
    IMAGE_TLS_DIRECTORY64* tls = image->at_rva<IMAGE_TLS_DIRECTORY64>(d.VirtualAddress);
    const u8* tmpl = (const u8*)tls->StartAddressOfRawData;
    u32 tmpl_size = (u32)(tls->EndAddressOfRawData - tls->StartAddressOfRawData);
    u32 zero_fill = tls->SizeOfZeroFill;
    if (tmpl_size + zero_fill > tls_reserve_capacity()) {
        LOG_E("loader", "%s needs 0x%x bytes of TLS, only 0x%x reserved", image->name, tmpl_size + zero_fill,
              tls_reserve_capacity());
        return false;
    }
    *(u32*)tls->AddressOfIndex = tls_reserve_index();
    m_tls_template = tmpl;
    m_tls_template_size = tmpl_size;
    m_tls_zero_fill = zero_fill;
    m_tls_image = image; // publish for thread attach
    init_thread_tls();   // the calling thread already exists
    LOG_I("loader", "%s: TLS template 0x%x bytes (+0x%x zero fill) in slot %u", image->name, tmpl_size, zero_fill,
          tls_reserve_index());
    return true;
}

void Loader::init_thread_tls() {
    if (!m_tls_image)
        return;
    u8* block = (u8*)tls_reserve_block();
    memcpy(block, m_tls_template, m_tls_template_size);
    memset(block + m_tls_template_size, 0, m_tls_zero_fill);
}

// A title DLL can go through the native loader when everything it imports is a plain host DLL:
// nothing we implement (HLE) and no other title DLL.
bool Loader::can_load_natively(const wchar_t* path) const {
    char names[128][64];
    u32 count = 0;
    if (!pe_file_import_names(path, names, ARRAY_COUNT(names), &count))
        return false;
    for (u32 i = 0; i < count; i++) {
        if (hle_find_module(names[i]))
            return false;
        wchar_t local[MAX_PATH];
        if (!is_host_runtime_dll(names[i]) && title_local_path(names[i], local, MAX_PATH))
            return false;
    }
    return true;
}

void Loader::call_tls_callbacks(PeImage* image, DWORD reason) {
    const IMAGE_DATA_DIRECTORY& d = image->dir(IMAGE_DIRECTORY_ENTRY_TLS);
    if (!d.Size)
        return;
    IMAGE_TLS_DIRECTORY64* tls = image->at_rva<IMAGE_TLS_DIRECTORY64>(d.VirtualAddress);
    PIMAGE_TLS_CALLBACK* cb = (PIMAGE_TLS_CALLBACK*)tls->AddressOfCallBacks;
    for (; cb && *cb; cb++)
        (*cb)(image->base, reason, nullptr);
}

typedef BOOL(WINAPI* DllEntryFn)(HINSTANCE, DWORD, LPVOID);

// Win32 start address of the calling thread (what was passed to CreateThread).
static void* current_thread_start_address() {
    void* start = nullptr;
    const ULONG k_thread_query_set_win32_start_address = 9;
    if (g_nt_query_information_thread)
        g_nt_query_information_thread(GetCurrentThread(), k_thread_query_set_win32_start_address, &start, sizeof(start),
                                      nullptr);
    return start;
}

void Loader::notify_thread(DWORD reason) {
    if (!m_main)
        return;
    if (reason == DLL_THREAD_ATTACH) {
        // Every thread gets the title images' TLS templates: it's a plain copy and gives correct
        // initial values for any title code that ends up running there.
        init_thread_tls();
        crash_watch_current_thread();
        // Title TLS callbacks / DLL thread notifications only run for threads the title created.
        // Running them on our helper threads or host thread-pool workers executes title CRT code
        // under the loader lock at arbitrary points (e.g. while the title CRT is still starting)
        // and deadlocks. Thread-local objects with dynamic init are still initialized lazily on
        // first access thanks to MSVC's TLS guards.
        bool title_thread = find_containing(current_thread_start_address()) != nullptr;
        g_title_thread.set(title_thread);
        if (!title_thread)
            return;
        call_tls_callbacks(m_main, reason);
    } else if (!g_title_thread.get()) {
        return;
    }
    u32 n = m_count;
    for (u32 i = 0; i < n; i++) {
        PeImage* img = &m_images[i];
        if (img == m_main || !img->initialized)
            continue;
        if (reason == DLL_THREAD_ATTACH)
            call_tls_callbacks(img, reason);
        DllEntryFn entry = (DllEntryFn)img->entry_point();
        if (entry)
            entry((HINSTANCE)img->base, reason, nullptr);
        if (reason == DLL_THREAD_DETACH)
            call_tls_callbacks(img, reason);
    }
    if (reason == DLL_THREAD_DETACH)
        call_tls_callbacks(m_main, reason);
}

void Loader::thread_callback(u32 reason) { g_loader.notify_thread(reason); }

PeImage* Loader::load_image(const wchar_t* path, bool is_main) {
    if (m_count >= k_max_images) {
        LOG_E("loader", "too many images");
        return nullptr;
    }
    PeImage* image = &m_images[m_count];
    if (!image->map_from_file(path))
        return nullptr;
    // Publish before binding imports so import cycles and find_containing() see it.
    m_count = m_count + 1;
    if (is_main)
        m_main = image;

    image->register_unwind_info();
    if (!setup_image_tls(image))
        return nullptr;

    if (!bind_imports(image))
        return nullptr;
    image->protect_sections();

    if (!is_main) {
        call_tls_callbacks(image, DLL_PROCESS_ATTACH); // native order: TLS callbacks, then DllMain
        DllEntryFn entry = (DllEntryFn)image->entry_point();
        if (entry) {
            LOG_D("loader", "%s: DllMain(PROCESS_ATTACH)", image->name);
            if (!entry((HINSTANCE)image->base, DLL_PROCESS_ATTACH, nullptr)) {
                LOG_E("loader", "%s: DllMain(PROCESS_ATTACH) failed", image->name);
                return nullptr;
            }
        }
        image->initialized = true;
    }
    return image;
}

PeImage* Loader::load_main(const wchar_t* exe_path) {
    EnterCriticalSection(&m_lock);
    PeImage* image = load_image(exe_path, true);
    LeaveCriticalSection(&m_lock);
    return image;
}

void Loader::run_main() {
    SLUDGE_ASSERT(m_main);
    LOG_I("loader", "%s: running TLS callbacks", m_main->name);
    call_tls_callbacks(m_main, DLL_PROCESS_ATTACH);
    m_main->initialized = true;

    void* entry = m_main->entry_point();
    char where[64];
    snprintf(where, sizeof(where), "0x%llx", (unsigned long long)m_main->ida_address(entry));
    LOG_I("loader", "%s: jumping to entry point %s", m_main->name, where);
    log_flush();

    // CRT entry points take no meaningful arguments; the native loader passes the PEB.
    typedef int(__cdecl * ExeEntryFn)(void* peb);
    int rc = ((ExeEntryFn)entry)((void*)__readgsqword(0x60));
    LOG_I("loader", "title entry point returned %d", rc);
    log_shutdown();
    ExitProcess((UINT)rc);
}
