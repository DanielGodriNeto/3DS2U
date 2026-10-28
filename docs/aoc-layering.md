# DLC discovery shim and RomFS layers

`AocManager` holds a caller-provided installed-DLC manifest for one game. IDs are
64-bit Title IDs, sorted and deduplicated; zero IDs and more than 4096 manifest
entries are rejected. Reconfiguration replaces the entire manifest and game ID.
The default manifest is empty. No installation is fabricated or discovered from
the SD card automatically.

The requested `aoc:u` commands are a **project shim**, not the native Nintendo 3DS
DLC service protocol. For comparison, [libctru AM DLC calls](https://github.com/devkitPro/libctru/blob/master/libctru/source/services/am.c)
use AM commands such as `AMAPP_GetDLCContentInfoCount` and
`AMAPP_ListDLCContentInfos`. Implementing this shim does not implement those AM
commands, entitlement/ticket checking or the native FS DLC archive protocol.

## Service dispatch and IPC contract

`HLEServiceManager` registers `aoc:u` and `fs:USER` in its explicit service-port
table. `OpenServicePort(name, handle)` allocates a session, `HandleServiceIPC`
dispatches requests by that session, and `CloseServicePort` releases it.
`srvExit` invalidates all service sessions. Closed IDs are not reused, and at
most 64 service sessions can be open. Guest kernel `srv`/SendSyncRequest routing
must still invoke these explicit entry points; the existing SVC dispatcher does
not automatically forward arbitrary guest IPC.

All words below are guest little-endian and are accessed through `Read32` and
`Write32`. Command and output addresses must be four-byte aligned and within
mapped FCRAM.

| Request | Words following the command header | Successful reply |
| --- | --- | --- |
| Count `0x00020000` | None | `0x00020080`, result=0, total count |
| List `0x00030042` | Capacity in Title IDs, writable buffer descriptor, output address | `0x00030082`, result=0, returned count, descriptor, output address |

List returns `min(capacity, installed_count)` IDs from the start of the sorted
manifest. Each Title ID occupies **eight bytes**: low LE32 word followed by high
LE32 word. This retains the full 64-bit value rather than truncating it to the
example's single `uint32_t`. The descriptor is `((capacity * 8) << 4) | 0xC`.
Capacity zero uses descriptor `0xC`, may use a null output address, and returns
zero IDs. Unused output bytes are preserved.

The entire declared output span is validated before any Title IDs are written;
overlap with the 20-byte IPC request/reply region is rejected. Invalid descriptors,
overflow, alignment or addresses return `AocManager::kInvalidArgument`; unknown
commands return `kNotSupported`. When an IPC reply can be written safely, errors
produce a result-only response with the request's command ID. These remain MVP
error codes. The service has no asynchronous installation notifications.

## Mounting base and DLC assets

```cpp
wiiu::RomFSReader reader;
if (!reader.MountFile(base_romfs_path)) throw std::runtime_error(reader.GetLastError());
if (!reader.MountLayerFile(dlc_romfs_path)) throw std::runtime_error(reader.GetLastError());
services.AttachRomFS(reader);
const std::array<std::uint64_t, 1> installed{dlc_title_id};
services.GetAocManager().SetInstalledDLCs(game_title_id, installed);
```

The caller supplies only DLCs associated with the current game and mounts their
decrypted Level-3 RomFS images in the intended priority order. The manifest and
mount set are configured independently; reconfiguring game IDs does not unmount
images. On a game switch, replace the base mount before adding the new game's
DLC layers. File-backed mounts require already-mounted storage.

- `Mount(bytes, level3_offset)` or `MountFile(path, level3_offset)` replaces all
  mounts, creating base index 0. Replacement invalidates all file handles, even
  if the replacement fails, retaining the original single-mount API behavior.
- `MountLayer` and `MountLayerFile` append a validated, owned layer. Later layers
  have higher priority. A failed append leaves previous layers and handles intact.
- `OpenFile` checks each layer from highest to lowest priority for the complete
  path. A missing file falls back through earlier layers to the base. An empty
  file in DLC overrides a nonempty base file; it is not treated as missing.
- Open handles retain the original layer's contents when a newer overlay is added.
  Handles use a layer index, so vector growth does not redirect existing reads.
- `Unmount` clears every layer and invalidates handles. There is no individual
  layer removal, whiteout/deletion protocol or merged directory enumeration.

Both buffer and file mounts copy their contents into owned RAM and validate
metadata with the existing Level-3 parser. UTF-8 and UTF-16 paths and explicit
Level-3 offsets remain supported. This consumes additional RAM for each image;
file loading also uses a temporary input buffer. The reader is not an IVFC/NCCH
decryptor, an on-demand disk filesystem or a thread-safe mount manager. Serialize
mounts, opens and reads through the service/engine lock.

## Verification

From an x64 MSVC developer prompt at the repository root:

```bat
if not exist build mkdir build
cl /nologo /std:c++20 /EHsc /W4 /WX /I wiiu_runtime\include /I wiiu_runtime\tests\audio_platform /I wiiu_runtime\tests\stubs wiiu_runtime\tests\aoc_tests.cpp wiiu_runtime\src\aoc_manager.cpp wiiu_runtime\src\romfs_reader.cpp wiiu_runtime\src\hle_services.cpp wiiu_runtime\src\save_manager.cpp wiiu_runtime\src\audio_driver.cpp wiiu_runtime\src\cro_loader.cpp wiiu_runtime\src\wiiu_memory.cpp wiiu_runtime\src\hid_manager.cpp /Fo:build\ /Fe:build\aoc_tests.exe
build\aoc_tests.exe
```

Tests cover count/list replies, full LE Title IDs, duplicate manifests, limited
capacity, guards and invalid buffers, service routing/closed sessions, base-only
and DLC-only files, shared-file overrides, empty overrides, stable handles,
failed appends and replacement invalidation. Synthetic file fixtures stay under
unique `build/aoc-tests-*` directories. Existing RomFS and save tests provide
regression coverage. Cemu/hardware execution and commercial title booting are
separate validation steps.
