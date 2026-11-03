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


# Large channel select preparation results

2026-10-27. Apple M4, macOS 27.0.1 arm64, Apple Clang 21, C11 `-O2 -g -fno-omit-frame-pointer`, one processor for performance measurements.

Large parked selects now use batched handle validation and bounded deduplication or in-place heapsort. The 4,096-distinct-channel case measured about 18.45 times baseline throughput with a 94.9% lower paired p99. Longer small-select measurements found paired median throughput changes from −1.9% to +1.6% and zero median p99 change for 1–8 operands. These are local observations, not universal latency guarantees.

## Change and preserved behavior

Only the production collector in `src/core/sync/channel_select.c` changes. For at most eight operations, the old collector is retained after the new large-input dispatch check. The public select dispatch/wait/cleanup body is byte-for-byte unchanged, as are ready probes. The original operation array and selected-index mapping are preserved. No public ABI or cancellation/publication atomic ordering changes.

The large collector uses the existing batch resolver, which validates all handles in operation order and rolls back acquired pins on error. A bounded probe handles up to 64 distinct channels with occurrence counts and sorts only that small lock set. A 65th distinct channel selects iterative heapsort of the separate pointer array followed by linear duplicate removal. Exactly one pin per distinct channel remains until the existing cleanup completes. Numeric address order matches the prior lock order. No extra heap allocation is introduced; 64 size_t counters add 512 bytes of logical scratch on this host. Compiler inlining can affect actual frame reservation.

An exploratory probe exposed a sort-all regression: 4,096 operations naming nine channels became about 20% slower with an eight-channel cutoff. Those six process summaries and the candidate source hash are retained in `duplicate9-probe.json`. The final bounded-set path removes that regression: the main comparison measured +9.9% throughput and −8.1% paired p99 for the same repeated-channel configuration.

## Large distinct channel results

Each case uses seven fresh measured process pairs with alternating order, discarded process warmups and untimed in-process warmup calls. Parked-case senders wait for the selector to become PARKED, and measured park counts cover every call, ensuring that the changed preparation path is exercised. Payloads, selected indices, checksums and work counts are checked.

| Channels | Baseline select/s | Prepared select/s | Paired throughput change | Baseline p99 us | Prepared p99 us | Paired p99 change |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 9 | 1,475,456 | 1,582,734 | +5.24% | 0.916 | 0.792 | -8.95% |
| 32 | 561,173 | 647,570 | +14.19% | 2.167 | 1.916 | -13.47% |
| 64 | 294,951 | 317,386 | +8.41% | 8.208 | 6.542 | -20.31% |
| 65 | 282,968 | 283,433 | +1.59% | 8.083 | 8.209 | +4.05% |
| 128 | 112,668 | 169,155 | +50.50% | 14.625 | 9.458 | -35.68% |
| 512 | 12,033 | 42,553 | +256.09% | 98.417 | 32.708 | -66.98% |
| 2,048 | 995 | 10,198 | +912.61% | 1107.667 | 116.042 | -89.41% |
| 4,096 | 264 | 4,918 | +1745.08% | 4548.584 | 233.000 | -94.90% |

Absolute columns are medians of process measurements; percentage changes are medians of paired ratios and need not equal ratios of the absolute columns. Throughput includes the exact select exchange and validation. p99 measures entry-to-return of each select, including preparation, scheduling/wakeup and cleanup. Setup, spawn/join and teardown are excluded. The data retain all per-process values and ratio ranges. At 4,096 channels, measured throughput ratios ranged from about 17.65 to 19.58.

## Small select confirmation

A second run lengthened each small ready case to 2,000,000 iterations and each small parked case to 524,288 iterations per process. The candidate and selection of cases were unchanged. All eight paired median p99 changes were zero; the host clock quantizes very short calls, so equal p99 does not prove identical instruction cost.

| Mode | Channels | Paired throughput change | Baseline p99 ns | Prepared p99 ns | Paired p99 change |
| --- | ---: | ---: | ---: | ---: | ---: |
| ready | 1 | -0.14% | 42 | 42 | +0.00% |
| ready | 2 | -1.87% | 42 | 42 | +0.00% |
| ready | 4 | +0.59% | 83 | 83 | +0.00% |
| ready | 8 | +1.58% | 84 | 84 | +0.00% |
| park | 1 | +1.06% | 417 | 417 | +0.00% |
| park | 2 | +0.21% | 417 | 417 | +0.00% |
| park | 4 | +0.08% | 500 | 500 | +0.00% |
| park | 8 | -0.34% | 666 | 625 | +0.00% |

The full main sweep also retains unchanged large-ready-path measurements and repeated-channel cases with 512/4,096 operations over 1, 8, 9 or 64 distinct channels. No claim is made that already-ready large selects became faster; their preparation path is bypassed. Mixed small/large traffic across many workers and other platforms were not measured.

## Correctness and gates

- `test_large_select`: 27 shapes across 2, 8, 9, 64, 65, 512 and 4,096 operations; 270 two-selector close/cancel scenarios with opposite operand orders and both setup/parked trigger phases; 14 stale-tail/timeout cases. It checks winner mapping, losing outputs, active-destroy EBUSY and complete pin/waiter/token cleanup. Passes normally and under ASan/UBSan.
- `test_runtime_select_edges`, `test_runtime_api_edges`, `test_sync_primitives`, `test_runtime_core`, `test_multi_runtime_core`, `test_runtime_fuzz`, `test_runtime_invariants`, `test_runtime_stress`, `test_abi_contract`, `test_abi_compat`, and `test_shared_load` pass. Including the new test, all 12 selected native process checks pass.
- Production build, 131-symbol shared export audit and two-artifact production test-hook audit pass. CMake configuration and test registration pass; native execution uses Make to avoid duplicate builds.
- The full `make test-asan` attempt passes the new test and proceeds through API/core/buffer/shutdown checks, then stops at the known watchdog EAGAIN failure. This same ordinary watchdog failure was independently reproduced before the earlier I/O-index change; see the linked prior evidence. No whole-sanitizer pass is claimed.

The unchanged `docs/requirements.txt` remains a macOS `compressed,dataless` placeholder, so the previously blocked whole-package/license reads were not repeated. New-file notices, Python syntax and changed-source whitespace were checked. Linux, BSD, Windows and TSan execution remain unverified. Stress, native races and sanitizer checks do not constitute a complete C11 proof.

## Reproduction and artifacts

The baseline is the actual pre-select-change working tree, including the preceding I/O watch index and all other user edits. Benchmark source and build flags match exactly between the two executables. Binary hashes and the baseline source manifest are retained.

- [Algorithm, build and benchmark commands](../../experiments/select_prepare/README.md)
- [Complete main sweep and exact latency histograms](comparison.json)
- [Long small-select confirmation](small-confirmation.json)
- [Rejected exploratory repeated-channel probe](duplicate9-probe.json)
- [Native outputs, provenance and scope checks](verification.json)
- [Sanitizer log](asan.log.gz)
- [Reversible task-only patch](select-prepare.patch)
- [Prior watchdog baseline reproduction](../watch-index-2026-10-27/verification.json)

All 640 main/confirmation process records and exact per-call value/count histograms are retained, including warmups and unfavorable samples. The six exploratory process summaries are retained separately. The patch contains only this task’s changes relative to the already modified initial working tree.


## Cleanup

Removed only this task’s temporary baseline executable, build-file/source checkpoints, CMake configuration, temporary logs, benchmark object and two reproducible experiment/test executables. No deletion failed. Current production libraries and caches, dependencies, prior experiments and user modifications were preserved. Final source, native tests, 640 process records, exact histograms, exploratory evidence and the task-only patch remain.
