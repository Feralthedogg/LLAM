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
Windows native support is planned but not shipped in 1.0.0.

Verification must be platform-local:

```bash
make verify-linux CC=gcc
CC=clang make verify-darwin
```

Do not assume benchmark parity means identical behavior. Track I/O submit
syscalls, direct path usage, wake latency, and skipped stress phases separately
per platform.

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

