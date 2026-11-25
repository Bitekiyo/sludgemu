#include "gpu/gpu.h"

#include "common/log.h"

static GpuOptions g_options;
static ID3D12Device13* g_device = nullptr;
static IDXGIFactory6* g_factory = nullptr;
static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;

void gpu_set_options(const GpuOptions& options) {
    g_options = options;
}

// What the translation relies on. Console titles are written against RDNA2-class features:
// shader model 6.6+ (wave intrinsics, 64-bit atomics, dynamic resources), enhanced barriers and
// mesh shaders.
static bool check_features(ID3D12Device* device) {
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_6};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
        LOG_E("gpu", "adapter lacks shader model 6.6");
        return false;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 = {};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12))) ||
        !o12.EnhancedBarriersSupported) {
        LOG_E("gpu", "adapter lacks enhanced barriers");
        return false;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))) ||
        o7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
        LOG_E("gpu", "adapter lacks mesh shaders");
        return false;
    }
    return true;
}

static void CALLBACK on_debug_message(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                                      D3D12_MESSAGE_ID id, LPCSTR description, void*) {
    if (severity <= D3D12_MESSAGE_SEVERITY_ERROR)
        LOG_E("d3d12dbg", "[%d] %s", id, description);
    else if (severity == D3D12_MESSAGE_SEVERITY_WARNING)
        LOG_W("d3d12dbg", "[%d] %s", id, description);
    else
        LOG_D("d3d12dbg", "[%d] %s", id, description);
}

// Debug layer messages go to our log, next to what the title was doing.
static void route_debug_messages(ID3D12Device* device) {
    ID3D12InfoQueue1* queue = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
        LOG_W("gpu", "debug layer messages unavailable (no ID3D12InfoQueue1)");
        return;
    }
    DWORD cookie = 0;
    if (FAILED(queue->RegisterMessageCallback(on_debug_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr,
                                              &cookie)))
        LOG_W("gpu", "cannot register for debug layer messages");
    queue->Release();
}

static BOOL CALLBACK create_device(PINIT_ONCE, void*, void**) {
    if (g_options.debug_layer) {
        ID3D12Debug* debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug->Release();
            LOG_I("gpu", "D3D12 debug layer enabled");
        } else {
            LOG_W("gpu", "D3D12 debug layer requested but unavailable");
        }
    }

    UINT factory_flags = g_options.debug_layer ? DXGI_CREATE_FACTORY_DEBUG : 0;
    if (FAILED(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&g_factory)))) {
        LOG_E("gpu", "CreateDXGIFactory2 failed");
        return TRUE;
    }

    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; g_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                           IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        ID3D12Device13* device = nullptr;
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)))) {
            if (check_features(device)) {
                LOG_I("gpu", "adapter: %ls (%llu MiB VRAM)", desc.Description,
                      (u64)desc.DedicatedVideoMemory >> 20);
                g_device = device;
                adapter->Release();
                break;
            }
            LOG_W("gpu", "skipping adapter %ls: missing required features", desc.Description);
            device->Release();
        }
        adapter->Release();
    }
    if (!g_device)
        LOG_E("gpu", "no suitable D3D12 adapter found");
    else if (g_options.debug_layer)
        route_debug_messages(g_device);
    return TRUE;
}

ID3D12Device13* gpu_device() {
    InitOnceExecuteOnce(&g_init_once, create_device, nullptr, nullptr);
    return g_device;
}

IDXGIFactory6* gpu_factory() {
    InitOnceExecuteOnce(&g_init_once, create_device, nullptr, nullptr);
    return g_factory;
}

// Nothing submits GPU work yet, so nothing can still be in use. Command submission replaces this
// with fence-tracked deferred release.
void gpu_retire(IUnknown* object) {
    if (object)
        object->Release();
}
