#pragma once

#include "common/types.h"

// BCPack decoder: the lossless BCn texture codec of Xbox Series DirectStorage.
//
// bc_format: 1-7 for BC1-BC7.
// src/src_size: one packed stream (the whole compressed payload of a request).
// dst/dst_size: the decoded BCn blocks; dst_size must be a multiple of the block
//               size (8 bytes for BC1/BC4, 16 otherwise).  The block count is
//               implied by dst_size; the stream itself carries no header.
//
// Returns false for invalid arguments or a malformed stream (never reads past
// src_size or writes past dst_size).  On failure dst holds a partial result.
//
// No heap allocation.  Uses about 26 KB of stack at most (BC7: two 256-entry
// history rings plus a small record batch).  Thread-safe: no shared state.
bool bcpack_decode(u32 bc_format, const void* src, u64 src_size, void* dst, u64 dst_size);
