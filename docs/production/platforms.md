# Platform differences

Linux, macOS, and Windows have different kernel contracts. Verify and measure
each one on its own platform.

## Kernel contracts

Linux uses io_uring/liburing where available. macOS uses kqueue for
readiness and runtime wakeups; Darwin-specific Mach scheduler/wake hints stay
behind Darwin-only guards. Windows native support covers scheduler/core, Windows
event wake handles, x86_64 context switching, IOCP-backed Winsock requests, and
overlapped HANDLE `ReadFile`/`WriteFile` requests. Windows 10 and Windows 11
share the public API; LLAM selects a `win10-conservative` or `win11-batched`
tuning policy at runtime and CI forces both branches through
`LLAM_WINDOWS_FORCE_GENERATION`.

## Windows IOCP backend

The native Windows IOCP request backend covers overlapped `WSARecv`, `WSASend`,
`AcceptEx`, `ConnectEx`, and generic HANDLE `ReadFile`/`WriteFile` requests.
One-shot readiness support covers TCP `POLLOUT` with a zero-byte overlapped send.
TCP and UDP `POLLIN` stay on the cooperative/direct fallback path by default; set
`LLAM_WINDOWS_IOCP_TCP_POLLIN=1` for one-byte `WSARecv(MSG_PEEK)` probes or
`LLAM_WINDOWS_IOCP_UDP_POLLIN=1` for `WSARecvFrom(MSG_PEEK)` probes only for
controlled native-probe smoke or benchmark runs. Waitable HANDLE polling uses `WaitForSingleObject` semantics;
AF_UNIX sockets and unsupported poll masks remain documented fallback cases.
Windows 11 policy also attempts `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` on
associated socket/HANDLE objects. When Windows accepts that mode and an
overlapped operation succeeds synchronously, LLAM completes the request inline
through the normal completion finalizer instead of waiting for an IOCP packet.
Runtime dumps report `windows_skip(success_handles=... failures=...
immediate=...)` so this optimization's hit rate and compatibility can be
checked per host.

## Rollout gates

Production rollout is gated by native Windows CMake/CTest, Windows 2022 stress on
each push, Windows 2022 and 2025 stress in the nightly run, forced Windows 10/11
policy checks, IOCP smoke coverage, HANDLE I/O
coverage, public handle/lifetime edge tests, shutdown and owned-buffer tests,
shared-library load checks, and benchmark smoke coverage.
BSD support ended in 3.0.0; no BSD VM or artifact gate remains active.
The top-level Makefile does not maintain a separate Windows compiler pipeline:
when `HOST_PLATFORM=windows` it delegates build, test, shared/static, package,
and explicit executable/test targets to the native CMake backend. This keeps the
Windows entry point available to `make` users while preserving CMake as the
single source of truth for MSVC/MASM and IOCP target wiring.

## Verify on each platform

Verification must be platform-local:

=== "Linux"

    ```sh
    make verify-linux CC=gcc
    ./scripts/docker_verify_linux.sh
    ```

=== "macOS"

    ```sh
    CC=clang make verify-darwin
    ```

=== "Windows"

    ```powershell
    .\scripts\verify_windows.ps1 -Native
    ```

Do not assume benchmark parity means identical behavior. Track I/O submit
syscalls, direct path usage, wake latency, and skipped stress phases separately
per platform.

## I/O direct-path expectations

Managed `read`, `write`, `recv`, `poll`, `connect`, and `accept` first try an
immediate direct path when that can complete without parking the current task.
For `accept`, this means a connection already present in the kernel listen
backlog can be consumed directly before LLAM submits a backend request. This
keeps serial connect/accept tests from depending on backend re-arm timing and
reduces one worker round trip on hot server paths.

On macOS, managed `accept` calls that cannot use multishot accept-watch default
to the compensated helper path controlled by `LLAM_ACCEPT_DIRECT_BLOCKING`. The
helper keeps the listener nonblocking and polls in short slices, avoiding
scheduler-worker pinning while sidestepping one-shot kqueue accept races
observed on hosted runners. Plain `llam_accept(fd, NULL, NULL)` still prefers
the multishot accept-watch backend after the immediate direct backlog probe.

The backend path is still the correctness path for not-ready descriptors,
timeouts, cancellation, and unsupported direct operations. If a direct path
regresses, compare both throughput and `io_submit_syscalls`; a lower syscall
count is useful only when the observed completion rate and tail latency do not
regress.
