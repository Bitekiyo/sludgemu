# Third-party code

## Included in this repository

| Files | From | License |
|-------|------|---------|
| `src/hle/audio/xma_decoder.cpp`, `xma_decoder.h`, `xma_tables.h` | XMA (WMA Pro) decoder based on [FFmpeg](https://ffmpeg.org/) `libavcodec/wmaprodec.c`, `wmaprodata.h`, `wma.c`, `libavutil/float_dsp.c`, `libavcodec/sinewin_tablegen.h`; reduced to XMA and adapted (see the file headers) | LGPL-2.1-or-later |

These files keep their original license. Their headers name the authors and list what was changed.

## Fetched at build time

| Dependency | Used for | License |
|------------|----------|---------|
| [libdeflate](https://github.com/ebiggers/libdeflate) | zlib/deflate decompression for DirectStorage requests | MIT |
| [Opus](https://github.com/xiph/opus) | Opus decoding (XApu) | BSD-3-Clause |
| [AMD addrlib](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/amd/addrlib) (Mesa) | GPU surface tiling | MIT |
| Microsoft.Direct3D.D3D12 (Agility SDK) | Direct3D 12 runtime | Microsoft redistributable license |
| Microsoft.Direct3D.DirectStorage | DirectStorage runtime | Microsoft redistributable license |
| Microsoft.Direct3D.DXC | Shader compiler | Microsoft redistributable license / University of Illinois NCSA |
