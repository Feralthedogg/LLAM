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

LLAM is cooperative. Long C loops that neither call LLAM waits nor yield can
delay cancellation, watchdog progress, allocator quiescence, and runnable task
handoff. Use `llam_task_safepoint()` inside CPU-heavy loops when an immediate
yield would be too expensive, and use `llam_yield()` when fairness matters more
than raw loop throughput.

Example:

```c
for (uint64_t i = 0; i < work_items; ++i) {
    process_item(i);
    if ((i & 0x3ffU) == 0U) {
        llam_task_safepoint();
    }
}
```

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

Linux and macOS have different kernel contracts. Linux uses io_uring/liburing
where available, while macOS uses kqueue and Darwin-specific scheduler hints.
Windows native support covers scheduler/core, Windows event wake handles, x86_64
context switching, IOCP-backed Winsock requests, and overlapped HANDLE
`ReadFile`/`WriteFile` requests. Windows 10 and Windows 11 share the public API;
LLAM selects a `win10-conservative` or `win11-batched` tuning policy at runtime
and CI forces both branches through `LLAM_WINDOWS_FORCE_GENERATION`.

The native Windows IOCP request backend covers overlapped `WSARecv`, `WSASend`,
`AcceptEx`, `ConnectEx`, and generic HANDLE `ReadFile`/`WriteFile` requests.
One-shot readiness support covers TCP `POLLOUT` with a zero-byte overlapped send
and UDP `POLLIN` with `WSARecvFrom(MSG_PEEK)`. TCP `POLLIN` stays on the
cooperative/direct fallback path by default; set
`LLAM_WINDOWS_IOCP_TCP_POLLIN=1` only for controlled native-probe smoke or
benchmark runs. Waitable HANDLE polling uses `WaitForSingleObject` semantics;
AF_UNIX sockets and unsupported poll masks remain documented fallback cases.

Production rollout is gated by native Windows CMake/CTest, Windows 2022/2025
stress, forced Windows 10/11 policy checks, IOCP smoke coverage, HANDLE I/O
coverage, shared-library export checks, and benchmark smoke coverage.

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
`llam_runtime_destroy()` are the canonical embedding-facing lifecycle APIs, but
LLAM 1.0 still has one active runtime per process. The handle currently names
that active runtime and makes future ABI expansion explicit; it does not yet
allow two independent schedulers in one address space.

This is intentional for the 1.0 line because global task context, TLS shard/task
state, signal/fault hooks, and platform I/O ownership are still process-scoped.
Embedders should treat a second `llam_runtime_create()` returning `EBUSY` as the
defined behavior, not as a transient initialization failure.

The path to true multi-runtime support is intentionally staged:

- Move task/shard TLS lookup behind an active-runtime cursor instead of direct
  `g_llam_runtime` access.
- Make process signal/fault hooks reference-counted and shared between runtime
  instances.
- Allocate I/O nodes, block helpers, stack caches, and debug task lists from the
  `llam_runtime_t` handle only.
- Add tests that create two isolated runtimes in one process and verify that
  task IDs, channels, timers, cancellation tokens, and I/O completions never
  cross runtime boundaries.

Until those are complete, do not emulate multiple runtimes by repeatedly
initializing and shutting LLAM down from concurrent embedding threads. Serialize
runtime lifecycle calls at the host application boundary.

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

Minimum production counters to export are `ctx_switches`, `parks`, `wakes`,
`io_submits`, `io_submit_syscalls`, `io_completions`, `active_workers`,
`online_workers`, `queue_overflows`, `overflow_depth`, and opaque blocking
duration counters.

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
python3 scripts/bench_runtime_compare.py --runtime all --rounds 9 --warmup 1
```

## 10. Release Gate

Before tagging a 1.0.x build, require:

- `make verify-linux CC=gcc` or `./scripts/docker_verify_linux.sh` on Linux.
- `CC=clang make verify-darwin` or the macOS GitHub Actions matrix.
- Native Windows CMake/CTest through `scripts/verify_windows.ps1 -Native` and
  the Windows 2022/2025 stress jobs, including forced Windows 10/11 policies and
  opt-in TCP `POLLIN` IOCP smoke.
- `Nightly Deep CI` on at least one recent `main` commit before a release
  candidate: POSIX standard composite, Windows 2022/2025 policy stress,
  deterministic runtime fuzz, conservative benchmark guardrails, ASan/UBSan
  quick gate, and experimental TSan diagnostics.
- `Weekly Soak` hour-long server composite on Linux x86_64 and macOS arm64
  before claiming long-running server stability.
- `python3 scripts/stress_server_composite.py --quick --seed 1234` on at least one POSIX
  platform. Quick mode is a hosted-runner smoke gate and uses a lower absolute
  flood delivery threshold than standard/hour mode; delivery ratio must still
  remain exact. Keep the printed seed from failed randomized edge runs so the
  same churn/reset sequence can be replayed.
- The hour-long server composite profile before claiming server stability.
- `python3 scripts/bench_runtime_compare.py --runtime all` for the public
  LLAM/Go/Tokio comparison graph.
- No unexplained `skipped=` phases. A skipped phase is acceptable only when the
  platform contract explicitly lacks that backend feature.
