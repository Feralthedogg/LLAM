# Windows 10/11 Native Backend Roadmap

LLAM 1.0.0 ships a native Windows 10/11 backend candidate. The scheduler core
builds natively, uses the Windows x64 ABI context-switch assembly path, and has
been smoke-tested on Windows 10. The request backend now drives overlapped
Winsock socket I/O through IOCP for `WSARecv`, `WSASend`, `AcceptEx`, and
`ConnectEx`. One-shot readiness covers TCP `POLLOUT` through zero-byte
overlapped `WSASend` and UDP `POLLIN` through `WSARecvFrom(MSG_PEEK)` so
datagrams are not consumed. TCP `POLLIN` uses the cooperative/direct readiness
fallback because repeated overlapped stream-readiness probes are not stable
enough across Windows 10/11 loopback workloads yet. AF_UNIX, UDP `POLLOUT`, and
multi-direction/unsupported poll masks remain on the direct/blocking fallback
path.

## Current Status

| Area | Status |
| --- | --- |
| Public platform macros | Present through `LLAM_PLATFORM_WINDOWS`, `llam_fd_t` as `SOCKET`, `LLAM_INVALID_FD`, and `LLAM_FD_IS_INVALID`. |
| Windows 10/11 detection | Present. Windows 11 is NT 10.0 build `>= 22000`; Windows 10 is NT 10.0 below that build. |
| IOCP tuning policy | Present. Both generations use IOCP, but `win10-conservative` and `win11-batched` are separate code paths with different batch, prepost, timeout, timer, and skip-completion defaults. |
| Native scheduler backend | Present for Windows x86_64 through GNU as and MASM context-switch assembly, Windows event wake handles, `VirtualAlloc` stack mappings, and runtime lifecycle smoke coverage. |
| Native I/O backend | Present for one-shot socket requests: WSARecv/WSASend, AcceptEx, ConnectEx, TCP `POLLOUT`, and UDP `POLLIN` readiness are bound to IOCP completions. TCP `POLLIN`, AF_UNIX, and unsupported poll masks remain fallback. |
| Native package artifacts | Not published until Windows 10 and Windows 11 CI pass the full acceptance gate. |
| Verification today | CMake can build static/shared runtime libraries and Windows-native policy/scheduler smoke tests. WSL verification remains available for the Linux backend. |

`scripts/verify_windows.ps1 -Native` reports the detected Windows generation,
selected IOCP policy family, builds the native CMake targets, and runs the
Windows CTest suite. It also reruns the Windows-native subset with
`LLAM_WINDOWS_FORCE_GENERATION=10` and `=11` so both policy branches are covered
on a single host.

## Windows 10 vs Windows 11 Policy

Windows 10 and Windows 11 do not require different public APIs. LLAM uses the
same completion primitives on both:

- `CreateIoCompletionPort` for backend ownership.
- `GetQueuedCompletionStatusEx` for batched completion drains.
- `PostQueuedCompletionStatus` for control/wake packets.
- Overlapped Winsock operations for `read`, `write`, `accept`, and `connect`.
- Zero-byte overlapped `WSASend` for TCP `POLLOUT` readiness.
- `WSARecvFrom(MSG_PEEK)` for UDP `POLLIN` readiness.
- Cooperative/direct fallback for TCP `POLLIN` readiness.

The generation branch is a tuning decision, not a semantic decision. It is still
implemented as an explicit code split so native backend code can consume one
stable policy object without re-checking the OS version:

| Generation | Detection | Strategy | Concrete defaults |
| --- | --- | --- | --- |
| Windows 10 | NT `10.0`, build `< 22000` | `win10-conservative` | `completion_batch=64`, `control_batch=16`, `accept_prepost=1..2`, `recv_prepost=8..16`, `poll_timeout_ms=10`, `timer_granularity_ms=10`, `skip_completion_on_success=0`. |
| Windows 11 | NT `10.0`, build `>= 22000` | `win11-batched` | `completion_batch=64..128`, `control_batch=32`, `accept_prepost=2..4`, `recv_prepost=16..32`, `poll_timeout_ms=5`, `timer_granularity_ms=1`, `skip_completion_on_success=1`. |

The policy code lives in `src/internal/runtime_windows.h` and
`src/core/runtime_windows.c`. It is covered by `test_windows_policy`.

## Planned Architecture

| Subsystem | Windows path |
| --- | --- |
| Context switching | Windows x86_64 assembly preserving the Windows x64 callee-saved GPR, XMM6-XMM15, and FP-control state. LLAM keeps its own task stacks; it does not use the OS Fiber allocator. |
| I/O readiness/completion | IOCP owns one-shot socket request completions through overlapped WSARecv/WSASend, AcceptEx, ConnectEx, TCP write-readiness probes, and UDP read-readiness probes. TCP read readiness, AF_UNIX, and unsupported poll masks stay on fallback paths. |
| Scheduler wake | Windows event handles integrated through the existing shard/node wake abstraction. |
| Blocking compensation | Dedicated helper workers using `WaitOnAddress` wake words instead of pthread condvar waits on native Windows. |
| Timers | Scheduler idle waits can use high-resolution waitable timers for sub-16ms precise deadlines. |
| Packaging | Native Windows archives after CI can build, test, smoke-run, and benchmark both Windows 10 and Windows 11. |

## Public Contract

The Windows backend must preserve the existing public API and ABI contract:

- `llam_*` remains the canonical public API.
- `nm_*` remains compatibility-only.
- `llam_fd_t` is a Windows `SOCKET` on native Windows builds.
- `LLAM_INVALID_FD` maps to `INVALID_SOCKET`, and descriptor-returning APIs
  should be checked with `LLAM_FD_IS_INVALID(fd)`.
- I/O functions keep syscall-style return conventions: success value or the
  documented failure sentinel with `errno` set.
- `llam_abi_get_info()` and public struct size handshakes stay unchanged.

Any Windows-only implementation detail must stay below `src/` and must not
appear in public headers unless it is part of the documented platform contract.

## Acceptance Gate

Native Windows support can be marked supported only after all of these pass on
Windows 10 and Windows 11:

- Build static and shared runtime libraries with the selected compiler toolchain.
- Run native Windows CMake tests: ABI, runtime core, sync primitives, Windows
  policy, Windows runtime smoke, and Windows IOCP socket round-trip.
- Run `demo`, `stress`, and `bench` smoke tests.
- Verify `llam_connect`, `llam_accept`, `llam_read`, `llam_write`, TCP
  `llam_poll_fd(POLLOUT)`, and UDP `llam_poll_fd(POLLIN)` on native IOCP
  sockets; verify TCP `llam_poll_fd(POLLIN)`, AF_UNIX poll fallback,
  `llam_read_owned`, and
  `llam_recv_owned`.
- Verify shared-library loading and exported `llam_*` plus compatibility
  `nm_*` symbols.
- Publish release archives only after native CI covers the target.

## 1.0.0 Boundaries

- Native Windows scheduler/core smoke support is present.
- Native Windows archives are not published until full Windows 10/11 CI lands.
- The IOCP request backend covers one-shot socket `read`/`write`/`accept`/`connect`
  plus gated TCP `POLLOUT` and UDP `POLLIN` readiness; TCP `POLLIN` remains on
  the cooperative/direct fallback path.
- Windows performance comparison is still gated on longer Windows 10/11 stress
  and benchmark coverage.
- WSL verification is still Linux-backend verification, not native Windows support.
