# 3DSX extraction and packaging

Build `build/extractor.exe` and `build/3ds2u_translator.exe` as documented in the
README, then run:

```bat
build_3ds2u_app.cmd game.3dsx game_wiiu.rpx
```

The script resolves the input relative to the caller, switches to the project
root, checks host tools and devkitPPC/wut, extracts the container, translates
executable text, installs generated sources and invokes make. It publishes
`build/game_wiiu.rpx` and `build/game_wiiu.romfs`. The sidecar is a diagnostic copy;
the RPX contains its own RomFS bytes and needs no filesystem path to mount them.

## Extractor

Run `build/extractor.exe path/to/game.3dsx` from the desired output directory.
Version zero, flags zero, an at-least-0x20-byte header, and relocation headers
with at least two word-sized counts are supported. The standard 0x2C extended
header supplies bounded SMDH and RomFS offsets. SMDH icons are not packaged.
Limits are 512 MiB input, 128 MiB mapped executable and 64 MiB embedded RomFS.

The extractor writes:

| File | Content |
| --- | --- |
| `extracted.code` | Page-padded text, rodata and initialized data, with relocations applied |
| `extracted.exheader` | Synthetic 0x400-byte CodeSetInfo/BSS metadata for BinaryLoader |
| `extracted.romfs` | Appended Level-3 RomFS, or an empty file when absent |
| `extracted_romfs.cpp` | Strong `GetPackagedRomFS()` implementation, including an empty package when absent |

The data size in 3DSX includes BSS; those trailing bytes are absent from the file.
Segments retain their original page-rounded relative placement and are relocated
to `0x08000000`, the current runtime entry. Absolute and cross-segment relative
word relocation runs are parsed as skip/patch counts. Relative subtype zero emits
32-bit differences; subtype one emits PREL31 differences. Tagged Thumb pointers
are preserved. Duplicate/out-of-range patches, nonempty unknown relocation kinds,
bad subtypes and patches outside initialized bytes are rejected. BSS is zeroed by
the generated dispatcher, using the synthetic metadata rather than stored bytes.

This is not a signed NCCH ExHeader and supplies no security capabilities.
The source format and relocation semantics were checked against
[devkitPro's 3dsxtool](https://github.com/devkitPro/3dstools/blob/master/src/3dsxtool.cpp)
and [Azahar's loader](https://github.com/azahar-emu/azahar/blob/master/src/core/loader/3dsx.cpp).
An NCCH magic at offset zero or the normal signed-header offset `0x100` produces
a ctrtool/decryption warning and exit code 2. Other errors return 1; success is 0.
Commercial inputs are never decrypted by this tool.

## Runtime package

The master script passes `--exheader extracted.exheader` to the translator.
Raw-code mode would decode data/padding as instructions and omit section/BSS
mapping, so the metadata is required even though the user supplies only two
pipeline arguments. The standalone translator's raw-code behavior is unchanged.

`extracted_romfs.cpp` is installed as ignored generated source
`wiiu_runtime/src/generated_romfs.cpp`. It overrides the runtime's weak empty
package. Main mounts the embedded bytes with `RomFSReader`, attaches the reader
to `HLEServiceManager` and initializes its explicit FS helpers before guest
execution. A build without assets overwrites the generated package with an empty
implementation, preventing reuse of a previous game's RomFS. Standalone manual
builds can omit the generated file and use the empty fallback.

RomFS bytes must be an unencrypted Level-3 image. The extractor validates them
with the same reader used at runtime; IVFC wrappers and cryptographic integrity
verification are not added here. Automatic guest FS session/SVC routing, libctru
startup environment and `_prm` initialization remain separate implementation work.

The two-argument pipeline assumes ARM text. 3DSX does not identify Thumb ranges
or literal pools, and it cannot make the existing partial decoder handle arbitrary
homebrew automatically. Unsupported instructions stop translation. For known
mixed-mode ranges, extract manually and invoke the translator's existing Thumb
options with the generated ExHeader. A successful RPX build is not proof that a
real application boots.

Every failed stage stops the pipeline with nonzero status. Existing extraction
outputs or previously published RPX files can remain after failure; check the
exit code and final success message. Generated sources/intermediates are replaced
on subsequent successful runs. Builds share the runtime output directory and must
run serially.

## Tests

`extractor/tests/extractor_tests.cpp` constructs a 3DSX containing executable text,
rodata/data pointers, absolute and PREL31 relocations, BSS and a small RomFS file.
It checks the extracted image through BinaryLoader and RomFSReader and writes
`build/extractor_fixture.3dsx` for the complete build pipeline. The unit tests also
reject truncated headers/tables, invalid runs, sizes and RomFS offsets.

From an MSVC developer prompt at the project root:

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX /I extractor\include /I translator\include /I wiiu_runtime\include extractor\tests\extractor_tests.cpp extractor\src\three_dsx.cpp translator\src\binary_loader.cpp wiiu_runtime\src\romfs_reader.cpp /Fo:build\ /Fe:build\extractor_tests.exe
build\extractor_tests.exe
build_3ds2u_app.cmd build\extractor_fixture.3dsx homebrew_pipeline_test.rpx
```

The fixture executes MOV followed by a self-branch; it carries `romfs:/test.txt`
containing `3DS2U_ROMFS_OK`. `package_tests.cpp` checks that the generated C++
package can be mounted and read on a host. Cemu/hardware execution is unverified.
