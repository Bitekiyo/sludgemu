#pragma once

#include "gpu/console_layout.h"
#include "gpu/gpu_ring.h"
#include "hle/d3d12/d3d12_objects.h"

// Textures whose contents come from title memory.
//
// Console textures live in title memory; desktop textures are separate GPU resources. A texture the
// GPU only reads (no render target, depth or unordered access use) gets its contents from title memory
// whenever that memory gets new contents: when the texture is created (titles create textures over
// data they loaded), when pages are mapped into it (physical page mappings, GPU page mapping copies),
// when the CPU writes it (glyph caches, video frames), and when a device finishes writing it
// (DirectStorage requests, see mem_expect_device_writes; memory being written waits for that). Loading
// a texture arms a write watch on its memory (see mem_watch_writes): the first write after that marks
// it again, so the next submission loads what the CPU wrote before it, as the console's GPU would read
// it. Loading is GPU work at the start of a submission: the GPU detiles the texture from title memory
// (see gpu_untile), or copies a linear one straight from it.
//
// On desktop such textures stay in the common layout: title barriers keep their synchronization but
// change no layouts, so uploads can run between any two submissions.
//
// Titles map memory into textures page by page as they stream them in, and views only reach the mips
// streamed in so far. Desktop textures of such memory are reserved resources: their tiles get VRAM
// (gpu/tile_pool.h) while the title memory their texels come from is mapped, and give it back when the
// title unmaps it, so they take what the console's take. Linear textures (written by the CPU, copied
// whole) are ordinary ones.

void texture_sync_init();

// Diagnostics: SLUDGE_TRACE_TEXTURES=<width>x<height> logs the life of textures of that size (creation,
// layout, views, marks, loads and what title memory held then); with SLUDGE_TRACE_TEXTURES_DUMP=<dir>
// each load also writes the memory it read there. Whether a texture of this description is traced.
bool texture_sync_traced(const D3D12_RESOURCE_DESC1& desc);
// Whether a traced texture overlaps [address, address + size); false at once when nothing is traced.
bool texture_sync_trace_overlaps(u64 address, u64 size);

// Whether a texture with this console description is kept in sync with its title memory.
bool texture_sync_tracks(const D3D12_RESOURCE_DESC1& desc);
// Whether such a texture's desktop texture should be reserved, its tiles following the title's mappings
// (where desktop D3D12 allows reserved textures of its kind; see ConsoleResource::reserved).
bool texture_sync_tiles(const D3D12_RESOURCE_DESC1& desc);

void texture_sync_add(ConsoleResource* r);
void texture_sync_remove(ConsoleResource* r);
// Tiles the reserved texture of `r` (at title address `start`) holds now; 0 for others (memory reports,
// `r` is only compared, so it may be gone).
u32 texture_sync_tiles_held(u64 start, const ConsoleResource* r);
// The console layout description of a texture description (console_texture_layout).
ConsoleTextureDesc texture_layout_desc(const D3D12_RESOURCE_DESC1& desc);

// Title memory [address, address + size) has new contents. Mapping changes call this themselves.
void texture_sync_mark(u64 address, u64 size);

// Whether uploads are waiting; cheap.
bool texture_sync_pending();

// What recorded uploads use: the textures they write, the title memory windows they read and the
// scratch ring, each with a reference to retire (gpu_retire) once the list is submitted. Textures and
// windows can be retired by other threads (a texture's last view rewritten, a mapping change) before
// the list is submitted; the references here keep them until the GPU is done.
#define TEXTURE_UPLOAD_SOURCES 256
struct TextureUploadSources {
    ID3D12Resource* resources[TEXTURE_UPLOAD_SOURCES];
    u32 count;
};

// Records the waiting uploads into `list` (a direct or compute list for `queue`), detiled on the GPU
// into the queue's `scratch` ring; textures whose memory isn't one block for the GPU are copied into
// its `staging` ring (an upload ring) first. Both rings get gpu_ring_submitted once the list is
// submitted. Tile mappings of reserved textures change on `queue` right away, so the list must be the
// queue's next, and the queue must count a submission after them even if nothing was recorded (their
// textures are in `sources` then). Returns whether anything was recorded.
bool texture_sync_record(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList8* list,
                         TextureUploadSources* sources, GpuRing* scratch, GpuRing* staging);
