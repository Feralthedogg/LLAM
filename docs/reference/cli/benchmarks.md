# Benchmark tools

Run the benchmark binary and the scripts that compare runtimes.

## Bench

Run every local benchmark case:

```sh
./bench
```

Run one case:

```sh
LLAM_BENCH_ONLY=spawn_join ./bench
LLAM_BENCH_ONLY=channel_pingpong ./bench
LLAM_BENCH_ONLY=io_echo ./bench
LLAM_BENCH_ONLY=poll_wake ./bench
```

Important benchmark environment variables:

| Variable | Meaning |
| --- | --- |
| `LLAM_BENCH_ONLY` | Run one benchmark case. |
| `LLAM_BENCH_ROUNDS` | Measured rounds. |
| `LLAM_BENCH_WARMUP_ROUNDS` | Warmup rounds. |
| `LLAM_BENCH_SPAWN_TASKS` | Spawn/join workload size. |
| `LLAM_BENCH_CHANNEL_MESSAGES` | Channel workload size. |
| `LLAM_BENCH_SELECT_OPS` | Select workload size, capped at `65536`. |
| `LLAM_BENCH_IO_MESSAGES` | I/O echo workload size. |
| `LLAM_BENCH_POLL_EVENTS` | Poll wake workload size. |
| `LLAM_BENCH_SLEEP_TASKS` | Sleep fanout workload size. |
| `LLAM_BENCH_OPAQUE_SCOPES` | Opaque blocking workload size. |

## Benchmark scripts

`bench_runtime_compare.py` creates comparison artifacts for LLAM, Go, and Tokio:

```sh
python3 scripts/bench_runtime_compare.py \
  --runtime all \
  --cases spawn_join,select_recv_ready,poll_wake \
  --isolate-cases
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--rounds <n>` | `31` | Measured rounds per benchmark process. |
| `--warmup <n>` | `5` | Warmup rounds. |
| `--timeout <sec>` | `180` | Per-command timeout. |
| `--out-dir <dir>` | `object/bench_compare` | CSV/PNG output directory. |
| `--runtime <name>` | `all` | `all`, `llam`, `go`, or `tokio`. |
| `--no-build` | off | Skip build steps. |
| `--llam-binary <path>` | none | Prebuilt LLAM benchmark binary. Requires `--no-build`. |
| `--go-binary <path>` | none | Prebuilt Go benchmark binary. Requires `--no-build`. |
| `--tokio-binary <path>` | none | Prebuilt Tokio benchmark binary. Requires `--no-build`. |
| `--cases <cases>` | all | Comma-separated, whitespace-separated, or repeated cases. |
| `--isolate-cases` | off | Run each case in a fresh process. Use for release-quality numbers. |
| `--samples <n>` | `3` | Process-level samples per runtime. |
| `--sample-policy <mode>` | `median` | Row selection policy when multiple samples are collected. |
| `--spread-warning-ratio <r>` | `1.50` | Warn when max/min sample spread exceeds this ratio; `0` disables. |

Windows builds use `CMAKE_GENERATOR` when set. The CI lanes select Visual Studio
17 2022 or 18 2026 to match the installed toolchain.

`cpu_spike_compare.py` fills every LLAM worker with a busy task that has no
preemption poll and compares timer, I/O, and scheduled control-trigger latency
with Go at the same `GOMAXPROCS` width. It uses nearest-rank percentiles, so p99
normally equals max when fewer than 100 rounds are requested:

```sh
python3 scripts/cpu_spike_compare.py --rounds 7 --spike-ms 200
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--rounds <n>` | `7` | Independent CPU-spike samples. |
| `--spike-ms <n>` | `200` | Duration of the fully occupied worker interval. |
| `--settle-ms <n>` | `100` | Time to park probes and start hogs before the trigger. |
| `--timeout <sec>` | `60` | Per-runtime process timeout. |
| `--out-dir <dir>` | `object/cpu_spike` | Go binary and comparison CSV directory. |
| `--llam-bin <path>` | `cpu_spike` | LLAM diagnostic executable. |
| `--go-bin <path>` | generated under `--out-dir` | Go diagnostic executable. |
| `--no-build` | off | Reuse both executables without rebuilding. |

`bench_guard.py` is the fast regression gate:

```sh
python3 scripts/bench_guard.py --cases spawn_join,channel_pingpong --min-ops spawn_join=100000
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--bench <path>` | `./bench` | Bench binary. |
| `--cases <cases>` | default guard cases | Comma-separated cases. |
| `--min-ops <case=value,...>` | `LLAM_BENCH_GUARD_MIN_OPS` | Minimum ops/sec thresholds. |
| `--rounds <n>` | `3` | Measured rounds. |
| `--warmup <n>` | `1` | Warmup rounds. |
| `--timeout <sec>` | `180` | Timeout for the bench process. |
| `--spawn-tasks <n>` | `128` | Spawn/join guard workload. |
| `--channel-messages <n>` | `1024` | Channel guard workload. |
| `--select-ops <n>` | `512` | Requested select workload, up to `65536` with the bundled bench. A different reported count fails the guard. |
| `--io-messages <n>` | `128` | I/O guard workload. |
| `--poll-events <n>` | `128` | Poll guard workload. |
| `--sleep-tasks <n>` | `256` | Sleep guard workload. |
| `--opaque-scopes <n>` | `16` | Opaque blocking guard workload. |
| `--out <path>` | unset | Optional JSON result path. |

`bench_matrix.py` runs LLAM bench across runtime profiles:

```sh
python3 scripts/bench_matrix.py --profiles baseline,spin,sqpoll --json-out object/bench_matrix.json
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--rounds <n>` | `7` | Sets `LLAM_BENCH_ROUNDS`. |
| `--timeout <sec>` | `60` | Per-profile timeout. |
| `--spin-ns <n>` | `50000` | Spin budget for the `spin` profile. |
| `--spin-iters <n>` | `10000` | Spin iteration cap for the `spin` profile. |
| `--profiles <list>` | built-in profile list | Comma-separated profile names. |
| `--json-out <path>` | unset | Optional raw JSON result path. |
| `--no-build` | off | Skip `make bench`. |
