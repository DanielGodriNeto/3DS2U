# GSP display transfers and dual-screen presentation

`DisplayTransfer::ExecuteTransfer` accepts guest FCRAM source/destination addresses,
packed dimensions (`width | height << 16`) and native GX transfer flags. It supports
RGBA8, RGB8 and RGB565 in either direction, vertical flipping, and complete 8x8
Morton tiles. Source and destination dimensions must match, and each dimension is
limited to 1024. AA downsampling, cropping, raw texture-copy mode, 32x32 tiling,
RGB5A1 and RGBA4 are rejected before changing memory.

Pixel decoding is bytewise: 3DS RGBA8 storage is **A,B,G,R**, RGB8 storage is
**B,G,R**, and RGB565 is a little-endian packed word. RGB8 and BGR8 are different
names used for the same native format ID 1; there is no extra RGB-order format ID.
Canonical GX2 upload data is R,G,B,A. No native casts or blanket 32-bit byte swaps
are applied to packed three-byte pixels. Conversion is staged before writing the
destination, including when source and destination overlap.

| Flags | Meaning |
| --- | --- |
| Bit 0 | Flip destination vertically |
| Bit 1 | Input linear; otherwise 8x8 Morton |
| Bit 5 | Preserve layout; otherwise exchange linear/Morton |
| Bits 8–10 | Input format: 0 RGBA8, 1 RGB8, 2 RGB565 |
| Bits 12–14 | Output format, same IDs |

For example, `0x1022` converts linear ABGR8 into linear BGR8. `0x1000` converts
tiled ABGR8 into linear BGR8. Tiled surfaces require width and height divisible by
eight. Physical VRAM addresses and general guest virtual/physical translation are
outside this runtime's fixed FCRAM mapping.

## GX packets

`GSPGPU::ProcessGXCommand(memory, packet_address)` reads one eight-word LE GX
packet, separate from the PICA register stream handled by `ProcessCommandBuffer`:

```text
word 0: control/type (low byte = 0x03)
word 1: source address
word 2: destination address
word 3: source dimensions
word 4: destination dimensions
word 5: transfer flags
word 6,7: zero
```

Native DisplayTransfer has command ID **0x03**, while **0x01** submits a PICA
command list. These IDs and dimension/flag packing follow
[libctru GX submission](https://github.com/devkitPro/libctru/blob/master/libctru/source/gpu/gx.c)
and [GX definitions](https://github.com/devkitPro/libctru/blob/master/libctru/include/3ds/gpu/gx.h).
To consume the earlier MVP's requested 0x01 transfer packet explicitly, pass
`GSPGPU::GXDialect::MVPDisplayTransfer01`; native dispatch never guesses packet type.
Pixel byte order and layout flags were checked against the
[PICA external-register declarations](https://github.com/azahar-emu/azahar/blob/master/src/video_core/pica/regs_external.h).

The runtime main attaches a display-aware GSPGPU to `HLEServiceManager`.
`GSPGPU_SubmitGXCommand` is an explicit HLE helper returning zero on success.
Full shared-memory GX queue consumption, IPC session routing and completion-event
delivery remain separate work. Submit on the main execution/GX2 owner core while
in the foreground, under the engine execution lock when guest threads are active.

## TV and DRC

Linear transfer destinations of 400x240 feed TV; 320x240 feed DRC. Native sideways
240x400 and 240x320 buffers are rotated into those upright sizes. This MVP uses
dimensions to recognize displays; `SetFramebuffer(Top/Bottom, address)` restricts
updates to explicitly selected destinations. Automatic interpretation of real GSP
LCD swap state, arbitrary pitch and stereoscopic right-eye buffers is not included.
Offscreen transfers with display-sized dimensions therefore require explicit
framebuffer selection to avoid presentation.

Each screen owns a persistent RGBA8 snapshot, independent of later FCRAM writes or
the other screen. `EndFrame` uploads the snapshots as textures and draws separate
letterboxed quads into distinct GX2 color/depth/context targets: 1280x720 TV and
854x480 DRC. Register-mode shaders use an identity matrix for the presentation
quads; the guest matrix is restored afterward. The scan buffers are configured
with `GX2SetTVBuffer`/`GX2SetDRCBuffer` during the first end pass, then copied and
swapped each frame without resetting pending flips. GPU uploads survive until
`GX2DrawDone`. Direct MVP triangle draws retain their previous mirrored behavior
when display snapshots are absent.

## Tests

From an MSVC developer prompt at the project root:

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX /I wiiu_runtime\tests\stubs /I wiiu_runtime\include wiiu_runtime\tests\display_transfer_tests.cpp wiiu_runtime\src\display_transfer.cpp wiiu_runtime\src\gsp_gpu.cpp wiiu_runtime\src\pica200.cpp wiiu_runtime\src\texture_decoder.cpp /Fo:build\ /Fe:build\display_transfer_tests.exe
build\display_transfer_tests.exe
```

Tests cover every pixel in a 16x16 RGBA8-to-RGB8 image, boundary sentinels, all four
layout combinations, RGB565 expansion, flip/overlap behavior, invalid ranges and
flags, native/legacy GX dispatch, HLE submission and independent rotated screens.
The tests use host memory/GX2 substitutes; they do not execute real GX2 shaders.

`wiiu_runtime/tests/display_transfer_demo.cpp` is a standalone native wut test:
TV should show a red checkerboard, DRC a green checkerboard, with black aspect
bars. It exercises the transfer/HLE/GX2 presentation path without translated ARM
code. Build output `build/display_transfer_demo.rpx` is for Cemu/hardware testing;
visual validation has not been performed here.

Build it with `make -C wiiu_runtime display-demo` in the configured devkitPro
environment, then open `build/display_transfer_demo.rpx` in Cemu and enable its
GamePad view to inspect the bottom screen.
