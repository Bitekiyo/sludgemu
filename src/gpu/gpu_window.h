#pragma once

#include "gpu/gpu.h"

// Zero-copy GPU access to title memory.
//
// Title memory is shared by CPU and GPU on console. Here, each block of title memory the GPU uses
// (see MemBlock) gets a window: a desktop buffer placed on a heap opened over the host pages, so
// the GPU reads and writes the same RAM as the title's CPU. Opening a heap pins its pages, so
// windows are opened only when the GPU first needs a block, and closed when the title remaps or
// releases its memory.

struct GpuAddress {
    ID3D12Resource* buffer; // the window; valid until the GPU is done with work recorded now
    u64 offset;             // of the translated address within `buffer`
    u64 desktop_address;    // desktop GPU address of the same memory
};

void gpu_window_init();

// Translates title GPU addresses [address, address + size) to desktop memory. Fails (logged) if
// the range isn't mapped or isn't inside one block of title memory.
bool gpu_translate(u64 address, u64 size, GpuAddress* out);
