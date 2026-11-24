# Architecture

## Process model

`sludgemu.exe` is a normal GUI-subsystem Windows program. It maps the title into its own
process; there is no separate launcher process and no hosted view. To the user the title looks
like any native game: it creates its own top-level window, which we turn into a resizable
desktop window with borderless fullscreen on F11 / Alt+Enter.

## Loader (`src/loader`)

The title is mapped by hand instead of through the native loader, because we need full control
over import resolution and over the address space:

1. Map headers and sections at the image's preferred base (relocate only if that fails), so
   addresses match the disassembler.
2. Register `.pdata` with `RtlAddFunctionTable` so SEH, C++ exceptions and stack walks work.
3. Set up implicit TLS (see below).
4. Bind imports. For each imported DLL name:
   1. **HLE module**: a console system DLL implemented by sludgemu (`xmem.dll`,
      `d3d12_xs.dll`, ...). Exports we haven't written resolve to logging stubs.
   2. **Title-local DLL**: a DLL shipped in the package, mapped recursively by the same loader.
      The VC++ runtime DLLs are the exception: they always come from the host.
   3. **Host DLL**: loaded from System32 / API sets. Individual functions can be replaced by
      *host overrides*, e.g. `LoadLibraryExW`, `GetProcAddress` and `GetModuleHandleW` (which
      must know about our mapped modules) or `CreateWindowExA` (window management).
5. Apply section protections, run DLL entry points, then the title's TLS callbacks and entry
   point.

Every unresolved import becomes a stub that logs `module!function called from <address>` with
the address in disassembler terms, and returns 0. Nothing fails silently.

### TLS

Windows only allocates implicit TLS for natively loaded images. `sludge_tls.dll` is loaded
natively and owns a slot with a large static TLS block. The title's `_tls_index` is pointed at
that slot. On every thread attach, the launcher copies the title's TLS template into the block
and runs the title's TLS callbacks.

## Console subsystems (planned)

| Area | Console side | Desktop side |
|---|---|---|
| Memory | title-managed virtual/physical pages, GPU sees CPU addresses (unified memory) | reserved address ranges + tracking so GPU resources can be shadowed in D3D12 heaps |
| Graphics | console D3D12 (`d3d12_xs`), RDNA2 shader binaries, console swapchain/present | desktop D3D12; shaders recompiled at runtime (RDNA2 ISA → IR → DXIL via DXC, disk cache); our swapchain scales the title's output to the window |
| Texture layout | console tiling/swizzle modes (`xg_xs`) | detiling on upload/readback |
| Ray tracing | console BVH builder | DXR acceleration structures |
| Audio | DSP/APU/SHAPE hardware | software implementation feeding XAudio2/WASAPI |
| Storage | console DirectStorage with hardware decompression | software decompression + file I/O |
| Input | GameInput (console) | desktop GameInput / XInput / keyboard+mouse |

## Debugging conventions

- Log addresses in title images as `module+rva [preferred-base address]`, so they can be pasted
  into a disassembler directly.
- Graphics problems are investigated with a frame capture (RenderDoc / Nsight Graphics), not by
  guessing.
