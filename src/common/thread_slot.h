#pragma once

#include <windows.h>

#include "common/log.h"
#include "common/types.h"

// Per-thread value in a dynamic TLS slot (TlsAlloc). sludgemu.exe must not use implicit TLS
// (thread_local / __declspec(thread)): its implicit TLS slot belongs to the title, see
// loader/tls_reserve.h.
class ThreadSlot {
public:
    void init() {
        m_index = TlsAlloc();
        if (m_index == TLS_OUT_OF_INDEXES)
            FATAL("out of dynamic TLS slots");
    }
    uptr get() const { return (uptr)TlsGetValue(m_index); }
    void set(uptr value) const { TlsSetValue(m_index, (void*)value); }

private:
    DWORD m_index = 0;
};
