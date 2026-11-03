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

# Windows multi-M async preemption, 2026-10-08

# Windows fixed-primary multi-M: local handoff

Implementation is present in the local checkout. The selected experimental
feature has native two- and three-primary protocol coverage; the whole repository
is NOT merge/release-ready. No commit, push, PR or deployment was performed.

## What changed

- Per-primary actual native handle, stack bounds, binding/run identity, captured
  and pending state, reserved origin queue and persistent operation slot.
- All-native-workers startup barrier, before user dispatch/automatic capture;
  nth failure and stop abort without trying to execute NEW tasks.
- One rotating serialized controller with at most two new probes per tick.
  Several admitted operations may coexist; physical native suspension stays one.
- Origin restoration for full async contexts. Only a fresh eligible explicit
  cooperative yield with old async ownership retired can release G affinity.
  Runtime-internal yields, park/offload and inline foreign detach stay constrained.
- Immutable first-dispatch debt owner, separate allocation/service/finalizer
  owners, ordered global drain and nonblocking counted reclamation.
- Task-owned Windows errno/LastError preservation, native-TLS return checks,
  Win64 private handle-vs-self identity correction, sparse process affinity.
- Exact duplicated binding handle created/successfully closed ledgers. A whole-
  process handle count is diagnostic, not an ownership/liveness proxy.
- Existing expert API/ABI/default-OFF retained. No new public plan/run API.

| Continuation / ownership | Routing |
| --- | --- |
| Full captured/READY/restoring context | Capture M only |
| Running restored G, banks FREE | Still origin-bound; G.ctx can be stale |
| Internal yield or park/offload | Origin-bound |
| Fresh completed eligible user yield, no old async ownership | One ordinary READY entry; destination rebinds |

## Verified

- Latest stable-header MinGW GCC15.2 Release: native multi-M CTest **14/14**,
  5.36 seconds, real 2/3 Ms, bounded children and sparse affinity.
- Exact 240M-call numerical result **3008183512622477815**; repeated automatic
  capture/local-peer/restore/direct-witness chains per actual native M.
- Concurrent operations2 with suspension max1; immediate and delayed migration,
  allocation0/debt1/finalizer2, old scan retained after destination DEAD,
  stale identities, real public destroy, repeat run and zero-entry rollback.
- Win64 live handle/self/duplicate identity truth table and scheduler/G TLS,
  errno/LastError sentinels; foreign refusal preserves P/active-G/service state,
  supported callback offload remains affine.
- Final feature bank/op/debt/retire/finalizer/quota counts zero; owned duplicated
  handles created/closed exactly2/2 or3/3, including repeated runs and aborts.
- Full native executable matrix: **103 pass, 3 skips, 1 broker failure /107**.
  One-M, default-off, public/fail-fast, ABI/shared-load and I/O cases passed.
  A skipped lease executable still completed eligible checks; Linux cases are
  not counted as Windows passes. Host Python/package commands excluded (9),
  not silently converted into native passes.
- macOS final `make -j4 CC=clang all test` passed; earlier one helper-handoff
  failure remains in the history, not claimed root-cause fixed. Diagnostic-only
  enhancement, isolated lease checks and a fixed50-case reproduction passed.
- Actual Ubuntu amd64 container, LLVM18.1.3: metadata/registration/plugin **3/3**,
  4.55 seconds. Prior 2CPU/4GiB/512-PID limits and read-only source mount kept.
  Container stopped afterward; existing contents preserved.
- Actual PE export table: **131** public functions, none extra/missing.
  Generic nm on PE sees nonexported globals, so its mismatch is not a DLL export
  leak. Production-hook audit passed both production artifacts.
- `python3 -m mkdocs build --strict` and `git diff --check` passed.
- License policy unit27 + package layout4 passed. All-tree policy remains blocked
  by the unrelated historical allocator status artifact described below.

## Remaining global blockers / preserved non-passes

1. Windows broker session budget case rejected request63 with EPIPE in a capped
   full matrix. An isolated unmasked diagnostic passed13.89s without code change;
   that does NOT erase the original failure or establish its cause. Broker
   session close/flush was not rewritten in this fixed-primary task.
2. `make check-license-policy` currently flags old deleted-license paths in
   `research/allocator-algorithm-upgrade-2026-10-08/artifacts/preexisting-status.txt`.
   This is user-owned historical measurement output; preserved rather than
   redacted to make the global gate green. Earlier policy pass is not substituted
   for the latest failure.
3. Concurrent allocator work changed private headers during verification. The
   latest focused cohort was rebuilt and recorded. An earlier broad suite does
   not qualify arbitrary later working-tree edits. User was asked whether to
   pause or use a tested-cohort handoff; no unrelated changes were reset.
4. Sampling-based FREE/REMOTE and late-tail observation failures are retained;
   deterministic production-boundary seams replaced those missed-coverage
   oracles, not the runtime or its numerical/ownership requirements.
5. Whole-process handle totals varied+1/+2 and remain un-attributed. Exact owned
   binding-handle ledger passes are NOT a claim of process-wide leak freedom.

## Review / limits

Designated GPT-6 Pro MM-01 approved design; D1 found the global-drain observation
ordering issue; D2 accepted the narrow feature protocol; D3 approved the private
identity/front-door corrections, with narrow audits and final-tree tests. D4
bounded handoff disposition ACCEPTED the feature-local implementation and
qualification, while declaring global merge/release NOT READY. These are reviews of reported source
protocol/results, not independent repository/host inspection.

Supported evidence: default-off experimental Win64/MinGW, fixed qualified primary
workers, caller-certified immutable PE-image ranges and explicit process-wide
fail-fast acknowledgement; 2/3 primaries with the pinned R8 scalar fixture.
Unsupported/unqualified: dynamic scaling/merge, async helper/relief execution,
cross-M full native-context restoration, arbitrary C/native-TLS migration or
arbitrary-PC capture, wider compiler/ISA/XSTATE/CET profiles and forced cancellation
of an uncooperative G. No stress, soak or release-quality speedup claim.

adoption_status: ACCEPTED_BOUNDED_LOCAL_HANDOFF.
merge_readiness: NOT_READY_GLOBAL_GATES. The owned duplication/close ledger
qualifies its tracked resource obligation only; whole-process drift remains
un-attributed, potentially including other LLAM resources. No causal conclusion
about the retained broker/Mac failures or performance ranges is established.

## Small dormant-path smoke

Matching GCC15 Release,16 actual workers,512 spawn/join operations,5 rounds and
warmup1; three alternating baseline/current pairs after the other cases finished.
Baseline throughput: 1.084/1.129/1.179 M/s; current:1.107/1.211/1.052 M/s.
Ranges overlap. Shared host and concurrent allocator edits prohibit attributing a
speedup or claiming an isolated release-quality regression result.

## Reproduction and evidence

Local cross-build:
```sh
cmake -S . -B BUILD -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64 \\
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \\
  -DCMAKE_BUILD_TYPE=Release -DLLAM_ENABLE_WINDOWS_BACKEND=ON \\
  -DLLAM_EXPERIMENTAL_ASYNC_PREEMPT=ON -DLLAM_WINDOWS_ASYNC_SCHEDULER_ORACLE=ON \\
  -DLLAM_WINDOWS_ASYNC_CERTIFIED_DLL=PINNED_DLL
cmake --build BUILD --target test_windows_async_multim_oracle -j4
ctest --test-dir BUILD --output-on-failure -R '^test_windows_async_multim_'
```

The last command runs on the native Windows test host; cross-building alone is
not native qualification. Host resources were read before bounded serial tests;
no large server stress/soak or shared configuration mutation.

Raw early final batch: [native samples](native-samples.md). More current generated
native CTest log: `/tmp/llam-win-multim-20261008.GQpKl2/native/final107-LastTest.log`.
Source baseline snapshot:
`/tmp/llam-win-multim-20261008.GQpKl2/baseline-source.tar.gz`, SHA256
505f8bc55b50711c1d82cef22e9d9f81ba7fc76e11fc8717b63407ed0347facb.

Latest focused cohort:

- runtime_types.h: a4038ce4b7a5c171a85c4b4b3f3b54e6829cb39c4a54f1044019aee8920f2a9f
- windows_async_lifetime.c: bd76f7d3f208da2007d80ee703f97dc2401e925e8285e8a8fcbc7f88014a0b15
- runtime_windows_compat.h: 9899ba375896262a45cd4196d278b9ea640df3da8bd1b76492345e65ae04ca1d
- blocking_api.c: 237e37f3a8500613a620968acdbc8e12eecb081bae65701afe894b72243ae70b

Microsoft primary references supporting native identity/handle semantics:
[thread handles and identifiers](https://learn.microsoft.com/en-us/windows/win32/procthread/thread-handles-and-identifiers),
[GetThreadId](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadid),
[kernel-handle tag bits](https://devblogs.microsoft.com/oldnewthing/20050121-00/?p=36633).
