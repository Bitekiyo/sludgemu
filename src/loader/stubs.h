#pragma once

#include "common/types.h"

// Logging stubs for imports we can't resolve. Each stub is a tiny thunk that logs
// "module!function called from <addr>" (first few calls) and returns a fixed value.

void stubs_init(bool break_on_call);

// ordinal is used when name is null. The stub returns 0.
void* stub_create(const char* module, const char* name, u16 ordinal);

// Same, returning a fixed value (e.g. E_NOTIMPL for COM-style methods, where 0 means success).
void* stub_create_ex(const char* module, const char* name, u16 ordinal, u64 return_value);

// Number of stubs that have been hit at least once.
u32 stubs_hit_count();
