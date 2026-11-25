// Tests for XMA decoding: the context engine (hle/audio/xma.cpp) on a synthetic stream (frames
// walked across packets, end-of-packet padding, output a subframe at a time as the ring has room,
// buffers handed back, loop points), and the frame decoder (hle/audio/xma_decoder.cpp): its inverse
// MDCT against the textbook formula, and random data never crashing it.

#include <windows.h>
#include <math.h>
#include <string.h>

#include "hle/audio/apu.h"
#include "hle/audio/xma_decoder.h"
#include "test.h"

namespace {

// APU addresses in the test are offsets into this arena (0 stays invalid).
alignas(4096) u8 g_arena[256 * 1024];

struct Field {
    u32 word, shift, bits;
};
const Field kSizeRead0 = {0, 0, 12}, kNumLoops = {0, 12, 8}, kValidBuffer = {0, 20, 2},
            kSizeWrite = {0, 22, 5}, kOffsetWrite = {0, 27, 5}, kLoopSubframeEnd = {1, 12, 2},
            kLoopSubframeSkip = {1, 17, 3}, kNumChannels = {1, 29, 1}, kValidWrite = {1, 31, 1},
            kOffsetRead = {2, 0, 26}, kLoopStart = {3, 0, 26}, kLoopEnd = {4, 0, 26},
            kCurrentBuffer = {4, 31, 1}, kPtrRead0 = {5, 0, 32}, kPtrWrite = {7, 0, 32},
            kWriteRead = {9, 0, 5};

u32 get(const u32* c, Field f) {
    return f.bits == 32 ? c[f.word] : (c[f.word] >> f.shift) & ((1u << f.bits) - 1);
}

void set(u32* c, Field f, u32 v) {
    u32 mask = f.bits == 32 ? 0xFFFFFFFFu : ((1u << f.bits) - 1) << f.shift;
    c[f.word] = (c[f.word] & ~mask) | ((f.bits == 32 ? v : v << f.shift) & mask);
}

// Writes a 15-bit big-endian bit field at bit `pos` of the buffer.
void put15(u8* buf, u32 pos, u32 value) {
    for (u32 i = 0; i < 15; i++) {
        u32 p = pos + i, b = (value >> (14 - i)) & 1;
        buf[p >> 3] = (u8)((buf[p >> 3] & ~(0x80 >> (p & 7))) | (b << (7 - (p & 7))));
    }
}

// Packet header: frames starting in the packet, bit offset of the first one, metadata, skip count.
void put_header(u8* packet, u32 frames, u32 first_frame) {
    packet[0] = (u8)((frames << 2) | (first_frame >> 13));
    packet[1] = (u8)(first_frame >> 5);
    packet[2] = (u8)(((first_frame & 31) << 3) | 1);
    packet[3] = 0;
}

const u32 kInput = 0x8000, kRing = 0x10000, kContexts = 0x20000;

u32 g_seed = 12345;
u32 random_u32() {
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed;
}

void test_engine() {
    memset(g_arena, 0, sizeof(g_arena));
    // Packet 0: frames at 32 (4000 bits), 4032 (5000), 9032 (6000), then padding (a length of all
    // ones). Packet 1: 100 bits of no frame, one frame at 16516 ending 10 bits before the packet
    // end: too little for another length field, and no packet continues it.
    u8* in = g_arena + kInput;
    put_header(in, 3, 0);
    put_header(in + 2048, 1, 100);
    put15(in, 32, 4000);
    put15(in, 4032, 5000);
    put15(in, 9032, 6000);
    put15(in, 15032, 0x7FFF);
    put15(in, 16516, 32768 - 10 - 16516);

    u32* c = (u32*)(g_arena + kContexts);
    u32 src[16] = {};
    set(src, kPtrRead0, kInput);
    set(src, kSizeRead0, 2);
    set(src, kValidBuffer, 1);
    set(src, kPtrWrite, kRing);
    set(src, kSizeWrite, 31);
    set(src, kValidWrite, 1);
    set(src, kNumChannels, 0); // mono: a subframe is 128 samples = 1 block
    xma_set_contexts(c, 2);
    xma_update(0, 0x03ffffff, src);
    xma_enable(0, true);
    xma_run();
    CHECK(get(c, kOffsetWrite) == 16);  // four frames
    CHECK(get(c, kValidBuffer) == 0);   // buffer 0 handed back after its last packet
    CHECK(get(c, kCurrentBuffer) == 1); // waiting for buffer 1
    xma_run();
    CHECK(get(c, kOffsetWrite) == 16); // nothing more without input

    // Loop once from the third frame (subframes 0-1 of it) back to the first, skipping its first
    // subframe: 4 + 4 + 2, then 3 + 4 + 4 + 4.
    set(src, kOffsetWrite, 0);
    set(src, kWriteRead, 0);
    set(src, kOffsetRead, 0);
    set(src, kLoopStart, 32);
    set(src, kLoopEnd, 9032);
    set(src, kLoopSubframeEnd, 1);
    set(src, kLoopSubframeSkip, 1);
    set(src, kNumLoops, 1);
    set(src, kCurrentBuffer, 0);
    xma_update(0, 0x03ffffff, src);
    xma_run();
    CHECK(get(c, kOffsetWrite) == 25);
    CHECK(get(c, kNumLoops) == 0);

    // A full ring stops decoding until the title moves its read offset; nothing is decoded ahead.
    set(src, kOffsetWrite, 0);
    set(src, kWriteRead, 0);
    set(src, kOffsetRead, 0);
    set(src, kNumLoops, 0);
    set(src, kValidBuffer, 1);
    set(src, kCurrentBuffer, 0);
    set(src, kSizeWrite, 9); // room for two frames (one block stays free)
    xma_update(0, 0x03ffffff, src);
    xma_run();
    CHECK(get(c, kOffsetWrite) == 8);
    CHECK(get(c, kOffsetRead) == 9032); // stopped before frame 3
    xma_increment_write_read(0, 8);
    xma_run();
    CHECK(get(c, kOffsetWrite) == 7); // (8 + 8) % 9: frames 3 and 4 written
    CHECK(get(c, kValidBuffer) == 1); // the end isn't seen until there is room again
    xma_increment_write_read(0, 8);
    xma_run();
    CHECK(get(c, kValidBuffer) == 0);
    CHECK(get(c, kOffsetWrite) == 7);

    xma_set_contexts(nullptr, 0);
}

// The fast inverse MDCT against the definition: output i of the half transform is the middle half
// of the full one, scaled by 2/n for 16-bit range.
void test_imdct() {
    static f32 in[512], out[512];
    for (u32 n = 128; n <= 512; n *= 2) {
        for (u32 k = 0; k < n; k++)
            in[k] = (f32)((s32)(random_u32() >> 16) - 32768) / 64.0f;
        xma_decoder_imdct(n, out, in);
        const f64 pi = 3.14159265358979323846, phase = pi / (4.0 * n);
        f64 worst = 0.0, peak = 0.0;
        for (u32 i = 0; i < n / 2; i++) {
            f64 down = 0.0, up = 0.0;
            for (u32 j = 0; j < n; j++) {
                down += cos((2.0 * j + 1) * phase * (2.0 * n - 2.0 * i - 1)) * in[j];
                up += cos((2.0 * j + 1) * phase * (3.0 * n + 2.0 * i + 1)) * in[j];
            }
            f64 a = down * 2.0 / n, b = -up * 2.0 / n;
            worst = fmax(worst, fmax(fabs(a - out[i]), fabs(b - out[i + n / 2])));
            peak = fmax(peak, fmax(fabs(a), fabs(b)));
        }
        CHECK(worst < peak * 1e-5);
    }
}

// Random frames must decode or fail cleanly, never read or write out of bounds.
void test_decoder_fuzz() {
    static u8 frame[XMA_FRAME_MAX_BYTES + XMA_FRAME_PADDING];
    static s16 out[XMA_FRAME_SAMPLES * 2];
    static XmaDecoder d;
    for (u32 run = 0; run < 3000; run++) {
        u32 channels = 1 + (run & 1);
        if (run % 50 == 0)
            xma_decoder_reset(&d, channels, (run / 50) & 3);
        d.channels = channels;
        u32 start = random_u32() & 7, bits = 16 + random_u32() % (32767 - 16);
        u32 bytes = (start + bits + 7) / 8;
        for (u32 i = 0; i < bytes; i++)
            frame[i] = (u8)(random_u32() >> 24);
        memset(frame + bytes, 0, XMA_FRAME_PADDING);
        xma_decoder_frame(&d, frame, start, bits, out);
    }
    CHECK(true);
}

} // namespace

void* apu_ptr(u32 address) {
    return address && address < sizeof(g_arena) ? g_arena + address : nullptr;
}

void test_xma() {
    test_engine();
    test_imdct();
    test_decoder_fuzz();
}
