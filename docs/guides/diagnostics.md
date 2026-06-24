# Diagnostics

LLAM exposes structured stats for automation and human-readable dumps for rare
hangs or incident reports.

## Stats Snapshots

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

## Runtime Dumps

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

## CI Timeout Dumps

The stress binaries support `LLAM_RUNTIME_DUMP_ON_SIGNAL` on POSIX. CI wraps
long-running commands with `scripts/run_with_timeout.py` so timeout logs include
partial stdout and a runtime dump before process cleanup.

## Expensive Diagnostics

Enable these only while investigating:

| Variable | Use |
| --- | --- |
| `LLAM_TRACE_EVENTS=1` | Per-worker trace event diagnostics. |
| `LLAM_WAKE_LATENCY_METRICS=1` | Wake latency measurements. |
| `LLAM_STACK_SAMPLING=1` | Stack high-water sampling. |

These add overhead and should not define release benchmark baselines.
