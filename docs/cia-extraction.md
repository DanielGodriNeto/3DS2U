# Native CIA extraction

The C++20 extractor accepts `.cia`, `.3dsx` and [NCSD `.3ds`/`.cci`](ncsd-extraction.md). It uses its own AES-128,
CBC/CTR and SHA256 implementation; it does not invoke ctrtool or download keys.

```bat
build\extractor.exe game.cia
build_3ds2u_app.cmd game.cia game_wiiu.rpx
```

For extraction diagnostics and AOT experiments with stale integrity hashes:

```bat
build\extractor.exe nsmb2.cia --ignore-hash
```

Diagnostics identify the Title ID, common-key index, requested key names/KeyX slots,
and the failing TMD table, TMD chunk records, CIA content, NCCH ExHeader, ExeFS
superblock, `.code`, or seed verification hash. Expected and actual digests are
printed on failure; secret key values are never printed. TMD metadata checks
precede decryption. The CIA content hash covers outer-CBC-decoded bytes before
inner NCCH CTR decryption, so an inner KeyX cannot cause that hash mismatch.

`--ignore-hash` changes hash failures into warnings for CIA and NCSD extraction.
It does not recover missing/wrong keys, bypass missing NCCH magic, or disable
bounds, decompression, section-layout or RomFS validation. A missing NCCH magic
is detected before hashing content; that header is outside NCCH CTR encryption.
The master build script does not enable this override automatically.

The standalone extractor reads `keys.txt` in its current directory. The master
script changes to the project root, so put that pipeline's key file there. The
file is ignored by Git. Key values are never printed or included in generated
sources. All examples below contain placeholder zeroes, not Nintendo keys.

## Keys and encryption layers

Ticket TitleKeys are encrypted; a TitleKey and CommonKeyY are **not** the inputs
to the 3DS Normal-Key scrambler. The implementation uses:

1. A full indexed common Normal Key, or a common KeyX/KeyY pair combined as
   `ROL128((ROL128(KeyX,2) XOR KeyY) + C,87)` with the public hardware scrambler
   constant C. C is an algorithm constant, not a secret key.
2. AES-CBC to decrypt the ticket TitleKey, with the big-endian Title ID in the
   first eight IV bytes and zero padding.
3. AES-CBC to decrypt the selected CIA content with that TitleKey. The content
   index occupies the first two IV bytes in big-endian order.
4. AES-CTR for NCCH ExHeader, ExeFS and RomFS. These use separate NCCH keys, not
   the CIA TitleKey. A common key alone cannot decrypt an encrypted NCCH.

Key entry names are normalized to lowercase, followed by `=`, and exactly 32 hexadecimal digits; `#`
starts a comment. Duplicate names and malformed values fail. Required values
depend on the ticket/NCCH flags:

```text
# Full common Normal Key for ticket common-key index 0:
common_key_0=00000000000000000000000000000000
# common_key is an alternative name for common_key_0.

# Standard NCCH KeyX; KeyY is read from the NCCH signature:
keyx_2c=00000000000000000000000000000000

# Supply these only for the indicated secure crypto methods:
keyx_25=00000000000000000000000000000000
keyx_18=00000000000000000000000000000000
keyx_1b=00000000000000000000000000000000

# Alternative common-key derivation, when common_key_<index> is absent:
keyx_3d=00000000000000000000000000000000
common_key_y_0=00000000000000000000000000000000

# Seed entry is selected using the 16-digit NCCH Program ID:
seed_0004000001234500=00000000000000000000000000000000

# Fixed system NCCH requires its applicable Normal Key; applications use zero:
ncch_fixed_system_key=00000000000000000000000000000000
```

Other common-key indices use decimal suffixes, for example `common_key_1` or
`common_key_y_1`. No secret common keys, KeyXs, fixed system key or title seeds
are embedded. Missing key material produces a named error; the extractor never
silently substitutes a zero key. Without `keys.txt`, fully unencrypted CIA/NCCH
content can still be extracted.

## Parsing and output

CIA header, certificate/ticket/TMD section ranges and 64-byte section alignment
are checked. The ticket and TMD signed-body offset is selected from their
RSA4096/RSA2048/ECDSA SHA256 signature type. Ticket/TMD Title IDs must agree.
The content-info hashes, chunk-table coverage, included-content bitmap, sizes
and indices are validated. The boot content selected by the TMD is extracted;
other content partitions are not merged into it.

The selected content is fetched through bounded random-access reads and hashed
in 1 MiB chunks. CBC reads use the preceding cipher block when seeking. This
avoids reading the entire CIA into RAM. The selected content's TMD SHA256 must
match before its NCCH is processed.

NCCH versions 0/2 reverse the eight on-disk Partition ID bytes, append section
1/2/3 in counter byte 8, and zero-pad. Version 1 retains on-disk byte order and
adds the region's byte offset to the counter. In-region reads advance CTR by
16-byte blocks and retain the intra-block byte offset.

NCCH crypto methods 0, 1, 0xA and 0xB select KeyX slots 2C, 25, 18 and 1B.
The ExHeader and ExeFS header use the primary 2C key; `.code` and RomFS use the
secondary key. Seed crypto checks `SHA256(seed || ProgramID_LE)` against the
header, then derives the secondary KeyY from `SHA256(original_KeyY || seed)`.
Fixed application content uses zero automatically. Fixed system content requires
`ncch_fixed_system_key` (or its legacy alias `ncch_fixed_key`). This selection and
the NCCH parser are shared between CIA and NCSD; cartridges need no TitleKey.

The extractor validates the ExHeader hash, ExeFS superblock hash and `.code`
hash. A compressed `.code` is decoded with bounded backwards BLZ/LZSS, including
overlapping backreferences; its output is capped at 256 MiB. The compression bit
is cleared in the emitted decrypted ExHeader. Initialized segment lengths must
agree with the uncompressed code length. IVFC metadata identifies the aligned
Level-3 RomFS payload; the existing RomFS reader validates its directory tables.

Outputs retain the existing pipeline names: `extracted.code`,
`extracted.exheader`, `extracted.romfs`, and `extracted_romfs.cpp`. NCCH segment
addresses are retained. The tool does not relocate a retail executable to a new
base or perform 3DSX relocations on NCCH code. Direct `.cxi`/NCCH input is still
unsupported; use a CIA container for this path.

## Limits and verification

This supports the described 3DS SHA256 CIA/NCCH layouts; it is not a universal
title converter. Personalized tickets need device-specific conversion. TWL/
SHA1 formats, unknown NCCH crypto methods, nonstandard ExeFS superblocks and
containers without executable boot `.code` fail explicitly. Keys/seeds are
caller-provided. Certificate-chain/RSA signatures and the full IVFC hash tree
are not authenticated; content hashes detect corruption or wrong decryption
keys but do not establish provenance.

Extracted code/RomFS are held in RAM, and the existing C++ asset package can be
very large. Translation remains limited by the translator's address ranges,
instruction support, code/data separation and runtime services. The emitted
dispatcher enters the ExHeader `.text` address, so retail `.text` at `0x00100000`
is entered there when it has a translated block; translation rejects an image whose
entry has none. Whether a given title then runs is unverified: later unsupported
instructions, SVCs, services or data accesses are expected failures.

AES CTR/CBC tests use public [NIST SP 800-38A vectors](https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38a.pdf).
Counter layouts and metadata field interpretation were checked against the
[NCCH reference](https://github.com/d0k3/GodMode9/blob/master/arm9/source/game/ncch.c),
[CIA layout](https://github.com/d0k3/GodMode9/blob/master/arm9/source/game/cia.c),
and [TMD structures](https://github.com/d0k3/GodMode9/blob/master/arm9/source/game/tmd.h).
The implementation is independently written; it does not copy their crypto code.

From an x64 MSVC developer prompt:

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /I extractor\include extractor\tests\crypto_tests.cpp extractor\src\crypto_engine.cpp /Fo:build\ /Fe:build\crypto_tests.exe
build\crypto_tests.exe
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /I extractor\include /I translator\include /I wiiu_runtime\include extractor\tests\cia_tests.cpp extractor\src\cia_extractor.cpp extractor\src\ncch_extractor.cpp extractor\src\key_store.cpp extractor\src\crypto_engine.cpp translator\src\binary_loader.cpp wiiu_runtime\src\romfs_reader.cpp /Fo:build\ /Fe:build\cia_tests.exe
build\cia_tests.exe
```

`crypto_tests.cpp` covers AES CTR/CBC/ECB, partial CTR reads, counter overflow,
Normal-Key derivation, NCCH counter versions and SHA256 padding/streaming.
`cia_tests.cpp` supplies synthetic CIA/CBC + NCCH/CTR cases, secure/seed keys,
BLZ, IVFC, missing/wrong keys, hashes and translator metadata. Fixtures use only
public NIST test keys and synthetic bytes. Real-title extraction and hardware
execution require separate validation.
