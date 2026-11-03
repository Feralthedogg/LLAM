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

- delivery_target: INTEGRATE in /Users/feral/Desktop/Programming/LLAM.
- User request: implement item 1, multi-M Windows preemption, after the R8
  single-primary-M compiler-metadata integration. Existing direction to use
  the designated ChatGPT Pro conversation for important decisions persists.
- Goal: several fixed primary M workers execute and automatically preempt
  certified Gs in one runtime with correct origin-M restoration, worker-local
  binding lifetime, safe G migration at cooperative/free-bank boundaries and
  complete stop/destroy drain. Keep the public ABI and expert opt-in contract.
- Starting scope assumption for Pro to review: fixed primary worker pool.
  Dynamic P merge/scale and opaque/relief M qualification are separate features;
  the current code rejects those modes and they are not silently enabled here.
- Guardrails: unchanged disabled/cooperative behavior; exact numerical results;
  no lost task, bank/binding/debt/ref leak, wrong-M restore, stopped-peer-runtime
  or stale native-handle use. Captured/READY/restoring Gs remain origin-bound.
- Native shared Windows host: inspect CPU/free memory before bounded tests,
  cap build concurrency and run short serial tests. No large stress/soak.
- Base: main at 34dcc74a7e3cbb584967f6bf5f7816673ed5169b, heavily dirty from
  prior user work. No attached managed worktree. Preserve all existing edits.
  Starting source snapshot:
  /tmp/llam-win-multim-20261008.GQpKl2/baseline-source.tar.gz
  SHA-256 505f8bc55b50711c1d82cef22e9d9f81ba7fc76e11fc8717b63407ed0347facb.
- codex_model: unknown exact variant (host identifies GPT-6 family);
  profile: general. No subagents requested for this slice.
- Pro selection verified on 2026-10-08: UI family GPT-6 checked and Pro
  at 5/5; official current model guide identifies GPT-6 family and pro mode.
  The first UI load failed; one reload restored normal selection.
  Source: https://developers.openai.com/api/docs/guides/latest-model
  Designated conversation: existing IAB tab 1, URL retained from R8.
- Research mode: engineering source/ownership analysis, bounded native
  protocol experiments, real-path integration and regression verification.
- Acceptance: two+ distinct primary native workers each perform repeated
  automatic capture/restore/witness on the same finite call and preserve its
  result; demonstrate independently owned outstanding operations; verify
  migration exclusion while bank-owned, safe unowned migration, startup
  failure and stop/destroy drain; run the prior single-M regression plus
  relevant ABI and ordinary runtime gates on final code.
- adoption_status: ACCEPTED_BOUNDED_LOCAL_HANDOFF;
  merge_readiness: NOT_READY_GLOBAL_GATES.
- No commit, push, PR, release, deployment or shared host configuration change.

## Pro exchange

- T-PRO-WIN-MM-01: send confirmed in the designated GPT-6 Pro conversation.
  Kickoff after source inspection: current
  native handle/stack bounds/binding/captured/pending state and watchdog
  operation are runtime-singletons. Shard-0-only lifecycle and global debt
  predicates need review; READY Gs currently have no Windows origin migration
  hook. Ask Pro to select the smallest cohesive multi-primary-M design,
  admission/drain invariants and discriminating native test batch.
- T-PRO-WIN-MM-01 response complete: APPROVE TO IMPLEMENT fixed primary-M
  bindings and persistent per-M operations with one serialized controller.
  Important revision: affinity is a cooperative service epoch, not inferred
  from FREE banks. Only a genuine completed explicit-yield save on the source
  scheduler may release it; park/offload wakeups remain origin-bound here.
  Allocate/qualify slots before run, register each actual native primary after
  worker identity, then commit a generation-tagged all-worker startup barrier.
  Partial startup abort must bypass NEW-task drain. Rotate nonblocking operation
  service; independent outstanding operations do not mean parallel suspension.
  Immutable first-dispatch debt owner, allocation owner, current service M and
  terminal finalizer owner are separate identities. All fixed Ms participate in
  global drain before any unbind/join. Preserve all unsupported-mode refusals.
- Pro requested native discriminators: two real Ms each repeat exact
  capture/restore/witness on one finite certified call; hold M0 scheduler (not
  controller) to show two outstanding operations; verify fresh cooperative
  migration and stale-owner exclusion; fault all-worker startup before user
  dispatch, mixed stop/destroy drain and unchanged single-M/default-off gates.
  Return source and traces for acceptance review; this is design approval, not
  implementation acceptance or universal Windows support.

## Baseline / prerequisites

- Current-source MinGW x64 Release baseline cross-build succeeded, with
  experimental backend and scheduler oracles. On native Windows the automatic
  transaction oracle passed and the compiler-certified fixture passed both
  disabled numerical equality and repeated positive capture/restore/witness.
- Guarded test_legacy_lease_internal.c's pthread.h by LLAM_PLATFORM_POSIX to
  avoid conflict with native Windows private pthread compatibility declarations;
  strict MinGW syntax check passed. Native/full-target verification pending.
- Windows allowed-CPU enumeration now honors a valid nonzero process affinity
  mask; strict MinGW syntax passed. This permits a two-worker child test without
  modifying the shared host. Multi-group/zero-mask fallback is preserved.
- Preliminary affinity-limited baseline bench still created 16 runtime workers
  and is NOT a matched regression comparison. Retain sample but do not claim a
  speedup. Final baseline/candidate comparison must match actual worker counts.

## Implementation / D1

- Per-primary native binding / stable operation slots, OPEN startup barrier,
  partial abort, origin intrusive queue, explicit fresh-yield epoch handoff,
  immutable execution-debt owner and terminal finalizer owner implemented.
  One-M automatic transaction and certified finite arithmetic tests passed
  on the native Windows host after the initial conversion.
- Two distinct native primary Ms each repeated five automatic exact
  capture/local-peer/restore/witness chains in one 240,000,000-iteration call.
  Both results matched 3008183512622477815; all bank/op/debt roots drained.
- READY-held M0 plus M1 service demonstrated two independently owned
  operations, maximum physical runtime-owned suspension one. Mixed stop with
  READY + pending RESTORING passed. Nth-slot allocation, thread start,
  duplicate-handle and post-duplication qualification failures all passed with
  user entry count zero (2 or 3 actual Ms, bounded cases).
- Forced cooperative B1 -> A0 migration passed: allocated on 0, debt charged
  once to 1, terminal cleanup on 0. Each finite invocation repeated automatic
  capture/restoration on its actual native M. FREE remote continuation remained
  origin-bound before explicit fresh save. Copied wrong-B identities were
  refused against a live A-owned bank. Logical errno and LastError preserved.
  Test forces placement of an already-qualified ordinary READY entry through
  the existing reinjection path; this is not a work-stealing throughput claim.
- T-PRO-WIN-MM-D1 sent and response fully read: APPROVE TO CONTINUE, final
  acceptance OPEN. Pro reviews reported protocol/evidence, not independent
  source inspection. Confirm source-local ordinary READY publication (already
  implemented); make global zero coherent across cross-owner handoff; inspect
  optimized destination TLS/error return, and test delayed ref release,
  three distinct owners, mixed public destroy and repeated native bindings.
- D1 correction: global drain now inspects ALL immutable execution-owner debts
  before ANY finalizer/operation/ready roots. Acquire observation of a terminal
  release orders inspection of its previously registered finalizer root. The
  earlier interleaved per-slot scan could report false zero across owners.
- GNU Release objdump confirms task-to-scheduler return obtains errno by a new
  _errno() call AFTER ctx_switch; it caches only the immutable import function
  address, not the source native errno address. Destination LastError uses its
  imported native API and task-owned value. Full final emitted-code record and
  owner-thread scheduler sentinels remain pending.
- Initial three-owner test observed allocation0/debt1/finalizer2, execution0 /
  finalizer1 and all three actual Ms alive during a retained scan pin, but then
  failed an extra FREE-remote sampling coverage assertion. This nondeterministic
  sampling belongs to the separate two-M migration test, not the three-owner
  retirement oracle. The three-owner assertion is scoped to its decisive
  ownership/global-drain checks; retain the initial non-pass in the report.
- Subsequent two-M sampling also missed the FREE/REMOTE interval. Replaced
  sampling entirely with a deterministic, lock-free AFTER_BANK_FREE production
  seam after a real automatic direct witness. The unpinned running G is
  demonstrably still origin-bound with FREE/extrefs0 and REMOTE continuation.
  Updated two- and three-owner tests both pass; no save/capture is fabricated.
- Runtime-internal retries and safepoint yields use private affine yield, so
  only eligible explicit user yield requests release. Other platforms/builds
  preprocess this to the original function. Worker critical/disable depth also
  blocks release requests. Native negative + immediate TLS/error checks pass.
- Final 14-mode serial native batch passed, raw commands/traces in
  native-samples.md. Sparse allowed CPU ids are checked, per-process native
  handle counts before/after ordinary runs match. Delayed op_finish releases
  the latest source save; destination goes DEAD/capsule-free before the old
  scan/binding release, leaving finalizer0 and old binding1 while both Ms live.
  Repeat forces a late old sweep, then fresh run2/incarnation2, valid copied
  identity and old-generation rejection. Startup stop executes zero user Gs.
- T-PRO-WIN-MM-D2 complete and fully read: protocol ACCEPTED / APPROVE on
  reported implementation/evidence, no remaining concrete feature MUST-FIX;
  adoption still conditional on same-final-tree one-M/default-off/public/
  fail-fast/ABI regressions. No independent repo/host inspection or deployment.
- First complete native CTest: 108 attempted cases, two worker/helper failures,
  one Mac Python/CMake bench harness Not Run, two Linux-only skips. Failures
  retained. General Win64 shim compared pthread_self raw TID with owned native
  HANDLE, yielding false inequality. Tagged full-DWORD borrowed self ids now
  compare via actual GetThreadId without conflating namespaces or clobbering
  LastError; join/close handle representation unchanged. Actual worker identity
  test passed, capped legacy checks passed with one existing POSIX-only skip.
- Final source audit also found the front-door enter_blocking async refusal
  was Linux-only: multi-M Windows could take redirect without helper startup.
  Extended early ENOTSUP to enabled experimental Win64 before P/depth mutation,
  preserving default-off behavior and callback offload. Native 2/3-M negative
  assertions plus owners3/delayed pass. T-PRO-WIN-MM-D3 sent, awaiting review.
- Actual Linux LLVM18 container regression passed metadata, registration,
  plugin 3/3 (4.55s) on the mounted current tree. Read-only source mount, prior
  two-CPU/4GiB/512-PID limits preserved; no security-setting expansion.
- macOS make -j4 CC=clang all test passed after internal-yield conversion;
  final rerun after ordinary Win64 corrections/documentation is in progress.
  License policy passed (27 policy + 4 package-layout tests and untracked scan).
- D3 fully read: both Win64 identity namespace repair and front-door foreign
  refusal APPROVED; D2 feature acceptance stands. Narrow consumer audit finds
  two direct-I/O callers, both return immediately on failed enter (poll hint
  unwinds) without native blocking or leave. pthread producers are private
  create/self; real owned handles feed join, borrowed self identities feed
  comparisons, with run/binding/incarnation authority kept separate.
- Added live two-thread handle/self/duplicate identity truth table and
  LastError/errno sentinels. Strengthened foreign refusal to preserve P owner /
  generation / lease epoch, active G, service origin and no redirect/helper
  activation. Supported callback offload returns correctly and stays affine.
- An attempted whole-process handle-count equality proxy varied by +1/+2 in
  otherwise correct native cases. Retained those non-passes. Exact runtime-owned
  duplicated-handle created/closed ledgers now cover actual DuplicateHandle /
  successful CloseHandle (checked result), staged abort and run-end/shutdown.
  Private counts match 2/2 or 3/3, both repeated runs match, 14/14 pass (5.75s).
  Whole-process counts stay diagnostic and are NOT claimed attributable to LLAM
  or proven leak-free. This does not replace bank/binding/operation/debt checks.
- Mac final full gate had one eligible helper-handoff failure. Added diagnostic
  output without weakening assertions; isolated clean-stop case, 50 fixed
  reproductions, then complete lease checks passed. Later same-tree full
  make -j4 CC=clang all test passed. Historical non-pass remains recorded.
- Windows full 107-case post-fix matrix: all feature/ordinary/ABI/I/O gates except
  broker session budget passed; 3 expected skips. test_security_capability
  rejected request63 with EPIPE in its Windows broker session-budget case.
  Separate unmasked diagnostic run passed 13.89s without any broker code change;
  the capped timing failure is unresolved, NOT converted to a pass. No broker
  transport rewrite is in this task's fixed-primary feature scope.
- Documentation strict build passed after removing an unrelated broken source-
  checkout link (kept the actual toolchain recipe path). PE export-table audit
  shows exactly 131 public functions, no extra/missing; generic nm-based script
  on a PE DLL read the whole symbol table and produced a false mismatch. Actual
  PE export inspection is retained as the authoritative Windows check.
  Production test-hook audit passed both production artifacts.
- Latest all-tree license policy fails on a concurrently created historical
  allocator preexisting-status.txt (old deleted-license paths). Its 27 policy
  and 4 package-layout tests pass. Preserve this user measurement artifact;
  do not redact unrelated history to manufacture a green global gate.
- Concurrent allocator work changed runtime_types.h/task_span.c during final
  verification; asked user whether to pause or report the tested cohort. Keep
  all changes. Current integration adoption/merge readiness are NOT final yet.
- T-PRO-WIN-MM-D4 complete and fully read: ACCEPTED for bounded local handoff
  of the implemented, experimentally qualified fixed-primary Win64 feature.
  No demonstrated feature-local must-fix remains on reported evidence. Global
  merge/release NOT READY: broker failure, policy/history artifact and live-tree
  cohort remain visible. Owned duplication/checked-close ledger is accepted for
  its exact resource domain; unattributed whole-process handle drift is NOT
  dismissed or claimed harmless. No scope expansion/commit/release authorized.
- Latest recorded focused cohort: native 14/14 in5.36s plus identity truth table
  PASS0.76s, expanded foreign ownership negative and supported offload positive.
  Important file hashes are in report.md; they are not a complete immutable
  source manifest. Later live runtime_types.h edits (a0ad5b... cohort) are not
  automatically qualified by earlier broad matrix/focused results.
- Pro screenshot proof saved from the actual selected conversation capture:
  /tmp/llam-win-multim-20261008.GQpKl2/native/pro-handoff.jpg (22086 bytes).
  Cropped only the disposition panel to omit unrelated personal chat/sidebar
  content. Original capture bytes preserved, no generated/edited result image.
