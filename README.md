# 3DS2U

For architectural handoff and safe maintenance, start with the
[maintainer guide](docs/maintainer-guide.md),
[security and privacy policy](SECURITY.md), and
[contribution workflow](CONTRIBUTING.md). Agent-specific repository rules are
in [AGENTS.md](AGENTS.md).

3DS2U is an experimental **3DS ARM11 to Wii U PowerPC ahead-of-time (AOT)
static recompiler**. A PC tool converts supported ARM/Thumb instructions into
C++20 basic-block functions. A devkitPPC/wut runtime dispatches those functions
on Wii U and provides guest memory, graphics, input, and placeholder OS services.

The current deliverable is a visual MVP and buildable `.rpx`, not a general
3DS emulator or a production-compatible game converter. Successful translation
and linking do not establish that a real game boots or that GPU output is correct.

## Repository map and workflow

The host pipeline is split into `extractor/` and `translator/`. The extractor
turns supported 3DSX/CIA/NCSD inputs into bounded executable sections,
metadata and an embedded RomFS. The translator decodes selected ARM32,
Thumb-16/Thumb-32 and VFP forms, builds a mode-aware CFG and emits C++20.
`wiiu_runtime/` supplies the Wii U dispatcher and the project-specific memory,
graphics, input, audio, filesystem and HLE bridges. Detailed invariants and
handoff procedures are in [docs/maintainer-guide.md](docs/maintainer-guide.md).

Use synthetic fixtures and decrypted `.code`/ExHeader test inputs that you are
authorized to use. Do not commit keys, seeds, firmware, commercial binaries,
personal logs or generated build output. Run host validation before cross
compilation, keep unified builds serial, and distinguish host, Cemu and
hardware evidence. See [AGENTS.md](AGENTS.md) for contributor and AI-agent
requirements.

## Architecture and features

```text
uncompressed .code + decrypted ExHeader
              |
PC: loader -> ARM32/T16 decoder -> CFG -> C++ emitter
              |
       translated_game.cpp
              |
    devkitPPC + wut -> application.rpx
              |
Wii U/Cemu: block dispatcher + memory + GX2 + VPAD + HLE
```

| Component | Current implementation |
| --- | --- |
| ARM32 | Selected ALU operations, CPSR NZCV and conditional execution, B/BL/BX/BLX, immediate word LDR/STR, LDM/STM |
| Thumb-16 | Immediate arithmetic, register ALU operations, word load/store, PUSH/POP, branches, explicit mode ranges |
| VFP / T32 | Scalar single/double VFPv2 math, conversions, transfers and FPSCR; mixed T16/T32 fetch, wide calls, selected wide ALU and byte/word memory forms |
| CRO modules | Checked CRO0 segment mapping, named/indexed imports, ARM/Thumb relocations, shared heap reservations and explicit RO HLE helpers; execution requires pretranslated blocks |
| Display transfer | RGBA8/RGB8/RGB565 conversion, linear/Morton blits and separate top-screen TV / bottom-screen DRC presentation |
| Loader | Uncompressed ExeFS `.code` plus separate decrypted ExHeader; text/rodata/data and BSS mapping; raw instruction fixtures |
| Extraction/package | Native CIA/NCSD + shared NCCH/CTR/BLZ extraction and version-zero 3DSX relocation, generated section metadata, embedded Level-3 RomFS and a two-argument build pipeline |
| Memory | Selectable 128/256 MiB O3DS/N3DS FCRAM aligned to 4 KiB; bounded accesses; guest little-endian data swapped on big-endian PPC |
| Graphics | MVP PICA200-to-GX2 bridge: clear color, vertex buffer, stride/count, triangle draws; float24 uniforms, default matrix vertex shader and color pixel shader |
| GPU command buffers | PICA packet parser, byte masks, FIFO/consecutive writes, FINALIZE and deferred draw snapshots through the MVP bridge |
| Textures | 8×8 Morton deswizzling for RGBA8/RGB565/RGBA4444, linear RGBA8 GX2 upload, UV/color-modulated sampling on texture unit 0 |
| TEV | Six-stage source/modifier/constant tracking, Replace/Multiply/Add/Subtract, fixed GX2 combiner programs and per-draw uniform blocks through project register aliases |
| Input | VPAD buttons/dual sticks, N3DS ZL/ZR and C-Stick; HID/IRRST ring snapshots, calibrated 320×240 touch and readable aliases |
| HLE / audio | srv/apt placeholders; explicit CSND/DSP playback helpers; 16 AX voices, PCM8/PCM16 conversion, configured DSP ADPCM and synthetic volume/pitch MMIO |
| RomFS / FS | Prioritized base/DLC Level-3 layers from buffers/files, Unicode paths, read-only RomFS plus persistent title-scoped SaveData/ExtData, explicit fs:USER/file IPC handlers |
| DLC discovery | Configured per-game Title IDs, explicit `aoc:u` count/list shim and service-port dispatch |
| Kernel SVC | ARM/T16 interception, model-aware page heap, CoreInit worker affinity on cores 0/1/2, address arbitration and thread/event waits |
| Dispatch | ARM/Thumb-aware block lookup, mode exchange, bounded execution slices and yield after draw commands |

The graphics path does **not** translate arbitrary PICA200 shaders or command
lists. HID, graphics and audio register protocols include project-specific MVP
registers. Service methods are not a general SVC/IPC interception layer.

## Requirements

- Windows Command Prompt for the master `.cmd` workflow.
- A C++20 host compiler. The commands below use MSVC in an **x64 Native Tools
  Command Prompt for Visual Studio 2022**.
- devkitPro with **devkitPPC**, **wut**, **elf2rpl**, **GNU make**, and its MSYS
  shell installed.
- Cemu or Wii U homebrew hardware for runtime validation.
- An uncompressed `.3dsx` or supported `.cia`/`.3ds`/`.cci` with required user-supplied
  keys/seeds. Manual translation accepts `.code` plus its decrypted ExHeader.

The master script uses these environment variables:

| Variable | Default |
| --- | --- |
| `DEVKITPRO` | `%SystemDrive%/devkitPro` |
| `DEVKITPPC` | `%DEVKITPRO%/devkitPPC` |
| `WUT_ROOT` | `%DEVKITPRO%/wut` |
| `MAKE_EXE` | `%DEVKITPRO%/msys2/usr/bin/make.exe` |

Use native Windows paths for overrides. The conventional MSYS
`/opt/devkitpro` and `/opt/devkitpro/devkitPPC` defaults are converted to the
default Windows installation paths. Quote paths containing spaces.

```bat
set "DEVKITPRO=C:/devkitPro"
set "DEVKITPPC=C:/devkitPro/devkitPPC"
set "WUT_ROOT=C:/devkitPro/wut"
```

## Build the PC translator

From the repository root in the MSVC developer command prompt:

```bat
if not exist build mkdir build
cl /nologo /std:c++20 /EHsc /W4 /WX /I translator\include translator\src\*.cpp /Fo:build\ /Fe:build\3ds2u_translator.exe
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /I extractor\include /I wiiu_runtime\include extractor\src\*.cpp wiiu_runtime\src\romfs_reader.cpp /Fo:build\ /Fe:build\extractor.exe
```

The unified pipeline requires both executables; it does not rebuild host tools.
Rebuild the corresponding tool after changing its sources.

## Translator CLI

```bat
build\3ds2u_translator.exe game.code game.cpp --exheader game.exheader
```

For raw ARM instruction fixtures without section metadata:

```bat
build\3ds2u_translator.exe color_test.code color_test.cpp
```

For known Thumb text or mixed ARM/Thumb ranges:

```bat
build\3ds2u_translator.exe game.code game.cpp --exheader game.exheader --thumb
build\3ds2u_translator.exe game.code game.cpp --exheader game.exheader --thumb-range 0x08000010:0xC
```

Repeat `--thumb-range address:size` for additional nonoverlapping ranges.

Add `--split-instructions N` (N from 1) to write `translated_game_blocks_NNNN.cpp` units of about N
instructions next to the main file instead of one huge source. Copy the main file and every
`*_blocks_*.cpp` into `wiiu_runtime/src/` together (the unified build does this for you).
Addresses and sizes accept decimal or `0x` notation. ExHeader metadata does not
identify ARM/Thumb boundaries or literal pools; ranges must be supplied explicitly.

The CLI logs input/output paths, section sizes, instruction/block counts and
entry point. It embeds initialized sections and initializes their zero-filled
tails in the generated dispatcher. See [loader details](docs/exefs-loader.md)
and [Thumb support](docs/thumb-support.md).

## Unified build

```bat
build_3ds2u_app.cmd game.3dsx game_wiiu.rpx
build_3ds2u_app.cmd game.cia game_wiiu.rpx
build_3ds2u_app.cmd game.3ds game_wiiu.rpx
build_3ds2u_app.cmd game.cci game_wiiu.rpx
```

CIA/NCSD inputs load required user-supplied `keys.txt` entries from the project root; see [native CIA extraction](docs/cia-extraction.md).

The two arguments are the input 3DSX/CIA/NCSD and an output filename ending in `.rpx`,
without directory components. Quote paths containing spaces. Input paths resolve
relative to the caller; the script builds from the project root.

The pipeline checks prerequisites, extracts text/rodata/data (relocating 3DSX),
generates section metadata, translates executable text, embeds the appended RomFS,
and builds with devkitPPC/wut. Outputs are `build/game_wiiu.rpx` and a diagnostic
`build/game_wiiu.romfs` sidecar. RomFS is embedded and mounted at startup, so the
RPX does not depend on locating the sidecar in Cemu or on an SD card.

Extraction writes `extracted.code`, `extracted.exheader`, `extracted.romfs` and
`extracted_romfs.cpp` in the project root. The script passes the generated
ExHeader to the translator to keep data and BSS out of instruction decoding.
It installs generated code as `wiiu_runtime/src/translated_game.cpp` and assets as
`wiiu_runtime/src/generated_romfs.cpp`. An absent RomFS produces an empty package
and clears the preceding build's assets. Unknown formats, invalid relocations,
extraction, translation, copy and compilation failures stop with nonzero status.
An older published RPX may remain after failure; check the final success message.

Host tools must already be built. The default pipeline assumes ARM text; real
Thumb ranges, literal pools, libctru startup and service IPC remain compatibility
limits. See [extraction/package details](docs/extraction-pipeline.md). For manual
`.code` builds, run the translator with `--exheader`, install its generated source,
then invoke `make -C wiiu_runtime all`. Manual builds use any installed generated
RomFS package, or the empty fallback when that file is absent.

Only `wiiu_runtime/src/*.cpp` are compiled; the root `translated_game.cpp` is a
historical fixture. Objects and the intermediate ELF live in `build/runtime/`.
Builds must run serially. Changes to toolchain paths or flags require fresh objects.

## Reproducible triangle package

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX generate_triangle_test.cpp /Fo:build\ /Fe:build\generate_triangle_test.exe
build\generate_triangle_test.exe
build\3ds2u_translator.exe triangle_test.code output_translated.cpp --exheader triangle_test.exheader
copy /Y output_translated.cpp wiiu_runtime\src\translated_game.cpp
"%DEVKITPRO%/msys2/usr/bin/make.exe" -C wiiu_runtime all
copy /Y build\runtime\wiiu_runtime.rpx build\triangle_demo.rpx
```

The generator writes a 52-byte ARM text section and three position/color vertices
in a separate data page. The guest registers its vertex buffer and repeatedly
triggers a triangle draw. The resulting standalone test package is
`build/triangle_demo.rpx`; section bytes and default shaders are embedded, so the
input `.code`/ExHeader files are not needed when launching that RPX.

## Testing in Cemu

1. Start Cemu and open **Options → Input settings**. Configure Controller 1 as
   **Wii U GamePad**, select an available input device, and map/save its buttons.
   See the [controller configuration guide](https://cemu.cfw.guide/controller-configuration.html).
2. Select **File → Load** and open `build/triangle_demo.rpx` (or your generated
   RPX). This workflow loads an executable directly, rather than installing a
   title. The [Cemu wiki](https://wiki.cemu.info/wiki/Old_Troubleshooting)
   documents the File → Load action.
3. Inspect the TV output and GamePad view for the triangle. The program is
   intended to keep drawing until stopped. Rendering remains a Cemu/hardware
   validation step; passing a build alone does not prove the shaders render.
4. For an input-aware guest fixture, press mapped buttons and exercise the
   GamePad touchscreen. The triangle fixture itself does not consume input.
   See [HID register layout](docs/hid-mvp.md).
5. If startup fails or output is blank, inspect Cemu's `log.txt` for `[3DS2U]`
   and HLE messages, especially an unmapped ARM/Thumb PC or memory exception.
   Keep needed logs locally; redact local paths, device/account identifiers and
   proprietary input metadata before sharing diagnostics.
6. Stop emulation through Cemu when finished. The native audio demo plays a
   440 Hz tone; guest playback requires the explicit HLE/MMIO audio bridge.

No successful Cemu visual/input validation is claimed by this package. A
user-reported Cemu 2.6 attempt ended on a missing translated block; see the
[redacted runtime-validation record](docs/runtime-validation.md). For native
hardware, launch the RPX using your existing Wii U homebrew environment.

## Tests and implementation notes

Tests are under `translator/tests` and `wiiu_runtime/tests`. They cover branch
targets, conditions, ALU flags, addressing modes, loader metadata, T16 operations,
geometry MMIO, HID and audio. Tests execute emitted C++ and use host substitutes
for Wii U platform APIs; they are not a replacement for hardware validation.

For example, to run the production audio service/memory paths on an MSVC host:

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX /I wiiu_runtime\include /I wiiu_runtime\tests\audio_platform /I wiiu_runtime\tests\stubs wiiu_runtime\tests\audio_stub_tests.cpp wiiu_runtime\src\hle_services.cpp wiiu_runtime\src\aoc_manager.cpp wiiu_runtime\src\save_manager.cpp wiiu_runtime\src\audio_driver.cpp wiiu_runtime\src\cro_loader.cpp wiiu_runtime\src\romfs_reader.cpp wiiu_runtime\src\wiiu_memory.cpp wiiu_runtime\src\hid_manager.cpp /Fo:build\ /Fe:build\audio_stub_tests.exe
build\audio_stub_tests.exe
```

Additional architecture notes: [graphics bridge](docs/graphics-mvp.md),
[GSP command buffers](docs/gsp-command-buffers.md),
[vertex uniforms](docs/vertex-uniforms.md),
[texture pipeline](docs/textures.md),
[RomFS and file services](docs/romfs.md), [DLC service and layers](docs/aoc-layering.md), [persistent saves](docs/save-data.md),
[SVC runtime](docs/svc-runtime.md), [N3DS expansion](docs/n3ds-expansion.md),
[VFP and T32](docs/vfp-thumb32.md),
[CRO modules and RO HLE](docs/cro-modules.md),
[display transfer and dual screens](docs/display-transfer.md),
[native AX audio](docs/audio-driver.md),
[3DSX extraction and packaging](docs/extraction-pipeline.md), [CIA decryption](docs/cia-extraction.md), [NCSD cartridges](docs/ncsd-extraction.md),
[TEV combiner](docs/tev-combiner.md),
[HID bridge](docs/hid-mvp.md), and [audio stubs](docs/audio-stubs.md).

## Current compatibility limits

- The extractor accepts version-zero 3DSX and documented CIA/NCSD/NCCH layouts
  with caller-supplied keys/seeds. Direct NCCH/CXI, TWL/SHA1, personalized tickets,
  ELF and compressed 3DSX remain unsupported; BLZ `.code` is handled during CIA extraction.
- Instruction support is partial. IT and unimplemented T32 forms fail translation.
  Some unhandled ARM VFP forms currently log a warning and may be emitted as
  comments (no-ops); see [VFP/T32 limitations](docs/vfp-thumb32.md) and
  [issue #11](https://github.com/Wiirizon/3DS2U/issues/11). Automatic code/data
  separation and reachable-code discovery are absent.
- Guest memory uses a flat O3DS 128 MiB or N3DS 256 MiB window starting at
  `0x08000000`. General MMU mappings and full system services remain incomplete.
- The emitted dispatcher starts at the entry parsed from the ExHeader `.text` (or
  `0x08000000` for raw code and 3DSX). A missing translated block at that entry fails
  translation; a runtime miss halts with mode, key and block count. Host tests use
  synthetic fixtures only. A user-reported Cemu 2.6 run mounted the packaged RomFS
  and then halted on a missing translated block; the cause is unknown and no
  successful Cemu visual/input or Wii U hardware run has been established. See the
  [redacted runtime-validation record](docs/runtime-validation.md).
- The geometry bridge expects LE float3 position plus float4 color and custom
  draw registers. It does not implement retail PICA command streams or shaders.
- Native AX playback supports explicit mono buffer submissions. Full CSND command
  parsing, DSP firmware/descriptor handling, gapless queues and general service
  IPC are not implemented; real game audio is not automatically intercepted.
- Execution is cooperatively sliced rather than synchronized to a complete 3DS
  interrupt, VBlank, GPU or OS scheduling model.
