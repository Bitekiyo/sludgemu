// Console shader translation: memory instructions (scalar and buffer loads, vertex fetches, images,
// LDS, global memory).

#include <stdio.h>
#include <string.h>

#include "shader/translate_internal.h"

static Tag sgpr_tag(const Translator& t, u32 reg) {
    return t.tags.sgpr[reg & 127];
}

static const char* soffset(Translator& t, const RdnaInst& in, u32 i) {
    const RdnaOperand& o = in.src[i];
    if (o.kind == OPND_SPECIAL && o.reg == SRC_NULL)
        return "0u";
    return src_u32(t, in, i);
}

static const char* const k_load_fn[5] = {"", "Load", "Load2", "Load3", "Load4"};
static const char* const k_store_fn[5] = {"", "Store", "Store2", "Store3", "Store4"};

// The kind of descriptor at a known place in a table, from the table's ranges.
static bool range_kind(const Translator& t, u32 parameter, u32 descriptor, TableRangeKind* kind) {
    if (parameter >= t.info->parameter_count)
        return false;
    const UserDataParameter& p = t.info->parameters[parameter];
    if (p.kind != USER_DATA_RESOURCE_TABLE)
        return false;
    u32 kinds = 0;
    for (u32 i = 0; i < p.range_count; i++) {
        const TableRange& r = p.ranges[i];
        kinds |= 1u << r.kind;
        if (descriptor != NO_DESCRIPTOR && descriptor >= r.offset &&
            (r.count == ~0u || descriptor < r.offset + r.count)) {
            *kind = r.kind;
            return true;
        }
    }
    // Unknown place: decided if the whole table holds one kind.
    for (u32 k = 0; k < 3; k++)
        if (kinds == (1u << k)) {
            *kind = (TableRangeKind)k;
            return true;
        }
    return false;
}

// The descriptor kind for a heap register, or `fallback` when the root signature does not tell.
static TableRangeKind heap_kind(const Translator& t, u32 reg, TableRangeKind fallback) {
    Tag tag = sgpr_tag(t, reg);
    TableRangeKind kind = fallback;
    range_kind(t, tag_payload(tag) >> 1, t.tags.descriptor[reg & 127], &kind);
    return kind;
}

static void write_loaded(Translator& t, const RdnaInst& in, u32 dwords, const char* value) {
    static const char k_comp[4] = {'x', 'y', 'z', 'w'};
    for (u32 i = 0; i < dwords; i++)
        write_vgpr(t, in, in.dst[0].reg + i, dwords == 1 ? value : fmt(t, "%s.%c", value, k_comp[i]));
}

// --- scalar memory -----------------------------------------------------------------------------------

static bool emit_scalar_memory(Translator& t, const RdnaInst& in) {
    if (in.op == OP_S_DCACHE_INV || in.op == OP_S_GL1_INV)
        return true;
    if (in.op == OP_S_MEMTIME || in.op == OP_S_MEMREALTIME) {
        write_sgpr(t, in.dst[0], 0, "0u");
        write_sgpr(t, in.dst[0], 1, "0u");
        return true;
    }
    u32 base = in.src[0].reg;
    u32 dwords = in.dst[0].count;
    const char* offset = fmt(t, "(%s + 0x%xu + %s)", sgpr(t, base), (u32)in.mem.offset, soffset(t, in, 1));
    bool buffer = in.op >= OP_S_BUFFER_LOAD_DWORD && in.op <= OP_S_BUFFER_LOAD_DWORDX16;
    Tag lo = sgpr_tag(t, base), hi = sgpr_tag(t, base + 1);

    if (!buffer && tag_kind(lo) == TAG_HEAP) {
        // How far into its table the program reads: console tables are plain memory, and programs may
        // read past the ranges their root signature declares.
        u32 parameter = tag_payload(lo) >> 1;
        if (parameter < SHADER_MAX_ROOT_PARAMETERS) {
            u32 slot = (tag_payload(lo) & 1) == HEAP_SAMPLERS ? 16 : 32;
            u32 at = t.tags.descriptor[base & 127];
            bool constant = in.src[1].kind == OPND_SPECIAL && in.src[1].reg == SRC_NULL;
            u16& reach = t.out->table_reach[parameter];
            if (at != NO_DESCRIPTOR && constant && in.mem.offset >= 0) {
                u32 end = (at * slot + (u32)in.mem.offset + 4 * dwords + slot - 1) / slot;
                if (reach != TABLE_REACH_UNKNOWN && end > reach)
                    reach = (u16)(end < TABLE_REACH_UNKNOWN ? end : TABLE_REACH_UNKNOWN);
            } else {
                reach = TABLE_REACH_UNKNOWN;
            }
        }
        // Descriptors: each destination dword holds its own heap byte address.
        emit(t, "{ uint a_ = %s;", offset);
        t.depth++;
        for (u32 i = 0; i < dwords; i++)
            write_sgpr(t, in.dst[0], i, fmt(t, "a_ + %uu", 4 * i));
        t.depth--;
        emit(t, "}");
        return true;
    }
    if (!buffer && tag_kind(lo) == TAG_VB_TABLE) {
        for (u32 i = 0; i < dwords; i++)
            write_sgpr(t, in.dst[0], i, "0u"); // vertex buffer descriptors: tracked statically
        return true;
    }
    if (tag_kind(hi) == TAG_ROOT_HI) {
        u32 parameter = tag_payload(hi);
        t.root_used |= 1ull << parameter;
        emit(t, "{ uint a_ = %s;", offset);
        t.depth++;
        for (u32 i = 0; i < dwords; i += 4) {
            u32 n = dwords - i < 4 ? dwords - i : 4;
            emit(t, "uint%s l%u_ = g_root%u.%s(a_ + %uu);", n == 1 ? "" : fmt(t, "%u", n), i, parameter,
                 k_load_fn[n], 4 * i);
            for (u32 k = 0; k < n; k++)
                write_sgpr(t, in.dst[0], i + k, n == 1 ? fmt(t, "l%u_", i) : fmt(t, "l%u_.%c", i, "xyzw"[k]));
        }
        t.depth--;
        emit(t, "}");
        return true;
    }
    if (buffer && tag_kind(lo) == TAG_HEAP && (tag_payload(lo) & 1) == HEAP_RESOURCES) {
        TableRangeKind kind = heap_kind(t, base, RANGE_CBV);
        emit(t, "{ uint a_ = %s;", fmt(t, "(0x%xu + %s)", (u32)in.mem.offset, soffset(t, in, 1)));
        t.depth++;
        // Constant buffers too are raw buffer views on desktop.
        emit(t, "%s b_ = ResourceDescriptorHeap[%s >> 5];",
             kind == RANGE_UAV ? "RWByteAddressBuffer" : "ByteAddressBuffer", sgpr(t, base));
        for (u32 i = 0; i < dwords; i++)
            write_sgpr(t, in.dst[0], i, fmt(t, "b_.Load(a_ + %uu)", 4 * i));
        t.depth--;
        emit(t, "}");
        return true;
    }
    translator_fail(t, "%s at 0x%x from an address the translator cannot place", rdna_op_name(in.op),
                    in.offset);
    return false;
}

// --- buffers -----------------------------------------------------------------------------------------

static u32 buffer_dwords(const RdnaInst& in) {
    return in.dst[0].count - (in.mem.tfe ? 1 : 0);
}

static bool is_store(RdnaOp op) {
    return (op >= OP_BUFFER_STORE_FORMAT_X && op <= OP_BUFFER_STORE_FORMAT_XYZW) ||
           (op >= OP_BUFFER_STORE_BYTE && op <= OP_BUFFER_STORE_DWORDX3) ||
           (op >= OP_TBUFFER_STORE_FORMAT_X && op <= OP_TBUFFER_STORE_FORMAT_XYZW);
}

static bool is_format_access(RdnaOp op) {
    return op <= OP_BUFFER_STORE_FORMAT_XYZW ||
           (op >= OP_TBUFFER_LOAD_FORMAT_X && op <= OP_TBUFFER_STORE_FORMAT_XYZW);
}

// Formatted accesses with two 16-bit components per register.
static bool is_d16_format(RdnaOp op) {
    return (op >= OP_BUFFER_LOAD_FORMAT_D16_X && op <= OP_BUFFER_STORE_FORMAT_D16_XYZW) ||
           (op >= OP_TBUFFER_LOAD_FORMAT_D16_X && op <= OP_TBUFFER_STORE_FORMAT_D16_XYZW);
}

// Vertex element formats: components and bytes per component (0 for packed formats), and which
// input assembler component each component in memory becomes.
struct FormatLayout {
    u8 components;
    u8 component_bytes;
    u8 order[4];
    u8 value_class; // 0 float (also unorm/snorm), 1 uint, 2 sint
};

static bool vertex_format_layout(u32 format, FormatLayout* out) {
    static const u8 k_rgba[4] = {0, 1, 2, 3};
    u8 components = 0, bytes = 0;
    switch (format) {
    case 2:
    case 3:
    case 4:
        components = 4;
        bytes = 4;
        break; // R32G32B32A32
    case 6:
    case 7:
    case 8:
        components = 3;
        bytes = 4;
        break; // R32G32B32
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
        components = 4;
        bytes = 2;
        break; // R16G16B16A16
    case 16:
    case 17:
    case 18:
        components = 2;
        bytes = 4;
        break; // R32G32
    case 24:
    case 25:
    case 26:
        components = 1;
        bytes = 4;
        break; // packed 32-bit: one fetch
    case 28:
    case 29:
    case 30:
    case 31:
    case 32:
    case 87:
        components = 4;
        bytes = 1;
        break; // RGBA8, BGRA8
    case 34:
    case 35:
    case 36:
    case 37:
    case 38:
        components = 2;
        bytes = 2;
        break; // R16G16
    case 41:
    case 42:
    case 43:
        components = 1;
        bytes = 4;
        break; // R32
    case 49:
    case 50:
    case 51:
    case 52:
        components = 2;
        bytes = 1;
        break; // R8G8
    case 54:
    case 56:
    case 57:
    case 58:
    case 59:
        components = 1;
        bytes = 2;
        break; // R16
    case 61:
    case 62:
    case 63:
    case 64:
        components = 1;
        bytes = 1;
        break; // R8
    default:
        return false;
    }
    out->components = components;
    out->component_bytes = bytes;
    memcpy(out->order, k_rgba, 4);
    switch (format) {
    case 3:
    case 7:
    case 12:
    case 17:
    case 25:
    case 30:
    case 36:
    case 42:
    case 50:
    case 57:
    case 62:
        out->value_class = 1;
        break;
    case 4:
    case 8:
    case 14:
    case 18:
    case 32:
    case 38:
    case 43:
    case 52:
    case 59:
    case 64:
        out->value_class = 2;
        break;
    default:
        out->value_class = 0;
        break;
    }
    if (format == 87) { // B8G8R8A8: blue first in memory
        out->order[0] = 2;
        out->order[2] = 0;
    }
    return true;
}

static bool emit_vertex_fetch(Translator& t, const RdnaInst& in, u32 slot) {
    if (t.info->stage != SHADER_VERTEX || is_store(in.op) || in.mem.offen || is_d16_format(in.op)) {
        translator_fail(t, "unexpected access through a vertex buffer descriptor at 0x%x", in.offset);
        return false;
    }
    const RdnaOperand& so = in.src[2];
    if (!(so.kind == OPND_INLINE && so.reg == SRC_ZERO) && !(so.kind == OPND_SPECIAL && so.reg == SRC_NULL)) {
        translator_fail(t, "vertex fetch with a register offset at 0x%x", in.offset);
        return false;
    }
    // The element holding the fetched bytes; fetches may read some of its components only.
    u32 element = ~0u;
    FormatLayout layout = {};
    u32 offset = (u32)in.mem.offset;
    for (u32 i = 0; i < t.info->element_count; i++) {
        const VertexElement& e = t.info->elements[i];
        FormatLayout l;
        if (e.slot != slot || !vertex_format_layout(e.format, &l))
            continue;
        if (offset >= e.offset && offset < e.offset + (u32)l.components * l.component_bytes) {
            element = i;
            layout = l;
        }
    }
    if (element == ~0u) {
        translator_fail(t, "vertex fetch of slot %u offset %d matches no input element", slot, in.mem.offset);
        return false;
    }
    bool typed = is_format_access(in.op);
    u32 dwords = buffer_dwords(in);
    u32 first = offset - t.info->elements[element].offset; // bytes into the element
    if (typed)
        first = layout.component_bytes ? first / layout.component_bytes : 0;
    else if (first % 4) {
        translator_fail(t, "raw vertex fetch inside a dword at 0x%x", in.offset);
        return false;
    } else
        first /= 4;
    if ((t.out->vertex_element_mask & (1u << element)) && t.fetch_typed[element] != typed) {
        translator_fail(t, "vertex element %u fetched both raw and formatted", element);
        return false;
    }
    t.out->vertex_element_mask |= 1u << element;
    t.fetch_typed[element] = typed;
    t.fetch_class[element] = layout.value_class;
    // Typed elements always read all four components (missing ones read as 0, 0, 0, 1).
    u32 needed = typed ? 4 : first + dwords;
    if (needed > t.vs_fetch_components[element])
        t.vs_fetch_components[element] = (u8)needed;
    if (first + dwords > 4) {
        translator_fail(t, "vertex fetch past its element at 0x%x", in.offset);
        return false;
    }
    static const char k_comp[4] = {'x', 'y', 'z', 'w'};
    for (u32 i = 0; i < dwords; i++) {
        u32 c = typed ? (first + i < layout.components ? layout.order[first + i] : first + i) : first + i;
        const char* value = fmt(t, "input.e%u.%c", element, k_comp[c & 3]);
        write_vgpr(t, in, in.dst[0].reg + i,
                   typed && layout.value_class != 1 ? fmt(t, "asuint(%s)", value) : value);
    }
    return true;
}

static bool emit_buffer(Translator& t, const RdnaInst& in) {
    if (in.op == OP_BUFFER_GL0_INV || in.op == OP_BUFFER_GL1_INV)
        return true;
    u32 rsrc = in.src[1].reg;
    Tag lo = sgpr_tag(t, rsrc), hi = sgpr_tag(t, rsrc + 1);
    if (tag_kind(lo) == TAG_VB_DESC)
        return emit_vertex_fetch(t, in, tag_payload(lo));
    if (in.mem.lds || in.mem.tfe) {
        translator_fail(t, "buffer access with LDS or TFE at 0x%x", in.offset);
        return false;
    }
    bool store = is_store(in.op);
    bool format = is_format_access(in.op);
    u32 dwords = store ? in.src[1].count : buffer_dwords(in);
    if (store)
        dwords = (u32)((in.op >= OP_TBUFFER_STORE_FORMAT_X)     ? in.op - OP_TBUFFER_STORE_FORMAT_X + 1
                       : (in.op <= OP_BUFFER_STORE_FORMAT_XYZW) ? in.op - OP_BUFFER_STORE_FORMAT_X + 1
                       : in.op == OP_BUFFER_STORE_DWORDX2       ? 2
                       : in.op == OP_BUFFER_STORE_DWORDX3       ? 3
                       : in.op == OP_BUFFER_STORE_DWORDX4       ? 4
                                                                : 1);
    bool sub_dword = (in.op >= OP_BUFFER_LOAD_UBYTE && in.op <= OP_BUFFER_LOAD_SSHORT) ||
                     (in.op >= OP_BUFFER_STORE_BYTE && in.op <= OP_BUFFER_STORE_SHORT_D16_HI) ||
                     in.op >= OP_BUFFER_LOAD_UBYTE_D16 && in.op <= OP_BUFFER_LOAD_SHORT_D16_HI;
    if (sub_dword || is_d16_format(in.op) ||
        (in.op >= OP_BUFFER_ATOMIC_SWAP && in.op <= OP_BUFFER_ATOMIC_FMAX)) {
        translator_fail(t, "unsupported %s", rdna_op_name(in.op));
        return false;
    }
    u32 vaddr = in.src[0].reg;
    const char* index = in.mem.idxen ? vgpr(t, vaddr) : "0u";
    const char* voffset = in.mem.offen ? vgpr(t, vaddr + (in.mem.idxen ? 1 : 0)) : "0u";
    const char* offset = fmt(t, "(%s + 0x%xu + %s)", voffset, (u32)in.mem.offset, soffset(t, in, 2));

    if (tag_kind(hi) == TAG_ROOT_HI) {
        if (format) {
            translator_fail(t, "formatted access to a root buffer at 0x%x", in.offset);
            return false;
        }
        u32 parameter = tag_payload(hi);
        t.root_used |= 1ull << parameter;
        // Stride comes from the descriptor's second dword, which the program built itself.
        emit(t, "{ uint a_ = %s + %s * ((%s >> 16) & 0x3fffu) + %s;", sgpr(t, rsrc), index, sgpr(t, rsrc + 1),
             offset);
        t.depth++;
        if (store) {
            if (t.info->parameters[parameter].kind != USER_DATA_ROOT_UAV) {
                translator_fail(t, "store to a read-only root buffer at 0x%x", in.offset);
                return false;
            }
            const char* data = vgpr(t, in.dst[0].reg);
            if (dwords > 1) {
                char list[96] = {};
                usize n = 0;
                for (u32 i = 0; i < dwords; i++)
                    n += (usize)snprintf(list + n, sizeof(list) - n, "%sv%u", i ? ", " : "",
                                         in.dst[0].reg + i);
                for (u32 i = 0; i < dwords; i++)
                    use_vgpr(t, in.dst[0].reg + i);
                data = fmt(t, "uint%u(%s)", dwords, list);
            }
            emit(t, "if (EXEC) g_root%u.%s(a_, %s);", parameter, k_store_fn[dwords], data);
        } else {
            write_loaded(t, in, dwords, fmt(t, "g_root%u.%s(a_)", parameter, k_load_fn[dwords]));
        }
        t.depth--;
        emit(t, "}");
        return true;
    }
    if (tag_kind(lo) == TAG_HEAP && (tag_payload(lo) & 1) == HEAP_RESOURCES) {
        TableRangeKind kind = heap_kind(t, rsrc, store ? RANGE_UAV : RANGE_SRV);
        const char* heap_index = fmt(t, "(%s >> 5)", sgpr(t, rsrc));
        emit(t, "{");
        t.depth++;
        if (format) {
            if (in.enc == ENC_MTBUF || in.mem.offen || in.mem.offset) {
                translator_fail(t, "%s with an offset or format at 0x%x", rdna_op_name(in.op), in.offset);
                return false;
            }
            emit(t, "%s<float4> b_ = ResourceDescriptorHeap[%s];",
                 kind == RANGE_UAV || store ? "RWBuffer" : "Buffer", heap_index);
            if (store) {
                const char* c[4];
                for (u32 i = 0; i < 4; i++)
                    c[i] = i < dwords ? fmt(t, "asfloat(%s)", vgpr(t, in.dst[0].reg + i)) : "0.0f";
                emit(t, "if (EXEC) b_[%s] = float4(%s, %s, %s, %s);", index, c[0], c[1], c[2], c[3]);
            } else {
                emit(t, "float4 r_ = b_[%s];", index);
                static const char k_comp[4] = {'x', 'y', 'z', 'w'};
                for (u32 i = 0; i < dwords; i++)
                    write_vgpr(t, in, in.dst[0].reg + i, fmt(t, "asuint(r_.%c)", k_comp[i]));
            }
        } else {
            if (kind == RANGE_CBV && store) {
                translator_fail(t, "store to a constant buffer at 0x%x", in.offset);
                return false;
            }
            const char* stride = "0u";
            if (in.mem.idxen) {
                t.needs_descriptor_info = true;
                stride = fmt(t, "g_descriptor_info.Load(%s * 16u)", heap_index);
            }
            // Stores go through UAVs whatever the root signature says (dynamically indexed tables).
            emit(t, "%s b_ = ResourceDescriptorHeap[%s];",
                 kind == RANGE_UAV || store ? "RWByteAddressBuffer" : "ByteAddressBuffer", heap_index);
            emit(t, "uint a_ = %s * %s + %s;", index, stride, offset);
            if (store) {
                char list[96] = {};
                usize n = 0;
                for (u32 i = 0; i < dwords; i++) {
                    n += (usize)snprintf(list + n, sizeof(list) - n, "%sv%u", i ? ", " : "",
                                         in.dst[0].reg + i);
                    use_vgpr(t, in.dst[0].reg + i);
                }
                emit(t, "if (EXEC) b_.%s(a_, %s%s%s);", k_store_fn[dwords],
                     dwords > 1 ? fmt(t, "uint%u(", dwords) : "", list, dwords > 1 ? ")" : "");
            } else {
                write_loaded(t, in, dwords, fmt(t, "b_.%s(a_)", k_load_fn[dwords]));
            }
        }
        t.depth--;
        emit(t, "}");
        return true;
    }
    translator_fail(t, "%s at 0x%x through a descriptor the translator cannot place", rdna_op_name(in.op),
                    in.offset);
    return false;
}

// --- images ------------------------------------------------------------------------------------------

// Samplers built from constants: the descriptor's fields in D3D12 terms. False (with the reason) for
// what a desktop static sampler can't express.
static bool decode_sampler(const u32 w[4], InlineSampler* out, const char** why) {
    static const u8 k_address[8] = {1, 2, 3, 5, 4, 5, 4, 5}; // wrap, mirror, clamp, mirror once, border
    memset(out, 0, sizeof(*out));
    for (u32 a = 0; a < 3; a++)
        out->address[a] = k_address[(w[0] >> (3 * a)) & 7];
    u32 aniso_ratio = (w[0] >> 9) & 7;
    u32 compare = (w[0] >> 12) & 7;
    out->unnormalized = (w[0] >> 15) & 1;
    u32 reduction = (w[0] >> 29) & 3;
    out->min_lod = (f32)(w[1] & 0xFFF) / 256.0f;
    out->max_lod = (f32)((w[1] >> 12) & 0xFFF) / 256.0f;
    out->mip_bias = (f32)((s32)(w[2] << 18) >> 18) / 256.0f;
    u32 mag = (w[2] >> 20) & 3, min = (w[2] >> 22) & 3, mip = (w[2] >> 26) & 3;
    out->border_color = (u8)((w[3] >> 30) & 3);
    if (out->border_color == 3) {
        *why = "a border color from the border color table";
        return false;
    }
    if (reduction == 3) {
        *why = "an unknown reduction mode";
        return false;
    }
    if (mag >= 2 || min >= 2)
        out->filter = 0x55; // anisotropic
    else
        out->filter = (min == 1 ? 0x10 : 0) | (mag == 1 ? 0x4 : 0) | (mip == 2 ? 0x1 : 0);
    if (compare)
        out->filter |= 0x80;
    if (reduction)
        out->filter |= reduction == 1 ? 0x100 : 0x180; // minimum, maximum
    out->comparison = (u8)(compare + 1);
    out->max_anisotropy = (u8)(1u << (aniso_ratio > 4 ? 4 : aniso_ratio));
    if (out->max_lod >= 15.99f)
        out->max_lod = 3.402823466e+38f; // saturated: no limit
    return true;
}

// The program's inline sampler for a descriptor, added on first use; -1 (failed) if impossible.
static s32 inline_sampler(Translator& t, const RdnaInst& in, const u32 words[4], bool compare) {
    InlineSampler s;
    const char* why = nullptr;
    if (!decode_sampler(words, &s, &why)) {
        translator_fail(t, "%s at 0x%x with a constant sampler using %s", rdna_op_name(in.op), in.offset,
                        why);
        return -1;
    }
    TranslatedShader& out = *t.out;
    u32 i = 0;
    while (i < out.sampler_count && memcmp(&out.samplers[i], &s, sizeof(s)) != 0)
        i++;
    if (i == out.sampler_count) {
        if (i == SHADER_MAX_INLINE_SAMPLERS) {
            translator_fail(t, "more than %d constant samplers", SHADER_MAX_INLINE_SAMPLERS);
            return -1;
        }
        out.samplers[out.sampler_count++] = s;
        if (compare)
            t.sampler_compare |= 1u << i;
    } else if (((t.sampler_compare >> i) & 1) != (compare ? 1u : 0u)) {
        translator_fail(t, "constant sampler %u used with and without comparison", i);
        return -1;
    }
    return (s32)i;
}

struct ImageForm {
    bool sample, gather, load, store, mip, lod, bias, lod_zero, compare, grad, clamp, offset;
};

static ImageForm image_form(RdnaOp op) {
    ImageForm f = {};
    const char* name = rdna_op_name(op);
    if (op >= OP_IMAGE_SAMPLE && op <= OP_IMAGE_SAMPLE_C_LZ_O)
        f.sample = true;
    else if (op >= OP_IMAGE_GATHER4 && op <= OP_IMAGE_GATHER4_C_LZ_O)
        f.gather = true;
    else if (op <= OP_IMAGE_LOAD_MIP_PCK_SGN || op == OP_IMAGE_MSAA_LOAD)
        f.load = true;
    else if (op >= OP_IMAGE_STORE && op <= OP_IMAGE_STORE_MIP_PCK)
        f.store = true;
    f.mip = op == OP_IMAGE_LOAD_MIP || op == OP_IMAGE_STORE_MIP;
    if (f.sample || f.gather) {
        const char* suffix = strchr(name + 6, '_'); // after "image_"
        suffix = suffix ? strchr(suffix + 1, '_') : nullptr;
        for (const char* s = suffix; s && *s; s = strchr(s + 1, '_')) {
            if (!strncmp(s, "_lz", 3))
                f.lod_zero = true;
            else if (!strncmp(s, "_l", 2) && (s[2] == '_' || !s[2]))
                f.lod = true;
            else if (!strncmp(s, "_b", 2) && (s[2] == '_' || !s[2]))
                f.bias = true;
            else if (!strncmp(s, "_c", 2) && (s[2] == '_' || !s[2]))
                f.compare = true;
            else if (!strncmp(s, "_cl", 3))
                f.clamp = true;
            else if (!strncmp(s, "_d", 2) && (s[2] == '_' || !s[2]))
                f.grad = true;
            else if (!strncmp(s, "_o", 2) && (s[2] == '_' || !s[2]))
                f.offset = true;
        }
    }
    return f;
}

static const char* const k_texture_types[8] = {"Texture1D",   "Texture2D",       "Texture3D",
                                               "TextureCube", "Texture1DArray",  "Texture2DArray",
                                               "Texture2DMS", "Texture2DMSArray"};
static const u8 k_dim_coords[8] = {1, 2, 3, 3, 2, 3, 2, 3};
static const u8 k_dim_grads[8] = {1, 2, 3, 3, 1, 2, 2, 2};

static const char* float_vector(Translator& t, const u32* regs, u32 count) {
    if (count == 1)
        return fmt(t, "asfloat(%s)", vgpr(t, regs[0]));
    char list[160] = {};
    usize n = 0;
    for (u32 i = 0; i < count; i++)
        n += (usize)snprintf(list + n, sizeof(list) - n, "%sasfloat(v%u)", i ? ", " : "", regs[i]);
    for (u32 i = 0; i < count; i++)
        use_vgpr(t, regs[i]);
    return fmt(t, "float%u(%s)", count, list);
}

static const char* uint_vector(Translator& t, const u32* regs, u32 count) {
    if (count == 1)
        return vgpr(t, regs[0]);
    char list[160] = {};
    usize n = 0;
    for (u32 i = 0; i < count; i++)
        n += (usize)snprintf(list + n, sizeof(list) - n, "%sv%u", i ? ", " : "", regs[i]);
    for (u32 i = 0; i < count; i++)
        use_vgpr(t, regs[i]);
    return fmt(t, "uint%u(%s)", count, list);
}

static bool emit_image(Translator& t, const RdnaInst& in) {
    const RdnaMemory& m = in.mem;
    ImageForm form = image_form(in.op);
    if (in.op == OP_IMAGE_BVH_INTERSECT_RAY || in.op == OP_IMAGE_BVH64_INTERSECT_RAY) {
        translator_fail(t, "unsupported %s", rdna_op_name(in.op));
        return false;
    }
    if (m.a16 || m.d16 || m.tfe || m.lwe) {
        translator_fail(t, "%s with 16-bit data or addresses at 0x%x", rdna_op_name(in.op), in.offset);
        return false;
    }
    if (m.dim == DIM_CUBE && (form.sample || form.gather)) {
        translator_fail(t, "cube sampling at 0x%x", in.offset);
        return false;
    }
    if (form.offset) {
        translator_fail(t, "%s with texel offsets at 0x%x", rdna_op_name(in.op), in.offset);
        return false;
    }
    u32 rsrc = in.src[1].reg;
    Tag tag = sgpr_tag(t, rsrc);
    if (tag_kind(tag) != TAG_HEAP || (tag_payload(tag) & 1) != HEAP_RESOURCES) {
        translator_fail(t, "%s at 0x%x with a resource not from a descriptor table", rdna_op_name(in.op),
                        in.offset);
        return false;
    }
    // Address VGPRs in order.
    u32 regs[16];
    u32 count = rdna_image_address_count(in.op, m.dim);
    if (count > 16) {
        translator_fail(t, "too many image address registers");
        return false;
    }
    for (u32 i = 0; i < count; i++)
        regs[i] = m.nsa_count && i > 0 ? m.nsa_addr[i - 1] : in.src[0].reg + i;
    u32 at = 0;
    u32 bias_reg = 0, compare_reg = 0, lod_reg = 0, clamp_reg = 0, grad_regs[6] = {};
    if (form.bias)
        bias_reg = regs[at++];
    if (form.compare)
        compare_reg = regs[at++];
    u32 grads = k_dim_grads[m.dim];
    if (form.grad) {
        for (u32 i = 0; i < grads * 2; i++)
            grad_regs[i] = regs[at++];
    }
    u32 coords = k_dim_coords[m.dim];
    const u32* coord_regs = regs + at;
    at += coords;
    if (form.lod || form.mip || in.op == OP_IMAGE_MSAA_LOAD)
        lod_reg = regs[at++];
    if (form.clamp)
        clamp_reg = regs[at++];

    TableRangeKind kind = heap_kind(t, rsrc, form.store ? RANGE_UAV : RANGE_SRV);
    bool uav = kind == RANGE_UAV || form.store;
    const char* resource = fmt(t, "%s >> 5", sgpr(t, rsrc));
    emit(t, "{");
    t.depth++;
    if (uav) {
        static const char* const k_rw[8] = {"RWTexture1D",
                                            "RWTexture2D",
                                            "RWTexture3D",
                                            "RWTexture2DArray",
                                            "RWTexture1DArray",
                                            "RWTexture2DArray",
                                            "",
                                            ""};
        if (!k_rw[m.dim][0]) {
            translator_fail(t, "storage image of dimension %u", m.dim);
            return false;
        }
        emit(t, "%s<float4> tex_ = ResourceDescriptorHeap[%s];", k_rw[m.dim], resource);
    } else {
        emit(t, "%s<float4> tex_ = ResourceDescriptorHeap[%s];", k_texture_types[m.dim], resource);
    }
    const char* value = nullptr;
    if (form.sample || form.gather) {
        u32 samp = in.src[2].reg;
        Tag stag = sgpr_tag(t, samp);
        u32 words[4];
        bool literal = true;
        for (u32 i = 0; i < 4; i++)
            literal &= sgpr_constant(t.tags, samp + i, &words[i]);
        if (literal) {
            s32 index = inline_sampler(t, in, words, form.compare);
            if (index < 0)
                return false;
            emit(t, "%s smp_ = g_sampler%d;", form.compare ? "SamplerComparisonState" : "SamplerState",
                 index);
        } else if (tag_kind(stag) == TAG_HEAP && (tag_payload(stag) & 1) == HEAP_SAMPLERS) {
            emit(t, "%s smp_ = SamplerDescriptorHeap[%s >> 4];",
                 form.compare ? "SamplerComparisonState" : "SamplerState", sgpr(t, samp));
        } else {
            translator_fail(t, "%s at 0x%x with a sampler not from a sampler table", rdna_op_name(in.op),
                            in.offset);
            return false;
        }
        const char* uv = float_vector(t, coord_regs, coords);
        if (form.gather) {
            static const char* const k_gather[4] = {"GatherRed", "GatherGreen", "GatherBlue", "GatherAlpha"};
            u32 c = m.dmask & 2 ? 1 : m.dmask & 4 ? 2 : m.dmask & 8 ? 3 : 0;
            if (form.compare)
                value = fmt(t, "tex_.GatherCmpRed(smp_, %s, asfloat(%s))", uv, vgpr(t, compare_reg));
            else
                value = fmt(t, "tex_.%s(smp_, %s)", k_gather[c], uv);
            if (form.lod || form.bias || form.grad || form.clamp) {
                translator_fail(t, "%s variant at 0x%x", rdna_op_name(in.op), in.offset);
                return false;
            }
        } else if (form.compare) {
            const char* ref = fmt(t, "asfloat(%s)", vgpr(t, compare_reg));
            if (form.lod_zero)
                value = fmt(t, "tex_.SampleCmpLevelZero(smp_, %s, %s).xxxx", uv, ref);
            else if (form.lod)
                value =
                    fmt(t, "tex_.SampleCmpLevel(smp_, %s, %s, asfloat(%s)).xxxx", uv, ref, vgpr(t, lod_reg));
            else if (!form.bias && !form.grad && !form.clamp)
                value = fmt(t, "tex_.SampleCmp(smp_, %s, %s).xxxx", uv, ref);
        } else if (form.lod_zero) {
            value = fmt(t, "tex_.SampleLevel(smp_, %s, 0.0f)", uv);
        } else if (form.lod) {
            value = fmt(t, "tex_.SampleLevel(smp_, %s, asfloat(%s))", uv, vgpr(t, lod_reg));
        } else if (form.bias && !form.clamp) {
            value = fmt(t, "tex_.SampleBias(smp_, %s, asfloat(%s))", uv, vgpr(t, bias_reg));
        } else if (form.grad && !form.clamp) {
            u32 dx[3], dy[3];
            for (u32 i = 0; i < grads; i++) {
                dx[i] = grad_regs[i];
                dy[i] = grad_regs[grads + i];
            }
            value = fmt(t, "tex_.SampleGrad(smp_, %s, %s, %s)", uv, float_vector(t, dx, grads),
                        float_vector(t, dy, grads));
        } else if (!form.bias && !form.grad) {
            value = form.clamp ? fmt(t, "tex_.Sample(smp_, %s, 0, asfloat(%s))", uv, vgpr(t, clamp_reg))
                               : fmt(t, "tex_.Sample(smp_, %s)", uv);
        }
        if (!value) {
            translator_fail(t, "%s variant at 0x%x", rdna_op_name(in.op), in.offset);
            return false;
        }
    } else if (form.load) {
        const char* xy = uint_vector(t, coord_regs, coords);
        if (in.op == OP_IMAGE_MSAA_LOAD)
            value = fmt(t, "tex_.Load(%s, %s)", xy, vgpr(t, lod_reg));
        else if (uav)
            value = fmt(t, "tex_[%s]", xy);
        else
            value = fmt(t, "tex_.Load(uint%u(%s, %s))", coords + 1, xy, form.mip ? vgpr(t, lod_reg) : "0u");
    } else if (form.store) {
        const char* xy = uint_vector(t, coord_regs, coords);
        const char* c[4] = {"0.0f", "0.0f", "0.0f", "0.0f"};
        u32 source = 0;
        for (u32 i = 0; i < 4; i++)
            if (m.dmask & (1u << i))
                c[i] = fmt(t, "asfloat(%s)", vgpr(t, in.dst[0].reg + source++));
        emit(t, "if (EXEC) tex_[%s] = float4(%s, %s, %s, %s);", xy, c[0], c[1], c[2], c[3]);
        t.depth--;
        emit(t, "}");
        return true;
    } else if (in.op == OP_IMAGE_GET_RESINFO) {
        emit(t, "uint w_ = 0u, h_ = 0u, d_ = 1u, l_ = 1u;");
        const char* mip = vgpr(t, in.src[0].reg);
        switch (m.dim) {
        case DIM_1D:
            emit(t, "tex_.GetDimensions(%s, w_, l_);", mip);
            break;
        case DIM_2D:
        case DIM_CUBE:
            emit(t, "tex_.GetDimensions(%s, w_, h_, l_);", mip);
            break;
        case DIM_3D:
        case DIM_2D_ARRAY:
            emit(t, "tex_.GetDimensions(%s, w_, h_, d_, l_);", mip);
            break;
        case DIM_1D_ARRAY:
            emit(t, "tex_.GetDimensions(%s, w_, h_, l_);", mip);
            break;
        default:
            translator_fail(t, "resource info of dimension %u", m.dim);
            return false;
        }
        value = "uint4(w_, h_, d_, l_)";
        u32 dst = in.dst[0].reg;
        for (u32 i = 0; i < 4; i++)
            if (m.dmask & (1u << i))
                write_vgpr(t, in, dst++, fmt(t, "%s.%c", value, "xyzw"[i]));
        t.depth--;
        emit(t, "}");
        return true;
    } else if (in.op == OP_IMAGE_GET_LOD) {
        u32 samp = in.src[2].reg;
        emit(t, "SamplerState smp_ = SamplerDescriptorHeap[%s >> 4];", sgpr(t, samp));
        const char* uv = float_vector(t, coord_regs, coords);
        value = fmt(
            t,
            "float4(tex_.CalculateLevelOfDetail(smp_, %s), tex_.CalculateLevelOfDetailUnclamped(smp_, %s), "
            "0.0f, 0.0f)",
            uv, uv);
    } else {
        translator_fail(t, "unsupported %s", rdna_op_name(in.op));
        return false;
    }
    emit(t, "float4 r_ = %s;", value);
    u32 dst = in.dst[0].reg;
    u32 mask = form.gather ? 0xF : m.dmask;
    for (u32 i = 0; i < 4; i++)
        if (mask & (1u << i))
            write_vgpr(t, in, dst++, fmt(t, "asuint(r_.%c)", "xyzw"[i]));
    t.depth--;
    emit(t, "}");
    return true;
}

// --- LDS ---------------------------------------------------------------------------------------------

static bool emit_lds(Translator& t, const RdnaInst& in) {
    if (in.mem.gds) {
        translator_fail(t, "GDS access at 0x%x", in.offset);
        return false;
    }
    t.uses_lds = true;
    const char* addr = vgpr(t, in.src[0].reg);
    u32 off0 = (u32)in.mem.offset, off1 = in.mem.offset1;
    u32 offset = off0 | (off1 << 8);
    const char* d0 = vgpr(t, in.src[1].reg);
    switch (in.op) {
    case OP_DS_WRITE_B32:
        emit(t, "if (EXEC) g_lds[(%s + %uu) >> 2] = %s;", addr, offset, d0);
        return true;
    case OP_DS_WRITE_B64:
        emit(t, "if (EXEC) { g_lds[(%s + %uu) >> 2] = %s; g_lds[(%s + %uu) >> 2] = %s; }", addr, offset, d0,
             addr, offset + 4, vgpr(t, in.src[1].reg + 1));
        return true;
    case OP_DS_WRITE_B128:
        emit(t,
             "if (EXEC) { uint a_ = (%s + %uu) >> 2; g_lds[a_] = %s; g_lds[a_ + 1u] = %s; g_lds[a_ + 2u] = "
             "%s; "
             "g_lds[a_ + 3u] = %s; }",
             addr, offset, d0, vgpr(t, in.src[1].reg + 1), vgpr(t, in.src[1].reg + 2),
             vgpr(t, in.src[1].reg + 3));
        return true;
    case OP_DS_WRITE2_B32:
    case OP_DS_WRITE2ST64_B32: {
        u32 unit = in.op == OP_DS_WRITE2ST64_B32 ? 256 : 4;
        emit(t, "if (EXEC) { g_lds[(%s + %uu) >> 2] = %s; g_lds[(%s + %uu) >> 2] = %s; }", addr, off0 * unit,
             d0, addr, off1 * unit, vgpr(t, in.src[2].reg));
        return true;
    }
    case OP_DS_READ_B32:
        write_vgpr(t, in, in.dst[0].reg, fmt(t, "g_lds[(%s + %uu) >> 2]", addr, offset));
        return true;
    case OP_DS_READ_B64:
    case OP_DS_READ_B128: {
        u32 n = in.op == OP_DS_READ_B64 ? 2 : 4;
        emit(t, "{ uint a_ = (%s + %uu) >> 2;", addr, offset);
        t.depth++;
        // Read every dword before writing any: the destination may overlap the address.
        for (u32 i = 0; i < n; i++)
            emit(t, "uint r%u_ = g_lds[a_ + %uu];", i, i);
        for (u32 i = 0; i < n; i++)
            write_vgpr(t, in, in.dst[0].reg + i, fmt(t, "r%u_", i));
        t.depth--;
        emit(t, "}");
        return true;
    }
    case OP_DS_READ2_B32:
    case OP_DS_READ2ST64_B32: {
        u32 unit = in.op == OP_DS_READ2ST64_B32 ? 256 : 4;
        emit(t, "{ uint r0_ = g_lds[(%s + %uu) >> 2]; uint r1_ = g_lds[(%s + %uu) >> 2];", addr, off0 * unit,
             addr, off1 * unit);
        t.depth++;
        write_vgpr(t, in, in.dst[0].reg, "r0_");
        write_vgpr(t, in, in.dst[0].reg + 1, "r1_");
        t.depth--;
        emit(t, "}");
        return true;
    }
    case OP_DS_ADD_U32:
    case OP_DS_MIN_U32:
    case OP_DS_MAX_U32:
    case OP_DS_MIN_I32:
    case OP_DS_MAX_I32:
    case OP_DS_AND_B32:
    case OP_DS_OR_B32:
    case OP_DS_XOR_B32:
    case OP_DS_ADD_RTN_U32:
    case OP_DS_MIN_RTN_U32:
    case OP_DS_MAX_RTN_U32:
    case OP_DS_AND_RTN_B32:
    case OP_DS_OR_RTN_B32:
    case OP_DS_XOR_RTN_B32:
    case OP_DS_WRXCHG_RTN_B32: {
        const char* fn;
        bool rtn = in.op >= OP_DS_ADD_RTN_U32;
        switch (in.op) {
        case OP_DS_ADD_U32:
        case OP_DS_ADD_RTN_U32:
            fn = "InterlockedAdd";
            break;
        case OP_DS_MIN_U32:
        case OP_DS_MIN_RTN_U32:
        case OP_DS_MIN_I32:
            fn = "InterlockedMin";
            break;
        case OP_DS_MAX_U32:
        case OP_DS_MAX_RTN_U32:
        case OP_DS_MAX_I32:
            fn = "InterlockedMax";
            break;
        case OP_DS_AND_B32:
        case OP_DS_AND_RTN_B32:
            fn = "InterlockedAnd";
            break;
        case OP_DS_OR_B32:
        case OP_DS_OR_RTN_B32:
            fn = "InterlockedOr";
            break;
        case OP_DS_WRXCHG_RTN_B32:
            fn = "InterlockedExchange";
            break;
        default:
            fn = "InterlockedXor";
            break;
        }
        if (in.op == OP_DS_MIN_I32 || in.op == OP_DS_MAX_I32) {
            translator_fail(t, "signed LDS atomics");
            return false;
        }
        if (rtn) {
            emit(t, "{ uint o_ = 0u; if (EXEC) %s(g_lds[(%s + %uu) >> 2], %s, o_);", fn, addr, offset, d0);
            t.depth++;
            write_vgpr(t, in, in.dst[0].reg, "o_");
            t.depth--;
            emit(t, "}");
        } else {
            emit(t, "if (EXEC) %s(g_lds[(%s + %uu) >> 2], %s);", fn, addr, offset, d0);
        }
        return true;
    }
    case OP_DS_BPERMUTE_B32:
        // Read data0 from the lane addressed (in bytes) by this lane's address.
        write_vgpr(
            t, in, in.dst[0].reg,
            fmt(t, "WaveReadLaneAt(%s, ((%s + %uu) >> 2) & %su)", d0, addr, offset, t.wave32 ? "31" : "63"));
        return true;
    default:
        translator_fail(t, "unsupported %s", rdna_op_name(in.op));
        return false;
    }
}

// --- global memory -----------------------------------------------------------------------------------

static bool emit_global(Translator& t, const RdnaInst& in) {
    const RdnaOperand& saddr = in.src[2];
    u32 parameter = ~0u;
    const char* base = "0u";
    if (saddr.kind == OPND_SGPR) {
        Tag hi = sgpr_tag(t, saddr.reg + 1);
        if (tag_kind(hi) == TAG_ROOT_HI) {
            parameter = tag_payload(hi);
            base = fmt(t, "(%s + %s)", sgpr(t, saddr.reg), vgpr(t, in.src[0].reg));
        }
    } else {
        Tag hi = t.tags.vgpr[(in.src[0].reg + 1) & 255];
        if (tag_kind(hi) == TAG_ROOT_HI) {
            parameter = tag_payload(hi);
            base = vgpr(t, in.src[0].reg);
        }
    }
    if (parameter == ~0u) {
        translator_fail(t, "%s at 0x%x from an address the translator cannot place", rdna_op_name(in.op),
                        in.offset);
        return false;
    }
    t.root_used |= 1ull << parameter;
    u32 dwords = in.dst[0].count;
    const char* a = fmt(t, "(%s + 0x%xu)", base, (u32)in.mem.offset);
    if (in.op >= OP_GLOBAL_LOAD_DWORD && in.op <= OP_GLOBAL_LOAD_DWORDX3) {
        write_loaded(t, in, dwords, fmt(t, "g_root%u.%s(%s)", parameter, k_load_fn[dwords], a));
        return true;
    }
    translator_fail(t, "unsupported %s", rdna_op_name(in.op));
    return false;
}

bool emit_memory(Translator& t, const RdnaInst& in) {
    switch (in.enc) {
    case ENC_SMEM:
        return emit_scalar_memory(t, in);
    case ENC_MUBUF:
    case ENC_MTBUF:
        return emit_buffer(t, in);
    case ENC_MIMG:
        return emit_image(t, in);
    case ENC_DS:
        return emit_lds(t, in);
    case ENC_GLOBAL:
        return emit_global(t, in);
    default:
        translator_fail(t, "unsupported %s", rdna_op_name(in.op));
        return false;
    }
}
