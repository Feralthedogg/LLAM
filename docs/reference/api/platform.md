# Platform types

Portable platform types and feature macros, plus the experimental Windows
asynchronous preemption opt-in.

## Platform types

| Type or macro | Purpose |
| --- | --- |
| `llam_fd_t` | POSIX fd or Windows `SOCKET`. |
| `llam_handle_t` | POSIX fd alias or Windows HANDLE-compatible type. |
| `llam_iovec_t` / `llam_mut_iovec_t` | Portable immutable and mutable scatter/gather slices. |
| `LLAM_INVALID_FD` / `LLAM_FD_IS_INVALID` | Portable fd/socket invalid sentinel and predicate. |
| `LLAM_INVALID_HANDLE` / `LLAM_HANDLE_IS_INVALID` | Portable handle invalid sentinel and predicate. |
| `LLAM_PLATFORM_WINDOWS` / `LLAM_PLATFORM_POSIX` | Major platform-family feature macros. |
| `LLAM_PLATFORM_LINUX` / `LLAM_PLATFORM_DARWIN` / `LLAM_PLATFORM_BSD` | Platform detection macros; BSD detection now leads to a build error. |
| `LLAM_PLATFORM_FREEBSD` / `LLAM_PLATFORM_OPENBSD` / `LLAM_PLATFORM_NETBSD` / `LLAM_PLATFORM_DRAGONFLY` | Legacy BSD detection macros; unsupported in 3.0.0. |
| `LLAM_PLATFORM_KQUEUE` | Non-zero on macOS targets. |
| `LLAM_ARCH_X86_64` / `LLAM_ARCH_AARCH64` | Public architecture feature macros. |
| `LLAM_PLATFORM_NAME` | Human-readable platform name used by diagnostics. |
| `LLAM_API` | Public symbol visibility/import/export macro. |

## Experimental Windows preemption

`llam_runtime_enable_experimental_windows_async_preempt_ex()` atomically opts
an idle, fixed-primary Win64 runtime from `llam_runtime_create_compat()` into the
experimental backend. The caller passes
nonoverlapping, half-open, **caller-certified** PE-image PC ranges and the
`LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST` flag. LLAM copies the ranges,
checks PE executable backing and machine support, and pins their modules; it
cannot prove that arbitrary code is safe to interrupt. A committed remote
context failure can terminate the entire hosting process. Other builds retain
the public symbol but return `ENOTSUP`. See [Options](../options/runtime.md) and the
[Windows build contract](../../start/build.md#build-on-windows) before using it.
A successful call racing destroy reports that enablement committed first, not
that the runtime remains usable after the call returns.
Each actual primary worker must register before user dispatch starts. Captured
full native contexts stay on their capture worker; only a completed eligible
explicit cooperative yield with all old asynchronous ownership retired can
release a G for ordinary migration. Internal yields and park/offload wakes
remain origin-bound. Do not retain native-TLS addresses across migration.
Dynamic scaling, async helper/relief execution and inline foreign detach are
not admitted. Native qualification currently covers two and three primary Ms
with the pinned scalar-leaf fixture, not every workload or compiler profile.
