# Thumb-16 translation

```text
build\3ds2u_translator.exe input.code output.cpp --thumb
build\3ds2u_translator.exe input.code output.cpp --thumb-range 0x08000010:0xC
build\3ds2u_translator.exe input.code output.cpp --exheader input.exheader --thumb
```

`--thumb` fetches initialized `.text` as little-endian halfwords. For mixed images,
repeat `--thumb-range <guest_virtual_address>:<byte_size>` for known T16 regions;
the remaining text is decoded as ARM words. Ranges must not overlap or leave
`.text`, and ARM portions must remain word aligned. ExHeader metadata does not
describe instruction modes or literal pools. Register-based branch targets are
not statically inferred; their destination blocks must have been emitted in the
appropriate explicit mode range.

Supported T16 forms:

- MOV/CMP/ADD/SUB imm8, with architectural NZCV updates.
- All 16 register ALU operations, including boundary-safe register shifts and rotates.
- Word LDR/STR with imm5 scaled by four.
- PUSH/POP low registers with optional LR/PC.
- Conditional and unconditional branches, using PC+4 and signed halfword offsets.
- BX and BLX register, including execution-state changes and link return tagging.

ARM BX, BLX register/immediate, and LDM-to-PC also exchange execution state.
`CPUContext::thumb` holds state independently of the four CPSR flags. Architectural
PCs are even; `DispatchKey` tags Thumb table entries with bit zero. This permits
ARM and Thumb blocks at the same canonical address. Dispatcher initialization
selects the mode of the lowest emitted block; callers using another entry can
set CPU state or pass a Thumb-tagged address to `Run`.

T32, Thumb long BL/BLX pairs, IT, high-register data processing, immediate shifts,
byte/halfword transfers, and other omitted formats are not implemented. Fetching
a long-instruction prefix fails explicitly; other unsupported T16 instructions
fail emission rather than silently executing as no-ops.

Tests generate and execute the translator's actual C++ bodies against independent
arithmetic/shift references, and run a mixed-mode CLI fixture through a host
dispatcher. PowerPC compilation validates the real runtime ABI; Cemu/hardware
execution remains a separate check.

Encoding references: [GNU CGEN Thumb descriptions](https://sourceware.org/cgen/gen-doc/arm-thumb-insn.html)
and [Arm branch/interworking guide](https://support.arm.com/documentation/den0013/0400/ARM-Thumb-Unified-Assembly-Language-Instructions/Branches).
