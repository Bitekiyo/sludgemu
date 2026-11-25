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

// Releases a desktop object once the GPU can no longer be using it.
void gpu_retire(IUnknown* object);
