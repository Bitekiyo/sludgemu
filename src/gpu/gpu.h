#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "common/types.h"

// The desktop D3D12 device everything is translated to (Agility SDK runtime).

struct GpuOptions {
    bool debug_layer; // D3D12 debug layer + GPU-based validation off (too slow for a title)
};

void gpu_set_options(const GpuOptions& options);

// Creates the desktop device on first use. Returns nullptr (and logs why) if no suitable
// adapter exists.
ID3D12Device13* gpu_device();
IDXGIFactory6* gpu_factory();

// Command queues running translated work. Registering gives a queue a slot whose fence counts its
// submissions; call gpu_queue_submitted after every ExecuteCommandLists. Unregistering waits for
// the queue to finish its work.
#define GPU_NO_QUEUE 0xffffffffu
u32 gpu_queue_register(ID3D12CommandQueue* queue);
void gpu_queue_unregister(u32 slot);
void gpu_queue_submitted(u32 slot);

// Releases a desktop object once no queue can still be using it: after every queue finished what
// it had been given when the object was retired.
void gpu_retire(IUnknown* object);

// Waits until every queue finished all submitted work.
void gpu_wait_idle();
