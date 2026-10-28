# Native AX audio

`AudioDriver` exclusively owns AX and 16 mono voices. The engine owns the driver,
joins guest workers before destroying it, and calls `Pump` each execution slice.
The runtime main initializes the driver and attaches its CSND/DSP HLE helpers.
Driver methods serialize voice state with a recursive CoreInit mutex; the AX
frame callback only increments an atomic counter, without allocating or locking.

The installed [wut AX core API](https://github.com/devkitPro/wut/blob/master/include/sndcore2/core.h)
provides `AXInit` and `AXRegisterAppFrameCallback`, not `AXRegisterApp`.
The [voice API](https://github.com/devkitPro/wut/blob/master/include/sndcore2/voice.h)
provides `AXSetVoiceSrcType`, `AXSetVoiceSrc` and `AXSetVoiceSrcRatio`, not
`AXSetVoiceSrcMaster`. Playback rates are relative to `AXGetInputSamplesPerSec`,
rather than assuming AX is initialized at 48 kHz. Both TV and DRC receive the
voice on their left/right main buses.

## Buffer formats and ownership

`PlayVoice(channel, address, rate, format, size, loop)` accepts project format IDs:

| ID | Guest input | AX buffer |
| --- | --- | --- |
| 0 | Unsigned PCM8 | Signed LPCM8, bytes XOR 0x80 |
| 1 | Signed little-endian PCM16 | Big-endian LPCM16, sample bytes exchanged |
| 2 | Nintendo DSP ADPCM | Raw DSP ADPCM, with explicit coefficient/history state |

The API is mono, using a whole-buffer loop starting at sample zero. PCM16 guest
addresses/sizes require two-byte alignment; ADPCM requires complete eight-byte
frames and an eight-byte-aligned guest address. Up to 4 MiB per voice submission
is supported. Rates must be 1000..192000 Hz and the effective SRC ratio must lie
within `[1/65536,16]`. Channels are 0..15.

ADPCM cannot be decoded from sample bytes alone. Call `SetADPCMParameters` with
the 16 signed coefficients, initial two-sample history, and loop history first.
The first frame supplies predictor/scale; invalid predictors are rejected. AX
ADPCM offsets are measured in nibbles and begin at 2, skipping the frame header.
This is Nintendo DSP ADPCM, **not CSND IMA ADPCM**. The helper's format argument
does not parse libctru CSND channel flags or arbitrary DSP firmware descriptors.

Guest samples are copied into 256-byte-aligned, driver-owned DMA allocations
with zero guard padding. `DCStoreRange` flushes the host FCRAM source pointer and
the converted upload; a numeric guest address is never passed as a host pointer.
Subsequent guest writes do not modify active playback. Resubmitting a channel
restarts it with a new snapshot. This does not yet implement gapless descriptor
queues, stereo interleaving, resupply callbacks, firmware mixing or DSP execution.

Replaced allocations remain owned until six app-frame callbacks after their
voice update, covering the default single-stage AX pipeline conservatively.
`Pump` retires these allocations on the caller thread. Pending uploads have a
16 MiB aggregate budget; submissions exceeding it fail until retirement catches
up. Stopped voices retain their current allocation until replacement or shutdown.
`AXQuit` precedes freeing all remaining DMA buffers. Hardware/Cemu validation of
DMA timing remains outstanding.

## HLE and MMIO

`csndPlaySound` and `DSP_PlayBuffer` explicitly submit to the attached driver,
returning zero on success. Invalid submissions return MVP error `0xD9030001`;
a missing driver returns `0xD9030002`. Playing/ready helpers reflect active voice
state when a driver is attached. Initialization retains the old successful
silent-stub behavior when no driver is attached; playback never silently succeeds
in that case. CSND/DSP exit stops all voices in this shared MVP mixer.

The memory bridge handles audio addresses before FCRAM validation. Direct
`Pica200::WriteMMIO` callers can also forward audio addresses through the attached
memory; audio is not interpreted as GPU state and is forbidden inside PICA lists.
MMIO values are numeric guest integers, without a second endian swap.

The following **synthetic MVP channel bank**, starting at
`0x1EC10100 + channel * 0x20`, is not the real 3DS DSP/CSND register layout:

| Offset | Meaning |
| --- | --- |
| +00 | Volume Q15, 0..0x8000 |
| +04 | Pitch multiplier 16.16, default 0x10000 |
| +08 | FCRAM source address |
| +0C | Byte length |
| +10 | Sample rate in Hz |
| +14 | Project format ID |
| +18 | Control: bit 0 start, bit 1 loop, bit 2 stop |
| +1C | Read-only active status, 0 or 1 |

Aligned 16/32-bit accesses are supported with guest little-endian halfword
selection. Volume/pitch changes affect active AX voices immediately. Unknown
audio addresses preserve the previous safe readiness/no-op stub behavior.
Automatic real CSND shared-memory command parsing, service IPC and DSP buffer
descriptor extraction are not implemented by these explicit helpers.

## Validation and listening test

`audio_driver_tests.cpp` uses production memory/HLE/driver code and AX platform
substitutes. It checks a synthetic PCM16 sine buffer byte-for-byte, signed PCM8,
ADPCM parameters/nibble offsets, TV/DRC mixing, alignment/cache flushes, gain and
pitch, deferred buffer lifetime, failed submissions, partial initialization
cleanup, HLE calls and MMIO routing. Existing stub tests remain separate.

From an MSVC developer prompt at the project root:

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX /I wiiu_runtime\include /I wiiu_runtime\tests\audio_platform /I wiiu_runtime\tests\stubs wiiu_runtime\tests\audio_driver_tests.cpp wiiu_runtime\src\audio_driver.cpp wiiu_runtime\src\hle_services.cpp wiiu_runtime\src\cro_loader.cpp wiiu_runtime\src\romfs_reader.cpp wiiu_runtime\src\wiiu_memory.cpp wiiu_runtime\src\hid_manager.cpp /Fo:build\ /Fe:build\audio_driver_tests.exe
build\audio_driver_tests.exe
```

Build the standalone listening test using `make -C wiiu_runtime audio-demo` in
the configured devkitPro environment. `build/audio_driver_demo.rpx` submits a
looping 440 Hz PCM16 tone through HLE to TV/DRC. It runs native code without a
translated guest program and does not initialize graphics. Actual audible output
in Cemu or on hardware has not been verified here.
