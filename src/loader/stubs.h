#pragma once

#include "common/types.h"

// Logging stubs for imports we can't resolve. Each stub is a tiny thunk that logs
// "module!function called from <addr>" (first few calls) and returns 0.

void stubs_init(bool break_on_call);

// ordinal is used when name is null.
void* stub_create(const char* module, const char* name, u16 ordinal);

// Number of stubs that have been hit at least once.
u32 stubs_hit_count();
