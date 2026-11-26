#pragma once

#include "gpu/gpu.h"

// VRAM for reserved textures, handed out in 64 KiB tiles from heaps of GPU_TILE_HEAP_TILES tiles.
//
// Console titles map memory into textures page by page as they stream their contents in; the desktop
// textures of such memory are reserved resources whose tiles get VRAM from here only where the title
// mapped memory, so they take what the console's do. A tile is a number naming its heap and its place
// in it. Thread safe.

#define GPU_TILE_SIZE       KiB(64)
#define GPU_TILE_HEAP_TILES 1024u // 64 MiB heaps
#define GPU_TILE_MAX_HEAPS  256u  // 16 GiB of tiles
#define GPU_NO_TILE         0xffffffffu

// `count` tiles, all from one heap (one tile mapping call covers them). False, logged once, if VRAM
// runs out.
bool gpu_tiles_alloc(u32 count, u32* tiles);

// Tiles a reserved resource stopped using; handed out again once the GPU finished the work submitted
// before this call.
void gpu_tiles_free(u32 count, const u32* tiles);

// The heap holding a tile and the tile's offset in it, in tiles (for UpdateTileMappings).
ID3D12Heap* gpu_tile_heap(u32 tile);
// Names the heap holding a tile apart from heaps the slot held before (0: no heap): a new heap can get a
// released one's address.
u32 gpu_tile_heap_serial(u32 tile);
inline u32 gpu_tile_offset(u32 tile) {
    return tile % GPU_TILE_HEAP_TILES;
}

// Bytes of tiles in use and of the heaps holding them (memory reports).
void gpu_tile_usage(u64* used, u64* reserved);
