// XMA frame decoder (see xma_decoder.h).
//
// Based on FFmpeg libavcodec/wmaprodec.c and wma.c (run/level decoding), libavutil/float_dsp.c
// (windowed overlap-add) and libavcodec/sinewin_tablegen.h:
//   Copyright (c) 2007 Baptiste Coudurier, Benjamin Larsson, Ulion
//   Copyright (c) 2008 - 2011 Sascha Sommer, Benjamin Larsson
//   Copyright (c) 2000, 2001 Fabrice Bellard
//   Copyright (c) 2005 Balatoni Denes
//   Copyright (c) 2006 Loren Merritt
//
// This file is free software; you can redistribute it and/or modify it under the terms of the GNU
// Lesser General Public License as published by the Free Software Foundation; either version 2.1
// of the License, or (at your option) any later version.
//
// It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
// implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser
// General Public License for more details.
//
// Changes from the original: reduced to what XMA uses (512-sample frames of at most four
// subframes, one or two channels, frames prefixed with their 15-bit length, dynamic range control
// data present), so multichannel decorrelation and the subwoofer cutoff are gone; packet parsing is
// done by the caller (xma.cpp); own bit reader, Huffman decoder and inverse MDCT; output is 16-bit
// interleaved samples.
//
// A frame holds 512 samples per channel, split into subframes (blocks) of 512, 256 or 128 samples,
// possibly differently per channel. Each block carries scale factors per frequency band, a
// quantization step and Huffman-coded coefficients (vectors of 4, 2 and 1 values, then run/level
// pairs); stereo blocks may be mid/side coded per band. Coefficients are scaled, run through an
// inverse MDCT and overlap-added with the previous block through a sine window.

#include <intrin.h>
#include <math.h>
#include <string.h>

#include "hle/audio/xma_decoder.h"
#include "hle/audio/xma_tables.h"

#define FRAME           XMA_FRAME_SAMPLES
#define MIN_BLOCK       128 // smallest subframe
#define BLOCK_SIZES     3   // 512, 256, 128
#define LENGTH_BITS     15
#define QUANT_STEP_BASE 90 // 90 * 16 bits per sample >> 4

static u32 log2_floor(u32 v) {
    unsigned long i;
    return _BitScanReverse(&i, v) ? (u32)i : 0;
}

// --- bit reader ----------------------------------------------------------------------------------

struct Bits {
    const u8* data;
    u32 size; // readable bytes, padding included
    u32 pos;  // in bits
    u32 end;  // end of the frame in bits
};

// The next 64 bits, MSB first. Positions past the padding (only reachable through corrupt data)
// read as zeros.
static u64 peek(const Bits& b) {
    u32 byte = b.pos >> 3;
    if (byte + 8 > b.size)
        return 0;
    u64 v;
    memcpy(&v, b.data + byte, 8);
    return _byteswap_uint64(v) << (b.pos & 7);
}

static u32 get_bits(Bits& b, u32 n) { // n <= 32
    if (!n)
        return 0;
    u32 v = (u32)(peek(b) >> (64 - n));
    b.pos += n;
    return v;
}

static u32 get_bit(Bits& b) {
    return get_bits(b, 1);
}

static s32 get_sbits(Bits& b, u32 n) {
    return (s32)(get_bits(b, n) << (32 - n)) >> (32 - n);
}

// Escaped values: 8, 16, 24 or 31 bits.
static u32 get_large_value(Bits& b) {
    u32 n = 8;
    if (get_bit(b)) {
        n += 8;
        if (get_bit(b)) {
            n += 8;
            if (get_bit(b))
                n += 7;
        }
    }
    return get_bits(b, n);
}

// --- Huffman codes ---------------------------------------------------------------------------------
//
// The tables list codes by length in tree order, so codes are assigned sequentially: each one is
// the previous plus one unit of its own length, left-aligned in 32 bits. Decoding finds the last
// code not above the next 32 input bits: the first 9 bits narrow it to a range (usually one code),
// a binary search does the rest.

#define VLC_MAX   272
#define VLC_INDEX 9

struct Vlc {
    u32 code[VLC_MAX];
    s32 symbol[VLC_MAX];
    u8 len[VLC_MAX];
    u16 first[1 << VLC_INDEX]; // candidate range per 9-bit prefix
    u16 last[1 << VLC_INDEX];
    u64 end; // first value past the last code
};

static void vlc_init(Vlc* v, u32 count, const u8* lens, u32 lens_stride, const u8* syms8, const u16* syms16,
                     u32 syms_stride, s32 offset) {
    u64 code = 0;
    for (u32 i = 0; i < count; i++) {
        v->code[i] = (u32)code;
        v->len[i] = lens[i * lens_stride];
        v->symbol[i] = (s32)(syms16 ? syms16[i * syms_stride] : syms8[i * syms_stride]) + offset;
        code += 1ull << (32 - v->len[i]);
    }
    v->end = code;
    u32 lo = 0, hi = 0;
    for (u32 p = 0; p < (1u << VLC_INDEX); p++) {
        u32 first = p << (32 - VLC_INDEX), last = first | ((1u << (32 - VLC_INDEX)) - 1);
        while (lo + 1 < count && v->code[lo + 1] <= first)
            lo++;
        if (hi < lo)
            hi = lo;
        while (hi + 1 < count && v->code[hi + 1] <= last)
            hi++;
        v->first[p] = (u16)lo;
        v->last[p] = (u16)hi;
    }
}

// Returns the symbol, or -1 for bits that are no code (consuming nothing).
static s32 vlc_get(Bits& b, const Vlc& v) {
    u32 bits = (u32)(peek(b) >> 32);
    if (bits >= v.end)
        return -1;
    u32 lo = v.first[bits >> (32 - VLC_INDEX)], hi = v.last[bits >> (32 - VLC_INDEX)];
    while (lo < hi) {
        u32 mid = (lo + hi + 1) / 2;
        if (v.code[mid] <= bits)
            lo = mid;
        else
            hi = mid - 1;
    }
    b.pos += v.len[lo];
    return v.symbol[lo];
}

static Vlc g_sf_vlc, g_sf_rl_vlc, g_coef_vlc[2], g_vec4_vlc, g_vec2_vlc, g_vec1_vlc;

// --- inverse MDCT ----------------------------------------------------------------------------------
//
// The half inverse MDCT of n coefficients (n output samples: the middle half of the full 2n, the
// rest follows by symmetry), through an n/4-point complex FFT between two rotations.

struct Imdct {
    u32 n;
    f32 scale;
    u16 rev[FRAME / 2];
    f32 tcos[FRAME / 2], tsin[FRAME / 2];
    f32 wre[FRAME / 4], wim[FRAME / 4];
};

static Imdct g_imdct[BLOCK_SIZES];       // by log2(n) - 7
static f32 g_window[BLOCK_SIZES][FRAME]; // sine windows of 128, 256 and 512 samples

static void imdct_init(Imdct* t, u32 n) {
    const f64 pi = 3.14159265358979323846;
    u32 m = n / 2, bits = log2_floor(m);
    t->n = n;
    t->scale = 2.0f / (f32)n; // FFmpeg's 1 / (n / 2) / 32768, times 32768 for 16-bit output
    for (u32 k = 0; k < m; k++) {
        u32 r = 0;
        for (u32 i = 0; i < bits; i++)
            r |= ((k >> i) & 1) << (bits - 1 - i);
        t->rev[k] = (u16)r;
        f64 alpha = 2.0 * pi * ((f64)k + 0.125) / (2.0 * n);
        t->tcos[k] = (f32)-cos(alpha);
        t->tsin[k] = (f32)-sin(alpha);
    }
    for (u32 k = 0; k < m / 2; k++) {
        t->wre[k] = (f32)cos(2.0 * pi * k / m);
        t->wim[k] = (f32)sin(2.0 * pi * k / m);
    }
}

// In-place radix-2 FFT (positive exponent) of m points given in bit-reversed order.
static void fft(const Imdct& t, f32* re, f32* im, u32 m) {
    for (u32 size = 2; size <= m; size *= 2) {
        u32 half = size / 2, step = m / size;
        for (u32 start = 0; start < m; start += size) {
            for (u32 k = 0; k < half; k++) {
                f32 wr = t.wre[k * step], wi = t.wim[k * step];
                u32 a = start + k, b = a + half;
                f32 br = re[b] * wr - im[b] * wi, bi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - br;
                im[b] = im[a] - bi;
                re[a] += br;
                im[a] += bi;
            }
        }
    }
}

static void imdct_half(const Imdct& t, f32* out, const f32* in) {
    u32 n = t.n, m = n / 2, m2 = n / 4;
    f32 re[FRAME / 2], im[FRAME / 2];
    for (u32 k = 0; k < m; k++) {
        f32 a = in[n - 1 - 2 * k], b = in[2 * k];
        u32 j = t.rev[k];
        re[j] = a * t.tcos[k] - b * t.tsin[k];
        im[j] = a * t.tsin[k] + b * t.tcos[k];
    }
    fft(t, re, im, m);
    for (u32 k = 0; k < m2; k++) {
        u32 a = m2 - k - 1, b = m2 + k;
        f32 r0 = im[a] * t.tsin[a] - re[a] * t.tcos[a];
        f32 i1 = im[a] * t.tcos[a] + re[a] * t.tsin[a];
        f32 r1 = im[b] * t.tsin[b] - re[b] * t.tcos[b];
        f32 i0 = im[b] * t.tcos[b] + re[b] * t.tsin[b];
        re[a] = r0;
        im[a] = i0;
        re[b] = r1;
        im[b] = i1;
    }
    for (u32 k = 0; k < m; k++) {
        out[2 * k] = re[k] * t.scale;
        out[2 * k + 1] = im[k] * t.scale;
    }
}

// --- band layout per sample rate -------------------------------------------------------------------

struct XmaRateTables {
    s32 num_sfb[BLOCK_SIZES];
    s16 sfb_offsets[BLOCK_SIZES][XMA_MAX_BANDS];            // band starts, multiples of 4
    s8 sf_offsets[BLOCK_SIZES][BLOCK_SIZES][XMA_MAX_BANDS]; // band of another block size covering a band
};

static XmaRateTables g_rates[4];

static void rate_init(XmaRateTables* r, s32 rate) {
    for (u32 i = 0; i < BLOCK_SIZES; i++) {
        s32 len = FRAME >> i, band = 1;
        r->sfb_offsets[i][0] = 0;
        for (u32 x = 0; x < XMA_MAX_BANDS - 1 && r->sfb_offsets[i][band - 1] < len; x++) {
            s32 offset = ((len * 2 * critical_freq[x]) / rate + 2) & ~3;
            if (offset > r->sfb_offsets[i][band - 1])
                r->sfb_offsets[i][band++] = (s16)offset;
            if (offset >= len)
                break;
        }
        r->sfb_offsets[i][band - 1] = (s16)len;
        r->num_sfb[i] = band - 1;
    }
    for (u32 i = 0; i < BLOCK_SIZES; i++) {
        for (s32 b = 0; b < r->num_sfb[i]; b++) {
            s32 offset = ((r->sfb_offsets[i][b] + r->sfb_offsets[i][b + 1] - 1) << i) >> 1;
            for (u32 x = 0; x < BLOCK_SIZES; x++) {
                s32 v = 0;
                while (v + 1 < XMA_MAX_BANDS - 1 && (r->sfb_offsets[x][v + 1] << x) < offset)
                    v++;
                r->sf_offsets[i][x][b] = (s8)v;
            }
        }
    }
}

void xma_decoder_init() {
    static const s32 rates[4] = {24000, 32000, 44100, 48000};
    for (u32 i = 0; i < 4; i++)
        rate_init(&g_rates[i], rates[i]);

    vlc_init(&g_sf_vlc, HUFF_SCALE_SIZE, &scale_table[0][1], 2, &scale_table[0][0], nullptr, 2, -60);
    vlc_init(&g_sf_rl_vlc, HUFF_SCALE_RL_SIZE, &scale_rl_table[0][1], 2, &scale_rl_table[0][0], nullptr, 2,
             0);
    vlc_init(&g_coef_vlc[0], HUFF_COEF0_SIZE, coef0_lens, 1, nullptr, coef0_syms, 1, 0);
    vlc_init(&g_coef_vlc[1], HUFF_COEF1_SIZE, &coef1_table[0][1], 2, &coef1_table[0][0], nullptr, 2, 0);
    vlc_init(&g_vec4_vlc, HUFF_VEC4_SIZE, vec4_lens, 1, nullptr, vec4_syms, 1, -1);
    vlc_init(&g_vec2_vlc, HUFF_VEC2_SIZE, &vec2_table[0][1], 2, &vec2_table[0][0], nullptr, 2, -1);
    vlc_init(&g_vec1_vlc, HUFF_VEC1_SIZE, &vec1_table[0][1], 2, &vec1_table[0][0], nullptr, 2, 0);

    for (u32 i = 0; i < BLOCK_SIZES; i++) {
        u32 n = MIN_BLOCK << i;
        imdct_init(&g_imdct[i], n);
        for (u32 k = 0; k < n; k++)
            g_window[i][k] = sinf(((f32)k + 0.5f) * (3.14159265358979323846f / (2.0f * (f32)n)));
    }
}

void xma_decoder_reset(XmaDecoder* d, u32 channels, u32 rate_index) {
    memset(d, 0, sizeof(*d));
    d->rate = &g_rates[rate_index & 3];
    d->channels = channels == 2 ? 2 : 1;
    for (u32 c = 0; c < 2; c++)
        d->channel[c].prev_block_len = FRAME;
}

// --- frame decoding --------------------------------------------------------------------------------

// State of the subframe being decoded.
struct Subframe {
    Bits b;
    s32 len;
    s32 channels; // channels that contain it
    s32 channel_index[2];
    s32 num_bands;
    const s16* sfb_offsets;
    s32 table_idx;
    s32 esc_len;
    bool transmit_num_vec_coeffs;
    bool parsed_all;
    // Channel group (stereo blocks only): mid/side per band.
    bool transform;
    s8 transform_band[XMA_MAX_BANDS];
};

static s32 subframe_length(XmaDecoder* d, Subframe& s, s32 offset) {
    (void)d;
    if (offset == FRAME - MIN_BLOCK)
        return MIN_BLOCK; // only one length fits
    if (s.b.pos >= s.b.end)
        return -1;
    s32 shift = 0;
    if (get_bit(s.b))
        shift = 1 + (s32)get_bits(s.b, 1);
    return FRAME >> shift;
}

// How the frame is split into subframes per channel: channels whose subframes so far end first
// get the next length, each (unless all channels share one layout) with a bit saying whether it
// takes part.
static bool decode_tile_header(XmaDecoder* d, Subframe& s) {
    u16 num_samples[2] = {0, 0};
    u8 contains[2];
    s32 channels_for_cur = (s32)d->channels;
    s32 min_len = 0;
    for (u32 c = 0; c < d->channels; c++)
        d->channel[c].num_subframes = 0;
    bool fixed_layout = get_bit(s.b) != 0;

    do {
        for (u32 c = 0; c < d->channels; c++) {
            if (num_samples[c] == min_len)
                contains[c] = (fixed_layout || channels_for_cur == 1 || min_len == FRAME - MIN_BLOCK)
                                  ? 1
                                  : (u8)get_bit(s.b);
            else
                contains[c] = 0;
        }
        s32 len = subframe_length(d, s, min_len);
        if (len <= 0)
            return false;
        min_len += len;
        for (u32 c = 0; c < d->channels; c++) {
            XmaChannel* ch = &d->channel[c];
            if (contains[c]) {
                if (ch->num_subframes >= XMA_MAX_SUBFRAMES)
                    return false;
                ch->subframe_len[ch->num_subframes++] = (u16)len;
                num_samples[c] = (u16)(num_samples[c] + len);
                if (num_samples[c] > FRAME)
                    return false;
            } else if (num_samples[c] <= min_len) {
                if (num_samples[c] < min_len) {
                    channels_for_cur = 0;
                    min_len = num_samples[c];
                }
                ++channels_for_cur;
            }
        }
    } while (min_len < FRAME);
    return true;
}

// Stereo coding of the subframe: none, or mid/side for all or some bands.
static bool decode_channel_transform(XmaDecoder* d, Subframe& s) {
    s.transform = false;
    if (d->channels < 2)
        return true;
    if (get_bit(s.b))
        return false; // a transform mode not seen in this format
    if (s.channels == 2) {
        for (s32 i = 0; i < 2; i++)
            d->channel[s.channel_index[i]].grouped = 1;
        if (get_bit(s.b)) {
            if (get_bit(s.b))
                return false; // unknown transform type
        } else {
            s.transform = true;
        }
    }
    if (s.transform) {
        if (!get_bit(s.b)) {
            for (s32 i = 0; i < s.num_bands; i++)
                s.transform_band[i] = (s8)get_bit(s.b);
        } else {
            memset(s.transform_band, 1, (size_t)s.num_bands);
        }
    }
    return true;
}

// Run/level coded coefficients from `offset` on; escapes carry explicit levels and runs.
static void decode_run_level(Subframe& s, const Vlc& vlc, const f32* level_table, const u16* run_table,
                             f32* coeffs, s32 offset) {
    const s32 mask = s.len - 1;
    for (; offset < s.len; offset++) {
        s32 code = vlc_get(s.b, vlc);
        if (code > 1) {
            offset += run_table[code];
            f32 level = level_table[code];
            coeffs[offset & mask] = get_bit(s.b) ? level : -level;
        } else if (code == 1) {
            break; // end of block
        } else {
            u32 level = get_large_value(s.b);
            if (get_bit(s.b)) {
                if (get_bit(s.b)) {
                    if (get_bit(s.b))
                        return; // broken escape
                    offset += (s32)get_bits(s.b, (u32)s.esc_len) + 4;
                } else {
                    offset += (s32)get_bits(s.b, 2) + 1;
                }
            }
            coeffs[offset & mask] = get_bit(s.b) ? (f32)level : -(f32)level;
        }
    }
}

static void decode_coeffs(XmaDecoder* d, Subframe& s, s32 c) {
    XmaChannel* ch = &d->channel[c];
    u32 table = get_bit(s.b);
    const Vlc& vlc = g_coef_vlc[table];
    const u16* run = table ? coef1_run : coef0_run;
    const f32* level = table ? coef1_level : coef0_level;
    bool rl_mode = false;
    s32 cur = 0, zeros = 0;

    // Vectors of four values (or two pairs, or single escaped values) until enough zeros in a row
    // make run/level coding take over.
    while ((s.transmit_num_vec_coeffs || !rl_mode) && cur + 3 < ch->num_vec_coeffs) {
        u32 vals[4];
        s32 idx = vlc_get(s.b, g_vec4_vlc);
        if (idx < 0) {
            for (u32 i = 0; i < 4; i += 2) {
                idx = vlc_get(s.b, g_vec2_vlc);
                if (idx < 0) {
                    u32 v0 = (u32)vlc_get(s.b, g_vec1_vlc);
                    if (v0 == HUFF_VEC1_SIZE - 1)
                        v0 += get_large_value(s.b);
                    u32 v1 = (u32)vlc_get(s.b, g_vec1_vlc);
                    if (v1 == HUFF_VEC1_SIZE - 1)
                        v1 += get_large_value(s.b);
                    vals[i] = v0;
                    vals[i + 1] = v1;
                } else {
                    vals[i] = (u32)idx >> 4;
                    vals[i + 1] = (u32)idx & 0xF;
                }
            }
        } else {
            vals[0] = (u32)idx >> 12;
            vals[1] = ((u32)idx >> 8) & 0xF;
            vals[2] = ((u32)idx >> 4) & 0xF;
            vals[3] = (u32)idx & 0xF;
        }
        for (u32 i = 0; i < 4; i++) {
            if (vals[i]) {
                f32 v = (f32)vals[i];
                ch->coeffs[cur] = get_bit(s.b) ? v : -v;
                zeros = 0;
            } else {
                ch->coeffs[cur] = 0.0f;
                rl_mode |= ++zeros > (s.len >> 8);
            }
            ++cur;
        }
    }
    if (cur < s.len) {
        memset(ch->coeffs + cur, 0, sizeof(f32) * (size_t)(s.len - cur));
        decode_run_level(s, vlc, level, run, ch->coeffs, cur);
    }
}

// Scale factors: DPCM coded when new, else resampled from the last ones (other block sizes have
// other bands) and corrected with run/level coded differences.
static bool decode_scale_factors(XmaDecoder* d, Subframe& s) {
    for (s32 i = 0; i < s.channels; i++) {
        XmaChannel* ch = &d->channel[s.channel_index[i]];
        ch->scale_factors = ch->saved_scale_factors[!ch->scale_factor_idx];
        if (ch->reuse_sf) {
            const s8* map = d->rate->sf_offsets[s.table_idx][ch->table_idx];
            for (s32 b = 0; b < s.num_bands; b++)
                ch->scale_factors[b] = ch->saved_scale_factors[ch->scale_factor_idx][map[b]];
        }
        if (!ch->cur_subframe || get_bit(s.b)) {
            if (!ch->reuse_sf) {
                ch->scale_factor_step = (u8)(get_bits(s.b, 2) + 1);
                s32 val = 45 / ch->scale_factor_step;
                for (s32 b = 0; b < s.num_bands; b++) {
                    val += vlc_get(s.b, g_sf_vlc);
                    ch->scale_factors[b] = val;
                }
            } else {
                for (s32 b = 0; b < s.num_bands; b++) {
                    s32 idx = vlc_get(s.b, g_sf_rl_vlc), skip, val, sign;
                    if (!idx) {
                        u32 code = get_bits(s.b, 14);
                        val = (s32)(code >> 6);
                        sign = (s32)(code & 1) - 1;
                        skip = (s32)(code & 0x3f) >> 1;
                    } else if (idx == 1) {
                        break;
                    } else {
                        if (idx < 0)
                            return false;
                        skip = scale_rl_run[idx];
                        val = scale_rl_level[idx];
                        sign = (s32)get_bit(s.b) - 1;
                    }
                    b += skip;
                    if (b >= s.num_bands)
                        return false;
                    ch->scale_factors[b] += (val ^ sign) - sign;
                }
            }
            ch->scale_factor_idx = !ch->scale_factor_idx;
            ch->table_idx = (u8)s.table_idx;
            ch->reuse_sf = 1;
        }
        ch->max_scale_factor = ch->scale_factors[0];
        for (s32 b = 1; b < s.num_bands; b++)
            if (ch->scale_factors[b] > ch->max_scale_factor)
                ch->max_scale_factor = ch->scale_factors[b];
    }
    return true;
}

// Mid/side back to left/right in the bands that use it; other bands of a coded pair are scaled
// by 181/128 (about sqrt(2)).
static void inverse_channel_transform(XmaDecoder* d, Subframe& s) {
    if (!s.transform)
        return;
    f32* a = d->channel[s.channel_index[0]].coeffs;
    f32* b = d->channel[s.channel_index[1]].coeffs;
    for (s32 band = 0; band < s.num_bands; band++) {
        s32 start = s.sfb_offsets[band];
        s32 end = s.sfb_offsets[band + 1] < s.len ? s.sfb_offsets[band + 1] : s.len;
        if (s.transform_band[band] == 1) {
            for (s32 y = start; y < end; y++) {
                f32 m = a[y], n = b[y];
                a[y] = m - n;
                b[y] = m + n;
            }
        } else {
            for (s32 y = start; y < end; y++) {
                a[y] *= 181.0f / 128.0f;
                b[y] *= 181.0f / 128.0f;
            }
        }
    }
}

// Overlap-add with the previous block through a sine window as long as the shorter of the two.
static void window_blocks(XmaDecoder* d, Subframe& s) {
    for (s32 i = 0; i < s.channels; i++) {
        XmaChannel* ch = &d->channel[s.channel_index[i]];
        s32 winlen = ch->prev_block_len;
        f32* start = ch->coeffs - (winlen >> 1);
        if (s.len < winlen) {
            start += (winlen - s.len) >> 1;
            winlen = s.len;
        }
        const f32* win = g_window[log2_floor((u32)winlen) - 7];
        s32 half = winlen >> 1;
        f32* dst = start + half;
        const f32* src0 = start + half;
        const f32* src1 = start + half;
        const f32* w = win + half;
        for (s32 k = -half, j = half - 1; k < 0; k++, j--) {
            f32 s0 = src0[k], s1 = src1[j], wi = w[k], wj = w[j];
            dst[k] = s0 * wj - s1 * wi;
            dst[j] = s0 * wi + s1 * wj;
        }
        ch->prev_block_len = (s16)s.len;
    }
}

static bool decode_subframe(XmaDecoder* d, Subframe& s) {
    s32 offset = FRAME, len = FRAME;
    s32 total = FRAME * (s32)d->channels;
    bool transmit_coeffs = false;

    // The next block is the one of the channel with the fewest decoded samples.
    for (u32 c = 0; c < d->channels; c++) {
        XmaChannel* ch = &d->channel[c];
        ch->grouped = 0;
        if (offset > ch->decoded_samples) {
            offset = ch->decoded_samples;
            len = ch->subframe_len[ch->cur_subframe];
        }
    }
    s.channels = 0;
    for (u32 c = 0; c < d->channels; c++) {
        XmaChannel* ch = &d->channel[c];
        total -= ch->decoded_samples;
        if (offset == ch->decoded_samples && len == ch->subframe_len[ch->cur_subframe]) {
            total -= len;
            ch->decoded_samples = (u16)(ch->decoded_samples + len);
            s.channel_index[s.channels++] = (s32)c;
        }
    }
    if (!total)
        s.parsed_all = true;

    s.table_idx = (s32)log2_floor((u32)(FRAME / len));
    s.num_bands = d->rate->num_sfb[s.table_idx];
    s.sfb_offsets = d->rate->sfb_offsets[s.table_idx];
    offset += FRAME / 2;
    for (s32 i = 0; i < s.channels; i++)
        d->channel[s.channel_index[i]].coeffs = &d->channel[s.channel_index[i]].out[offset];
    s.len = len;
    s.esc_len = (s32)log2_floor((u32)(len - 1)) + 1;

    // Extended header: fill bits to skip.
    if (get_bit(s.b)) {
        u32 fill = get_bits(s.b, 2);
        if (!fill)
            fill = get_bits(s.b, get_bits(s.b, 4)) + 1;
        if (s.b.pos + fill > s.b.end)
            return false;
        s.b.pos += fill;
    }
    if (get_bit(s.b))
        return false; // reserved

    if (!decode_channel_transform(d, s))
        return false;

    for (s32 i = 0; i < s.channels; i++) {
        XmaChannel* ch = &d->channel[s.channel_index[i]];
        ch->transmit_coefs = (u8)get_bit(s.b);
        transmit_coeffs |= ch->transmit_coefs != 0;
    }

    if (transmit_coeffs) {
        s.transmit_num_vec_coeffs = get_bit(s.b) != 0;
        if (s.transmit_num_vec_coeffs) {
            u32 bits = log2_floor((u32)((len + 3) / 4)) + 1;
            for (s32 i = 0; i < s.channels; i++) {
                u32 n = get_bits(s.b, bits) << 2;
                if ((s32)n > len)
                    return false;
                d->channel[s.channel_index[i]].num_vec_coeffs = (u16)n;
            }
        } else {
            for (s32 i = 0; i < s.channels; i++)
                d->channel[s.channel_index[i]].num_vec_coeffs = (u16)len;
        }

        s32 quant_step = QUANT_STEP_BASE;
        s32 step = get_sbits(s.b, 6);
        quant_step += step;
        if (step == -32 || step == 31) {
            s32 sign = (step == 31) - 1, quant = 0;
            while (s.b.pos + 5 < s.b.end && (step = (s32)get_bits(s.b, 5)) == 31)
                quant += 31;
            quant_step += ((quant + step) ^ sign) - sign;
        }
        if (s.channels == 1) {
            d->channel[s.channel_index[0]].quant_step = quant_step;
        } else {
            u32 modifier_len = get_bits(s.b, 3);
            for (s32 i = 0; i < s.channels; i++) {
                XmaChannel* ch = &d->channel[s.channel_index[i]];
                ch->quant_step = quant_step;
                if (get_bit(s.b))
                    ch->quant_step += modifier_len ? (s32)get_bits(s.b, modifier_len) + 1 : 1;
            }
        }
        if (!decode_scale_factors(d, s))
            return false;
    }

    for (s32 i = 0; i < s.channels; i++) {
        s32 c = s.channel_index[i];
        XmaChannel* ch = &d->channel[c];
        if (ch->transmit_coefs && s.b.pos < s.b.end)
            decode_coeffs(d, s, c);
        else
            memset(ch->coeffs, 0, sizeof(f32) * (size_t)len);
    }

    if (transmit_coeffs) {
        const Imdct& t = g_imdct[log2_floor((u32)len) - 7];
        inverse_channel_transform(d, s);
        for (s32 i = 0; i < s.channels; i++) {
            XmaChannel* ch = &d->channel[s.channel_index[i]];
            f32 tmp[FRAME];
            for (s32 b = 0; b < s.num_bands; b++) {
                s32 start = s.sfb_offsets[b];
                s32 end = s.sfb_offsets[b + 1] < len ? s.sfb_offsets[b + 1] : len;
                s32 exp =
                    ch->quant_step - (ch->max_scale_factor - ch->scale_factors[b]) * ch->scale_factor_step;
                f32 quant = (f32)pow(10.0, exp / 20.0);
                for (s32 k = start; k < end; k++)
                    tmp[k] = ch->coeffs[k] * quant;
            }
            imdct_half(t, ch->coeffs, tmp);
        }
    }

    window_blocks(d, s);

    for (s32 i = 0; i < s.channels; i++) {
        XmaChannel* ch = &d->channel[s.channel_index[i]];
        if (ch->cur_subframe >= ch->num_subframes)
            return false;
        ++ch->cur_subframe;
    }
    return true;
}

static s16 to_s16(f32 v) {
    s32 i = (s32)lrintf(v);
    return (s16)(i < -32768 ? -32768 : i > 32767 ? 32767 : i);
}

bool xma_decoder_frame(XmaDecoder* d, const u8* data, u32 start_bit, u32 bits, s16* out) {
    Subframe s;
    memset(&s, 0, sizeof(s));
    s.b.data = data;
    s.b.pos = start_bit;
    s.b.end = start_bit + bits;
    s.b.size = (s.b.end + 7) / 8 + XMA_FRAME_PADDING;

    bool ok = bits > LENGTH_BITS && (s.b.end + 7) / 8 <= XMA_FRAME_MAX_BYTES;
    if (ok) {
        get_bits(s.b, LENGTH_BITS); // the caller walked frames by it already
        ok = decode_tile_header(d, s);
    }
    if (ok) {
        // Stereo post-processing matrix (unused by the output stage), dynamic range control gain,
        // and the frame's trim counts (stream edges, handled by titles through skip counts).
        if (d->channels > 1 && get_bit(s.b) && get_bit(s.b))
            s.b.pos += 4 * d->channels * d->channels;
        get_bits(s.b, 8);
        if (get_bit(s.b)) {
            if (get_bit(s.b))
                get_bits(s.b, 10);
            if (get_bit(s.b))
                get_bits(s.b, 10);
        }
        for (u32 c = 0; c < d->channels; c++) {
            d->channel[c].decoded_samples = 0;
            d->channel[c].cur_subframe = 0;
            d->channel[c].reuse_sf = 0;
        }
        for (u32 guard = 0; ok && !s.parsed_all; guard++)
            ok = guard < 2 * XMA_MAX_SUBFRAMES && decode_subframe(d, s);
        ok = ok && s.b.pos <= s.b.end;
    }
    if (!ok) {
        memset(out, 0, sizeof(s16) * FRAME * d->channels);
        xma_decoder_reset(d, d->channels, (u32)(d->rate - g_rates));
        return false;
    }

    for (u32 c = 0; c < d->channels; c++) {
        XmaChannel* ch = &d->channel[c];
        for (u32 i = 0; i < FRAME; i++)
            out[i * d->channels + c] = to_s16(ch->out[i]);
        // The second half of the last block overlaps the next frame.
        memcpy(ch->out, ch->out + FRAME, sizeof(f32) * FRAME / 2);
    }
    return true;
}

void xma_decoder_imdct(u32 n, f32* out, const f32* in) {
    imdct_half(g_imdct[log2_floor(n) - 7], out, in);
}
