#include "core/title.h"

#include <stdio.h>
#include <stdlib.h>

#include "common/log.h"
#include "common/str.h"

// Reads a whole small file into a NUL-terminated heap buffer.
static char* read_text_file(const wchar_t* path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return nullptr;
    LARGE_INTEGER size;
    char* buf = nullptr;
    if (GetFileSizeEx(f, &size) && size.QuadPart < (LONGLONG)MiB(4)) {
        buf = (char*)HeapAlloc(GetProcessHeap(), 0, (usize)size.QuadPart + 1);
        DWORD got = 0;
        if (buf && ReadFile(f, buf, (DWORD)size.QuadPart, &got, nullptr)) {
            buf[got] = 0;
        } else if (buf) {
            HeapFree(GetProcessHeap(), 0, buf);
            buf = nullptr;
        }
    }
    CloseHandle(f);
    return buf;
}

// Finds `<tag ` and returns a pointer to it, or nullptr.
static const char* xml_find_tag(const char* xml, const char* tag) {
    char needle[64];
    snprintf(needle, sizeof(needle), "<%s", tag);
    usize n = strlen(needle);
    for (const char* p = strstr(xml, needle); p; p = strstr(p + 1, needle)) {
        char c = p[n];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/')
            return p;
    }
    return nullptr;
}

// Copies attribute `attr` of the tag starting at `tag` (UTF-8). Good enough for the flat,
// tool-generated config files; not a general XML parser.
static bool xml_attr(const char* tag, const char* attr, char* out, usize cap) {
    const char* end = strchr(tag, '>');
    if (!end)
        return false;
    usize alen = strlen(attr);
    for (const char* p = tag; p + alen + 2 < end; p++) {
        if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r') &&
            strncmp(p, attr, alen) == 0 && p[alen] == '=' && p[alen + 1] == '"') {
            const char* v = p + alen + 2;
            const char* q = strchr(v, '"');
            if (!q || q > end)
                return false;
            usize len = (usize)(q - v);
            if (len >= cap)
                len = cap - 1;
            memcpy(out, v, len);
            out[len] = 0;
            return true;
        }
    }
    return false;
}

// Copies the text content of the element starting at `tag` (up to the next '<').
static bool xml_text(const char* tag, char* out, usize cap) {
    const char* v = tag ? strchr(tag, '>') : nullptr;
    if (!v)
        return false;
    usize i = 0;
    for (v++; i + 1 < cap && *v && *v != '<'; v++)
        out[i++] = *v;
    out[i] = 0;
    return true;
}

static void set_error(wchar_t* error, usize cap, const wchar_t* fmt, const wchar_t* arg) {
    if (error && cap) {
        _snwprintf(error, cap, fmt, arg);
        error[cap - 1] = 0;
    }
}

TitleInfo g_title;

bool title_resolve(const wchar_t* path, TitleInfo* out, wchar_t* error, usize error_cap) {
    memset(out, 0, sizeof(*out));

    wchar_t full[MAX_PATH];
    if (!GetFullPathNameW(path, MAX_PATH, full, nullptr)) {
        set_error(error, error_cap, L"Invalid path: %s", path);
        return false;
    }
    usize len = wcslen(full);
    while (len > 3 && (full[len - 1] == L'\\' || full[len - 1] == L'/'))
        full[--len] = 0;

    DWORD attr = GetFileAttributesW(full);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        set_error(error, error_cap, L"Path does not exist:\n%s", full);
        return false;
    }
    bool is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (is_dir)
        str_copy_w(out->dir, MAX_PATH, full);
    else
        path_dir_w(out->dir, MAX_PATH, full);

    wchar_t config_path[MAX_PATH];
    _snwprintf(config_path, MAX_PATH, L"%s\\MicrosoftGame.Config", out->dir);
    config_path[MAX_PATH - 1] = 0;
    char* config = read_text_file(config_path);

    char exe_name[MAX_PATH] = {};
    char display[512] = {};
    if (config) {
        if (const char* exe_tag = xml_find_tag(config, "Executable")) {
            xml_attr(exe_tag, "Name", exe_name, sizeof(exe_name));
            xml_attr(exe_tag, "OverrideDisplayName", display, sizeof(display));
        }
        if (!display[0]) {
            if (const char* sv = xml_find_tag(config, "ShellVisuals"))
                xml_attr(sv, "DefaultDisplayName", display, sizeof(display));
        }
        xml_text(xml_find_tag(config, "TitleId"), out->title_id, sizeof(out->title_id));
        if (const char* pls = xml_find_tag(config, "PersistentLocalStorage")) {
            char size_mb[32];
            if (xml_text(xml_find_tag(pls, "SizeMB"), size_mb, sizeof(size_mb)))
                out->pls_quota = strtoull(size_mb, nullptr, 10) << 20;
        }
        HeapFree(GetProcessHeap(), 0, config);
    } else {
        LOG_W("title", "no MicrosoftGame.Config in package directory");
    }

    if (is_dir) {
        if (!exe_name[0]) {
            set_error(error, error_cap, L"No executable listed in MicrosoftGame.Config in:\n%s", out->dir);
            return false;
        }
        wchar_t wexe[MAX_PATH];
        str_to_wide(wexe, MAX_PATH, exe_name);
        _snwprintf(out->exe_path, MAX_PATH, L"%s\\%s", out->dir, wexe);
        out->exe_path[MAX_PATH - 1] = 0;
    } else {
        str_copy_w(out->exe_path, MAX_PATH, full);
    }

    if (GetFileAttributesW(out->exe_path) == INVALID_FILE_ATTRIBUTES) {
        set_error(error, error_cap, L"Title executable not found:\n%s", out->exe_path);
        return false;
    }

    if (display[0])
        str_to_wide(out->display_name, ARRAY_COUNT(out->display_name), display);
    else
        str_copy_w(out->display_name, ARRAY_COUNT(out->display_name), path_file_name_w(out->exe_path));
    return true;
}

bool title_data_dir(const wchar_t* name, wchar_t* out, usize cap) {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return false;
    wchar_t title_id[16];
    str_to_wide(title_id, ARRAY_COUNT(title_id), g_title.title_id[0] ? g_title.title_id : "unknown");

    // Create each level; CreateDirectoryW fails harmlessly when it already exists.
    const wchar_t* levels[3] = {L"sludgemu", title_id, name};
    usize len = (usize)_snwprintf(out, cap, L"%s", base);
    for (u32 i = 0; i < 3; i++) {
        int written = _snwprintf(out + len, cap - len, L"\\%s", levels[i]);
        if (written < 0 || len + (usize)written + 2 > cap)
            return false;
        len += (usize)written;
        if (!CreateDirectoryW(out, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }
    out[len++] = L'\\';
    out[len] = 0;
    return true;
}
