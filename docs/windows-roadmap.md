# Windows 10/11 Native Backend Roadmap

LLAM 1.0.0 does not ship a native Windows runtime backend. Windows support is
planned, and the current repository keeps only the public platform/API boundary
and WSL verification path in place.

## Current Status

| Area | Status |
| --- | --- |
| Public platform macros | Present through `LLAM_PLATFORM_WINDOWS`, `llam_fd_t` as `SOCKET`, `LLAM_INVALID_FD`, and `LLAM_FD_IS_INVALID`. |
| Native scheduler backend | Planned. |
| Native I/O backend | Planned. |
| Native package artifacts | Not published in 1.0.0. |
| Verification today | Use WSL to run the Linux backend through `scripts/verify_windows.ps1`. |

`scripts/verify_windows.ps1 -Native` intentionally exits with status `2` until
the native backend is complete.

## Planned Architecture

| Subsystem | Planned Windows path |
| --- | --- |
| Context switching | Fiber-based switching or dedicated Windows assembly path, selected after measurement. |
| I/O readiness/completion | IOCP for sockets and overlapped operations. |
| Scheduler wake | Windows wait handles or keyed events integrated with worker wake paths. |
| Blocking compensation | Windows thread-pool or dedicated helper workers aligned with existing `llam_call_blocking` semantics. |
| Timers | Waitable timers or shard-local timer batching behind the existing timer API. |
| Packaging | Native Windows archives after CI can build, test, and smoke-run the backend. |

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
- Run the full test suite equivalent to `make test` or CMake `ctest`.
- Run `demo`, `stress`, and `bench` smoke tests.
- Verify `llam_connect`, `llam_accept`, `llam_poll_fd`, `llam_read_owned`, and
  `llam_recv_owned` on native sockets.
- Verify shared-library loading and exported `llam_*` plus compatibility
  `nm_*` symbols.
- Publish release archives only after native CI covers the target.

## Non-Goals For 1.0.0

- No native Windows archive is published.
- No IOCP backend is claimed as complete.
- No Windows performance comparison is claimed.
- WSL verification is not treated as native Windows support.
