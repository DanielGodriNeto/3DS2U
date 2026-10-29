# HID register bridge

The engine polls VPAD once at the beginning of each cooperative `Run` slice.
`VPADInit`/`VPADShutdown` remain owned by the runtime entry point. No additional
input poll runs in `main.cpp`.

The bridge retains its custom register protocol and now provides HID/IRRST
shared-memory ring snapshots. Retail HID service handles, shared-memory mapping
SVCs and input events are not automatically emulated. See [N3DS expansion](n3ds-expansion.md)
for ring offsets and integration. The custom word registers are:

| Address | Read value |
| --- | --- |
| `0x1EC00000` | Held keys |
| `0x1EC00004` | Pressed keys |
| `0x1EC00008` | Released keys |
| `0x1EC0000C` | Touch X, 0–319 |
| `0x1EC00010` | Touch Y, 0–239 |
| `0x1EC00014` | Touch active, 0 or 1 |
| `0x1EC00018` | Circle Pad, packed signed 16-bit X/Y |
| `0x1EC0001C` | C-Stick, packed signed 16-bit X/Y (N3DS) |
| `0x1EC00020` | Extended held keys (N3DS) |

Buttons use 3DS bits 0–11: A, B, Select, Start, Right, Left, Up, Down, R, L,
X, Y. N3DS mode adds ZL/ZR at bits 14/15, C-Stick directions at bits 24–27,
and both modes expose Circle Pad directions at bits 28–31. VPAD Minus/Plus map to Select/Start.
Touch coordinates are calibrated
at 854×480, clamped, then scaled to the 3DS bottom screen. Inaccurate or
inactive touches return zero coordinates and inactive status.

Byte reads and aligned 16-bit reads select the corresponding little-endian portion of a register;
aligned 32-bit reads return its numeric value. MMIO values bypass FCRAM byte
swapping. Unsupported/unaligned reads and writes fail rather than access FCRAM.
Pressed/released bits persist throughout a slice and clear on the next poll.
No-sample/busy results retain held/touch state. Other read failures clear state
and report the held buttons as released for one slice.

Actual VPAD/Cemu behavior requires console or emulator validation.
