# LLAM Operations Guide

This guide maps real-world deployment risks to the runtime controls, tests, and
diagnostics that should be used before treating a LLAM build as production
ready.

## 1. File Descriptor Limits

High fanout poll, accept, and inflight-I/O tests can exhaust the process
`RLIMIT_NOFILE` before they stress the runtime. Production services should set a
high soft descriptor limit before startup:

```bash
ulimit -S -n 65535
```

The bundled `stress` binary automatically clamps
`LLAM_STRESS_DYNAMIC_LIVE_POLL_WAITERS` to the current fd budget, reserving a
fixed baseline for listeners, pipes, shared libraries, and runtime internals.
If you need maximum-load validation, raise the limit explicitly and confirm the
stress log no longer prints `fd budget clamped`.

## 2. Timeout, Cancellation, and Rehome Races

Timeout and cancellation paths should always be tested together with dynamic
workers. The stress suite covers join, mutex, condition variable, channel, poll,
I/O cancellation, live accept watches, live poll watches, inflight I/O, and
worker rehome.

Recommended command:

```bash
LLAM_STRESS_ROUNDS=64 \
LLAM_STRESS_DYNAMIC_ROUNDS=8 \
LLAM_STRESS_DYNAMIC_SLEEP_TASKS=4096 \
LLAM_STRESS_DYNAMIC_SLEEP_YIELDS=32 \
LLAM_STRESS_DYNAMIC_LIVE_POLL_WAITERS=1024 \
LLAM_STRESS_PHASE_WATCHDOG_SEC=90 \
./stress
```

If a platform intentionally lacks one backend feature, the phase must print a
clear `skipped=` reason rather than silently reducing coverage.

## 3. File Descriptor Reuse

LLAM I/O watch tables must not wake a new file/socket that reused the same
numeric fd as an old watch. Linux watch lookup validates `(dev, ino)` identity
where the kernel exposes it. Darwin uses kqueue state ownership and explicit
watch cleanup. Regression coverage should include timeout/close/reopen paths and
must be run under descriptor pressure.

User code should still treat descriptor ownership conventionally: close an fd
only after all LLAM tasks that may poll, read, write, accept, or connect on it
have stopped or have observed cancellation.

## 4. CPU-Bound Tasks

LLAM remains stackful and cooperative at the actual context-switch boundary.
Automatic preemption is request-based: watchdog/runtime policy can ask an
over-budget task to give up the worker, but the task must still hit a LLAM
boundary such as `llam_task_safepoint()`, `LLAM_PREEMPT_POLL`,
`LLAM_PREEMPT_POLL_EVERY`, `llam_yield()`, a wait, or an I/O call. Long C loops
that never reach one of those boundaries can delay cancellation, watchdog
progress, allocator quiescence, and runnable task handoff.

Use `LLAM_PREEMPT_POLL_EVERY` inside CPU-heavy loops when an immediate yield
would be too expensive, and use `llam_yield()` when fairness matters more than
raw loop throughput.

Example:

```c
size_t poll_counter = 0;

for (uint64_t i = 0; i < work_items; ++i) {
    process_item(i);
    LLAM_PREEMPT_POLL_EVERY(poll_counter++, 1024U);
}
```

Operational knobs:

- `LLAM_PREEMPT_MODE=off|cooperative|auto|strict` selects the preemption
  policy. `auto` is the normal diagnostic-safe mode; `strict` is for finding
  loops that do not poll often enough.
- `LLAM_PREEMPT_POLL_PERIOD` overrides the task-local safepoint flag-poll
  period. Use this only when measuring CPU-loop latency vs overhead.
- `LLAM_PREEMPT_QUANTUM_NS` overrides the runtime slice budget. Keep it at `0`
  unless a benchmark or incident investigation needs a fixed quantum.
- `LLAM_SAFEPOINT_CLOCK_PERIOD` controls cheap-safepoint clock sampling and can
  reduce timestamp overhead in very hot loops.

## 5. Memory and Stack Pressure

Stackful tasks are fast only when task metadata and stacks are reused. For
high-fanout workloads, prewarm caches during service startup:

```bash
LLAM_TASK_CACHE_PREWARM=65536 \
LLAM_STACK_CACHE_PREWARM=8192 \
./service
```

Use larger stack classes only for known deep C call chains. Enable
`LLAM_STACK_SAMPLING=1` during staging to catch near-overflow behavior, then
disable it for release benchmarking unless diagnostics are required.

## 6. Platform Differences

Linux, kqueue platforms, and Windows have different kernel contracts. Linux
uses io_uring/liburing where available. macOS and the BSD family use kqueue for
readiness and runtime wakeups; Darwin-specific Mach scheduler/wake hints stay
behind Darwin-only guards. Windows native support covers scheduler/core, Windows
event wake handles, x86_64 context switching, IOCP-backed Winsock requests, and
overlapped HANDLE `ReadFile`/`WriteFile` requests. Windows 10 and Windows 11
share the public API; LLAM selects a `win10-conservative` or `win11-batched`
tuning policy at runtime and CI forces both branches through
`LLAM_WINDOWS_FORCE_GENERATION`.

The native Windows IOCP request backend covers overlapped `WSARecv`, `WSASend`,
`AcceptEx`, `ConnectEx`, and generic HANDLE `ReadFile`/`WriteFile` requests.
One-shot readiness support covers TCP `POLLOUT` with a zero-byte overlapped send
and UDP `POLLIN` with `WSARecvFrom(MSG_PEEK)`. TCP `POLLIN` stays on the
cooperative/direct fallback path by default; set
`LLAM_WINDOWS_IOCP_TCP_POLLIN=1` only for controlled native-probe smoke or
benchmark runs. Waitable HANDLE polling uses `WaitForSingleObject` semantics;
AF_UNIX sockets and unsupported poll masks remain documented fallback cases.
Windows 11 policy also attempts `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` on
associated socket/HANDLE objects. When Windows accepts that mode and an
overlapped operation succeeds synchronously, LLAM completes the request inline
through the normal completion finalizer instead of waiting for an IOCP packet.
Runtime dumps report `windows_skip(success_handles=... failures=...
immediate=...)` so this optimization's hit rate and compatibility can be
checked per host.

Production rollout is gated by native Windows CMake/CTest, Windows 2022/2025
stress, forced Windows 10/11 policy checks, IOCP smoke coverage, HANDLE I/O
coverage, public handle/lifetime edge tests, shutdown and owned-buffer tests,
shared-library export checks, and benchmark smoke coverage.
BSD rollout is gated by the `.github/workflows/bsd.yml` VM matrix for FreeBSD,
OpenBSD, NetBSD, and DragonFly BSD. That gate currently builds the runtime,
runs core/API/select/I/O-buffer/shared-load smoke tests, and validates release
archive shape for each target before BSD artifacts are treated as publishable.
DragonFlyBSD package repository or package-install outages are treated as
infrastructure skips because the public mirror availability has been unstable;
compiler, build, test, and package-shape failures after dependencies are present
remain hard failures. FreeBSD, OpenBSD, and NetBSD package or test failures are
not skipped.
The top-level Makefile does not maintain a separate Windows compiler pipeline:
when `HOST_PLATFORM=windows` it delegates build, test, shared/static, package,
and explicit executable/test targets to the native CMake backend. This keeps the
Windows entry point available to `make` users while preserving CMake as the
single source of truth for MSVC/MASM and IOCP target wiring.

Verification must be platform-local:

```bash
make verify-linux CC=gcc
CC=clang make verify-darwin
./scripts/docker_verify_linux.sh
.\scripts\verify_windows.ps1 -Native
```

Do not assume benchmark parity means identical behavior. Track I/O submit
syscalls, direct path usage, wake latency, and skipped stress phases separately
per platform.

## 6.1 I/O Direct-Path Expectations

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

## 6.2 Runtime Handle Boundary

`llam_runtime_create()`, `llam_runtime_run_handle()`, and
`llam_runtime_destroy()` are the canonical embedding-facing lifecycle APIs.
`llam_runtime_spawn_ex()` should be used to attach root tasks to a specific
runtime handle. Legacy host-thread lifecycle calls remain wrappers for the
process-default runtime, while managed task spawn/stop/shutdown wrappers use
the current task's owner runtime.

Explicit runtime handles own their scheduler state, active allocation caches,
blocking helper pool, and platform backend routing. Cold process-wide reusable
storage may exist only for owner-poisoned idle objects; acquisition restamps the
current owner before returning a public handle. Public handles are still backed
by process-wide family-tagged slot tables; each object is owner-tagged so
cross-runtime managed use fails with `EXDEV` instead of silently crossing queues
or wait lists. The slot tables use sealed generation tokens from a slot-stable
affine permutation of the internal epoch space, derived from table secret
material and slot identity. Trivial first-handle guesses, monotonic
next-generation guesses, and old consumed generations do not become valid
handles after object creation or slot reuse. This is an in-process UAF/FFI
hardening layer, not a cryptographic capability boundary against code that can
already read or corrupt LLAM memory.

When the threat model includes untrusted code with possible same-process memory
corruption, use a process boundary. The internal broker-control foundation keeps
capability MAC keys and object authority inside a broker-owned runtime. The
default runtime handles remain the trusted in-process fast path; ordinary
runtime creation does not require broker entropy and must not be described as a
cryptographic security boundary.

The current broker path is intentionally narrower than the public C API. It
covers subject-bound tokens, POSIX Unix-domain and Windows named-pipe control
transports, bounded buffer/channel grants, descriptor/HANDLE grants, private
ring setup over the control transport, broker-owned buffer/channel/descriptor
data-plane operations, predefined task commands, attenuation/revocation,
stale-output zeroing, active-operation pins during destroy, and raw-token
minting rejection. Treat the following as operational invariants:

- Descriptor/HANDLE authority is registered through `SCM_RIGHTS` on POSIX or
  `DuplicateHandle` from the connected named-pipe peer on Windows; raw numeric
  fd/HANDLE values in client messages are not authority.
- Broker transports bind tokens to the issuing subject/session and reject replay
  on another session with `EACCES`.
- Token issuance requires OS entropy for nonce bytes; entropy failure returns
  `EIO` and clears partial output.
- Broker wire-read failures and failed broker-owned reads clear caller output,
  and successful short reads clear the unused suffix.
- Broker-owned fds and ring transport fds are close-on-exec on POSIX.
- Broker destroy rejects new operations, drains already-pinned operations, and
  scrubs MAC keys, revocation state, object ids, and subject sessions.

`docs/security.md` is the authority for broker threat boundaries, current
coverage, non-goals, and follow-up RPC work.

Managed task lifecycle wrappers are constrained to the current owner runtime.
If task code receives a foreign runtime handle and calls `llam_runtime_destroy()`,
LLAM ignores it rather than converting that handle into a peer-runtime stop
signal. Host threads should perform explicit cross-runtime orchestration.

Spawn-time cancellation tokens follow the same owner boundary. A task can only
retain a cancellation token owned by its target runtime, and task-group spawns
publish children onto the group owner runtime rather than a caller's unrelated
current/default runtime.

The remaining hardening path is incremental:

- Keep process signal/fault hooks reference-counted and safe across concurrent
  runtime lifecycles.
- Expand platform I/O tests for completion-after-cancel, fd/socket reuse, and
  runtime-destroy ordering on Linux, macOS, and Windows.
- Keep two-runtime tests in CI so task IDs, channels, timers, cancellation
  tokens, caches, and I/O completions stay isolated.

Host applications should not repeatedly initialize and shut down the legacy
default runtime from concurrent embedding threads. Use explicit runtime handles
for concurrent embedding.

## 7. Experimental Options

The stable API exposes experiments through `experimental_flags`, but those flags
are still policy switches. Treat these as release-gated:

- Use `LLAM_RUNTIME_PROFILE_RELEASE_FAST` or defaults for production baselines.
- Enable `LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS` only after dynamic stress
  passes on the target kernel.
- Enable worker rings, multishot watches, huge allocation, or SQPOLL only with a
  before/after benchmark table and stress logs.
- Keep legacy `NM_*` environment aliases only for old automation; new scripts
  should use canonical `LLAM_*` names.

## 8. Observability

Use structured stats for automation and human dumps for incidents:

- `llam_runtime_collect_stats_ex()` for ABI-safe in-process snapshots.
- `llam_runtime_write_stats_json(fd)` for machine-readable stress/CI logs.
- `llam_dump_runtime_state(fd)` for bug reports and post-failure diagnostics.
- `LLAM_TRACE_EVENTS=1` and `LLAM_WAKE_LATENCY_METRICS=1` only when diagnosing
  tail latency, because they add measurement overhead.
- `scripts/run_with_timeout.py` for long CI stress commands that must preserve
  partial output, broadcast POSIX dump signals to wrapped runtime processes,
  and then interrupt or kill a hung process tree.

The human dump is the incident artifact to attach when a rare hang is reported.
It intentionally includes lifecycle state (`initialized`, `exec_started`,
`stop_requested`, live tasks, live shards, active I/O waiters), block-helper
state, node submit/control/watch queue depths, shard wake and I/O ownership
state, and task-level wait ownership. For a parked task, inspect
`wait_owner=`, `cancel_registered=`, `deadline_ns=`, `io_req=`, and
`block_job=` first; those fields distinguish cancellation loss, timeout loss,
wake handoff loss, I/O request ownership races, and opaque blocking stalls.

The `stress` executable honors `LLAM_RUNTIME_DUMP_ON_SIGNAL` on POSIX. When set,
`SIGUSR2` requests a dump to that path from a helper thread. CI uses this through
`scripts/run_with_timeout.py --dump-on-timeout ...` before sending the final
interrupt, so timeout artifacts contain both the partial stress log and the
latest runtime ownership snapshot.

Server edge stress logs split client-side socket failures into expected
churn/cleanup errors and unexpected failures. Treat
`unexpected_client_errors` as the gating signal; expected `EPIPE`, reset, or
cleanup-fd errors can occur when the harness intentionally resets, half-closes,
or tears down sockets.

When `OUT_DIR` or `LLAM_SERVER_COMPOSITE_DUMP_DIR` is set, the composite server
harness also wires `LLAM_CHAT_DUMP_ON_STOP` to a per-server artifact path. This
keeps shutdown-time runtime dumps next to the stress logs without requiring a
manual reproduction pass.

Minimum production counters to export are `ctx_switches`, `parks`, `wakes`,
`io_submits`, `io_submit_syscalls`, `io_completions`, `active_workers`,
`online_workers`, `queue_overflows`, `overflow_depth`, and opaque blocking
duration counters.

## 8.1 Bug-Hunter Validation Profile

When investigating LLAM itself, reproduce failures against focused runtime
tests before changing the implementation. The current high-signal profile is:

```bash
make -B test

make OBJDIR=object-asan-hunt CC=clang \
  CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  test_runtime_api_edges test_runtime_core test_runtime_fuzz \
  test_runtime_invariants test_runtime_shutdown_internal \
  test_runtime_stress test_io_buffers test_sync_primitives

ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  LLAM_RUNTIME_FUZZ_SCENARIOS=512 ./test_runtime_fuzz

ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  LLAM_RUNTIME_STRESS_ROUNDS=6 \
  LLAM_RUNTIME_STRESS_DYNAMIC_ROUNDS=4 \
  ./test_runtime_stress
```

The Makefile records object-build and link signatures, so custom sanitizer or
`OBJDIR=object-*` builds cannot silently reuse normal test binaries or leave a
normal target linked from mismatched objects. Run `make clean` between unrelated
profiles when you want a fully empty tree; it removes `object-*`, analyzer
`.plist` files, and link-signature files in addition to ordinary build outputs.

Use ThreadSanitizer on focused race tests rather than the longest stackful
edge tests:

```bash
make OBJDIR=object-tsan-hunt CC=clang \
  CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=thread -fno-omit-frame-pointer' \
  LDLIBS='-pthread -fsanitize=thread' \
  test_runtime_invariants test_runtime_fuzz test_sync_primitives

LLAM_RUNTIME_INVARIANT_SCENARIOS=24 ./test_runtime_invariants
LLAM_RUNTIME_FUZZ_SCENARIOS=64 ./test_runtime_fuzz
./test_sync_primitives
```

Treat a finding as actionable only after it is reproduced by one of the above
tests, a deterministic seed, or a small targeted reproducer. Static review
findings should become tests first unless the failure is an obvious compile-time
or contract violation.

Run the static-analysis and dependency-audit gates before release candidates or
security hardening merges:

```bash
make analyze-cppcheck
make audit-deps
make test-fuzz-heavy
make test-process-utils
make test-runtime-soak
make test-hardening
```

`make analyze-cppcheck` pins cppcheck to a 64-bit platform model because the
public handle ABI intentionally rejects 32-bit `uintptr_t` builds at
preprocessor time. `make audit-deps` audits the locked Rust comparison-harness
dependencies; LLAM's C runtime does not vendor those crates.
`make test-fuzz-heavy` runs the deterministic runtime fuzz suite at maximum
built-in single-runtime and multi-runtime scenario counts. `make
test-process-utils` verifies CI helper timeout cleanup, including descendant
process cleanup for hung benchmark/soak wrappers. `make test-runtime-soak`
repeats direct LLAM core tests for a configurable time window without the
example server policy in the loop; use `RUNTIME_SOAK_SECONDS`,
`RUNTIME_SOAK_SEED`, `RUNTIME_SOAK_FUZZ_SCENARIOS`, and
`RUNTIME_SOAK_MULTI_FUZZ_SCENARIOS` to scale it. `make test-hardening` is the
one-command local hardening gate for release candidates: it runs static
analysis, dependency audit, process-helper regression tests, ASan/UBSan, TSan,
and the heavy fuzz profile.

The current direct runtime tests are intended to catch bugs independent of the
example chat server policy: lifecycle races, task handle ownership, cancellation
token destroy/cancel races, lost wakeups, channel close/select races, blocking
callback cancellation, managed/unmanaged boundary behavior, POSIX I/O edge
semantics, owned-buffer lifetime after shutdown, and runtime dump ownership.

## 9. Channel Select Benchmarking

Channel select has three focused benchmark cases:

- `select_recv_ready`: both LLAM and comparison runtimes exercise the already
  ready path where selection should not park.
- `select_park_wake`: the waiter parks on multiple channels and a peer wakes
  one selected operation.
- `select_timeout`: the immediate-timeout/no-ready path used to measure
  validation and deadline overhead.

Run them directly with:

```bash
LLAM_BENCH_ONLY=select_recv_ready ./bench
LLAM_BENCH_ONLY=select_park_wake ./bench
LLAM_BENCH_ONLY=select_timeout ./bench
```

The cross-runtime script includes these cases for LLAM, Go, and Tokio:

```bash
python3 scripts/bench_runtime_compare.py --runtime all --rounds 9 --warmup 1 --isolate-cases
```

By default the script runs three process-level samples per runtime and reports
the median row for each benchmark case. This keeps short cases such as
`spawn_join` from being dominated by a single OS scheduling outlier. Use
`--samples 1` only for quick smoke checks. The script writes
`runtime_compare_samples.csv` beside the selected median CSV and warns when a
runtime/case has a large max/min sample spread. A spread warning is diagnostic,
not a failure; rerun with `--isolate-cases` before treating that case as a
regression.

Use `--isolate-cases` for release-quality numbers so each case runs in a fresh
process. This avoids cross-case worker-count, timer, cache, and CPU-frequency
state from a previous benchmark case leaking into the next one.

```bash
python3 scripts/bench_runtime_compare.py \
  --runtime all \
  --cases spawn_join,select_recv_ready,poll_wake \
  --rounds 31 \
  --warmup 1 \
  --isolate-cases
```

## 10. Release Gate

Before tagging a release build, require:

- `make verify-linux CC=gcc` or `./scripts/docker_verify_linux.sh` on Linux.
- `CC=clang make verify-darwin` or the macOS GitHub Actions matrix.
- BSD VM smoke through `.github/workflows/bsd.yml` for FreeBSD, OpenBSD,
  NetBSD, and DragonFly BSD before publishing BSD artifacts.
- Native Windows CMake/CTest through `scripts/verify_windows.ps1 -Native` and
  the Windows 2022/2025 stress jobs, including forced Windows 10/11 policies and
  opt-in TCP `POLLIN` IOCP smoke.
- `Nightly Deep CI` on at least one recent `main` commit before a release
  candidate: POSIX standard composite, Windows 2022/2025 policy stress,
  deterministic runtime fuzz, conservative scheduler/channel/select/I/O
  benchmark guardrails, ASan/UBSan
  quick gate, and experimental TSan diagnostics.
- `Weekly Soak` direct runtime soak plus hour-long server composite on Linux
  x86_64 and macOS arm64. The direct runtime soak repeats fuzz,
  multi-runtime ownership/isolation, runtime stress, shutdown, and owned-buffer
  tests with changing seeds. The server profile is the long-running
  stability/accounting gate: it keeps the high-rate best-effort flood load, but
  absolute delivery-MPS regression checks belong to `Stress`, `Nightly Deep CI`,
  and scheduled benchmark jobs. The soak still fails on zero traffic, missing
  stats, accounting gaps, closed-outbox drops, forced server stop,
  resource-limit violations, or unexpected edge client errors. Require this
  gate before claiming long-running runtime/server stability.
- `python3 scripts/stress_server_composite.py --quick --seed 1234` on at least one POSIX
  platform. Quick mode is a hosted-runner smoke gate and uses a lower absolute
  flood delivery threshold than standard mode; delivery ratio must still remain
  exact. Keep the printed seed from failed randomized edge runs so the same
  churn/reset sequence can be replayed.
- `python3 scripts/bench_runtime_compare.py --runtime all --isolate-cases` for
  the public LLAM/Go/Tokio comparison graph. Keep the default multi-sample
  median mode for release numbers.
- No unexplained `skipped=` phases. A skipped phase is acceptable only when the
  platform contract explicitly lacks that backend feature.
