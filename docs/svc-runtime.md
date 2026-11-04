# SVC dispatch and native workers

ARM `SVC` and Thumb `SVC` instructions terminate their translated basic block.
The emitter stores the next guest PC, calls `wiiu::SvcHandler::Execute(cpu, id)`,
and returns. ARM conditions guard the whole operation. CPSR flags remain intact.
`CPUContext::svc_handler` is a non-owning runtime pointer; scheduling/exit fields
are runtime metadata, not architectural registers.

The engine owns its kernel instance, memory and block table. Register all blocks
and load/reserve all image/BSS ranges before running. Emitted dispatcher setup now
calls `ReserveImageRange`; old generated files should be regenerated. The heap
starts at the first page after the highest reserved image address. Without image
metadata the first FCRAM page is reserved as a minimum entry-point guard.

Image output sets `r[15]` and the Thumb flag from the translated block at the
parsed ExHeader entry, so retail `.text` is not forced to `0x08000000`. If that
entry has neither an ARM nor a Thumb block, or has both, the translator reports
`entry point 0x... has no translated block` (or `has both ARM and Thumb translated
blocks`) and writes no output file. At runtime, `DispatchOne` never calls a missing
block: it logs the guest PC, mode (`ARM`/`Thumb`), lookup key and registered block
count, then halts the main CPU. The line compares against the translator
`Entry point` line. Old generated sources predate this and leave `r[15]` at 0.

## Architectural SVC numbers

The libctru wrappers use **0x22 for ArbitrateAddress**, **0x24 for
WaitSynchronization**, and **0x27 for DuplicateHandle**. The task's 0x24/0x27
assignments would misinterpret actual binaries, so no conflicting aliases were
introduced. DuplicateHandle and multi-object WaitSynchronizationN are unsupported.
Reference: [libctru assembly](https://github.com/devkitPro/libctru/blob/master/libctru/source/svc.s).

| ID | Implemented operation / raw register ABI |
| --- | --- |
| `0x01` | ControlMemory: r0 operation, r1 requested address, r2 second address, r3 size, r4 permissions; returns result r0 and address r1 |
| `0x08` | CreateThread: r0 priority, r1 ARM/Thumb-tagged entry, r2 argument, r3 guest stack top, r4 processor; returns result r0 and handle r1 |
| `0x09` | ExitThread: request guest exit and terminate a native worker at the dispatcher boundary |
| `0x17`–`0x19` | Create, signal and clear a one-shot or sticky event |
| `0x21` | CreateAddressArbiter, returns handle r1 |
| `0x22` | ArbitrateAddress: r0 arbiter, r1 word address, r2 type, r3 signed value/count, r4:r5 timeout ns |
| `0x23` | Close supported kernel handles; closing while a waiter is pending is rejected |
| `0x24` | WaitSynchronization: r0 thread/event handle, r2:r3 timeout ns |

Unknown SVCs return `kNotSupported`. Other errors are the public `k*` MVP result
constants, not a complete Nintendo result catalog. Address-arbiter handles are
not waitable synchronization objects.

## Heap behavior

ControlMemory supports operation 3 (commit/allocate) and 1 (free), with r2 zero.
Sizes round up to 4 KiB; addresses must be page aligned and stay within
the selected O3DS `[0x08000000, 0x10000000)` or N3DS
`[0x08000000, 0x18000000)` window, above reserved images. Address zero requests first-fit
allocation. Explicit overlapping addresses fail. Free requires an exact allocation
start and matching rounded size; double frees fail. Pages are zeroed on allocation
and release. Permissions 1–7 are accepted but not enforced: this flat software
window does not enforce per-page access permissions or unmap freed pages.
Reserve/map/protect/linear-memory operations and a general MMU remain unsupported.

## Threads and synchronization

Each guest worker has its own CPUContext and caller-provided guest stack pointer.
The guest stack is validated inside FCRAM and must be 16-byte aligned. It should
belong to memory already reserved/allocated by the caller. Separately, the runtime
allocates an aligned 128 KiB PPC stack and stable OSThread storage, starts a native
trampoline using `OSCreateThread`, then calls `OSResumeThread`. Guest priorities
0–63 map to PPC priorities 0–31 by division by two. Processor 0/1 selects CPU0/1;
N3DS mode additionally maps processor 2 to CPU2. The mask is supplied at creation
and through `OSSetThreadAffinity` before resume; a failed explicit update retains
the creation mask. -1/-2 allows any Wii U core. The runtime entry point attempts
to pin the GX2 rendering thread to Core 1. This is an approximation, not 3DS scheduler fidelity.

Translated guest blocks are serialized with an OSMutex so shared FCRAM and runtime
state are not executed concurrently. Wait requests only mark the CPU blocked and
return from the block. Native workers park through per-waiter CoreInit events
outside that mutex; the main guest thread yields to rendering/input and polls
readiness on its next slice. Address predicates and decrement/write operations
run under the kernel mutex. Direct external memory access must also be serialized.

Arbitration implements signal (0), wait-if-less (1), decrement-and-wait-if-less (2),
and timeout variants (3/4), using signed guest word comparisons. Negative signal
counts wake all matching waiters; positive counts wake that many in FIFO order.
Signals are not stored as future arbitration credits. Timeouts use OS ticks with
saturating nanosecond conversion; -1 is infinite and zero is a nonblocking poll.
The MVP returns result zero on signal **or timeout**, as requested; `GetWaitState`
distinguishes Waiting, Signaled and TimedOut. Real timeout result codes and
priority-ordered arbiter selection are not modeled. Infinite waits still require
a matching signal; this code cannot repair guest synchronization bugs.

Native workers yield periodically and cannot draw through memory's PICA MMIO
bridge; GX2 remains on the main execution thread. Returning to an unmapped PC
stops that worker rather than calling an ARM address as a PPC function pointer.
Worker exit reaches `OSExitThread` after releasing all runtime locks. Main guest
exit returns to the Wii U app cleanup path, preserving ProcUI/GX2 shutdown.

Shutdown wakes parked workers, requests stop between blocks and joins all native
threads before freeing memory/stacks. There is a 64-worker lifetime limit and
256 live events/arbiters. Closed worker records remain until shutdown so native
thread storage cannot be freed prematurely. Shutdown must run on the owner/main
thread, not a guest worker.

## Verification and limits

`translator/tests/svc_emitter_tests.cpp` produces real ARM, conditional ARM and
T16 SVC bodies; `wiiu_runtime/tests/svc_tests.cpp` executes them with the production
engine/memory/kernel code. `translator/tests/entry_dispatch_tests.cpp` checks ARM,
Thumb and raw entry emission, and rejection of missing or ambiguous entry blocks
using synthetic ExHeaders. `wiiu_runtime/tests/dispatch_entry_tests.cpp` checks
entry dispatch, mode, and the halt diagnostic for a missing block with synthetic
blocks that touch no guest memory. Both are host-only, and the runtime test is not in
`scripts/run_host_tests.cmd`. The missing-block diagnostic partially addresses the
boot-crash report: the reporter's binary was not available, and no Wii U or Cemu run
verified that a title boots. Host CoreInit substitutes create actual host threads
and events. Tests check allocation boundaries/reuse, arbitration FIFO signaling,
timeouts, one-shot events, stack alignment, thread ABI/priority, create failure,
worker waits, exit and waking/joining a parked worker on shutdown.

General TLS, process handles, mutex SVCs, address-space permissions, exclusive
ARM instructions, full IPC service routing and preemptive guest scheduling remain
incomplete. This work does not establish commercial-game compatibility or native
Wii U scheduler/graphics correctness; Cemu/hardware validation is still needed.
