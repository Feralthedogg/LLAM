# Diagnostics

LLAM exposes structured stats for automation and human-readable dumps for rare
hangs or incident reports.

## Stats snapshots

Use size-aware stats for FFI and embedding code:

```c
llam_runtime_stats_t stats;
llam_runtime_collect_stats_ex(&stats, LLAM_RUNTIME_STATS_CURRENT_SIZE);
```

For logs, emit JSON:

```c
llam_runtime_write_stats_json(fd);
```

Consumers should ignore unknown JSON fields. The schema is additive.

## Runtime dumps

`llam_dump_runtime_state(fd)` is the artifact to capture when a task appears
stuck. It includes lifecycle state, live tasks, active I/O waiters, blocking
jobs, queue depths, shard ownership, and task-level wait ownership.

For parked tasks, inspect:

- `wait_owner=`
- `cancel_registered=`
- `deadline_ns=`
- `io_req=`
- `block_job=`

These fields separate cancellation loss, timeout loss, wake handoff loss, I/O
ownership races, and opaque blocking stalls.

## CI timeout dumps

The stress binaries support `LLAM_RUNTIME_DUMP_ON_SIGNAL` on POSIX. CI wraps
long-running commands with `scripts/run_with_timeout.py` so timeout logs include
partial stdout and a runtime dump before process cleanup.

## Expensive diagnostics

Enable these only while investigating:

| Variable | Use |
| --- | --- |
| `LLAM_TRACE_EVENTS=1` | Per-worker trace event diagnostics. |
| `LLAM_WAKE_LATENCY_METRICS=1` | Wake latency measurements. |
| `LLAM_STACK_SAMPLING=1` | Stack high-water sampling. |

These add overhead and should not define release benchmark baselines.

## Observability

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
