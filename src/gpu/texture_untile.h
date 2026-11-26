#pragma once

#include "gpu/console_layout.h"
#include "gpu/gpu.h"

// Detiling console textures on the GPU (shaders/untile.hlsl): a compute pass reads a subresource's
// elements from title memory where the console layout puts them and writes them in the desktop
// footprint layout, for a copy into the desktop texture. Nothing touches texel data on the CPU.

// Records detiling one subresource. `source` is the desktop GPU address the subresource's addressing
// counts from (the texture's start in title memory, through a window); `destination` receives rows of
// `row_pitch` bytes, depth slices `slice_pitch` apart (4-byte aligned). The list must allow compute
// work; the root signature and pipeline are set here, so callers set theirs again afterwards.
// Destination writes are unordered accesses: a barrier must follow before the data is read.
bool gpu_untile(ID3D12GraphicsCommandList* list, u64 source, const ConsoleSubresourceAddressing& a,
                u64 destination, u32 row_pitch, u32 slice_pitch);
