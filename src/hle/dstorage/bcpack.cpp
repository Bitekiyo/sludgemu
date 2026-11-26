// BCPack decoder.  docs/BCPACK.md describes the stream; this file follows it in
// three layers:
//   1. a multiplexed rANS reader that turns the byte stream into per-block
//      "records" (one small symbol per field of a block),
//   2. per-format reconstruction: each record holds deltas against an earlier
//      block (or a predefined dictionary block), giving the block's endpoints
//      and indices in a working form,
//   3. packing the working form into the actual BCn block.
//
// Working forms for BC1-BC5 keep every field in its own bit lane of a 32- or
// 64-bit word, so adding a reference block is one SWAR add per word.

#include "hle/dstorage/bcpack.h"

#include <emmintrin.h>
#include <intrin.h>
#include <string.h>

// --- table layouts (filled by bcpack_tables.inc) -----------------------------

// One rANS distribution, split into power-of-two buckets of equal-frequency
// symbols (see docs/BCPACK.md).
struct RansRow {
    u32 start[12]; // first slot per bucket; 0x10000 past the last bucket
    u32 freq[10];  // slots per symbol
    u64 recip[10]; // ceil(2^32 / freq): (n * recip) >> 32 == n / freq for n < 2^16
};

// BC1: color = c0 | c1 << 16 (RGB565 each), index = 2 bits per texel.
struct Bc1Work {
    u32 color;
    u32 index;
};

// BC2: alpha = 4 bits per texel, then the BC1-style color part.
struct Bc2Work {
    u64 alpha;
    u32 color;
    u32 index;
};

// BC3: endpoints = c0 | c1 << 16 | a0 << 32 | a1 << 40; 3-bit alpha and
// 2-bit color indices per texel.
struct Bc3Work {
    u64 endpoints;
    u64 alpha_index;
    u32 color_index;
};

// BC4: 3 bits per texel; endpoints = a0 | a1 << 8.
struct Bc4Work {
    u64 index;
    u16 endpoints;
};

// BC5: two BC4 channels; endpoints = r0 | r1 << 8 | g0 << 16 | g1 << 24.
struct Bc5Work {
    u64 index[2];
    u32 endpoints;
};

// BC6H endpoints, left-aligned in 16 bits (low bits below the mode's
// precision are zero): v[0..2] = e0 of R, G, B; v[3..5] = e1..e3 of R;
// v[6..8] = e1..e3 of G; v[9..11] = e1..e3 of B.  Single-region modes use
// only e0/e1 (e2/e3 are derived copies).
struct Bc6Endpoints {
    u8 mode;  // 1-14
    u8 shape; // partition
    u16 v[12];
};

// BC6H indices, signed and centered: texel index = value + 2^(bits-1).
struct Bc6Indices {
    u8 mode;
    s8 index[16];
};

// One run of a BC6H block layout: `len` bits of a field, from bit `src`, go to
// block bit `dst`; reversed runs flip the bit order.
struct Bc6Run {
    u8 field;
    u8 src;
    u8 dst;
    u8 len;
    u8 reversed;
};

// BC7 endpoints, left-aligned in 16 bits: v[subset][R0 R1 G0 G1 B0 B1 A0 A1].
struct Bc7Endpoints {
    u16 config;      // see k_bc7_config
    u8 precision[4]; // R, G, B, A bits (red carries the p-bit in its low bit)
    u16 v[3][8];
};

// BC7 indices, sign-extended raw index values; set 1 exists in modes 4/5.
struct Bc7Indices {
    u8 bits[2];
    s8 index[2][16];
};

#include "hle/dstorage/bcpack_tables.inc"

// --- small helpers ------------------------------------------------------------

enum {
    k_lane_max = 16,
    k_round_bytes = 8,      // a round hands out at most this many input bytes
    k_idle_round_limit = 4, // rounds without any progress before giving up
    k_batch_blocks = 64,    // blocks decoded into records before reconstruction
};

// Stored deltas are zigzag coded (0, -1, 1, -2, ...).  Callers mask the
// result to the width of the field.
static inline u32 unzigzag(u32 v) {
    return (v >> 1) ^ (0u - (v & 1));
}

// Spreads 8 bits to the even bit positions of 16.
static inline u32 spread_bits(u32 v) {
    v = (v | v << 4) & 0x0F0F;
    v = (v | v << 2) & 0x3333;
    v = (v | v << 1) & 0x5555;
    return v;
}

// Wide fields are split over two symbols of narrower lanes: the first symbol
// supplies the even bits, the second the odd bits.
static inline u32 merge_bits(u32 even, u32 odd) {
    return spread_bits(even) | spread_bits(odd) << 1;
}

// Adds two words lane by lane, each lane wrapping on its own; `high` marks the
// top bit of every lane.
static inline u64 lane_add(u64 a, u64 b, u64 high) {
    return ((a & ~high) + (b & ~high)) ^ ((a ^ b) & high);
}

// Stored indices count along the endpoint line (0 = first endpoint, max =
// second endpoint); BCn numbers the second endpoint 1 and the interpolated
// values from 2.  So: max -> 1, 0 -> 0, others +1.  `lsb` marks the low bit
// of each lane.
static inline u64 remap_index2(u64 x, u64 lsb) {
    u64 b0 = x & lsb;
    u64 b1 = (x >> 1) & lsb;
    u64 last = b0 & b1;
    return (x ^ (last * 3)) + (b0 | b1);
}

static inline u64 remap_index3(u64 x, u64 lsb) {
    u64 b0 = x & lsb;
    u64 b1 = (x >> 1) & lsb;
    u64 b2 = (x >> 2) & lsb;
    u64 last = b0 & b1 & b2;
    return (x ^ (last * 7)) + (b0 | b1 | b2);
}

static const u64 k_lsb2 = 0x5555555555555555ull; // 2-bit lanes
static const u64 k_lsb3 = 0x0000249249249249ull; // 16 3-bit lanes

static inline void store_u32(u8* p, u32 v) {
    memcpy(p, &v, 4);
}

static inline void store_u64(u8* p, u64 v) {
    memcpy(p, &v, 8);
}

// A block refers to earlier blocks by distance.  Distance 0 means "no
// reference"; distances reaching before the first block select predefined
// dictionary entries (entry 0 is one block before the stream).
template <typename T>
static inline const T* pick_reference(u64 block, u32 distance, const T* ring, const T* predef,
                                      const T* zero) {
    if (distance == 0)
        return zero;
    if (block >= distance)
        return &ring[(block - distance) & 255];
    return &predef[distance - block - 1];
}

// Formats whose first two lanes carry 8-bit symbols store the reference
// selectors directly; the others split each over two 4-bit symbols.
struct References {
    u32 index_distance;
    u32 endpoint_distance;
};

static inline References references_direct(const u8* r) {
    References refs;
    refs.index_distance = r[1];
    refs.endpoint_distance = (r[1] + unzigzag(r[0])) & 255;
    return refs;
}

static inline References references_split(const u8* r) {
    References refs;
    refs.index_distance = merge_bits(r[2], r[3]);
    refs.endpoint_distance = (refs.index_distance + unzigzag(merge_bits(r[0], r[1]))) & 255;
    return refs;
}

// --- multiplexed rANS reader ------------------------------------------------

enum : u8 {
    PHASE_HEADER,      // next byte is a fragment header
    PHASE_LENGTH,      // next byte is the (low) length byte
    PHASE_LENGTH_HIGH, // next byte is the high length byte
    PHASE_DATA,        // fragment payload
};

struct RansLane {
    const RansRow* row; // distribution of the current fragment
    u32 state;
    u32 frag_left; // payload bytes not yet taken into the state
    u32 syms_left; // symbols still owed; keeps counting below zero
    u32 input;     // byte handed to the lane...
    u32 has_input; // ...and not consumed yet
    u32 wants_input;
    u32 has_symbol; // a decoded symbol waits to be stored
    u32 symbol;
    u32 phase;
    u32 lane_class;  // symbol class of this lane (symbols have class + 1 bits)
    u32 long_length; // fragment header flags
    u32 reset_state;
};

struct SymbolReader {
    RansLane lane[k_lane_max];
    const u8* src;
    u64 src_size;
    u64 src_pos;
    u64 position; // symbol position the lanes are currently filling
    u32 lane_count;
    u32 per_block_shift; // log2 of symbols per lane per block (0 or 1)
    u32 record_size;     // lane_count << per_block_shift
    u32 stored_mask;     // lanes that delivered the symbol for `position`
    u32 symbol_mask;     // lanes holding a decoded symbol (mirrors has_symbol)
    u32 full_mask;
};

static void reader_init(SymbolReader* r, const u8* lane_class, u32 lane_count, u32 per_block_shift,
                        const u8* src, u64 src_size, u64 block_count) {
    memset(r, 0, sizeof(*r));
    r->src = src;
    r->src_size = src_size;
    r->lane_count = lane_count;
    r->per_block_shift = per_block_shift;
    r->record_size = lane_count << per_block_shift;
    r->full_mask = (1u << lane_count) - 1;
    for (u32 i = 0; i < lane_count; i++) {
        RansLane* l = &r->lane[i];
        l->phase = PHASE_HEADER;
        l->lane_class = lane_class[i];
        l->wants_input = 1;
        // the reference keeps this count in 32 bits
        l->syms_left = (u32)(block_count << per_block_shift);
    }
}

// Header and length bytes of a fragment.
static void lane_take_header_byte(RansLane* l) {
    u32 b = l->input;
    l->has_input = 0;
    l->wants_input = 1;
    if (l->phase == PHASE_HEADER) {
        // bits 0-3: distribution, 4: reset state afterwards, 5: two length
        // bytes, 6-7: how many bits the symbols are narrower than the lane's
        u32 symbol_class = l->lane_class - (b >> 6);
        l->row = &k_rans_rows[symbol_class * 16 + (b & 15)];
        l->reset_state = (b >> 4) & 1;
        l->long_length = (b >> 5) & 1;
        l->frag_left = 0;
        l->phase = PHASE_LENGTH;
    } else if (l->phase == PHASE_LENGTH) {
        if (l->long_length) {
            l->frag_left = b;
            l->phase = PHASE_LENGTH_HIGH;
        } else {
            l->frag_left = b + 1;
            l->phase = PHASE_DATA;
        }
    } else {
        l->frag_left = (l->frag_left | b << 8) + 257;
        l->phase = PHASE_DATA;
    }
}

// Advances a lane by one step: takes its pending byte if it can use it, and
// decodes a symbol when the state holds enough information.
static inline void lane_step(RansLane* l) {
    if (l->phase != PHASE_DATA) {
        if (l->has_input)
            lane_take_header_byte(l);
        return;
    }

    // Data-dependent choices below are computed with masks: they are close to
    // random, so branches would mispredict about half the time.
    u32 x = l->state;
    u32 take = (x < (1u << 24)) & l->has_input;
    u32 take_mask = 0u - take;
    x = (x & ~take_mask) | ((x << 8 | l->input) & take_mask);
    l->frag_left -= take;
    l->has_input ^= take;
    l->wants_input = (l->frag_left != 0) & (l->has_input ^ 1);
    if (x <= (1u << 24)) {
        if (l->frag_left != 0) {
            l->state = x;
        } else {
            // Fragment exhausted: the next byte is a new header.  The state
            // carries over unchanged (a byte just shifted in is dropped) unless
            // the header asked for a restart.
            l->phase = PHASE_HEADER;
            l->wants_input = 1;
            if (l->reset_state)
                l->state = 0;
        }
        return;
    }

    // Decode one symbol: 16-bit slot -> bucket -> symbol within the bucket.
    // Bucket starts ascend, so the count of starts <= slot is the bucket.
    l->syms_left--;
    u32 slot = x & 0xFFFF;
    const RansRow* row = l->row;
    __m128i limit = _mm_set1_epi32((int)slot + 1);
    __m128i lo = _mm_cmpgt_epi32(limit, _mm_loadu_si128((const __m128i*)(row->start + 1)));
    __m128i hi = _mm_cmpgt_epi32(limit, _mm_loadu_si128((const __m128i*)(row->start + 5)));
    u32 below = (u32)_mm_movemask_ps(_mm_castsi128_ps(lo)) | (u32)_mm_movemask_ps(_mm_castsi128_ps(hi)) << 4;
    unsigned long k;
    _BitScanForward(&k, ~below);
    u32 offset = slot - row->start[k];
    u32 freq = row->freq[k];
    u32 r = (u32)((offset * row->recip[k]) >> 32);
    // Symbol 0 owns slots 1..freq-1 (slot 0 is never produced by an encoder),
    // which shows up as one slot less and a start one higher.  Bucket k > 0
    // starts at symbol 2^(k-1); r is always 0 in bucket 0.
    u32 first = k == 0;
    l->symbol = ((1u << k) >> 1) + r;
    l->state = (x >> 16) * (freq - first) + offset - r * freq - first;
    l->has_symbol = 1;
}

// Runs rounds until the next `block_count` blocks have all their symbols, and
// writes them as records (record_size bytes per block, lane-major).
//
// A round visits the lanes in order.  Each lane that wants a byte gets the
// next input byte, but only 8 bytes go out per round; a lane that wants one
// after that sits the round out.  A lane stores its decoded symbol unless it
// already delivered one for the current position, in which case it waits for
// the slower lanes.  The position advances once every lane delivered.
static bool read_records(SymbolReader* r, u8* records, u32 block_count) {
    u64 first = r->position;
    u64 end = first + ((u64)block_count << r->per_block_shift);
    u32 shift = r->per_block_shift;
    u32 idle_rounds = 0;
    while (r->position < end) {
        u64 rel = r->position - first;
        u8* out = records + (rel >> shift) * r->record_size + (rel & ((1u << shift) - 1));
        // With a full round's worth of input left, lanes fetch without
        // bounds checks (the byte is read even when unused).
        bool near_end = r->src_size - r->src_pos < k_round_bytes;
        const u8* in = r->src + r->src_pos;
        u32 stored = r->stored_mask;
        u32 symbols = r->symbol_mask;
        u32 used = 0;
        u32 decoded = 0;
        // Lanes that already delivered and hold their next symbol wait; skip
        // them by mask rather than by a hard-to-predict branch per lane.
        u32 active = r->full_mask & ~(symbols & stored);
        while (active) {
            unsigned long index;
            _BitScanForward(&index, active);
            active &= active - 1;
            u32 i = (u32)index;
            RansLane* l = &r->lane[i];
            u32 wants = l->wants_input;
            if (wants & (used >= k_round_bytes))
                continue;
            u32 fetch = wants & (l->syms_left != 0);
            u32 byte;
            if (near_end) {
                if (fetch && r->src_pos + used >= r->src_size)
                    return false;            // stream ends early
                byte = fetch ? in[used] : 0; // near the end: read only when needed
            } else {
                byte = in[used & 7];
            }
            // a lane that fetches holds no byte, so only then is input replaced
            u32 fetch_mask = 0u - fetch;
            l->input = (l->input & ~fetch_mask) | (byte & fetch_mask);
            l->has_input |= fetch;
            used += fetch;
            u32 has = l->has_symbol;
            u8* slot = out + (i << shift);
            u32 has_mask = 0u - has;
            *slot = (u8)((*slot & ~has_mask) | (l->symbol & has_mask));
            stored |= has << i;
            l->has_symbol = 0;
            lane_step(l);
            decoded |= l->has_symbol;
            symbols = (symbols & ~(1u << i)) | l->has_symbol << i;
        }
        r->symbol_mask = symbols;
        r->src_pos += used;
        u32 progress = used | decoded | (stored != r->stored_mask);
        if (stored == r->full_mask) {
            r->position++;
            stored = 0;
        }
        r->stored_mask = stored;
        // A malformed stream can leave every lane waiting on something that
        // never comes; detect it instead of spinning.
        if (progress) {
            idle_rounds = 0;
        } else if (++idle_rounds >= k_idle_round_limit) {
            return false;
        }
    }
    return true;
}

// --- BC1 ---------------------------------------------------------------------
// Record: [0] endpoint ref (zigzag, relative to [1]), [1] index ref distance,
// [2..7] R0 G0 B0 R1 G1 B1 deltas, [8..11] index quads.

static inline u32 color565(u32 r, u32 g, u32 b) {
    return (unzigzag(r) & 31) << 11 | (unzigzag(g) & 63) << 5 | (unzigzag(b) & 31);
}

static inline u32 quads(const u16* table, const u8* q) {
    return table[q[0]] | (u32)table[q[1]] << 4 | (u32)table[q[2]] << 16 | (u32)table[q[3]] << 20;
}

static const u64 k_high_565x2 = 0x84108410ull;
static const u64 k_high_2bit = 0xAAAAAAAAAAAAAAAAull;
static const u64 k_high_3bit = 0x0000924924924924ull;
static const u64 k_high_4bit = 0x8888888888888888ull;

static bool decode_bc1(SymbolReader* r, u8* dst, u64 block_count) {
    static const Bc1Work zero = {0, 0};
    Bc1Work ring[256];
    u8 records[k_batch_blocks * 12];
    for (u64 first = 0; first < block_count; first += k_batch_blocks) {
        u32 n = block_count - first < k_batch_blocks ? (u32)(block_count - first) : k_batch_blocks;
        if (!read_records(r, records, n))
            return false;
        for (u32 i = 0; i < n; i++) {
            const u8* rec = records + i * 12;
            u64 block = first + i;
            References refs = references_direct(rec);
            const Bc1Work* ep = pick_reference(block, refs.endpoint_distance, ring, k_predef_bc1, &zero);
            const Bc1Work* ix = pick_reference(block, refs.index_distance, ring, k_predef_bc1, &zero);
            Bc1Work w;
            u32 color = color565(rec[2], rec[3], rec[4]) | color565(rec[5], rec[6], rec[7]) << 16;
            w.color = (u32)lane_add(color, ep->color, k_high_565x2);
            w.index = (u32)lane_add(quads(k_quad_bc1, rec + 8), ix->index, k_high_2bit);
            ring[block & 255] = w;
            u8* out = dst + block * 8;
            store_u32(out, w.color);
            store_u32(out + 4, (u32)remap_index2(w.index, k_lsb2));
        }
    }
    return true;
}

// --- BC2 ---------------------------------------------------------------------
// Record: [0..3] refs, [4..19] alpha deltas per texel, [20..25] R0 R1 G0 G1
// B0 B1 deltas, [26..29] index quads.

static bool decode_bc2(SymbolReader* r, u8* dst, u64 block_count) {
    static const Bc2Work zero = {0, 0, 0};
    Bc2Work ring[256];
    u8 records[k_batch_blocks * 30];
    for (u64 first = 0; first < block_count; first += k_batch_blocks) {
        u32 n = block_count - first < k_batch_blocks ? (u32)(block_count - first) : k_batch_blocks;
        if (!read_records(r, records, n))
            return false;
        for (u32 i = 0; i < n; i++) {
            const u8* rec = records + i * 30;
            u64 block = first + i;
            References refs = references_split(rec);
            const Bc2Work* ep = pick_reference(block, refs.endpoint_distance, ring, k_predef_bc2, &zero);
            const Bc2Work* ix = pick_reference(block, refs.index_distance, ring, k_predef_bc2, &zero);
            u64 alpha = 0;
            for (u32 t = 0; t < 16; t++)
                alpha |= (u64)(unzigzag(rec[4 + t]) & 15) << (4 * t);
            u32 color = color565(rec[20], rec[22], rec[24]) | color565(rec[21], rec[23], rec[25]) << 16;
            Bc2Work w;
            w.alpha = lane_add(alpha, ep->alpha, k_high_4bit);
            w.color = (u32)lane_add(color, ep->color, k_high_565x2);
            w.index = (u32)lane_add(quads(k_quad_bc2, rec + 26), ix->index, k_high_2bit);
            ring[block & 255] = w;
            // alpha values are centered: 0 means 8
            u8* out = dst + block * 16;
            store_u64(out, w.alpha ^ 0x8888888888888888ull);
            store_u32(out + 8, w.color);
            store_u32(out + 12, (u32)remap_index2(w.index, k_lsb2));
        }
    }
    return true;
}

// --- BC3 ---------------------------------------------------------------------
// Record: [0..3] refs, [4] [5] alpha endpoint deltas, [6..13] alpha index
// pairs, [14..19] R0 R1 G0 G1 B0 B1 deltas, [20..23] color index quads.

static inline u64 pairs(const u8* table, const u8* p) {
    u64 v = 0;
    for (u32 k = 0; k < 8; k++)
        v |= (u64)table[p[k]] << (6 * k);
    return v;
}

static bool decode_bc3(SymbolReader* r, u8* dst, u64 block_count) {
    static const Bc3Work zero = {0, 0, 0};
    static const u64 high = 0x0000808084108410ull;
    Bc3Work ring[256];
    u8 records[k_batch_blocks * 24];
    for (u64 first = 0; first < block_count; first += k_batch_blocks) {
        u32 n = block_count - first < k_batch_blocks ? (u32)(block_count - first) : k_batch_blocks;
        if (!read_records(r, records, n))
            return false;
        for (u32 i = 0; i < n; i++) {
            const u8* rec = records + i * 24;
            u64 block = first + i;
            References refs = references_split(rec);
            const Bc3Work* ep = pick_reference(block, refs.endpoint_distance, ring, k_predef_bc3, &zero);
            const Bc3Work* ix = pick_reference(block, refs.index_distance, ring, k_predef_bc3, &zero);
            u64 endpoints = color565(rec[14], rec[16], rec[18]) |
                            (u64)color565(rec[15], rec[17], rec[19]) << 16 |
                            (u64)(unzigzag(rec[4]) & 255) << 32 | (u64)(unzigzag(rec[5]) & 255) << 40;
            Bc3Work w;
            w.endpoints = lane_add(endpoints, ep->endpoints, high);
            w.alpha_index = lane_add(pairs(k_pair_bc3, rec + 6), ix->alpha_index, k_high_3bit);
            w.color_index = (u32)lane_add(quads(k_quad_bc3, rec + 20), ix->color_index, k_high_2bit);
            ring[block & 255] = w;
            // alpha endpoints are centered: 0 means 128
            u8* out = dst + block * 16;
            store_u64(out, ((w.endpoints >> 32) ^ 0x8080) | remap_index3(w.alpha_index, k_lsb3) << 16);
            store_u32(out + 8, (u32)w.endpoints);
            store_u32(out + 12, (u32)remap_index2(w.color_index, k_lsb2));
        }
    }
    return true;
}

// --- BC4 ---------------------------------------------------------------------
// Record: [0] [1] refs, [2] [3] endpoint deltas, [4..11] index pairs.

static bool decode_bc4(SymbolReader* r, u8* dst, u64 block_count) {
    static const Bc4Work zero = {0, 0};
    Bc4Work ring[256];
    u8 records[k_batch_blocks * 12];
    for (u64 first = 0; first < block_count; first += k_batch_blocks) {
        u32 n = block_count - first < k_batch_blocks ? (u32)(block_count - first) : k_batch_blocks;
        if (!read_records(r, records, n))
            return false;
        for (u32 i = 0; i < n; i++) {
            const u8* rec = records + i * 12;
            u64 block = first + i;
            References refs = references_direct(rec);
            const Bc4Work* ep = pick_reference(block, refs.endpoint_distance, ring, k_predef_bc4, &zero);
            const Bc4Work* ix = pick_reference(block, refs.index_distance, ring, k_predef_bc4, &zero);
            u32 endpoints = (unzigzag(rec[2]) & 255) | (unzigzag(rec[3]) & 255) << 8;
            Bc4Work w;
            w.endpoints = (u16)lane_add(endpoints, ep->endpoints, 0x8080);
            w.index = lane_add(pairs(k_pair_bc4, rec + 4), ix->index, k_high_3bit);
            ring[block & 255] = w;
            store_u64(dst + block * 8, (u64)(w.endpoints ^ 0x8080u) | remap_index3(w.index, k_lsb3) << 16);
        }
    }
    return true;
}

// --- BC5 ---------------------------------------------------------------------
// Record: [0..3] refs, [4..7] R0 R1 G0 G1 deltas, [8..15] red index pairs,
// [16..23] green index pairs.

static bool decode_bc5(SymbolReader* r, u8* dst, u64 block_count) {
    static const Bc5Work zero = {{0, 0}, 0};
    Bc5Work ring[256];
    u8 records[k_batch_blocks * 24];
    for (u64 first = 0; first < block_count; first += k_batch_blocks) {
        u32 n = block_count - first < k_batch_blocks ? (u32)(block_count - first) : k_batch_blocks;
        if (!read_records(r, records, n))
            return false;
        for (u32 i = 0; i < n; i++) {
            const u8* rec = records + i * 24;
            u64 block = first + i;
            References refs = references_split(rec);
            const Bc5Work* ep = pick_reference(block, refs.endpoint_distance, ring, k_predef_bc5, &zero);
            const Bc5Work* ix = pick_reference(block, refs.index_distance, ring, k_predef_bc5, &zero);
            u32 endpoints = (unzigzag(rec[4]) & 255) | (unzigzag(rec[5]) & 255) << 8 |
                            (unzigzag(rec[6]) & 255) << 16 | (unzigzag(rec[7]) & 255) << 24;
            Bc5Work w;
            w.endpoints = (u32)lane_add(endpoints, ep->endpoints, 0x80808080);
            w.index[0] = lane_add(pairs(k_pair_bc5, rec + 8), ix->index[0], k_high_3bit);
            w.index[1] = lane_add(pairs(k_pair_bc5, rec + 16), ix->index[1], k_high_3bit);
            ring[block & 255] = w;
            u8* out = dst + block * 16;
            store_u64(out, (u64)((w.endpoints & 0xFFFF) ^ 0x8080) | remap_index3(w.index[0], k_lsb3) << 16);
            store_u64(out + 8, (u64)((w.endpoints >> 16) ^ 0x8080) | remap_index3(w.index[1], k_lsb3) << 16);
        }
    }
    return true;
}

// --- BC6H --------------------------------------------------------------------
// Record: [0..3] refs; [4..9] e0 of R G B (each two symbols, bit-interleaved);
// [10..15] e1 of R G B (same); [16..21] e2/e3 of R, G, B (two-region modes);
// [22..29] index pairs (bit-interleaved); [30] mode - 1; [31] shape.

// Index sets of BC6H/BC7 blocks (16 signed values, one byte each).
//
// Deltas: eight symbols carry two 4-bit zigzag values each, one in the odd
// and one in the even bits.  The first six symbols list the odd-bit value
// first, the last two the even-bit value first.
//
// Adding a reference: both are aligned to the top of 16 bits (current width
// `bits`, reference width `ref_bits`), added, and shifted back by the current
// width.  That wraps at the current width and rescales a reference of a
// different width.  Computed for all 16 texels at once with SSE2.
static void add_index_set(s8* out, const u8* first_six, const u8* last_two, u32 bits, const s8* ref,
                          u32 ref_bits) {
    u64 sym = 0;
    memcpy(&sym, first_six, 6);
    sym |= (u64)last_two[0] << 48 | (u64)last_two[1] << 56;
    // compact the even and the odd bits of every byte into its low nibble
    u64 even = sym & 0x5555555555555555ull;
    even = (even | even >> 1) & 0x3333333333333333ull;
    even = (even | even >> 2) & 0x0F0F0F0F0F0F0F0Full;
    u64 odd = (sym >> 1) & 0x5555555555555555ull;
    odd = (odd | odd >> 1) & 0x3333333333333333ull;
    odd = (odd | odd >> 2) & 0x0F0F0F0F0F0F0F0Full;
    u64 swap = (odd ^ even) & 0xFFFF000000000000ull; // last two symbols: even first
    __m128i first = _mm_cvtsi64_si128((s64)(odd ^ swap));
    __m128i second = _mm_cvtsi64_si128((s64)(even ^ swap));
    __m128i n = _mm_unpacklo_epi8(first, second); // texel order
    // unzigzag each byte: (n >> 1) ^ -(n & 1)
    __m128i half = _mm_and_si128(_mm_srli_epi16(n, 1), _mm_set1_epi8(7));
    __m128i sign = _mm_sub_epi8(_mm_setzero_si128(), _mm_and_si128(n, _mm_set1_epi8(1)));
    __m128i cur = _mm_xor_si128(half, sign);
    __m128i r = _mm_loadu_si128((const __m128i*)ref);
    // sign-extend bytes to 16 bits: duplicate each byte, arithmetic shift down
    __m128i cur_lo = _mm_srai_epi16(_mm_unpacklo_epi8(cur, cur), 8);
    __m128i cur_hi = _mm_srai_epi16(_mm_unpackhi_epi8(cur, cur), 8);
    __m128i ref_lo = _mm_srai_epi16(_mm_unpacklo_epi8(r, r), 8);
    __m128i ref_hi = _mm_srai_epi16(_mm_unpackhi_epi8(r, r), 8);
    __m128i up = _mm_cvtsi32_si128((int)(16 - bits));
    __m128i ref_up = _mm_cvtsi32_si128((int)(16 - ref_bits));
    __m128i lo = _mm_sra_epi16(_mm_add_epi16(_mm_sll_epi16(cur_lo, up), _mm_sll_epi16(ref_lo, ref_up)), up);
    __m128i hi = _mm_sra_epi16(_mm_add_epi16(_mm_sll_epi16(cur_hi, up), _mm_sll_epi16(ref_hi, ref_up)), up);
    _mm_storeu_si128((__m128i*)out, _mm_packs_epi16(lo, hi));
}

// Packs 16 index values of `bits` bits (low bits of each byte after adding
// `bias`) into one word, texel 0 in the lowest bits.
static u64 pack_index_set(const s8* index, u32 bits, u32 bias) {
    u64 half[2];
    u64 lane = 0x0101010101010101ull * ((1u << bits) - 1);
    for (u32 h = 0; h < 2; h++) {
        u64 x;
        memcpy(&x, index + 8 * h, 8);
        x = lane_add(x, 0x0101010101010101ull * bias, 0x8080808080808080ull) & lane;
        x = (x & 0x00FF00FF00FF00FFull) | ((x >> 8) & 0x00FF00FF00FF00FFull) << bits;
        x = (x & 0x0000FFFF0000FFFFull) | ((x >> 16) & 0x0000FFFF0000FFFFull) << (2 * bits);
        x = (x & 0x00000000FFFFFFFFull) | (x >> 32) << (4 * bits);
        half[h] = x;
    }
    return half[0] | half[1] << (8 * bits);
}

static inline u64 put_block_bits(u64* hi, u64 lo, u64 v, u32 dst) {
    if (dst >= 64) {
        *hi |= v << (dst - 64);
        return lo;
    }
    if (dst)
        *hi |= v >> (64 - dst);
    return lo | v << dst;
}

static bool decode_bc6(SymbolReader* r, u8* dst, u64 block_count) {
    Bc6Endpoints ep_ring[256];
    Bc6Indices ix_ring[256];
    u8 records[k_batch_blocks * 32];
    for (u64 first = 0; first < block_count; first += k_batch_blocks) {
        u32 n = block_count - first < k_batch_blocks ? (u32)(block_count - first) : k_batch_blocks;
        if (!read_records(r, records, n))
            return false;
        for (u32 i = 0; i < n; i++) {
            const u8* rec = records + i * 32;
            u64 block = first + i;
            u32 mode = rec[30] + 1u;
            if (mode > 14)
                return false; // reserved mode: not produced by encoders
            References refs = references_split(rec);
            const Bc6Endpoints* ref_ep =
                pick_reference(block, refs.endpoint_distance, ep_ring, k_predef_ep_bc6, &k_zero_ep_bc6);
            const Bc6Indices* ref_ix =
                pick_reference(block, refs.index_distance, ix_ring, k_predef_ix_bc6, &k_zero_ix_bc6);
            bool two_regions = mode < 11;
            const u8* prec = k_bc6_precision[mode];
            u32 sh[4] = {16u - prec[0], 16u - prec[1], 16u - prec[2], 16u - prec[3]};

            // current deltas, left-aligned to the mode's precisions
            u16 cur[12];
            for (u32 c = 0; c < 3; c++) {
                cur[c] = (u16)(unzigzag(merge_bits(rec[4 + 2 * c], rec[5 + 2 * c])) << sh[0]);
                cur[3 + 3 * c] = (u16)(unzigzag(merge_bits(rec[10 + 2 * c], rec[11 + 2 * c])) << sh[1 + c]);
                if (two_regions) {
                    cur[4 + 3 * c] = (u16)(unzigzag(rec[16 + 2 * c]) << sh[1 + c]);
                    cur[5 + 3 * c] = (u16)(unzigzag(rec[17 + 2 * c]) << sh[1 + c]);
                }
            }

            // endpoints = current + reference, truncated to the precisions
            Bc6Endpoints e;
            e.mode = (u8)mode;
            e.shape = rec[31];
            for (u32 c = 0; c < 3; c++) {
                e.v[c] = (u16)((cur[c] + ref_ep->v[c]) & (0xFFFFu << sh[0]));
                u32 mask = 0xFFFFu << sh[1 + c];
                u32 last = two_regions ? 3 : 1;
                for (u32 k = 0; k < last; k++)
                    e.v[3 + 3 * c + k] = (u16)((cur[3 + 3 * c + k] + ref_ep->v[3 + 3 * c + k]) & mask);
                if (!two_regions) {
                    // one region: e2/e3 restate e0/e1 so the layout stays uniform
                    e.v[4 + 3 * c] = (u16)(((s32)(s16)e.v[c] >> sh[0]) << sh[1]);
                    e.v[5 + 3 * c] = e.v[3 + 3 * c];
                }
            }
            ep_ring[block & 255] = e;

            // indices = current + reference
            Bc6Indices x;
            x.mode = (u8)mode;
            u32 bits = k_bc6_index_bits[mode];
            add_index_set(x.index, rec + 22, rec + 28, bits, ref_ix->index,
                          k_bc6_index_bits[ref_ix->mode & 15]);
            ix_ring[block & 255] = x;

            // block fields: shape, endpoints at their precisions, packed indices
            u64 field[14];
            field[0] = e.shape;
            u32 count = two_regions ? 4 : 2;
            for (u32 c = 0; c < 3; c++) {
                field[1 + 4 * c] = (u16)(((s32)(s16)e.v[c] ^ 0x8000) >> sh[0]);
                for (u32 k = 1; k < 4; k++) {
                    s32 v = (s32)(s16)e.v[3 + 3 * c + k - 1];
                    field[1 + 4 * c + k] = k < count ? (u16)((v ^ 0x8000) >> sh[1 + c]) : 0;
                }
            }
            // indices are centered: texel index = value + 2^(bits-1)
            u64 packed = pack_index_set(x.index, bits, 1u << (bits - 1));
            // anchor texels drop their top index bit
            if (two_regions) {
                u64 keep = (4ull << (3 * k_bc6_anchor[e.shape & 31])) - 1;
                packed = (packed & keep) | ((packed >> 1) & ~keep);
                packed = (packed & 3) | ((packed >> 1) & ~3ull);
            } else {
                packed = (packed & 7) | ((packed >> 1) & ~7ull);
            }
            field[13] = packed;

            u64 lo = k_bc6_mode_bits[mode];
            u64 hi = 0;
            for (u32 k = k_bc6_run_start[mode]; k < k_bc6_run_start[mode + 1]; k++) {
                const Bc6Run* run = &k_bc6_runs[k];
                u64 v = (field[run->field] >> run->src) & ((1ull << run->len) - 1);
                if (run->reversed) {
                    u64 flipped = 0;
                    for (u32 j = 0; j < run->len; j++)
                        flipped |= ((v >> j) & 1) << (run->len - 1 - j);
                    v = flipped;
                }
                lo = put_block_bits(&hi, lo, v, run->dst);
            }
            u8* out = dst + block * 16;
            store_u64(out, lo);
            store_u64(out + 8, hi);
        }
    }
    return true;
}

// --- BC7 ---------------------------------------------------------------------
// Record: [0..3] refs; [4..21] endpoint deltas (layout depends on the subset
// count); [22..29] index set 0 (bit-interleaved pairs); [12..17] [20] [21]
// index set 1 in modes 4/5; [30] [31] configuration (mode, partition, ...).

struct BitWriter {
    u64 lo;
    u64 hi;
    u32 pos;
};

static inline void put_bits(BitWriter* w, u64 v, u32 count) {
    v &= (1ull << count) - 1;
    w->lo = put_block_bits(&w->hi, w->lo, v, w->pos);
    w->pos += count;
}

// Packs index set (bits per texel) and drops the top bit of each anchor texel.
static void put_indices(BitWriter* w, const s8* index, u32 bits, u32 subsets, u32 partition) {
    u64 v = pack_index_set(index, bits, 0);
    if (subsets >= 2) {
        // with three subsets the third subset's anchor (never below the
        // second's in the table) is removed first, so the second's position
        // is still valid afterwards
        const u8* anchor = subsets == 2 ? k_bc7_anchor[0] : k_bc7_anchor[2];
        u64 keep = (1ull << (bits * (anchor[partition & 63] + 1u) - 1)) - 1;
        v = (v & keep) | ((v >> 1) & ~keep);
        if (subsets == 3) {
            keep = (1ull << (bits * (k_bc7_anchor[1][partition & 63] + 1u) - 1)) - 1;
            v = (v & keep) | ((v >> 1) & ~keep);
        }
    }
    u64 keep = (1ull << (bits - 1)) - 1;
    v = (v & keep) | ((v >> 1) & ~keep);
    put_bits(w, v, 16 * bits - subsets);
}

static bool decode_bc7(SymbolReader* r, u8* dst, u64 block_count) {
    // index bits of set 0 and set 1 per mode
    static const u8 index_bits[8][2] = {{3, 0}, {3, 0}, {2, 0}, {2, 0}, {3, 2}, {2, 2}, {4, 0}, {2, 0}};
    // endpoint bits of a color / alpha channel in the block, per mode
    static const u8 color_bits[8] = {4, 6, 5, 7, 5, 7, 7, 5};
    static const u8 alpha_bits[8] = {0, 0, 0, 0, 6, 8, 7, 5};
    static const Bc7Endpoints zero_ep = {};
    static const Bc7Indices zero_ix = {};
    Bc7Endpoints ep_ring[256];
    Bc7Indices ix_ring[256];
    u8 records[k_batch_blocks * 32];
    for (u64 first = 0; first < block_count; first += k_batch_blocks) {
        u32 n = block_count - first < k_batch_blocks ? (u32)(block_count - first) : k_batch_blocks;
        if (!read_records(r, records, n))
            return false;
        for (u32 i = 0; i < n; i++) {
            const u8* rec = records + i * 32;
            u64 block = first + i;
            u32 slot = rec[30] | (u32)rec[31] << 5;
            u32 config = slot < 512 ? k_bc7_config[slot] : 0;
            if (config == 0)
                return false; // no BC7 configuration for this symbol pair
            u32 mode = config & 7;
            u32 rotation = (config >> 3) & 3;
            u32 subsets = (config >> 6) & 3;
            u32 partition = config >> 8;
            const u8* prec = k_bc7_precision[config & 31];
            References refs = references_split(rec);
            const Bc7Endpoints* ref_ep =
                pick_reference(block, refs.endpoint_distance, ep_ring, k_predef_ep_bc7, &zero_ep);
            const Bc7Indices* ref_ix =
                pick_reference(block, refs.index_distance, ix_ring, k_predef_ix_bc7, &zero_ix);

            // endpoint deltas: which record byte feeds which subset/channel
            u8 raw[3][8];
            if (subsets == 3) {
                const u8 s0[8] = {rec[4], rec[5], rec[6], rec[7], rec[8], rec[9], 0, 0};
                const u8 s1[8] = {rec[12], rec[13], rec[14], rec[15], rec[16], rec[17], 0, 0};
                const u8 s2[8] = {rec[20], rec[21], rec[10], rec[11], rec[18], rec[19], 0, 0};
                memcpy(raw[0], s0, 8);
                memcpy(raw[1], s1, 8);
                memcpy(raw[2], s2, 8);
            } else {
                memcpy(raw[0], rec + 4, 8);
                memcpy(raw[1], rec + 12, 8);
            }

            // endpoints = current + reference (reference masked to the current
            // precision; a reference with fewer subsets lends its first subset)
            Bc7Endpoints e;
            memset(&e, 0, sizeof(e));
            e.config = (u16)config;
            memcpy(e.precision, prec, 4);
            u32 ref_subsets = (ref_ep->config >> 6) & 3;
            u32 shared = subsets < ref_subsets ? subsets : ref_subsets;
            u32 channels = prec[3] ? 8 : 6;
            for (u32 c = 0; c < channels; c++) {
                u32 shift = 16u - prec[c >> 1];
                u32 mask = 0xFFFFu << shift;
                for (u32 s = 0; s < subsets; s++) {
                    u32 cur = unzigzag(raw[s][c]) << shift;
                    u32 ref = ref_ep->v[s < shared ? s : 0][c];
                    e.v[s][c] = (u16)(cur + (ref & mask));
                }
            }
            ep_ring[block & 255] = e;

            // indices = current + reference
            Bc7Indices x;
            u32 bits0 = index_bits[mode][0];
            u32 bits1 = index_bits[mode][1];
            x.bits[0] = (u8)bits0;
            x.bits[1] = (u8)bits1;
            add_index_set(x.index[0], rec + 22, rec + 28, bits0, ref_ix->index[0], ref_ix->bits[0]);
            if (bits1) {
                add_index_set(x.index[1], rec + 12, rec + 20, bits1, ref_ix->index[1], ref_ix->bits[1]);
            } else {
                memset(x.index[1], 0, 16);
            }
            ix_ring[block & 255] = x;

            // channel bit strings, endpoints in subset order
            u64 channel[4] = {0, 0, 0, 0};
            u32 pbits = 0;
            u32 count = 2 * subsets;
            bool has_pbits = mode == 0 || mode == 3 || mode == 6 || mode == 7;
            if (has_pbits || mode == 1) {
                // the p-bit rides as the low bit of the red value
                u32 shift = 16u - prec[0];
                if (mode == 1) {
                    pbits = (e.v[0][0] >> shift & 1) | (e.v[1][0] >> shift & 1) << 1;
                } else {
                    for (u32 j = count; j-- > 0;)
                        pbits = pbits << 1 | (e.v[j >> 1][j & 1] >> shift & 1);
                }
                for (u32 j = 0; j < count; j++)
                    channel[0] |= (u64)(e.v[j >> 1][j & 1] >> shift >> 1) << (j * (prec[0] - 1u));
            } else {
                for (u32 j = 0; j < count; j++)
                    channel[0] |= (u64)(e.v[j >> 1][j & 1] >> (16u - prec[0])) << (j * prec[0]);
            }
            for (u32 c = 1; c < 4; c++) {
                for (u32 j = 0; j < count; j++)
                    channel[c] |= (u64)(e.v[j >> 1][2 * c + (j & 1)] >> (16u - prec[c])) << (j * prec[c]);
            }
            if (rotation) {
                // rotation modes swap alpha with one color channel
                u64 t = channel[3];
                channel[3] = channel[rotation - 1];
                channel[rotation - 1] = t;
            }

            BitWriter w = {0, 0, 0};
            put_bits(&w, 1ull << mode, mode + 1);
            if (mode == 0) {
                put_bits(&w, partition, 4);
            } else if (mode == 4) {
                put_bits(&w, rotation, 2);
                put_bits(&w, (config >> 5) & 1, 1);
            } else if (mode == 5) {
                put_bits(&w, rotation, 2);
            } else if (mode != 6) {
                put_bits(&w, partition, 6);
            }
            u32 cw = color_bits[mode] * count;
            put_bits(&w, channel[0], cw);
            put_bits(&w, channel[1], cw);
            put_bits(&w, channel[2], cw);
            if (alpha_bits[mode])
                put_bits(&w, channel[3], alpha_bits[mode] * count);
            if (has_pbits)
                put_bits(&w, pbits, count);
            else if (mode == 1)
                put_bits(&w, pbits, 2);
            if (mode == 4) {
                put_indices(&w, x.index[1], 2, subsets, partition);
                put_indices(&w, x.index[0], 3, subsets, partition);
            } else if (mode == 5) {
                put_indices(&w, x.index[0], 2, subsets, partition);
                put_indices(&w, x.index[1], 2, subsets, partition);
            } else {
                put_indices(&w, x.index[0], bits0, subsets, partition);
            }
            u8* out = dst + block * 16;
            store_u64(out, w.lo);
            store_u64(out + 8, w.hi);
        }
    }
    return true;
}

// --- entry point -------------------------------------------------------------

bool bcpack_decode(u32 bc_format, const void* src, u64 src_size, void* dst, u64 dst_size) {
    if (bc_format < 1 || bc_format > 7)
        return false;
    u64 block_size = (bc_format == 1 || bc_format == 4) ? 8 : 16;
    if (dst_size % block_size != 0)
        return false;
    if ((!src && src_size) || (!dst && dst_size))
        return false;
    u64 block_count = dst_size / block_size;
    if (block_count == 0)
        return true;
    // lanes count symbols in 32 bits
    if (block_count > 0x7FFFFFFF)
        return false;

    SymbolReader reader;
    const u8* in = (const u8*)src;
    u8* out = (u8*)dst;
    switch (bc_format) {
    case 1:
        reader_init(&reader, k_lane_class_bc1, 12, 0, in, src_size, block_count);
        return decode_bc1(&reader, out, block_count);
    case 2:
        reader_init(&reader, k_lane_class_bc2, 15, 1, in, src_size, block_count);
        return decode_bc2(&reader, out, block_count);
    case 3:
        reader_init(&reader, k_lane_class_bc3, 12, 1, in, src_size, block_count);
        return decode_bc3(&reader, out, block_count);
    case 4:
        reader_init(&reader, k_lane_class_bc4, 12, 0, in, src_size, block_count);
        return decode_bc4(&reader, out, block_count);
    case 5:
        reader_init(&reader, k_lane_class_bc5, 12, 1, in, src_size, block_count);
        return decode_bc5(&reader, out, block_count);
    case 6:
        reader_init(&reader, k_lane_class_bc6, 16, 1, in, src_size, block_count);
        return decode_bc6(&reader, out, block_count);
    default:
        reader_init(&reader, k_lane_class_bc7, 16, 1, in, src_size, block_count);
        return decode_bc7(&reader, out, block_count);
    }
}
