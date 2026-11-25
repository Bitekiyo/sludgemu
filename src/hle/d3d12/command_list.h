#pragma once

#include "hle/d3d12/d3d12_objects.h"

// Shared by the command list sources: command_list.cpp (lifetime and state) and
// command_list_work.cpp (draws, dispatches, copies, clears, queries).

// The desktop handle for a console GPU descriptor handle in one of the list's bound heaps.
bool list_gpu_descriptor(const ConsoleCommandList* list, u64 handle, D3D12_GPU_DESCRIPTOR_HANDLE* out);

// The desktop CPU-only handle for `count` console descriptors starting at `handle`.
bool list_cpu_descriptor(u64 handle, u32 count, D3D12_CPU_DESCRIPTOR_HANDLE* out);

// The desktop resource behind a console resource and where the console resource starts in it.
ID3D12Resource* list_resource(void* console_resource, u64* offset);

extern const ConsoleEntry k_list_work_entries[];
extern const u32 k_list_work_entry_count;
