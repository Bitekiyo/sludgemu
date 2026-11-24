#pragma once

#include "common/types.h"

// Small string helpers. ASCII-only case folding: module/function names in PE files are ASCII.

bool str_ieq(const char* a, const char* b);
bool str_ieq_w(const wchar_t* a, const wchar_t* b);
bool str_ends_with_i(const char* s, const char* suffix);

// Always NUL-terminates (if cap > 0). Returns the number of chars copied.
usize str_copy(char* dst, usize cap, const char* src);
usize str_copy_w(wchar_t* dst, usize cap, const wchar_t* src);

// Lossy narrow/widen for ASCII-ish names and paths used in logs.
usize str_to_wide(wchar_t* dst, usize cap, const char* src);
usize str_to_narrow(char* dst, usize cap, const wchar_t* src);

// Pointer to the file name part of a path (after the last '\\' or '/').
const char* path_file_name(const char* path);
const wchar_t* path_file_name_w(const wchar_t* path);

// Copies the directory part of `path` (without trailing separator) into dst.
void path_dir_w(wchar_t* dst, usize cap, const wchar_t* path);
