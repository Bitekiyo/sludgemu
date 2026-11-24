#pragma once

#include "common/types.h"

// Console file-system paths as seen by the title, mapped to host paths.
//
// On console the title's package is mounted at G:\. Everything else a title touches comes from
// runtime APIs (persistent local storage, save data, ...) which hand out host paths directly, so
// only the package drive needs translating. Separators are normalized and repeated separators
// collapsed, since titles build paths like "G://settings/".

// Writes the host path for `path` to `out`. Paths that are not console paths are copied
// unchanged. Returns false if the result doesn't fit.
bool vfs_translate(const wchar_t* path, wchar_t* out, usize cap);
