#pragma once

#include "common/types.h"

// Implicit TLS for the title executable.
//
// Compilers address an executable's thread-local variables with a hard-coded TLS index of 0,
// because the Windows loader always gives the main executable slot 0. The title exe therefore
// has to own slot 0. sludgemu.exe's own implicit TLS consists of nothing but a large reserve
// block, so slot 0 (sludgemu.exe's) is handed to the title: on every thread attach the loader
// copies the title's TLS template into the slot-0 block, just as the native loader would.
//
// sludgemu.exe must not use implicit TLS for anything else (no thread_local / __declspec(thread),
// built with /Zc:threadSafeInit-). tls_reserve_init() verifies that.

typedef void (*TlsThreadCallback)(u32 reason); // DLL_THREAD_ATTACH / DLL_THREAD_DETACH

// Checks the slot-0 invariants and installs the thread attach/detach callback.
bool tls_reserve_init(TlsThreadCallback callback);

u32 tls_reserve_index(); // always 0
u32 tls_reserve_capacity();
void* tls_reserve_block(); // calling thread's slot-0 block
