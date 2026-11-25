// XMA frame decoder: one XMA (WMA Pro) frame of a mono or stereo stream to 512 samples per channel.
//
// Based on FFmpeg libavcodec/wmaprodec.c:
//   Copyright (c) 2007 Baptiste Coudurier, Benjamin Larsson, Ulion
//   Copyright (c) 2008 - 2011 Sascha Sommer, Benjamin Larsson
//
// This file is free software; you can redistribute it and/or modify it under the terms of the GNU
// Lesser General Public License as published by the Free Software Foundation; either version 2.1
// of the License, or (at your option) any later version.
//
// It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
// implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser
// General Public License for more details.

#pragma once

#include "common/types.h"

#define XMA_FRAME_SAMPLES 512
// Bytes a frame can span (15-bit length plus the start offset in its first byte), and the zero
// padding the decoder may read past it.
#define XMA_FRAME_MAX_BYTES 4097
#define XMA_FRAME_PADDING   8

#define XMA_MAX_SUBFRAMES 32
#define XMA_MAX_BANDS     29

struct XmaRateTables;

// Per channel state carried from frame to frame.
struct XmaChannel {
    f32 out[XMA_FRAME_SAMPLES + XMA_FRAME_SAMPLES / 2]; // frame being built + tail of the last block
    f32* coeffs;                                        // current subframe inside `out`
    s32 saved_scale_factors[2][XMA_MAX_BANDS];
    s32* scale_factors;
    s32 quant_step;
    s32 max_scale_factor;
    u16 subframe_len[XMA_MAX_SUBFRAMES + 1];
    u16 decoded_samples;
    u16 num_vec_coeffs;
    s16 prev_block_len;
    u8 num_subframes;
    u8 cur_subframe;
    u8 transmit_coefs;
    u8 grouped;
    u8 reuse_sf;
    u8 scale_factor_step;
    u8 scale_factor_idx;
    u8 table_idx;
};

struct XmaDecoder {
    const XmaRateTables* rate; // scale factor band layout for the stream's sample rate
    u32 channels;              // 1 or 2
    XmaChannel channel[2];
};

// Builds the shared tables. Call once before decoding.
void xma_decoder_init();

// Starts a stream: no overlap from earlier frames. `rate_index`: 0 = 24 kHz, 1 = 32 kHz,
// 2 = 44.1 kHz, 3 = 48 kHz.
void xma_decoder_reset(XmaDecoder* d, u32 channels, u32 rate_index);

// Decodes the frame at bit `start_bit` (< 8) of `data`, `bits` long (its length field included),
// into XMA_FRAME_SAMPLES 16-bit samples per channel, interleaved. `data` must be followed by
// XMA_FRAME_PADDING zero bytes. Returns false for a corrupt frame (the output is then silence and
// the stream starts over).
bool xma_decoder_frame(XmaDecoder* d, const u8* data, u32 start_bit, u32 bits, s16* out);

// The decoder's half inverse MDCT of n (128, 256 or 512) coefficients, scaled for 16-bit output.
// Exposed for tests.
void xma_decoder_imdct(u32 n, f32* out, const f32* in);
