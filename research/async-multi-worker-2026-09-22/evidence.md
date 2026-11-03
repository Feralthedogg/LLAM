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

# Evidence and integration checks

## I-MULTI-01: origin-M multi-worker and foreign coexistence

Primary outcome: a runtime with multiple workers can actually preempt/resume
certified no-poll work and service declared foreign offload concurrently without
rescue, wrong-M restore, duplicate execution or premature reclaim. This is a
capability extension, not a claimed throughput improvement. Baseline is the
one-P experiment snapshotted in `.artifacts/async-multi.sbnKtq/`.

Regression plan before implementation: retain native macOS baseline `bench`
executable, compare spawn_join, channel_pingpong and opaque_block in fresh
processes under matching options (3 independent processes, 7 rounds/2 warmups).
Use existing bench guard thresholds unchanged; no new invented percent target.
These ordinary-path measurements are not Linux async performance qualification.
For the experimental path, compare admission/progress at 2 and 4 workers,
and assert actual capture counters so cooperative fallback cannot pass.

## Source observations (draft exposed: prior research and own code)

- E-M01, supported: existing enable/teardown prepares only shard-0 primary M and
  rejects more than one shard/non-deterministic mode. `async_context_linux_x86_64.c`.
- E-M02, supported: capsules currently go to ordinary P norm queues even though
  restore requires origin M/incarnation. Simply removing the one-shard guard is
  unsafe with stealing/overflow/merge. `scheduler_return`, `resume_task`,
  `core_queue.c`, `scheduler_engine.c`, `watchdog_merge.c` inspected.
- E-M03, supported: inline foreign enter relinquishes P to helper/redirect while
  keeping G on its native M; a capsule on that M needs an independent service
  guarantee. `blocking_api.c` and the prior report §7 inspected. Current blanket
  rejection prevents this conflict; any changed admission must preserve it.
- E-M04, supported with correction: `llam_call_blocking_result` already parks G
  and submits a callback to a fixed-size runtime blocking pool. `block_pending`
  includes queued/running work and rejects UINT_MAX overflow, **but there is no
  ordinary finite queue admission limit** in `llam_runtime_note_block_pending`.
  Kickoff described the pool as bounded; worker count is bounded, queued jobs
  need an explicit bound for this integration. This distinction will be sent
  to Pro at the next decision exchange, not hidden.
- E-M05, supported: secondary scheduler workers can start dispatch before primary
  shard 0 enters its loop. Every-worker ABI preflight needs a run-wide startup
  barrier and failure release before admitting any user work. `lifecycle/run.c`
  and `scheduler_loop` inspected; partial pthread-start failure must release it.
- E-M06, supported: dynamic offline pauses owner then migrates queues/timers/I/O.
  New origin-M queue/pins must participate in preflight, locked final merge
  predicates, idle predicates and drain; a zero ordinary-queue depth is not
  absence of work. `watchdog_scale.c`/`watchdog_merge.c` inspected.

Independent verification here means actual source inspection and later execution,
not independent-model consensus. No new implementation claim is yet verified.
Prior evidence: `research/async-preemption-2026-09-22/` and
`.artifacts/async-base.zJu7M6/implementation.md`; do not rewrite historical status.

## Executed baseline checks

- E-M07: newly built baseline Debug library + existing one-P test passed the full
  no-poll/SSE/state/retirement/failure suite under QEMU Nehalem in this run's
  container. `baseline-async-sse.log`; emulation only, not native qualification.
- E-M08: a two-CPU affinity-limited admission probe recorded `workers=2`,
  `enable_rc=-1`, `errno=95`, `reason=single-shard-required`, `user_tasks=0`.
  `baseline-multi-admission.log`; the probe's process exit 0 indicates successful
  observation, not successful enablement or async execution. This is the valid
  capability baseline for multi-worker admission, not a throughput denominator.
- E-M09: native baseline benchmark guard (7 rounds/2 warmups) passed unchanged
  thresholds: spawn_join 2,843,325.17 ops/s; channel_pingpong 3,424,819.53 ops/s.
  One-process sanity check only; final paired three-process comparisons remain
  NOT_RUN. `baseline-bench-guard.{log,json}` contains conditions/raw output.

## Final implementation and verification observations

- I-MULTI-02: repeated multi-worker no-poll runs pass with 2 and 4 primary Ms;
  startup barrier checks the actual per-thread frame probe before dispatch.
  Both AVX-capable and baseline-SSE emulated CPU models execute the restored Gs
  on the recorded origin native thread. Logs are `multi-sse-repeat-*.log`,
  `multi-avx-final.log`, and `multi-snapshot-diagnostic.log`.
- I-MULTI-03: bounded capsule and blocking admission retain the tested 64 MiB
  and 1024-ticket defaults; allocation failure, concurrent quota admission,
  timeout, queued cancellation, callback completion, startup failure, plain
  stop, hard destroy, and delayed normal-descriptor replacement all have
  explicit outcomes in `multi-sse-repeat-1.log` / `multi-avx-final.log`.
- I-MULTI-04: the empty-only dynamic worker regression now observes online count
  6 -> 4 with origin-affine parked Gs on M4/M5, then reopens M2 when eight
  started no-poll Gs create pressure on M0. The probe runs on M2 only after its
  native startup qualification. No G crosses to another M. The test additionally
  observes that an older pause cleanup token cannot clear a newer pause request.
- I-MULTI-05: `check_legacy_lease_boundary.py` accepts the current source and
  its 139 mutation/regression tests pass. `make check-license-policy` passes 27
  policy tests, 3 archive/install tests, and the all-current-files header check.
  `scripts/test_preempt_package.py` and `mkdocs build --strict` pass.
- I-MULTI-06: native macOS `make -j4 CC=clang all test` passed. A separate
  CMake/CTest build passed 47/47 with two expected async backend skips. Direct
  async binaries were built and run under QEMU for the two positive x86 CPU
  models; this does not qualify native x86-64 or production support. TSan and
  native Windows/Darwin async qualification remain untested.
- Third-party snapshot verification compares all 19 tracked `third_party/magic2`
  upstream files against the before-work SHA-256 inventory. All are unchanged.
  The LLAM-authored provenance metadata `third_party/magic2/UPSTREAM.md` was
  deliberately switched to Apache-2.0; the vendored and byte-identical private
  mirror remain under the upstream MIT terms.
- The final ordinary-path baseline/candidate samples, if the paired run has
  completed, live beside `baseline-bench-guard.{log,json}` under
  `.artifacts/async-multi.sbnKtq/`. The baseline and final binaries are both
  native Mach-O ARM64 with matching 7-round / 2-warmup cases. No async throughput
  result is inferred from these ordinary-runtime cases.

- I-MULTI-07: final ordinary benchmark guard passed the unchanged spawn/join
  and channel thresholds. Eight fresh 7-round/2-warmup runs per build over
  spawn/join, channel ping-pong, and opaque blocking produced medians of about
  1.98M vs 2.23M ops/s, 3.29M vs 3.52M ops/s, and 88.35 vs 88.14 ops/s
  respectively (baseline vs final). Sample ranges overlap substantially; the
  host varied enough that these measurements support no reliable speedup or
  regression conclusion. All raw logs are `bench-paired-*.log` and
  `bench-abba-*.log`; final guard is `bench-final-guard-recheck.{log,json}`.
- I-MULTI-08: the broader native ASan/UBSan suite passed runtime-core,
  I/O-buffer, shutdown, and worker-identity checks, then one run of
  `test_legacy_lease_internal` hit its helper-activation/deadline oracle. A
  focused rebuild/run of that same target passed; the overall ASan suite was
  therefore not a clean single-run pass. No sanitizer memory report was
  emitted before that deadline failure. TSan and native Windows packaging were
  not run locally.
- The repository sanitizer target removes root-level `asan-test_*` binaries
  before rebuilding and on exit. It removed a pre-existing generated
  `asan-test_legacy_lease_internal`; that filename was rebuilt from the current
  source and its focused test passed. The original binary bytes were not
  preserved, but the test target is regenerated and available at the same path.
