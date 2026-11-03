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


# Wake and handoff policy experiment results

2026-10-27, Apple M4 (10 logical CPUs), macOS 27.0.1 arm64, Apple Clang 21.

The experiment is implemented and the focused concurrency checks pass. Training selected burst 8 from the verified finite template. Fresh confirmation does not establish a general performance improvement, so the production default remains 64. The whole-repository package and license gates remain incomplete because file reads blocked; details appear below.

## Policy and verification scope

The experiment searches the existing shared wake/yield handoff burst setting, preserving every runtime guard and atomic operation. The experimental service bound is 16 direct transitions before scheduler service. It is a research constraint, not an existing API guarantee. A 17-transition counterexample eliminates bursts 0 (unbounded), 256, 64 and 32. The remaining 16, 8, 4 and 1 enter training. Native observations reached streak 64 for the baseline and 8 for the selected policy.

The wake-handshake model explores 38 distinct states and two terminal states under an SC abstraction. It rejects both deliberate duplicate-enqueue and enqueue-before-arm mutations. The native verifier replays six coarse call orders and runs 3,000 three-thread trials against the production wake/park functions. Node lifetime is retained throughout; these results are not a full C11, reclamation, cancellation, or scheduler proof. The existing runtime tests provide additional implementation coverage. No ETH solver is implemented.

## Confirmation measurements

The same optimized binary and one processor are used for both policies. Each case has three training repetitions and seven fresh confirmation pairs, plus discarded warmups. Training order rotates and confirmation alternates execution order. Work, exact delivery, checksums, processor count and active handoff paths are checked on every run. The winner is frozen before confirmation.

Latency is message send entry to receive return, sampled at one varying phase per 16-message block. p99 uses nearest-rank on individual event samples (4,096 for the pair case and 16,384 for four pairs per confirmation process). Throughput counts request/ack exchanges and excludes setup, joining and teardown. These are closed-loop local channel measurements.

| Workload | Baseline M exchanges/s | Burst 8 M exchanges/s | Paired throughput change | Baseline p99 ns | Burst 8 p99 ns | Paired p99 change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| pair | 3.390 | 3.339 | -0.38% | 292 | 334 | +14.38% |
| contended | 3.298 | 3.229 | -2.08% | 1417 | 1375 | -2.96% |
| timers | 3.457 | 3.423 | -1.48% | 1333 | 1333 | +0.00% |

Absolute values are medians of process measurements. Changes are medians of paired ratios, so they need not equal the ratio of the two medians.

| Workload | Throughput ratio range | Message p99 ratio range |
| --- | ---: | ---: |
| pair | 0.911–1.231 | 1.000–1.336 |
| contended | 0.851–0.991 | 0.941–1.117 |
| timers | 0.960–1.011 | 0.969–1.032 |

The single-pair p99 regression and lower throughput prevent recommending a global switch to burst 8. The contended p99 difference is small and varies across pairs. Timer-observer lateness has a lower median p99 (4,625 → 1,583 ns), but some runs have only 92 observer samples; that statistic is diagnostic and is not evidence for a production latency improvement. The experiment establishes a shorter direct chain, not a universal speedup.

## Native regression results

Each of the following passes with inherited LLAM overrides removed, once under the baseline and once with `LLAM_YIELD_DIRECT_HANDOFF_BURST=8`: `test_runtime_core`, `test_multi_runtime_core`, `test_runtime_select_edges`, `test_runtime_shutdown_internal`, `test_runtime_invariants`, `test_legacy_lease_internal`, `test_sync_primitives`, `test_runtime_fuzz`, and `test_processor_queue_timer_internal` (18 successful process runs). Individual tests may set their own scoped policy options; the benchmark separately verifies its actual burst.

`make -j4 CC=clang test-handoff` passes 11 Python/model/native-smoke tests and the native protocol verifier. CMake configuration and its model CTest entry pass; native builds were exercised with Make to avoid duplicate builds. The production test-hook audit passes for both library artifacts. Linux, BSD and Windows execution, sanitizer runs, and full weak-memory model checking were not performed.

## Whole repository gate limitations

`make -j4 CC=clang all test` initially exposed an existing missing `watchdog_worker.o` test-hook override. The narrow Makefile correction resolved the undefined `llam_watchdog_test_scheduler_point` reference and allowed ownership/publication tests to link. Production source and CMake hook semantics are unchanged.

The resumed full gate reached `scripts/test_preempt_package.py` and stalled copying `docs/requirements.txt`. `lsof` identified that open file, and `ls -lO` reported `compressed,dataless`. The task-owned process was interrupted without changing the original file. `make check-license-policy` passed its unit tests but its final repository scan also blocked in a file read and was interrupted. Neither command is reported as a complete pass. All five new C/Python files passed a scoped notice check.

## Reproduce and inspect

```sh
make -j4 CC=clang test-handoff
python3 -B scripts/bench_handoff.py --samples 7 --training-samples 3 --warmup 1 \
  --rounds 65536 --service-bound 16 --output research/handoff-new-run.json
```

- [Experiment design and limits](../../experiments/handoff/README.md)
- [Measurements, counterexamples, exact sample histograms, commands and provenance](comparison.json)
- [Native test outputs and gate status](verification.json)
- [Only this task’s changes to the already-modified build files](build-registration.patch)

All 108 training/warmup/confirmation process records are retained. The sorted nanosecond samples were losslessly converted to exact value/count histograms and checked value-for-value, reducing the result from 11,628,085 to 447,402 bytes. The original measurement-source hashes remain in the data; the serializer change is recorded separately.


## Cleanup

Removed only the task-created CMake configuration, intermediate experiment objects, two reproducible experiment executables, and temporary work copies/logs. No deletion failed. Existing build caches, dependencies, user measurements and the dataless original file were preserved. Final sources, exact measurement histograms, verification evidence and the build-registration patch remain. Rebuild the experiment with the commands above.
