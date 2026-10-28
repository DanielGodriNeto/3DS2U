# ExeFS .code loading

```text
build\3ds2u_translator.exe game.code game.cpp --exheader game.exheader
```

An extracted ExeFS `.code` has no embedded section table or independent entry-point
offset. Supply its decrypted NCCH ExHeader (0x400 or 0x800 bytes) to identify
`.text`, `.rodata`, `.data`, BSS, and the `.text` address used as the entry point.
The loader accepts uncompressed payloads with a clear ExHeader compression flag.
Compressed payloads, encrypted containers, ELF, and 3DSX parsing are not implemented.

The two-argument CLI remains available for raw ARM fixtures such as `color_test.code`.
In that mode, the entire input is explicitly assumed to be ARM `.text` at
`0x08000000`; no section boundaries or entry-point fields are inferred from bytes.

File offsets use page-rounded initialized section sizes. Guest virtual addresses
come from the ExHeader. The loader packs mapped pages into simulated FCRAM backing
starting at `0x08000000`, capped at 128 MiB, and records the guest-to-backing mapping
in `BinarySection`. Guest addresses can have gaps and are not relocated. BSS starts
immediately after initialized `.data`; BSS and page tails are zero initialized.
`GetGuestBytes` resolves a guest address through these descriptors.

Only initialized `.text` is passed to the ARM decoder and CFG builder. The exposed
instruction addresses are aligned ARM candidates, not proof that a word is an ARM
instruction. Mixed Thumb/ARM code, literal-pool identification, and reachable-code
analysis require additional metadata/analysis.

The mapped image is currently PC-side state. Emitting initialized data into the
Wii U runtime and supporting guest mappings outside its current fixed window are
separate runtime integration work; this loader alone does not enable full games.

Format references: [Luma3DS loader](https://github.com/LumaTeam/Luma3DS/blob/master/sysmodules/loader/source/loader.c)
and [NCCH CodeSetInfo layout](https://github.com/azahar-emu/azahar/blob/master/src/core/file_sys/ncch_container.h).
