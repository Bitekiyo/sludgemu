#include "gpu/texture_untile.h"

#include <string.h>

#include "common/log.h"
#include "shaders/untile_cs.h"

// The shader's parameters (Params in shaders/untile.hlsl), as root constants.
struct UntileParams {
    u32 base;
    u32 row_pitch;
    u32 slice_pitch;
    u32 width, height, depth;
    u32 element_shift;
    u32 linear;
    u32 pitch;
    u32 src_slice_pitch;
    u32 shifts;
    u32 row_blocks;
    u32 slice_stride;
    u32 block_xor;
    u32 tail;
    u32 units;
    u16 bits[40]; // x bits 0-13, y bits 14-27, z bits 28-39
};
static_assert(sizeof(UntileParams) == 36 * 4, "untile shader parameters");

#define ROOT_PARAMS 0
#define ROOT_SOURCE 1
#define ROOT_DESTINATION 2

static ID3D12RootSignature* g_signature;
static ID3D12PipelineState* g_pipeline;
static volatile long g_state; // 0: not made yet, 1: ready, 2: failed
static SRWLOCK g_lock = SRWLOCK_INIT;

static bool ready() {
    if (g_state)
        return g_state == 1;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_state) {
        ID3D12Device13* device = gpu_device();
        HRESULT hr = device->CreateRootSignature(0, k_untile_cs, sizeof(k_untile_cs), IID_PPV_ARGS(&g_signature));
        if (SUCCEEDED(hr)) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
            d.pRootSignature = g_signature;
            d.CS = {k_untile_cs, sizeof(k_untile_cs)};
            hr = device->CreateComputePipelineState(&d, IID_PPV_ARGS(&g_pipeline));
        }
        if (FAILED(hr) && !gpu_device_lost(hr))
            LOG_E("gpu", "cannot make the texture detiling pipeline (0x%08lx); textures get no contents",
                  (unsigned long)hr);
        InterlockedExchange(&g_state, SUCCEEDED(hr) ? 1 : 2);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return g_state == 1;
}

static u32 log2_of(u32 v) {
    u32 n = 0;
    while ((1u << n) < v)
        n++;
    return n;
}

bool gpu_untile(ID3D12GraphicsCommandList* list, u64 source, const ConsoleSubresourceAddressing& a, u64 destination,
                u32 row_pitch, u32 slice_pitch) {
    if (!ready() || a.base > 0xFFFFFFFFull || a.slice_stride > 0xFFFFFFFFull)
        return false;
    UntileParams p = {};
    p.base = (u32)a.base;
    p.row_pitch = row_pitch;
    p.slice_pitch = slice_pitch;
    p.width = a.width;
    p.height = a.height;
    p.depth = a.depth;
    p.element_shift = log2_of(a.bytes_per_element);
    p.linear = a.linear;
    p.pitch = a.pitch;
    p.src_slice_pitch = a.slice_pitch;
    p.shifts = a.block_shift[0] | a.block_shift[1] << 5 | a.block_shift[2] << 10 | a.block_bytes_shift << 15;
    p.row_blocks = a.row_blocks;
    p.slice_stride = (u32)a.slice_stride;
    p.block_xor = a.block_xor;
    p.tail = a.tail[0] | a.tail[1] << 10 | a.tail[2] << 20;
    p.units = p.element_shift >= 2 ? a.width : (a.width * a.bytes_per_element + 3) / 4;
    memcpy(p.bits, a.bits_x, sizeof(a.bits_x));
    memcpy(p.bits + 14, a.bits_y, sizeof(a.bits_y));
    memcpy(p.bits + 28, a.bits_z, sizeof(a.bits_z));
    list->SetComputeRootSignature(g_signature);
    list->SetPipelineState(g_pipeline);
    list->SetComputeRoot32BitConstants(ROOT_PARAMS, sizeof(p) / 4, &p, 0);
    list->SetComputeRootShaderResourceView(ROOT_SOURCE, source);
    list->SetComputeRootUnorderedAccessView(ROOT_DESTINATION, destination);
    list->Dispatch((p.units + 63) / 64, a.height, a.depth);
    return true;
}
