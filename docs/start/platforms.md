# Platform support

LLAM keeps platform support tied to local kernel contracts and verification
paths. Do not infer behavior from another operating system's backend.

## Support matrix

| Platform | Status | Backend | Verification |
| --- | --- | --- | --- |
| Linux x86_64 | Primary | io_uring/liburing | `make verify-linux CC=gcc` |
| Linux aarch64 | Supported | io_uring/liburing | `make verify-linux CC=gcc` |
| macOS arm64 | Primary macOS path | kqueue | `CC=clang make verify-darwin` |
| macOS x86_64 | Supported | kqueue | `CC=clang make verify-darwin` |
| Windows 10/11 x86_64 | Supported | IOCP | native CMake/CTest and `scripts/verify_windows.ps1 -Native` |

Prebuilt macOS archives are built and tested on macOS 26 for ARM64 and macOS 15 for
Intel. For other macOS versions, use a source build and validate its deployment
target. Native asynchronous qualification remains a separate check.

Recommended compilers are GCC or Clang on Linux, Apple Clang on macOS, and MSVC with
MASM or MinGW through CMake on Windows.

## Backend notes

Linux uses io_uring for the native async path. macOS uses kqueue for
readiness and runtime wakeups, with Darwin-specific scheduler hints kept behind
Darwin checks. Windows uses IOCP for overlapped Winsock and HANDLE operations.

Windows TCP `POLLIN` currently defaults to the cooperative/direct fallback path
unless `LLAM_WINDOWS_IOCP_TCP_POLLIN=1` is enabled for controlled smoke or
benchmark runs.

The ordinary and managed constructors use compiler/image admission and a native
probe on all selected Ms before committing asynchronous execution. Their profile
matrix is separate from the runtime/backend matrix above:

| Native managed profile | Evidence and limits |
| --- | --- |
| macOS ARM64 | Ordinary compiler C, original-M/fresh-P restore, mixed compatibility lane, affine TLS destruction/refill, dynamic Ps and shutdown tested natively. |
| Linux ARM64 | Native ARM64 Linux kernel/container execution; signal mailbox, LLPM2 ELF image admission, ordinary C and registered M/P tests. |
| Linux x86-64 | Native Linux x86-64 CI covers signal frames, register/error/mask preservation, registered Ms/Ps, affine lifetime and dynamic P cases. Explicit two-P/one-CPU oversubscription tests are separate from hardware/performance qualification; wider ISA state still requires its native probe. |
| Windows x86-64 / MinGW | Actual Windows 10 x64 tests cover LLPM2 PE admission, ordinary C, registered Ms/Ps, native FLS destruction/refill and shutdown. The LLVM 23.1.2 producer was cross-hosted; native Windows LLVM tooling installation and the MSVC asynchronous profile remain separate gates. Ordinary MSVC runtime/IOCP builds are covered by native Windows CI. |
| macOS x86-64 | The ordinary Intel runtime passes native CI. The experimental Mach asynchronous adapter requires explicit enablement and is not qualified by that CI; Rosetta is rejected as a native profile. |

Qualified Linux, macOS ARM64 and MinGW builds compile the native backend by
default. `LLAM_EXPERIMENTAL_ASYNC_PREEMPT=OFF` disables it; explicit `ON` also
builds the experimental Intel macOS adapter. Compilation is not a
successful runtime probe or a release/performance qualification. Unknown ISA,
mitigation, TLS, signal or context state is refused before capture.

ARM64 native CI uses macOS 26 and its SDK so the Mach SME state query is
available. Older SDKs that omit `ARM_SME_STATE`, or hosts whose extended state
cannot be qualified, return `ENOTSUP` for native capture. Explicit `*_compat`
constructors retain their boundary/safepoint behavior on those hosts; default
constructors refuse the unsupported profile.

Mach capture retains the native thread's SSBS policy bit; a restore refuses a
policy change before mutating the thread. Linux ARM64 accepts an exact inactive,
non-streaming SVE header and retains its vector length, while live SVE, SME and
unknown records remain outside the FPSIMD profile. Linux x86-64 retains PKRU in
the full kernel XSAVE payload only while the original M's protection-key policy
matches its startup baseline. Callbacks, signal handlers and other code must not
change those native policies while continuations are saved; these profiles do
not virtualize a separate mitigation or protection-key policy for each task.

Linux x86-64 checks the kernel's shadow-stack status on each native worker.
Compiler CET instrumentation alone does not imply that shadow stacks are active;
an active or unknown protection status is refused without disabling protection.

The default constructors require a qualified asynchronous profile and reject
unsupported profiles. Explicit `*_compat` constructors retain boundary/safepoint
preemption. Intel macOS and MSVC builds compile no native backend by default, so
`llam_runtime_create()` and `llam_runtime_init()` fail with `ENOTSUP` there; use the
`*_compat` constructors on those builds. The expert Windows
caller-certified-range API remains available with its separate process-wide
fail-fast acknowledgement; it does not qualify arbitrary C or JIT mappings.
See the [managed C guide](../guides/managed-c.md) and
[build guide](build.md) for the distinct contracts and verification commands.

## BSD retirement

FreeBSD, OpenBSD, NetBSD and DragonFly BSD are unsupported from **3.0.0**.
CMake, Make, public platform headers, the installer and release packaging reject
these targets. BSD CI and release artifacts have been retired. The macOS kqueue
backend remains supported.

Use versioned 2.x sources, headers, documentation and installers for an existing
BSD deployment; the 3.0 installer does not install legacy BSD archives. This does
not promise continued 2.x maintenance. Earlier BSD adapters, experiment reports
and measurement artifacts remain historical material and do not qualify 3.0.
