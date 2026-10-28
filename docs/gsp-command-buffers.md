# GSP GPU command-stream entry point

```cpp
wiiu::GSPGPU gpu(pica200);
gpu.ProcessCommandBuffer(memory, guest_buffer_address, size_in_bytes);
```

Call this entry point while the GX2 frame is active. It consumes a PICA command
stream in guest FCRAM virtual memory, not a GSP shared-memory queue or IPC message.
Guest SVC/IPC dispatch and physical-to-virtual address translation remain separate.
The referenced Pica200 and memory objects must outlive processing.

## Packet layout

The parser follows the [libctru encoder](https://github.com/devkitPro/libctru/blob/master/libctru/source/gpu/gpu.c):

1. First parameter word.
2. Header: bits 0–15 register ID, 16–19 byte enable mask, 20–27 extra parameter
   count, bit 31 consecutive-register mode. Reserved bits 28–30 are rejected.
3. Extra parameters, if present. Count is **additional** parameters, so a packet
   always has 1–256 parameter writes.
4. One ignored padding word when the extra count is odd, aligning the next packet
   to eight bytes.

This is parameter-first ordering, not header-first. `WiiUMemory::Read32` supplies
numeric little-endian guest words on both the PPC target and host test memory;
the parser does not swap again or cast guest bytes to native words.

Consecutive mode increments the **word register ID** by one per value. Otherwise,
every value goes to the same register, preserving FIFO write order. The bridge
address is `0x1EF00000 + register_id * 4`. Byte mask bit zero enables the least
significant byte. Disabled bytes retain their previous Pica200 shadow value,
including values written through direct MMIO. A zero mask performs no write.
A draw-register write whose low byte is masked off cannot retrigger a draw.

Pica200 stores unknown registers for observation/masked updates. The extended MVP
uniform bank converts float24 values and snapshots them for GX2 matrix draws;
see [vertex uniforms](vertex-uniforms.md) for its custom address mapping.

## MVP register mapping

| Stream register ID | Existing bridge address | Meaning |
| --- | --- | --- |
| `0x0000` | `0x1EF00000` | MVP clear color |
| `0x0080` | `0x1EF00200` | MVP vertex array guest address |
| `0x0081` | `0x1EF00204` | MVP byte stride |
| `0x0082` | `0x1EF00208` | MVP vertex count |
| `0x0083` | `0x1EF0020C` | MVP draw trigger, value one |
| `0x00A0`–`0x021F` | `0x1EF00280`–`0x1EF0087C` | MVP float24 uniform scalar words |
| `0x0010` | `0x1EF00040` | FINALIZE marker |

These geometry/clear/uniform assignments retain the project's **custom MVP
protocol**. They do not match retail PICA internal-register semantics. In
particular, real PICA register ID `0x0200` maps to bridge address `0x1EF00800`,
not `0x1EF00200`. Real vertex/shader/FIFO interpretation must be added before
arbitrary libctru or commercial command streams can render correctly. The
[libctru register definitions](https://github.com/devkitPro/libctru/blob/master/libctru/include/3ds/gpu/registers.h)
identify real FINALIZE as register `0x0010`.

## Submission and failures

The caller supplies the exact byte range, aligned to eight bytes. Real libctru
submission additionally pads whole buffers to sixteen bytes; this software parser
also accepts an eight-byte final packet for synthetic callers. Zero size is a no-op.
The full advertised range must lie inside FCRAM. Wrapped addresses, truncated
payloads/padding, invalid headers and consecutive ID overflow are rejected.

An enabled write to FINALIZE stops parsing after that write; trailing submission
padding or duplicate FINALIZE packets are ignored. Without a marker, reaching the
provided size also completes the list. Syntax is checked before applying any
register writes up to the first FINALIZE.

Draw triggers queue snapshots of vertex address, stride and count. At completion,
all queued geometry ranges are validated, then one GX2 draw is issued for **each
trigger**, in order. This preserves multiple draws instead of collapsing them
into the final register state. Native submission starts only after parsing ends.
Successful draws request presentation through the existing cooperative CPU yield.
Direct MMIO draws retain their immediate behavior.

On execution failure, queued draws are discarded and batching is reset. Applied
register writes are not rolled back, and a GPU/allocation failure during native
submission can occur after earlier draws were submitted. There is no concurrent
command-buffer mutation support. Nested command lists are rejected.

## Host verification

From an MSVC x64 developer command prompt at the repository root:

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX /I wiiu_runtime\tests\stubs /I wiiu_runtime\include wiiu_runtime\tests\gsp_gpu_tests.cpp wiiu_runtime\src\gsp_gpu.cpp wiiu_runtime\src\display_transfer.cpp wiiu_runtime\src\pica200.cpp wiiu_runtime\src\texture_decoder.cpp /Fo:build\ /Fe:build\gsp_gpu_tests.exe
build\gsp_gpu_tests.exe
```

The suite executes the real parser and Pica200 bridge against host LE memory and
a recording GX2 substitute. It checks command layout/padding, register state,
byte masks, FIFO/consecutive writes, finalization, deferred/multiple draws,
geometry payload bytes and malformed-buffer recovery. Native rendering still
requires Cemu or Wii U testing.
