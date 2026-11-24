#pragma once

#include <windows.h>

#include "common/types.h"

// Console graphics objects.
//
// Every console D3D12 interface is a plain object whose first member points to a function table;
// the title's (inlined) interface wrappers call table entries as C functions with the object as
// the first argument. Methods are not C++ virtuals, the entry order differs from desktop D3D12,
// and some interfaces have public data members right after this header that title code reads
// directly. So each object we hand out starts with ConsoleObjectHeader, then the interface's
// public members, then our private state.
//
// Calling convention: entries are C functions, so an entry returning a struct larger than 8 bytes
// receives the hidden return pointer first and the object second, unlike a desktop COM method.
// Never forward a title call straight into a desktop vtable.

struct ConsoleObjectHeader {
    void* const* function_table;
    u32 flags; // private data present (bit 0), collective refcount disabled (bit 1), internal
    u32 ref_count;
};
static_assert(sizeof(ConsoleObjectHeader) == 16, "console object header layout");

struct ConsoleEntry {
    u16 index;
    void* fn;
};

#define CONSOLE_ENTRY(index, fn) {index, (void*)&fn}

// Builds a function table of `count` entries: the implemented ones from `impl`, every other entry
// a logging stub named "<interface>::<entry>" that returns E_NOTIMPL. Tables live for the
// lifetime of the process.
void* const* console_build_table(const char* interface_name, const char* const* entry_names, u32 count,
                                 const ConsoleEntry* impl, u32 impl_count);
