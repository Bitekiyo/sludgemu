#pragma once

#include <windows.h>
#include <d3d12.h>

#include "common/types.h"

// Console graphics objects.
//
// Every console D3D12 interface is a plain object whose first member points to a function table;
// the title's (inlined) interface wrappers call table entries as C functions with the object as
// the first argument. Methods are not C++ virtuals, the entry order differs from desktop D3D12,
// and some interfaces have public data members right after this header that title code reads
// directly. So each object we hand out starts with ConsoleObjectHeader, then the interface's
// public members, then our type-specific state.
//
// Our bookkeeping (class, backing desktop object) lives in a ConsolePrivate placed immediately
// *before* the object, so the title-visible layout stays exactly the console one and the common
// entries (IUnknown, ID3D12Object, ID3D12DeviceChild) are shared by all types.
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

struct ConsoleClass {
    const char* name;
    const GUID* const* iids; // interfaces the object answers to (IUnknown is implied)
    u32 iid_count;
    void (*destroy)(void* object); // optional: releases type-specific state before the memory goes
};

struct ConsolePrivate {
    const ConsoleClass* klass;
    ID3D12Object* desktop; // owned reference to the backing desktop object, or nullptr
};

inline ConsolePrivate* console_private(const void* object) {
    return (ConsolePrivate*)object - 1;
}

struct ConsoleEntry {
    u16 index;
    void* fn;
};

#define CONSOLE_ENTRY(index, fn) {index, (void*)&fn}

// Builds a function table of `count` entries: the common entries (0-7 where the table has them),
// then `impl` (which may override them), and a logging stub named "<interface>::<entry>"
// returning E_NOTIMPL for everything else. Tables live for the lifetime of the process.
void* const* console_build_table(const char* interface_name, const char* const* entry_names, u32 count,
                                 const ConsoleEntry* impl, u32 impl_count);

// Allocates a zeroed object of `size` bytes (public layout + state) with one reference.
// Takes ownership of `desktop`.
void* console_alloc(usize size, const ConsoleClass* klass, void* const* table, ID3D12Object* desktop);

// Hands a freshly created object to the title as `iid`: on success *out holds it; either way the
// creation reference is dropped.
HRESULT console_return(void* object, REFIID iid, void** out);

// QueryInterface on an existing object.
HRESULT console_query(void* object, REFIID iid, void** out);

// The console device (set once by device creation); GetDevice entries return it.
void console_set_device(void* device);
