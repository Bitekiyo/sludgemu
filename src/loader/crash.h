#pragma once

#include "common/types.h"

// Exception logging + minidumps.
void crash_install(const wchar_t* dump_path);

// "title.exe+0x1234 [0x140001234]" for title images,
// "kernelbase.dll+0x5678" for host modules, raw pointer otherwise.
void describe_address(const void* addr, char* out, usize cap);
