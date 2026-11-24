#include <windows.h>
#include <intrin.h>

#include "tls/sludge_tls.h"

// Provided by the CRT's TLS support (tlssup.obj): our implicit TLS slot.
extern "C" ULONG _tls_index;
extern "C" IMAGE_DOS_HEADER __ImageBase;

// Titles typically need a few KiB; keep generous headroom.
#define SLUDGE_TLS_RESERVE (64 * 1024)

__declspec(thread) __declspec(align(64)) static unsigned char t_reserve[SLUDGE_TLS_RESERVE];

static volatile SludgeTlsThreadCallback g_callback = nullptr;
static uint32_t g_capacity = 0;

static void compute_capacity() {
    // The slot's block is a copy of our whole TLS template (StartAddressOfRawData ..
    // EndAddressOfRawData + SizeOfZeroFill), so that is how much the title may use.
    const unsigned char* base = (const unsigned char*)&__ImageBase;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + __ImageBase.e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    const IMAGE_TLS_DIRECTORY64* tls = (const IMAGE_TLS_DIRECTORY64*)(base + dir.VirtualAddress);
    g_capacity = (uint32_t)(tls->EndAddressOfRawData - tls->StartAddressOfRawData) + tls->SizeOfZeroFill;
}

uint32_t sludge_tls_index(void) { return (uint32_t)_tls_index; }

uint32_t sludge_tls_capacity(void) {
    t_reserve[0] = t_reserve[0]; // keep the reserve referenced so the linker can't drop it
    return g_capacity;
}

void* sludge_tls_block(void) {
    void** slots = (void**)__readgsqword(0x58); // TEB->ThreadLocalStoragePointer
    return slots[_tls_index];
}

void sludge_tls_set_thread_callback(SludgeTlsThreadCallback callback) { g_callback = callback; }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        compute_capacity();
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH: {
        SludgeTlsThreadCallback cb = g_callback;
        if (cb)
            cb((uint32_t)reason);
        break;
    }
    default:
        break;
    }
    return TRUE;
}
