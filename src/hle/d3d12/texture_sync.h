#pragma once

#include "hle/d3d12/d3d12_objects.h"

// Textures whose contents come from title memory.
//
// Console textures live in title memory; desktop textures are separate GPU resources. A texture the
// GPU only reads (no render target, depth or unordered access use) gets its contents uploaded from
// title memory, detiled from the console layout, whenever that memory gets new contents: when pages
// are mapped into it by GPU page mapping copies (see page_pool.cpp), and for linear textures when a
// per-frame check sees their contents change.
//
// On desktop such textures stay in the common layout: title barriers keep their synchronization but
// change no layouts, so uploads can run between any two submissions.

// Whether a texture with this console description is kept in sync with its title memory.
bool texture_sync_tracks(const D3D12_RESOURCE_DESC1& desc);

void texture_sync_add(ConsoleResource* r);
void texture_sync_remove(ConsoleResource* r);

// Title memory [address, address + size) has new contents.
void texture_sync_mark(u64 address, u64 size);

// Linear textures (the layout titles write with the CPU, through aliases of their pages as well)
// whose contents changed since their last upload become waiting uploads. Once per frame.
void texture_sync_check_linear();

// Whether uploads are waiting; cheap.
bool texture_sync_pending();

// Records the waiting uploads into `list` (a direct or compute list). Returns the upload buffer the
// copies read, to be retired once the list is submitted, or nullptr if nothing was recorded.
ID3D12Resource* texture_sync_record(ID3D12GraphicsCommandList8* list);
