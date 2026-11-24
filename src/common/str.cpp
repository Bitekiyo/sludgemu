#include "common/str.h"

#include <windows.h>

static inline char lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static inline wchar_t lower_ascii_w(wchar_t c) { return (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c; }

bool str_ieq(const char* a, const char* b) {
    for (;; a++, b++) {
        char ca = lower_ascii(*a), cb = lower_ascii(*b);
        if (ca != cb)
            return false;
        if (!ca)
            return true;
    }
}

bool str_ieq_w(const wchar_t* a, const wchar_t* b) {
    for (;; a++, b++) {
        wchar_t ca = lower_ascii_w(*a), cb = lower_ascii_w(*b);
        if (ca != cb)
            return false;
        if (!ca)
            return true;
    }
}

bool str_ends_with_i(const char* s, const char* suffix) {
    usize ls = strlen(s), lx = strlen(suffix);
    if (lx > ls)
        return false;
    return str_ieq(s + ls - lx, suffix);
}

usize str_copy(char* dst, usize cap, const char* src) {
    if (!cap)
        return 0;
    usize i = 0;
    for (; i + 1 < cap && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
    return i;
}

usize str_copy_w(wchar_t* dst, usize cap, const wchar_t* src) {
    if (!cap)
        return 0;
    usize i = 0;
    for (; i + 1 < cap && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
    return i;
}

usize str_to_wide(wchar_t* dst, usize cap, const char* src) {
    if (!cap)
        return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap);
    if (n <= 0) {
        dst[0] = 0;
        return 0;
    }
    dst[cap - 1] = 0;
    return (usize)n - 1;
}

usize str_to_narrow(char* dst, usize cap, const wchar_t* src) {
    if (!cap)
        return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)cap, nullptr, nullptr);
    if (n <= 0) {
        dst[0] = 0;
        return 0;
    }
    dst[cap - 1] = 0;
    return (usize)n - 1;
}

const char* path_file_name(const char* path) {
    const char* r = path;
    for (const char* p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            r = p + 1;
    return r;
}

const wchar_t* path_file_name_w(const wchar_t* path) {
    const wchar_t* r = path;
    for (const wchar_t* p = path; *p; p++)
        if (*p == L'\\' || *p == L'/')
            r = p + 1;
    return r;
}

void path_dir_w(wchar_t* dst, usize cap, const wchar_t* path) {
    const wchar_t* name = path_file_name_w(path);
    usize len = (usize)(name - path);
    if (len > 0)
        len--; // drop separator
    if (len >= cap)
        len = cap - 1;
    memcpy(dst, path, len * sizeof(wchar_t));
    dst[len] = 0;
}
