#pragma once

#include <windows.h>
#include <d3d12.h>

#include "hle/d3d12/console_object.h"

// Console D3D12 object types and their creation functions (one source file per type).

// Console descriptor sizes (raw hardware descriptors, fixed per heap type).
#define CONSOLE_DESCRIPTOR_SIZE_RESOURCE 32 // CBV / SRV / UAV
#define CONSOLE_DESCRIPTOR_SIZE_SAMPLER  16
#define CONSOLE_DESCRIPTOR_SIZE_RTV      112
#define CONSOLE_DESCRIPTOR_SIZE_DSV      128

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

// The command buffer write cursor of a console command list. Inlined title code writes packets only
// while current < limit_draw / limit_api; we keep them null.
struct ConsolePutBuffer {
    u32* current;
    u32* limit;
    u32* limit_draw;
    u32* limit_api;
    u32* unbiased_limit_api;
    u32 reserved[64];
};

struct ConsoleCommandList {
    ConsoleObjectHeader header;
    D3D12_COMMAND_LIST_TYPE type;
    ConsolePutBuffer putter;
    u8 cpu_state[4];                      // shmem dirty, wave32 PSO, last draw indexed, predication used
    u32 graphics_root_packet_header[128]; // per root parameter, set with the root signature
    u32 compute_root_packet_header[128];
    u32 graphics_pso_shadow[8]; // copy of the bound pipeline state's packet

    ID3D12GraphicsCommandList7* desktop;
};

struct ConsoleCommandListDesc {
    D3D12_COMMAND_LIST_TYPE type;
    u32 flags;
    u32 node_mask;
    u32 resource_barrier_batch_size;
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
void console_command_list_init();

HRESULT console_create_command_queue(ConsoleDevice* device, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID iid,
                                     void** out);
HRESULT console_create_command_allocator(ConsoleDevice* device, D3D12_COMMAND_LIST_TYPE type, REFIID iid,
                                         void** out);
HRESULT console_create_command_list(ConsoleDevice* device, const ConsoleCommandListDesc* desc,
                                    ConsoleCommandAllocator* allocator, void* initial_state, REFIID iid,
                                    void** out);
HRESULT console_create_fence(ConsoleDevice* device, UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID iid,
                             void** out);
