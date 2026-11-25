#include "hle/d3d12/console_object.h"

#include <stdio.h>
#include <stdlib.h>

#include "common/log.h"
#include "loader/stubs.h"

static void* g_device = nullptr;

void console_set_device(void* device) {
    g_device = device;
}

// --- common entries: IUnknown, ID3D12Object, ID3D12DeviceChild ---------------------------------

static HRESULT WINAPI object_query_interface(void* self, REFIID iid, void** out) {
    const ConsoleClass* klass = console_private(self)->klass;
    bool known = iid == __uuidof(IUnknown);
    for (u32 i = 0; !known && i < klass->iid_count; i++)
        known = iid == *klass->iids[i];
    if (!known) {
        *out = nullptr;
        LOG_W("d3d12", "%s::QueryInterface: unknown interface {%08lx-...}", klass->name, iid.Data1);
        return E_NOINTERFACE;
    }
    InterlockedIncrement((volatile long*)&((ConsoleObjectHeader*)self)->ref_count);
    *out = self;
    return S_OK;
}

static ULONG WINAPI object_add_ref(void* self) {
    return (ULONG)InterlockedIncrement((volatile long*)&((ConsoleObjectHeader*)self)->ref_count);
}

static ULONG WINAPI object_release(void* self) {
    long refs = InterlockedDecrement((volatile long*)&((ConsoleObjectHeader*)self)->ref_count);
    if (refs == 0) {
        ConsolePrivate* priv = console_private(self);
        if (priv->klass->destroy)
            priv->klass->destroy(self);
        if (priv->desktop)
            priv->desktop->Release();
        free(priv);
    }
    return (ULONG)refs;
}

static HRESULT WINAPI object_get_private_data(void* self, REFGUID guid, UINT* size, void* data) {
    ID3D12Object* desktop = console_private(self)->desktop;
    return desktop ? desktop->GetPrivateData(guid, size, data) : E_NOTIMPL;
}

static HRESULT WINAPI object_set_private_data(void* self, REFGUID guid, UINT size, const void* data) {
    ID3D12Object* desktop = console_private(self)->desktop;
    return desktop ? desktop->SetPrivateData(guid, size, data) : E_NOTIMPL;
}

// The interface is a console object; its IUnknown entries line up with a COM vtable on x64, so
// the desktop runtime can hold and release it.
static HRESULT WINAPI object_set_private_data_interface(void* self, REFGUID guid, const IUnknown* data) {
    ID3D12Object* desktop = console_private(self)->desktop;
    return desktop ? desktop->SetPrivateDataInterface(guid, data) : E_NOTIMPL;
}

static HRESULT WINAPI object_set_name(void* self, LPCWSTR name) {
    ID3D12Object* desktop = console_private(self)->desktop;
    return desktop ? desktop->SetName(name) : S_OK;
}

static HRESULT WINAPI object_get_device(void* self, REFIID iid, void** out) {
    if (!g_device) {
        *out = nullptr;
        return E_FAIL;
    }
    return object_query_interface(g_device, iid, out);
}

static const ConsoleEntry k_common_entries[] = {
    CONSOLE_ENTRY(0, object_query_interface),  CONSOLE_ENTRY(1, object_add_ref),
    CONSOLE_ENTRY(2, object_release),          CONSOLE_ENTRY(3, object_get_private_data),
    CONSOLE_ENTRY(4, object_set_private_data), CONSOLE_ENTRY(5, object_set_private_data_interface),
    CONSOLE_ENTRY(6, object_set_name),         CONSOLE_ENTRY(7, object_get_device),
};

// --- tables and allocation -----------------------------------------------------------------------

static bool is_common_entry(const char* const* entry_names, u32 index) {
    static const char* const k_names[] = {"QueryInterface", "AddRef",         "Release",
                                          "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface",
                                          "SetName",        "GetDevice"};
    return index < ARRAY_COUNT(k_names) && strcmp(entry_names[index], k_names[index]) == 0;
}

void* const* console_build_table(const char* interface_name, const char* const* entry_names, u32 count,
                                 const ConsoleEntry* impl, u32 impl_count) {
    void** table = (void**)calloc(count, sizeof(void*));
    if (!table)
        FATAL("out of memory building the %s function table", interface_name);
    for (u32 i = 0; i < ARRAY_COUNT(k_common_entries); i++) {
        if (is_common_entry(entry_names, k_common_entries[i].index))
            table[k_common_entries[i].index] = k_common_entries[i].fn;
    }
    for (u32 i = 0; i < impl_count; i++) {
        if (impl[i].index >= count)
            FATAL("%s: entry %u out of range (%u entries)", interface_name, impl[i].index, count);
        table[impl[i].index] = impl[i].fn;
    }
    for (u32 i = 0; i < count; i++) {
        if (table[i])
            continue;
        char name[128];
        snprintf(name, sizeof(name), "%s::%s", interface_name, entry_names[i]);
        table[i] = stub_create_ex("d3d12_xs.dll", name, 0, (u64)(u32)E_NOTIMPL);
    }
    return table;
}

void* console_alloc(usize size, const ConsoleClass* klass, void* const* table, ID3D12Object* desktop) {
    ConsolePrivate* priv = (ConsolePrivate*)calloc(1, sizeof(ConsolePrivate) + size);
    if (!priv)
        FATAL("out of memory allocating a %s", klass->name);
    priv->klass = klass;
    priv->desktop = desktop;
    ConsoleObjectHeader* object = (ConsoleObjectHeader*)(priv + 1);
    object->function_table = table;
    object->ref_count = 1;
    return object;
}

bool console_try_add_ref(void* object) {
    volatile long* refs = (volatile long*)&((ConsoleObjectHeader*)object)->ref_count;
    long seen = *refs;
    while (seen > 0) {
        long previous = InterlockedCompareExchange(refs, seen + 1, seen);
        if (previous == seen)
            return true;
        seen = previous;
    }
    return false;
}

void console_release(void* object) {
    object_release(object);
}

HRESULT console_query(void* object, REFIID iid, void** out) {
    return object_query_interface(object, iid, out);
}

HRESULT console_return(void* object, REFIID iid, void** out) {
    HRESULT hr = out ? object_query_interface(object, iid, out) : S_FALSE;
    object_release(object);
    return hr;
}
