#pragma once

#include "hle/d3d12/d3d12_objects.h"

// Console barriers recorded as desktop enhanced barriers.
//
// Buffers are views into GPU windows, which several resources share, so they can't keep per-resource
// states; enhanced barriers give buffers only synchronization and access, which is all they need.
// Textures get layouts derived from the states the title names.

// The texture layout equivalent to a console resource state.
D3D12_BARRIER_LAYOUT console_layout_from_state(u32 state);

// The desktop value of a console texture layout. The console enumeration has a present layout of its
// own before the last one; desktop's present layout is its common layout.
D3D12_BARRIER_LAYOUT console_desktop_layout(u32 layout);

// The layout a console texture is in on desktop when the title names `layout` (a desktop value).
// Textures that lose UAV access on desktop (console_texture_loses_uav) take the common layouts in
// place of the UAV ones.
D3D12_BARRIER_LAYOUT console_texture_layout(const ConsoleResource* r, D3D12_BARRIER_LAYOUT layout);

// ResourceBarrier: legacy transition, aliasing and UAV barriers.
void console_record_resource_barriers(ID3D12GraphicsCommandList7* list, u32 count,
                                      const D3D12_RESOURCE_BARRIER* barriers);

// Barrier: enhanced barrier groups naming console resources.
void console_record_barrier_groups(ID3D12GraphicsCommandList7* list, u32 count,
                                   const D3D12_BARRIER_GROUP* groups);

// Waits for all earlier work and makes its writes visible to all later work.
void console_record_full_barrier(ID3D12GraphicsCommandList7* list);
