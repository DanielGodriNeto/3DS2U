# Guest memory mapping

`WiiUMemory` owns one FCRAM allocation (128 MiB, or 256 MiB in N3DS mode) and
resolves 32-bit guest addresses to offsets in it. Two views exist:

- **Canonical window**: guest `0x08000000 + n` is backing offset `n`, for
  `n < GetSize()`. Heap and CRO allocations live here, above `GetImageEnd()`.
- **Image mappings**: `MapImageRange(guest_va, image_offset, size)` makes the
  guest range `[guest_va, guest_va + size)` resolve to backing
  `[image_offset, image_offset + size)`. This is how ExHeader sections keep their
  real addresses (for example `.text` at `0x00100000`) while the loader packs
  their pages into compact backing. The ranges may have gaps between them.

Generated `InitDispatcher` code maps every section with `mapped_size != 0`, then
reserves, zeroes and fills it. `mapped_size` is page rounded, so initialized
bytes, the page tail and BSS are one mapped range; everything after the initialized
bytes is zero.

## Invariants

`MapImageRange` validates in 64-bit arithmetic and either maps the whole range or
throws and leaves the mapper unchanged.

| Rejected input | Exception |
| --- | --- |
| `size == 0` | `std::out_of_range` |
| `guest_va`, `image_offset` or `size` not a multiple of 4 KiB | `std::out_of_range` |
| `guest_va + size` beyond 2^32 (ending exactly at 2^32 is valid) | `std::out_of_range` |
| `image_offset + size` beyond the FCRAM size | `std::out_of_range` |
| Guest range inside the canonical window with a backing other than its own (identity: `guest_va == 0x08000000 + image_offset`) | `std::logic_error` |
| Guest range overlapping an existing mapping | `std::logic_error` |
| Backing overlapping the backing of an existing mapping | `std::logic_error` |
| Backing overlapping a dynamic (heap/CRO) reservation | `std::logic_error` |
| Backing above `GetImageEnd()` once any dynamic reservation exists | `std::logic_error` |

Adjacent ranges (end of one equal to start of the next, in guest space or in
backing) are accepted. A guest address therefore resolves to at most one image
mapping and an FCRAM byte belongs to at most one image mapping.

Access rules for `Read*`, `Write*`, `GetPointer`, `LoadImage` and `ClearRange`:

- An access that starts in a mapping must fit entirely inside it. One that runs
  past the end throws `std::out_of_range` ("straddles the end of a guest image
  mapping"), also when the next range is adjacent. Identity mappings are the
  exception: they continue into the canonical window.
- An address below `0x08000000` that is in no mapping throws `std::out_of_range`
  ("unmapped"). A canonical address beyond the FCRAM size throws as before.
- A zero-width access exactly at the end of a mapping is allowed.
- Guest data stays little-endian. 16/32-bit accesses swap to native values on
  the big-endian PPC host; raw byte buffers (`GetPointer`, `LoadImage`) keep guest
  byte order. MMIO windows still receive numeric values.

## Aliasing and limits

- Image backing is also reachable through the canonical window at
  `0x08000000 + image_offset`. `MapImageRange` (and `ReserveImageRange`) move
  `GetImageEnd()` past that backing, so `FindFreeRange` and `ReserveDynamicRange`
  never hand it out. After the first dynamic reservation `GetImageEnd()` is frozen,
  so new image backing must already lie below it.
- Mappings are software only: no permissions, no faults delivered to the guest, no
  remapping or unmapping, and no `ControlMemory`/`MapMemoryBlock` page-table
  semantics. `ReserveImageRange` after a dynamic reservation still throws.
- The translator-side `BinaryLoader` already rejects section overlap and
  wrapping. It does not know the runtime's canonical window, so an ExHeader
  that places a non-identity section inside `0x08000000..` fails at
  `InitDispatcher` time, not at translation time.

## Host test

`wiiu_runtime/tests/guest_memory_tests.cpp` links only `wiiu_memory.cpp` and
defines the platform doubles itself. Contiguous, gapped, boundary, straddling,
overlapping, misaligned, wrapping and dynamic-range cases are covered. From the
repository root, with a C++20 compiler (`g++` shown; the MSVC form is the same
include set with `/EHsc /W4 /WX`):

```sh
g++ -std=c++20 -Wall -Wextra -Werror -Iwiiu_runtime/include -Itranslator/include -Iwiiu_runtime/tests/svc_platform -Iwiiu_runtime/tests/audio_platform -Iwiiu_runtime/tests/stubs wiiu_runtime/tests/guest_memory_tests.cpp wiiu_runtime/src/wiiu_memory.cpp -o guest_memory_tests
./guest_memory_tests
```

The test double uses `_aligned_malloc`, so it builds on Windows toolchains. The
suite runs on a little-endian host only; the PPC byte-swap path and the Wii U
build are not exercised.
