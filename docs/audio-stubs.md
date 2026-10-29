# Audio HLE stubs

This page describes the fallback when no `AudioDriver` is attached. The runtime
now attaches [native AX audio](audio-driver.md); its explicit playback calls and
synthetic channel registers replace silent behavior for supported submissions.

`HLEServiceManager::csndInit`, `csndExit`, `dspInit`, and `dspExit` log stub
success and return zero. No audio device, firmware, service handle, IPC session,
DMA transfer or playback is created. Generated code must explicitly call these
helpers; arbitrary guest SVC/IPC service requests are not intercepted here.

Nonblocking helper responses are `dspGetSemaphoreState() == 1`,
`dspIsChannelReady(channel) == true`, and `csndIsChannelPlaying(channel) == false`.
Without a driver they remain constant across initialization/exit calls. With a
driver attached, playing/ready queries reflect its active AX voice state.

The memory bridge intercepts aligned 16/32-bit accesses in
`[0x1EC10000, 0x1EC18000)` before FCRAM address validation. Writes log a no-op
and never touch FCRAM. Reads return zero except these **synthetic MVP registers**:

| Address | Numeric read value |
| --- | --- |
| `0x1EC10004` | `1`: channel/DMA ready |
| `0x1EC10008` | `1`: DSP semaphore signaled |

The flags are always asserted and cannot be cleared by writes. Halfword reads
use guest little-endian ordering: the low half returns one and the high half
returns zero. MMIO returns numeric values directly, without a second PPC byte
swap; ordinary FCRAM accesses still swap on PowerPC. Unaligned/cross-boundary
accesses fail rather than reach physical memory.

This is not a register-accurate CSND/DSP implementation. Default-zero reads
satisfy waits for idle/clear bits; other real hardware polling protocols may need
additional explicit handlers. These stubs cannot ensure arbitrary homebrew audio
initialization completes. Audio remains silent.

`audio_stub_tests.cpp` links the production service and memory implementations
against host heap/logging/input/graphics substitutes, checking service logs,
status flags, MMIO routing/bounds, and guest memory byte order.
