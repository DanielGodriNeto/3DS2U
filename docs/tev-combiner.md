# TEV combiner and static GX2 fragment pipeline

`TEVCombiner` tracks six stages. The requested `[0x1EF00C00,0x1EF00D00)` window
is a **project MMIO alias**, not the native PICA register-address mapping.
Each 0x10-byte stage record contains source, modifier, operator and scale words.
Six constant colors follow at `0x1EF00C60 + stage*4`. Other words in the bank are
retained for masked readback. Constants use R in bits 0–7 and A in bits 24–31.

Fields within each record follow the PICA encodings documented by the
[PICA texturing declarations](https://github.com/azahar-emu/azahar/blob/master/src/video_core/pica/regs_texturing.h):

| Word | Fields |
| --- | --- |
| Sources | RGB A/B/C at bits 0/4/8; alpha A/B/C at bits 16/20/24; four bits each |
| Modifiers | RGB at bits 0/4/8; alpha at bits 12/16/20; alpha fields are three bits |
| Operators | RGB bits 0–3; alpha bits 16–19 |
| Scales | RGB bits 0–1; alpha bits 16–17; values 0/1/2 mean x1/x2/x4 |

Supported sources are PrimaryColor=0, Texture0=3, Constant=14 and Previous=15.
Supported RGB modifiers are source color=0, complement=1, source alpha=2 and
complemented alpha=3; alpha modifiers are source alpha=0 and complement=1.
Supported operators are Replace=0, Multiply=1, Add=2 and Subtract=5. Replace is
also used to make unused stages pass through Previous. Third operands are tracked
but not consumed by this subset's one/two-input operations. Used unsupported
fields and reserved scales throw before any geometry in that command list draws.

The default stage zero modulates Texture0 with PrimaryColor in both RGB and
alpha; stages 1–5 replace with Previous. In the untextured shader, Texture0 is
white. The initial Previous value is PrimaryColor. This preserves the bridge's
earlier untextured/texture-modulated behavior without requiring new guest writes.

## Uniform block and shader

The fragment program is a fixed compile-time-generated Latte instruction array;
there is no per-draw program generation or external shader compiler. Six stages
are unrolled. Uniforms select behavior through one-hot source weights and affine
modifier coefficients. Each stage has eleven vec4s:

| Rows | Values |
| --- | --- |
| 0–3 | Operand A: RGB source weights, alpha source weights, RGB modifier, alpha modifier |
| 4–7 | Operand B, same layout |
| 8 | Normalized constant RGBA |
| 9 | RGB coefficients for `A*x + B*y + A*B*z` |
| 10 | Alpha coefficients for the same expression |

Source weights index Primary, Texture0, Constant and Previous. The RGB modifier
is `{colorWeight, alphaWeight, sign, bias}`; the alpha modifier is
`{sign,bias,0,0}`. For example, Multiply uses operation coefficients `{0,0,1,0}`,
while Subtract uses `{1,-1,0,0}`. Stage results are clamped to [0,1].

`BindTEVState` builds this block. Each draw uploads a separate 256-byte-aligned
snapshot to PS binding 0 with `GX2SetPixelUniformBlock`. It also uploads the
existing row-major projection/modelview matrix to VS binding 1. Both vertex and
pixel programs now use GX2 uniform-block mode. CPU/uniform cache invalidation
precedes use, and uploads remain owned until `GX2DrawDone`. TV and DRC context
switches rebind both blocks. LCD presentation quads use a default combiner and
restore the guest configuration afterward.

The pixel shader uses KCache source selectors, short ALU clauses, a texture
sample when UV input is present, and a final RGBA export. The machine-code
fields were checked against [Latte instruction definitions](https://github.com/decaf-emu/decaf-emu/blob/master/src/libgpu/latte/latte_instructions.h)
and [opcode definitions](https://github.com/decaf-emu/decaf-emu/blob/master/src/libgpu/latte/latte_instructions_def.inl).
The aligned block binding follows [wut's shader API](https://github.com/devkitPro/wut/blob/master/include/gx2/shaders.h).

`Pica200` copies TEV state into each queued draw alongside texture and vertex
uniform state. Changing registers later cannot alter earlier draws. Batch
validation builds every TEV block before submission, so unsupported state in a
later request cannot partially draw the earlier requests.

## Validation and limits

`tev_tests.cpp` verifies the Texture0 × PrimaryColor uniform encoding explicitly,
then compares the **emitted machine code** against an independent reference for
400 randomized six-stage configurations. A host interpreter implements the used
Latte control-flow/OP2 subset, including VLIW simultaneous read/write semantics.
Tests include independent RGB/alpha sources, modifiers, constants, scales,
masked writes and deferred draw snapshots. Texture regression tests also execute
the active TEV shader rather than the historical four-multiply reference.

From an MSVC developer prompt:

```bat
cl /nologo /std:c++20 /O2 /EHsc /W4 /WX /I wiiu_runtime\tests\stubs /I wiiu_runtime\include wiiu_runtime\tests\tev_tests.cpp wiiu_runtime\src\tev_combiner.cpp wiiu_runtime\src\pica200.cpp wiiu_runtime\src\texture_decoder.cpp /Fo:build\ /Fe:build\tev_tests.exe
build\tev_tests.exe
```

Build `make -C wiiu_runtime tev-demo` in the configured devkitPro environment to
create `build/tev_demo.rpx`. It draws an 8x8 white/blue texture multiplied by a
green primary color, then adds a red constant in stage one. Expected output is
a green/purple checkerboard on TV and DRC. The native shader's actual rendering
in Cemu or on hardware is **not verified** by the host interpreter or compiler.

This is a basic float/clamp TEV subset, not bit-exact PICA fixed-point arithmetic.
Texture units 1–3, fragment lighting inputs, previous-buffer feedback, LERP, DOT3,
add-signed, multiply-add, fog, alpha test and logic/blend state remain unsupported
or outside this combiner. Native retail PICA register routing also remains
separate from the project aliases and custom geometry/uniform banks. No claim
is made that this enables Super Mario 3D Land or another commercial title to boot.
