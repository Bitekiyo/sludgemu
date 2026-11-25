#pragma once

#include <windows.h>
#include <d3d12.h>

#include "gpu/gpu_window.h"
#include "hle/d3d12/console_object.h"

// Console D3D12 object types and their creation functions (one source file per type).

// Console descriptor sizes (raw hardware descriptors, fixed per heap type).
#define CONSOLE_DESCRIPTOR_SIZE_RESOURCE 32 // CBV / SRV / UAV
#define CONSOLE_DESCRIPTOR_SIZE_SAMPLER  16
#define CONSOLE_DESCRIPTOR_SIZE_RTV      112
#define CONSOLE_DESCRIPTOR_SIZE_DSV      128

// XMemAlloc object types the console runtime uses for GPU memory: resources (descriptor heaps
// included) and displayable surfaces.
#define XMEM_D3D_OBJECT_RESOURCE    4
#define XMEM_D3D_OBJECT_PRESENTABLE 19

// Console heap type 4: a sampler heap without the usual size limit (with fewer border colors).
#define CONSOLE_DESCRIPTOR_HEAP_TYPE_SAMPLER_UNBOUNDED 4

// Size of one console descriptor of a heap type, 0 for an unknown type.
u32 console_descriptor_size(u32 heap_type);

struct ConsoleDevice {
    ConsoleObjectHeader header;
    u64 null_constant_buffer_address; // GPU address of a zero-filled constant buffer
    u8 out_of_line_flags[64];         // non-zero: titles route inlinable descriptor writes through the table

    ID3D12Device13* desktop;
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

// A resource at a title-chosen GPU address. Title memory is shared by CPU and GPU on console, so
// the GPU address is also the CPU address of the resource's memory. Buffers are the title memory
// itself, seen through GPU windows (gpu/gpu_window.h); textures get a desktop texture on first use,
// since desktop texture layouts differ from console ones.
struct ConsoleResource {
    ConsoleObjectHeader header;

    D3D12_RESOURCE_DESC1 desc;
    u64 gpu_address;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_HEAP_FLAGS heap_flags;
    D3D12_BARRIER_LAYOUT initial_layout;
    bool has_clear_value;
    D3D12_CLEAR_VALUE clear_value;

    void* owned_memory; // committed resources: memory the runtime allocated (XMemAlloc)
    u64 owned_attributes;

    SRWLOCK lock;            // guards creating the desktop texture
    ID3D12Resource* texture; // textures only
};

void console_resource_init();

HRESULT console_create_placed_resource(ConsoleDevice* device, u64 gpu_address,
                                       const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT initial_layout,
                                       const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out);

D3D12_RESOURCE_DESC1 console_desc1_from_desc(const D3D12_RESOURCE_DESC& desc);

// Memory a resource takes when the title places it.
D3D12_RESOURCE_ALLOCATION_INFO console_resource_allocation_info(const D3D12_RESOURCE_DESC1& desc);

// A committed resource: the runtime allocates its memory, like the console runtime, through
// XMemAlloc.
HRESULT console_create_committed_resource(ConsoleDevice* device, const D3D12_HEAP_PROPERTIES* heap,
                                          D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1* desc,
                                          D3D12_BARRIER_LAYOUT initial_layout, const D3D12_CLEAR_VALUE* clear,
                                          REFIID iid, void** out);

// The desktop resource backing `resource` (created on first use) and the offset of the title
// resource's first byte in it. nullptr if it can't be created.
ID3D12Resource* console_resource_desktop(ConsoleResource* resource, u64* offset);

// Translates title GPU addresses [address, address + size) to the desktop GPU address of the same
// memory (see gpu_translate).
bool console_translate_gpu_address(u64 address, u64 size, u64* desktop_address);

// A descriptor heap. Console descriptors live in title memory and handles are plain addresses into
// it (CPU and GPU handles alike), which title code steps through with the console descriptor sizes.
// Each heap is mirrored by desktop descriptors of the same type and count: console slot i is
// desktop slot i, and every descriptor write goes to both. Desktop D3D12 cannot read descriptors
// back from shader-visible heaps, so those also keep a CPU-only copy that writes go to first and
// copies read from.
struct ConsoleDescriptorHeap {
    ConsoleObjectHeader header;

    D3D12_DESCRIPTOR_HEAP_DESC desc;
    u8* memory;
    u32 stride;
    u32 desktop_count; // can be smaller than desc.NumDescriptors (desktop limits)
    D3D12_DESCRIPTOR_HEAP_TYPE desktop_type;
    ID3D12DescriptorHeap* cpu;     // CPU-only descriptors (every heap)
    ID3D12DescriptorHeap* visible; // shader-visible descriptors, or nullptr
    u64 cpu_start;
    u64 visible_cpu_start;
    u64 visible_gpu_start;
};

// Where a console descriptor handle points.
struct DescriptorSlot {
    D3D12_DESCRIPTOR_HEAP_TYPE type; // desktop heap type
    u32 desktop_stride;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;         // CPU-only copy: write and copy from here
    D3D12_CPU_DESCRIPTOR_HANDLE visible_cpu; // shader-visible copy, 0 if none
    D3D12_GPU_DESCRIPTOR_HANDLE visible_gpu;
    u8* console; // the console slot
};

void console_descriptor_heap_init();

HRESULT console_create_descriptor_heap(ConsoleDevice* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                       REFIID iid, void** out);

// Finds where `count` descriptors starting at a console CPU or GPU handle live. False (logged) if
// they are not all inside one live heap and within what its desktop mirror holds.
bool console_descriptor_slots(u64 handle, u32 count, DescriptorSlot* out);

// After writing `count` descriptors at slot.cpu, updates the shader-visible copy.
void console_descriptor_publish(const DescriptorSlot& slot, u32 count);
