#pragma once

#include <windows.h>

#include "common/types.h"

// The Game Runtime (xgameruntime.dll) ABI.
//
// Titles link a static library of thunks for the flat runtime API (XUserAddAsync,
// XTaskQueueCreate, ...). Each thunk asks the runtime DLL for an interface with
// QueryApiImpl(provider, iid, &object), calls one vtable method with the original arguments
// (object pointer first), then releases the object. Slots 0-2 are IUnknown.
//
// We describe each vtable as a RuntimeApi: the provider it belongs to, the interface IDs it
// serves (a base interface plus interfaces derived from it share one vtable) and its methods by
// slot. A method without an implementation becomes a logging stub returning E_NOTIMPL.
//
// Every implementation takes the interface object as its first parameter (unused: all our
// interface objects are stateless singletons), followed by the flat API's parameters.

struct RuntimeMethod {
    u16 slot;
    const char* name; // flat API name, for logs
    void* fn;         // nullptr: not implemented yet
};

struct RuntimeApi {
    const char* name;
    const GUID* provider;
    const GUID* iids;
    u32 iid_count;
    const RuntimeMethod* methods;
    u32 method_count;
};

#define RUNTIME_API(name, provider, iids, methods)                                                           \
    {name, provider, iids, ARRAY_COUNT(iids), methods, ARRAY_COUNT(methods)}

// The `this` parameter of runtime interface methods.
typedef void* RuntimeSelf;
