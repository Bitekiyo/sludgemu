// d3d12_xs.dll: console D3D12 runtime entry points.

#include <windows.h>

#include "common/log.h"
#include "core/memory.h"
#include "hle/d3d12/device.h"
#include "hle/hle.h"

// Console graphics virtual-memory configuration (in/out). Layout matches the title's ABI.
struct GpuVmConfig {
    u32 flags;                // in
    u32 page_table_4mb_pages; // in (console page table sizing, ignored)
    u32 frame_preserve_bytes; // in (cross-process frame buffer preservation, ignored)
    u32 pad;
    u64 min_graphics_address; // out
    u64 max_graphics_address; // out
};
static_assert(sizeof(GpuVmConfig) == 32, "GpuVmConfig layout");

#define GPU_VM_FLAG_LEGACY_LOWER_BOUND 0x1 // graphics addresses start at 1 GiB instead of 2 GiB

static HRESULT WINAPI D3D12ConfigureVirtualMemoryX(GpuVmConfig* cfg) {
    if (!cfg)
        return E_INVALIDARG;
    u64 lowest = (cfg->flags & GPU_VM_FLAG_LEGACY_LOWER_BOUND) ? GiB(1) : GiB(2);
    u64 highest = (1ull << 40) - 1; // 40-bit GPU virtual address space
    mem_set_gpu_window(lowest, highest);
    cfg->min_graphics_address = lowest;
    cfg->max_graphics_address = highest;
    LOG_I("d3d12", "ConfigureVirtualMemoryX(flags=0x%x, pt=%u, preserve=%u) -> graphics VA 0x%llx..0x%llx",
          cfg->flags, cfg->page_table_4mb_pages, cfg->frame_preserve_bytes, lowest, highest);
    return S_OK;
}

static HRESULT WINAPI D3D12XboxCreateDevice(void* adapter, const ConsoleDeviceParams* params, REFIID iid,
                                            void** device) {
    return console_create_device(adapter, params, iid, device);
}

static const HleExport k_exports[] = {
    HLE_EXPORT(D3D12ConfigureVirtualMemoryX),
    HLE_EXPORT(D3D12XboxCreateDevice),
};

extern const HleModule g_hle_module_d3d12_xs = {"d3d12_xs.dll", k_exports, ARRAY_COUNT(k_exports)};
