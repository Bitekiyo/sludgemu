#include "hle/d3d12/texture_sync.h"

#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "core/memory.h"
#include "gpu/console_layout.h"
#include "gpu/formats.h"
#include "gpu/gpu.h"
#include "gpu/gpu_window.h"
#include "gpu/texture_untile.h"

struct SyncEntry {
    u64 start;
    u64 end;
    ConsoleResource* resource; // registered while alive (texture_sync_remove before destruction)
    bool dirty;
    bool linear; // laid out for CPU writes: copied by the GPU where the layout allows
    // How its subresources are laid out, for detiling (made at the first upload; plan order).
    ConsoleSubresourceAddressing* addressing;
};

static struct {
    SyncEntry* entries; // sorted by start; textures may overlap (aliasing)
    u32 count;
    u32 capacity;
    SRWLOCK lock;
    volatile long dirty; // entries marked and not uploaded yet
} g_sync = {nullptr, 0, 0, SRWLOCK_INIT, 0};

// Detiled data per submission: the rest waits for the next one (a texture larger than this goes
// alone).
#define UPLOAD_BUDGET MiB(128)

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
    g_sync.entries[at] = {r->gpu_address, r->gpu_address + info.SizeInBytes, r, true, linear, nullptr};
    g_sync.count++;
    InterlockedIncrement(&g_sync.dirty);
    ReleaseSRWLockExclusive(&g_sync.lock);
}

void texture_sync_remove(ConsoleResource* r) {
    u64 start = 0, size = 0;
    AcquireSRWLockExclusive(&g_sync.lock);
    for (u32 i = lower_bound(r->gpu_address); i < g_sync.count && g_sync.entries[i].start == r->gpu_address;
         i++) {
        if (g_sync.entries[i].resource != r)
            continue;
        if (g_sync.entries[i].dirty)
            InterlockedDecrement(&g_sync.dirty);
        free(g_sync.entries[i].addressing);
        start = g_sync.entries[i].start;
        size = g_sync.entries[i].end - start;
        memmove(&g_sync.entries[i], &g_sync.entries[i + 1], (g_sync.count - i - 1) * sizeof(SyncEntry));
        g_sync.count--;
        break;
    }
    ReleaseSRWLockExclusive(&g_sync.lock);
    mem_unwatch_writes((const void*)start, size); // its memory needn't stay protected
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
    mem_set_write_listener(texture_sync_mark);
}

bool texture_sync_pending() {
    return g_sync.dirty != 0 || mem_device_writes_outstanding();
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
    if (!gpu_translate_contiguous(r->gpu_address, size, &window))
        return false; // not one block of memory for the GPU
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

// Console layouts whose addressing was checked against the layout (see
// console_subresource_addressing): textures described the same way skip the check.
#define CHECKED_LAYOUTS 4096
static u64 g_checked[CHECKED_LAYOUTS]; // hashes of descriptions; under g_sync.lock

static u64 desc_hash(const ConsoleTextureDesc& t) {
    u64 h = 0xcbf29ce484222325ull;
    const u8* b = (const u8*)&t;
    for (usize i = 0; i < sizeof(t); i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h | 1;
}

// The addressing of every subresource of a texture, in plan order (slice by slice, mip by mip);
// nullptr (logged) if its layout can't be detiled.
static ConsoleSubresourceAddressing* make_addressing(const ConsoleResource* r, u32 count) {
    ConsoleTextureDesc t = layout_desc(r->desc);
    ConsoleTextureLayout layout;
    if (!console_texture_layout(t, &layout))
        return nullptr;
    ConsoleSubresourceAddressing* a = (ConsoleSubresourceAddressing*)malloc(count * sizeof(*a));
    if (!a)
        return nullptr;
    u64 h = desc_hash(t);
    u32 slot = (u32)(h >> 40) % CHECKED_LAYOUTS;
    bool checked = g_checked[slot] == h;
    u32 mips = layout.mip_levels;
    for (u32 i = 0; i < count; i++) {
        // Each mip of the first slice is checked once per kind of texture.
        bool check = !checked && i < mips;
        if (!console_subresource_addressing(t, layout, 0, i % mips, i / mips, &a[i], check)) {
            LOG_W_ONCE("d3d12", "textures of format %d in layout 0x%x can't be detiled; they get no contents",
                       t.format, t.layout);
            free(a);
            return nullptr;
        }
    }
    g_checked[slot] = h;
    return a;
}

// A texture to detile in this submission: its subresources' places in the scratch ring.
// Memory the GPU can't see as one block (frames that don't follow each other) is copied as it is into
// the staging ring first, and detiled from there.
#define NOT_STAGED ~0ull

struct Load {
    SyncEntry* entry;
    ID3D12Resource* texture;
    u64 source;       // desktop GPU address of the texture's memory (or its staged copy)
    u64 staged;       // offset of its copy in this submission's staging region, or NOT_STAGED
    u32 first, count; // in s_copies
};

static SubresourceCopy s_copies[MAX_UPLOADS * 16]; // under g_sync.lock

static void scratch_barrier(ID3D12GraphicsCommandList8* list, ID3D12Resource* buffer,
                            D3D12_BARRIER_SYNC sync_before, D3D12_BARRIER_ACCESS before,
                            D3D12_BARRIER_SYNC sync_after, D3D12_BARRIER_ACCESS after) {
    D3D12_BUFFER_BARRIER b = {sync_before, sync_after, before, after, buffer, 0, UINT64_MAX};
    D3D12_BARRIER_GROUP g = {D3D12_BARRIER_TYPE_BUFFER, 1};
    g.pBufferBarriers = &b;
    list->Barrier(1, &g);
}

bool texture_sync_record(ID3D12GraphicsCommandList8* list, TextureUploadSources* sources, GpuRing* scratch,
                         GpuRing* staging) {
    sources->count = 0;
    mem_poll_device_writes(); // finished ones mark their textures
    if (!g_sync.dirty)
        return false;
    Load loads[MAX_UPLOADS];
    u32 load_count = 0, copy_count = 0;
    u64 total = 0, staged_total = 0;
    bool copied = false;

    AcquireSRWLockExclusive(&g_sync.lock);
    // Linear textures the GPU can copy from title memory are copied now; the others are detiled on
    // the GPU within the budget, and the rest wait for the next submission.
    for (u32 i = 0; i < g_sync.count; i++) {
        SyncEntry& e = g_sync.entries[i];
        if (!e.dirty)
            continue;
        // Still being written: uploaded once the writes finish (they mark it again). Protecting it now
        // would also fail writes the kernel makes.
        if (mem_device_writes_pending(e.start, e.end - e.start))
            continue;
        const D3D12_RESOURCE_DESC1& d = e.resource->desc;
        u32 subresources = (d.MipLevels ? d.MipLevels : 1) *
                           (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : d.DepthOrArraySize);
        bool full = load_count == MAX_UPLOADS || (load_count && total >= UPLOAD_BUDGET) ||
                    copy_count + subresources > ARRAY_COUNT(s_copies);
        if (full && !e.linear)
            continue;
        bool mapped = mem_is_mapped((const void*)e.start, e.end - e.start);
        // Watched before it is read: what the CPU writes from now on marks it again.
        if (mapped)
            mem_watch_writes((const void*)e.start, e.end - e.start);
        if (mapped && e.linear && copy_from_title_memory(list, e.resource, e.end - e.start, sources)) {
            e.dirty = false;
            InterlockedDecrement(&g_sync.dirty);
            copied = true;
            continue;
        }
        if (full)
            continue;
        e.dirty = false;
        InterlockedDecrement(&g_sync.dirty);
        if (!mapped)
            continue; // nothing to read yet: pages arrive with a later mapping, which marks it again
        u64 unused;
        ID3D12Resource* texture = console_resource_desktop(e.resource, &unused);
        u32 count = 0;
        u64 offset = total;
        if (!texture || !plan_texture(e.resource, &s_copies[copy_count], ARRAY_COUNT(s_copies) - copy_count,
                                      &count, &offset))
            continue;
        if (!e.addressing)
            e.addressing = make_addressing(e.resource, count);
        if (!e.addressing)
            continue;
        if (sources->count + 4 >= TEXTURE_UPLOAD_SOURCES) {
            e.dirty = true; // no room left: with the next submission
            InterlockedIncrement(&g_sync.dirty);
            continue;
        }
        keep_source(sources, texture);
        GpuAddress window = {};
        if (gpu_translate_contiguous(e.start, e.end - e.start, &window)) {
            keep_source(sources, window.buffer);
            loads[load_count++] = {&e, texture, window.desktop_address, NOT_STAGED, copy_count, count};
        } else {
            loads[load_count++] = {&e, texture, 0, staged_total, copy_count, count};
            staged_total = ALIGN_UP(staged_total + (e.end - e.start), (u64)256);
        }
        copy_count += count;
        total = offset;
    }
    u64 base = 0, staged_base = 0;
    if (load_count && (!gpu_ring_alloc(scratch, total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &base) ||
                       (staged_total && !gpu_ring_alloc(staging, staged_total, 256, &staged_base)))) {
        for (u32 l = 0; l < load_count; l++) { // with the next submission
            loads[l].entry->dirty = true;
            InterlockedIncrement(&g_sync.dirty);
        }
        load_count = 0;
    }
    if (!load_count) {
        ReleaseSRWLockExclusive(&g_sync.lock);
        return copied;
    }
    keep_source(sources, scratch->buffer);
    if (staged_total) {
        keep_source(sources, staging->buffer);
        for (u32 l = 0; l < load_count; l++) {
            Load& ld = loads[l];
            if (ld.staged == NOT_STAGED)
                continue;
            u64 size = ld.entry->end - ld.entry->start;
            mem_prepare((const void*)ld.entry->start, size);
            memcpy(staging->mapped + staged_base + ld.staged, (const void*)ld.entry->start, size);
            ld.source = staging->address + staged_base + ld.staged;
        }
    }

    // Detile everything into the ring, then copy it into the textures.
    scratch_barrier(list, scratch->buffer, D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    for (u32 l = 0; l < load_count; l++) {
        const Load& ld = loads[l];
        for (u32 k = 0; k < ld.count; k++) {
            const SubresourceCopy& c = s_copies[ld.first + k];
            gpu_untile(list, ld.source, ld.entry->addressing[k], scratch->address + base + c.offset,
                       (u32)c.row_pitch, (u32)c.slice_pitch);
        }
    }
    scratch_barrier(list, scratch->buffer, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_COPY,
                    D3D12_BARRIER_ACCESS_COPY_SOURCE);
    for (u32 l = 0; l < load_count; l++) {
        const Load& ld = loads[l];
        const D3D12_RESOURCE_DESC1& d = ld.entry->resource->desc;
        u32 mips = d.MipLevels ? d.MipLevels : 1;
        texture_barrier(list, ld.texture, true);
        for (u32 k = 0; k < ld.count; k++) {
            const SubresourceCopy& c = s_copies[ld.first + k];
            D3D12_TEXTURE_COPY_LOCATION dst = {ld.texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            dst.SubresourceIndex = c.mip + c.slice * mips;
            D3D12_TEXTURE_COPY_LOCATION src = {scratch->buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            src.PlacedFootprint.Offset = base + c.offset;
            src.PlacedFootprint.Footprint = {format_desktop_resource(d.Format), c.width, c.height, c.depth,
                                             (UINT)c.row_pitch};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        texture_barrier(list, ld.texture, false);
    }
    scratch_barrier(list, scratch->buffer, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                    D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS);
    ReleaseSRWLockExclusive(&g_sync.lock);
    return true;
}
