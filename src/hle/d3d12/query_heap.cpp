// Console ID3D12QueryHeap.

#include "common/log.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"

static const GUID* const k_query_heap_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                                &__uuidof(ID3D12Pageable), &__uuidof(ID3D12QueryHeap)};

static const ConsoleClass k_query_heap_class = {"ID3D12QueryHeap", k_query_heap_iids,
                                                ARRAY_COUNT(k_query_heap_iids), nullptr};

static void* const* g_query_heap_table = nullptr;

// Console-only query heap types: DMA engine timestamps, and queries that skip the console's
// pipeline synchronization. Desktop has the plain kinds.
#define CONSOLE_QUERY_HEAP_DMA_TIMESTAMP         7
#define CONSOLE_QUERY_HEAP_OCCLUSION_NO_SYNC     9
#define CONSOLE_QUERY_HEAP_TIMESTAMP_NO_SYNC     10
#define CONSOLE_QUERY_HEAP_DMA_TIMESTAMP_NO_SYNC 11

static D3D12_QUERY_HEAP_TYPE desktop_query_heap_type(u32 type) {
    switch (type) {
    case CONSOLE_QUERY_HEAP_DMA_TIMESTAMP:
    case CONSOLE_QUERY_HEAP_DMA_TIMESTAMP_NO_SYNC:
        return D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP;
    case CONSOLE_QUERY_HEAP_OCCLUSION_NO_SYNC:
        return D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    case CONSOLE_QUERY_HEAP_TIMESTAMP_NO_SYNC:
        return D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    default:
        return (D3D12_QUERY_HEAP_TYPE)type;
    }
}

void console_query_heap_init() {
    g_query_heap_table = console_build_table("ID3D12QueryHeap", k_query_heap_entries,
                                             ARRAY_COUNT(k_query_heap_entries), nullptr, 0);
}

HRESULT console_create_query_heap(ConsoleDevice* device, const D3D12_QUERY_HEAP_DESC* desc, REFIID iid,
                                  void** out) {
    if (!desc)
        return E_INVALIDARG;
    D3D12_QUERY_HEAP_DESC d = *desc;
    d.Type = desktop_query_heap_type(desc->Type);
    d.NodeMask = 0;
    ID3D12QueryHeap* desktop = nullptr;
    HRESULT hr = device->desktop->CreateQueryHeap(&d, IID_PPV_ARGS(&desktop));
    if (FAILED(hr) && d.Type == D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP) {
        // Copy queue timestamps are optional on desktop; such a heap then only works on the
        // graphics and compute queues.
        LOG_W("d3d12", "copy queue timestamps unsupported; using a plain timestamp heap");
        d.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        hr = device->desktop->CreateQueryHeap(&d, IID_PPV_ARGS(&desktop));
    }
    if (FAILED(hr)) {
        LOG_E("d3d12", "CreateQueryHeap(type %d, %u) failed (0x%08lx)", desc->Type, desc->Count,
              (unsigned long)hr);
        return hr;
    }
    LOG_D("d3d12", "query heap type %d, %u queries", desc->Type, desc->Count);
    ConsoleQueryHeap* heap = (ConsoleQueryHeap*)console_alloc(sizeof(ConsoleQueryHeap), &k_query_heap_class,
                                                              g_query_heap_table, desktop);
    heap->desc = *desc;
    heap->desktop = desktop;
    return console_return(heap, iid, out);
}
