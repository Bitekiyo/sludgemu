// xgameruntime.dll: the Game Runtime. Titles load it dynamically (from their statically linked
// runtime thunks) and reach every runtime API through QueryApiImpl. See runtime_api.h.

#include "hle/xgameruntime/xgameruntime.h"

#include <stdio.h>

#include "common/log.h"
#include "hle/hle.h"
#include "hle/xgameruntime/runtime_api.h"
#include "hle/xgameruntime/task_queue.h"
#include "loader/stubs.h"

#define RUNTIME_APIS(X)                                                                                      \
    X(XTaskQueue)                                                                                            \
    X(XUser)                                                                                                 \
    X(XUserGamertag)                                                                                         \
    X(XUserDevice)                                                                                           \
    X(XGameSave)                                                                                             \
    X(XPackage)                                                                                              \
    X(XStore)                                                                                                \
    X(XSystem)                                                                                               \
    X(XSystemAnalytics)                                                                                      \
    X(XGame)                                                                                                 \
    X(XLaunch)                                                                                               \
    X(XDisplay)                                                                                              \
    X(XGameRuntimeFeature)                                                                                   \
    X(XError)                                                                                                \
    X(XGameEvent)                                                                                            \
    X(XPersistentLocalStorage)                                                                               \
    X(XNetworking)                                                                                           \
    X(XGameUi)                                                                                               \
    X(XAppCapture)                                                                                           \
    X(XAppBroadcast)                                                                                         \
    X(XSpeech)                                                                                               \
    X(XGameInvite)                                                                                           \
    X(XGameActivation)                                                                                       \
    X(XGameProtocol)                                                                                         \
    X(XGameStreaming)                                                                                        \
    X(XGameStreamingExperimental)

#define DECLARE_API(name) extern const RuntimeApi g_runtime_api_##name;
RUNTIME_APIS(DECLARE_API)
#undef DECLARE_API

#define LIST_API(name) &g_runtime_api_##name,
static const RuntimeApi* const k_apis[] = {RUNTIME_APIS(LIST_API)};
#undef LIST_API

#define API_COUNT ARRAY_COUNT(k_apis)
#define MAX_SLOTS 96

// One stateless object per vtable; reference counting is a no-op.
struct RuntimeObject {
    void* const* vtable;
};

static void* g_vtables[API_COUNT][MAX_SLOTS];
static RuntimeObject g_objects[API_COUNT];

static bool guid_eq(const GUID& a, const GUID& b) {
    return memcmp(&a, &b, sizeof(GUID)) == 0;
}

static RuntimeObject* find_object(const GUID& provider, const GUID& iid) {
    for (u32 i = 0; i < API_COUNT; i++) {
        const RuntimeApi* api = k_apis[i];
        if (!guid_eq(*api->provider, provider))
            continue;
        for (u32 k = 0; k < api->iid_count; k++) {
            if (guid_eq(api->iids[k], iid))
                return &g_objects[i];
        }
    }
    return nullptr;
}

static u32 object_index(const RuntimeObject* obj) {
    return (u32)(obj - g_objects);
}

static HRESULT WINAPI object_query_interface(RuntimeObject* self, const GUID* iid, void** out) {
    RuntimeObject* obj = find_object(*k_apis[object_index(self)]->provider, *iid);
    *out = obj;
    return obj ? S_OK : E_NOINTERFACE;
}

static ULONG WINAPI object_add_ref(RuntimeObject*) {
    return 1;
}
static ULONG WINAPI object_release(RuntimeObject*) {
    return 1;
}

static void build_vtable(u32 index) {
    const RuntimeApi* api = k_apis[index];
    void** vtable = g_vtables[index];

    u32 slot_count = 3;
    for (u32 i = 0; i < api->method_count; i++) {
        u32 slot = api->methods[i].slot;
        if (slot >= MAX_SLOTS)
            FATAL("runtime api %s: slot %u exceeds MAX_SLOTS", api->name, slot);
        if (slot + 1 > slot_count)
            slot_count = slot + 1;
        vtable[slot] = api->methods[i].fn
                           ? api->methods[i].fn
                           : stub_create_ex("xgameruntime.dll", api->methods[i].name, 0, (u64)(u32)E_NOTIMPL);
    }
    vtable[0] = (void*)&object_query_interface;
    vtable[1] = (void*)&object_add_ref;
    vtable[2] = (void*)&object_release;

    // Slots we have no name for (methods the title's thunks never call).
    for (u32 slot = 3; slot < slot_count; slot++) {
        if (!vtable[slot]) {
            char name[64];
            snprintf(name, sizeof(name), "%s#%u", api->name, slot);
            vtable[slot] = stub_create_ex("xgameruntime.dll", name, 0, (u64)(u32)E_NOTIMPL);
        }
    }
    g_objects[index].vtable = vtable;
}

void xgameruntime_init() {
    task_queue_init();
    for (u32 i = 0; i < API_COUNT; i++)
        build_vtable(i);
}

// --- exports -------------------------------------------------------------------------------------

// The thunk library passes its runtime version so a newer library can detect an older runtime.
// We implement the interfaces the thunks ask for, so any version is accepted.
static HRESULT WINAPI InitializeApiImpl(u64 version, u64 build) {
    LOG_I("xgr", "InitializeApiImpl(%llx, %llx)", version, build);
    return S_OK;
}

static HRESULT WINAPI InitializeApiImplEx(u64 version, u64 build, u64 flags) {
    LOG_I("xgr", "InitializeApiImplEx(%llx, %llx, flags=%llx)", version, build, flags);
    return S_OK;
}

static HRESULT WINAPI InitializeApiImplEx2(u64 version, u64 build, u64 flags, void* options) {
    LOG_I("xgr", "InitializeApiImplEx2(%llx, %llx, flags=%llx, options=%p)", version, build, flags, options);
    return S_OK;
}

static void WINAPI UninitializeApiImpl() {
    LOG_I("xgr", "UninitializeApiImpl");
}

static HRESULT WINAPI QueryApiImpl(const GUID* provider, const GUID* iid, void** out) {
    RuntimeObject* obj = find_object(*provider, *iid);
    *out = obj;
    if (obj)
        return S_OK;
    LOG_W("xgr", "QueryApiImpl: unknown interface {%08lx-...} of provider {%08lx-...}", iid->Data1,
          provider->Data1);
    return REGDB_E_CLASSNOTREG;
}

static void WINAPI XErrorReport(HRESULT hr, const char* message) {
    LOG_E("xgr", "runtime error 0x%08lx: %s", (unsigned long)hr, message ? message : "");
}

static HRESULT WINAPI DllCanUnloadNow() {
    return S_FALSE;
}

static const HleExport k_exports[] = {
    HLE_EXPORT(DllCanUnloadNow),      HLE_EXPORT(InitializeApiImpl), HLE_EXPORT(InitializeApiImplEx),
    HLE_EXPORT(InitializeApiImplEx2), HLE_EXPORT(QueryApiImpl),      HLE_EXPORT(UninitializeApiImpl),
    HLE_EXPORT(XErrorReport),
};

extern const HleModule g_hle_module_xgameruntime = {"xgameruntime.dll", k_exports, ARRAY_COUNT(k_exports)};
