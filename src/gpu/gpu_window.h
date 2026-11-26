#pragma once

#include "gpu/gpu.h"

// Zero-copy GPU access to title memory.
//
// Title memory is shared by CPU and GPU on console. Here, the GPU reaches title memory through
// windows: desktop buffers placed on heaps opened over the host pages, so the GPU reads and writes
// the same RAM as the title's CPU. Physical memory has one window per chunk, open for the rest of
// the run once the GPU first reaches the chunk. Other GPU-visible memory gets windows over the parts
// the GPU uses (opening a heap pins its pages), which close when the title remaps or releases that
// memory. Desktop addresses handed out stay valid that long, however long a recorded command list
// or a descriptor keeps them.

struct GpuAddress {
    ID3D12Resource* buffer; // the window
    u64 offset;             // of the translated address within `buffer`
    u64 desktop_address;    // desktop GPU address of the same memory
    u64 available;          // bytes from there the same translation reaches (at least the size asked for)
};

void gpu_window_init();

// Translates title GPU addresses [address, address + size) to desktop memory. Fails (logged) if
// the range isn't mapped or isn't inside one block of title memory.
bool gpu_translate(u64 address, u64 size, GpuAddress* out);

// The same, but a range that isn't inside one block fails quietly: for callers with another way
// to reach such memory.
bool gpu_translate_contiguous(u64 address, u64 size, GpuAddress* out);

// Changes whenever title mappings under translated addresses change (never 0): views made of
// addresses translated before a change may show memory the title no longer maps there.
u32 gpu_window_generation();
