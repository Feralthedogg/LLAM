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


# I/O watch indexing results

2026-10-27. Apple M4, macOS 27.0.1 arm64, Apple Clang 21, C11 `-O2 -g -fno-omit-frame-pointer`, one processor and one native I/O node.

Poll, accept and receive lookup now use a node-owned hash index with the original identity and lifecycle checks. Large lookup sets show a clear gain. Real socket throughput improves at large connection counts, while end-to-end p99 changes remain mixed. The focused index/lifecycle checks pass. A watchdog failure reproduces before this change, so whole-suite and sanitizer completion are not claimed.

## Implementation and preserved contracts

The ownership lists and `watch_lock` remain authoritative. An embedded entry indexes each watch by fd hash; lookup still checks event masks, device/inode, accept local address, accepts-waiters, retired and destroy-pending state. Growth relinks entries under the lock without moving watches and preserves same-fd precedence. Actual destruction removes index membership before freeing a watch. Shutdown drops bucket arrays after workers/control operations drain. No public header, ABI contract or production atomic ordering changed.

First bucket-allocation failure falls back to the complete ownership list until it empties. Growth failure retains the complete old index. New watches never become absent from an allegedly complete index. The original success/error behavior and errno are preserved. Close, rehome traversal and final list unlink remain linear; the optimization does not remove fstat or promise constant-time whole I/O operations.

Measured private sizes (bytes): poll 64 → 88, accept 216 → 240, recv 88 → 112, node 832 → 896. Bucket pointers cost 8 bytes per slot. At 4,096 poll watches, extra entries plus buckets are 128 KiB, in addition to node metadata. Bucket capacity retains its peak until that table empties, then frees.

## Matched measurements

The baseline executable was captured from the actual pre-index working tree before modifying runtime sources. It is not a clean-HEAD baseline; existing user changes, including the earlier handoff experiment, were retained. Both executables use the same benchmark logic and compiler settings; the benchmark received formatting only after the baseline capture. Each case alternates baseline/indexed fresh processes, uses one warmup pair and seven measured pairs, validates work and stores exact per-operation latency histograms.

### Lookup cost

Get-or-create lookup on pre-existing real socket watch objects, including identity fstat and watch_lock, 32,768 timed operations. This is a lookup microbenchmark.

| Connections | Baseline lookup/s | Indexed lookup/s | Paired throughput change | Baseline p99 ns | Indexed p99 ns | Paired p99 change |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 4,661,747 | 4,543,932 | +6.19% | 292 | 292 | -0.30% |
| 16 | 4,613,559 | 4,539,526 | +0.22% | 292 | 291 | +0.00% |
| 128 | 3,438,375 | 4,435,901 | +31.68% | 458 | 292 | -30.05% |
| 1,024 | 1,089,801 | 3,518,601 | +229.44% | 2,750 | 583 | -78.47% |
| 4,096 | 234,056 | 3,333,200 | +1224.94% | 11,167 | 583 | -95.16% |

### Real socket I/O confirmation

The first 8,192-request I/O sweep had substantial small-case variability. A separate confirmation increased each process to 32,768 requests without changing the candidate. Both favorable and unfavorable original samples remain in `comparison.json`.

Each connection has a managed poll/read/echo task and a real AF_UNIX socketpair. A native client waits for every initial poll waiter to attach, then sends sequential requests across a descriptor permutation. Throughput counts exact replies; p99 is client write-to-reply RTT. Setup, spawn, join and teardown are outside the timed region. This does not measure TCP, open-loop load, multi-node scaling or connection churn.

| Connections | Baseline echo/s | Indexed echo/s | Paired throughput change | Baseline p99 us | Indexed p99 us | Paired p99 change |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 109,295 | 114,060 | +5.07% | 27.792 | 26.334 | -8.43% |
| 16 | 45,995 | 46,186 | -0.85% | 28.958 | 26.750 | -9.06% |
| 128 | 44,802 | 45,737 | +0.62% | 28.334 | 35.000 | +22.03% |
| 1,024 | 42,261 | 43,764 | +3.09% | 36.542 | 31.792 | -10.64% |
| 4,096 | 32,668 | 37,918 | +14.00% | 60.000 | 61.458 | +2.80% |

Absolute columns are medians of process measurements. Percentage changes are medians of paired ratios, so they need not equal ratios of the absolute medians. The data also retains ratio ranges. At 4,096 connections, all seven long-run throughput ratios exceeded 1.10; its p99 ratio range was approximately 0.675–1.320. The large-set throughput gain is supported by these local samples. A general I/O p99 improvement is not established, including the +22% paired p99 result at 128 connections.

## Correctness evidence

- New index regression: all three kinds, growth/collisions, first-allocation and resize failure, identity/event mismatch, non-list-order removal and zero-live cleanup pass.
- Close/migration/FD-reuse: two forced orderings plus 500 overlapping native pthread trials pass. A retained source pin spans close and dup2 reuse; late forwarding cannot use the fresh generation, and old-pin release leaves the new entry intact. This uses real production helpers with detached two-node backend fixtures, not exhaustive kernel callback schedules.
- `test_connect_io`, `test_io_buffers`, `test_runtime_io_dump`, `test_runtime_shutdown_internal`, `test_runtime_core`, `test_multi_runtime_core`, `test_runtime_api_edges`, `test_runtime_stress`, `test_runtime_fuzz`, `test_runtime_invariants`, `test_abi_contract`, `test_abi_compat` and `test_shared_load` pass. Together with the new test, 14 selected native processes pass.
- Production build, 131-symbol shared export audit and two-artifact production test-hook audit pass. CMake configuration and test registration succeed; native builds were checked with Make.
- ASan/UBSan: the new 502-case test plus API/core/runtime-regression/buffer/shutdown checks pass before the watchdog test stops the overall sanitizer suite. The log is retained.

## Existing blockers and untested scope

`test_watchdog_liveness_internal` returns 1 after `llam_runtime_run_handle` reports `EAGAIN`, in both the indexed ordinary build and a separate build restored from the captured pre-index source files. The ordinary failure is therefore reproduced independently of this index. The sanitizer suite stops at the same watchdog result; no sanitizer baseline comparison or full-suite pass is claimed. This task leaves the watchdog behavior unchanged.

The prior package/license gates are blocked reading the unchanged `docs/requirements.txt` macOS cloud placeholder (`compressed,dataless`). Repeating that unchanged blocking read was avoided. Scoped notices, Python syntax and changed-path whitespace were checked. Linux, BSD, Windows and TSan execution remain unverified; Windows IOCP lookup behavior was not modified.

## Reproduction and artifacts

- [Build commands, algorithm details and test limitations](../../experiments/watch_index/README.md)
- [First complete sweep and raw histograms](comparison.json)
- [Long I/O confirmation and raw histograms](io-confirmation.json)
- [Validation outputs, baseline reproduction, sizes and source hashes](verification.json)
- [Sanitizer log](asan.log.gz)
- [Only this task’s source and build-registration patch](watch-index.patch)

The JSON files retain all 240 process records and every observed nanosecond value/count. Whitespace was removed losslessly to reduce artifact size. Existing source modifications and previous research artifacts were preserved.


## Cleanup

Removed the task-owned baseline source/build copy, temporary configuration and logs, benchmark intermediate object, and two reproducible experiment executables. The temporary third_party symlink was unlinked without traversing its target. No deletion failed. Current production build caches/libraries, original dependencies, prior measurements and user edits were preserved. Final source, tests, exact raw histograms, sanitizer evidence and the reversible patch remain.
