# Texture unit 0 MVP

The bridge accepts three 3DS Morton-tiled formats and expands them to linear
RGBA8 before GX2 upload. It supports complete 8×8 tiles with dimensions from
8 to 1024, divisible by eight. Tiles are stored in row-major order, with x/y
bits interleaved inside each tile. No vertical flip is applied; guest UVs must
use the same row convention.

| `TextureFormat` | Register value | Source bytes per texel | Decoding |
| --- | --- | --- | --- |
| `RGBA8888` | 0 | 4 | Guest bytes A,B,G,R become output R,G,B,A |
| `RGB565` | 3 | 2 | LE packed R5:G6:B5, replicated to 8-bit channels, alpha 255 |
| `RGBA4444` | 4 | 2 | LE packed R4:G4:B4:A4, each nibble expanded by ×17 |

The pixel layouts follow the [Azahar format decoder](https://github.com/azahar-emu/azahar/blob/master/src/common/color.h).
All decoding uses byte accesses, so PPC byte order and unaligned guest pixels
cannot change channel reconstruction. Raw texel bytes obtained via `GetPointer`
must not be byte-swapped a second time.

## Decoder interface

```cpp
Deswizzle3DSTexture(source_pointer, destination_pointer, width, height, format);
Deswizzle3DSTexture(source_span, destination_span, width, height, format);
```

Every destination is **width × height × 4 bytes**, including 16-bit inputs.
`TextureSourceSize` returns the required source byte count after validating
dimensions/format. The span overload validates capacities and rejects overlap.
The raw-pointer signature cannot discover allocation sizes; its caller must
provide sufficient buffers. Invalid dimensions, null buffers, overlapping data,
unsupported formats and insufficient spans throw instead of decoding.

## Register and vertex contracts

| Guest address | MVP value |
| --- | --- |
| `0x1EF00B00` | Texture FCRAM virtual address; zero disables texturing |
| `0x1EF00B04` | Width in low 16 bits, height in high 16 bits |
| `0x1EF00B0C` | Format ID from the table above |

These addresses and dimension fields continue the **custom MVP bridge**. They
are not a complete implementation of retail PICA texture registers, address
encoding, texture environment stages, or samplers. GSP stream IDs for these
addresses are `0x02C0`, `0x02C1`, and `0x02C3`.

A textured vertex has LE float3 position at offset 0, float4 color at offset 12,
and **float2 UV at offset 28**. Its stride must be at least 36 bytes, divisible
by four. UVs are normalized, sampled with point filtering and clamp-to-edge.
Existing 28-byte vertices remain valid when texture address is zero.

Draw triggers snapshot texture address, dimensions and format alongside geometry
and uniforms. All referenced FCRAM ranges are checked before batch submission.
Source image bytes are read during submission; mutating guest memory concurrently
with command execution is unsupported. Setting an invalid nonzero texture address
or unsupported layout fails the draw rather than silently sampling another image.

## GX2 path

`GX2Manager::BindTexture0` consumes tightly packed **linear RGBA8** and currently
accepts `GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8`. The decoder handles packed source
formats before this call. GX2 calculates image size, alignment and pitch for a
linear-aligned surface. The runtime verifies row capacity, copies rows using
the calculated pitch, invalidates with `GX2_INVALIDATE_MODE_CPU_TEXTURE`, and
binds the texture/sampler at pixel unit zero.

Allocations remain owned through `GX2DrawDone`; changing textures during a frame
does not free images referenced by earlier draws. Texture bindings are restored
for both TV and DRC contexts. Frame completion clears stale descriptors after
freeing uploads; subsequent guest draws rebind their captured texture state.

The textured fetch/vertex variant exports color and UV while retaining the matrix
transform. The pixel variant samples unit zero and computes
`outColor = inColor * texture(texSampler, inUV)` with four MUL instructions.
An untextured shader variant preserves existing color-only fixtures. Shader
encoding references: [Latte instruction fields](https://github.com/decaf-emu/decaf-emu/blob/master/src/libgpu/latte/latte_instructions.h).

Only one base texture level is uploaded. Mipmaps, ETC compression, other PICA
formats, texture caches, filtering/wrapping registers, TEV stages and arbitrary
shader translation remain unimplemented.

## Verification

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX /I wiiu_runtime\tests\stubs /I wiiu_runtime\include wiiu_runtime\tests\texture_tests.cpp wiiu_runtime\src\texture_decoder.cpp wiiu_runtime\src\pica200.cpp wiiu_runtime\src\gsp_gpu.cpp wiiu_runtime\src\display_transfer.cpp /Fo:build\ /Fe:build\texture_tests.exe
build\texture_tests.exe
```

Tests check the 16×16 checkerboard at (0,0), (7,7), (8,0) and every other texel;
a non-square gradient; 16-bit reconstruction; output guards; invalid buffers;
deferred texture enable/disable; and encoded shader operands/multiplication.
The GX2 test substitute records uploads and draws. It does not validate actual
GPU sampling, interpolation or texture orientation. Cemu/Wii U visual testing
remains necessary. The existing triangle generator does not upload a texture;
its RPX build validates that the untextured fixture still compiles with this path.
