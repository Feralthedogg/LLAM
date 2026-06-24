# CLI And Tools

Use this page when you need the command to run, the knobs it accepts, and the
output you should trust. For API-level runtime knobs, use
[Options](options.md). For deployment environment variables, use
[Environment Variables](environment.md).

## Tool Map

| Tool | Use it for | First command |
| --- | --- | --- |
| `demo` | Small scheduler demo. | `./demo` |
| `stress` | Runtime and synchronization stress cases. | `./stress` |
| `bench` | Local benchmark cases. | `./bench` |
| `server` | Best-effort POSIX chat server example. | `./server 7777` |
| `server_lossless` | Lossless chat server build. | `./server_lossless 7777` |
| `server_flood` | Native throughput and accounting load. | `./server_flood --server ./server --clients 16 --duration 60` |
| `llam_broker` | Broker capability smoke and local transport hooks. | `./llam_broker --self-test` |
| `scripts/stress_server.py` | Exact chat fanout correctness. | `python3 scripts/stress_server.py --clients 16 --messages 8` |
| `scripts/stress_server_composite.py` | Correctness, flood, edge, and soak profiles. | `python3 scripts/stress_server_composite.py --quick` |
| `scripts/bench_runtime_compare.py` | LLAM, Go, and Tokio comparison artifacts. | `python3 scripts/bench_runtime_compare.py --runtime all --isolate-cases` |
| `scripts/bench_guard.py` | Conservative regression gate. | `python3 scripts/bench_guard.py` |
| `scripts/runtime_soak.py` | Time-bounded core runtime soak loop. | `python3 scripts/runtime_soak.py --duration 300` |

## Chat Server

Run the default best-effort server:

```sh
./server 7777
```

Run a lossless server:

```sh
./server --lossless 7777
./server_lossless 7777
LLAM_CHAT_LOSSLESS=1 ./server 7777
```

| Option | Meaning |
| --- | --- |
| `--public` | Bind on the public interface instead of the local default. |
| `--lossless` | Apply producer backpressure when a client outbox is full. |
| `--best-effort` | Drop per-client deliveries when a client outbox is full. |
| `-h`, `--help` | Print usage. |
| `port` | TCP port. Defaults to `7777`. |

Use best-effort mode for throughput ceilings. Use lossless mode when every
expected chat delivery must arrive.

## Native Server Flood

`server_flood` starts a target server, drives nonblocking TCP clients, and
reports both inbound message rate and observed broadcast delivery rate.

```sh
./server_flood --server ./server --server-best-effort \
  --clients 16 --duration 60 --message-bytes 8 --batch 64 --target-mps 0.30
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--server <path>` | `./server` | Server binary to launch. |
| `--host <ip>` | `127.0.0.1` | Bind/connect host. |
| `--clients <n>` | `4` | TCP clients. Valid range is `2..4096`. |
| `--duration <sec>` | `3.0` | Main send duration. Must be positive. |
| `--drain-sec <sec>` | `1.0` | Time to read late fanout after sending stops. |
| `--message-bytes <n>` | `8` | Message size. Valid range is `2..1024`. |
| `--batch <n>` | `128` | Per-client send batch. Valid range is `1..65536`. |
| `--target-mps <m>` | `0.0` | Optional target inbound messages/sec. `0` means unthrottled. |
| `--min-delivery-mps <m>` | `0.0` | Fail if observed delivery rate is below this value. |
| `--min-delivery-ratio <r>` | `0.0` | Fail if observed/expected broadcast ratio is below this value. |
| `--shutdown-timeout <sec>` | `30.0` | Server shutdown wait before forced cleanup. |
| `--server-lossless` | off | Start the target server with `--lossless`. |
| `--server-best-effort` | on | Start the target server with `--best-effort`. |
| `--allow-forced-stop` | off | Do not fail when cleanup must force-stop the server. |
| `--fail-on-forced-stop` | on | Fail when cleanup force-stops the server. |
| `--allow-missing-stats` | off | Do not fail if the server stats file is missing. |
| `--fail-on-missing-stats` | on | Fail if the server stats file is missing. |
| `-h`, `--help` | | Print usage. |

The same defaults can be set with `LLAM_SERVER_FLOOD_*` environment variables;
see [Environment Variables](environment.md#example-server-harness).

## Exact Server Stress

Use `stress_server.py` when correctness matters more than raw throughput. It
starts the server, connects clients, sends deterministic payloads, and waits
for the expected fanout.

```sh
python3 scripts/stress_server.py --clients 64 --messages 16 --payload-bytes 64
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--server <path>` | `./server` | Server binary to launch. |
| `--host <host>` | `127.0.0.1` | Bind/connect host. |
| `--clients <n>` | `16` | Client count. Must be at least `2`. |
| `--messages <n>` | `8` | Messages per client. |
| `--payload-bytes <n>` | `48` | Random payload bytes appended to each line. |
| `--timeout <sec>` | `20` | Wait limit for all expected broadcasts. |
| `--connect-spread-ms <ms>` | `5` | Delay between connects to avoid testing only backlog behavior. |
| `--seed <n>` | unset | Deterministic payload seed. |

## Composite Server Stress

Use `stress_server_composite.py` for the full chat-server operational gate. It
combines exact fanout checks, native flood, payload variation, churn, slow
receivers, half-close/reset behavior, RSS sampling, and fd sampling.

```sh
python3 scripts/stress_server_composite.py --quick
python3 scripts/stress_server_composite.py --soak-hour
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--server <path>` | `./server` | Server binary for correctness and edge phases. |
| `--server-flood <path>` | `./server_flood` | Native flood binary. |
| `--host <host>` | `127.0.0.1` | Bind/connect host. |
| `--correctness-matrix <cases>` | `8:8:16,16:4:64,32:2:1024` | Comma-separated `clients:messages:payload_bytes` cases. |
| `--correctness-timeout <sec>` | `30` | Per-correctness-case timeout. |
| `--flood-duration <sec>` | `60` | Main native flood duration. |
| `--flood-min-delivery-ratio <r>` | `0.0` | Minimum observed/expected broadcast ratio for flood phases. |
| `--allow-flood-forced-stop` | off | Allow native flood cleanup to force-stop the server. |
| `--payload-flood-duration <sec>` | `10` | Duration for payload-size flood phases. |
| `--edge-duration <sec>` | `60` | Edge/churn phase duration. |
| `--edge-clients <n>` | `24` | Clients used by edge phases. |
| `--churn-threads <n>` | `4` | Concurrent churn workers. |
| `--slow-threads <n>` | `4` | Slow receiver workers. |
| `--resource-interval <sec>` | `1.0` | RSS/fd sampling interval. |
| `--shutdown-timeout <sec>` | `30` | Server shutdown timeout. |
| `--command-timeout-padding <sec>` | `30` | Extra timeout before process-tree cleanup. |
| `--max-rss-mb <n>` | `2048` | Fail above this resident-set size. |
| `--max-fds <n>` | `4096` | Fail above this file descriptor count. |
| `--max-unexpected-client-errors <n>` | `0` | Allowed unexpected edge client errors; `-1` disables the check. |
| `--seed <n>` | unset | Deterministic edge payload/churn seed. |
| `--skip-correctness` | off | Skip exact fanout checks. |
| `--skip-flood` | off | Skip native flood phases. |
| `--skip-edge` | off | Skip edge/churn phases. |
| `--quick` | off | Short smoke profile. Mutually exclusive with `--soak-hour`. |
| `--soak-hour` | off | Long one-hour profile. Mutually exclusive with `--quick`. |

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
| `LLAM_BENCH_SELECT_OPS` | Select workload size. |
| `LLAM_BENCH_IO_MESSAGES` | I/O echo workload size. |
| `LLAM_BENCH_POLL_EVENTS` | Poll wake workload size. |
| `LLAM_BENCH_SLEEP_TASKS` | Sleep fanout workload size. |
| `LLAM_BENCH_OPAQUE_SCOPES` | Opaque blocking workload size. |

## Benchmark Scripts

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
| `--cases <cases>` | all | Comma-separated, whitespace-separated, or repeated cases. |
| `--isolate-cases` | off | Run each case in a fresh process. Use for release-quality numbers. |
| `--samples <n>` | `3` | Process-level samples per runtime. |
| `--sample-policy <mode>` | `median` | Row selection policy when multiple samples are collected. |
| `--spread-warning-ratio <r>` | `1.50` | Warn when max/min sample spread exceeds this ratio; `0` disables. |

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
| `--select-ops <n>` | `512` | Select guard workload. |
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

## Runtime Soak

`runtime_soak.py` repeatedly runs focused runtime tests for a bounded duration.

```sh
python3 scripts/runtime_soak.py --duration 300 --timeout 60
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--duration <sec>` | `300` | Total soak duration. |
| `--timeout <sec>` | `180` | Per-command timeout. |
| `--seed <n>` | fixed LLAM seed | Runtime fuzz seed. |
| `--fuzz-scenarios <n>` | `128` | Single-runtime fuzz scenarios. |
| `--multi-fuzz-scenarios <n>` | `64` | Multi-runtime fuzz scenarios. |
| `--runtime-fuzz <path>` | `./test_runtime_fuzz` | Runtime fuzz test binary. |
| `--multi-runtime-core <path>` | `./test_multi_runtime_core` | Multi-runtime core test binary. |
| `--runtime-stress <path>` | `./test_runtime_stress` | Runtime stress test binary. |
| `--runtime-shutdown <path>` | `./test_runtime_shutdown_internal` | Shutdown test binary. |
| `--io-buffers <path>` | `./test_io_buffers` | I/O buffer test binary. |

## Broker

`llam_broker` is a broker/capability control-plane smoke hook. It is useful for
testing local transport setup and capability validation, not as a production
broker daemon.

| Command | Meaning |
| --- | --- |
| `./llam_broker --self-test` | Run in-process broker capability smoke. |
| `./llam_broker --serve <name>` | Serve local transport forever. |
| `./llam_broker --serve-once <name>` | Serve one local transport connection. |
| `./llam_broker --serve-n <name> <connections>` | Serve a bounded number of connections. |
| `./llam_broker --client-self-test <name>` | Run a client self-test against a local transport name. |
| `./llam_broker --help` | Print usage. |

## Verification Scripts

| Script | Platform | Use |
| --- | --- | --- |
| `scripts/verify_linux.sh` | Linux | Linux local verification gate. |
| `scripts/docker_verify_linux.sh` | Linux host with Docker | Linux verification inside the configured image. |
| `scripts/verify_darwin.sh` | macOS | Darwin local verification gate. |
| `scripts/verify_windows.ps1` | Windows / WSL | Default WSL-oriented verification. |
| `scripts/verify_windows.ps1 -Native` | Windows | Native Windows CMake and CTest gate. |

## Release Scripts

Release packaging is intentionally strict: scripts reject unsafe paths,
unexpected links, special files, and archive layout mismatches before producing
or installing artifacts.

| Command | Use |
| --- | --- |
| `./scripts/package_release.sh [target]` | Package the current POSIX host target. Target defaults from `uname`. |
| `.\scripts\package_release_windows.ps1` | Package the native Windows target. |
| `./scripts/install.sh --prefix "$HOME/.local"` | Install a release archive on POSIX. |
| `.\scripts\install.ps1 -Prefix "$env:LOCALAPPDATA\LLAM"` | Install a release archive on Windows. |

See [Options](options.md#install-script-options) for install parameters.
