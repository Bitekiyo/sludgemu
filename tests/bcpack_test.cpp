// BCPack decoder robustness: streams are title data, so malformed ones must fail cleanly, never read
// past the stream or write past the blocks. (Exactness is checked against the reference codec
// outside this repository: its output can't be shipped here.)

#include <string.h>
#include <windows.h>

#include "hle/dstorage/bcpack.h"
#include "test.h"

// `size` bytes ending right at a no-access page, so any overrun faults.
struct Guarded {
    u8* base;
    u64 reserved;
    u8* data;
};

static Guarded guarded_alloc(u64 size) {
    Guarded g = {};
    u64 pages = (size + 4095) / 4096;
    g.reserved = (pages + 1) * 4096;
    g.base = (u8*)VirtualAlloc(nullptr, g.reserved, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g.base)
        return g;
    DWORD old;
    VirtualProtect(g.base + pages * 4096, 4096, PAGE_NOACCESS, &old);
    g.data = g.base + pages * 4096 - size;
    return g;
}

static void guarded_free(Guarded* g) {
    if (g->base)
        VirtualFree(g->base, 0, MEM_RELEASE);
}

static u64 g_rng = 0x9E3779B97F4A7C15ull;

static u64 next_random() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

void test_bcpack() {
    u8 block[16] = {};
    // arguments
    CHECK(!bcpack_decode(0, block, sizeof(block), block, 16));
    CHECK(!bcpack_decode(8, block, sizeof(block), block, 16));
    CHECK(!bcpack_decode(2, block, sizeof(block), block, 8)); // BC2 blocks are 16 bytes
    CHECK(!bcpack_decode(1, nullptr, 16, block, 8));
    CHECK(bcpack_decode(1, nullptr, 0, nullptr, 0)); // no blocks: nothing to do

    // Random and truncated streams for every format and many sizes: any outcome but a fault.
    for (u32 format = 1; format <= 7; format++) {
        u32 block_size = format == 1 || format == 4 ? 8 : 16;
        for (u32 trial = 0; trial < 300; trial++) {
            u64 blocks = 1 + next_random() % 300;
            u64 src_size = next_random() % 4096;
            Guarded src = guarded_alloc(src_size ? src_size : 1);
            Guarded dst = guarded_alloc(blocks * block_size);
            CHECK(src.data && dst.data);
            if (!src.data || !dst.data)
                return;
            for (u64 i = 0; i < src_size; i++)
                src.data[i] = (u8)next_random();
            // some streams start like plausible fragments: small headers and lengths
            if (trial % 3 == 0 && src_size >= 2)
                for (u64 i = 0; i < src_size; i += 64)
                    src.data[i] &= 0x1F;
            bcpack_decode(format, src.data, src_size, dst.data, blocks * block_size);
            guarded_free(&src);
            guarded_free(&dst);
        }
    }
}
