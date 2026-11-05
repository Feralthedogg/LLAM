# Environment variables

Environment variables are operational and tuning controls. Prefer source-level
runtime options for libraries and use environment overrides for deployment,
diagnostics, benchmarks, and CI.

The runtime reads these variables when a runtime starts or on first use, unless a
section says a bundled program reads them instead. Where a variable and an
`llam_runtime_opts_t` field set the same thing, such as `LLAM_RUNTIME_PROFILE` and
`profile`, the variable wins.

## Runtime profiles

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_RUNTIME_PROFILE` | `balanced`, `release-fast`, `debug-safe`, `io-latency` | Select the high-level runtime policy. |
| `LLAM_PREEMPT_MODE` | `off`, `cooperative`, `auto`, `strict` | Select the preemption request policy. `auto`, the default, requests preemption under budget pressure; `strict` is diagnostic and polls aggressively. `off` and `cooperative` apply to compatibility runtimes: they make `llam_runtime_create()` and `llam_runtime_init()` fail with `EINVAL`. |
| `LLAM_PREEMPT_POLL_PERIOD` | `0`-`4096` | Override the task-local safepoint flag-poll period. Unset uses the profile default; `0` and `1` check at every safepoint. `strict` mode forces `1`. |
| `LLAM_PREEMPT_QUANTUM_NS` | nanoseconds | Override the global preemption slice, capped at one second. `0` uses task-class defaults. |
| `LLAM_SAFEPOINT_CLOCK_PERIOD` | `0`-`4096` | Bound cheap-safepoint clock sampling. `0` samples every safepoint. |
| `LLAM_YIELD_DIRECT_HANDOFF` | `0`, `1`, `2`, unset | Let a yield switch directly to same-shard runnable work. `0` disables it, `1` limits it to the handoff after a socket write, and `2` (or `full`, `ordinary`) also covers ordinary yields. Unset selects `2` on Linux and macOS, except in `debug-safe`, and on Windows only in `release-fast`. Direct yields need `LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ` on a compatibility runtime. With no runnable peer, a direct yield keeps the current task running for up to 64 consecutive turns. |

`auto` and `strict` do not by themselves enable asynchronous capture on a
compatibility constructor. Native async support compiles by default on qualified
targets, but it activates only through the ordinary or
[managed constructors](../guides/managed-c.md), the expert Windows API, or the private
Linux x86-64 safe-range path described in
[Preemption metadata](../guides/preempt-metadata.md).

## Experimental options

The bundled `demo`, `stress`, and `bench` programs read these variables and turn
them into `llam_runtime_opts_t` fields. The runtime library does not read them, so
setting one has no effect on your own program. Set `experimental_flags` and the
other [runtime options](options/runtime.md#runtime-options) in code instead.

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_EXPERIMENTAL_DYNAMIC_WORKERS` | `0`, `1` | Soft-park and reactivate idle workers. These programs default to `1`. |
| `LLAM_EXPERIMENTAL_WORKER_RINGS` | `0`, `1` | Enable per-worker I/O ring mode. |
| `LLAM_EXPERIMENTAL_WORKER_RINGS_MULTISHOT` | `0`, `1` | Enable multishot watches with worker rings. |
| `LLAM_EXPERIMENTAL_LOCKFREE_NORMQ` | `0`, `1` | Toggle the lock-free normal queue. These programs default to `1`. |
| `LLAM_EXPERIMENTAL_HUGE_ALLOC` | `0`, `1` | Prefer hugepage-friendly allocator backing. |
| `LLAM_EXPERIMENTAL_SQPOLL` | `0`, `1` | Enable Linux io_uring SQPOLL experiment. |
| `LLAM_SQPOLL_CPU` | CPU number | Select the SQPOLL CPU. |
| `LLAM_IDLE_SPIN_NS` | nanoseconds | Idle spin time before kernel sleep. Read only by `bench`. |
| `LLAM_IDLE_SPIN_ITERS` | iteration count | Idle spin iteration limit. Read only by `bench`. |

## I/O policy

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_DIRECT_BLOCKING_IO` | `0`, `1` | Allow eligible blocking socket read/write operations to run through compensated direct blocking regions; default is disabled. |
| `LLAM_DIRECT_BLOCKING_POLL` | `0`, `1`, unset | Control direct blocking poll fallback. Unset uses it for finite waits on Linux, and for infinite waits only when the backend cannot poll; for finite waits on macOS in `release-fast` and `io-latency`; and never on Windows. |
| `LLAM_ACCEPT_DIRECT_BLOCKING` | `0`, `1` | Route managed `accept` calls that cannot use multishot accept-watch through a compensated helper poll loop; default is enabled on kqueue platforms and disabled elsewhere. |
| `LLAM_WINDOWS_IOCP_TCP_POLLIN` | `0`, `1` | Opt into Windows TCP `POLLIN` IOCP probe path. |
| `LLAM_WINDOWS_IOCP_UDP_POLLIN` | `0`, `1` | Opt into Windows UDP `POLLIN` IOCP probe path. |
| `LLAM_IO_POLL_REDIRECT_TIMEOUT_MS` | `0`-`3600000` | Redirect direct-poll waits at least this long through opaque blocking compensation, and keep shorter finite waits off the direct path. Default is `1000` on Linux and `0`, which disables it, elsewhere. |
| `LLAM_IO_COOP_YIELD` | `0`, `1` | Enable cooperative yields around direct I/O fast paths; default is enabled on kqueue platforms, Linux, and Windows. |
| `LLAM_IO_POLL_COOP_YIELD` | `0`, `1` | Enable cooperative yields in poll readiness paths; default is enabled on kqueue platforms, Linux, and Windows. |
| `LLAM_IO_POLL_PRE_YIELD` | `0`, `1` | Let poll hand off to same-shard runnable producers before the first readiness probe; default is enabled on kqueue platforms and Windows. |
| `LLAM_IO_POLL_EXTRA_YIELD` | `0`, `1` | Add an extra poll-readiness yield; default is enabled on kqueue platforms and Windows. |
| `LLAM_IO_POLL_READY_YIELDS` | `0`-`8` | Bound short same-shard ready-yield probes before poll parks in the backend; default is `2` on kqueue platforms and Windows and `1` on Linux. |
| `LLAM_READ_READY_INITIAL_HANDOFF` | `0`, `1` | Let `llam_read_when_ready()` hand off once to local producers before its first read probe; default is disabled. |
| `LLAM_READ_READY_DIRECT_BLOCKING` | `0`, `1` | Let infinite `llam_read_when_ready()` use compensated direct blocking reads; default is disabled. |
| `LLAM_POLL_SOCKET_PEEK` | `0`, `1` | Use `MSG_PEEK` for socket `POLLIN` fast checks; default is enabled on kqueue platforms and Windows and opt-in on Linux. |
| `LLAM_IO_WRITE_HANDOFF` | `0`, `1` | Yield after small socket writes so local readers can run; default is enabled on macOS, Linux, and Windows. |
| `LLAM_IO_WRITE_DIRECT_LOCAL_HANDOFF` | `0`, `1` | Prefer direct same-shard task handoff after eligible socket writes; default is enabled on macOS, Linux, and Windows. |
| `LLAM_IO_WRITE_HANDOFF_REQUIRE_WORK` | `0`, `1` | Skip the write handoff when the shard has no other local work; default is enabled. |
| `LLAM_IO_WRITE_HANDOFF_RECENT_YIELD_NS` | nanoseconds | Skip the write handoff this soon after an explicit yield; default is `1000000` on kqueue platforms and Windows and `0`, which disables it, on Linux. |
| `LLAM_IO_WRITE_HANDOFF_CHECK_FD` | `0`, `1` | Inspect the descriptor's blocking and socket state before the handoff; default is enabled only on Windows. |

## Platform policy

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_BIND_WORKERS` | `0`, `1` | Bind worker threads to CPUs. Linux only; default is disabled. |
| `LLAM_DARWIN_MACH_SCHED` | `0`, `1` | Toggle Darwin Mach/QoS scheduler hints; default is enabled on macOS. |
| `LLAM_ASYNC_CAPSULE_BUDGET_BYTES` | positive byte count | Experimental Linux x86-64 async-preemption capsule budget; default `67108864` bytes. |
| `LLAM_ASYNC_BLOCK_JOB_LIMIT` | positive integer | Experimental Linux x86-64 async-preemption blocking-job admission limit; default `1024`. |
| `LLAM_FP_CONTROL_CONTEXT` | `0`, `1` | Preserve floating-point control state across x86_64 context switches; default is enabled. |
| `LLAM_XSAVE_CONTEXT` | `0`, `1` | Save extended state with XSAVE on Linux x86_64 context switches; default is disabled because it raises per-context allocation and switch cost. |
| `LLAM_WINDOWS_HIGH_RES_WAKE_TIMER` | `0`, `1` | Use a high-resolution waitable timer for Windows wakes; default is enabled. |
| `LLAM_WINDOWS_NONBLOCK_CACHE` | `0`, `1` | Cache nonblocking socket state on Windows; default is disabled, because a recycled `SOCKET` value can make an entry stale. |
| `LLAM_WINDOWS_UNSAFE_SKIP_SCHEDULER_SIMD` | `0`, `1` | Experimental Windows x64 ceiling mode: skip scheduler-context SIMD save/restore. |
| `LLAM_WINDOWS_UNSAFE_SKIP_TASK_SIMD` | `0`, `1` | Experimental Windows x64 ceiling mode: skip task-context XMM6-XMM15 save/restore. Only valid when managed tasks do not rely on callee-saved SIMD state across LLAM yields/waits. |
| `LLAM_AARCH64_UNSAFE_SKIP_SCHEDULER_SIMD` | `0`, `1` | Experimental macOS/Linux ARM64 ceiling mode: skip scheduler-context SIMD save/restore while task contexts still preserve ABI-required `d8-d15`. |
| `LLAM_ARM64_UNSAFE_SKIP_SCHEDULER_SIMD` | `0`, `1` | Alias for `LLAM_AARCH64_UNSAFE_SKIP_SCHEDULER_SIMD`. |

## Adaptive tuning

The watchdog-attached governor is off unless you turn it on. `observe` publishes
windows and JSON state without changing policy.

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_AUTOTUNE` | `off`, `observe`, `on`, `frozen` | Enable the watchdog-attached adaptive guardrail/probe governor. `observe` publishes windows and JSON state without changing policy; `on` probes the direct handoff budget only when the sampled hit rate suggests the current budget is poor. |
| `LLAM_AUTOTUNE_DOMAINS` | `workers,idle,handoff,preempt,io` | Restrict the domains the governor may observe or eventually control. Unset defaults to the safe idle/handoff set, plus workers when dynamic workers are active. |
| `LLAM_AUTOTUNE_DECISION_INTERVAL_NS` | nanoseconds | Override the low-frequency governor decision interval. `0` uses the default 100ms cadence. |
| `LLAM_AUTOTUNE_MIN_HOLD_NS` | nanoseconds | Record the minimum policy hold period used by future controllers. `0` uses the default 500ms hold. |
| `LLAM_AUTOTUNE_SAMPLE_PERIOD` | count | Downsample autotune-only handoff and wake-latency telemetry. Rounded up to a power of two and capped at `1024`. Default is `64`; `1` records every event. |
| `LLAM_AUTOTUNE_WAKE_P99_NS` | nanoseconds | Optional wake-latency rollback guardrail. `0` leaves latency sampling off in `on` mode; nonzero samples p50/p99 and rolls back the handoff budget when sampled p99 exceeds the target. `observe` mode samples without changing policy. |
| `LLAM_DIRECT_HANDOFF_STATS` | `0`, `1` | Record full handoff counters whatever the autotune sample period is. |

## Caches and memory

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_TASK_CACHE_PREWARM` | task count | Prewarm task metadata per processor, rounded up to 16-object slabs; default 16 in balanced/debug-safe and 128 in release-fast/io-latency. `0` disables prewarm; capped at 4096. |
| `LLAM_STACK_CACHE_PREWARM` | stack count | Prewarm default stacks for the runtime before high fanout workloads; default 128, or 256 in release-fast. `0` disables prewarm; capped at 4096. |
| `LLAM_TIMER_HEAP_PREWARM` | timer slots | Preallocate shard timer heap slots to avoid growth during sleep/deadline fanout; default 1024 in release-fast, 0 in debug-safe, and 512 otherwise. Capped at 1048576. |
| `LLAM_STACK_SAMPLING` | `0`, `1` | Enable stack high-water sampling diagnostics. |
| `LLAM_OPAQUE_REDIRECT_FASTPATH` | `0`, `1` | Prefer redirect over helper handoff for compatibility opaque blocking; default is disabled. |
| `LLAM_CHANNEL_CACHE_CAP` | count | Reusable capacity-one channels kept in the process-wide cache; default 64, capped at 4096. |
| `LLAM_CHANNEL_TLS_CACHE_CAP` | count | Reusable capacity-one channels kept per thread; default 16, capped at 1024. |

## Scheduler internals

These tune internal fast paths. Change them only to measure or diagnose one of them,
and keep the defaults in production.

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_WAKE_HANDOFF` | `0`, `1` | Let a wake hand off directly to a same-shard waiter; default is enabled except in debug-safe. |
| `LLAM_CHANNEL_LOCAL_HANDOFF` | `0`, `1` | Apply that handoff to channel wakes; defaults to the `LLAM_WAKE_HANDOFF` value. |
| `LLAM_CHANNEL_SAFEPOINT_INTERVAL` | `0`-`65536` | Channel operations between safepoints; default 1 in debug-safe, 64 in release-fast, and 32 otherwise. `0` is treated as `1`. |
| `LLAM_YIELD_DIRECT_HANDOFF_BURST` | `0`-`65535` | Starting budget for consecutive direct handoffs; default 64. `0` removes the budget. A direct yield with no runnable peer stops at 64 turns whatever this value is. |
| `LLAM_YIELD_DIRECT_HANDOFF_ALLOW_TIMERS` | `0`, `1` | Allow a direct yield handoff to a peer while timers are armed; default is enabled on macOS except in debug-safe, and disabled elsewhere. A yield with no peer and a parked join or select always stop at an armed timer. |
| `LLAM_YIELD_DIRECT_HANDOFF_LIVE_LIMIT` | `0`-`1048576` | Live-task limit above which direct handoff stops applying; `0` means no limit. Default is `0`, or `16` on Windows above two processors. |
| `LLAM_SPAWN_FANOUT_WAKE_INTERVAL` | `0`-`65535` | Wake other workers when a spawn finds this many tasks in the target's normal queue. `0` disables it. Default is `0`, or `128` on Windows except in debug-safe. |
| `LLAM_SPAWN_FANOUT_ADAPTIVE` | `0`, `1` | On Windows, raise that threshold while no I/O or timers are active, unless `LLAM_SPAWN_FANOUT_WAKE_INTERVAL` is set. Default is disabled. |
| `LLAM_TASK_LIST_EAGER` | `0`, `1` | Link new tasks into the diagnostic task list at spawn, rather than when they first park; default is enabled only in debug-safe. |
| `LLAM_STRICT_SAFEPOINT` | `0`, `1` | Run full safepoints instead of cheap ones; default is disabled. |
| `LLAM_DIAG_LIGHT_SAFEPOINT` | `0`, `1` | Force cheap safepoints on or off. Takes precedence over `LLAM_STRICT_SAFEPOINT`. |
| `LLAM_OPAQUE_HANDOFF_SPIN_ITERS` | count | Helper-handoff spin iterations for opaque blocking; default 16. |
| `LLAM_OPAQUE_MACH_SEM` | `0`, `1` | Use Mach semaphores for the opaque helper on macOS; default is enabled. |
| `LLAM_OPAQUE_MACH_SEM_INDEFINITE` | `0`, `1` | Let that semaphore wait without a timeout; default is disabled. |
| `LLAM_OPAQUE_DARWIN_ULOCK` | `0`, `1` | Use Darwin ulock for the opaque helper; default is disabled. |

## Diagnostics

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_TRACE_EVENTS` | `0`, `1` | Enable per-worker trace ring diagnostics. |
| `LLAM_WAKE_LATENCY_METRICS` | `0`, `1` | Enable wake-latency diagnostics. |
| `LLAM_RUN_TIMING` | `0`, `1` | Enable task run-time accounting. Stack sampling also turns it on. |
| `LLAM_OPAQUE_TIMING` | `0`, `1` | Enable opaque-blocking timing counters. |
| `LLAM_DYNAMIC_TRACE` | `0`, `1` | Log dynamic worker scaling decisions. |
| `LLAM_RUNTIME_DUMP_ON_SIGNAL` | path | POSIX stress dump target for signal-triggered dumps. |

## Install and release

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_INSTALL_VERSION` | version | Default version used by release install scripts. |
| `LLAM_VERSION` | version | Version override used by SDK metadata and release/package flows. |
| `LLAM_ABI_MAJOR` | integer | ABI major override used by SDK metadata generation. |
| `LLAM_LLVM_BIN` | path | LLVM tool directory for the managed `llam-clang` driver. |
| `LLAM_MANAGED_PLUGIN` | path | Managed compiler plugin path, overriding the driver's search. |

## Verification and benchmarks

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_VERIFY_LINUX_EXPERIMENTAL` | `0`, `1` | Enable experimental Linux verification paths. |
| `LLAM_VERIFY_DARWIN_EXPERIMENTAL` | `0`, `1` | Enable experimental Darwin verification paths. |
| `LLAM_BENCH_ONLY` | case name | Run one benchmark case. |
| `LLAM_BENCH_ROUNDS` | count | Benchmark measurement rounds. |
| `LLAM_BENCH_WARMUP_ROUNDS` | count | Benchmark warmup rounds. |
| `LLAM_BENCH_SPAWN_TASKS` | count | Spawn benchmark size. |
| `LLAM_BENCH_CHANNEL_MESSAGES` | count | Channel benchmark message count. |
| `LLAM_BENCH_IO_MESSAGES` | count | I/O echo benchmark message count. |
| `LLAM_BENCH_POLL_EVENTS` | count | Poll wake benchmark event count. |
| `LLAM_BENCH_SLEEP_TASKS` | count | Sleep fanout task count. |
| `LLAM_BENCH_OPAQUE_SCOPES` | count | Opaque blocking benchmark scope count. |
| `LLAM_STRESS_DYNAMIC_LIVE_POLL_WAITERS` | waiter count | Stress live poll/accept/inflight waiters; automatically clamped by fd budget. |

`bench` also reads `LLAM_BENCH_SELECT_OPS`, `LLAM_BENCH_SLEEP_US`,
`LLAM_BENCH_SLEEP_YIELDS`, `LLAM_BENCH_CSV`, `LLAM_BENCH_ASYNC`,
`LLAM_BENCH_POLL_FUSED`, `LLAM_BENCH_POLL_INDEFINITE`,
`LLAM_BENCH_REUSE_SOCKETPAIR`, and `LLAM_BENCH_WINDOWS_AFUNIX`. `stress` reads the
other `LLAM_STRESS_*` round, phase, and sleep settings, and `cpu_spike` reads
`LLAM_CPU_SPIKE_*`.

Some scripts take flag defaults from the environment: `LLAM_SERVER_STRESS_*` for
`stress_server.py`, `LLAM_SERVER_COMPOSITE_*` for `stress_server_composite.py`,
`LLAM_BENCH_COMPARE_*` for `bench_runtime_compare.py`, and `LLAM_BENCH_GUARD_*` for
`bench_guard.py`. The script's [CLI reference](cli/index.md) gives the meaning of
each flag.

## Example server harness

These variables tune bundled example-server tests, not the LLAM public ABI.

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_CHAT_LOSSLESS` | `0`, `1` | Run the chat server with lossless outbox backpressure. |
| `LLAM_CHAT_QUIET` | `0`, `1` | Quiet chat server logging. |
| `LLAM_CHAT_STATS_PATH` | path | Write chat server stats for flood/composite harnesses. |
| `LLAM_CHAT_DUMP_ON_STOP` | path | Write a runtime dump when the chat server stops. |
| `LLAM_SERVER_FLOOD_CLIENTS` | count | Flood client count. |
| `LLAM_SERVER_FLOOD_DURATION` | seconds | Flood duration. |
| `LLAM_SERVER_FLOOD_DRAIN_SEC` | seconds | Drain window after flood traffic. |
| `LLAM_SERVER_FLOOD_MESSAGE_BYTES` | bytes | Flood payload size. |
| `LLAM_SERVER_FLOOD_BATCH` | count | Messages sent per client batch. |
| `LLAM_SERVER_FLOOD_TARGET_MPS` | rate | Target inbound rate in millions of messages per second; `0.30` is 300,000. |
| `LLAM_SERVER_FLOOD_MIN_DELIVERY_MPS` | rate | Minimum accepted delivery rate in millions of deliveries per second; `1.3` is 1,300,000. |
| `LLAM_SERVER_FLOOD_MIN_DELIVERY_RATIO` | ratio | Minimum accepted delivery ratio. |
| `LLAM_SERVER_FLOOD_SHUTDOWN_TIMEOUT` | seconds | Server shutdown timeout. |
| `LLAM_SERVER_FLOOD_SERVER_LOSSLESS` | `0`, `1` | Start the target server in lossless mode. |
| `LLAM_SERVER_FLOOD_ALLOW_FORCED_STOP` | `0`, `1` | Allow forced server stop. |
| `LLAM_SERVER_FLOOD_FAIL_ON_FORCED_STOP` | `0`, `1` | Treat forced stop as failure. |
| `LLAM_SERVER_FLOOD_ALLOW_MISSING_STATS` | `0`, `1` | Allow missing stats file. |
| `LLAM_SERVER_FLOOD_FAIL_ON_MISSING_STATS` | `0`, `1` | Treat missing stats as failure. |
| `LLAM_SERVER_FLOOD_DUMP_DIR` | path | Dump directory for flood diagnostics. |
| `LLAM_SERVER_COMPOSITE_DUMP_DIR` | path | Dump directory for composite diagnostics. |

## Legacy names

Old automation can still use earlier spellings for variables read through the
runtime's environment helper: the runtime library's own settings and the C
benchmark, demo, and stress programs built on `examples/env_compat.h`. For those,
any `LLAM_FOO` falls back to `NM_FOO`, and the worker variables also accept
`LLAM_EXPERIMENTAL_SHARD_RINGS`, `LLAM_EXPERIMENTAL_SHARD_RINGS_MULTISHOT`, and
`LLAM_EXPERIMENTAL_DYNAMIC_SHARDS`.

Everything else reads only the `LLAM_` name: the chat server and flood programs
(`LLAM_CHAT_*`, `LLAM_SERVER_FLOOD_*`), `LLAM_RUNTIME_DUMP_ON_SIGNAL`,
`LLAM_WINDOWS_FORCE_GENERATION`, Python tools, install scripts, and compiler
drivers. Use the canonical names in new scripts.

For C structs, build settings, and install script options, see [Options](options/runtime.md).
