# Platform Support

LLAM keeps platform support tied to local kernel contracts and verification
paths. Do not infer behavior from another operating system's backend.

| Platform | Status | Backend | Verification |
| --- | --- | --- | --- |
| Linux x86_64 | Primary | io_uring/liburing | `make verify-linux CC=gcc` |
| Linux aarch64 | Supported | io_uring/liburing | `make verify-linux CC=gcc` |
| macOS arm64 | Primary macOS path | kqueue | `CC=clang make verify-darwin` |
| macOS x86_64 | Supported | kqueue | `CC=clang make verify-darwin` |
| FreeBSD x86_64 | BSD CI smoke gate | kqueue | `.github/workflows/bsd.yml` |
| OpenBSD x86_64 | BSD CI smoke gate | kqueue | `.github/workflows/bsd.yml` |
| NetBSD x86_64 | BSD CI smoke gate | kqueue | `.github/workflows/bsd.yml` |
| DragonFly BSD x86_64 | Experimental | kqueue | allowed-failure BSD smoke; release artifact build remains hard-gated |
| Windows 10/11 x86_64 | Supported | IOCP | native CMake/CTest and `scripts/verify_windows.ps1 -Native` |

## Backend Notes

Linux uses io_uring for the native async path. macOS and BSD use kqueue for
readiness and runtime wakeups, with Darwin-specific scheduler hints kept behind
Darwin checks. Windows uses IOCP for overlapped Winsock and HANDLE operations.

Windows TCP `POLLIN` currently defaults to the cooperative/direct fallback path
unless `LLAM_WINDOWS_IOCP_TCP_POLLIN=1` is enabled for controlled smoke or
benchmark runs.

DragonFly BSD remains visible in CI, but its public VM/package infrastructure
is not yet reliable enough for a hard push gate.
