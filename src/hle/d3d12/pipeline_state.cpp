// Console ID3D12PipelineState.
//
// Console pipeline states carry console GPU code, translated by gpu/shader_programs, and
// fixed-function state as hardware registers, decoded here into desktop descriptions. Desktop
// pipelines also depend on the formats drawn to, which console pipelines leave to the bound
// targets: they are made when a draw first uses a set of formats, on a worker thread once the
// programs are built. Draws are skipped until then.

#include <stddef.h>
#include <string.h>

#include "common/log.h"
#include "gpu/gpu.h"
#include "gpu/shader_programs.h"
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
static ID3D12Device* g_device = nullptr;

static void pipeline_state_destroy(void* object) {
    ConsolePipelineState* pso = (ConsolePipelineState*)object;
    for (u32 i = 0; i < pso->variant_count; i++)
        gpu_retire(pso->variants[i].desktop);
    if (pso->root_signature)
        console_release(pso->root_signature);
}

void console_pipeline_state_init() {
    g_pipeline_state_table = console_build_table("ID3D12PipelineState", k_pipeline_state_entries,
                                                 ARRAY_COUNT(k_pipeline_state_entries), nullptr, 0);
    g_device = gpu_device();
}

static ConsolePipelineState* new_pipeline_state(bool compute, void* root_signature, const void* packet,
                                                usize packet_size) {
    ConsolePipelineState* pso = (ConsolePipelineState*)console_alloc(
        sizeof(ConsolePipelineState), &k_pipeline_state_class, g_pipeline_state_table, nullptr);
    if (packet)
        memcpy(pso->packet, packet, packet_size < sizeof(pso->packet) ? packet_size : sizeof(pso->packet));
    pso->compute = compute;
    InitializeSRWLock(&pso->lock);
    if (root_signature && console_try_add_ref(root_signature))
        pso->root_signature = (ConsoleRootSignature*)root_signature;
    return pso;
}

// --- fixed-function state from registers -------------------------------------------------------------

// Hardware stencil operations by D3D12_STENCIL_OP (from 1), and blend combine functions by
// D3D12_BLEND_OP (from 1).
static const u8 k_stencil_ops[8] = {0, 1, 3, 5, 6, 7, 8, 9};
static const u8 k_blend_ops[5] = {0, 1, 4, 2, 3};

static D3D12_STENCIL_OP stencil_op(u32 hw) {
    for (u32 i = 0; i < ARRAY_COUNT(k_stencil_ops); i++)
        if (k_stencil_ops[i] == hw)
            return (D3D12_STENCIL_OP)(i + 1);
    return D3D12_STENCIL_OP_KEEP;
}

static D3D12_BLEND_OP blend_op(u32 hw) {
    for (u32 i = 0; i < ARRAY_COUNT(k_blend_ops); i++)
        if (k_blend_ops[i] == hw)
            return (D3D12_BLEND_OP)(i + 1);
    return D3D12_BLEND_OP_ADD;
}

// The logic operation by the ROP3 code (the console names copy as 0xCC).
static D3D12_LOGIC_OP logic_op(u32 rop3) {
    switch (rop3) {
    case 0x00:
        return D3D12_LOGIC_OP_CLEAR;
    case 0x11:
        return D3D12_LOGIC_OP_NOR;
    case 0x22:
        return D3D12_LOGIC_OP_AND_INVERTED;
    case 0x33:
        return D3D12_LOGIC_OP_COPY_INVERTED;
    case 0x44:
        return D3D12_LOGIC_OP_AND_REVERSE;
    case 0x55:
        return D3D12_LOGIC_OP_INVERT;
    case 0x66:
        return D3D12_LOGIC_OP_XOR;
    case 0x77:
        return D3D12_LOGIC_OP_NAND;
    case 0x88:
        return D3D12_LOGIC_OP_AND;
    case 0x99:
        return D3D12_LOGIC_OP_EQUIV;
    case 0xAA:
        return D3D12_LOGIC_OP_NOOP;
    case 0xBB:
        return D3D12_LOGIC_OP_OR_INVERTED;
    case 0xDD:
        return D3D12_LOGIC_OP_OR_REVERSE;
    case 0xEE:
        return D3D12_LOGIC_OP_OR;
    case 0xFF:
        return D3D12_LOGIC_OP_SET;
    default:
        return D3D12_LOGIC_OP_COPY;
    }
}

static u32 reg(const u8* context, u32 offset) {
    u32 v;
    memcpy(&v, context + offset, 4);
    return v;
}

static f32 reg_f32(const u8* context, u32 offset) {
    f32 v;
    memcpy(&v, context + offset, 4);
    return v;
}

// Context register block offsets.
#define CTX_COLOR_CONTROL   36 // CB_COLOR_CONTROL (ROP3 in bits 16..23)
#define CTX_CLIP_CONTROL    68 // PA_CL_CLIP_CNTL
#define CTX_MODE_CONTROL    72 // PA_SU_SC_MODE_CNTL
#define CTX_BIAS_CLAMP      84 // polygon offset clamp, scale (x16), offset
#define CTX_BIAS_SLOPE      88
#define CTX_BIAS_OFFSET     92
#define CTX_DEPTH_CONTROL   96  // DB_DEPTH_CONTROL
#define CTX_STENCIL_CONTROL 100 // DB_STENCIL_CONTROL
#define CTX_STENCIL_MASKS   104 // front ref, read mask, write mask, op value; then back
#define CTX_ALPHA_TO_MASK   112
#define CTX_TARGET_MASK     116 // CB_TARGET_MASK: 4 bits per target
#define CTX_BLEND_CONTROL   120 // CB_BLEND0_CONTROL..CB_BLEND7_CONTROL
#define CTX_SIZE            156

static void decode_fixed_state(const u8* ctx, const u8* metadata, PipelineFixedState* f) {
    D3D12_DEPTH_STENCIL_DESC& ds = f->depth_stencil;
    u32 depth = reg(ctx, CTX_DEPTH_CONTROL);
    u32 stencil = reg(ctx, CTX_STENCIL_CONTROL);
    ds.StencilEnable = depth & 1;
    ds.DepthEnable = (depth >> 1) & 1;
    ds.DepthWriteMask = (depth >> 2) & 1 ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = (D3D12_COMPARISON_FUNC)(((depth >> 4) & 7) + 1);
    ds.StencilReadMask = ctx[CTX_STENCIL_MASKS + 1];
    ds.StencilWriteMask = ctx[CTX_STENCIL_MASKS + 2];
    ds.FrontFace.StencilFailOp = stencil_op(stencil & 0xF);
    ds.FrontFace.StencilPassOp = stencil_op((stencil >> 4) & 0xF);
    ds.FrontFace.StencilDepthFailOp = stencil_op((stencil >> 8) & 0xF);
    ds.FrontFace.StencilFunc = (D3D12_COMPARISON_FUNC)(((depth >> 8) & 7) + 1);
    ds.BackFace.StencilFailOp = stencil_op((stencil >> 12) & 0xF);
    ds.BackFace.StencilPassOp = stencil_op((stencil >> 16) & 0xF);
    ds.BackFace.StencilDepthFailOp = stencil_op((stencil >> 20) & 0xF);
    ds.BackFace.StencilFunc = (D3D12_COMPARISON_FUNC)(((depth >> 20) & 7) + 1);

    D3D12_RASTERIZER_DESC& rs = f->rasterizer;
    u32 mode = reg(ctx, CTX_MODE_CONTROL);
    rs.FillMode = (D3D12_FILL_MODE)(((mode >> 8) & 7) + 1);
    if (rs.FillMode != D3D12_FILL_MODE_WIREFRAME && rs.FillMode != D3D12_FILL_MODE_SOLID)
        rs.FillMode = D3D12_FILL_MODE_SOLID;
    rs.CullMode = (mode & 3) == 0 ? D3D12_CULL_MODE_NONE
                  : (mode & 1)    ? D3D12_CULL_MODE_FRONT
                                  : D3D12_CULL_MODE_BACK;
    rs.FrontCounterClockwise = ((mode >> 2) & 1) == 0;
    rs.DepthBias = (INT)reg_f32(ctx, CTX_BIAS_OFFSET);
    rs.DepthBiasClamp = reg_f32(ctx, CTX_BIAS_CLAMP);
    rs.SlopeScaledDepthBias = reg_f32(ctx, CTX_BIAS_SLOPE) * 0.0625f;
    rs.DepthClipEnable = ((reg(ctx, CTX_CLIP_CONTROL) >> 26) & 1) == 0;
    u8 flags = metadata[44];
    rs.MultisampleEnable = (flags >> 1) & 1;
    rs.AntialiasedLineEnable = (flags >> 2) & 1;
    rs.ForcedSampleCount = metadata[42] & 0xF;
    rs.ConservativeRaster = (flags >> 3) & 1 ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON
                                             : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC& b = f->blend;
    b.AlphaToCoverageEnable = reg(ctx, CTX_ALPHA_TO_MASK) & 1;
    b.IndependentBlendEnable = TRUE;
    D3D12_LOGIC_OP logic = logic_op(ctx[CTX_COLOR_CONTROL + 2]);
    u32 target_mask = reg(ctx, CTX_TARGET_MASK);
    for (u32 i = 0; i < 8; i++) {
        D3D12_RENDER_TARGET_BLEND_DESC& t = b.RenderTarget[i];
        u32 c = reg(ctx, CTX_BLEND_CONTROL + 4 * i);
        t.BlendEnable = (c >> 30) & 1;
        t.SrcBlend = (D3D12_BLEND)((c & 0x1F) + 1);
        t.BlendOp = blend_op((c >> 5) & 7);
        t.DestBlend = (D3D12_BLEND)(((c >> 8) & 0x1F) + 1);
        t.SrcBlendAlpha = (D3D12_BLEND)(((c >> 16) & 0x1F) + 1);
        t.BlendOpAlpha = blend_op((c >> 21) & 7);
        t.DestBlendAlpha = (D3D12_BLEND)(((c >> 24) & 0x1F) + 1);
        // Without separate alpha factors the color ones apply to alpha as well.
        if (!((c >> 29) & 1)) {
            t.SrcBlendAlpha = t.SrcBlend;
            t.DestBlendAlpha = t.DestBlend;
            t.BlendOpAlpha = t.BlendOp;
        }
        t.LogicOpEnable = !((c >> 31) & 1) && logic != D3D12_LOGIC_OP_COPY;
        t.LogicOp = logic;
        t.RenderTargetWriteMask = (UINT8)((target_mask >> (4 * i)) & 0xF);
        if (t.LogicOpEnable)
            t.BlendEnable = FALSE;
    }
    // Dual-source blending is defined for the first target alone.
    D3D12_BLEND src = b.RenderTarget[0].SrcBlend, dst = b.RenderTarget[0].DestBlend;
    if ((src >= D3D12_BLEND_SRC1_COLOR && src <= D3D12_BLEND_INV_SRC1_ALPHA) ||
        (dst >= D3D12_BLEND_SRC1_COLOR && dst <= D3D12_BLEND_INV_SRC1_ALPHA))
        b.IndependentBlendEnable = FALSE;
}

// --- creation ----------------------------------------------------------------------------------------

static void request_graphics_programs(ConsolePipelineState* pso, const ConsoleSerializedPipeline* s,
                                      const ConsoleGraphicsShaders& g) {
    const ConsoleRootSignature* rs = pso->root_signature;
    const ConsoleSerializedShader& vs = s->stages[HW_STAGE_VS];
    const ConsoleSerializedShader& ps = s->stages[HW_STAGE_PS];
    if (!rs || !vs.code || !vs.code_size)
        return;
    ShaderInfo info;
    memset(&info, 0, sizeof(info));
    info.stage = SHADER_VERTEX;
    console_user_data_layout(rs->layout, CONSOLE_FIRST_ROOT_SGPR_GRAPHICS, &info);
    info.user_sgprs = g.vs_user_sgprs;
    info.element_count = g.element_count;
    memcpy(info.elements, g.elements, sizeof(info.elements));
    pso->programs[0] = shader_program_get(vs.code, (u32)vs.code_size, info);
    if (ps.code && ps.code_size) {
        memset(&info, 0, sizeof(info));
        info.stage = SHADER_PIXEL;
        console_user_data_layout(rs->layout, CONSOLE_FIRST_ROOT_SGPR_GRAPHICS, &info);
        info.user_sgprs = g.ps_user_sgprs;
        info.ps_input_addr = g.ps_input_ena;
        info.ps_color_format = g.ps_color_format;
        pso->programs[1] = shader_program_get(ps.code, (u32)ps.code_size, info);
    }
    pso->translated = pso->programs[0] != nullptr;
}

HRESULT console_deserialize_pipeline_state(ConsoleDevice*, bool compute, const ConsoleSerializedPipeline* s,
                                           REFIID iid, void** out) {
    if (!s || !s->packet)
        return E_INVALIDARG;
    if (!out)
        return E_POINTER;
    ConsolePipelineState* pso = new_pipeline_state(compute, s->root_signature, s->packet, s->packet_size);
    if (compute) {
        ConsoleComputeShader c;
        const ConsoleSerializedShader& cs = s->stages[0];
        if (pso->root_signature && cs.code && console_parse_compute(s->packet, s->packet_size, &c)) {
            ShaderInfo info;
            memset(&info, 0, sizeof(info));
            info.stage = SHADER_COMPUTE;
            console_user_data_layout(pso->root_signature->layout, CONSOLE_FIRST_ROOT_SGPR_COMPUTE, &info);
            info.user_sgprs = c.user_sgprs;
            memcpy(info.threads, c.threads, sizeof(info.threads));
            info.group_id_mask = c.group_id_mask;
            info.thread_id_count = c.thread_id_count;
            info.lds_bytes = c.lds_bytes;
            pso->programs[0] = shader_program_get(cs.code, (u32)cs.code_size, info);
            pso->translated = pso->programs[0] != nullptr;
        } else {
            LOG_W_ONCE("d3d12",
                       "compute pipeline in an unknown serialized form; dispatches with it are skipped");
        }
        return console_return(pso, iid, out);
    }
    ConsoleGraphicsShaders g;
    usize context = console_graphics_context_offset(s->packet, s->packet_size);
    if (!s->metadata ||
        !console_parse_graphics(s->packet, s->packet_size, s->metadata, s->metadata_size, &g) ||
        context + CTX_SIZE > s->packet_size) {
        LOG_W_ONCE("d3d12", "graphics pipeline in an unknown serialized form; draws with it are skipped");
        return console_return(pso, iid, out);
    }
    PipelineFixedState& f = pso->fixed;
    decode_fixed_state((const u8*)s->packet + context, (const u8*)s->metadata, &f);
    f.sample_mask = g.sample_mask;
    f.topology_type = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)g.primitive_topology_type;
    if (f.topology_type >= D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) { // no tessellation stages here
        LOG_W_ONCE("d3d12", "pipeline with console primitive topology type %u; drawn as triangles",
                   f.topology_type);
        f.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
    f.strip_cut = (D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)(((const u8*)s->metadata)[39] & 3);
    f.element_count = g.element_count;
    memcpy(f.elements, g.elements, sizeof(f.elements));
    memcpy(f.element_per_instance, g.element_per_instance, sizeof(f.element_per_instance));
    memcpy(f.element_step_rate, g.element_step_rate, sizeof(f.element_step_rate));
    request_graphics_programs(pso, s, g);
    return console_return(pso, iid, out);
}

HRESULT console_create_pipeline_state(ConsoleDevice*, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                      REFIID iid, void** out) {
    if (!desc || !desc->pPipelineStateSubobjectStream)
        return E_INVALIDARG;
    if (!out)
        return E_POINTER;
    LOG_W_ONCE("d3d12",
               "pipeline states from state streams are not translated yet; draws with them are skipped");
    return console_return(new_pipeline_state(false, nullptr, nullptr, 0), iid, out);
}

HRESULT console_create_compute_pipeline_state(ConsoleDevice*, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                              REFIID iid, void** out) {
    if (!desc)
        return E_INVALIDARG;
    if (!out)
        return E_POINTER;
    LOG_W_ONCE("d3d12",
               "compute pipeline states from shader containers are not translated yet; dispatches with "
               "them are skipped");
    return console_return(new_pipeline_state(true, desc->pRootSignature, nullptr, 0), iid, out);
}

// Console derived state descriptions: a type, then the description it replaces.
enum DerivedStateType : u32 {
    DERIVED_DEPTH_STENCIL,
    DERIVED_RASTERIZER,
    DERIVED_BLEND,
    DERIVED_SAMPLE_MASK,
    DERIVED_PIXEL_SHADER_OPTIONS,
    DERIVED_DEPTH_STENCIL_OPTIONS,
    DERIVED_SHADER_LIMITS,
    DERIVED_TESSELLATION_PARAMETERS,
};

struct DerivedState {
    DerivedStateType type;
    union {
        D3D12_DEPTH_STENCIL_DESC depth_stencil;
        D3D12_RASTERIZER_DESC rasterizer;
        D3D12_BLEND_DESC blend;
        UINT sample_mask;
    };
};

HRESULT console_derive_pipeline_state(ConsoleDevice*, void* source, u32 count, const void* descs, REFIID iid,
                                      void** out) {
    const ConsolePipelineState* src = (const ConsolePipelineState*)source;
    if (!src || (count && !descs))
        return E_INVALIDARG;
    if (!out)
        return E_POINTER;
    ConsolePipelineState* pso =
        new_pipeline_state(src->compute, src->root_signature, src->packet, sizeof(src->packet));
    pso->translated = src->translated;
    pso->programs[0] = src->programs[0];
    pso->programs[1] = src->programs[1];
    pso->fixed = src->fixed;
    if (!src->compute) {
        const DerivedState* d = (const DerivedState*)descs;
        for (u32 i = 0; i < count; i++) {
            switch (d[i].type) {
            case DERIVED_DEPTH_STENCIL:
                pso->fixed.depth_stencil = d[i].depth_stencil;
                break;
            case DERIVED_RASTERIZER:
                pso->fixed.rasterizer = d[i].rasterizer;
                break;
            case DERIVED_BLEND:
                pso->fixed.blend = d[i].blend;
                break;
            case DERIVED_SAMPLE_MASK:
                pso->fixed.sample_mask = d[i].sample_mask;
                break;
            default:
                break; // console options and limits: nothing on the desktop
            }
        }
    }
    return console_return(pso, iid, out);
}

// --- desktop pipelines -------------------------------------------------------------------------------

struct VariantJob {
    ConsolePipelineState* pso; // referenced for the job
    u32 index;
};

static bool programs_ready(const ConsolePipelineState* pso) {
    for (u32 i = 0; i < 2; i++)
        if (pso->programs[i] && pso->programs[i]->state != PROGRAM_READY)
            return false;
    return true;
}

static bool programs_failed(const ConsolePipelineState* pso) {
    for (u32 i = 0; i < 2; i++)
        if (pso->programs[i] && pso->programs[i]->state == PROGRAM_FAILED)
            return true;
    return false;
}

static ID3D12PipelineState* create_graphics(const ConsolePipelineState* pso, const PipelineOutputs& o) {
    const PipelineFixedState& f = pso->fixed;
    const ShaderProgram* vs = pso->programs[0];
    const ShaderProgram* ps = pso->programs[1];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = pso->root_signature->desktop;
    d.VS = {vs->dxil, vs->dxil_size};
    if (ps)
        d.PS = {ps->dxil, ps->dxil_size};
    d.BlendState = f.blend;
    d.SampleMask = f.sample_mask;
    d.RasterizerState = f.rasterizer;
    d.DepthStencilState = f.depth_stencil;
    if (o.depth_format == DXGI_FORMAT_UNKNOWN) {
        d.DepthStencilState.DepthEnable = FALSE;
        d.DepthStencilState.StencilEnable = FALSE;
    }
    // Input elements the vertex shader reads, by element index; raw fetches read dwords.
    D3D12_INPUT_ELEMENT_DESC elements[SHADER_MAX_VERTEX_ELEMENTS];
    u32 element_count = 0;
    for (u32 i = 0; i < f.element_count; i++) {
        if (!(vs->vertex_element_mask & (1u << i)))
            continue;
        D3D12_INPUT_ELEMENT_DESC& e = elements[element_count++];
        e.SemanticName = "E";
        e.SemanticIndex = i;
        e.Format = (DXGI_FORMAT)vs->element_format[i];
        e.InputSlot = f.elements[i].slot;
        e.AlignedByteOffset = f.elements[i].offset;
        e.InputSlotClass = f.element_per_instance[i] ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                     : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        e.InstanceDataStepRate = f.element_per_instance[i] ? f.element_step_rate[i] : 0;
    }
    d.InputLayout = {elements, element_count};
    d.IBStripCutValue = f.strip_cut;
    d.PrimitiveTopologyType = f.topology_type ? f.topology_type : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = o.render_target_count;
    for (u32 i = 0; i < 8; i++)
        d.RTVFormats[i] = (DXGI_FORMAT)o.render_target_formats[i];
    d.DSVFormat = (DXGI_FORMAT)o.depth_format;
    d.SampleDesc.Count = o.sample_count ? o.sample_count : 1;
    ID3D12PipelineState* desktop = nullptr;
    HRESULT hr = g_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&desktop));
    if (FAILED(hr))
        LOG_W("d3d12", "creating a translated graphics pipeline failed (0x%08lx): vs %016llx ps %016llx",
              (unsigned long)hr, (unsigned long long)vs->key, ps ? (unsigned long long)ps->key : 0ull);
    return desktop;
}

static ID3D12PipelineState* create_compute(const ConsolePipelineState* pso) {
    const ShaderProgram* cs = pso->programs[0];
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = pso->root_signature->desktop;
    d.CS = {cs->dxil, cs->dxil_size};
    ID3D12PipelineState* desktop = nullptr;
    HRESULT hr = g_device->CreateComputePipelineState(&d, IID_PPV_ARGS(&desktop));
    if (FAILED(hr))
        LOG_W("d3d12", "creating a translated compute pipeline failed (0x%08lx): cs %016llx",
              (unsigned long)hr, (unsigned long long)cs->key);
    return desktop;
}

static void build_variant(void* context) {
    VariantJob* job = (VariantJob*)context;
    ConsolePipelineState* pso = job->pso;
    PipelineVariant& v = pso->variants[job->index];
    ID3D12PipelineState* desktop = pso->compute ? create_compute(pso) : create_graphics(pso, v.outputs);
    v.desktop = desktop;
    InterlockedExchange((volatile LONG*)&v.state, desktop ? VARIANT_READY : VARIANT_FAILED);
    console_release(pso);
    free(job);
}

ID3D12PipelineState* console_pipeline_desktop(ConsolePipelineState* pso, const PipelineOutputs* outputs) {
    if (!pso || !pso->translated || !pso->root_signature || !pso->root_signature->desktop)
        return nullptr;
    if (!programs_ready(pso))
        return nullptr;
    PipelineOutputs key = {};
    if (!pso->compute && outputs)
        key = *outputs;
    AcquireSRWLockShared(&pso->lock);
    for (u32 i = 0; i < pso->variant_count; i++) {
        PipelineVariant& v = pso->variants[i];
        if (memcmp(&v.outputs, &key, sizeof(key)) == 0) {
            ID3D12PipelineState* desktop = v.state == VARIANT_READY ? v.desktop : nullptr;
            ReleaseSRWLockShared(&pso->lock);
            return desktop;
        }
    }
    ReleaseSRWLockShared(&pso->lock);
    if (programs_failed(pso))
        return nullptr;
    // A new set of outputs: made on a worker.
    AcquireSRWLockExclusive(&pso->lock);
    for (u32 i = 0; i < pso->variant_count; i++)
        if (memcmp(&pso->variants[i].outputs, &key, sizeof(key)) == 0) {
            ReleaseSRWLockExclusive(&pso->lock);
            return nullptr; // another thread got there first
        }
    if (pso->variant_count == PIPELINE_MAX_VARIANTS) {
        ReleaseSRWLockExclusive(&pso->lock);
        LOG_W_ONCE("d3d12", "a pipeline is drawn with more than %d sets of formats; draws skipped",
                   PIPELINE_MAX_VARIANTS);
        return nullptr;
    }
    u32 index = pso->variant_count++;
    PipelineVariant& v = pso->variants[index];
    v.outputs = key;
    v.state = VARIANT_BUILDING;
    v.desktop = nullptr;
    ReleaseSRWLockExclusive(&pso->lock);
    VariantJob* job = (VariantJob*)malloc(sizeof(VariantJob));
    if (!job || !console_try_add_ref(pso)) {
        free(job);
        InterlockedExchange((volatile LONG*)&v.state, VARIANT_FAILED);
        return nullptr;
    }
    job->pso = pso;
    job->index = index;
    gpu_job_queue(build_variant, job);
    return nullptr;
}
