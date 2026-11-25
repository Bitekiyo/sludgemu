#pragma once

#include "shader/console_pipeline.h"
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
struct ConsolePipelineState;

// The output formats a draw renders to: desktop pipelines are made per set.
struct PipelineOutputs {
    u8 render_target_formats[8];
    u8 render_target_count;
    u8 depth_format;
    u8 sample_count;
    u8 rect_list; // drawn as console rectangle lists (geometry shader expansion)
};

// Root arguments: tables as desktop heap indices, root descriptors as desktop GPU addresses. They
// reach the desktop list before each draw or dispatch, as the user data translated shaders read.
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
    bool user_data_dirty[2];                 // graphics, compute: root arguments changed since the last flush
    ConsolePipelineState* pipeline;          // set by the title (not referenced: the title keeps it alive)
    ID3D12PipelineState* bound;              // the desktop pipeline set on the desktop list
    ID3D12RootSignature* bound_signature[2]; // graphics, compute: set on the desktop list
    PipelineOutputs outputs;                 // formats of the bound targets
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
    u32 skipped_draws; // draws and dispatches without a desktop pipeline yet
    ListState state;

    // Descriptor tables the recording uses, mirrored again when the list is submitted: titles may
    // fill tables after recording the draws that use them (the GPU reads them only when it runs).
    struct TableUse {
        ConsoleDescriptorHeap* heap;
        u32 first;
        u32 count;
        u32 desktop_first;
    }* tables;
    u32 table_count;
    u32 table_capacity;
    // Recent relocated tables of this recording, by console slot (see console_descriptor_place).
    struct Relocation {
        ConsoleDescriptorHeap* heap;
        u32 first;
        u32 count;
        u32 desktop_first;
    } relocations[16];
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

// The texture can't keep UAV access on desktop (depth textures that are also UAVs on the console).
bool console_texture_loses_uav(const D3D12_RESOURCE_DESC1& desc);

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
    // Every console slot has a CPU-only desktop descriptor. Shader-visible heaps are capped on
    // desktop: when the console heap is larger, its first `identity_count` slots keep their index
    // in the shader-visible heap, and tables past them are copied into a ring above them when used.
    u32 visible_count; // shader-visible desktop slots
    u32 identity_count;
    u32 relocation_first; // the ring: [relocation_first, relocation_first + relocation_count)
    u32 relocation_count;
    volatile long relocation_cursor;
    D3D12_DESCRIPTOR_HEAP_TYPE desktop_type;
    ID3D12DescriptorHeap* cpu;     // CPU-only descriptors (every heap)
    ID3D12DescriptorHeap* visible; // shader-visible descriptors, or nullptr
    u32 desktop_stride;
    u64 cpu_start;
    u64 visible_cpu_start;
    u64 visible_gpu_start;
    // Shader-visible resource heaps: per shader-visible slot, the constant buffer descriptor the
    // title wrote itself whose desktop view the slot holds (see console_descriptor_mirror).
    struct NativeView {
        u64 address;
        u64 records;
        u32 generation; // gpu_window_generation() when made, 0 if the slot holds something else
        u32 unused;
    }* native;
};

// Where a console descriptor handle points.
struct DescriptorSlot {
    D3D12_DESCRIPTOR_HEAP_TYPE type; // desktop heap type
    u32 desktop_stride;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;         // CPU-only copy: write and copy from here
    D3D12_CPU_DESCRIPTOR_HANDLE visible_cpu; // shader-visible copy at the same index, 0 if none
    D3D12_GPU_DESCRIPTOR_HANDLE visible_gpu;
    u8* console; // the console slot
    ConsoleDescriptorHeap* heap;
    u32 index;
};

void console_descriptor_heap_init();

// Console descriptor slots written by us start with this marker, then the slot's descriptor type
// and its desktop CPU-only descriptor.
#define DESCRIPTOR_MARKER_MAGIC 0x47444C53u // "SLDG"

// Brings console slots [first, first + count), which the title may have written or copied itself,
// to shader-visible desktop slots from `desktop_first`: the same index or a relocation.
void console_descriptor_mirror(ConsoleDescriptorHeap* heap, u32 first, u32 count, u32 desktop_first);

// Where a table of `count` console slots from `first` is visible to shaders: the same index if the
// heap shows it there, else room in the relocation ring (to be mirrored into). ~0u if neither.
u32 console_descriptor_place(ConsoleDescriptorHeap* heap, u32 first, u32 count);

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

// A root signature. Translated shaders read the console's user data (the root arguments as they lie
// in user SGPRs) from root constants, and root descriptors from root SRVs/UAVs; the desktop
// signature has that layout (see shader/translate.h).
#define ROOT_USER_DATA_SPACE        100 // root constants (b0) and root descriptors (t<n>/u<n>, n = parameter)
#define ROOT_STATIC_SAMPLER_SPACE   102
#define ROOT_MAX_SIGNATURE_VARIANTS 16

struct ConsoleRootSignature {
    ConsoleObjectHeader header;

    const D3D12_ROOT_SIGNATURE_DESC1* desc; // owned by `deserializer`
    ID3D12VersionedRootSignatureDeserializer* deserializer;
    ID3D12RootSignature* desktop;
    u8 constant_offset[ROOT_MAX_PARAMETERS]; // root constants: where each parameter's values start

    ConsoleRootLayout layout;                  // for shader translation
    u8 user_dwords;                            // user data dwords in the desktop root constants
    u8 user_sgpr[ROOT_MAX_PARAMETERS];         // each parameter's first user SGPR
    s8 desktop_parameter[ROOT_MAX_PARAMETERS]; // root descriptors: their desktop root parameter

    // Desktop signatures that also carry pipelines' inline samplers, by their samplers' hash.
    SRWLOCK variant_lock;
    u32 variant_count;
    struct SignatureVariant {
        u64 key;
        ID3D12RootSignature* desktop;
    } variants[ROOT_MAX_SIGNATURE_VARIANTS];
};

// Programs' inline samplers, for the stage that sees them.
struct SamplerSet {
    const InlineSampler* samplers;
    u32 count;
    D3D12_SHADER_VISIBILITY visibility;
};

// The desktop signature for pipelines whose programs have these inline samplers: the translated
// signature itself without any. Nullptr (logged) if it can't be made.
ID3D12RootSignature* console_root_signature_desktop(ConsoleRootSignature* rs, const SamplerSet* sets,
                                                    u32 set_count);

void console_root_signature_init();

HRESULT console_create_root_signature(ConsoleDevice* device, const void* blob, usize size, REFIID iid,
                                      void** out);

// Serialized console pipelines as titles hand them to DeserializeGraphicsPipelineStateX and
// DeserializeComputePipelineStateX: the root signature, the packet (registers), pipeline metadata,
// then per stage the GPU code and its (debugging) metadata. Graphics pipelines list the hardware
// stages pixel, vertex, geometry, export, hull, local and fetch; compute pipelines only compute.
struct ConsoleSerializedShader {
    const void* code;
    usize code_size;
    const void* metadata;
    usize metadata_size;
};

enum ConsoleHardwareStage : u32 {
    HW_STAGE_PS,
    HW_STAGE_VS,
    HW_STAGE_GS,
    HW_STAGE_ES,
    HW_STAGE_HS,
    HW_STAGE_LS,
    HW_STAGE_FS,
    HW_STAGE_COUNT,
};

struct ConsoleSerializedPipeline {
    void* root_signature;
    const void* packet;
    usize packet_size;
    const void* metadata;
    usize metadata_size;
    ConsoleSerializedShader stages[HW_STAGE_COUNT]; // compute: stages[0] is the compute shader
};

struct ShaderProgram;

// Fixed-function state of a graphics pipeline, decoded from its registers or set by deriving.
struct PipelineFixedState {
    D3D12_BLEND_DESC blend;
    D3D12_RASTERIZER_DESC rasterizer;
    D3D12_DEPTH_STENCIL_DESC depth_stencil;
    u32 sample_mask;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut;
    u8 element_count;
    VertexElement elements[SHADER_MAX_VERTEX_ELEMENTS];
    bool element_per_instance[SHADER_MAX_VERTEX_ELEMENTS];
    u32 element_step_rate[SHADER_MAX_VERTEX_ELEMENTS];
};

enum PipelineVariantState : u32 {
    VARIANT_BUILDING,
    VARIANT_READY,
    VARIANT_FAILED,
};

#define PIPELINE_MAX_VARIANTS 8

struct PipelineVariant {
    PipelineOutputs outputs;
    volatile u32 state; // PipelineVariantState
    ID3D12PipelineState* desktop;
    ID3D12RootSignature* signature; // the desktop signature it was made with (owned by the signature)
};

// A pipeline state: the console's packet for it (public; title code copies it into command lists)
// and its translation: shader programs, fixed-function state and desktop pipelines, made when a
// draw first uses a set of output formats once the programs are built.
struct ConsolePipelineState {
    ConsoleObjectHeader header;
    u32 packet[8];

    ConsoleRootSignature* root_signature; // referenced, or nullptr
    bool compute;
    bool translated;            // programs were requested (otherwise draws with it are skipped)
    ShaderProgram* programs[2]; // graphics: vertex, pixel (may be null); compute: compute
    PipelineFixedState fixed;

    SRWLOCK lock;
    u32 variant_count;
    PipelineVariant variants[PIPELINE_MAX_VARIANTS];
};

void console_pipeline_state_init();

HRESULT console_create_pipeline_state(ConsoleDevice* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                      REFIID iid, void** out);
HRESULT console_create_compute_pipeline_state(ConsoleDevice* device,
                                              const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid,
                                              void** out);
// Derived pipelines: a copy of `source` with some fixed-function state changed by `descs` (the
// console's derived state descriptions, `count` of them).
HRESULT console_derive_pipeline_state(ConsoleDevice* device, void* source, u32 count, const void* descs,
                                      REFIID iid, void** out);
HRESULT console_deserialize_pipeline_state(ConsoleDevice* device, bool compute,
                                           const ConsoleSerializedPipeline* serialized, REFIID iid,
                                           void** out);

// The desktop pipeline for drawing (or dispatching) with `pso` to `outputs`, or nullptr while it is
// being made or if it cannot be.
ID3D12PipelineState* console_pipeline_desktop(ConsolePipelineState* pso, const PipelineOutputs* outputs,
                                              ID3D12RootSignature** signature);

// Page pools (page_pool.cpp): title memory registered as a source of GPU page mapping copies.
HRESULT console_register_page_pool(u64 address, u32 page_count, u32 flags, HANDLE* handle);
HRESULT console_unregister_page_pool(HANDLE handle);
