// Detiles one subresource of a console texture: reads its elements from title memory (through a
// window over it) where the console layout puts them, and writes them in the desktop footprint layout
// (rows of row_pitch bytes, depth slices of slice_pitch), for a copy into the desktop texture.
//
// Console layouts (see ConsoleSubresourceAddressing in gpu/console_layout.h): swizzled ones put
// elements in blocks, row after row of blocks, volumes block slice after block slice; inside a block,
// an element's place is the XOR of a fixed address pattern for every set bit of its coordinates,
// counted from the mip's place in the mip tail, with the array slice's pattern XORed in. Linear ones
// are rows of `pitch` bytes.
//
// Threads: one per element for elements of 4 bytes and more, one per destination dword (several
// elements) for 1 and 2-byte elements, so every store is a whole dword.

#define ROOT_SIGNATURE                                                                                       \
    "RootConstants(num32BitConstants=36, b0), SRV(t0, flags=DATA_STATIC_WHILE_SET_AT_EXECUTE), UAV(u0)"

struct Params {
    uint base;            // the subresource's base, from the start of `src`
    uint row_pitch;       // destination
    uint slice_pitch;     // destination
    uint width;           // in elements
    uint height;
    uint depth;
    uint element_shift;   // log2 of the bytes per element
    uint is_linear;
    uint pitch;           // linear: bytes per row
    uint src_slice_pitch; // linear volumes: bytes per depth slice
    uint shifts;          // block shifts: x | y << 5 | z << 10 | bytes << 15
    uint row_blocks;
    uint slice_stride;    // volumes: bytes from one block slice to the next
    uint block_xor;
    uint tail;            // x | y << 10 | z << 20
    uint units;           // per destination row: elements, or dwords for small elements
    uint4 bits[5];        // 16-bit patterns: x bits 0-13, y bits 14-27, z bits 28-39
};

ConstantBuffer<Params> p : register(b0);
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);

uint pattern_bits(uint first, uint count, uint v) {
    uint r = 0;
    for (uint bit = 0; bit < count && (v >> bit) != 0; bit++) {
        if ((v >> bit) & 1) {
            uint i = first + bit;
            r ^= (p.bits[i >> 3][(i >> 1) & 3] >> ((i & 1) * 16)) & 0xffff;
        }
    }
    return r;
}

uint element_address(uint x, uint y, uint z) {
    if (p.is_linear)
        return p.base + z * p.src_slice_pitch + y * p.pitch + (x << p.element_shift);
    uint sx = p.shifts & 31, sy = (p.shifts >> 5) & 31, sz = (p.shifts >> 10) & 31, sb = (p.shifts >> 15) & 31;
    uint tx = p.tail & 1023, ty = (p.tail >> 10) & 1023, tz = p.tail >> 20;
    uint block = (y >> sy) * p.row_blocks + (x >> sx);
    uint in_block = pattern_bits(0, 14, x + tx) ^ pattern_bits(14, 14, y + ty) ^ pattern_bits(28, 12, z + tz) ^
                    p.block_xor;
    return p.base + (z >> sz) * p.slice_stride + (block << sb) + in_block;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= p.units)
        return;
    uint row = id.z * p.slice_pitch + id.y * p.row_pitch;
    uint shift = p.element_shift;
    if (shift >= 2) {
        uint at = element_address(id.x, id.y, id.z);
        uint to = row + (id.x << shift);
        if (shift == 2)
            dst.Store(to, src.Load(at));
        else if (shift == 3)
            dst.Store2(to, src.Load2(at));
        else
            dst.Store4(to, src.Load4(at));
        return;
    }
    // 1 and 2-byte elements: the elements of one destination dword.
    uint per = 4u >> shift;
    uint bits = 8u << shift;
    uint mask = (1u << bits) - 1;
    uint value = 0;
    for (uint j = 0; j < per; j++) {
        uint x = id.x * per + j;
        if (x >= p.width)
            break;
        uint at = element_address(x, id.y, id.z);
        uint word = src.Load(at & ~3u);
        value |= ((word >> ((at & 3) * 8)) & mask) << (j * bits);
    }
    dst.Store(row + id.x * 4, value);
}
