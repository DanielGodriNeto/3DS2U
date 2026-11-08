# Triangle graphics bridge

The geometry MMIO protocol is a project-specific MVP interface, not a full PICA200
register/command-processor implementation.

| Register | Meaning |
| --- | --- |
| `0x1EF00000` | Clear color, `0x00RRGGBB` |
| `0x1EF00200` | Guest virtual address of the vertex array |
| `0x1EF00204` | Byte stride, at least 28 and divisible by four |
| `0x1EF00208` | Vertex count, divisible by three |
| `0x1EF0020C` | Write `1` to draw |

Each vertex starts with little-endian float3 XYZ at offset 0 and little-endian
float4 RGBA at offset 12. The fetch shader supplies W=1; the vertex shader
transforms position with the uploaded matrix (identity by default).
Extra stride padding is allowed, up to 2044 bytes. Pointers must be
word aligned. The complete `count * stride` range is checked against FCRAM before
it is exposed to GX2.

When texture unit 0 is enabled, append float2 UV at offset 28 (stride at least
36 bytes). The textured shader multiplies vertex color by a sampled texel;
see [textures](textures.md) for the register and pixel format contracts.

The GX2 manager copies guest LE bytes into aligned per-frame uploads and uses
`GX2_ENDIAN_SWAP_NONE` for both attributes. Uploads stay alive through
`GX2DrawDone`. Minimal embedded vertex/pixel programs export position and pass
color through; the vertex shader now applies a row-major mat4 from c0–c3,
defaulting to identity. See [vertex uniforms](vertex-uniforms.md).
A GX2-generated fetch shader binds the two attributes. No GLSL
compiler, GFD asset, or proprietary SDK shader is required. Shader words in
`default_shaders.hpp` are serialized LE before GPU cache invalidation.

The runtime clears both outputs before guest execution, draws to TV and GamePad,
and yields the CPU slice at a draw-command block boundary. The test loops over
the trigger store, producing one draw per presented frame. This is cooperative
MVP scheduling, not a complete GPU timing model.

```text
build\generate_triangle_test.exe
build\3ds2u_translator.exe triangle_test.code build\triangle_test.cpp --exheader triangle_test.exheader
```

`generate_triangle_test.cpp` writes uncompressed ExeFS-style text/data pages and a
decrypted ExHeader. `.text` is 52 bytes at `0x08000000`; `.data` is 84 bytes at
`0x08001000`. The emitter embeds initialized section bytes in C++ and initializes
the runtime memory during dispatcher setup, including zeroed mapped page tails.
Guest VAs still need to fit the runtime's fixed FCRAM window; this does not add a
general guest MMU mapper.

The standalone triangle build uses `build/triangle_test.cpp` in place of the root
`translated_game.cpp`, preserving the existing color-test source. Build/link and
host tests do not establish that native shaders render correctly in Cemu or on
hardware; visual validation is still required.

Public encoding references: [Latte instruction fields](https://github.com/decaf-emu/decaf-emu/blob/master/src/libgpu/latte/latte_instructions.h),
[Latte opcode definitions](https://github.com/decaf-emu/decaf-emu/blob/master/src/libgpu/latte/latte_instructions_def.inl),
and [SPI shader registers](https://github.com/decaf-emu/decaf-emu/blob/master/src/libgpu/latte/latte_registers_spi.h).

## Supported PICA200 subset

Everything not listed here is outside the MVP. The bridge records such writes and,
in strict mode, rejects them. By default they stay silent unless the runtime reads
the counter; no runtime code reads it yet:

- `Pica200::UnsupportedWriteCount()` counts writes that are unsupported. The count
  saturates at `UINT32_MAX`. `HasUnsupportedWrite()` and
  `FirstUnsupportedAddress()` record the first unsupported address.
- `SetStrictRegisters(true)` makes an unsupported write throw
  `std::invalid_argument` with the message
  `Unsupported PICA200 register write at 0x<ADDRESS>`. The address is recorded
  and the throw happens before any register state changes. The default is `false`.
- In default mode nothing else changes: misaligned writes are ignored, unknown
  aligned registers are retained for readback, and a draw value other than 1 is
  retained but does not draw.

Counted as unsupported (only these three cases):

1. An in-window address (`0x1EF00000`–`0x1EFFFFFE`) that is not 4-byte aligned.
2. An aligned in-window address that has no defined behaviour, outside the TEV
   and uniform ranges and outside the registers listed below.
3. A write to the draw register `0x1EF0020C` with byte 0 enabled and a merged
   value other than 1.

Not counted: addresses below `0x1EF00000` or at/above `0x1EFFFFFF`, the audio
bridge range `0x1EC10000`–`0x1EC17FFF`, and aligned writes to every register
listed below.
Known gaps in the counter: a draw write of 1 with a zero vertex count, or a draw
write without byte 0, is silently ignored and not counted. TEV fields that are
used but unsupported are not counted at write time; they throw when the draw is
built, as described in [TEV combiner](tev-combiner.md).

### Registers handled

| Address | Behaviour |
| --- | --- |
| `0x1EF00000` | Clear color, `0x00RRGGBB`; alpha is 1 |
| `0x1EF00200` | Vertex buffer guest address |
| `0x1EF00204` | Vertex stride in bytes (default 28) |
| `0x1EF00208` | Vertex count |
| `0x1EF0020C` | Draw trigger, value `1` (see below) |
| `0x1EF00280`–`0x1EF0087C` | 384 uniform words, step 4; low 24 bits are float24 |
| `0x1EF00B00` | Texture unit 0 address; zero disables texturing |
| `0x1EF00B04` | Texture unit 0 dimensions: width in bits 0–15, height in bits 16–31 |
| `0x1EF00B0C` | Texture unit 0 format (see the table below) |
| `0x1EF00C00`–`0x1EF00CFC` | TEV window: all 64 words are readable and writable; only the fields below are consumed |

Aligned writes to the TEV window and to the uniform range are never counted;
misaligned ones are (case 1).

### TEV fields accepted

- Stage records (sources, modifiers, operators, scales) are at `0x1EF00C00` +
  `stage*0x10`, and the stage constants are at `0x1EF00C60` + `stage*4`.
- Sources: PrimaryColor=0, Texture0=3, Constant=14, Previous=15.
- RGB modifiers: 0–3. Alpha modifiers: 0–1.
- Operators: Replace=0, Multiply=1, Add=2, Subtract=5. Third operands are stored
  but not consumed.
- Scales: 0, 1 or 2 (x1, x2, x4).

Any other used field throws in `BuildUniformBlock`, before the draw is submitted.

### Draw protocol

Writing `1` to `0x1EF0020C` with byte 0 enabled draws using the most recent vertex
buffer address, stride, count, uniforms, texture unit 0 state and TEV state. A
count of zero does nothing. Between `BeginCommandList` and `EndCommandList` the
draws are queued and validated together; `EndCommandList` submits them in order.

### Vertex layouts accepted

`ValidateDraw` accepts a buffer only if all of these hold:

- The address is 4-byte aligned.
- The stride is between 28 and 2044 bytes and divisible by 4.
- The count is divisible by 3.
- `count * stride` fits in 32 bits and lies in the FCRAM range.

Layouts:

- Untextured (texture address 0): float3 position at offset 0, float4 color at
  offset 12.
- Textured (texture address nonzero): the same, plus float2 UV at offset 28.
  The stride must be at least 36.

### Texture formats

Texture unit 0 accepts complete 8×8 tiles, with dimensions from 8 to 1024 that
are divisible by 8. Formats are:

| Format | Register value | Source bytes per texel |
| --- | --- | --- |
| `RGBA8888` | 0 | 4 |
| `RGB565` | 3 | 2 |
| `RGBA4444` | 4 | 2 |

Any other format value fails the draw.

### Not supported

- Shader programs: no PICA shader upload or command-processor decoding. The
  fixed fetch, vertex and fragment shaders are the only programs.
- Command-list packets: there is no packet decoder. `BeginCommandList` and
  `EndCommandList` are host API calls, not guest FIFO packets.
- Indexed draws: there is no index buffer register.
- Other texture stages: texture units 1–3 and the other texture unit 0 words,
  such as `0x1EF00B08`, are unsupported. An unknown aligned write to these is
  counted.
- Fragment lighting, fog, alpha test, blend and logic state, and LERP, DOT3,
  add-signed or multiply-add combiner modes.

Real PICA register IDs can land on the uniform or TEV aliases. Those writes are
then handled as MVP state and are not counted. Writes outside the MMIO window are
not PICA and are not counted.

Host verification for the counter and strict mode:

```text
g++ -std=c++20 -static -Wall -Wextra -Werror -I wiiu_runtime/tests/stubs -I wiiu_runtime/include -o build/pica200_tests.exe wiiu_runtime/tests/pica200_tests.cpp wiiu_runtime/src/pica200.cpp wiiu_runtime/src/tev_combiner.cpp wiiu_runtime/src/texture_decoder.cpp
build/pica200_tests.exe
```

This test is not yet part of `scripts/run_host_tests.cmd`.
