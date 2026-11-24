#pragma once

#include <windows.h>
#include <d3d12.h>

#include "hle/d3d12/console_object.h"

// Console D3D12 object types and their creation functions (one source file per type).

struct ConsoleDevice {
    ConsoleObjectHeader header;
    u64 null_constant_buffer_address; // GPU address of a zero-filled constant buffer
    u8 out_of_line_flags[64];         // non-zero: titles route inlinable descriptor writes through the table

    ID3D12Device10* desktop;
};

struct ConsoleCommandQueue {
    ConsoleObjectHeader header;
    D3D12_COMMAND_LIST_TYPE type;

    ID3D12CommandQueue* desktop;
};

struct ConsoleFence {
    ConsoleObjectHeader header;

    ID3D12Fence1* desktop;
};

struct ConsoleCommandAllocator {
    ConsoleObjectHeader header;

    D3D12_COMMAND_LIST_TYPE type;
    ID3D12CommandAllocator* desktop;
};

// Console extended queue description (CreateCommandQueueX).
struct ConsoleQueueDescX {
    D3D12_COMMAND_LIST_TYPE type;
    u32 flags; // desktop queue flags | console priority in bits 24-27
    u32 engine_or_pipe_index;
    u32 queue_index;
};

// Builds each type's function table; called once when the device is created.
void console_command_queue_init();
void console_fence_init();
void console_command_allocator_init();

HRESULT console_create_command_queue(ConsoleDevice* device, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID iid,
                                     void** out);
HRESULT console_create_command_allocator(ConsoleDevice* device, D3D12_COMMAND_LIST_TYPE type, REFIID iid,
                                         void** out);
HRESULT console_create_fence(ConsoleDevice* device, UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID iid,
                             void** out);
