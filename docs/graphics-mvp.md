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
