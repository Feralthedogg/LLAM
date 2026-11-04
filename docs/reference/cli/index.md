# CLI tool map

Use this page when you need the command to run, the knobs it accepts, and the
output you should trust. For API-level runtime knobs, use
[Options](../options/runtime.md). For deployment environment variables, use
[Environment variables](../environment.md).

| Tool | Use it for | First command |
| --- | --- | --- |
| `demo` | Runnable examples of the public runtime API. | `./demo` |
| `stress` | Regression coverage for scheduling, sync, timeouts, I/O, and dynamic workers. | `./stress` |
| `bench` | Microbenchmarks for spawn/join, channels, channel select, I/O, poll, sleep fanout, and opaque blocking. | `./bench` |
| `server` | Minimal LLAM-backed TCP chat backend for local best-effort testing. | `./server 7777` |
| `server_lossless` | The same chat backend with lossless outbox backpressure enabled by default. | `./server_lossless 7777` |
| `server_flood` | Native nonblocking throughput flood driver for the chat server. | `./server_flood --server ./server --clients 16 --duration 60` |
| `llam_broker` | Internal broker-control self-test and local client/server smoke harness. | `./llam_broker --self-test` |
| `llam_preempt_metadata_inspect` | Validate and print linked `.llam.preempt` records. | `./llam_preempt_metadata_inspect --required preempt.bin` |
| `tools/llam-clang` | LLVM driver. It runs the [managed C toolchain](../../guides/managed-c.md) by default, and `--llam-preempt` selects the optional Linux x86-64 metadata pass. | `tools/llam-clang --llam-preempt=auto app.c -o app` |
| `scripts/stress_server.py` | Exact chat fanout correctness. | `python3 scripts/stress_server.py --clients 16 --messages 8` |
| `scripts/stress_server_composite.py` | Correctness, flood, edge, and soak profiles. | `python3 scripts/stress_server_composite.py --quick` |
| `scripts/bench_runtime_compare.py` | LLAM, Go, and Tokio comparison artifacts. | `python3 scripts/bench_runtime_compare.py --runtime all --isolate-cases` |
| `scripts/cpu_spike_compare.py` | Unpolled CPU saturation versus Go asynchronous preemption. | `make cpu-spike-compare` |
| `scripts/bench_guard.py` | Conservative hard-fail regression guard for scheduler, channel, select, I/O echo, and poll-wake throughput. | `python3 scripts/bench_guard.py` |
| `scripts/runtime_soak.py` | Time-bounded core runtime soak loop. | `python3 scripts/runtime_soak.py --duration 300` |
| `scripts/run_with_timeout.py` | CI-safe long-process runner that preserves partial logs, broadcasts POSIX dump signals to wrapped runtimes, and cleans up hung stress jobs plus descendants. | `python3 scripts/run_with_timeout.py --help` |

The runtime programs in this table are plain C, so they select the `*_compat`
constructors. The `test_*` binaries are listed in
[Testing strategy](../../internals/testing.md#test-binaries).
