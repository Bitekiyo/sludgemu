#include "hle/d3d12/texture_sync.h"

#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "core/memory.h"
#include "gpu/console_layout.h"
#include "gpu/formats.h"
#include "gpu/gpu.h"

struct SyncEntry {
    u64 start;
    u64 end;
    ConsoleResource* resource; // registered while alive (texture_sync_remove before destruction)
    bool dirty;
    bool linear; // laid out for CPU writes: uploaded every frame
};

static struct {
    SyncEntry* entries; // sorted by start; textures may overlap (aliasing)
    u32 count;
    u32 capacity;
    SRWLOCK lock;
    volatile long dirty; // entries marked and not uploaded yet
} g_sync = {nullptr, 0, 0, SRWLOCK_INIT, 0};

// Upload volume per submission: the rest waits for the next one.
#define UPLOAD_BUDGET MiB(256)

bool texture_sync_tracks(const D3D12_RESOURCE_DESC1& desc) {
    const D3D12_RESOURCE_FLAGS written = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && !(desc.Flags & written) &&
           desc.SampleDesc.Count <= 1;
}

static u32 lower_bound(u64 start) {
    u32 lo = 0, hi = g_sync.count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_sync.entries[mid].start < start)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

void texture_sync_add(ConsoleResource* r) {
    D3D12_RESOURCE_ALLOCATION_INFO info = console_resource_allocation_info(r->desc);
    if (info.SizeInBytes == UINT64_MAX)
        return;
    AcquireSRWLockExclusive(&g_sync.lock);
    if (g_sync.count == g_sync.capacity) {
        u32 capacity = g_sync.capacity ? g_sync.capacity * 2 : 256;
        SyncEntry* grown = (SyncEntry*)realloc(g_sync.entries, capacity * sizeof(SyncEntry));
        if (!grown) {
            ReleaseSRWLockExclusive(&g_sync.lock);
            LOG_E("d3d12", "out of memory tracking textures; texture at 0x%llx won't get its contents",
                  r->gpu_address);
            return;
        }
        g_sync.entries = grown;
        g_sync.capacity = capacity;
    }
    u32 at = lower_bound(r->gpu_address);
    memmove(&g_sync.entries[at + 1], &g_sync.entries[at], (g_sync.count - at) * sizeof(SyncEntry));
    // Console layouts store a swizzle mode; mode 0 is linear.
    bool linear =
        r->desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR || r->desc.Layout == CONSOLE_LAYOUT_SWIZZLE;
    // Its memory may hold the contents already; if it isn't mapped yet, the upload is skipped and the
    // mapping marks it again.
    g_sync.entries[at] = {r->gpu_address, r->gpu_address + info.SizeInBytes, r, true, linear};
    g_sync.count++;
    InterlockedIncrement(&g_sync.dirty);
    ReleaseSRWLockExclusive(&g_sync.lock);
}

void texture_sync_remove(ConsoleResource* r) {
    AcquireSRWLockExclusive(&g_sync.lock);
    for (u32 i = lower_bound(r->gpu_address); i < g_sync.count && g_sync.entries[i].start == r->gpu_address;
         i++) {
        if (g_sync.entries[i].resource != r)
            continue;
        if (g_sync.entries[i].dirty)
            InterlockedDecrement(&g_sync.dirty);
        memmove(&g_sync.entries[i], &g_sync.entries[i + 1], (g_sync.count - i - 1) * sizeof(SyncEntry));
        g_sync.count--;
        break;
    }
    ReleaseSRWLockExclusive(&g_sync.lock);
}

void texture_sync_mark(u64 address, u64 size) {
    u64 end = address + size;
    AcquireSRWLockExclusive(&g_sync.lock);
    // Entries starting before `end` may overlap; textures are at most a few hundred MiB, so look back
    // from the first entry starting at or after `end`.
    for (u32 i = lower_bound(end); i > 0; i--) {
        SyncEntry& e = g_sync.entries[i - 1];
        if (e.end <= address) {
            if (address - e.start > GiB(4))
                break; // far before: nothing earlier can reach
            continue;
        }
        if (!e.dirty) {
            e.dirty = true;
            InterlockedIncrement(&g_sync.dirty);
        }
    }
    ReleaseSRWLockExclusive(&g_sync.lock);
}

static void on_mapping_changed(const u8* base, u64 size) {
    texture_sync_mark((u64)base, size);
}

void texture_sync_init() {
    mem_add_mapping_listener(on_mapping_changed);
}

void texture_sync_new_frame() {
    AcquireSRWLockExclusive(&g_sync.lock);
    for (u32 i = 0; i < g_sync.count; i++) {
        SyncEntry& e = g_sync.entries[i];
        if (e.linear && !e.dirty) {
            e.dirty = true;
            InterlockedIncrement(&g_sync.dirty);
        }
    }
    ReleaseSRWLockExclusive(&g_sync.lock);
}

bool texture_sync_pending() {
    return g_sync.dirty != 0;
}

// --- uploads -----------------------------------------------------------------------------------------

struct SubresourceCopy {
    u32 mip;
    u32 slice;
    u32 width, height, depth; // in texels, block aligned for the footprint
    u64 row_pitch;
    u64 slice_pitch;
    u64 offset; // in the upload buffer
};

// One texture's subresources and where they go in the upload buffer, from `offset` on. False for
// textures whose desktop format stores texels differently (stand-ins need conversion).
static bool plan_texture(const ConsoleResource* r, SubresourceCopy* copies, u32 capacity, u32* count,
                         u64* offset) {
    const D3D12_RESOURCE_DESC1& d = r->desc;
    FormatInfo console = format_info(d.Format);
    FormatInfo desktop = format_info(format_desktop_resource(d.Format));
    if (!console.bytes_per_element || console.bytes_per_element != desktop.bytes_per_element ||
        console.block_width != desktop.block_width) {
        LOG_W_ONCE("d3d12",
                   "textures of format %d are stored differently on desktop; their contents aren't "
                   "uploaded yet",
                   d.Format);
        return false;
    }
    u32 mips = d.MipLevels ? d.MipLevels : 1;
    u32 slices = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : d.DepthOrArraySize;
    *count = 0;
    for (u32 s = 0; s < slices; s++) {
        for (u32 m = 0; m < mips; m++) {
            if (*count == capacity)
                return false;
            SubresourceCopy& c = copies[(*count)++];
            u32 w = (u32)(d.Width >> m), h = d.Height >> m;
            u32 depth = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? d.DepthOrArraySize >> m : 1;
            w = w ? w : 1;
            h = h ? h : 1;
            depth = depth ? depth : 1;
            u32 bw = (w + console.block_width - 1) / console.block_width;
            u32 bh = (h + console.block_height - 1) / console.block_height;
            c.mip = m;
            c.slice = s;
            c.width = bw * console.block_width;
            c.height = bh * console.block_height;
            c.depth = depth;
            c.row_pitch =
                ALIGN_UP((u64)bw * console.bytes_per_element, (u64)D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            c.slice_pitch = c.row_pitch * bh;
            *offset = ALIGN_UP(*offset, (u64)D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            c.offset = *offset;
            *offset += c.slice_pitch * depth;
        }
    }
    return true;
}

static ConsoleTextureDesc layout_desc(const D3D12_RESOURCE_DESC1& d) {
    ConsoleTextureDesc t = {};
    t.dimension = d.Dimension;
    t.width = d.Width;
    t.height = d.Height;
    t.depth_or_array_size = d.DepthOrArraySize;
    t.mip_levels = d.MipLevels;
    t.format = d.Format;
    t.sample_count = d.SampleDesc.Count;
    t.layout = d.Layout;
    t.flags = d.Flags;
    return t;
}

static void texture_barrier(ID3D12GraphicsCommandList8* list, ID3D12Resource* texture, bool before_copy) {
    D3D12_TEXTURE_BARRIER b = {};
    b.SyncBefore = before_copy ? D3D12_BARRIER_SYNC_ALL : D3D12_BARRIER_SYNC_COPY;
    b.SyncAfter = before_copy ? D3D12_BARRIER_SYNC_COPY : D3D12_BARRIER_SYNC_ALL;
    b.AccessBefore = before_copy ? D3D12_BARRIER_ACCESS_COMMON : D3D12_BARRIER_ACCESS_COPY_DEST;
    b.AccessAfter = before_copy ? D3D12_BARRIER_ACCESS_COPY_DEST : D3D12_BARRIER_ACCESS_COMMON;
    b.LayoutBefore = D3D12_BARRIER_LAYOUT_COMMON;
    b.LayoutAfter = D3D12_BARRIER_LAYOUT_COMMON;
    b.pResource = texture;
    b.Subresources.IndexOrFirstMipLevel = 0xFFFFFFFF; // all
    D3D12_BARRIER_GROUP g = {D3D12_BARRIER_TYPE_TEXTURE, 1};
    g.pTextureBarriers = &b;
    list->Barrier(1, &g);
}

// Copies from buffers into textures need 512-byte aligned offsets and 256-byte aligned row pitches,
// unless the device lifts that.
static bool unrestricted_copy_pitch() {
    static volatile long s_known = 0; // 1: restricted, 2: unrestricted
    if (!s_known) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS13 o = {};
        bool lifted =
            SUCCEEDED(gpu_device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13, &o, sizeof(o))) &&
            o.UnrestrictedBufferTextureCopyPitchSupported;
        InterlockedExchange(&s_known, lifted ? 2 : 1);
    }
    return s_known == 2;
}

// Holds a reference to a resource the recorded copies use. False if there is no room for another.
static bool keep_source(TextureUploadSources* sources, ID3D12Resource* resource) {
    for (u32 i = 0; i < sources->count; i++)
        if (sources->resources[i] == resource)
            return true;
    if (sources->count == TEXTURE_UPLOAD_SOURCES - 1)
        return false; // the last entry is for the upload buffer
    resource->AddRef();
    sources->resources[sources->count++] = resource;
    return true;
}

// A linear texture of one array slice, copied by the GPU straight from title memory through its
// window: each mip is a footprint at its offset and pitch in the console layout. False (nothing
// recorded) if the layout isn't one copies can take.
static bool copy_from_title_memory(ID3D12GraphicsCommandList8* list, ConsoleResource* r, u64 size,
                                   TextureUploadSources* sources) {
    const D3D12_RESOURCE_DESC1& d = r->desc;
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D || d.DepthOrArraySize != 1)
        return false;
    FormatInfo console = format_info(d.Format);
    FormatInfo desktop = format_info(format_desktop_resource(d.Format));
    if (!console.bytes_per_element || console.bytes_per_element != desktop.bytes_per_element ||
        console.block_width != desktop.block_width)
        return false;
    ConsoleTextureDesc t = layout_desc(d);
    ConsoleTextureLayout layout;
    if (!console_texture_layout(t, &layout) || layout.plane_count != 1)
        return false;
    const ConsolePlaneLayout& p = layout.planes[0];
    bool unrestricted = unrestricted_copy_pitch();
    GpuAddress window;
    if (!gpu_translate(r->gpu_address, size, &window))
        return false;
    for (u32 m = 0; m < layout.mip_levels; m++) {
        u64 at = window.offset + p.base_offset + p.mips[m].offset;
        if (p.mips[m].swizzle_mode != 0 ||
            (!unrestricted && (at % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT ||
                               p.mips[m].pitch_bytes % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT)))
            return false;
    }
    u64 unused;
    ID3D12Resource* texture = console_resource_desktop(r, &unused);
    if (!texture || sources->count + 2 >= TEXTURE_UPLOAD_SOURCES)
        return false;
    keep_source(sources, window.buffer);
    keep_source(sources, texture);
    texture_barrier(list, texture, true);
    for (u32 m = 0; m < layout.mip_levels; m++) {
        const ConsoleMipLayout& mip = p.mips[m];
        u32 w = (u32)(d.Width >> m), h = d.Height >> m;
        u32 bw = ((w ? w : 1) + console.block_width - 1) / console.block_width;
        u32 bh = ((h ? h : 1) + console.block_height - 1) / console.block_height;
        D3D12_TEXTURE_COPY_LOCATION dst = {texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = m;
        D3D12_TEXTURE_COPY_LOCATION src = {window.buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint.Offset = window.offset + p.base_offset + mip.offset;
        src.PlacedFootprint.Footprint = {format_desktop_resource(d.Format), bw * console.block_width,
                                         bh * console.block_height, 1, mip.pitch_bytes};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    texture_barrier(list, texture, false);
    return true;
}

#define MAX_SUBRESOURCES 512
#define MAX_UPLOADS      64

bool texture_sync_record(ID3D12GraphicsCommandList8* list, TextureUploadSources* sources) {
    sources->count = 0;
    if (!texture_sync_pending())
        return false;
    static SubresourceCopy s_copies[MAX_UPLOADS][MAX_SUBRESOURCES]; // under g_sync.lock
    ConsoleResource* uploads[MAX_UPLOADS];
    u32 copy_counts[MAX_UPLOADS];
    u32 upload_count = 0;
    u64 total = 0;
    bool copied = false;

    AcquireSRWLockExclusive(&g_sync.lock);
    // Linear textures the GPU can copy from title memory are copied now; the others are uploaded
    // from the CPU within the budget, and the rest wait for the next submission.
    for (u32 i = 0; i < g_sync.count; i++) {
        SyncEntry& e = g_sync.entries[i];
        if (!e.dirty)
            continue;
        bool mapped = mem_is_mapped((const void*)e.start, e.end - e.start);
        if (mapped && e.linear && copy_from_title_memory(list, e.resource, e.end - e.start, sources))
            mapped = false, copied = true; // done
        else if (mapped && (upload_count == MAX_UPLOADS || total >= UPLOAD_BUDGET))
            continue;
        e.dirty = false;
        InterlockedDecrement(&g_sync.dirty);
        if (!mapped)
            continue; // copied, or nothing to read yet (pages arrive with a later mapping)
        if (e.linear)
            LOG_W_ONCE(
                "d3d12",
                "linear textures the GPU can't copy from title memory (texture at 0x%llx, %llux%u x%u, "
                "format %d) are uploaded by the CPU every frame",
                e.start, e.resource->desc.Width, e.resource->desc.Height, e.resource->desc.DepthOrArraySize,
                e.resource->desc.Format);
        u64 offset = total;
        u64 unused;
        ID3D12Resource* texture = console_resource_desktop(e.resource, &unused);
        if (!texture || !plan_texture(e.resource, s_copies[upload_count], MAX_SUBRESOURCES,
                                      &copy_counts[upload_count], &offset))
            continue;
        if (!keep_source(sources, texture)) {
            e.dirty = true; // no room left: with the next submission
            InterlockedIncrement(&g_sync.dirty);
            continue;
        }
        total = offset;
        uploads[upload_count++] = e.resource;
    }
    if (!upload_count) {
        ReleaseSRWLockExclusive(&g_sync.lock);
        return copied;
    }

    ID3D12Resource* buffer = nullptr;
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = total;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    u8* mapped = nullptr;
    HRESULT hr = gpu_device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&buffer));
    if (SUCCEEDED(hr)) {
        gpu_set_name(buffer, "texture upload (%llu KiB)", total >> 10);
        hr = buffer->Map(0, nullptr, (void**)&mapped);
    }
    if (FAILED(hr)) {
        LOG_E("d3d12", "cannot create a %llu MiB texture upload buffer (0x%08lx)", total >> 20,
              (unsigned long)hr);
        if (buffer)
            buffer->Release();
        // Try again with the next submission.
        for (u32 u = 0; u < upload_count; u++)
            for (u32 i = lower_bound(uploads[u]->gpu_address); i < g_sync.count; i++)
                if (g_sync.entries[i].resource == uploads[u] && !g_sync.entries[i].dirty) {
                    g_sync.entries[i].dirty = true;
                    InterlockedIncrement(&g_sync.dirty);
                    break;
                }
        ReleaseSRWLockExclusive(&g_sync.lock);
        return copied;
    }

    for (u32 u = 0; u < upload_count; u++) {
        ConsoleResource* r = uploads[u];
        u64 unused;
        ID3D12Resource* texture = console_resource_desktop(r, &unused);
        ConsoleTextureDesc t = layout_desc(r->desc);
        ConsoleTextureLayout layout;
        if (!texture || !console_texture_layout(t, &layout))
            continue;
        texture_barrier(list, texture, true);
        u32 mips = r->desc.MipLevels ? r->desc.MipLevels : 1;
        for (u32 k = 0; k < copy_counts[u]; k++) {
            const SubresourceCopy& c = s_copies[u][k];
            if (!console_copy_subresource(t, layout, 0, c.mip, c.slice, (void*)r->gpu_address,
                                          mapped + c.offset, c.row_pitch, c.slice_pitch, false))
                continue;
            D3D12_TEXTURE_COPY_LOCATION dst = {texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            dst.SubresourceIndex = c.mip + c.slice * mips;
            D3D12_TEXTURE_COPY_LOCATION src = {buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            src.PlacedFootprint.Offset = c.offset;
            src.PlacedFootprint.Footprint = {format_desktop_resource(r->desc.Format), c.width, c.height,
                                             c.depth, (UINT)c.row_pitch};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        texture_barrier(list, texture, false);
    }
    ReleaseSRWLockExclusive(&g_sync.lock);
    buffer->Unmap(0, nullptr);
    sources->resources[sources->count++] = buffer; // room was left for it
    return true;
}
