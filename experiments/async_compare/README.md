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
See LICENSE.
-->

# Qualified native async runtime comparison

## Managed C cost and latency gate

`bench_managed_preempt` measures actual LLVM-qualified C through the managed
constructor. It is separate from the older caller-certified-range comparison
below. Build with LLVM 23.1.2 managed tooling enabled:

```sh
cmake --build build-managed --target bench_managed_preempt -j4
python3 -B experiments/async_compare/run_managed.py \
  --binary build-managed/bench_managed_preempt \
  --image build-managed/managed-bench-workload.dylib \
  --samples 30 --iterations 100000000 --quantum-ns 0 \
  --output research/managed-cost-new-run
```

Use `.so` on ELF platforms. The current benchmark adapter uses the shared
native protocol. The constructor's native default is selected by quantum `0`;
an explicit interval can be compared in a separate output directory. Existing
output directories are rejected. Each case alternates policy order, records a
warmup, retains all raw samples and checks exact results and complete native
drain. CPU, yield, timer and socket I/O cases, including probes competing with
CPU hogs, use matching worker counts, compiler output and build settings. The
I/O source is a native helper sending numbered bytes at 1 ms intervals through
a nonblocking socket pair; qualified C parks in `llam_read()` and records exact
delivery and dispatch lag. The report retains idle/hog p99 and p99.9 differences.
The runner keeps one fixed copy of the executable and workload in its output
directory and records their hashes, source hashes and sample timestamps.
`--cases cpu yield` selects a focused cost run; unmeasured gates remain null.

The measurement runtime uses production objects with test hooks disabled.
Only bounded native event recording is enabled: timestamps are taken in normal
M context, with no clock call in a signal handler or while a native target is
suspended. The event prefix, its sample count, refused capture requests,
configured/effective quantum, timer samples and storage/RSS are recorded.
Percentiles from a small prefix are observations, not a tail guarantee. Keep
the raw sample count when evaluating p99.9.

The initial normal-workload gate remains at most 5% throughput loss against
the cooperative policy. Accepted-request-to-landing observations are compared
with 2Q at p99 and 4Q at p99.9; refusal/unsafe deferral is reported separately.
The runner returns failure if the throughput or CPU landing gates fail. Run
isolated host measurements after VMs, builds and correctness tests finish.

## Earlier caller-certified-range comparison

This benchmark opts into LLAM's current Darwin arm64 native asynchronous context
profile. It enables the backend before spawning work, verifies real capture and
restore using the existing audited no-call assembly leaf, and then runs the
existing comparison workloads with that runtime mode active.

**The ordinary C benchmark bodies are outside the certified range. Their measured
native capture count is zero.** This measures the standard workloads under the
enabled profile and its configuration constraints. It is not qualification of
arbitrary C instructions for asynchronous interruption or a CPU-spike latency test.

## Build and run

```sh
make -j4 CC=clang LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1 \
  OBJDIR=object/async-compare bench_async_compare
go build -o object/async-compare/go_compare scripts/bench_go_compare.go
cargo build --release --locked --offline \
  --manifest-path scripts/bench_tokio_compare/Cargo.toml --bin bench_tokio_compare
python3 -B experiments/async_compare/run_compare.py \
  --llam ./bench_async_compare --go object/async-compare/go_compare \
  --tokio scripts/bench_tokio_compare/target/release/bench_tokio_compare \
  --output research/async-compare-new-run
```

The offline Cargo command assumes the existing dependencies are cached. No new
crate or toolchain is required. Select a new output directory for every run.
The runner invokes the existing `scripts/bench_runtime_compare.py` parser,
workloads, sample policy and plotting code with prebuilt executable overrides.
Its recording wrapper executes the real process and returns that result
unchanged; it retains stdout/stderr for activation and worker-count checks.

CMake also exposes `bench_async_compare` when configured for Darwin arm64 with
`LLAM_EXPERIMENTAL_ASYNC_PREEMPT=ON`. Normal `bench` defaults are retained. An
async request on the unqualified normal target fails explicitly instead of
silently producing an off-mode result.

## Qualification and timing boundaries

The supported profile requires deterministic one-primary scheduling, ordinary
locked normal queues, full SIMD preservation and no dynamic-worker or worker-ring
mode. The comparison uses RELEASE_FAST and a requested 1 ms preemption quantum.
The private qualification entry acknowledges fail-fast behavior only in this
controlled standalone benchmark process. The registered immutable executable
range is the existing `async_darwin_leaf_begin` to `async_darwin_leaf_end` interval.
No runtime, libc or arbitrary C function range is certified.

Before timing, two finite no-yield leaves generate genuine peer pressure. The
benchmark checks their exact arithmetic, GPR/NEON/FP snapshot and errno, requires
positive balanced capture/restore counts, and drains execution debt and native
binding operations. After the workload, a second check requires the mode still
to be enabled and records qualification versus workload capture counts.
Qualification execution time is excluded from the benchmark round samples.
Aggregate diagnostic context-switch counts include qualification activity.

The runner forces Go asynchronous preemption on with
`GODEBUG=asyncpreemptoff=0` and `GOMAXPROCS=1`. Tokio uses its existing multi-thread
builder with one worker. Its block_on caller is an additional executor context;
matching these settings does not imply identical OS-thread topology.

## Sample interpretation and exclusions

Each supported case runs in fresh processes, with five process samples,
127 measured rounds and eight warmup rounds. The existing median policy selects
the middle throughput sample and retains that same sample's p50/p99. Latencies
are **per-round workload-batch durations**, not individual operation or wake
latencies. Work sizes are the existing comparison defaults: 512 spawn/join tasks,
4,096 channel/select operations, 512 I/O/poll operations and 1,024 sleepers.
Select cases use two channel operands; SELECT_OPS is an iteration count, not a
wide-select channel count.

The common table excludes:

- `opaque_block`: native async profile rejects the benchmark's opaque enter.
- `select_timeout`: LLAM/Go immediate polling and Tokio zero-duration async
  timeout have different semantics. A first Tokio attempt exceeded the 180 s
  limit. This is already excluded from general speed conclusions in the deep
  comparison script.

Case isolation is required for this comparison. An initial multi-case LLAM smoke
failed at io_echo with EPERM, while all isolated measured I/O samples passed;
that same-process limitation was not repaired by this benchmark task.

See the recorded comparison (`research/async-runtime-compare-2026-10-27/REPORT.md`, local output)
for exact versions, results, p99 values, activation proofs, raw records and limits.
