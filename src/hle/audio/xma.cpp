// XMA decoding contexts of the audio hardware (see apu.h).
//
// The context is a 64-byte record of bit fields, read and written here field by field. Input is a
// pair of buffers of 2 KB packets; each packet starts with a 32-bit header (frames beginning in it,
// bit offset of the first one, metadata, packets of other streams to skip to reach this stream's
// next packet). Frames start with a 15-bit length in bits, run on across packet boundaries (past the
// next packet's header), never across buffers, and decode to 512 samples: four subframes of 128.
// A length of all ones marks the end of the data.
//
// Output is 16-bit samples, interleaved for stereo, into a ring of 256-byte blocks that titles drain
// by moving the ring's read offset. Frames are decoded whole, as long as the ring has room.
//
// Frame data is handed to a decoder; until one exists, frames decode to silence, so titles still
// see their streams play at the right pace.

#include <windows.h>
#include <string.h>

#include "common/log.h"
#include "hle/audio/apu.h"

// --- context fields --------------------------------------------------------------------------------

struct Field {
    u8 word;
    u8 shift;
    u8 bits;
};

static const Field SIZE_READ0 = {0, 0, 12}, NUM_LOOPS = {0, 12, 8}, VALID_BUFFER = {0, 20, 2},
                   SIZE_WRITE = {0, 22, 5}, OFFSET_WRITE = {0, 27, 5}, SIZE_READ1 = {1, 0, 12},
                   LOOP_SUBFRAME_END = {1, 12, 2}, LOOP_SUBFRAME_SKIP = {1, 17, 3},
                   SUBFRAMES_TO_DECODE = {1, 20, 4}, SUBFRAMES_TO_SKIP = {1, 24, 3}, SAMPLE_RATE = {1, 27, 2},
                   NUM_CHANNELS = {1, 29, 1}, VALID_WRITE = {1, 31, 1}, OFFSET_READ = {2, 0, 26},
                   ERROR_STATUS = {2, 26, 5}, ERROR_SET = {2, 31, 1}, LOOP_START = {3, 0, 26},
                   PARSER_ERROR_STATUS = {3, 26, 5}, PARSER_ERROR_SET = {3, 31, 1}, LOOP_END = {4, 0, 26},
                   PACKET_METADATA = {4, 26, 5}, CURRENT_BUFFER = {4, 31, 1}, PTR_READ0 = {5, 0, 32},
                   PTR_READ1 = {6, 0, 32}, PTR_WRITE = {7, 0, 32}, PTR_OVERLAP_ADD = {8, 0, 32},
                   WRITE_BUFFER_OFFSET_READ = {9, 0, 5};

static u32 get(const u32* c, Field f) {
    u32 mask = f.bits == 32 ? 0xFFFFFFFFu : (1u << f.bits) - 1;
    return (c[f.word] >> f.shift) & mask;
}

static void set(u32* c, Field f, u32 value) {
    u32 mask = f.bits == 32 ? 0xFFFFFFFFu : (1u << f.bits) - 1;
    c[f.word] = (c[f.word] & ~(mask << f.shift)) | ((value & mask) << f.shift);
}

// Fields an update copies, by update flag bit.
static const Field k_update_fields[] = {
    SIZE_READ0,         SIZE_READ1,          NUM_LOOPS,         VALID_BUFFER,
    {0, 20, 1},         {0, 21, 1},          SIZE_WRITE,        LOOP_SUBFRAME_END,
    LOOP_SUBFRAME_SKIP, SUBFRAMES_TO_DECODE, SUBFRAMES_TO_SKIP, SAMPLE_RATE,
    NUM_CHANNELS,       VALID_WRITE,         OFFSET_READ,       LOOP_START,
    LOOP_END,           PACKET_METADATA,     CURRENT_BUFFER,    PTR_READ0,
    PTR_READ1,          PTR_WRITE,           PTR_OVERLAP_ADD,   WRITE_BUFFER_OFFSET_READ,
    OFFSET_WRITE,
};
static_assert(ARRAY_COUNT(k_update_fields) == 25, "one field per update flag");

#define UPDATE_OFFSET_READ (1u << 14)

// --- state ---------------------------------------------------------------------------------------

#define MAX_CONTEXTS     512
#define PACKET_BYTES     2048u
#define PACKET_BITS      (PACKET_BYTES * 8u)
#define HEADER_BITS      32u
#define FRAME_SAMPLES    512u
#define SUBFRAME_SAMPLES 128u
#define BLOCK_BYTES      256u
#define FINAL_FRAME      0x7FFFu
#define INFINITE_LOOPS   255u

static struct {
    SRWLOCK lock;
    u32* contexts; // the title's array, XMA_CONTEXT_SIZE bytes each
    u32 count;
    u32 enabled[MAX_CONTEXTS / 32];
    u32 skip_pending[MAX_CONTEXTS / 32]; // subframes to skip before the next output
} g_xma = {SRWLOCK_INIT};

static u32* context(u32 index) {
    return g_xma.contexts && index < g_xma.count ? g_xma.contexts + index * (XMA_CONTEXT_SIZE / 4) : nullptr;
}

static bool bit(const u32* set, u32 i) {
    return (set[i / 32] >> (i % 32)) & 1;
}

static void set_bit(u32* set, u32 i, bool on) {
    if (on)
        set[i / 32] |= 1u << (i % 32);
    else
        set[i / 32] &= ~(1u << (i % 32));
}

void xma_set_contexts(void* contexts, u32 count) {
    AcquireSRWLockExclusive(&g_xma.lock);
    g_xma.contexts = (u32*)contexts;
    g_xma.count = count < MAX_CONTEXTS ? count : MAX_CONTEXTS;
    memset(g_xma.enabled, 0, sizeof(g_xma.enabled));
    ReleaseSRWLockExclusive(&g_xma.lock);
}

void xma_enable(u32 index, bool enable) {
    AcquireSRWLockExclusive(&g_xma.lock);
    if (context(index)) {
        if (enable && !bit(g_xma.enabled, index))
            set_bit(g_xma.skip_pending, index, true);
        set_bit(g_xma.enabled, index, enable);
    }
    ReleaseSRWLockExclusive(&g_xma.lock);
}

void xma_update(u32 index, u32 mask, const void* source) {
    AcquireSRWLockExclusive(&g_xma.lock);
    u32* c = context(index);
    const u32* src = (const u32*)source;
    if (c && src) {
        for (u32 b = 0; b < ARRAY_COUNT(k_update_fields); b++)
            if (mask & (1u << b))
                set(c, k_update_fields[b], get(src, k_update_fields[b]));
        if (mask & UPDATE_OFFSET_READ)
            set_bit(g_xma.skip_pending, index, true); // a new start point
    }
    ReleaseSRWLockExclusive(&g_xma.lock);
}

void xma_update_entry(u32 index, u32 ptr_read0, u32 ptr_read1, u32 size_read0, u32 size_read1,
                      u32 write_buffer_offset_read, u32 valid_buffer) {
    AcquireSRWLockExclusive(&g_xma.lock);
    if (u32* c = context(index)) {
        set(c, PTR_READ0, ptr_read0);
        set(c, PTR_READ1, ptr_read1);
        set(c, SIZE_READ0, size_read0);
        set(c, SIZE_READ1, size_read1);
        set(c, WRITE_BUFFER_OFFSET_READ, write_buffer_offset_read);
        set(c, VALID_BUFFER, get(c, VALID_BUFFER) | valid_buffer);
    }
    ReleaseSRWLockExclusive(&g_xma.lock);
}

void xma_increment_write_read(u32 index, u32 increment) {
    AcquireSRWLockExclusive(&g_xma.lock);
    if (u32* c = context(index)) {
        u32 blocks = get(c, SIZE_WRITE);
        if (blocks)
            set(c, WRITE_BUFFER_OFFSET_READ, (get(c, WRITE_BUFFER_OFFSET_READ) + increment) % blocks);
    }
    ReleaseSRWLockExclusive(&g_xma.lock);
}

void xma_update_read_buffer(u32 index, u32 buffer, u32 size, bool change_size, bool change_address,
                            u32 address) {
    AcquireSRWLockExclusive(&g_xma.lock);
    if (u32* c = context(index)) {
        if (change_size)
            set(c, buffer ? SIZE_READ1 : SIZE_READ0, size);
        if (change_address)
            set(c, buffer ? PTR_READ1 : PTR_READ0, address);
    }
    ReleaseSRWLockExclusive(&g_xma.lock);
}

// --- bitstream -----------------------------------------------------------------------------------

// One input buffer, positions in bits from its start (packet headers included).
struct InputBuffer {
    const u8* data;
    u32 bits;
};

static u32 read_bit(const InputBuffer& in, u32 pos) {
    return (in.data[pos >> 3] >> (7 - (pos & 7))) & 1;
}

// Moves `pos` on by `count` data bits of this stream, stepping over packet headers and the packets
// of other streams. Returns in.bits when it runs off the end.
static u32 advance(const InputBuffer& in, u32 pos, u32 count) {
    while (count) {
        u32 packet_start = pos / PACKET_BITS * PACKET_BITS;
        u32 left = packet_start + PACKET_BITS - pos;
        if (count < left)
            return pos + count;
        count -= left;
        u32 skip = in.data[packet_start / 8 + 3];
        u32 next = packet_start + (skip + 1) * PACKET_BITS;
        if (next >= in.bits)
            return in.bits;
        pos = next + HEADER_BITS;
    }
    return pos;
}

static u32 read_bits(const InputBuffer& in, u32* pos, u32 count) {
    u32 v = 0;
    for (u32 i = 0; i < count && *pos < in.bits; i++) {
        v = (v << 1) | read_bit(in, *pos);
        *pos = advance(in, *pos, 1);
    }
    return v;
}

// The first frame of the buffer: just past the first packet's header and the frame data
// continuing from the previous packet.
static u32 first_frame(const InputBuffer& in) {
    u32 offset = ((u32)(in.data[0] & 3) << 13) | ((u32)in.data[1] << 5) | (in.data[2] >> 3);
    return HEADER_BITS + offset;
}

// --- decoding ------------------------------------------------------------------------------------

// Decodes one frame (its data bits, starting with the length field) into FRAME_SAMPLES 16-bit
// samples per channel, interleaved. No decoder yet: silence.
static void decode_frame(const InputBuffer&, u32 frame_pos, u32 frame_bits, u32 channels, s16* out) {
    (void)frame_pos;
    (void)frame_bits;
    memset(out, 0, (usize)FRAME_SAMPLES * channels * sizeof(s16));
}

static InputBuffer input(const u32* c, u32 buffer) {
    InputBuffer in;
    u32 address = get(c, buffer ? PTR_READ1 : PTR_READ0) & ~(PACKET_BYTES - 1);
    in.data = (const u8*)apu_ptr(address);
    in.bits = get(c, buffer ? SIZE_READ1 : SIZE_READ0) * PACKET_BITS;
    if (!in.data)
        in.bits = 0;
    return in;
}

// The current buffer is used up: hand it back to the title and move to the other one.
static void finish_buffer(u32* c) {
    u32 current = get(c, CURRENT_BUFFER);
    set(c, VALID_BUFFER, get(c, VALID_BUFFER) & ~(1u << current));
    set(c, CURRENT_BUFFER, current ^ 1);
    set(c, OFFSET_READ, 0);
}

// Decodes frames into one context's output ring until input or room runs out.
static void run_context(u32 index) {
    u32* c = context(index);
    if (!get(c, VALID_WRITE))
        return;
    u32 blocks = get(c, SIZE_WRITE);
    u8* ring = (u8*)apu_ptr(get(c, PTR_WRITE));
    if (!blocks || !ring)
        return;
    u32 channels = get(c, NUM_CHANNELS) + 1;
    s16 samples[FRAME_SAMPLES * 2];

    for (int guard = 0; guard < 64; guard++) {
        u32 current = get(c, CURRENT_BUFFER);
        if (!(get(c, VALID_BUFFER) & (1u << current)))
            return; // waiting for the title to refill
        InputBuffer in = input(c, current);
        if (in.bits < PACKET_BITS) {
            finish_buffer(c);
            continue;
        }
        u32 pos = get(c, OFFSET_READ);
        if (pos < HEADER_BITS)
            pos = first_frame(in);

        // Loop back once the loop end is reached.
        u32 loops = get(c, NUM_LOOPS);
        u32 loop_end = get(c, LOOP_END);
        if (loops && loop_end && pos >= loop_end) {
            if (loops != INFINITE_LOOPS)
                set(c, NUM_LOOPS, loops - 1);
            pos = get(c, LOOP_START);
            set(c, OFFSET_READ, pos);
            continue;
        }

        if (pos + 15 > in.bits) {
            finish_buffer(c);
            continue;
        }
        u32 at = pos;
        u32 length = read_bits(in, &at, 15);
        if (length == FINAL_FRAME) {
            finish_buffer(c);
            continue;
        }
        if (length <= 15) {
            set(c, PARSER_ERROR_STATUS, 4); // bitstream corruption
            set(c, PARSER_ERROR_SET, 1);
            set_bit(g_xma.enabled, index, false);
            return;
        }
        u32 next = advance(in, pos, length);

        // Room for the whole frame? (One block always stays free, so a full ring can't look empty.)
        u32 write = get(c, OFFSET_WRITE), read = get(c, WRITE_BUFFER_OFFSET_READ);
        u32 used = (write + blocks - read) % blocks;
        u32 skip = bit(g_xma.skip_pending, index) ? get(c, SUBFRAMES_TO_SKIP) : 0;
        u32 out_samples = FRAME_SAMPLES - (skip < 4 ? skip : 4) * SUBFRAME_SAMPLES;
        u32 frame_blocks = out_samples * channels * (u32)sizeof(s16) / BLOCK_BYTES;
        if (used + frame_blocks >= blocks)
            return;

        decode_frame(in, pos, length, channels, samples);
        const u8* src = (const u8*)(samples + (FRAME_SAMPLES - out_samples) * channels);
        for (u32 b = 0; b < frame_blocks; b++, write = (write + 1) % blocks)
            memcpy(ring + (usize)write * BLOCK_BYTES, src + (usize)b * BLOCK_BYTES, BLOCK_BYTES);
        set(c, OFFSET_WRITE, write);
        set_bit(g_xma.skip_pending, index, false);

        if (next >= in.bits)
            finish_buffer(c);
        else
            set(c, OFFSET_READ, next);
    }
}

void xma_run() {
    AcquireSRWLockExclusive(&g_xma.lock);
    for (u32 w = 0; g_xma.contexts && w < (g_xma.count + 31) / 32; w++)
        for (u32 bits = g_xma.enabled[w]; bits; bits &= bits - 1) {
            u32 index = w * 32;
            unsigned long b;
            _BitScanForward(&b, bits);
            run_context(index + b);
        }
    ReleaseSRWLockExclusive(&g_xma.lock);
}
