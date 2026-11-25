#pragma once

#include <d3d12.h>

#include "common/types.h"

// Shows console presents in the title's window.
//
// A console present hands over up to two planes: the scene and, on top of it, a HUD with
// premultiplied alpha, each placed in a normalized rectangle of the 16:9 display. The presenter
// draws them, scaled, into a flip-model swap chain on the window, letterboxed to keep the aspect
// ratio whatever the window's size.

struct PresentPlane {
    ID3D12Resource* texture; // desktop texture, in a layout shaders can read (common)
    D3D12_RECT source;       // texels to show; empty for the whole texture
    f32 dest[4];             // left, top, right, bottom, normalized to the display
};

#define PRESENT_MAX_PLANES 2

// Draws `count` planes (bottom first) on `queue` after its earlier work and presents. The swap chain
// is created on the first call's queue, which must be a direct queue.
bool presenter_present(ID3D12CommandQueue* queue, const PresentPlane* planes, u32 count);
