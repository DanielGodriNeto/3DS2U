# N3DS memory, input and thread affinity

O3DS remains the default. Construct `WiiUMemory(true)` or `WiiUEngine(true)` to
select N3DS mode. The runtime entry point accepts `--new-3ds` as an application
argument and passes the selection into its engine; provide that argument through
the launcher when testing the RPX. No title/ExHeader-based model detection is
performed.

## Memory

The runtime's flat guest mapping starts at `0x08000000` and allocates a
4 KiB-aligned, zero-initialized buffer:

| Mode | Owned buffer | Guest window |
| --- | --- | --- |
| O3DS | 128 MiB | `[0x08000000, 0x10000000)` |
| N3DS | 256 MiB | `[0x08000000, 0x18000000)` |

Use `GetSize()`, `GetVirtualEnd()` and `IsNew3DS()` for instance bounds. The public
`kFCRAMSize` constant remains the legacy 128 MiB size for source compatibility.
All accesses, dynamic reservations, first-fit search, ControlMemory allocation,
CRO segment mapping and RO registration use the selected instance size. The heap
still starts above reserved images, shares reservations with CRO, rounds to pages,
and zeroes allocated/freed pages. A 200 MiB allocation fits in a sufficiently
empty N3DS window and fails in O3DS mode. Allocation can still fail if native
memory is unavailable or reserved image/module ranges leave insufficient space.

This expands the project's software window, not a complete retail 3DS physical/
virtual memory map. It does not add MMU mappings, kernel/system memory partitions,
hardware-model query services or New 3DS executable compatibility by itself.

## HID and IRRST snapshots

`WiiUEngine(true)` also constructs `HIDManager(true)`. Both modes map ordinary
buttons, calibrated bottom-screen touch and the left stick. N3DS mode additionally
maps ZL/ZR to bits 14/15 and the right stick to the C-Stick. Direction bits use
Circle Pad bits 28–31 and C-Stick bits 24–27 (right, left, up, down). Input bit
assignments match [libctru HID definitions](https://github.com/devkitPro/libctru/blob/master/libctru/include/3ds/services/hid.h).

Normalized VPAD stick axes are clamped to `[-1,1]`, rounded to signed `[-156,156]`,
and packed X in the low halfword and Y in the high halfword. Positive Y means up.
Nonfinite values become neutral. The chosen directional threshold is 40 axis
units. O3DS mode ignores ZL/ZR and right-stick input.

`GetSharedMemory()` returns raw LE bytes for a `0x2B0` HID block;
`GetIRRSTSharedMemory()` returns a `0x98` IRRST block. The published fields follow
the ring access patterns in [libctru HID](https://github.com/devkitPro/libctru/blob/master/libctru/source/services/hid.c)
and [IRRST](https://github.com/devkitPro/libctru/blob/master/libctru/source/services/irrst.c):

| Block | Index offset | Eight ring entries |
| --- | --- | --- |
| HID PAD | `0x10` | `0x28 + index*0x10`: held, pressed, released, packed Circle Pad |
| HID touch | `0xB8` | `0xC8 + index*8`: packed X/Y, active flag |
| IRRST | `0x10` | `0x18 + index*0x10`: extended held, pressed, released, packed C-Stick |

Each active section includes two unequal sequence timestamps in its initial
16 bytes. They are synthetic sample sequence values, not hardware tick timing.
The PAD ring excludes extended keys, which are published in IRRST. Ring edges
are derived from state changes. Missing/busy samples preserve the current ring
and axes, while legacy edge registers clear. Disconnection publishes a neutral
sample with releases. Unsupported accelerometer/gyroscope data remains zero.

The snapshots have read-only project aliases at `0x1EC01000` (HID) and
`0x1EC02000` (IRRST). Byte, halfword and word reads decode LE values without
double swapping on PowerPC. The legacy `0x1EC00000` registers remain available,
including packed axes at `+0x18/+0x1C` and extended held bits at `+0x20`.

A future HID service can publish/copy these snapshots to its mapped guest
addresses. For example, under the engine execution lock:

```cpp
memory.LoadImage(guest_hid_address, engine.GetHIDManager().GetSharedMemory());
memory.LoadImage(guest_irrst_address, engine.GetHIDManager().GetIRRSTSharedMemory());
```

Those guest ranges must already be reserved and within FCRAM. `GetPointer` remains
FCRAM-only; it does not expose mutable pointers to input aliases. Retail HID/IRRST
service handles, input event signaling and `svcMapMemoryBlock` routing remain
separate integration work. The engine updates HID under the same mutex used by
guest blocks to prevent concurrent snapshot reads during polling.

## Threads

CreateThread keeps its existing raw ABI: processor ID is in r4. Explicit IDs
0/1/2 map to affinity masks `1/2/4`; ID 2 requires N3DS mode. IDs -1 and -2 keep
the existing any-core approximation (mask 7). ID 3 and other invalid IDs fail.
The PPC stack remains 16-byte aligned. Workers are created suspended with the
requested affinity, updated with `OSSetThreadAffinity`, and then resumed. If
that update fails, the creation mask remains active and a diagnostic is logged.

`main.cpp` attempts to pin its GX2 thread to Core 1 before initializing graphics.
Guest blocks still serialize on the engine mutex; the affinity support does not
provide parallel translated-block execution or guest scheduler/timing fidelity.

## Host verification

From an x64 MSVC developer prompt at the repository root:

```bat
if not exist build mkdir build
cl /nologo /std:c++20 /EHsc /W4 /WX /I wiiu_runtime\include /I translator\include /I wiiu_runtime\tests\svc_platform /I wiiu_runtime\tests\audio_platform /I wiiu_runtime\tests\stubs wiiu_runtime\tests\n3ds_expansion_tests.cpp wiiu_runtime\src\svc_handler.cpp wiiu_runtime\src\wiiu_engine.cpp wiiu_runtime\src\wiiu_memory.cpp wiiu_runtime\src\hid_manager.cpp wiiu_runtime\src\hle_services.cpp wiiu_runtime\src\aoc_manager.cpp wiiu_runtime\src\save_manager.cpp wiiu_runtime\src\audio_driver.cpp wiiu_runtime\src\cro_loader.cpp wiiu_runtime\src\romfs_reader.cpp /Fo:build\ /Fe:build\n3ds_expansion_tests.exe
build\n3ds_expansion_tests.exe
```

The suite exercises a real 256 MiB host allocation and 200 MiB ControlMemory
request, zeroing/freeing, upper bounds, O3DS rejection, LE HID/IRRST rings, ZL/ZR,
both analog sticks, touch, ring rollover, missing samples, nonfinite axes and
native-thread mock affinity masks. Physical Wii U core scheduling, memory pressure
and Cemu/VPAD behavior require runtime testing.
