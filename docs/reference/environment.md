# Environment Variables

Environment variables are operational and tuning controls. Prefer source-level
runtime options for libraries and use environment overrides for deployment,
diagnostics, benchmarks, and CI.

## Runtime Profiles

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_RUNTIME_PROFILE` | `balanced`, `release-fast`, `debug-safe`, `io-latency` | Select the high-level runtime policy. |
| `LLAM_PREEMPT_MODE` | `off`, `cooperative`, `auto`, `strict` | Select cooperative preemption policy. |
| `LLAM_PREEMPT_POLL_PERIOD` | `0`-`4096` | Override task-local safepoint flag-poll period. |
| `LLAM_PREEMPT_QUANTUM_NS` | nanoseconds | Override global preemption slice. |
| `LLAM_SAFEPOINT_CLOCK_PERIOD` | `0`-`4096` | Bound cheap-safepoint clock sampling. |
| `LLAM_YIELD_DIRECT_HANDOFF` | `0`, `1`, unset | Allow ordinary yields to switch directly to same-shard runnable work when safe. |

## Experimental Runtime Options

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_EXPERIMENTAL_DYNAMIC_WORKERS` | `0`, `1` | Soft-park and reactivate idle workers. |
| `LLAM_EXPERIMENTAL_WORKER_RINGS` | `0`, `1` | Enable per-worker I/O ring mode. |
| `LLAM_EXPERIMENTAL_WORKER_RINGS_MULTISHOT` | `0`, `1` | Enable multishot watches with worker rings. |
| `LLAM_EXPERIMENTAL_LOCKFREE_NORMQ` | `0`, `1` | Use the lock-free normal queue. |
| `LLAM_EXPERIMENTAL_HUGE_ALLOC` | `0`, `1` | Prefer hugepage-friendly allocator backing. |
| `LLAM_EXPERIMENTAL_SQPOLL` | `0`, `1` | Enable Linux io_uring SQPOLL experiment. |
| `LLAM_SQPOLL_CPU` | CPU number | Select SQPOLL CPU. |
| `LLAM_IDLE_SPIN_NS` | nanoseconds | Idle spin time before kernel sleep. |
| `LLAM_IDLE_SPIN_ITERS` | iteration count | Idle spin iteration limit. |
| `LLAM_BIND_WORKERS` | `0`, `1` | Bind worker threads to platform CPUs when supported. |

## I/O Policy

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_DIRECT_BLOCKING_IO` | `0`, `1` | Allow eligible compensated direct blocking socket I/O. |
| `LLAM_DIRECT_BLOCKING_POLL` | `0`, `1`, unset | Control direct blocking poll fallback. |
| `LLAM_ACCEPT_DIRECT_BLOCKING` | `0`, `1` | Use compensated accept helper path where needed. |
| `LLAM_WINDOWS_IOCP_TCP_POLLIN` | `0`, `1` | Opt into Windows TCP `POLLIN` IOCP probe path. |
| `LLAM_IO_POLL_REDIRECT_TIMEOUT_MS` | milliseconds | Redirect long direct-poll waits through opaque blocking compensation on Linux. |
| `LLAM_IO_COOP_YIELD` | `0`, `1` | Enable cooperative yields around direct I/O fast paths. |
| `LLAM_IO_POLL_COOP_YIELD` | `0`, `1` | Enable cooperative yields in poll readiness paths. |
| `LLAM_IO_POLL_PRE_YIELD` | `0`, `1` | Let poll hand off to same-shard producers before the first readiness probe. |
| `LLAM_IO_POLL_EXTRA_YIELD` | `0`, `1` | Add an extra poll-readiness yield. |
| `LLAM_IO_POLL_READY_YIELDS` | `0`-`8` | Bound short ready-yield probes before parking. |
| `LLAM_READ_READY_INITIAL_HANDOFF` | `0`, `1` | Let `llam_read_when_ready()` hand off once before its first read probe. |
| `LLAM_READ_READY_DIRECT_BLOCKING` | `0`, `1` | Let infinite `llam_read_when_ready()` use compensated direct blocking reads. |
| `LLAM_POLL_SOCKET_PEEK` | `0`, `1` | Use `MSG_PEEK` for socket `POLLIN` fast checks. |
| `LLAM_IO_WRITE_HANDOFF` | `0`, `1` | Yield after small socket writes so local readers can run. |
| `LLAM_IO_WRITE_DIRECT_LOCAL_HANDOFF` | `0`, `1` | Prefer direct same-shard handoff after eligible socket writes. |

## Platform Policy

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_DARWIN_MACH_SCHED` | `0`, `1` | Toggle Darwin Mach/QoS scheduler hints. |
| `LLAM_WINDOWS_UNSAFE_SKIP_TASK_SIMD` | `0`, `1` | Experimental Windows x64 ceiling mode: skip task-context XMM6-XMM15 save/restore. |
| `LLAM_AARCH64_UNSAFE_SKIP_SCHEDULER_SIMD` | `0`, `1` | Experimental macOS/Linux ARM64 ceiling mode: skip scheduler-context SIMD save/restore. |
| `LLAM_ARM64_UNSAFE_SKIP_SCHEDULER_SIMD` | `0`, `1` | Alias for `LLAM_AARCH64_UNSAFE_SKIP_SCHEDULER_SIMD`. |

## Caches And Memory

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_TASK_CACHE_PREWARM` | task count | Prewarm task metadata slabs. |
| `LLAM_STACK_CACHE_PREWARM` | stack count | Prewarm stack caches. |
| `LLAM_TIMER_HEAP_PREWARM` | timer slots | Preallocate timer heap capacity. |
| `LLAM_STACK_SAMPLING` | `0`, `1` | Enable stack high-water diagnostics. |
| `LLAM_OPAQUE_REDIRECT_FASTPATH` | `0`, `1` | Prefer redirect over helper handoff for opaque blocking. |

## Diagnostics

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_TRACE_EVENTS` | `0`, `1` | Enable per-worker trace rings. |
| `LLAM_WAKE_LATENCY_METRICS` | `0`, `1` | Enable wake latency metrics. |
| `LLAM_RUNTIME_DUMP_ON_SIGNAL` | path | POSIX stress dump target for signal-triggered dumps. |

## Install And Release Helpers

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_INSTALL_VERSION` | version | Default version used by release install scripts. |
| `LLAM_VERSION` | version | Version override used by SDK metadata and release/package flows. |
| `LLAM_ABI_MAJOR` | integer | ABI major override used by SDK metadata generation. |

## Verification And Benchmarks

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
| `LLAM_STRESS_DYNAMIC_LIVE_POLL_WAITERS` | waiter count | Stress live poll/accept/inflight waiters; clamped by fd budget. |

## Example Server Harness

These variables tune bundled example-server tests, not the LLAM public ABI.

| Variable | Values | Meaning |
| --- | --- | --- |
| `LLAM_CHAT_LOSSLESS` | `0`, `1` | Run the chat server with lossless outbox backpressure. |
| `LLAM_CHAT_STATS_PATH` | path | Write chat server stats for flood/composite harnesses. |
| `LLAM_CHAT_DUMP_ON_STOP` | path | Write a runtime dump when the chat server stops. |
| `LLAM_SERVER_FLOOD_CLIENTS` | count | Flood client count. |
| `LLAM_SERVER_FLOOD_DURATION` | seconds | Flood duration. |
| `LLAM_SERVER_FLOOD_DRAIN_SEC` | seconds | Drain window after flood traffic. |
| `LLAM_SERVER_FLOOD_MESSAGE_BYTES` | bytes | Flood payload size. |
| `LLAM_SERVER_FLOOD_BATCH` | count | Messages sent per client batch. |
| `LLAM_SERVER_FLOOD_TARGET_MPS` | rate | Target messages per second. |
| `LLAM_SERVER_FLOOD_MIN_DELIVERY_MPS` | rate | Minimum accepted delivery rate. |
| `LLAM_SERVER_FLOOD_MIN_DELIVERY_RATIO` | ratio | Minimum accepted delivery ratio. |
| `LLAM_SERVER_FLOOD_SHUTDOWN_TIMEOUT` | seconds | Server shutdown timeout. |
| `LLAM_SERVER_FLOOD_SERVER_LOSSLESS` | `0`, `1` | Start the target server in lossless mode. |
| `LLAM_SERVER_FLOOD_ALLOW_FORCED_STOP` | `0`, `1` | Allow forced server stop. |
| `LLAM_SERVER_FLOOD_FAIL_ON_FORCED_STOP` | `0`, `1` | Treat forced stop as failure. |
| `LLAM_SERVER_FLOOD_ALLOW_MISSING_STATS` | `0`, `1` | Allow missing stats file. |
| `LLAM_SERVER_FLOOD_FAIL_ON_MISSING_STATS` | `0`, `1` | Treat missing stats as failure. |
| `LLAM_SERVER_FLOOD_DUMP_DIR` | path | Dump directory for flood diagnostics. |
| `LLAM_SERVER_COMPOSITE_DUMP_DIR` | path | Dump directory for composite diagnostics. |

For C structs, build settings, and install script options, see [Options](options.md).
