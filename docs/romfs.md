# RomFS and read-only FS HLE

`RomFSReader` owns copies of unencrypted **Level-3 filesystem images**.
`Mount(bytes, level3_offset)` also accepts an explicit offset into a larger
buffer. It does not discover/decrypt NCCH or IVFC containers or verify hashes.
The input may be released after mounting. `MountLayer` adds a higher-priority
image without replacing the base; `MountFile` and `MountLayerFile` load images
from host/mounted-device paths. See [DLC layering](aoc-layering.md) for the
priority, handle and service contracts.

```cpp
wiiu::RomFSReader reader;
if (!reader.Mount(romfs_bytes)) return; // GetLastError() describes mount failure.
services.AttachRomFS(reader);
const auto result = services.FSUSER_Initialize();
```

The reader must outlive FS calls through the attached manager. No SD-card file
is automatically loaded by the reader. The build pipeline embeds the base RomFS;
the runtime mounts that packaged image explicitly. Additional DLC mounts require
caller configuration.
To mount from FCRAM, pass a byte span over a checked `GetPointer(address, size)`
range. Owned image storage consumes additional host RAM.

## Format and paths

The 0x28-byte header contains ten LE32 fields: header size, directory hash
offset/size, directory metadata offset/size, file hash offset/size, file metadata
offset/size, and file data offset. Metadata references are relative table offsets;
`0xFFFFFFFF` means no entry. Directory records have a 24-byte fixed portion,
file records 32 bytes, followed by length-delimited UTF-16LE names padded to four
bytes. File data offsets and sizes are LE64; offsets are relative to file data.
See [libctru structures](https://github.com/devkitPro/libctru/blob/master/libctru/include/3ds/romfs.h).

Mounting checks table bounds/non-overlap, record boundaries, names, link targets,
parent consistency, cycles, duplicate names, unreachable records and file slices.
Hash references are checked, but lookup traverses child/sibling metadata chains;
hash bucket placement and integrity hashes are not authenticated.

`OpenFile(std::string, handle)` accepts UTF-8 `romfs:/...` or `/...` paths.
An overload accepts `std::u16string_view`. Matching is case-sensitive; malformed
Unicode, missing entries, relative paths, empty components, `.`/`..` and trailing
slashes fail and clear the output handle. UTF-8 is capped at 4096 bytes and
UTF-16 at 2048 code units. No host filesystem path is opened.

`ReadFile(handle, offset, buffer, size)` clamps reads to EOF and returns the
copied byte count. Zero-byte/EOF reads return zero. Invalid/stale handles or a
null output for a nonempty read throw. Pointer callers must supply the requested
output capacity. Handles belong to one reader and mount generation; remounting
or unmounting invalidates them. Integers use explicit byte assembly, safe on PPC.

## Service and IPC routing

`AttachRomFS` clears existing file sessions. `FSUSER_Initialize` returns zero.
The direct handlers are `FSUSER_OpenFileDirectly(memory, request_words, out_id)`,
`FSFILE_Read(memory, id, offset, output_address, size, bytes_read)` and
`FSFILE_Close(id)`. File reads validate the full requested FCRAM output range,
then copy raw bytes in 4096-byte chunks using `LoadImage`, without byte swapping.
There are at most 256 open sessions. IDs increase and are never reused.

Guest-buffer entry points are `HandleFSUserIPC(memory, command_address)` and
`HandleFSFileIPC(memory, file_session_id, command_address)`. They use guest
`Read32`/`Write32` for IPC numeric words and support these libctru request layouts:

| Session | Operation | Request header |
| --- | --- | --- |
| fs:USER | Initialize with current-process descriptor | `0x08010002` |
| fs:USER | OpenFileDirectly | `0x08030204` |
| File | Read | `0x080200C2` |
| File | Close | `0x08080000` |

Open supports archive ID 3 (current-process RomFS), PATH_EMPTY archive path,
PATH_ASCII/UTF-8 or UTF-16LE filenames, read flag 1 and attributes zero. Path
lengths include terminators. Static/writable buffer descriptors and their lengths
are checked. Replies contain results, moved session IDs or actual read lengths.
See [libctru IPC encoding](https://github.com/devkitPro/libctru/blob/master/libctru/source/services/fs.c).

Nonzero `kFS*` constants are **MVP error values**, not a complete catalog of
Nintendo FS results. Unsupported archives, flags, attributes and commands fail.
RomFS remains read-only. SaveData/ExtData writes, file creation and directory
creation are handled by the separate [save archive backend](save-data.md).
SDMC archives and directory enumeration remain unsupported.

These are explicit routing functions. A future guest SVC/session dispatcher must
call them; the current execution loop does not intercept IPC automatically.
Command buffers and output spans must lie in the fixed FCRAM window. Real TLS
addresses need additional mapping. There is no kernel buffer/handle transfer or
process authorization emulation. Invalid command addresses fail without unsafe
reply writes. Calls are single-threaded; concurrent remounts/reads are unsupported.

## Verification

`wiiu_runtime/tests/romfs_tests.cpp` constructs the official header/record layout
with `test.txt` containing exactly `3DS2U_ROMFS_OK`, a nested texture file, a
Unicode name, and a larger chunked-read file. It checks ownership, partial/EOF
reads, invalid metadata and cycles, remount invalidation, handle exhaustion,
UTF-8/UTF-16 IPC paths, reply fields and exact guest output bytes. It links the
production reader/services/memory against host platform substitutes. Runtime
asset mounting and Cemu/Wii U FS integration remain separate validation steps.
