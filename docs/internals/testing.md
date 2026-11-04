# Testing strategy

LLAM keeps coverage layered by cost and failure mode.

## Direct runtime tests

Direct tests own runtime correctness:

- lifecycle and shutdown
- task handle ownership
- cancellation
- lost wakeups
- channel close/select
- blocking callbacks
- multi-runtime isolation
- owned I/O buffers
- runtime diagnostics

When a server stress failure exposes a runtime bug, reduce it into a focused
runtime test. The example server remains an integration and policy workload, while
scheduler, cancellation, wakeup, select, and ownership regressions belong in direct
tests.

## Test binaries

| Binary | Covers |
| --- | --- |
| `test_abi_contract` | ABI metadata and size handshakes. |
| `test_abi_compat` | Old-prefix option and stat struct compatibility. |
| `test_shared_load` | `dlopen()` coverage for the shared library ABI surface. |
| `test_default_policy_internal` | The `ASYNC_REQUIRED` default, unqualified-entry refusal, and explicit `*_compat` constructors. |
| `test_managed_async_compiler` | A compiled image with poll-free CPU work under native capture, through the managed and default constructors. It needs `LLAM_MANAGED_TOOLING`. |
| `test_runtime_core` | Lifecycle, task metadata, yielding, sleeping, blocking callbacks, and stats. |
| `test_multi_runtime_core` | Explicit runtime-handle ownership, shutdown isolation, and cross-runtime misuse. |
| `test_runtime_api_edges` | Public API edges: task ownership, cancellation, channel close and drain, blocking callbacks, cond/mutex deadlines, and managed/unmanaged call boundaries. |
| `test_runtime_select_edges` | `llam_channel_select()` send, close, and cancel races, for lost-wakeup attribution outside the example server. |
| `test_runtime_io_dump` | A parked fd-readiness wait appears in `llam_dump_runtime_state()` as native request ownership or as an explicit blocking fallback job. |
| `test_runtime_group_local_edges` | Task-local isolation and structured task-group join, cancel, and destroy ownership. |
| `test_runtime_shutdown_internal` | Partial initialization, stop-time wakeup, internal resource cleanup, and I/O workers that outlive an active run. |
| `test_runtime_stress` | Direct scheduler, cancel, channel, condvar, nested spawn, and I/O cancel stress. |
| `test_runtime_fuzz` | Deterministic randomized scheduler, cancel, and channel scenarios. `make test-fuzz-heavy` raises it to the maximum built-in scenario counts, and `make test-runtime-soak` repeats it with changing seeds. |
| `test_sync_primitives` | Mutex, condition variable, channel, timeout, and close semantics. |
| `test_connect_io` | Direct and runtime-managed `llam_connect()` success and invalid input. |
| `test_io_buffers` | Direct and managed poll, read, and write, owned buffers, and `MSG_PEEK`. |
| `test_windows_policy` | Windows 10/11 policy selection and environment overrides. |
| `test_windows_runtime_smoke` | Native Windows scheduler and backend smoke. |
| `test_windows_iocp_io` | Windows IOCP socket and HANDLE I/O smoke. |
| `test_windows_iocp_dump` | Windows IOCP pending-request dumps for parked `AcceptEx` ownership. |
| `test_windows_handle_io` | Windows HANDLE read and write. |
| `test_security_capability` | Broker capability, token, transport, ring, descriptor, and stale-output hardening. |

For commands and profiles, see [Verification](../production/verification.md).

## Integration workloads

The example chat server is an integration workload. Lossless modes prove exact
fanout under backpressure. Best-effort flood modes allow bounded outbox drops,
but those drops must be accounted for by server-side stats.

## CI layers

| Layer | Purpose |
| --- | --- |
| PR and push gates | `linux`, `macos`, and `Stress`: fast platform tests, package smoke, static checks. |
| Stress workflow | Repeats `test_runtime_stress`, pins and reports server stress seeds, streams long logs through `scripts/run_with_timeout.py`, and uploads diagnostics on failure or timeout. |
| Nightly deep CI | `nightly.yml`: longer POSIX and Windows stress, deterministic fuzz, ASan/UBSan, allowed-failure Linux TSan diagnostics, benchmark guardrails. |
| Weekly soak | `soak.yml`: direct runtime soak plus the hour-long composite profile on Linux x86_64 and macOS arm64. |
| Runtime benchmarks | `benchmarks.yml`: scheduled LLAM/Go/Tokio comparisons with CSV/PNG artifacts. |
| Runtime diagnostic | `runtime-diagnostic.yml`: manually dispatched failure capture, with `scope` set to `all`, `linux`, `macos`, or `tsan`. |

Linux TSan is intentionally diagnostic rather than a hard push gate, because
stackful fiber runtimes can expose sanitizer-runtime interaction differences across
hosted environments. ASan/UBSan, direct runtime tests, stress jobs, and regular
platform CI remain hard gates.

Long-running stress jobs stream partial output through `scripts/run_with_timeout.py`,
request a signal-driven runtime dump before timeout shutdown on POSIX stress
binaries, and server composite runs can emit stop-time dumps into the artifact
directory.

## Security tests

Broker and public-handle hardening live in `test_security_capability` and
`llam_broker --self-test`. Transport paths also run local client/server smoke
coverage.

## Release rule

A release artifact should be published only after the target's platform-local
gate passes. Do not infer target readiness from another backend.
