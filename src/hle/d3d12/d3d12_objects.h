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

// Small desktop lists that write fence values to memory when the queue signals a fence.
#define QUEUE_SIGNAL_WRITERS 4

struct QueueSignalWriter {
    ID3D12CommandAllocator* allocator;
    ID3D12GraphicsCommandList8* list;
    u64 submission; // the queue submission that last ran it
};

struct ConsoleCommandQueue {
    ConsoleObjectHeader header;
    D3D12_COMMAND_LIST_TYPE type;

    ID3D12CommandQueue* desktop;
    u32 slot; // submission tracking (gpu_queue_register)
    SRWLOCK writer_lock;
    u32 next_writer;
    QueueSignalWriter writers[QUEUE_SIGNAL_WRITERS];
};

// Console fences also have a GPU address holding their value, which command lists write to signal
// them and wait on. Command lists turn such writes and waits into desktop fence signals and waits
// between the parts of a split list (see ConsoleCommandList).
struct ConsoleFence {
    ConsoleObjectHeader header;

    ID3D12Fence1* desktop;
    volatile u64* value; // GPU-visible title memory
    u64 value_desktop;   // its desktop GPU address, once known
};

// The fence whose value lives at `address`, or nullptr.
ConsoleFence* console_fence_at(u64 address);

// The desktop fence that follows the 64-bit counter at title GPU address `address`: a console
// fence's own, or one made on first use for memory the title writes and waits on from the GPU.
// Borrowed; nullptr if the address isn't GPU-visible.
ID3D12Fence* console_sync_fence(u64 address);

// The allocation callback lets titles supply console command buffer memory. Recording goes to
// desktop command lists instead, so it is kept but never called.
typedef void* (*ConsoleAllocatorCallback)(void* allocator, void* list, u32 xmem_type, usize size,
                                          u32 alignment, u32 flags, u64 context);

struct ConsoleCommandAllocator {
    ConsoleObjectHeader header;

    D3D12_COMMAND_LIST_TYPE type;
    ID3D12CommandAllocator* desktop;
    ConsoleAllocatorCallback callback;
    u64 callback_context;
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

struct ConsoleDescriptorHeap;
struct ConsoleRootSignature;
struct ConsoleFence;

// Root arguments as recorded on the desktop list (already translated).
enum RootArgKind : u8 {
    ROOT_ARG_NONE,
    ROOT_ARG_TABLE,
    ROOT_ARG_CONSTANTS,
    ROOT_ARG_CBV,
    ROOT_ARG_SRV,
    ROOT_ARG_UAV,
};

struct RootArgs {
    ConsoleRootSignature* signature;
    RootArgKind kind[64];
    u64 value[64];     // table handle or GPU address
    u32 constants[64]; // root constants, at each parameter's constant_offset
};

// Everything a command list has set that later commands depend on. When a list is split, the new
// desktop list starts from scratch and gets this state again.
struct ListState {
    ConsoleDescriptorHeap* heaps[2]; // bound shader-visible heaps: views, samplers
    RootArgs graphics;
    RootArgs compute;
    ID3D12PipelineState* pipeline; // nullptr until pipeline states are translated
    D3D12_PRIMITIVE_TOPOLOGY topology;
    u32 vertex_buffer_mask;
    D3D12_VERTEX_BUFFER_VIEW vertex_buffers[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    bool has_index_buffer;
    D3D12_INDEX_BUFFER_VIEW index_buffer;
    u32 viewport_count;
    D3D12_VIEWPORT viewports[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    u32 scissor_count;
    D3D12_RECT scissors[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    bool has_targets;
    u32 render_target_count;
    D3D12_CPU_DESCRIPTOR_HANDLE render_targets[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
    bool has_depth;
    D3D12_CPU_DESCRIPTOR_HANDLE depth;
    bool has_blend_factor;
    f32 blend_factor[4];
    bool has_stencil_ref;
    u32 stencil_front;
    u32 stencil_back;
    bool has_depth_bounds;
    f32 depth_min;
    f32 depth_max;
    ID3D12Resource* predication;
    u64 predication_offset;
    D3D12_PREDICATION_OP predication_op;
};

// What happens on the queue between two parts of a split list.
enum ListSyncKind : u8 {
    LIST_SYNC_SIGNAL, // after the part before: signal the fence
    LIST_SYNC_WAIT,   // before the part after: wait for the fence
};

struct ListSegment {
    ID3D12GraphicsCommandList8* list;
    ListSyncKind sync;  // between this part and the next (none after the last)
    ID3D12Fence* fence; // referenced
    u64 value;
};

struct ConsoleCommandList {
    ConsoleObjectHeader header;
    D3D12_COMMAND_LIST_TYPE type;
    ConsolePutBuffer putter;
    u8 cpu_state[4];                      // shmem dirty, wave32 PSO, last draw indexed, predication used
    u32 graphics_root_packet_header[128]; // per root parameter, set with the root signature
    u32 compute_root_packet_header[128];
    u32 graphics_pso_shadow[8]; // copy of the bound pipeline state's packet

    // The desktop list recording goes to: the last segment's. Console lists split where the GPU
    // signals or waits on a fence in memory, which desktop D3D12 only does between lists.
    ID3D12GraphicsCommandList8* desktop;
    ListSegment* segments; // segment_capacity desktop lists, reused across resets
    u32 segment_count;
    u32 segment_capacity;
    ConsoleCommandAllocator* allocator; // the allocator of the current recording
    ID3D12Device13* device;
    bool recording;
    ListState state;
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
    u32 desktop_stride;
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

struct ConsoleQueryHeap {
    ConsoleObjectHeader header;

    D3D12_QUERY_HEAP_DESC desc; // as the title asked for it
    ID3D12QueryHeap* desktop;
};

// A command signature: the layout of indirect arguments. Console layouts can include arguments
// desktop D3D12 has no equivalent for (descriptor tables, pipeline states, padding), so the
// description is kept and turned into desktop commands when indirect work is translated.
struct ConsoleCommandSignature {
    ConsoleObjectHeader header;

    u32 byte_stride;
    u32 argument_count;
    D3D12_INDIRECT_ARGUMENT_DESC* arguments;
    void* root_signature; // console root signature (referenced), or nullptr
};

void console_query_heap_init();
void console_command_signature_init();

HRESULT console_create_query_heap(ConsoleDevice* device, const D3D12_QUERY_HEAP_DESC* desc, REFIID iid,
                                  void** out);
HRESULT console_create_command_signature(ConsoleDevice* device, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                         void* root_signature, REFIID iid, void** out);

// Root signatures have at most 64 DWORDs of arguments, so at most 64 parameters.
#define ROOT_MAX_PARAMETERS 64

// A root signature, kept in its desktop form plus the description for translating root arguments.
struct ConsoleRootSignature {
    ConsoleObjectHeader header;

    const D3D12_ROOT_SIGNATURE_DESC1* desc; // owned by `deserializer`
    ID3D12VersionedRootSignatureDeserializer* deserializer;
    ID3D12RootSignature* desktop;
    u8 constant_offset[ROOT_MAX_PARAMETERS]; // root constants: where each parameter's values start
};

void console_root_signature_init();

HRESULT console_create_root_signature(ConsoleDevice* device, const void* blob, usize size, REFIID iid,
                                      void** out);

// Page pools (page_pool.cpp): title memory registered as a source of GPU page mapping copies.
HRESULT console_register_page_pool(u64 address, u32 page_count, u32 flags, HANDLE* handle);
HRESULT console_unregister_page_pool(HANDLE handle);
