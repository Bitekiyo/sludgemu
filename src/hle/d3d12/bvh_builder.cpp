// The console's acceleration structure builder (XboxBvhBuilder.dll). Titles ship it, but it writes
// the console GPU's own BVH format, which desktop GPUs can't trace, so the class is replaced: sizes
// come from the desktop driver's DXR.
//
// Builds, copies and rehydration are not recorded yet: desktop acceleration structures have to live
// in their own GPU memory rather than at the title's addresses, and every address that names one
// (instance descriptions, views, root arguments) needs translating. Nothing traces rays before
// shaders are translated. Postbuild information is still written, as the sizes the title will act
// on (it allocates compacted copies from them).

#include "common/log.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/hle.h"

// Build flags desktop DXR knows; the console adds one for dehydrated (serialized) structures.
#define DESKTOP_BUILD_FLAGS          0x3Fu
#define CONSOLE_BUILD_FLAG_DEHYDRATE 0x40000000u

// The title allocates the object (24 bytes) and runs our constructor on it; only we read it.
struct BvhBuilder {
    ID3D12Device13* device; // nullptr if the GPU has no DXR
    bool morton_64;         // a build quality hint for the console builder
};
static_assert(sizeof(BvhBuilder) <= 24, "XboxBvhBuilder objects are 24 bytes");

static BvhBuilder* bvh_construct(BvhBuilder* self, ConsoleDevice* device, bool) {
    self->device = nullptr;
    self->morton_64 = false;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    if (device &&
        SUCCEEDED(device->desktop->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))) &&
        o5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
        self->device = device->desktop;
    else
        LOG_W("d3d12", "the GPU has no DXR; acceleration structures get placeholder sizes");
    return self;
}

static void bvh_destruct(BvhBuilder*) {}

static void bvh_set_64bit_morton_codes(BvhBuilder* self, bool enable) {
    self->morton_64 = enable;
}

// Sizes of the structure `inputs` describe, as the desktop driver builds it.
static void prebuild_info(const BvhBuilder* self,
                          const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* inputs,
                          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* info) {
    if (inputs->Flags & CONSOLE_BUILD_FLAG_DEHYDRATE)
        LOG_W_ONCE("d3d12", "dehydrated acceleration structures are sized as ordinary ones");
    if (!self->device) {
        // Something the title can allocate; nothing will be built in it.
        *info = {D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT,
                 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT,
                 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT};
        return;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS d = *inputs;
    d.Flags = (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS)(d.Flags & DESKTOP_BUILD_FLAGS);
    self->device->GetRaytracingAccelerationStructurePrebuildInfo(&d, info);
}

static void bvh_get_prebuild_info(BvhBuilder* self,
                                  const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* inputs,
                                  const void* extra_options,
                                  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* info) {
    (void)extra_options; // console builder tuning
    if (!inputs || !info)
        return;
    prebuild_info(self, inputs, info);
}

// Writes a 64-bit value to title GPU memory from the list, in order with its other work.
static void write_u64(ConsoleCommandList* list, u64 address, u64 value) {
    u64 desktop;
    if (!console_translate_gpu_address(address, sizeof(u64), &desktop)) {
        LOG_W_ONCE("d3d12", "acceleration structure postbuild info at 0x%llx is not GPU-visible", address);
        return;
    }
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[2] = {{desktop, (u32)value},
                                                      {desktop + 4, (u32)(value >> 32)}};
    list->desktop->WriteBufferImmediate(2, params, nullptr);
}

// The postbuild information a build would report. Without a build, the structure's size is its
// largest: compaction can't shrink it, and serializing it takes its whole size.
static void write_postbuild_info(ConsoleCommandList* list,
                                 const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC& d,
                                 u64 size) {
    switch (d.InfoType) {
    case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE:
    case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE:
    case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION:
        write_u64(list, d.DestBuffer, size);
        break;
    case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION:
        write_u64(list, d.DestBuffer, size);
        write_u64(list, d.DestBuffer + 8, 0); // bottom-level structure pointers
        break;
    default:
        LOG_W_ONCE("d3d12", "acceleration structure postbuild info type %d not supported", d.InfoType);
        break;
    }
}

struct ConsoleBatchedBuildDesc {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc;
    u32 postbuild_count;
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* postbuild;
    u64 extra_options_size;
    const void* extra_options;
};
static_assert(sizeof(ConsoleBatchedBuildDesc) == 80, "console batched build description layout");

static void bvh_build_batched2(BvhBuilder* self, ConsoleCommandList* list, u32 count,
                               const ConsoleBatchedBuildDesc* descs) {
    LOG_W_ONCE("d3d12",
               "acceleration structure builds are not recorded yet; only their postbuild info is written");
    if (!list || !list->recording || !descs)
        return;
    for (u32 i = 0; i < count; i++) {
        const ConsoleBatchedBuildDesc& b = descs[i];
        if (!b.postbuild_count || !b.postbuild)
            continue;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info;
        prebuild_info(self, &b.desc.Inputs, &info);
        for (u32 p = 0; p < b.postbuild_count; p++)
            write_postbuild_info(list, b.postbuild[p], info.ResultDataMaxSizeInBytes);
    }
}

static void bvh_copy(BvhBuilder*, ConsoleCommandList*, u64, u64,
                     D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) {
    LOG_W_ONCE("d3d12", "acceleration structure copies (mode %d) are not recorded yet", mode);
}

static HRESULT bvh_rehydrate_batched2(BvhBuilder*, ConsoleCommandList*, u32,
                                      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*, const u64*) {
    LOG_W_ONCE("d3d12", "acceleration structure rehydration is not recorded yet");
    return S_OK;
}

static const HleExport k_exports[] = {
    HLE_EXPORT_AS(bvh_construct, "??0XboxBvhBuilder@@QEAA@PEAUID3D12Device11@@_N@Z"),
    HLE_EXPORT_AS(bvh_destruct, "??1XboxBvhBuilder@@QEAA@XZ"),
    HLE_EXPORT_AS(bvh_set_64bit_morton_codes, "?Set64BitMortonCodes@XboxBvhBuilder@@QEAAX_N@Z"),
    HLE_EXPORT_AS(bvh_get_prebuild_info,
                  "?GetRaytracingAccelerationStructurePrebuildInfo@XboxBvhBuilder@@QEAAXPEBUD3D12_BUILD_"
                  "RAYTRACING_ACCELERATION_STRUCTURE_INPUTS@@PEBUD3D12XBOX_XDXR_STREAM_DESC@@PEAUD3D12_"
                  "RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO@@@Z"),
    HLE_EXPORT_AS(
        bvh_build_batched2,
        "?BuildRaytracingAccelerationStructureBatched2X@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList6@@"
        "IPEBUD3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2@@@Z"),
    HLE_EXPORT_AS(
        bvh_copy,
        "?CopyRaytracingAccelerationStructure@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList4@@_"
        "K1W4D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE@@@Z"),
    HLE_EXPORT_AS(bvh_rehydrate_batched2, "?RehydrateRaytracingAccelerationStructureBatched2X@XboxBvhBuilder@"
                                          "@QEAAJPEAUID3D12GraphicsCommandList8@@"
                                          "IPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@PEB_K@Z"),
};

extern const HleModule g_hle_module_xboxbvhbuilder = {"xboxbvhbuilder.dll", k_exports,
                                                      ARRAY_COUNT(k_exports)};
