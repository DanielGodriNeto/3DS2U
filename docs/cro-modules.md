# CRO module loading

`CROLoader` accepts unfixed little-endian CRO0 images. The magic is at file offset
`0x80`, after four SHA-256 hashes; the fixed header ends at `0x138`. Header fields,
tables, strings, segment tags, import slices, relocation batches and patch ranges
are checked before modifying guest memory. Unsupported relocations, static CRS
relocations, unresolved imports and duplicate exports fail with `LastError()`.

The caller chooses a page-aligned free address inside the runtime FCRAM window.
The complete file is mapped there, preserving code/rodata/data file offsets. BSS
segments are zeroed consecutively after the page-rounded file length. Segment
descriptors provide the resulting guest addresses. Header/table offsets in the
copied guest file remain file offsets; this does not implement Nintendo's in-place
rebasing/fixing of CRO headers or its separate data/BSS mapping ABI.

Named exports join the loader's shared symbol namespace. Register main-executable
symbols with `RegisterSymbol(name, address)` before loading modules. Indexed and
anonymous imports resolve against previously loaded modules selected by name.
Module dependencies prevent unloading a provider while another loaded module
imports it. Circular and late-bound unresolved imports are unsupported.

```cpp
wiiu::CROLoader loader;
loader.RegisterSymbol("main_function", 0x08001000u);
wiiu::CROModule module;
if (!loader.LoadModule(memory, image.data(), image.size(), 0x08100000u, module)) {
    // Report loader.LastError().
}
```

## Relocations

Patches are composed bytewise as guest little-endian values, independent of host
endianness. Relocation records carry explicit signed addends, rather than addends
decoded from the old instruction. Supported types:

| Type | Interpretation |
| --- | --- |
| 0 | NONE |
| 2 | ABS32: `S + A`, preserving a Thumb symbol's state bit |
| 3 | REL32: `S + A - P` |
| 10 | THM_CALL: Thumb BL/BLX, range and alignment checked |
| 28 | CALL: ARM BL/BLX, including interworking |
| 29 | JUMP24: ARM B/conditional BL; interworking requires a veneer |
| 38 | TARGET1, absolute-address policy |
| 41 | TARGET2, explicit absolute-address policy for this runtime |
| 42 | PREL31, preserving the existing high bit |

Branch displacement uses `S + A - P`, clearing the symbol's Thumb state bit.
A call directly to a symbol normally uses `A=-8` in ARM and `A=-4` in Thumb;
Thumb BLX also accounts for alignment of the architectural PC. No veneers are
generated. Type 39 is SBREL31, **not TARGET2**, and is rejected.

The IDs and expressions follow [Arm's ELF ABI](https://github.com/ARM-software/abi-aa/blob/main/aaelf32/aaelf32.rst).
CRO record layouts were checked against the [Azahar CRO declarations](https://github.com/azahar-emu/azahar/blob/master/src/core/hle/service/ldr_ro/cro_helper.h)
and the [3dbrew CRO layout](https://www.3dbrew.org/wiki/CRO0).

## RO HLE and lifetime

`HLEServiceManager` owns a loader exposed through `GetCROLoader()` and provides
explicit `RO_RegisterModule`, `RO_LoadModule`, and `RO_UnloadModule` handlers.
Load takes source FCRAM address/size, destination address, and an output descriptor.
Register snapshots the four CRO hashes or the bounded hash table from a
[CRR0 header](https://www.3dbrew.org/wiki/CRR0). Registration performs structural
checks only: SHA-256/RSA attestation and matching CRO hashes to CRR entries are
not implemented, and loading does not require successful attestation. These are
HLE helper APIs, not a complete `ro:MOVE`/`ro:1` IPC protocol or service registry.

Use the same memory owner for the loader's lifetime. Serialize calls under the
engine execution lock, and quiesce all execution in a module before unloading.
Memory reservations are shared with SVC heap allocations; overlaps with images,
heaps or other modules fail. Unload clears the mapping, releases its reservation,
and removes its exports. Guest memory is still a flat buffer: reservations and
`CROModule::IsExecutable` are software metadata, not enforced MMU permissions.

**Loading guest instruction bytes does not produce PowerPC code.** The AOT tool
must pretranslate CRO functions at their final addresses and register dispatcher
blocks before execution. This change does not add JIT/interpreter fallback,
runtime dispatcher mutation, CRS extraction, or automatic module entry/exit calls.

## Validation

`wiiu_runtime/tests/cro_tests.cpp` exercises production memory and HLE paths with
host platform substitutes. It covers ABS32, TARGET aliases, signed ARM/Thumb calls,
interworking, internal relocations, named/indexed/anonymous linking, BSS, rejected
malformed inputs with unchanged memory, dependent unload, reservations and CRR
metadata. From an MSVC developer prompt in the project root:

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX /I wiiu_runtime\include /I wiiu_runtime\tests\audio_platform /I wiiu_runtime\tests\stubs wiiu_runtime\tests\cro_tests.cpp wiiu_runtime\src\cro_loader.cpp wiiu_runtime\src\romfs_reader.cpp wiiu_runtime\src\hle_services.cpp wiiu_runtime\src\aoc_manager.cpp wiiu_runtime\src\save_manager.cpp wiiu_runtime\src\audio_driver.cpp wiiu_runtime\src\wiiu_memory.cpp wiiu_runtime\src\hid_manager.cpp /Fo:build\ /Fe:build\cro_tests.exe
build\cro_tests.exe
```
