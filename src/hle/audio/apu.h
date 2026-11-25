#pragma once

#include "common/types.h"

// The audio hardware's memory and its XMA decoder, shared by the AcpHal pieces.

// Host pointer for an APU address, or nullptr outside the APU heap.
void* apu_ptr(u32 apu_address);

// --- XMA decoding contexts (SHAPE) ---------------------------------------------------------------
//
// Titles program XMA contexts in an array the hardware owns: input double buffers of 2 KB packets,
// an output ring of 16-bit samples in 256-byte blocks, and loop points. The decoder consumes input
// as fast as the output ring has room, which is how titles pace it. Commands from the audio control
// processor update the contexts; xma_run decodes.

#define XMA_CONTEXT_SIZE 64

void xma_set_contexts(void* contexts, u32 count); // nullptr: released
void xma_enable(u32 index, bool enable);

// ACP updates: `context` points to a full context whose fields named in `mask` are copied
// (SHAPE XMA update flags).
void xma_update(u32 index, u32 mask, const void* context);
// Compact updates: replace the read buffer pointers and sizes and the output read offset, and add
// the given input-buffer valid bits.
void xma_update_entry(u32 index, u32 ptr_read0, u32 ptr_read1, u32 size_read0, u32 size_read1,
                      u32 write_buffer_offset_read, u32 valid_buffer);
void xma_increment_write_read(u32 index, u32 increment);
void xma_update_read_buffer(u32 index, u32 buffer, u32 size, bool change_size, bool change_address,
                            u32 address);

// Decodes into every enabled context as far as its input and output allow.
void xma_run();
