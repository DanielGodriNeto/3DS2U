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
| Loader | Uncompressed ExeFS `.code` plus separate decrypted ExHeader; text/rodata/data and BSS mapping; raw instruction fixtures |
| Memory | 128 MiB FCRAM buffer aligned to 4 KiB; bounded accesses; guest little-endian data swapped on big-endian PPC |
| Graphics | MVP PICA200-to-GX2 bridge: clear color, vertex buffer, stride/count, triangle draws; float24 uniforms, default matrix vertex shader and color pixel shader |
| GPU command buffers | PICA packet parser, byte masks, FIFO/consecutive writes, FINALIZE and deferred draw snapshots through the MVP bridge |
| Input | VPAD buttons mapped to 3DS masks; calibrated touch scaled to 320×240; custom readable HID registers |
| HLE | srv/apt initialization and loop placeholders; CSND/DSP success and readiness stubs; audio MMIO no-op writes |
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
- An uncompressed `.code` and its matching decrypted 0x400/0x800-byte ExHeader.
  The included triangle generator creates a self-contained test pair.

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
```

The unified pipeline requires this executable to exist; it does not rebuild
the host translator. Rebuild it after changing translator sources.

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
build_3ds2u_app.cmd "path\game.code" "path\game.exheader" "my_game.rpx"
```

The output argument is a filename ending in `.rpx`, without directory components.
Input paths resolve relative to the caller's current directory; the script then
builds from the repository root, so it can be invoked from another directory.

The five logged steps validate prerequisites, run the translator, copy
`output_translated.cpp` to `wiiu_runtime/src/translated_game.cpp`, invoke the
runtime Makefile, and move the final RPX to `build\my_game.rpx`. Each failure
returns a nonzero exit code and stops subsequent stages. A failed invocation
can leave intermediate sources; an older published RPX can remain at the output
path, so check the exit code and the final success message.

Generated sources are overwritten on each run. The root `translated_game.cpp`
is a historical fixture and is not compiled by the runtime Makefile. Only
`wiiu_runtime/src/*.cpp` are included, preventing duplicate dispatcher definitions.
Objects/dependency files and the intermediate ELF live in `build/runtime/`.
Make tracks source/header dependencies; changes to toolchain paths or command-line
flags require a fresh object directory.

The master script currently supplies the ExHeader option and assumes ARM text.
For Thumb/mixed inputs, run the CLI with the appropriate mode options, copy its
output to `wiiu_runtime/src/translated_game.cpp`, then invoke the Makefile directly:

```bat
"%DEVKITPRO%/msys2/usr/bin/make.exe" -C wiiu_runtime all
```

This produces `build/runtime/wiiu_runtime.rpx`. The Makefile can also run under a
POSIX shell with corresponding devkitPro environment paths.

## Reproducible triangle package

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX generate_triangle_test.cpp /Fo:build\ /Fe:build\generate_triangle_test.exe
build\generate_triangle_test.exe
build_3ds2u_app.cmd triangle_test.code triangle_test.exheader triangle_demo.rpx
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
6. Stop emulation through Cemu when finished. CSND/DSP stubs produce no sound.

No successful Cemu visual/input validation is claimed by this package. For native
hardware, launch the RPX using your existing Wii U homebrew environment.

## Tests and implementation notes

Tests are under `translator/tests` and `wiiu_runtime/tests`. They cover branch
targets, conditions, ALU flags, addressing modes, loader metadata, T16 operations,
geometry MMIO, HID and audio. Tests execute emitted C++ and use host substitutes
for Wii U platform APIs; they are not a replacement for hardware validation.

For example, to run the production audio service/memory paths on an MSVC host:

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX /I wiiu_runtime\include /I wiiu_runtime\tests\audio_platform /I wiiu_runtime\tests\stubs wiiu_runtime\tests\audio_stub_tests.cpp wiiu_runtime\src\hle_services.cpp wiiu_runtime\src\wiiu_memory.cpp wiiu_runtime\src\hid_manager.cpp /Fo:build\ /Fe:build\audio_stub_tests.exe
build\audio_stub_tests.exe
```

Additional architecture notes: [graphics bridge](docs/graphics-mvp.md),
[GSP command buffers](docs/gsp-command-buffers.md),
[vertex uniforms](docs/vertex-uniforms.md),
[HID bridge](docs/hid-mvp.md), and [audio stubs](docs/audio-stubs.md).

## Current compatibility limits

- ELF, 3DSX, compressed `.code`, and encrypted container parsing are unsupported.
- Instruction support is partial. T32 and long Thumb BL pairs are unsupported;
  automatic code/data separation and reachable-code discovery are absent.
- Guest memory must fit the runtime's fixed `[0x08000000, 0x10000000)` FCRAM
  window. General MMU mappings, full system services, and game assets are absent.
- The runtime entry currently starts at `0x08000000`. The loader logs the ExHeader
  text address, but other runtime entry addresses require integration changes.
- The geometry bridge expects LE float3 position plus float4 color and custom
  draw registers. It does not implement retail PICA command streams or shaders.
- Audio remains silent. Dummy readiness flags cannot satisfy every real DSP
  polling protocol, and general service IPC is not implemented.
- Execution is cooperatively sliced rather than synchronized to a complete 3DS
  interrupt, VBlank, GPU or OS scheduling model.
