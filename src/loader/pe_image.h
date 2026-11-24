#pragma once

#include <windows.h>

#include "common/types.h"

// A PE32+ image mapped by hand into our process.
class PeImage {
public:
    // Reads the file and maps headers + sections, preferring the image's own base address so
    // that addresses match the disassembler. Applies base relocations if it can't.
    bool map_from_file(const wchar_t* path);
    void unmap();

    // Sets final page protections from section characteristics. Call after imports are bound.
    void protect_sections();

    // Registers .pdata with the OS so SEH / C++ exceptions / stack walks work in the image.
    bool register_unwind_info();

    void* find_export(const char* name) const;
    void* find_export_ordinal(u16 ordinal) const;

    bool contains(const void* p) const { return (const u8*)p >= base && (const u8*)p < base + size; }
    u32 rva_of(const void* p) const { return (u32)((const u8*)p - base); }

    // Address as the disassembler sees it (preferred base + RVA).
    u64 ida_address(const void* p) const { return preferred_base + rva_of(p); }

    IMAGE_NT_HEADERS64* nt() const { return (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew); }
    const IMAGE_DATA_DIRECTORY& dir(int index) const { return nt()->OptionalHeader.DataDirectory[index]; }

    template <typename T> T* at_rva(u32 rva) const { return (T*)(base + rva); }

    void* entry_point() const;

    u8* base = nullptr;
    u32 size = 0;
    u64 preferred_base = 0;
    bool is_dll = false;
    bool relocated = false;
    bool initialized = false;
    wchar_t path[MAX_PATH] = {};
    char name[64] = {}; // lowercase file name, e.g. "bink2scarlett.dll"

private:
    bool apply_relocations(s64 delta);
};
