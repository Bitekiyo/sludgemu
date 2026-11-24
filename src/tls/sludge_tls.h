#pragma once

// sludge_tls.dll — gives a manually mapped title image working implicit (static) TLS.
//
// Windows only allocates implicit TLS slots for images loaded by the native loader. This DLL is
// loaded natively, owns one slot with a large static TLS block, and the launcher points the
// title's _tls_index at that slot. On every thread attach/detach it forwards to a callback in the
// launcher, which copies the title's TLS template into the block and runs the title's TLS
// callbacks.

#include <stdint.h>

#ifdef SLUDGE_TLS_BUILD
#define SLUDGE_TLS_API extern "C" __declspec(dllexport)
#else
#define SLUDGE_TLS_API extern "C" __declspec(dllimport)
#endif

typedef void (*SludgeTlsThreadCallback)(uint32_t reason); // DLL_THREAD_ATTACH / DLL_THREAD_DETACH

// TLS slot index owned by this DLL.
SLUDGE_TLS_API uint32_t sludge_tls_index(void);

// Usable bytes in the block (from the start of the slot's block).
SLUDGE_TLS_API uint32_t sludge_tls_capacity(void);

// Start of the calling thread's block.
SLUDGE_TLS_API void* sludge_tls_block(void);

SLUDGE_TLS_API void sludge_tls_set_thread_callback(SludgeTlsThreadCallback callback);
