# GPU memory: page tables

Console titles share one memory between CPU and GPU, and the GPU reaches it through page tables:
an address is a page of 64 KiB anywhere in physical memory, the same frame can sit at several
addresses (titles alias render target memory, move texture pages around, map pools twice), and
memory can be GPU-only. Desktop GPUs have video memory of their own, reached through resources.

This layer gives the title's GPU addresses page tables of their own, in video memory.

## Page tables

Title GPU address space is covered by **segments**: reserved buffers of 4 GiB starting every 2 GiB
(segment *k* covers `[k * 2 GiB, k * 2 GiB + 4 GiB)`). Desktop buffer views can't start more than
4 GiB into their resource, so a single buffer can't cover a large range; with overlapping segments,
any access of up to 2 GiB starting in granule *k* lies inside segment *k*. Segments are created when
the GPU first reaches their granule.

A page of title memory the GPU reads or writes is backed by a **tile** of video memory
(`gpu/tile_pool.h`) and mapped into the segments that reach it. The tile belongs to what backs the
page, not to the address:

- in ranges of physical memory, the **frame** mapped there: aliases share the tile, so a frame
  mapped at two addresses is the same memory for the GPU, as on console;
- in other graphics ranges (title allocations that aren't physical memory), the page itself.

Translating a title GPU address (`gpu_translate`) gives the segment and the offset in it. Pages
get their tiles when a translation first reaches them; mapping happens on the queue of the next
submission, before it runs. When the title maps other frames at an address or unmaps it, the
segments follow at the next submission, and tiles nothing maps any more go back to the pool once
the GPU is done with them.

Memory the GPU never reaches as a buffer gets no tile of its own.

## Textures

Desktop textures lay texels out their own way, so they are resources of their own, but their memory
follows the same pages:

- **Render targets, depth and UAV textures** are reserved textures whose tiles map onto the tiles of
  the frames under them. Titles alias such memory (a render target in one pass, a buffer or another
  target in the next, with contents undefined across the switch), and on the desktop it is then the
  same video memory too: the GPU takes what the console's takes. Their contents never need title
  memory.
- **Textures the GPU only reads** get their contents from title memory, where files and the CPU put
  them; their tiles hold the desktop layout and are mapped where the title mapped memory
  (`hle/d3d12/texture_sync`). Their data is in RAM once and in video memory once.

## Coherence

A page with a tile has two copies, the tile and title memory. Which one is current:

- **CPU writes** (title stores, file reads, DirectStorage requests, the kernel) go to title memory.
  Pages with tiles are write-watched (`mem_watch_writes`); the first write after an upload marks
  the page, and the next submission uploads it before the title's work.
- **GPU writes** go to the tile. Pages work may write (copy destinations, unordered access views,
  query resolves) are taken from the CPU after the submission (no access); the first CPU access
  waits for the GPU and downloads the tile.
- **Pages the CPU polls** while the GPU writes them (fence values, counters written at the end of
  work) stay in title memory: the GPU writes them through a window over the page, so the CPU sees
  each value as the GPU writes it.

Memory both CPU and GPU use is resident twice; memory only one of them uses, once.

## System memory

Physical memory no longer needs to be laid out so that what is contiguous for the title is
contiguous in RAM, since the GPU doesn't read it in place: frames are bound densely, and only what
the CPU (or a device writing for it) touches is backed by RAM.

## Status

Being built in steps, each one working:

1. Pages the CPU can't access until a listener brings them up to date (the memory core).
2. Segments, tiles per frame or page, translation through segments, uploads of CPU writes,
   GPU-written pages taken from the CPU and downloaded on access, windows for polled pages; page
   table work runs on a queue of its own that title submissions wait for.
3. Texture uploads read title memory only (no windows over texture memory).
4. Render target, depth and UAV textures on frame tiles.
5. Dense frame binding in system memory.

Measured before, in a title's loading screen: 8.2 GB of physical memory sections held
3.9 GB of data, render target memory was backed in RAM by buffer views over it (2.4 GB), and video
memory allocations count towards the process's commit as well (Windows charges them in full).
