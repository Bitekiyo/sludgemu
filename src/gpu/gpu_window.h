#pragma once

#include "gpu/gpu.h"
#include "gpu/page_tables.h"

// GPU access to title memory in place, for what the CPU must see as the GPU writes it.
//
// The GPU reaches title memory through its page tables (gpu/page_tables.h). Small values the CPU polls
// while the GPU writes them (fence values, counters written at the end of work) are also written
// through windows: desktop buffers placed on heaps opened over the host pages, so the GPU writes the
// same RAM the title's CPU reads. Physical memory has one window per chunk, open for the rest of
// the run once the GPU first reaches the chunk. Other GPU-visible memory gets windows over the parts
// the GPU uses (opening a heap pins its pages), which close when the title remaps or releases that
// memory. Desktop addresses handed out stay valid that long, however long a recorded command list
// or a descriptor keeps them.

void gpu_window_init();

// Translates title GPU addresses [address, address + size) to a window over title memory. Fails
// (logged) if the range isn't mapped or isn't inside one block of title memory.
bool gpu_window_translate(u64 address, u64 size, GpuAddress* out);

// The same, but a range that isn't inside one block fails quietly: for callers with another way
// to reach such memory.
bool gpu_translate_contiguous(u64 address, u64 size, GpuAddress* out);

// Bytes of title memory the GPU sees through heaps opened over it (memory reports): physical memory
// chunks, other blocks, and windows over other blocks replaced by larger ones but kept (see
// gpu_window.cpp).
void gpu_window_usage(u64* chunks, u64* blocks, u64* replaced);

// Changes whenever title mappings under translated addresses change (never 0): views made of
// addresses translated before a change may show memory the title no longer maps there.
u32 gpu_window_generation();
