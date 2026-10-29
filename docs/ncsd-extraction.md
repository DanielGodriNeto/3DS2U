# NCSD cartridge extraction

The extractor accepts raw `.3ds` and `.cci` cartridge images as NCSD containers:

```bat
build\extractor.exe game.3ds
build\extractor.exe game.cci
build_3ds2u_app.cmd game.3ds game_wiiu.rpx
```

Extensions are case-insensitive. Outputs remain `extracted.code`,
`extracted.exheader`, `extracted.romfs` and `extracted_romfs.cpp`. The master
script installs the generated sources and embeds RomFS exactly as for CIA/3DSX.

## Shared NCCH path

`ncch_extractor.hpp` exposes `ExtractNCCH(read_at, content_size, keys)`. All reads
are relative to the NCCH partition header; the callback must return the requested
bytes or throw. Short reads and out-of-partition requests are checked explicitly.

The CIA adapter performs its outer Ticket/TitleKey CBC decryption before these
reads. The NCSD adapter simply rebases reads into partition 0. Both then use the
same NCCH counter, KeyX/KeyY derivation, ExHeader/ExeFS integrity checks, secure/
seed methods, backwards BLZ decompression and IVFC Level-3 RomFS extraction.
`KeyStore` is shared through `key_store.hpp`; it is no longer owned by the CIA
implementation. File-backed adapters use synchronous bounded reads rather than
loading the whole CIA or cartridge dump into RAM.

Decrypted dumps with stale crypto flags are detected per section. ExHeader uses
its stored SHA256; ExeFS uses its superblock SHA256 or a validated file table
(including names, `.code`, bounded sizes and non-overlapping ranges). A plaintext
ExeFS decision applies to its file payloads as well as its header. RomFS uses a
bounded IVFC header check. Fully plaintext dumps need no AES keys; mixed dumps
continue to decrypt the remaining encrypted sections.

Hash mismatches still fail unless `--ignore-hash` is supplied. The override never
disables file-table validation, section/file bounds, or decompression checks.

## NCSD validation

The parser reads the signed header's `NCSD` magic at `0x100`, media capacity,
media ID, filesystem types and eight partition records at `0x120`. Partition
offsets and lengths are LE32 counts of `0x200`-byte media units, converted to
64-bit byte ranges. Partitions must stay within both declared media capacity and
the physical input file, start beyond the cartridge metadata area (`0x4000`),
and not overlap. Empty records must have zero offsets.

Partition 0 must exist. It is selected explicitly as the primary executable;
the parser does not substitute manual/update partitions when it is missing.
A populated primary Partition ID table entry must match the NCCH header. The
filesystem/media-ID checks reject NAND-style NCSD images. Layout interpretation
follows the [NCSD reference structures](https://github.com/d0k3/GodMode9/blob/master/arm9/source/game/ncsd.h).

Trimmed dumps are accepted when only unused trailing card padding is missing.
Truncated declared partitions are rejected, including non-primary partitions.
Signed-header/NCSD RSA authentication, NAND decryption, cartridge save chips and
multiple-partition asset merging are not implemented.

## Keys

NCSD has no CIA Ticket or outer TitleKey layer. `common_key_*` entries are never
required by its extraction path. NCCH key requirements still depend on its flags:

- NoCrypto: no AES key is needed.
- Standard/secure NCCH: supply `keyx_2c` and any secondary KeyX/seed required by
  the NCCH header. Standard encryption does **not** imply a zero key.
- Fixed application NCCH: automatically use the zero Normal Key.
- Fixed system NCCH: supply `ncch_fixed_system_key`; the legacy name
  `ncch_fixed_key` is accepted as an alias. No real system key is embedded.

`crypto::NCCHFixedKey` selects the fixed application/system path from Program ID
bit 36. The application path uses zero even if an unrelated key file is loaded;
the system path fails explicitly if its key is missing. The NoCrypto flag takes
precedence over fixed/secure key selection.

The standalone CLI loads `keys.txt` from its current directory; the master script
uses the project root. See [CIA key configuration](cia-extraction.md) for the
entry syntax and [shared extraction limits](cia-extraction.md#limits-and-verification).
Extracting a cartridge does not relocate retail addresses or establish AOT/game
compatibility. Code/RomFS output is still held in RAM and compiled asset packages
can be large.

## Tests

From an x64 MSVC developer prompt at the repository root:

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /I extractor\include /I wiiu_runtime\include extractor\tests\ncsd_tests.cpp extractor\src\ncsd_extractor.cpp extractor\src\ncch_extractor.cpp extractor\src\key_store.cpp extractor\src\crypto_engine.cpp wiiu_runtime\src\romfs_reader.cpp /Fo:build\ /Fe:build\ncsd_tests.exe
build\ncsd_tests.exe
```

The tests share synthetic, public-NIST-key fixtures with CIA tests. They cover
primary partition rebasing, trimmed padding, overlap/truncation and NAND rejection,
standard/secure/seed crypto without common keys, zero-fixed applications, fixed
system key requirements, compressed code, RomFS bytes and short callbacks. Fixture
`.3ds`/`.cci` files stay in unique `build/ncsd-tests-*` directories.

CIA and NIST crypto suites also pass after the refactor. CLI smoke tests cover
`.3ds` and `.cci` without `keys.txt` using zero-fixed fixtures, and an encrypted
CIA using a synthetic key file. No retail title or Wii U/Cemu execution is claimed.
