# 3DS2U

3DS2U is an experimental **3DS ARM11 to Wii U PowerPC ahead-of-time (AOT)
static recompiler**. A PC tool converts supported ARM/Thumb instructions into
C++20 basic-block functions. A devkitPPC/wut runtime dispatches those functions
on Wii U and provides guest memory, graphics, input, and placeholder OS services.

The current deliverable is a visual MVP and buildable `.rpx`, not a general
3DS emulator or a production-compatible game converter. Successful translation
and linking do not establish that a real game boots or that GPU output is correct.

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
| Extraction/package | Version-zero 3DSX extraction and relocation, generated section metadata, embedded Level-3 RomFS and a two-argument build pipeline |
| Memory | 128 MiB FCRAM buffer aligned to 4 KiB; bounded accesses; guest little-endian data swapped on big-endian PPC |
| Graphics | MVP PICA200-to-GX2 bridge: clear color, vertex buffer, stride/count, triangle draws; float24 uniforms, default matrix vertex shader and color pixel shader |
| GPU command buffers | PICA packet parser, byte masks, FIFO/consecutive writes, FINALIZE and deferred draw snapshots through the MVP bridge |
| Textures | 8×8 Morton deswizzling for RGBA8/RGB565/RGBA4444, linear RGBA8 GX2 upload, UV/color-modulated sampling on texture unit 0 |
| TEV | Six-stage source/modifier/constant tracking, Replace/Multiply/Add/Subtract, fixed GX2 combiner programs and per-draw uniform blocks through project register aliases |
| Input | VPAD buttons mapped to 3DS masks; calibrated touch scaled to 320×240; custom readable HID registers |
| HLE / audio | srv/apt placeholders; explicit CSND/DSP playback helpers; 16 AX voices, PCM8/PCM16 conversion, configured DSP ADPCM and synthetic volume/pitch MMIO |
| RomFS / FS | Prioritized base/DLC Level-3 layers from buffers/files, Unicode paths, read-only RomFS plus persistent title-scoped SaveData/ExtData, explicit fs:USER/file IPC handlers |
| DLC discovery | Configured per-game Title IDs, explicit `aoc:u` count/list shim and service-port dispatch |
| Kernel SVC | ARM/T16 interception, image-aware page heap, CoreInit workers, address arbitration and thread/event waits |
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
- An uncompressed `.3dsx` homebrew container for the unified pipeline. Manual
  translation also accepts `.code` plus its matching decrypted ExHeader.

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
cl /nologo /std:c++20 /EHsc /W4 /WX /I extractor\include /I wiiu_runtime\include extractor\src\*.cpp wiiu_runtime\src\romfs_reader.cpp /Fo:build\ /Fe:build\extractor.exe
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
Addresses and sizes accept decimal or `0x` notation. ExHeader metadata does not
identify ARM/Thumb boundaries or literal pools; ranges must be supplied explicitly.

The CLI logs input/output paths, section sizes, instruction/block counts and
entry point. It embeds initialized sections and initializes their zero-filled
tails in the generated dispatcher. See [loader details](docs/exefs-loader.md)
and [Thumb support](docs/thumb-support.md).

## Unified build

```bat
build_3ds2u_app.cmd game.3dsx game_wiiu.rpx
```

The two arguments are the input 3DSX and an output filename ending in `.rpx`,
without directory components. Quote paths containing spaces. Input paths resolve
relative to the caller; the script builds from the project root.

The pipeline checks prerequisites, extracts and relocates text/rodata/data,
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
   Keep the build log, input metadata and emulator log when investigating failures.
6. Stop emulation through Cemu when finished. The native audio demo plays a
   440 Hz tone; guest playback requires the explicit HLE/MMIO audio bridge.

No successful Cemu visual/input validation is claimed by this package. For native
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
[SVC runtime](docs/svc-runtime.md),
[VFP and T32](docs/vfp-thumb32.md),
[CRO modules and RO HLE](docs/cro-modules.md),
[display transfer and dual screens](docs/display-transfer.md),
[native AX audio](docs/audio-driver.md),
[3DSX extraction and packaging](docs/extraction-pipeline.md),
[TEV combiner](docs/tev-combiner.md),
[HID bridge](docs/hid-mvp.md), and [audio stubs](docs/audio-stubs.md).

## Current compatibility limits

- ELF, compressed `.code`, compressed 3DSX and encrypted containers are unsupported.
  The extractor accepts the documented version-zero 3DSX subset; NCCH/CXI inputs
  require external extraction/decryption with tools such as ctrtool.
- Instruction support is partial. IT and unimplemented T32/VFP forms are unsupported;
  automatic code/data separation and reachable-code discovery are absent.
- Guest memory must fit the runtime's fixed `[0x08000000, 0x10000000)` FCRAM
  window. General MMU mappings, full system services, and game assets are absent.
- The runtime entry currently starts at `0x08000000`. The loader logs the ExHeader
  text address, but other runtime entry addresses require integration changes.
- The geometry bridge expects LE float3 position plus float4 color and custom
  draw registers. It does not implement retail PICA command streams or shaders.
- Native AX playback supports explicit mono buffer submissions. Full CSND command
  parsing, DSP firmware/descriptor handling, gapless queues and general service
  IPC are not implemented; real game audio is not automatically intercepted.
- Execution is cooperatively sliced rather than synchronized to a complete 3DS
  interrupt, VBlank, GPU or OS scheduling model.

