#include "core/vfs.h"

#include <windows.h>

#include "core/title.h"

static bool is_separator(wchar_t c) {
    return c == L'\\' || c == L'/';
}

static bool is_package_path(const wchar_t* path) {
    return (path[0] == L'G' || path[0] == L'g') && path[1] == L':' && (path[2] == 0 || is_separator(path[2]));
}

bool vfs_translate(const wchar_t* path, wchar_t* out, usize cap) {
    if (!is_package_path(path)) {
        usize len = wcslen(path);
        if (len + 1 > cap)
            return false;
        memcpy(out, path, (len + 1) * sizeof(wchar_t));
        return true;
    }

    usize n = wcslen(g_title.dir);
    if (n + 1 > cap)
        return false;
    memcpy(out, g_title.dir, n * sizeof(wchar_t));
    if (n && is_separator(out[n - 1]))
        n--;

    for (const wchar_t* p = path + 2; *p; p++) {
        wchar_t c = is_separator(*p) ? L'\\' : *p;
        if (c == L'\\' && n && out[n - 1] == L'\\')
            continue;
        if (n + 2 > cap)
            return false;
        out[n++] = c;
    }
    out[n] = 0;
    return true;
}
