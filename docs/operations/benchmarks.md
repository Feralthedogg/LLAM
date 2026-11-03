# Benchmarks

Benchmarks are for regression tracking and platform tuning. They should not be
mixed with correctness claims.

For every benchmark script parameter, see [CLI And Tools](../reference/cli-tools.md#benchmark-scripts).

## Run All Cases

```sh
./bench
```

Run one case:

```sh
LLAM_BENCH_ONLY=spawn_join ./bench
LLAM_BENCH_ONLY=channel_pingpong ./bench
LLAM_BENCH_ONLY=io_echo ./bench
LLAM_BENCH_ONLY=poll_wake ./bench
LLAM_BENCH_ONLY=sleep_fanout ./bench
LLAM_BENCH_ONLY=opaque_block ./bench
```

## Scale Workload Size

```sh
LLAM_BENCH_ROUNDS=31 LLAM_BENCH_WARMUP_ROUNDS=5 ./bench
LLAM_BENCH_SPAWN_TASKS=512 ./bench
LLAM_BENCH_CHANNEL_MESSAGES=4096 ./bench
LLAM_BENCH_IO_MESSAGES=512 ./bench
LLAM_BENCH_POLL_EVENTS=512 ./bench
```

## Runtime Comparisons

Go comparison:

```sh
go run scripts/bench_go_compare.go
```

LLAM, Go, and Tokio:

```sh
python3 scripts/bench_runtime_compare.py --runtime all --isolate-cases
```

Release-quality comparison numbers should use isolated case execution:

```sh
python3 scripts/bench_runtime_compare.py \
  --runtime all \
  --cases spawn_join,select_recv_ready,poll_wake \
  --isolate-cases
```

Magic2's private CPU-policy experiment is separate from the general runtime
table. Build it explicitly and compare the identical manual, direct, and
selective-auto paths with paired samples:

```sh
make -j4 bench_magic2_cpu
python3 scripts/bench_magic2_cpu.py --samples 9 --warmup 2 \
  --output /tmp/llam-magic2-cpu-comparison.json
```

It is a path-cost and admission-policy measurement, not a claim that Magic2
accelerates arbitrary task functions.

The scheduled `Runtime Benchmarks` workflow uploads CSV and PNG artifacts for
Linux, macOS, and Windows lanes.

For a deeper POSIX diagnostic run, use the isolated multi-axis runner:

```sh
python3 scripts/bench_deep_compare.py
```

It builds the LLAM, Go, and Tokio binaries once, then runs fresh processes for
workload scaling, Go/Tokio worker-width sweeps, short and long timer fanout,
and LLAM direct-handoff/queue-policy ablations. It writes every raw process
sample, scenario medians, CPU-time diagnostics, and a Markdown report under
`object/bench_deep/`. Use `--quick` to verify the harness before a full run.
The script uses POSIX child-resource accounting and is not a Windows benchmark
entry point.

## CPU-Spike Preemption Comparison

Run the POSIX CPU-spike diagnostic against Go's asynchronous preemption:

```sh
make cpu-spike-compare
```

For a shorter local smoke run:

```sh
python3 scripts/cpu_spike_compare.py \
  --rounds 3 \
  --spike-ms 100 \
  --settle-ms 75
```

The LLAM workload first parks timer and nonblocking socket readers, then starts
exactly one pinned busy-loop task per runtime-reported active worker. The loops
contain no `LLAM_PREEMPT_POLL`, safepoint, yield, wait, or I/O call. An
unmanaged OS thread fires the socket event and injects a latency-critical
control task while all workers are occupied. The Go workload receives the same
worker count as `GOMAXPROCS`, uses equivalent busy goroutines without
`runtime.Gosched`, and runs with `GODEBUG=asyncpreemptoff=0`. The comparison
driver also fixes LLAM to the release-fast profile with automatic preemption
and a 1 ms requested quantum.

The table and `object/cpu_spike/cpu_spike_compare.csv` report timer deadline
lateness, socket-completion dispatch latency, and scheduled control-trigger to
task-execution latency as p50, p99, and max values. Percentiles use the
nearest-rank estimator; with fewer than 100 rounds, p99 normally equals max.
This is a diagnostic comparison, not an absolute CI latency gate; CPU topology,
frequency policy, and host contention can materially change the values.

## Current-Source Managed Profile Validation

A saved timing is evidence for its recorded source and binary hashes. It is not
current-source qualification after an input changes. Keep prior reports intact,
rebuild the production benchmark/profile image, then collect a fresh matched run:

```sh
cmake --build build-managed --parallel 4 --target bench_managed_preempt
PYTHONDONTWRITEBYTECODE=1 python3 experiments/async_compare/run_managed.py \
  --binary build-managed/bench_managed_preempt \
  --image build-managed/managed-bench-workload.dylib \
  --output .artifacts/managed-current-source \
  --samples 30 --iterations 100000000 --quantum-ns 0
```

The example image suffix is for macOS; use the native image produced by the
configured build. This managed landing benchmark currently requires the shared
native adapter; a build on a different backend is not performance qualification.

Run without concurrent builds, tests, task VMs or other benchmark processes.
The runner freezes one executable/image pair for every policy, records all
production/build/compiler/workload and qualification-source inputs (including
assembler `.inc` files), and checks source/artifact identity again after the run.
Changed, added or removed inputs invalidate the current-source claim and make
the command fail, even if its timing thresholds passed. Raw process samples,
commands, requested/effective quantum, variability and all failures remain in
the report.

The six cases, one portable P, alternating policy order, one warmup and 30
measured samples per case/policy match the local acceptance matrix. CPU/yield
use eight CPU tasks; timer/I/O use one control task, with two CPU tasks added
in the hog variants.
Zero requested quantum preserves the constructor default; the original 5%
normal-throughput and CPU p99/p999 landing gates remain unchanged. These are local
host/profile results. Public performance tables still require scheduled artifacts
and wider platform evidence.

## Guardrails

`scripts/bench_guard.py` is a catastrophic-regression gate. Keep it
conservative. Public performance tables should come from scheduled benchmark
artifacts, not local one-off runs.
