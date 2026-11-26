# BCPack stream format

BCPack is the lossless codec DirectStorage on Xbox Series uses for BCn texture
data.  It does not compress bytes; it re-expresses every block as small
symbols (endpoint deltas, index deltas, reference distances) and entropy-codes
those with rANS.  This note describes the decoder side as implemented in
`src/hle/dstorage/bcpack.cpp`.  Constant tables come from `bcpack_tables.inc`
(extracted from the reference codec).

## Stream envelope

There is no header.  The caller knows the BCn format (1-7) and the decoded
size; the block count is `decoded size / block size` (8 bytes for BC1/BC4, 16
otherwise).  The packed size is not part of the format: the reference decoder
never looks at it, our decoder only uses it to bound reads.

## Lanes and records

A stream carries a fixed number of symbol *lanes* per format.  Each lane
produces `k` symbols per block (k = 1 for BC1/BC4, 2 otherwise), so a block is
described by a *record* of `lanes * k` symbols, lane-major (lane i supplies
record bytes `i*k .. i*k+k-1`).

| format | lanes | k | record |
|--------|-------|---|--------|
| BC1    | 12    | 1 | 12     |
| BC2    | 15    | 2 | 30     |
| BC3    | 12    | 2 | 24     |
| BC4    | 12    | 1 | 12     |
| BC5    | 12    | 2 | 24     |
| BC6H   | 16    | 2 | 32     |
| BC7    | 16    | 2 | 32     |

Every lane has a *class* c (3..7, table `k_lane_class_bcN`); its symbols have
at most c + 1 bits.

## Multiplexing the lanes over one byte stream

All lanes read from one byte stream.  Decoding runs in *rounds*; a round
visits the lanes in order 0..n-1:

1. A lane that wants a byte while 8 bytes have already been handed out in
   this round skips the round.
2. A lane that holds a decoded symbol it already delivered for the current
   record position skips the round (it waits for the slower lanes).
3. A lane that wants a byte and still owes symbols gets the next byte.
4. A lane holding an undelivered symbol stores it at the current position.
5. The lane takes one step (below).

After the round, if every lane delivered its symbol for the current position,
the position advances.  Decoding ends when the last position is complete.

"Owes symbols" is a 32-bit count of symbols still expected by that lane.  It is
decremented per decoded symbol and keeps going below zero: a lane may decode
one symbol beyond its quota (never stored) and then wants bytes again.  The
encoder models all of this, so a decoder must reproduce it exactly or the byte
assignment drifts.

## A lane: fragments and rANS

A lane's bytes form *fragments*:

```
header   bits 0-3  distribution d (0..15)
         bit  4    reset: state restarts at 0 after this fragment
         bit  5    long length
         bits 6-7  r: symbols use class (lane class - r)
length   short: 1 byte, payload = byte + 1
         long:  2 bytes (low, high), payload = value + 257
payload  that many bytes, fed into the rANS state
```

The rANS state is 32 bits, initially 0.  In the payload phase each step:

* if state < 2^24 and the lane holds a byte, `state = state << 8 | byte`
  (one payload byte used);
* the lane wants a byte if payload bytes remain and it holds none;
* if state <= 2^24: if payload remains, keep going; otherwise the fragment
  ends and the next byte is a new header.  On fragment end the state is *not*
  updated with a byte shifted in during that same step, and it carries over
  into the next fragment unless the reset flag was set (then it becomes 0);
* if state > 2^24: decode one symbol.

So a fresh state loads four bytes before the first symbol, and a lane may
decode its last symbols after the payload is used up.  (A state of exactly
2^24 with payload left never makes progress; only a corrupt stream can do
that.)

### Distributions

Probabilities have 16-bit precision.  Symbols of a class-c distribution fall
into buckets 0..c+1: bucket 0 is symbol 0, bucket k > 0 holds the 2^(k-1)
symbols [2^(k-1), 2^k), all with the same frequency `f_k`.  A bucket starts at
`s_k = sum of earlier buckets' slots`; the buckets fill exactly 65536 slots.
There are 8 classes x 16 distributions (`k_rans_buckets`).

Decoding one symbol from state x:

```
slot = x & 0xFFFF
k    = last bucket with s_k <= slot
r    = (slot - s_k) / f_k
k == 0: symbol 0,           x' = (x >> 16) * (f_0 - 1) + slot - 1
k  > 0: symbol 2^(k-1) + r, x' = (x >> 16) * f_k + (slot - s_k) - r * f_k
```

Symbol 0 owns slots 1..f_0-1 only; slot 0 is never produced by the encoder.

## Rebuilding blocks

### References

Every record starts with two reference selectors.  A block is the sum of the
record's deltas and two *reference* blocks: one supplies the endpoints, the
other the indices.  References are distances `d` back from the current block
b:

* d = 0: an all-zero reference (BC6H: a fixed "mode 1 / mode 11" zero entry);
* d <= b: the reconstructed working values of block b - d (a 256-entry ring
  is enough, d <= 255);
* d > b: predefined dictionary entry `d - b - 1` (255 entries per format,
  `k_predef_*`), i.e. the dictionary acts as the blocks before the stream.

The index reference distance is `di` and the endpoint distance is
`(di + unzigzag(z)) & 255` with a zigzag-coded relative value z.  BC1 and BC4
store z and di directly as symbols 0 and 1; the other formats split each over
two 4-bit symbols (first symbol = even bits, second = odd bits): z from
record bytes 0-1, di from bytes 2-3.

Deltas are zigzag coded (0, -1, 1, -2, ...).  Adding a reference wraps each
field at its own width, so working values never carry between fields.

### BC1-BC5

Working values keep every field in a bit lane, so a reference is added with
one lane-wise add per word.  Index fields are filled from per-format symbol
tables: a *quad* symbol gives the four 2-bit indices of a 2x2 texel group
(`k_quad_*`), a *pair* symbol gives two 3-bit indices of neighboring texels
(`k_pair_*`).

Stored indices count along the endpoint line (0 = first endpoint, max =
second endpoint); BCn numbers the second endpoint 1 and interpolants from 2, so
on output 0 stays, max becomes 1 and the rest increase by one.  Alpha endpoints
and BC2 alpha values are stored centered (output = value ^ 0x80 / ^ 0x8).

| format | record bytes |
|--------|--------------|
| BC1 | 0 z, 1 di, 2-7 R0 G0 B0 R1 G1 B1, 8-11 index quads (texel groups 0, 2, 8, 10) |
| BC2 | 0-3 refs, 4-19 alpha per texel (4-bit), 20-25 R0 R1 G0 G1 B0 B1, 26-29 quads |
| BC3 | 0-3 refs, 4-5 alpha endpoints, 6-13 alpha index pairs, 14-19 R0 R1 G0 G1 B0 B1, 20-23 color quads |
| BC4 | 0 z, 1 di, 2-3 endpoints, 4-11 index pairs |
| BC5 | 0-3 refs, 4-7 R0 R1 G0 G1, 8-15 red pairs, 16-23 green pairs |

Colors are RGB565 deltas (5/6/5-bit wrap); alpha endpoints wrap at 8 bits.

### BC6H

Record: 0-3 refs; 4-9 the base endpoint e0 of R, G, B, each a 16-bit value
interleaved over two symbols; 10-15 e1 of R, G, B (same); 16-21 e2/e3 of R,
G, B (two-region modes only); 22-29 index deltas; 30 mode - 1 (modes 1-14);
31 shape (partition).

Endpoints are kept left-aligned in 16 bits: a delta is unzigzagged and shifted
up by `16 - precision`, where the precisions per mode (`k_bc6_precision`) are
{base, R delta, G delta, B delta} bits.  Adding the reference is a 16-bit add
masked to the precision, so references of any mode mix freely.  Single-region
modes only code e0/e1; e2/e3 are restated as copies (e2 = e0 at the delta
precision, e3 = e1).  For the block, each endpoint becomes
`((value ^ 0x8000) >> (16 - precision))`.

Index deltas: each of eight symbols holds two 4-bit zigzag values, one in the
odd bits and one in the even bits.  Symbols 22-27 list the odd-bit value
first, symbols 28-29 the even-bit value first.  Indices are kept signed and
centered (texel index = value + 2^(bits-1)); adding the reference aligns both
to the top of 16 bits, adds, and shifts back by the current width, which also
rescales a reference with a different index width.

The block is assembled from fields (shape, 12 endpoint values, packed indices
with the anchor texels' top bits removed) using per-mode bit layouts
(`k_bc6_runs`: runs of consecutive field bits, some reversed).  The layouts
are learned from the reference implementation by the generator.

### BC7

Symbols 30 and 31 form a 10-bit value that selects a *configuration*
(`k_bc7_config`): mode, rotation, index selection, subset count and partition.
Only the 285 valid BC7 configurations exist; anything else is a corrupt
stream.  Endpoint precisions per channel come from `k_bc7_precision[mode |
rotation << 3]`; in p-bit modes the p-bit is carried as the low bit of the red
value (red has one bit more).

Endpoint deltas (record 4-21), per subset `[R0 R1 G0 G1 B0 B1 A0 A1]`:
three-subset modes use `{4-9}, {12-17}, {20 21 10 11 18 19}` (no alpha), other
modes `{4-11}` and `{12-19}`.  Values are left-aligned by `16 - precision`.
Adding the reference masks the reference to the current precision; if the
reference has fewer subsets, its first subset serves the extra ones.
(Dictionary entries have a subset count of 0, so they always lend subset 0.)

Indices: set 0 from symbols 22-29, set 1 (modes 4 and 5) from 12-17, 20, 21,
with the same odd/even split as BC6H.  Values are sign-extended raw indices;
adding a reference works as for BC6H.

The block is written in the standard BC7 order: mode bit, partition or
rotation/index-selection, R, G, B (and A) endpoint strings, p-bits, indices
(anchor texels lose their top bit; for three subsets the third subset's anchor
is removed before the second's).  Rotated blocks swap the alpha string with the
rotated channel's.  Mode 4 writes the 2-bit index set (set 1) first.

## Robustness notes

The reference decoder trusts its input.  `bcpack_decode` additionally returns
false when:

* the stream would need bytes past `src_size` (DLL-made streams never do);
* lanes stop making progress (the reference would spin forever);
* a BC6H mode symbol is outside 1-14 or a BC7 configuration symbol pair is not
  one of the 285 valid ones (the reference reads unrelated tables then);
* `dst_size` is not a whole number of blocks (the reference silently decodes
  the whole blocks and leaves the tail).
