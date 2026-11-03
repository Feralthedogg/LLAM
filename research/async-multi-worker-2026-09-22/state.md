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

# Multi-worker async/foreign integration state

- User request: implement remaining item 2 first, multi-worker and foreign-work coexistence.
- delivery_target: INTEGRATE; unconditional local implementation request.
- Repository: `/Users/feral/Desktop/Programming/LLAM`, main at
  `34dcc74a7e3cbb584967f6bf5f7816673ed5169b`, plus extensive pre-existing integrated
  edits and the completed one-P experimental base. Preserve all prior work.
- Snapshot: `.artifacts/async-multi.sbnKtq/baseline-src-tests.tar.gz` plus
  `baseline-tracked.patch`. Build/test logs for this run go there.
- Allowed: scoped local runtime/test/build/doc changes and validation; summaries
  and necessary implementation excerpts in the same user-designated Pro session.
  No commits, pushes, PRs, merges, deployment, security-setting changes or wrappers.
- Scope: opt-in Linux x86-64 multi-worker execution, origin-M restore, safe foreign
  coexistence, ownership/lifecycle/resource gates. Cross-M async migration,
  compiler v2, multiple concurrently enabled runtimes, new native ports and
  production/native qualification are separate work, not implicitly complete.
- Acceptance: multiple workers actually capture/resume distinct Gs without
  corruption or rescue; existing blocking integration can coexist without
  stranding origin-bound capsules; stop/return/admission races preserve lifetime;
  ABI/default behavior preserved; final-tree tests and an honest regression
  comparison. Exact admission policy to be selected with Pro before implementation.
- Native x86 qualification remains NOT_RUN by user's choice to do item 2 first.
  Existing Linux translator io_uring gate fails identically on the old baseline
  (`ENOSYS` probe); retain as a limitation, never claim that suite passes.
- Budget: no explicit token, time or message limit; no paid external workloads.
- codex_model: unknown; profile: general (host model not verified).
- Active methods: technology transfer/project integration; empirical correctness
  and regression checks. Reuse prior research, expand literature only for a new
  consequential gap. No novelty or new exhaustive literature survey requested.
- adoption_status: NOT_EVALUATED; merge_readiness: NOT_READY; status: CONTINUE.

## Pro session / messages

- Same conversation only:
  https://chatgpt.com/g/g-p-69b65124f21c819191677f32cf45e678-feral/c/6a99bf49-4338-83ee-86c8-257a9d6c6cb0
- Browser tab 2, authenticated; current composer visibly `6 Pro` at
  2026-09-22 14:56 UTC. Latest-model guidance and model catalog reopened from
  official OpenAI sources. Live selector: `최신` checked, `Pro, 5개 중 5번째`,
  alternatives GPT-5.6 Sol and GPT-5.5. Official latest-model page identifies
  GPT-6 Astra and Pro mode; no API identifier inferred from UI label.
- T-PRO-MULTI-01 / `multi-foreign-kickoff-v1`: send confirmed; response COMPLETE,
  full text read (UI 12m51s). Decision request:
  multi-M origin service/foreign admission and the smallest complete integration
  contract, reusing the prior capsule and retirement design.

## Current work

- Codex owns source inspection, local implementation/tests and this record.
- Pro leads the consequential coexistence/admission decision and final assessment.
- Baseline tests executed: one-P async suite PASS under explicit QEMU Nehalem;
  2-worker activation rejected with `single-shard-required`, errno ENOTSUP,
  no G dispatched. Native spawn_join/channel benchmark guard PASS. Baseline
  executable and raw logs retained; these are not candidate results.
- Additional source finding: blocking pool thread count is bounded but ordinary
  queue admission only checks UINT_MAX, not a finite resource limit (E-M04).
- Pro-selected contract: bind every G at FIRST dispatch (not first capture),
  retain origin M/incarnation/run + affine_live_g service pin until off-stack
  exit and retirement; all started-G yield/wake/capture/offload routes use a
  distinct origin queue protected by existing shard lock. Only never-started
  Gs may remain in ordinary steal/overflow queues. Existing public PINNED means
  unchanged. Pool offload supported; managed inline enter and helper dispatch
  forbidden in activated mode. Prepare every configured M, then qualify actual
  native bindings with all-worker pre-dispatch barrier and dedicated abort.
- B1 all-worker prepare/abort and B2 origin routing/lifetime: implementing next.
  B3 finite offload admission/compatibility follows. Static multi is mandatory;
  conservative dynamic (empty origin-service domains only) requires its own
  B4 gate, otherwise reject that experimental combination before effects.
- T-PRO-MULTI-02 / `bounded-admission-correction-v1`: send confirmed, response
  COMPLETE, fully read (11m02s). CHANGES REQUIRED: early separate block ticket
  before job/node/ref allocation; transfer ownership once; retain queued-cancel
  tombstone/running-cancel tickets until actual final cleanup. Capsule charge
  returned only after bank storage retirement. Defaults 64 MiB/1024 accepted as
  engineering defaults, not SLO/RSS bounds. Apply before B3 qualification.
- T-PRO-MULTI-03 / `stop-service-gap-v1`: send confirmed, response COMPLETE and
  fully read (8m40s). APPROVE A: separate async drain service accepts actual
  OWNED or STOPPING_DRAIN projection for the same prepared primary/run/G;
  ordinary snapshot refusals unchanged; controller stays service-only until
  run execution ends, not public refs/controller join. Source-confirmed gap:
  handler ignores stop, destroy closes ctrl service and P snapshots reject
  STOPPING before started affine Gs can drain; request a narrow drain-service
  choice. Distinguish from the observed one-P scheduling-order regression.
- No new subagents or production source edits before the Pro decision.

## Implementation checkpoint

- B1/B2 source connected: per-configured-primary preparation, run barrier/abort,
  first-dispatch origin+service pins, lock-protected intrusive M-ready queues,
  yield/reinject/timer routing, logical pressure/idle observations, helper guards.
  Dynamic+async is currently rejected pending B4 service reservation implementation.
- New normal test `test_async_preempt_multi_linux_x86_64`: actual 2/4-primary
  no-poll captures with overlapping held capsules, origin preservation through
  cooperative yield/sleep and native offload/nested callback. First SSE run PASS.
- One-P late-witness regression initially FAIL with capture count 2 vs expected1,
  all state/witness oracles intact. NEW/origin alternation initialization fixed;
  old one-P suite now PASS. Failed logs retained, no assertion weakened.
- B3 implemented initial capsule logical-byte quota (default 64 MiB), early
  blocking ticket (default 1024) and finalizer release; config environment names
  `LLAM_ASYNC_CAPSULE_BUDGET_BYTES` and `LLAM_ASYNC_BLOCK_JOB_LIMIT`.
  Final queue close check also closes the existing preparation/stop window.
  Detailed saturation/failure/race tests still to write/run.
- New quota assertion caught a genuine earlier-base never-started capsule leak:
  lazy unlisted NEW tasks bypassed `llam_free_task` on destroy. Activated async
  now forces lifetime-list registration. After this fix, old one-P + new 2/4-M
  suites PASS (`b3-lifetime-regression.log`). Do not retain the older stronger
  inference that a noncrashing destructor alone proved memory cleanup.
- Test hooks added for nth worker allocation/start/probe failures, block-job
  allocation failure, and a pre-allocation block-reservation pause. Their new
  dedicated tests remain pending. Make registration for multi test/sanitizers
  and CMake registration added; final full gates and docs NOT_RUN yet.
- Boundary audit initially found 10 lexical violations. Two load sums were
  separated to avoid pointer-arithmetic false positives; the owner-clear allowlist
  admits only the read-only origin guard + abort. Audit passes; add a regression
  test for that narrow accepted/rejected-call boundary.
- B1 fault + B3 tests now PASS under QEMU Nehalem: nth-M allocation/start/probe
  rollback with zero user dispatch; capsule quota/failures/repeated-run reuse;
  limit=1 running-cancel, limit=2 cancelled queued tombstone, timeout/retry,
  recursive native callback and pre-allocation reservation/stop rollback.
  `block-slots-sse.log`. A misplaced test preprocessor block initially failed
  compilation and was moved to the correct Linux branch; log retained.
- Async drain implementation in progress: private read-only service snapshot
  uses a new locked P projection accessor without changing ordinary ticket or
  watchdog snapshots; request carries normal/drain mode + run generation;
  controller dispatches service-only probes during close. Need full drain
  regression, negative authorization tests, explicit sender audit coverage.
- T-PRO-MULTI-04 / `dynamic-reservation-v1`: send confirmed; response COMPLETE,
  fully read (10m12s), CHANGES REQUIRED for broad reservation: retain entire
  owner-side quiescence, exact cleanup independent of old normal ticket, and
  no dependency on paused source/controller service. Explicitly permits idle
  domains that require no backend ownership movement as the first B4 subset.
- Actual stop tests PASS (`drain-guards-sse.log`): normal descriptors are
  published but delivery delayed before close, then private drain requests
  replace them. Both plain stop and concurrent host destroy service each M's
  no-poll G via its admitted controller, plus timer/offload cancellation, without
  rescue. Ordinary STOPPING snapshots still reject; malformed service tickets,
  helper/null bindings and foreign G are rejected in dedicated tests.
- Sender audit now has an operation-closed private drain contract with target/
  early-unlock/unknown-call mutation tests. 139 checker tests PASS.
- T-PRO-MULTI-05 / `idle-only-atomic-commit-v1`: pending send. Selected narrower
  B4 implementation: reject any nonempty/service-bound domain; no rehome calls
  at all. Validate pause + all empty conditions and commit under source locks;
  then exact-generation pause release only. No long-lived migration reservation.

## Final implementation state (2026-09-23)

- B1–B4 implemented locally. B4 is the approved idle-domain subset: async
  offlining asks for a versioned owner pause, checks that the shard has no
  started G, ready-origin work, restore obligation, active lease, timer, or I/O
  waiter, and performs the online-bit/count change under the `opaque_lock ->
  shard->lock` pair. After unlock it only clears that exact pause sequence.
  Async-enabled dynamic scaling does not call queue/timer/I/O rehome paths.
  Worker-ring mode is rejected at admission. Ordinary runtimes retain their
  existing migration path.
- Added a deterministic six-worker regression: parked channels keep origin M4
  and M5 online while empty M3/M2 reach floor 4; eight no-poll Gs on M0 then
  cause the watchdog to reopen M2, where a pinned probe is checked, followed by
  full release/join/destruction. It also forces stale pause cleanup ordering.
  Channel handles are created inside the owning runtime; the test waits until
  all six waiter nodes are actually registered before closing any channel.
- Current Linux x86-64 CMake Debug experimental build passes the multi-worker
  suite repeatedly under QEMU with Nehalem/SSE and Haswell/AVX CPU models,
  including the idle-domain off/on path, stop and hard-destroy drains, startup
  failure rollback, quotas, and offload races. The one-P suite also passes both
  models. These are explicit userspace emulation results, not native x86
  validation. Linux io_uring host suites still hit the earlier, baseline-known
  QEMU `ENOSYS` capability gate.
- Native macOS `make -j4 CC=clang all test` completed; CMake/CTest completed
  47/47 with the two Linux-only async executables skipped by platform guards.
  Separate Linux QEMU runs exercised those async targets. TSan and a native
  x86-64 platform run were not available in this session.
- The legacy-boundary checker passes all 139 tests; policy/package tests and
  strict docs build pass after the project moved to Apache-2.0. Raw paired
  ordinary-path benchmark logs and their noisy ranges are recorded in
  `.artifacts/async-multi.sbnKtq/`; ordinary throughput is regression context,
  not evidence for the experimental async path's speed.
- GPT 6 Pro was re-consulted in the same user-designated conversation. After
  checking the scheduler ACK boundary, all online CAS call sites, the corrected
  zero-count recovery path, and controller/shard teardown order, Pro affirmed
  that its three source-level caveats were closed. It approved local
  experimental delivery under the same native/production qualification limits.
- Final delivery caveats: QEMU-user execution is not native x86-64 signal-return
  validation; the Linux io_uring translator suite still has its reproduced
  baseline ENOSYS limitation; the full ASan suite did not pass in one run even
  though its isolated lease test rerun passed; TSan/Windows packaging remain
  untested. Nothing is committed or published.
