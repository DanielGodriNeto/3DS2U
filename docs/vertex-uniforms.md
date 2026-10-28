# Vertex uniforms and float24

`wiiu::float24_to_float32` accepts a numeric PICA float24 word (s1/e7/m16,
exponent bias 63). It rebiases the exponent, shifts the fraction, preserves signed
zero and infinity/NaN payload bits, and normalizes subnormals losslessly.
Conversion uses `std::bit_cast`, with no aliasing or host byte-order assumptions.
PICA arithmetic flushing of subnormals is not part of the conversion helper.

| Raw float24 | Float32 value |
| --- | --- |
| `0x000000` | +0.0 |
| `0x800000` | -0.0 |
| `0x3F0000` | 1.0 |
| `0x3F8000` | 1.5 |
| `0x400000` | 2.0 |
| `0xBF0000` | -1.0 |
| `0x7F0000` | +infinity |

The suggested identity value `0x3F8000` is not 1.0 in PICA format. See the
[libctru float32-to-float24 encoder](https://github.com/devkitPro/libctru/blob/master/libctru/source/gpu/gpu.c)
and [Azahar float format description](https://github.com/azahar-emu/azahar/blob/master/src/video_core/pica_types.h).

## MVP register bank

The bridge holds 96 vec4s (384 scalar words), starting at `0x1EF00280`.
Component `i` is written at `0x1EF00280 + 4*i`; its low 24 bits are float24.
Components run in order x, y, z, w. Byte-masked command writes preserve disabled
bytes before updating the converted value.

The original `0x1EF00280–0x1EF002DF` window fits only 24 scalar words, or six
vec4s. The same direct mapping now extends through `0x1EF0087F` (last scalar
starts at `0x1EF0087C`) to hold all 96 vectors. Stream register IDs are word
indices, so this bank uses IDs `0x00A0–0x021F`.

This is a **project-specific MVP mapping**, not the real PICA indexed/packed
uniform upload protocol. Real shader/FIFO decoding and vertex-program emulation
remain separate work. Existing real register IDs can conflict with this MVP bank.

`Pica200::GetVertexUniforms()` returns a contiguous `std::array<float, 384>`.
Every draw trigger captures its own converted array alongside vertex geometry.
Later uniform writes cannot change an already queued draw. On submission the
bridge calls `GX2Manager::SetVertexUniforms(data, 384)` before drawing.

## GX2 matrix convention

The first four vec4s are the rows of a combined modelview/projection mat4.
The default shader computes `clip_position = matrix * vec4(position.xyz, 1)`.
Color is passed through unchanged. The default matrix is identity so existing
triangle fixtures continue to work without uploading uniforms. Other uniforms
are stored but are not referenced by this default shader.

`SetVertexUniforms` counts **scalar floats**, requires a nonzero multiple of four
up to 384, and runs during an active GX2 frame. It caches native numeric floats.
The [TEV pipeline](tev-combiner.md) now snapshots the first 16 scalars into an
aligned VS uniform block at binding 1 with `GX2SetVertexUniformBlock`, replacing
the earlier register-mode upload. Blocks are rebound after each TV/DRC context
switch and retained until GPU completion. The default vertex program uses KCache
operands in `GX2_SHADER_MODE_UNIFORM_BLOCK`, alongside the TEV pixel block at
binding 0. Remaining stored vertex uniforms are reserved for future shaders.
See [wut shader interfaces](https://github.com/devkitPro/wut/blob/master/include/gx2/shaders.h).

The embedded Latte vertex program uses four DOT4 groups and exports the computed
R3 position. Programs remain serialized little-endian for GPU execution.
The float24 input and Float32 uniform API each consume numeric values, avoiding
an extra swap on big-endian PowerPC. The command parser continues to fetch guest
words through the endian-aware memory abstraction.

## Verification

`gsp_gpu_tests.cpp` checks signed zeros, corrected identity/1.5 encodings,
infinities/NaNs, all positive/negative subnormal mantissas, and representative
normal mantissas across every exponent. It also checks all 384 uniform slots are
addressable, per-draw state retention, converted array propagation, and the
encoded DOT4 program against identity and translated/scaled matrix examples.
The host shader check is a restricted instruction evaluator, not GPU execution.
Rendering/perspective behavior still requires Cemu or Wii U verification.
