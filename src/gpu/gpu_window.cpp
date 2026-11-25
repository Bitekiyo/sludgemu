#include "gpu/gpu_window.h"

#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "core/memory.h"

// Open windows, sorted by title address and never overlapping (blocks don't).
struct Window {
    u64 base;
    u64 end;
    u64 desktop_address;
    ID3D12Resource* buffer;
};

static struct {
    Window* entries;
    u32 count;
    u32 capacity;
    SRWLOCK lock;
} g_windows = {nullptr, 0, 0, SRWLOCK_INIT};

// Number of windows with base <= address. Lock held.
static u32 windows_at_or_below(u64 address) {
    u32 lo = 0, hi = g_windows.count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_windows.entries[mid].base <= address)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static volatile long g_generation = 1;

u32 gpu_window_generation() {
    return (u32)g_generation;
}

// The window containing `address`, or nullptr. Lock held.
static const Window* find_window(u64 address) {
    u32 at = windows_at_or_below(address);
    if (at > 0 && address < g_windows.entries[at - 1].end)
        return &g_windows.entries[at - 1];
    return nullptr;
}

// Closes the windows overlapping [start, end). Lock held exclusively.
static void remove_windows_locked(u64 start, u64 end) {
    u32 last = windows_at_or_below(end - 1);
    u32 first = last;
    while (first > 0 && g_windows.entries[first - 1].end > start)
        first--;
    for (u32 i = first; i < last; i++)
        gpu_retire(g_windows.entries[i].buffer);
    if (last > first && InterlockedIncrement(&g_generation) == 0)
        InterlockedIncrement(&g_generation);
    memmove(&g_windows.entries[first], &g_windows.entries[last], (g_windows.count - last) * sizeof(Window));
    g_windows.count -= last - first;
}

static bool fill(const Window* w, u64 address, u64 size, GpuAddress* out) {
    if (size > w->end - address) {
        LOG_E("gpu", "GPU access 0x%llx+0x%llx crosses the end of its memory block at 0x%llx", address, size,
              w->end);
        return false;
    }
    out->buffer = w->buffer;
    out->offset = address - w->base;
    out->desktop_address = w->desktop_address + out->offset;
    out->available = w->end - address;
    return true;
}

// Windows over large blocks (a title's multi-gigabyte pools) cover only the granules in use, so only
// memory the GPU touches gets pinned. Windows already open in the way are merged in, which keeps
// windows from overlapping; the new one replaces them.
#define WINDOW_GRANULE MiB(64)

static MemBlock window_extent(const MemBlock& block, u64 address, u64 size) {
    u64 block_end = (u64)block.base + block.size;
    u64 lo = ALIGN_DOWN(address, WINDOW_GRANULE);
    u64 hi = ALIGN_UP(address + size, WINDOW_GRANULE);
    lo = lo > (u64)block.base ? lo : (u64)block.base;
    hi = hi < block_end ? hi : block_end;
    AcquireSRWLockShared(&g_windows.lock);
    for (u32 i = windows_at_or_below(hi - 1); i-- > 0 && g_windows.entries[i].end > lo;) {
        if (g_windows.entries[i].base < lo)
            lo = g_windows.entries[i].base;
        if (g_windows.entries[i].end > hi)
            hi = g_windows.entries[i].end;
    }
    ReleaseSRWLockShared(&g_windows.lock);
    lo = lo > (u64)block.base ? lo : (u64)block.base; // a window can't reach past its block
    hi = hi < block_end ? hi : block_end;
    return {(u8*)lo, hi - lo, block.host + (lo - (u64)block.base)};
}

// Physical memory chunks are sections, which the GPU sees through heaps opened over their views in
// the linear view (mapped for the life of the process). Not over the section handles: heaps opened
// from file mappings keep the section's commit charged after the heap, the section and even the
// process are gone, so every run would leak all the physical memory it touched. Each chunk's heap is
// opened once and kept: chunks live as long as the process.
static ID3D12Heap** g_chunk_heaps = nullptr;
static SRWLOCK g_chunk_lock = SRWLOCK_INIT;

static ID3D12Heap* chunk_heap(u32 chunk, void* chunk_view) {
    AcquireSRWLockExclusive(&g_chunk_lock);
    if (!g_chunk_heaps)
        g_chunk_heaps = (ID3D12Heap**)calloc(mem_physical_chunk_count(), sizeof(ID3D12Heap*));
    ID3D12Heap* heap = g_chunk_heaps ? g_chunk_heaps[chunk] : nullptr;
    if (g_chunk_heaps && !heap) {
        HRESULT hr =
            gpu_device()->OpenExistingHeapFromAddress1(chunk_view, MEM_CHUNK_SIZE, IID_PPV_ARGS(&heap));
        if (FAILED(hr)) {
            gpu_device_lost(hr);
            LOG_E("gpu", "cannot open a GPU heap over physical memory chunk %u (0x%08lx)", chunk,
                  (unsigned long)hr);
            heap = nullptr;
        }
        g_chunk_heaps[chunk] = heap;
    }
    ReleaseSRWLockExclusive(&g_chunk_lock);
    return heap;
}

// The heap behind a block and the block's offset in it. The caller owns a reference.
static ID3D12Heap* block_heap(const MemBlock& block, u64* offset) {
    u32 chunk;
    if (mem_linear_location(block.host, &chunk, offset)) {
        ID3D12Heap* heap = chunk_heap(chunk, block.host - *offset);
        if (heap)
            heap->AddRef();
        return heap;
    }
    *offset = 0;
    ID3D12Heap* heap = nullptr;
    HRESULT hr = gpu_device()->OpenExistingHeapFromAddress1(block.host, block.size, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) {
        gpu_device_lost(hr);
        LOG_E("gpu", "cannot open a GPU heap over title memory %p+0x%llx (0x%08lx)", block.base, block.size,
              (unsigned long)hr);
        return nullptr;
    }
    return heap;
}

// A capture tool (RenderDoc) is loaded: windows are mapped for it (see open_window).
static bool g_capture_tool;

static ID3D12Resource* open_window(const MemBlock& block) {
    ID3D12Device13* device = gpu_device();
    u64 heap_offset;
    ID3D12Heap* heap = block_heap(block, &heap_offset);
    if (!heap)
        return nullptr;
    D3D12_RESOURCE_DESC1 desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = block.size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* buffer = nullptr;
    HRESULT hr = device->CreatePlacedResource2(heap, heap_offset, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED,
                                               nullptr, 0, nullptr, IID_PPV_ARGS(&buffer));
    heap->Release(); // the buffer keeps it alive
    if (FAILED(hr)) {
        LOG_E("gpu", "cannot place a window buffer over title memory %p+0x%llx (0x%08lx)", block.base,
              block.size, (unsigned long)hr);
        return nullptr;
    }
    // RenderDoc sees CPU writes to buffer memory only through mappings it knows of; mapping windows
    // keeps captures faithful to what the title wrote during the frame (constant buffers above all).
    if (g_capture_tool) {
        void* mapped = nullptr;
        D3D12_RANGE none = {0, 0};
        if (FAILED(buffer->Map(0, &none, &mapped)))
            LOG_W_ONCE("gpu", "cannot map window buffers for the capture tool; captures miss CPU writes");
    }
    LOG_D("gpu", "window over title memory %p+0x%llx", block.base, block.size);
    return buffer;
}

bool gpu_translate(u64 address, u64 size, GpuAddress* out) {
    if (size == 0)
        size = 1;
    AcquireSRWLockShared(&g_windows.lock);
    const Window* w = find_window(address);
    bool ok = w && fill(w, address, size, out);
    ReleaseSRWLockShared(&g_windows.lock);
    if (w)
        return ok;

    MemBlock block;
    mem_prepare((const void*)address, size); // the GPU can't fault pages in
    if (!mem_backing_block((const void*)address, &block)) {
        LOG_E("gpu", "GPU access to unmapped title memory 0x%llx+0x%llx", address, size);
        return false;
    }
    block = window_extent(block, address, size);
    mem_prepare(block.base, block.size); // a heap over title memory needs all of it committed
    ID3D12Resource* buffer = open_window(block);
    if (!buffer)
        return false;

    AcquireSRWLockExclusive(&g_windows.lock);
    if ((w = find_window(address)) != nullptr) {
        buffer->Release(); // another thread opened it first; never used
    } else {
        if (g_windows.count == g_windows.capacity) {
            u32 capacity = g_windows.capacity ? g_windows.capacity * 2 : 64;
            Window* grown = (Window*)realloc(g_windows.entries, capacity * sizeof(Window));
            if (!grown)
                FATAL("out of memory tracking GPU windows");
            g_windows.entries = grown;
            g_windows.capacity = capacity;
        }
        // A block can grow when the title maps frames that continue it; the new window replaces
        // the ones it now covers.
        remove_windows_locked((u64)block.base, (u64)block.base + block.size);
        u32 at = windows_at_or_below((u64)block.base);
        memmove(&g_windows.entries[at + 1], &g_windows.entries[at], (g_windows.count - at) * sizeof(Window));
        g_windows.entries[at] = {(u64)block.base, (u64)block.base + block.size,
                                 buffer->GetGPUVirtualAddress(), buffer};
        g_windows.count++;
        w = &g_windows.entries[at];
    }
    ok = fill(w, address, size, out);
    ReleaseSRWLockExclusive(&g_windows.lock);
    return ok;
}

// Title mappings changed: windows over those pages may show the wrong frames, or memory that is
// gone. Close them; the next access opens windows over the new blocks.
static void on_mapping_changed(const u8* base, u64 size) {
    AcquireSRWLockExclusive(&g_windows.lock);
    remove_windows_locked((u64)base, (u64)base + size);
    ReleaseSRWLockExclusive(&g_windows.lock);
}

void gpu_window_init() {
    g_capture_tool = GetModuleHandleW(L"renderdoc.dll") != nullptr;
    mem_set_mapping_listener(on_mapping_changed);
}
