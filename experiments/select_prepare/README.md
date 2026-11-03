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

# Large channel select preparation

The production change is confined to collection of the distinct channel lock
set in `src/core/sync/channel_select.c`. The public select dispatch, wait,
cancellation and cleanup body is unchanged. Ready probes are unchanged.
The original collector is retained for eight or fewer operations, preceded only
by the dispatch to the large collector for larger inputs.

## Algorithm and lifetime

For larger inputs, the existing batch resolver validates and pins all operands
in original operation order under the channel registry lock. It already serves
the large ready-probe path. It checks runtime ownership, stale handles and pin
limits, and releases the acquired prefix on failure.

A bounded distinct-set probe handles up to 64 distinct channels. For these
sets, occurrence counts allow duplicate pins to be released without sorting all
operands; the small distinct set uses insertion sort. If a 65th channel is found,
no pins have been released yet: the original resolved pointers are copied into
the separate lock array, sorted with iterative in-place heapsort and compacted
in one pass. Duplicate pins are released exactly once, retaining one pin per
unique channel until the existing cleanup path finishes.

The operation-to-channel array remains in original order. Lock ordering uses
`uintptr_t` address order, matching the existing small collector. This preserves
operation-index mapping and a canonical order when overlapping selectors name
the same channels in different orders. The existing ready/cancel/close winner
protocol, wait-node publication and atomic memory ordering are not modified.

Worst-case large-input preparation is O(n log n), with a fixed-size prefix probe.
When at most 64 distinct channels are repeated, preparation remains O(n) for that
bounded distinct count. There are no additional heap allocations. The helper
uses 64 `size_t` occurrence counters (512 bytes on the measured 64-bit target),
in addition to the pre-existing arrays. Compilers may inline the helper; this is
not a claim about exact machine stack-frame size.

## Native validation

```sh
make -j4 CC=clang test_large_select test_runtime_select_edges \
  test_runtime_api_edges test_sync_primitives
./test_large_select
./test_runtime_select_edges
./test_runtime_api_edges
./test_sync_primitives
```

`test_large_select` covers 27 configurations spanning 2, 8, 9, 64, 65, 512 and
4,096 operations, with distinct and repeated-channel sets around both dispatch
boundaries. Two managed selectors use opposite operation orders. Native host
threads race cancellation and close both during setup and after both selectors
park. Forced close-only/cancel-only cases establish each expected result.

There are 270 two-selector scenarios plus 14 stale-tail/timeout cases. The test
checks exactly one terminal result per selector, original operation mapping,
untouched losing result slots, EBUSY while parked, and successful channel/token
destruction after all pins and waiters drain. A stale tail must not consume a
ready value from an earlier operand. The test is registered with Make,
CMake/CTest and the existing `make test-asan` suite. Its native pthread race
harness skips Windows; the production algorithm remains portable C11.

These tests exercise actual runtime code and OS threads, not all possible C11
executions. No full weak-memory proof or TSan result is claimed.

## Measurement

Build the identical benchmark source against the actual pre-change runtime and
the candidate. Preserve the pre-change executable before rebuilding. Match the
working-tree changes, compiler and flags; a clean-HEAD baseline is not equivalent
in an already modified checkout. The retained patch and hashes identify the
change used for the recorded comparison.

```sh
make -j4 CC=clang bench_select_prepare
python3 -B scripts/bench_select_prepare.py --baseline /path/to/frozen/baseline-bench \
  --samples 7 --warmup 1 --output research/select-new-run.json
```

The driver alternates fresh baseline/candidate processes and refuses to overwrite
results. Explicit warmup calls are excluded inside each process, and one process
pair per case is discarded before the seven measured pairs. All observed
nanosecond values and counts are retained as exact histograms. Percentiles use
nearest-rank over individual select calls, not over process durations.

- `ready`: fill a chosen channel, then select. Throughput includes buffer fill,
  select and result validation; p99 measures select entry-to-return. This
  exercises the unchanged ready path.
- `park`: the sender waits until the selector is actually PARKED. Every measured
  call therefore exercises collection, wait-node allocation/publication,
  wakeup and cleanup. Recorded park counts must cover every measured call.
- Repeated-channel cases use 512 or 4,096 operations naming only 1, 8, 9 or 64
  channels. They guard against a sort-all regression on low-cardinality sets.

Every call validates the payload and original selected index. Runs use one
processor, RELEASE_FAST, the lock-free normal queue, and identical default
preemption/handoff settings. Inherited `LLAM_*` overrides are removed. Setup,
channel creation, task spawning/joining and teardown are outside timing.

A longer small-select confirmation can be run separately:

```sh
python3 -B scripts/bench_select_prepare.py --baseline /path/to/frozen/baseline-bench \
  --counts 1 2 4 8 --samples 7 --warmup 1 --skip-duplicates --iteration-scale 4 \
  --output research/select-small-confirmation.json
```

These are local closed-loop single-worker measurements. Small latencies are
quantized by the host clock. Mixed small/large traffic across many workers and
cross-platform performance require separate measurement. The small-path result
means no material regression was detected in these cases, not identical timing
for all inputs or host conditions.

See [the recorded results](../../research/select-prepare-2026-10-27/REPORT.md)
for complete channel-count tables, the rejected exploratory case, raw samples,
validation limitations and cleanup status.
