#include "gpu/gpu.h"

#include <stdlib.h>

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

// --- submission and deferred release ----------------------------------------------------------------
//
// Each queue that runs translated work counts its submissions on a fence. A retired object waits
// until every queue has finished what it had submitted when the object was retired.

#define GPU_MAX_QUEUES 16

struct GpuQueueSlot {
    ID3D12CommandQueue* queue; // nullptr: free slot
    ID3D12Fence* fence;
    u64 submitted;
};

struct RetiredObject {
    IUnknown* object;
    u64 after[GPU_MAX_QUEUES]; // per queue: the submission that must complete first
};

static SRWLOCK g_submit_lock = SRWLOCK_INIT;
static GpuQueueSlot g_queues[GPU_MAX_QUEUES];
static RetiredObject* g_retired = nullptr;
static u32 g_retired_count = 0;
static u32 g_retired_capacity = 0;

// Releases what no queue can still be using. Caller holds g_submit_lock.
static void collect_locked() {
    if (!g_retired_count)
        return;
    u64 completed[GPU_MAX_QUEUES];
    for (u32 q = 0; q < GPU_MAX_QUEUES; q++)
        completed[q] = g_queues[q].queue ? g_queues[q].fence->GetCompletedValue() : 0;
    u32 kept = 0;
    for (u32 i = 0; i < g_retired_count; i++) {
        RetiredObject& r = g_retired[i];
        bool done = true;
        for (u32 q = 0; q < GPU_MAX_QUEUES && done; q++)
            done = r.after[q] <= completed[q];
        if (done)
            r.object->Release();
        else
            g_retired[kept++] = r;
    }
    g_retired_count = kept;
}

u32 gpu_queue_register(ID3D12CommandQueue* queue) {
    ID3D12Fence* fence = nullptr;
    if (FAILED(gpu_device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        LOG_E("gpu", "cannot create a submission fence");
        return GPU_NO_QUEUE;
    }
    AcquireSRWLockExclusive(&g_submit_lock);
    u32 slot = GPU_NO_QUEUE;
    for (u32 q = 0; q < GPU_MAX_QUEUES; q++) {
        if (!g_queues[q].queue) {
            g_queues[q] = {queue, fence, 0};
            slot = q;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_submit_lock);
    if (slot == GPU_NO_QUEUE) {
        LOG_E("gpu", "more than %u command queues", GPU_MAX_QUEUES);
        fence->Release();
    }
    return slot;
}

void gpu_queue_unregister(u32 slot) {
    if (slot >= GPU_MAX_QUEUES)
        return;
    AcquireSRWLockExclusive(&g_submit_lock);
    GpuQueueSlot q = g_queues[slot];
    g_queues[slot] = {};
    // Whatever waited on this queue now only waits for it to drain, which happens below.
    for (u32 i = 0; i < g_retired_count; i++)
        g_retired[i].after[slot] = 0;
    ReleaseSRWLockExclusive(&g_submit_lock);
    if (q.fence->GetCompletedValue() < q.submitted)
        q.fence->SetEventOnCompletion(q.submitted, nullptr); // blocks until the queue is idle
    q.fence->Release();
}

void gpu_queue_submitted(u32 slot) {
    if (slot >= GPU_MAX_QUEUES)
        return;
    AcquireSRWLockExclusive(&g_submit_lock);
    GpuQueueSlot& q = g_queues[slot];
    q.queue->Signal(q.fence, ++q.submitted);
    collect_locked();
    ReleaseSRWLockExclusive(&g_submit_lock);
}

void gpu_retire(IUnknown* object) {
    if (!object)
        return;
    AcquireSRWLockExclusive(&g_submit_lock);
    if (g_retired_count == g_retired_capacity) {
        u32 capacity = g_retired_capacity ? g_retired_capacity * 2 : 256;
        RetiredObject* grown = (RetiredObject*)realloc(g_retired, capacity * sizeof(RetiredObject));
        if (!grown) {
            // Out of memory: wait for the GPU rather than release something it may be using.
            ReleaseSRWLockExclusive(&g_submit_lock);
            LOG_E("gpu", "retire list full; waiting for the GPU to go idle");
            gpu_wait_idle();
            object->Release();
            return;
        }
        g_retired = grown;
        g_retired_capacity = capacity;
    }
    RetiredObject& r = g_retired[g_retired_count++];
    r.object = object;
    for (u32 q = 0; q < GPU_MAX_QUEUES; q++)
        r.after[q] = g_queues[q].queue ? g_queues[q].submitted : 0;
    collect_locked();
    ReleaseSRWLockExclusive(&g_submit_lock);
}

void gpu_wait_idle() {
    AcquireSRWLockExclusive(&g_submit_lock);
    for (u32 q = 0; q < GPU_MAX_QUEUES; q++) {
        GpuQueueSlot& s = g_queues[q];
        if (s.queue && s.fence->GetCompletedValue() < s.submitted)
            s.fence->SetEventOnCompletion(s.submitted, nullptr);
    }
    collect_locked();
    ReleaseSRWLockExclusive(&g_submit_lock);
}
