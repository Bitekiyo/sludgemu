// Tests for the XMA context engine (hle/audio/xma.cpp) on a synthetic stream: frames are walked
// across packet boundaries, output lands in the ring in whole frames, buffers are handed back at the
// end marker, and loop points repeat the stream.

#include <windows.h>
#include <string.h>

#include "hle/audio/apu.h"
#include "test.h"

namespace {

// APU addresses in the test are offsets into this arena (0 stays invalid).
alignas(4096) u8 g_arena[256 * 1024];

struct Field {
    u32 word, shift, bits;
};
const Field kSizeRead0 = {0, 0, 12}, kNumLoops = {0, 12, 8}, kValidBuffer = {0, 20, 2},
            kSizeWrite = {0, 22, 5}, kOffsetWrite = {0, 27, 5}, kNumChannels = {1, 29, 1},
            kValidWrite = {1, 31, 1}, kOffsetRead = {2, 0, 26}, kLoopStart = {3, 0, 26},
            kLoopEnd = {4, 0, 26}, kCurrentBuffer = {4, 31, 1}, kPtrRead0 = {5, 0, 32},
            kPtrWrite = {7, 0, 32}, kWriteRead = {9, 0, 5};

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

const u32 kInput = 0x8000, kRing = 0x10000, kContexts = 0x20000;

} // namespace

void* apu_ptr(u32 address) {
    return address && address < sizeof(g_arena) ? g_arena + address : nullptr;
}

void test_xma() {
    memset(g_arena, 0, sizeof(g_arena));
    // Two packets: header 3 frames, first frame at bit 0 of the data, metadata 1, skip 0.
    u8* in = g_arena + kInput;
    for (u32 p = 0; p < 2; p++) {
        in[p * 2048 + 0] = (u8)(3 << 2);
        in[p * 2048 + 2] = 1;
    }
    // Frames at 32 (4000 bits), 4032 (5000), 9032 (6000), 15032 (3000, crossing into packet 1:
    // 1352 bits there, the rest after packet 1's header), then the end marker at 18064.
    put15(in, 32, 4000);
    put15(in, 4032, 5000);
    put15(in, 9032, 6000);
    put15(in, 15032, 3000);
    put15(in, 18064, 0x7FFF);

    u32* c = (u32*)(g_arena + kContexts);
    u32 src[16] = {};
    set(src, kPtrRead0, kInput);
    set(src, kSizeRead0, 2);
    set(src, kValidBuffer, 1);
    set(src, kPtrWrite, kRing);
    set(src, kSizeWrite, 31);
    set(src, kValidWrite, 1);
    set(src, kNumChannels, 0); // mono: a frame is 512 samples = 4 blocks
    xma_set_contexts(c, 2);
    xma_update(0, 0x03ffffff, src);
    xma_enable(0, true);
    xma_run();
    CHECK(get(c, kOffsetWrite) == 16);  // four frames
    CHECK(get(c, kValidBuffer) == 0);   // buffer 0 handed back at the end marker
    CHECK(get(c, kCurrentBuffer) == 1); // waiting for buffer 1
    xma_run();
    CHECK(get(c, kOffsetWrite) == 16); // nothing more without input

    // Loop: back to the first frame when the third would start, once. The ring is drained first.
    set(src, kOffsetWrite, 0);
    set(src, kWriteRead, 0);
    set(src, kOffsetRead, 0);
    set(src, kLoopStart, 32);
    set(src, kLoopEnd, 9032);
    set(src, kNumLoops, 1);
    set(src, kCurrentBuffer, 0);
    xma_update(0, 0x03ffffff, src);
    xma_run();
    CHECK(get(c, kOffsetWrite) == 24); // frames 1, 2, 1, 2, 3, 4
    CHECK(get(c, kNumLoops) == 0);

    // A nearly full ring stops decoding until the title moves its read offset.
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
    CHECK(get(c, kValidBuffer) == 0);

    xma_set_contexts(nullptr, 0);
}
