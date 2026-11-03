<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: Apache-2.0
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
See LICENSES/Apache-2.0.txt.
-->


# Native async enabled LLAM compared with Go and Tokio

2026-10-27. Apple M4 / macOS 27.0.1 arm64. Go 1.25.6, Rust 1.95.0, Tokio 1.52.2, Apple Clang 21.

The current qualified native async profile is enabled for LLAM, with genuine capture/restore verified before every timed process. **Only the audited assembly leaf is certified. The timed ordinary C workloads recorded zero native captures.** The numbers therefore compare the standard workloads under the enabled restricted profile; they do not establish asynchronous interruption performance for arbitrary C code or a CPU-spike workload.

LLAM uses deterministic one-primary scheduling, RELEASE_FAST, a 1 ms requested quantum, full SIMD state, no lock-free normal queue and no dynamic workers. These constraints come from the private Darwin async integration. Go uses GOMAXPROCS=1 and GODEBUG=asyncpreemptoff=0. Tokio uses its existing multi-thread builder with one worker and a separate block_on caller context, so the configured width does not imply identical OS-thread topology.

## Throughput

Each case uses five fresh process samples, 127 measured rounds and eight warmup rounds. The existing comparator selects the middle throughput sample and retains that same sample’s latency values. Compilers/builds are excluded from timing; all three executable builds are reused.

| Case | LLAM ops/s | Go ops/s | Tokio ops/s | LLAM / Go | LLAM / Tokio |
| --- | ---: | ---: | ---: | ---: | ---: |
| spawn_join | 409,196 | 5,745,255 | 1,334,984 | 0.07x | 0.31x |
| channel_pingpong | 816,954 | 6,740,701 | 204,367 | 0.12x | 4.00x |
| select_recv_ready | 29,810,290 | 26,930,984 | 41,669,260 | 1.11x | 0.72x |
| select_park_wake | 556,450 | 6,177,151 | 77,644 | 0.09x | 7.17x |
| io_echo | 58,651 | 162,162 | 162,570 | 0.36x | 0.36x |
| poll_wake | 283,165 | 203,444 | 794,073 | 1.39x | 0.36x |
| sleep_fanout | 28,561 | 30,759 | 30,799 | 0.93x | 0.93x |

The local outcomes vary by workload. Go has higher measured spawn/join, channel and parked-select throughput. LLAM exceeds Go in ready-select and poll-wake here. Tokio exceeds LLAM in spawn/join, ready-select and I/O/poll cases, while LLAM exceeds this Tokio harness in channel ping-pong and parked select. No global winner or cross-platform claim follows from this matrix.

## Batch p99

These are per-round batch durations in microseconds, not per-operation or wake-to-dispatch latency. The work sizes are 512 spawn/join tasks, 4,096 channel/select iterations, 512 I/O/poll operations and 1,024 sleeping tasks per round. The select workloads have two channel operands; SELECT_OPS=4096 is the iteration count, so this does not repeat the prior 4,096-channel select experiment.

| Case | LLAM p99 us | Go p99 us | Tokio p99 us |
| --- | ---: | ---: | ---: |
| spawn_join | 1,389.71 | 103.50 | 430.62 |
| channel_pingpong | 5,462.54 | 654.08 | 23,598.50 |
| select_recv_ready | 149.46 | 166.00 | 113.96 |
| select_park_wake | 7,955.96 | 718.88 | 55,562.62 |
| io_echo | 9,176.12 | 4,788.67 | 3,579.92 |
| poll_wake | 2,042.92 | 2,885.08 | 707.71 |
| sleep_fanout | 37,122.88 | 34,901.00 | 35,338.21 |

[Throughput and latency graph](results/runtime_compare.png)

## Activation and exclusions

The native regression initially completed 66 captures and 66 restores with exact arithmetic/GPR/NEON/errno/mask checks. Per-process preparation uses the same audited no-call leaf with two finite peer tasks, requires positive balanced native captures/restores, validates exact results and snapshots, and checks zero execution debt and binding operations. Across all 40 LLAM process records (including five diagnostic timeout runs), qualification recorded 2,308 captures/restores. The common table uses 35 verified LLAM records. Every timed workload recorded zero captures/restores and ended with native mode enabled.

The common matrix contains 105 successful processes: seven cases × three runtimes × five samples. An initial all-in-one LLAM smoke returned EPERM when reaching io_echo; isolated I/O smoke and every measured isolated I/O sample succeeded. The supported procedure explicitly isolates cases, as recommended by the repository benchmark guide. The same-process issue remains unresolved by this benchmark task.

`opaque_block` fails at opaque enter under this native profile and has no LLAM score. `select_timeout` is a semantics diagnostic in the existing deep-comparison guidance: LLAM/Go immediate polling and Tokio zero-duration async timeout are different. Tokio’s first requested diagnostic process exceeded the existing 180 s limit and was killed by the process-tree timeout handler. The first 100 successful outputs were reused and only the remaining 15 Tokio I/O/timer processes were collected afterward. Five LLAM and five Go timeout diagnostic samples remain available, but no three-way timeout score is fabricated.

## Files and reproduction

- [Build and execution instructions](../../experiments/async_compare/README.md)
- [Summary CSV](results/runtime_compare.csv)
- [All 105 common process metric rows](results/runtime_compare_samples.csv)
- [Ten diagnostic timeout rows](results/diagnostic_timeout_samples.csv)
- [Full child stdout/stderr](results/raw/)
- [Environment, executable hashes and activation proofs](results/metadata.json)
- [Verification, exact versions and source hashes](verification.json)
- [Task-only reversible patch](async-compare.patch)

The benchmark/parser regression tests pass, including prebuilt-executable selection and no-build guards. The ordinary benchmark still runs normally, and a request for async on the unqualified ordinary target fails instead of silently producing an off-mode result. CMake native-async target registration configures successfully; native execution uses Make. No production runtime source, API, preceding watch index or select optimization was modified in this task.

The run used existing Go/Rust toolchains and cached Cargo dependencies with --locked --offline. Existing dependency caches and earlier measurements are preserved. This task does not claim a full runtime-suite, sanitizer-suite or non-Darwin pass.


## Cleanup

Removed only the task-owned async runtime objects, temporary CMake setup/checkpoints/logs, Go benchmark executable and new comparison executable. No deletion failed. Existing Cargo/Go caches, the ordinary benchmark and pre-existing native preemption test executable were preserved. Source hashes confirm that the prior watch index and large-select optimizations remain unchanged. Raw process outputs, CSVs, graph, initial timeout log and task patch remain.
