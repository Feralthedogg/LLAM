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

The scheduled `Runtime Benchmarks` workflow uploads CSV and PNG artifacts for
Linux, macOS, and Windows lanes.

## Guardrails

`scripts/bench_guard.py` is a catastrophic-regression gate. Keep it
conservative. Public performance tables should come from scheduled benchmark
artifacts, not local one-off runs.
