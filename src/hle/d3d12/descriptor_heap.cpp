// Console ID3D12DescriptorHeap.

#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "core/memory.h"
#include "gpu/gpu.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"
#include "hle/xmem/xmem.h"

static const GUID* const k_descriptor_heap_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                                     &__uuidof(ID3D12Pageable),
                                                     &__uuidof(ID3D12DescriptorHeap)};

static void descriptor_heap_destroy(void* object);

static const ConsoleClass k_descriptor_heap_class = {"ID3D12DescriptorHeap", k_descriptor_heap_iids,
                                                     ARRAY_COUNT(k_descriptor_heap_iids),
                                                     descriptor_heap_destroy};

static void* const* g_descriptor_heap_table = nullptr;

// Desktop limits for shader-visible heaps: what the device reports, at least the tier 3 minimum.
static u32 g_max_visible_resources = 1000000;
static u32 g_max_visible_samplers = 2048;

static u32 g_desktop_stride[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES];

static u64 descriptor_memory_attributes(u64 size) {
    return xmem_d3d_attributes(XMEM_D3D_OBJECT_RESOURCE, XMEM_TYPE_GRAPHICS_CACHEABLE, size, MEM_PAGE_SIZE,
                               true);
}

// --- heap registry: console handle -> heap -----------------------------------------------------------

struct HeapEntry {
    u64 base;
    u64 end;
    ConsoleDescriptorHeap* heap;
};

static struct {
    HeapEntry* entries; // sorted by base, non-overlapping
    u32 count;
    u32 capacity;
    SRWLOCK lock;
} g_heaps = {nullptr, 0, 0, SRWLOCK_INIT};

static u32 heaps_at_or_below(u64 address) {
    u32 lo = 0, hi = g_heaps.count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_heaps.entries[mid].base <= address)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void register_heap(ConsoleDescriptorHeap* heap) {
    AcquireSRWLockExclusive(&g_heaps.lock);
    if (g_heaps.count == g_heaps.capacity) {
        u32 capacity = g_heaps.capacity ? g_heaps.capacity * 2 : 64;
        HeapEntry* grown = (HeapEntry*)realloc(g_heaps.entries, capacity * sizeof(HeapEntry));
        if (!grown)
            FATAL("out of memory tracking descriptor heaps");
        g_heaps.entries = grown;
        g_heaps.capacity = capacity;
    }
    u64 base = (u64)heap->memory;
    u32 at = heaps_at_or_below(base);
    memmove(&g_heaps.entries[at + 1], &g_heaps.entries[at], (g_heaps.count - at) * sizeof(HeapEntry));
    g_heaps.entries[at] = {base, base + (u64)heap->desc.NumDescriptors * heap->stride, heap};
    g_heaps.count++;
    ReleaseSRWLockExclusive(&g_heaps.lock);
}

static void unregister_heap(ConsoleDescriptorHeap* heap) {
    AcquireSRWLockExclusive(&g_heaps.lock);
    u32 at = heaps_at_or_below((u64)heap->memory);
    if (at > 0 && g_heaps.entries[at - 1].heap == heap) {
        memmove(&g_heaps.entries[at - 1], &g_heaps.entries[at], (g_heaps.count - at) * sizeof(HeapEntry));
        g_heaps.count--;
    }
    ReleaseSRWLockExclusive(&g_heaps.lock);
}

bool console_descriptor_slots(u64 handle, u32 count, DescriptorSlot* out) {
    bool ok = false;
    AcquireSRWLockShared(&g_heaps.lock);
    u32 at = heaps_at_or_below(handle);
    if (at > 0 && handle < g_heaps.entries[at - 1].end) {
        const ConsoleDescriptorHeap* heap = g_heaps.entries[at - 1].heap;
        u64 offset = handle - (u64)heap->memory;
        u64 index = offset / heap->stride;
        if (offset % heap->stride == 0 && index + count <= heap->desktop_count) {
            u32 stride = g_desktop_stride[heap->desktop_type];
            u64 desktop_offset = index * stride;
            out->type = heap->desktop_type;
            out->desktop_stride = stride;
            out->cpu.ptr = heap->cpu_start + desktop_offset;
            out->visible_cpu.ptr = heap->visible ? heap->visible_cpu_start + desktop_offset : 0;
            out->visible_gpu.ptr = heap->visible ? heap->visible_gpu_start + desktop_offset : 0;
            out->console = heap->memory + offset;
            ok = true;
        }
    }
    ReleaseSRWLockShared(&g_heaps.lock);
    if (!ok)
        LOG_E("d3d12", "descriptor handle 0x%llx (+%u) is not inside a live descriptor heap", handle, count);
    return ok;
}

void console_descriptor_publish(const DescriptorSlot& slot, u32 count) {
    if (slot.visible_cpu.ptr)
        gpu_device()->CopyDescriptorsSimple(count, slot.visible_cpu, slot.cpu, slot.type);
}

// --- object ------------------------------------------------------------------------------------------

static void descriptor_heap_destroy(void* object) {
    ConsoleDescriptorHeap* heap = (ConsoleDescriptorHeap*)object;
    unregister_heap(heap);
    gpu_retire(heap->cpu);
    gpu_retire(heap->visible);
    if (heap->memory)
        xmem_free(heap->memory, descriptor_memory_attributes((u64)heap->desc.NumDescriptors * heap->stride));
}

static void WINAPI descriptor_heap_get_desc(ConsoleDescriptorHeap* self, D3D12_DESCRIPTOR_HEAP_DESC* out) {
    *out = self->desc;
}

static D3D12_CPU_DESCRIPTOR_HANDLE WINAPI descriptor_heap_get_cpu_start(ConsoleDescriptorHeap* self) {
    return {(SIZE_T)self->memory};
}

static D3D12_GPU_DESCRIPTOR_HANDLE WINAPI descriptor_heap_get_gpu_start(ConsoleDescriptorHeap* self) {
    return {(u64)self->memory};
}

static const ConsoleEntry k_descriptor_heap_impl[] = {
    CONSOLE_ENTRY(8, descriptor_heap_get_desc),
    CONSOLE_ENTRY(9, descriptor_heap_get_cpu_start),
    CONSOLE_ENTRY(10, descriptor_heap_get_gpu_start),
};

void console_descriptor_heap_init() {
    g_descriptor_heap_table = console_build_table(
        "ID3D12DescriptorHeap", k_descriptor_heap_entries, ARRAY_COUNT(k_descriptor_heap_entries),
        k_descriptor_heap_impl, ARRAY_COUNT(k_descriptor_heap_impl));
    ID3D12Device13* device = gpu_device();
    for (u32 t = 0; t < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; t++)
        g_desktop_stride[t] = device->GetDescriptorHandleIncrementSize((D3D12_DESCRIPTOR_HEAP_TYPE)t);
    D3D12_FEATURE_DATA_D3D12_OPTIONS19 o19 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS19, &o19, sizeof(o19)))) {
        if (o19.MaxViewDescriptorHeapSize > g_max_visible_resources)
            g_max_visible_resources = o19.MaxViewDescriptorHeapSize;
        if (o19.MaxSamplerDescriptorHeapSize > g_max_visible_samplers)
            g_max_visible_samplers = o19.MaxSamplerDescriptorHeapSize;
    }
    LOG_I("d3d12", "shader-visible descriptor heaps: up to %u views, %u samplers", g_max_visible_resources,
          g_max_visible_samplers);
}

u32 console_descriptor_size(u32 type) {
    switch (type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
        return CONSOLE_DESCRIPTOR_SIZE_RESOURCE;
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
    case CONSOLE_DESCRIPTOR_HEAP_TYPE_SAMPLER_UNBOUNDED:
        return CONSOLE_DESCRIPTOR_SIZE_SAMPLER;
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
        return CONSOLE_DESCRIPTOR_SIZE_RTV;
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
        return CONSOLE_DESCRIPTOR_SIZE_DSV;
    default:
        return 0;
    }
}

HRESULT console_create_descriptor_heap(ConsoleDevice* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                       REFIID iid, void** out) {
    u32 stride = desc ? console_descriptor_size(desc->Type) : 0;
    if (!stride || desc->NumDescriptors == 0)
        return E_INVALIDARG;

    D3D12_DESCRIPTOR_HEAP_DESC d = *desc;
    if (d.Type == CONSOLE_DESCRIPTOR_HEAP_TYPE_SAMPLER_UNBOUNDED)
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    if (d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
        d.Flags &= ~D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    d.NodeMask = 0;
    if (d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
        u32 limit =
            d.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? g_max_visible_samplers : g_max_visible_resources;
        if (d.NumDescriptors > limit) {
            LOG_W("d3d12",
                  "descriptor heap of %u (type %d) exceeds the desktop limit of %u; slots past it are "
                  "unusable",
                  desc->NumDescriptors, desc->Type, limit);
            d.NumDescriptors = limit;
        }
    }
    ID3D12DescriptorHeap* visible = nullptr;
    ID3D12DescriptorHeap* cpu = nullptr;
    HRESULT hr = S_OK;
    if (d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
        hr = device->desktop->CreateDescriptorHeap(&d, IID_PPV_ARGS(&visible));
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC cpu_desc = d;
        cpu_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = device->desktop->CreateDescriptorHeap(&cpu_desc, IID_PPV_ARGS(&cpu));
    }
    if (FAILED(hr)) {
        LOG_E("d3d12", "CreateDescriptorHeap(type %d, %u, flags 0x%x) failed (0x%08lx)", d.Type,
              d.NumDescriptors, d.Flags, (unsigned long)hr);
        if (visible)
            visible->Release();
        return hr;
    }

    // Like the console runtime, take the memory from XMemAlloc, where the title's allocation hooks
    // may supply memory set aside for this heap. Handles must fit in 32 bits: console hardware
    // passes descriptor table addresses that way.
    u64 bytes = (u64)desc->NumDescriptors * stride;
    u8* memory = (u8*)xmem_alloc(bytes, descriptor_memory_attributes(bytes));
    if (!memory || (u64)memory + bytes > GiB(4)) {
        LOG_E("d3d12", "no memory below 4 GiB for a %llu KiB descriptor heap (got %p)", bytes >> 10, memory);
        if (memory)
            xmem_free(memory, descriptor_memory_attributes(bytes));
        cpu->Release();
        if (visible)
            visible->Release();
        return E_OUTOFMEMORY;
    }

    ConsoleDescriptorHeap* heap = (ConsoleDescriptorHeap*)console_alloc(
        sizeof(ConsoleDescriptorHeap), &k_descriptor_heap_class, g_descriptor_heap_table, nullptr);
    heap->desc = *desc;
    heap->memory = memory;
    heap->stride = stride;
    heap->desktop_count = d.NumDescriptors;
    heap->desktop_type = d.Type;
    heap->cpu = cpu;
    heap->visible = visible;
    heap->cpu_start = cpu->GetCPUDescriptorHandleForHeapStart().ptr;
    if (visible) {
        heap->visible_cpu_start = visible->GetCPUDescriptorHandleForHeapStart().ptr;
        heap->visible_gpu_start = visible->GetGPUDescriptorHandleForHeapStart().ptr;
    }
    register_heap(heap);
    LOG_D("d3d12", "descriptor heap type %d, %u descriptors, flags 0x%x at %p", desc->Type,
          desc->NumDescriptors, desc->Flags, memory);
    return console_return(heap, iid, out);
}
