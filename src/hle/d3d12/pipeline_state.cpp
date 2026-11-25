// Console ID3D12PipelineState.
//
// Console pipeline states carry console GPU shader code, which needs translating before desktop
// D3D12 can run it. Until shader translation exists, pipeline states are created without a desktop
// pipeline: titles get working objects, and command lists skip the draws and dispatches that use
// them (logged once).

#include <stddef.h>
#include <string.h>

#include "common/log.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"

static_assert(offsetof(ConsolePipelineState, packet) == 0x10, "pipeline state layout");
static_assert(offsetof(ConsolePipelineState, root_signature) == 0x30,
              "pipeline state state follows the public members");

static const GUID* const k_pipeline_state_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                                    &__uuidof(ID3D12Pageable),
                                                    &__uuidof(ID3D12PipelineState)};

static void pipeline_state_destroy(void* object);

static const ConsoleClass k_pipeline_state_class = {
    "ID3D12PipelineState", k_pipeline_state_iids, ARRAY_COUNT(k_pipeline_state_iids), pipeline_state_destroy};

static void* const* g_pipeline_state_table = nullptr;

static void pipeline_state_destroy(void* object) {
    ConsolePipelineState* pso = (ConsolePipelineState*)object;
    if (pso->root_signature)
        console_release(pso->root_signature);
}

void console_pipeline_state_init() {
    g_pipeline_state_table = console_build_table("ID3D12PipelineState", k_pipeline_state_entries,
                                                 ARRAY_COUNT(k_pipeline_state_entries), nullptr, 0);
}

// The console's own packet for the pipeline, which title code copies into command lists; serialized
// pipelines bring it along.
static HRESULT new_pipeline_state(bool compute, void* root_signature, const void* packet, usize packet_size,
                                  REFIID iid, void** out) {
    if (!out)
        return E_POINTER;
    LOG_W_ONCE("d3d12", "pipeline states are created without desktop pipelines until shaders are "
                        "translated; draws and dispatches are skipped");
    ConsolePipelineState* pso = (ConsolePipelineState*)console_alloc(
        sizeof(ConsolePipelineState), &k_pipeline_state_class, g_pipeline_state_table, nullptr);
    if (packet)
        memcpy(pso->packet, packet, packet_size < sizeof(pso->packet) ? packet_size : sizeof(pso->packet));
    pso->compute = compute;
    if (root_signature && console_try_add_ref(root_signature))
        pso->root_signature = (ConsoleRootSignature*)root_signature;
    return console_return(pso, iid, out);
}

HRESULT console_create_pipeline_state(ConsoleDevice*, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                      REFIID iid, void** out) {
    if (!desc || !desc->pPipelineStateSubobjectStream)
        return E_INVALIDARG;
    return new_pipeline_state(false, nullptr, nullptr, 0, iid, out);
}

HRESULT console_create_compute_pipeline_state(ConsoleDevice*, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                              REFIID iid, void** out) {
    if (!desc)
        return E_INVALIDARG;
    return new_pipeline_state(true, desc->pRootSignature, nullptr, 0, iid, out);
}

// A copy of `source` with some fixed-function state changed (the console's derived pipelines). The
// changes only matter to the desktop pipeline.
HRESULT console_derive_pipeline_state(ConsoleDevice*, void* source, REFIID iid, void** out) {
    const ConsolePipelineState* src = (const ConsolePipelineState*)source;
    if (!src)
        return E_INVALIDARG;
    return new_pipeline_state(src->compute, src->root_signature, src->packet, sizeof(src->packet), iid, out);
}

HRESULT console_deserialize_pipeline_state(ConsoleDevice*, bool compute, void* root_signature,
                                           const void* packet, usize packet_size, REFIID iid, void** out) {
    if (!packet)
        return E_INVALIDARG;
    return new_pipeline_state(compute, root_signature, packet, packet_size, iid, out);
}
