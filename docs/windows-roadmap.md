# Windows 10/11 Native Backend Roadmap

LLAM 1.0.0 ships a native Windows 10/11 backend candidate. The scheduler core
builds natively, uses the Windows x64 ABI context-switch assembly path, and has
been smoke-tested on Windows 10. The request backend now drives overlapped
Winsock socket I/O through IOCP for `WSARecv`, `WSASend`, `AcceptEx`, and
`ConnectEx`. One-shot readiness covers TCP `POLLOUT` through zero-byte
overlapped `WSASend` and UDP `POLLIN` through `WSARecvFrom(MSG_PEEK)` so
datagrams are not consumed. TCP `POLLIN` uses the cooperative/direct readiness
fallback because repeated overlapped stream-readiness probes are not stable
enough across Windows 10/11 loopback workloads yet; `LLAM_WINDOWS_IOCP_TCP_POLLIN=1`
enables an experimental one-byte `WSARecv(MSG_PEEK)` IOCP probe for controlled
measurement. AF_UNIX, UDP `POLLOUT`, and
multi-direction/unsupported poll masks remain on the direct/blocking fallback
path.

## Current Status

| Area | Status |
| --- | --- |
| Public platform macros | Present through `LLAM_PLATFORM_WINDOWS`, `llam_fd_t` as `SOCKET`, `LLAM_INVALID_FD`, and `LLAM_FD_IS_INVALID`. |
| Windows 10/11 detection | Present. Windows 11 is NT 10.0 build `>= 22000`; Windows 10 is NT 10.0 below that build. |
| IOCP tuning policy | Present. Both generations use IOCP, but `win10-conservative` and `win11-batched` are separate code paths with different batch, prepost, timeout, timer, and skip-completion defaults. |
| Native scheduler backend | Present for Windows x86_64 through GNU as and MASM context-switch assembly, Windows event wake handles, `VirtualAlloc` stack mappings, and runtime lifecycle smoke coverage. |
| Native I/O backend | Present for one-shot socket requests: WSARecv/WSASend, AcceptEx, ConnectEx, TCP `POLLOUT`, and UDP `POLLIN` readiness are bound to IOCP completions. TCP `POLLIN` has an opt-in IOCP probe through `LLAM_WINDOWS_IOCP_TCP_POLLIN=1`; AF_UNIX and unsupported poll masks remain fallback. |
| IOCP source layout | In progress. State/control queue helpers, socket association/extension loading, accept/op pools, control packet processing, submit path, completion path, and unsupported fallback stubs are split out of the original monolithic backend. The remaining root file now owns worker lifetime and cleanup only. |
| Native package artifacts | Release workflow now builds a Windows x86_64 archive from the native IOCP backend after Windows stress and IOCP smoke pass. |
| Verification today | Windows 10 has been verified with native MSVC CMake build/test/bench smoke. GitHub Actions covers Windows Server 2022 and Windows Server 2025 native stress smoke. WSL verification remains Linux-backend verification. |

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
- Cooperative/direct fallback for TCP `POLLIN` readiness by default, with
  `LLAM_WINDOWS_IOCP_TCP_POLLIN=1` available for experimental native probes.

The generation branch is a tuning decision, not a semantic decision. It is still
implemented as an explicit code split so native backend code can consume one
stable policy object without re-checking the OS version:

| Generation | Detection | Strategy | Concrete defaults |
| --- | --- | --- | --- |
| Windows 10 | NT `10.0`, build `< 22000` | `win10-conservative` | `completion_batch=64`, `control_batch=16`, `accept_prepost=1..2`, `recv_prepost=8..16`, `poll_timeout_ms=10`, `timer_granularity_ms=10`, `skip_completion_on_success=0`. |
| Windows 11 | NT `10.0`, build `>= 22000` | `win11-batched` | `completion_batch=64..128`, `control_batch=32`, `accept_prepost=2..4`, `recv_prepost=16..32`, `poll_timeout_ms=5`, `timer_granularity_ms=1`, `skip_completion_on_success=1`. |

The policy code lives in `src/internal/runtime_windows.h` and
`src/core/runtime_windows.c`. It is covered by `test_windows_policy`.

## IOCP Source Layout

The native backend is intentionally being decomposed by runtime responsibility:

| File | Responsibility |
| --- | --- |
| `src/io/windows/runtime_io_watch_windows_state.c` | IOCP queue/control/inflight owner state helpers. |
| `src/io/windows/runtime_io_watch_windows_socket.c` | FD association, `AcceptEx`/`ConnectEx` loading, socket type/family inspection, and poll-support gating. |
| `src/io/windows/runtime_io_watch_windows_pool.c` | Accept socket prepost cache and overlapped operation object cache. |
| `src/io/windows/runtime_io_watch_windows_control.c` | IOCP control packet drain and `CancelIoEx` request cancellation. |
| `src/io/windows/runtime_io_watch_windows_submit.c` | WSARecv/WSASend/AcceptEx/ConnectEx/poll request submission. |
| `src/io/windows/runtime_io_watch_windows_completion.c` | IOCP completion drain, accept/connect finalization, request result publication, and task wakeup. |
| `src/io/windows/runtime_io_watch_windows_fallback.c` | Unsupported multishot watch stubs for paths routed through direct/blocking fallback. |
| `src/io/windows/runtime_io_watch_windows.c` | Worker loop and IOCP node cleanup. |

The remaining decomposition target is native multishot readiness support for
currently unsupported Windows watch cases, not further file splitting. The
current CI gate covers default fallback behavior plus opt-in
`LLAM_WINDOWS_IOCP_TCP_POLLIN=1` stream-readiness probes so the native path can
be promoted only after repeated Windows 10/11 stress remains clean.

## Planned Architecture

| Subsystem | Windows path |
| --- | --- |
| Context switching | Windows x86_64 assembly preserving the Windows x64 callee-saved GPR, XMM6-XMM15, and FP-control state. LLAM keeps its own task stacks; it does not use the OS Fiber allocator. |
| I/O readiness/completion | IOCP owns one-shot socket request completions through overlapped WSARecv/WSASend, AcceptEx, ConnectEx, TCP write-readiness probes, UDP read-readiness probes, and opt-in TCP read-readiness probes. AF_UNIX and unsupported poll masks stay on fallback paths. |
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
- Run the select benchmark smoke cases `select_recv_ready`, `select_park_wake`,
  and `select_timeout` alongside the scheduler and I/O cases.
- Verify `llam_connect`, `llam_accept`, `llam_read`, `llam_write`, TCP
  `llam_poll_fd(POLLOUT)`, UDP `llam_poll_fd(POLLIN)`, and opt-in TCP
  `llam_poll_fd(POLLIN)` on native IOCP sockets; verify AF_UNIX poll fallback,
  `llam_read_owned`, and
  `llam_recv_owned`.
- Verify shared-library loading and exported `llam_*` plus compatibility
  `nm_*` symbols.
- Publish release archives only after native CI covers the target.

## 1.0.0 Boundaries

- Native Windows scheduler/core smoke support is present.
- Windows Server 2022/2025 CI covers native build, CTest, policy-forced 10/11
  smoke, and scheduler/IOCP benchmark smoke.
- Native Windows x86_64 archives are produced by the release workflow after the
  native Windows stress gate passes.
- The IOCP request backend covers one-shot socket `read`/`write`/`accept`/`connect`
  plus gated TCP `POLLOUT` and UDP `POLLIN` readiness; TCP `POLLIN` remains on
  the cooperative/direct fallback path unless `LLAM_WINDOWS_IOCP_TCP_POLLIN=1`
  is explicitly enabled.
- Windows performance comparison is still gated on longer Windows 10/11 stress
  and benchmark coverage.
- WSL verification is still Linux-backend verification, not native Windows support.
