#pragma once

#include "hle/d3d12/d3d12_objects.h"

// Textures whose contents come from title memory.
//
// Console textures live in title memory; desktop textures are separate GPU resources. A texture the
// GPU only reads (no render target, depth or unordered access use) gets its contents uploaded from
// title memory, detiled from the console layout, whenever that memory gets new contents: when the
// texture is created (titles create textures over data they loaded), when pages are mapped into it
// (physical page mappings, GPU page mapping copies), and when the CPU writes it (glyph caches, video
// frames). Uploading a texture write-protects its memory (see mem_watch_writes): the first write after
// that marks it again, so the next submission uploads what the CPU wrote before it, as the console's
// GPU would read it. Linear textures are copied by the GPU straight from title memory.
//
// On desktop such textures stay in the common layout: title barriers keep their synchronization but
// change no layouts, so uploads can run between any two submissions.

void texture_sync_init();

// Whether a texture with this console description is kept in sync with its title memory.
bool texture_sync_tracks(const D3D12_RESOURCE_DESC1& desc);

void texture_sync_add(ConsoleResource* r);
void texture_sync_remove(ConsoleResource* r);

// Title memory [address, address + size) has new contents. Mapping changes call this themselves.
void texture_sync_mark(u64 address, u64 size);

// Whether uploads are waiting; cheap.
bool texture_sync_pending();

// What recorded uploads use: the textures they write, the title memory windows and the upload buffer
// they read, each with a reference to retire (gpu_retire) once the list is submitted. Textures and
// windows can be retired by other threads (a texture's last view rewritten, a mapping change) before
// the list is submitted; the references here keep them until the GPU is done.
#define TEXTURE_UPLOAD_SOURCES 256
struct TextureUploadSources {
    ID3D12Resource* resources[TEXTURE_UPLOAD_SOURCES];
    u32 count;
};

// Records the waiting uploads into `list` (a direct or compute list). Returns whether anything was
// recorded.
bool texture_sync_record(ID3D12GraphicsCommandList8* list, TextureUploadSources* sources);
