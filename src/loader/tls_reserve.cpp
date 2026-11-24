#include "loader/tls_reserve.h"

#include <windows.h>
#include <intrin.h>

#include "common/log.h"

// Provided by the CRT's TLS support (tlssup.obj).
extern "C" ULONG _tls_index;
extern "C" IMAGE_DOS_HEADER __ImageBase;

#define TITLE_TLS_RESERVE (64 * 1024)
// The CRT's _tls_start/_tls_end markers and alignment padding around the reserve.
#define TLS_RESERVE_SLACK 256

// sludgemu.exe's only thread-local variable.
__declspec(thread) __declspec(align(64)) static u8 t_title_tls_reserve[TITLE_TLS_RESERVE];

static volatile TlsThreadCallback g_callback = nullptr;
static u32 g_capacity = 0;

// Executable TLS callback: the native loader calls it for every thread attach/detach.
static void NTAPI tls_callback(PVOID, DWORD reason, PVOID) {
    if (reason != DLL_THREAD_ATTACH && reason != DLL_THREAD_DETACH)
        return;
    TlsThreadCallback cb = g_callback;
    if (cb)
        cb((u32)reason);
}

// Registers tls_callback in the image's TLS callback array (.CRT$XLB sits between the CRT's
// .CRT$XLA / .CRT$XLZ markers) and forces the TLS directory to be emitted.
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:g_sludge_tls_callback")
#pragma const_seg(".CRT$XLB")
extern "C" const PIMAGE_TLS_CALLBACK g_sludge_tls_callback = tls_callback;
#pragma const_seg()

bool tls_reserve_init(TlsThreadCallback callback) {
    const u8* base = (const u8*)&__ImageBase;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + __ImageBase.e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    const IMAGE_TLS_DIRECTORY64* tls = (const IMAGE_TLS_DIRECTORY64*)(base + dir.VirtualAddress);
    g_capacity = (u32)(tls->EndAddressOfRawData - tls->StartAddressOfRawData) + tls->SizeOfZeroFill;

    // The reserve must be the start of the block and essentially all of it: anything else in
    // sludgemu.exe's TLS would be overwritten by the title's template.
    const u8* block = (const u8*)tls_reserve_block();
    const u8* reserve = &t_title_tls_reserve[0];
    if (_tls_index != 0) {
        LOG_E("tls", "sludgemu.exe got TLS index %lu, expected 0", _tls_index);
        return false;
    }
    if (reserve < block || reserve - block > TLS_RESERVE_SLACK || g_capacity > TITLE_TLS_RESERVE + TLS_RESERVE_SLACK) {
        LOG_E("tls", "sludgemu.exe uses implicit TLS besides the title reserve (block %p, reserve %p, size 0x%x)",
              block, reserve, g_capacity);
        return false;
    }
    g_callback = callback;
    return true;
}

u32 tls_reserve_index() { return 0; }
u32 tls_reserve_capacity() { return g_capacity; }

void* tls_reserve_block() {
    void** slots = (void**)__readgsqword(0x58); // TEB->ThreadLocalStoragePointer
    return slots[0];
}
