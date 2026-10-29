# Scalar VFPv2 and wide Thumb translation

## VFP register and arithmetic model

The CPU context holds 32 architectural 32-bit words in `vfp_bits`, with Float32
access through `GetS`/`SetS` and Float64 access through `GetD`/`SetD`. Dn aliases
S[2n] as its low word and S[2n+1] as its high word. This numeric bit assembly is
independent of PPC/PC byte order and avoids undefined union aliasing. D16–D31 are
rejected for the ARM11 VFPv2 bank.

Supported conditional ARM encodings:

- VADD, VSUB, VMUL, VDIV, VABS, VNEG and VSQRT, single/double precision.
- Signed 32-bit integer to F32; F32 to signed 32-bit integer, both truncating
  VCVT and FPSCR-rounded VCVTR forms; F32/F64 precision conversion both ways.
- VLDR/VSTR and IA/DB VLDM/VSTM, single/double registers and writeback.
- Raw VMOV between a core register and a single register; same-precision VFP
  register moves; VMRS/VMSR FPSCR, including VMRS-to-APSR NZCV via r15.

The instruction patterns follow [QEMU's VFP encoding description](https://github.com/qemu/qemu/blob/master/target/arm/tcg/vfp.decode).
Unsupported VFP encodings fail emission explicitly. FPSCR vector length/stride
and exception-trap enable modes fail at execution rather than run as scalar math.

`VFPOperation` saves the host floating environment, selects the FPSCR rounding
mode, clears host exceptions, performs the operation and merges IEEE exception
flags into FPSCR before restoring the host environment. VCVT.S32.F32 avoids C++
undefined float-to-integer conversions: NaN returns zero, and signed overflow
saturates to the relevant limit with invalid-operation status. Flush-to-zero input/output
and default-NaN modes are modeled by the CPU helpers. Loads, stores, moves and
sign operations preserve raw bits and NaN payloads without accidental arithmetic.

Build emitted math with strict rounding behavior: devkitPPC uses
`-frounding-math -ffp-contract=off`; host execution tests use MSVC `/fp:strict`.
Arithmetic uses host IEEE Float32/Float64, not a full software VFP engine. All
hardware-specific NaN propagation, precise exception/trap behavior and unusual
underflow/tininess differences still require cross-platform validation.

VFP scalar memory transfers route each 32-bit word through the endian-aware
memory interface. A double is transferred low word first, then high word; it is
never copied as a native big-endian C++ double into guest memory.

## T32 instruction representation and CFG

Thumb fetch reads LE halfwords. Prefixes with upper five bits 11101, 11110 or
11111 consume a second halfword; truncation fails. T16 opcodes occupy the low
16 bits; wide opcodes are stored as `first_halfword << 16 | second_halfword`.
`Instruction::Width()` therefore returns two or four bytes. T32 instructions
remain **halfword aligned**, including starts at page offset 0xFFE.

The CFG advances by each instruction's width, rejects overlapping instructions
and direct branches into the second half of a wide instruction, and records BLX
targets in ARM state. Fallthrough/LR addresses use the actual four-byte width.
Mode-range CLI options still identify Thumb ranges explicitly; they now accept
mixed T16/T32 within one range. A range must not split a wide instruction.

Supported wide forms:

- BL, BLX immediate and unconditional B.W, with S/J1/J2 offset reconstruction,
  sign extension, pipeline +4, Thumb-tagged return LR, and BLX word alignment.
- MOV.W immediate/register, LSL.W/LSR.W immediate or register amount, ADD.W and
  SUB.W shifted-register/modified-immediate forms, and optional flag updates.
- MOVW/MOVT and ADDW/SUBW plain immediate forms.
- LDR.W/STR.W and LDRB.W/STRB.W positive immediate, pre/post-indexed immediate,
  shifted register offset, and supported PC-relative loads. LDR-to-PC uses
  branch exchange and terminates the block. Byte transfers use bounded Read8/
  Write8 calls; byte MMIO is not added by this step.

Zero/32/>32 shift edges and modified-immediate replication are handled without
undefined C++ shifts. Unimplemented forms (including IT, conditional wide
branches, other wide ALU/memory forms and VFP in Thumb state) fail emission.
Reference: [T32 encoding descriptions](https://github.com/qemu/qemu/blob/master/target/arm/tcg/t32.decode).

ARM11 is ARMv6K: it supports the older paired Thumb BL/BLX encodings, but general
Thumb-2 data/memory instructions are a translator compatibility extension and
should not be described as native ARM11 instructions. Compatible older long-call
encodings work through the same paired-halfword fetch path.

## Verification

`translator/tests/vfp_tests.cpp` generates and executes the translator's actual
emitted bodies for arithmetic, aliases, conversions, rounding, FPSCR, register
and memory transfers. `thumb32_tests.cpp` checks mixed widths, page-crossing
fetch, positive/negative long offsets, CFG boundaries and emitted instruction
execution. Generated VFP test code is also linked with devkitPPC/wut to check the
PowerPC floating-environment/math interfaces. Host execution and native linking
do not establish identical results for every VFP corner case on Wii U hardware.
