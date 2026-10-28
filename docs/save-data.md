# SaveData and ExtData persistence

`SaveManager` backs archive `0x00000004` (SaveData) and archive `0x00000006`
(ExtData) with real files. The default base is `sd:/wiiu/apps/3ds2u/saves/`:

```text
<base>/<16-digit-title-id>/savedata/<guest-path>
<base>/<16-digit-title-id>/extdata/<16-digit-extdata-id>/<guest-path>
```

Provide the actual nonzero Title ID when constructing the manager. On Wii U,
mount the SD device first, and use its registered devoptab path. For example,
after successful `WHBMountSdCard()`, use `WHBGetSdCardMountPath()` plus
`/wiiu/apps/3ds2u/saves/` as the base. Keep the mount and manager alive until all
FS calls and guest workers stop. Serialize access with the engine/service lock.

```cpp
// title_id and mounted_base come from the application's title/storage setup.
wiiu::SaveManager saves(title_id, mounted_base);
wiiu::hle::HLEServiceManager services;
services.AttachSaveManager(saves);
if (services.FSUSER_Initialize() != 0) {
    throw std::runtime_error("FS initialization failed");
}
std::uint64_t archive{};
if (services.FSUSER_OpenArchive(4, archive) != 0) {
    throw std::runtime_error("SaveData archive mount failed");
}
```

The existing generated homebrew package does not supply a commercial Title ID.
The current runtime entry point therefore does not automatically mount a save
archive. The launcher/service integration must attach this manager with its
title identity and mounted storage path. As with the existing RomFS helpers,
these explicit FS IPC handlers still require service-session/SVC routing before
arbitrary guest IPC calls reach them. This implementation alone does not establish
commercial game compatibility.

## Operations and IPC

- `OpenArchive` creates missing title/archive directories. ExtData accepts an
  explicit 64-bit ID; the IPC binary low path is three LE words: SD media type
  `1`, ExtData ID low word, high word. Empty low paths select ID zero.
- Open flags use read `1`, write `2`, create `4`. Create requires write access;
  opening an existing file with create does not truncate it.
- `CreateFile` creates exclusively and zero-fills the requested size. Guest
  directory parents must already exist. Sizes/offsets are limited to `0x7fffffff`
  for portable signed-32-bit SD seeking.
- Writes are raw byte copies in chunks up to 32 KiB. Gaps are zero-filled.
  Write flag `1` flushes stdio and requests `_commit` on Windows or `fsync` on
  POSIX/wut; flag `0x100` allows normal host timestamp updates. Errors propagate.
- Formatting clears only the current title's SaveData, invalidating its open
  file/archive handles. ExtData and other title directories survive. Formatting
  is destructive and is not a transactional operation on storage failure.
- The HLE layer supports Initialize, Open/CloseArchive, FormatThisUserSaveData,
  CreateFile/CreateDirectory, OpenFile/OpenFileDirectly, and file Read/Write,
  GetSize, Flush and Close. Paths may be NUL-terminated ASCII or UTF-16LE.
  Handles and numeric IPC fields use guest LE accessors; file bytes are never
  swapped. Error results remain the project's MVP FS result codes.

Guest traversal, host absolute paths, symbolic links/reparse points, reserved
device names and ambiguous host filenames are rejected. The host base is trusted;
do not concurrently modify its directory tree outside the FS service. Path checks
do not provide protection against an external process racing filesystem changes.
Capacity quotas, journal duplication, encryption, secure values, archive commit
transactions and full Nintendo FS permissions/results are not emulated.

## Host tests

Run from an x64 MSVC developer prompt at the repository root:

```bat
if not exist build mkdir build
cl /nologo /std:c++20 /EHsc /W4 /WX /I wiiu_runtime\include /I wiiu_runtime\tests\audio_platform /I wiiu_runtime\tests\stubs wiiu_runtime\tests\save_tests.cpp wiiu_runtime\src\save_manager.cpp wiiu_runtime\src\hle_services.cpp wiiu_runtime\src\romfs_reader.cpp wiiu_runtime\src\audio_driver.cpp wiiu_runtime\src\cro_loader.cpp wiiu_runtime\src\wiiu_memory.cpp wiiu_runtime\src\hid_manager.cpp /Fo:build\ /Fe:build\save_tests.exe
build\save_tests.exe
```

Fixtures stay in unique `build/save-tests-*` directories. Tests exercise the
format/open/create/write/read boot sequence, a physical 1024-byte file, reopening
through a new manager, raw bytes, zero gaps, chunked writes, read-only handles,
invalid ranges, IPC/Unicode paths, ExtData/title isolation and stale handles.
The link test is skipped explicitly if the host cannot create symbolic links.
Host tests and a successful devkitPPC link do not validate Wii U SD-card I/O;
test that separately on hardware or Cemu with a writable mounted device.
