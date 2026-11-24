#include "loader/loader.h"

#include <winternl.h>
#include <stdio.h>

#include "common/log.h"
#include "common/str.h"
#include "hle/hle.h"
#include "loader/stubs.h"
#include "tls/sludge_tls.h"

Loader g_loader;

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
    sludge_tls_set_thread_callback(&Loader::thread_callback);
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

ModuleRef Loader::open_module(const char* dll_name) {
    ModuleRef ref;
    const char* name = path_file_name(dll_name);

    ref.hle = hle_find_module(name);
    if (ref.hle) {
        ref.kind = MODULE_HLE;
        return ref;
    }

    EnterCriticalSection(&m_lock);
    ref.image = find_by_name(name);
    if (!ref.image && !is_host_runtime_dll(name)) {
        wchar_t path[MAX_PATH];
        if (title_local_path(name, path, MAX_PATH))
            ref.image = load_image(path, false);
    }
    LeaveCriticalSection(&m_lock);
    if (ref.image) {
        ref.kind = MODULE_TITLE;
        return ref;
    }

    ref.host = LoadLibraryExA(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ref.host) {
        ref.kind = MODULE_HOST;
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

bool Loader::setup_main_tls(PeImage* image) {
    const IMAGE_DATA_DIRECTORY& d = image->dir(IMAGE_DIRECTORY_ENTRY_TLS);
    if (!d.Size)
        return true;
    IMAGE_TLS_DIRECTORY64* tls = image->at_rva<IMAGE_TLS_DIRECTORY64>(d.VirtualAddress);
    m_tls_template = (const u8*)tls->StartAddressOfRawData;
    m_tls_template_size = (u32)(tls->EndAddressOfRawData - tls->StartAddressOfRawData);
    m_tls_zero_fill = tls->SizeOfZeroFill;

    u32 needed = m_tls_template_size + m_tls_zero_fill;
    u32 capacity = sludge_tls_capacity();
    if (needed > capacity) {
        LOG_E("loader", "%s needs 0x%x bytes of TLS, only 0x%x reserved", image->name, needed, capacity);
        return false;
    }
    *(u32*)tls->AddressOfIndex = sludge_tls_index();
    m_tls_ready = true;
    init_thread_tls(); // the calling (main) thread already exists
    LOG_I("loader", "%s: TLS template 0x%x bytes (+0x%x zero fill) in slot %u", image->name, m_tls_template_size,
          m_tls_zero_fill, sludge_tls_index());
    return true;
}

void Loader::init_thread_tls() {
    u8* block = (u8*)sludge_tls_block();
    memcpy(block, m_tls_template, m_tls_template_size);
    memset(block + m_tls_template_size, 0, m_tls_zero_fill);
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

void Loader::notify_thread(DWORD reason) {
    if (!m_main)
        return;
    if (reason == DLL_THREAD_ATTACH) {
        if (m_tls_ready)
            init_thread_tls();
        call_tls_callbacks(m_main, reason);
    }
    u32 n = m_count;
    for (u32 i = 0; i < n; i++) {
        PeImage* img = &m_images[i];
        if (img == m_main || !img->initialized)
            continue;
        DllEntryFn entry = (DllEntryFn)img->entry_point();
        if (entry)
            entry((HINSTANCE)img->base, reason, nullptr);
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
    if (is_main) {
        if (!setup_main_tls(image))
            return nullptr;
    } else if (image->dir(IMAGE_DIRECTORY_ENTRY_TLS).Size) {
        LOG_W("loader", "%s has implicit TLS; only the main image's TLS is supported so far", image->name);
    }

    if (!bind_imports(image))
        return nullptr;
    image->protect_sections();

    if (!is_main) {
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
