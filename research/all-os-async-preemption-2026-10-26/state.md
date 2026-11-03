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

# All supported OS asynchronous preemption — active state

## Mission contract v1

- G0 / source: user request, 2026-10-26: "지금 지원하는 모든 os에서 완전한 비동기 선점형이 가능하도록 만들어".
- Delivery target: INTEGRATE, `/Users/feral/Desktop/Programming/LLAM`.
- AC1: A connected asynchronous-preemption path for every currently supported
  OS/architecture in `docs/reference/platform-support.md`; unsupported code or
  context must not be falsely reported as asynchronously preemptible.
- AC2: CPU-bound qualified managed code permits another runnable task to progress
  without an explicit application yield; resumed computation remains exact.
- AC3: Preserve ABI, ordinary disabled behavior, runtime isolation, signal/native
  state, ownership, startup rollback and stop/destroy draining. Arbitrary foreign
  C/locks/TLS do not acquire a new safety guarantee by changing an enable flag.
- AC4: Platform-appropriate behavioral and regression checks, reproducible source
  identity, accurate platform coverage, reviewed local handoff, and cleanup of
  this run's unnecessary in-project outputs. Missing native execution is NOT_RUN.
- Non-goals: commit/push/PR/release/deployment; unrelated broker, allocator,
  historical-artifact or licensing repair; forced cancellation of arbitrary C.
- Resource constraint: bounded tests on shared remote hosts after load checks;
  avoid redundant builds, installations and source copies. No user numeric budget.
- Interpretation awaiting design review: "full" concerns LLAM-managed qualified
  code, not arbitrary-PC interruption inside unknown foreign/library critical
  sections. Do not weaken G0 to a capability flag, a plan or compile-only evidence.

## Baseline and safe scope

- Branch `main`, HEAD `34dcc74a7e3cbb584967f6bf5f7816673ed5169b`.
- Starting working tree: 429 status entries. All pre-existing edits and artifacts
  are user-owned and preserved. Relevant content hashes will precede changes.
- Supported matrix: Linux x86_64/aarch64; macOS x86_64/arm64; FreeBSD, OpenBSD,
  NetBSD, DragonFly BSD x86_64; Windows 10/11 x86_64.
- Initial observation: only Linux/Windows x86_64 have default-off experimental
  asynchronous native-context backends. Windows additionally requires explicit
  certified PE ranges and process-failfast acknowledgement.

## Collaboration

- Codex model: unknown (no reliable current-session family metadata inspected).
- Profile: general; profile source: model-verification unavailable.
- Browser: in-app browser 2, tab 1; only the user's designated existing ChatGPT
  conversation is used (same private conversation as Windows multi-M research).
- Official guidance inspected: `https://developers.openai.com/api/docs/guides/latest-model`.
- UI verification, 2026-10-26: GPT-6 checked; Pro shown but locked with usage-limit
  notice. Selected and read back `Extra High`, 4 of 5. Earlier user-authorized
  fallback applies. Do not attribute these new exchanges to Pro mode.
- T-ALL-01: send confirmed, generation complete and full response read. Selected
  common lifecycle/native adapters and actual Mach arm64 fixture before runtime
  activation. Source/API references independently checked; no model consensus
  promoted into execution evidence.
- T-ALL-02: send confirmed, generation complete, full response read. Shared new
  adapter protocol accepted as a bounded integration route; native task retirement
  and target-side mask/errno exchange must accompany real scheduler integration.
  One-M persistent-operation staging proposed; a multiple-CPU-leaf liveness
  counterexample remains to resolve before calling the resulting runtime general.
- T-ALL-03: send confirmed in Extra High, complete response read. Accepted the
  multiple-no-yield-G liveness counterexample and capture-op -> G-owned READY
  root transfer. Separate arrival ticket, fresh restore refs, exact witness and
  counted deferred finalizers are required. Keep next READY queued while an old
  pending-restore slot is occupied. Exact legacy FXSAVE frame support approved
  for testing, without relaxing modern-context checks.
- T-ALL-04: UI readback now confirms Pro5/5 selected, no disabled/usage blocker.
  Sent actual native Linux PASS and Mac7/7 mixed-runtime evidence, ownership
  protocol description, decisive adversary questions and next-port sequencing.
  Response complete and fully read. Approved continued Mac hardening, requires
  task-only reader and witness/return races, repeated captures per original G,
  pending-signal transition and exact source handoff review. Next port selected:
  Linux arm64 async-signal-safe frontend + fresh actual signal-frame return,
  not remote-suspend emulation. Windows AND old Linux require the same-M mixed
  no-yield liveness oracle; prior passes do not prove that overlap property.
  Do not retroactively call T-ALL-01..03 Pro runs.
- Active modes: engineering applicability/technology transfer and empirical
  correctness; strategy to be selected with the research lead.

## Sparse active graph / admission

- D1 DISCOVERY -> AC1/AC3: inspect platform matrix, existing context and admission
  contracts. Reads: platform support, runtime headers, context/preempt code,
  scheduler integration and matching tests/build registration. Scratch writes:
  this state and evidence records only. Pass: concrete backend gaps and safe
  extension boundary; no product writes during D1. Status: active.
- D2 DISCOVERY -> AC1/AC3/AC4: research-lead design review plus primary OS/compiler
  contracts. Depends on D1 facts. Pass: defensible implementation route and test
  obligations; model agreement alone is not evidence. Status: ready.
- C1 CHANGE / ALL-1 -> AC1/AC3: introduce a private backend-neutral lifecycle
  seam and shared bank phase constants. Keep native context, locking, references
  and platform qualification in existing backends; no all-at-once Linux split.
  Exact writes: `src/internal/async_context.h`, `async_context_phase.h`,
  `runtime_internal.h`, `windows_async_lifetime.h`;
  `src/core/context/async_context.c`, `async_context_linux_x86_64.c`;
  `src/core/lifecycle/run.c`, `shutdown.c`; `src/core/task/task_stack.c`,
  `task_reclaim.c`; `Makefile`, `CMakeLists.txt`; focused tests as admitted next.
  Existing files checkpointed in the execution session before mutation. Linux
  phase renumbering remains internal; Windows assembly GATE=4 is preserved.
  Pass: identical lifecycle ordering/default behavior, focused Mac and native
  Linux/Windows regression; source review verifies no lost abort/drain hooks.
  Risk: lifecycle entry ordering and disabled Linux cleanup. Status: admitted.
- V1 VALIDATE -> AC2/AC3/AC4: affected real-path tests and cross-platform regression
  checks after C1. Status: proposed.
- C2 CHANGE / ALL-2 -> AC1/AC2/AC3: native Darwin arm64 Mach mechanism, using
  existing context switch ABI and no runtime activation yet. Exact writes:
  `src/internal/async_context_darwin.h`, `src/core/context/async_context_darwin.c`,
  `src/asm/darwin/arm64/async_context_arm64.S`,
  `tests/test_async_context_darwin.c`, `tests/fixtures/async_darwin_leaf.S`,
  `Makefile`, `CMakeLists.txt`, this run's evidence. Preconditions: native GPR/NEON
  flavors; unsupported SME/arm64e refused; borrowed Mach right remains owned by
  caller. No locks/allocation while target is suspended. No capability change.
  Pass: finite no-yield leaf on a separate G stack, repeated capture and native
  scheduler return, exact full-state restoration/result/mask/errno and balanced
  owned suspend increments; bad PC/SP/profile inputs do not mutate context.
  Independent primary contracts: XNU `thread_act.defs` and `doc/arm/sme.md` read;
  existing C/asm layouts inspected. Status: admitted.
- C-LFX CHANGE -> AC1/AC3/AC4: support the observed native Linux non-XSAVE
  FXSAVE signal frame. Writes limited to existing Linux context source,
  `src/internal/async_preempt_abi.h`, matching Linux async test and evidence.
  Evidence: XNU-independent Linux v6.12 `save_sw_bytes/save_xstate_epilog`
  publishes magic1 and size512/ext516 without an XSAVE header or magic2 trailer
  when !use_xsave; `__fpu_restore_sig` restores FPSSE in this case. Native probe
  matches exactly, uc_flags6 and complete CS/SS. Accept only the exact legacy
  hardware/profile combination; do not synthesize selectors, read absent XSAVE
  headers/trailer, relax modern XSAVE bounds, or remove PKRU/CET refusal.
  Pass: synthetic valid/invalid legacy format checks and actual native no-yield
  capture/restore, with previous skips preserved separately. Status: admitted.
- One active writer. Backlog: previous Windows broker EPIPE, whole-process HANDLE
  drift attribution, unrelated historical license record and allocator work.
- C3 CHANGE / ALL-2R -> AC1/AC2/AC3: new-adapter ownership protocol and real
  Darwin scheduler integration, initially fixed deterministic single-primary
  configuration; this does not satisfy the full matrix or multi-M criterion.
  Exact writes admitted: new `src/internal/async_context_shared.h` and
  `src/core/context/async_context_shared.c`; existing async-context headers/C,
  `runtime_platform.h`, `runtime_types.h`, `runtime_proto_core.h`, `runtime_internal.h`,
  `src/core/base/errno.c`, lifecycle init/run/shutdown, task alloc/stack/reclaim,
  scheduler/reinject/yield, blocking API/timer helper guards,
  `src/engine/watchdog/watchdog_worker.c`, Make/CMake, new real-path Darwin test
  and matching documentation/evidence. Preserve all existing changes; checkpoint
  affected files before edits. No new public ABI, no default-ON activation.
  Protocol candidate: completed capture transfers to G-owned READY root without
  FREE, releases short controller readers; restore takes fresh refs. Must not
  strand G1 READY behind a second certified CPU-bound G2 on the same M. Bank FREE
  still needs exact witness+readers0+no pending restore. Stop separates admission
  from accepted service. Target-side errno/mask exchange and counted deferred
  stack/finalizer roots are mandatory. No target lock/allocation during suspend.
  Pro T-ALL-03 review precedes qualification/advertisement of revised ownership.
  Tests: actual production capture→peer progress→restore; two no-yield leaves;
  error/mask isolation, repeated witness, delayed final controller access vs DEAD,
  start/stop/destroy, default behavior. Status: active. Private shared protocol,
  Darwin native provider and actual scheduler/lifecycle/retirement seams are
  written and built. Actual mixed no-yield scheduler test now passes. Initial
  qualification uses one deterministic primary and locked intrusive P queues;
  wider multi-M and platform qualification remain required for G0.

## Current disposition

- Research status: CONTINUE.
- adoption_status: NOT_EVALUATED for this all-OS extension.
- merge_readiness: NOT_READY.
- Baseline: `make -j4 CC=clang test_preempt_capabilities` PASS; executing
  `./test_preempt_capabilities` PASS (fallback matrix and state tracking).
- T-ALL-01 initial design read: common protocol/thin adapters selected; preserve
  native contexts as opaque and do not split all Linux code at once. Mach arm64
  mechanism fixture precedes runtime activation; no capability advertisement
  before the real restore oracle. Response final completion check pending.
- No cleanup performed yet in this run.
- C1 Mac default-OFF checks: `test_runtime_core`, `test_multi_runtime_core` and
  `test_preempt_capabilities` executed and passed after the seam change.
- C2 build: first fixture assembly rejected out-of-range STP/LDP immediates;
  corrected by using a separate save-area base, not by relaxing compiler checks.
  CMake Release build succeeded. `ctest --test-dir build-async-all-os
  --output-on-failure -R '^test_async_context_darwin$'` PASS, 1/1, 0.33 s.
  Observed: 8 native capture/restore/native-continuation witnesses, full GPR and
  all 32x128-bit NEON bytes, NZCV/FPCR/FPSR, errno/mask and exact finite result.
  Bad PC/SP and dead target refused. This is mechanism proof, NOT runtime async
  activation. First implementation does not yet test foreign suspend owner or
  injected partial Mach API failure, and refuses live SME/opaque arm64e profiles.
- Generated in-project build: `build-async-all-os/`, owned by this run, retained
  during verification; no source/user data in it. No cleanup before completion.
- Existing Linux test container restarted with its unchanged read-only `/work`
  bind and existing 2 CPU / 4 GiB limits. No Docker resource cleanup authorized.
- Windows read-only BatchMode authentication probe failed; password-based
  authorized session was established without saving credentials. Only current
  test binaries/harness were copied to a fresh child of the old test directory.
- Windows actual native regression: 22/22 exit 0, including 14 multi-M modes,
  four certified one-M/default-OFF modes, runtime/multi-runtime, identity and
  capability checks. Each case launched below-normal priority with sparse
  affinity0x15; pre-case load29–60%, free RAM at least13,428,056KiB. The harness
  stops before a case if load>65% or free RAM<2GiB. First harness Start-Process
  returned an unavailable exit status; preserved its logs, did not label it a
  native failure/pass. Corrected direct Process harness obtains real statuses.
- Linux Docker: runtime/multi-runtime/capabilities3 PASS; async2 SKIP because its
  signal context lacks SS. On the authorized native Debian13/x64 host (kernel
  6.12.96,1CPU,~1GiB available RAM), the same small binaries at nice19 also SKIP.
  Read-only native ABI probe resolves a DIFFERENT cause there: valid saved CS/SS,
  no OSXSAVE, magic1=0x46505853, extended_size516, xfeatures0, xstate_size512,
  uc_flags6. Existing parser rejects magic1 frames shorter than576. No predicate
  has been weakened; exact kernel-contract check and focused tests precede a fix.
- Native diagnostics/artifacts on that host: `/tmp/llam-all-os-20261026` (three
  test binaries, probe and its liburing dependency); no compiler installation or
  large load tests. Remote/Docker artifacts are outside authorized cleanup scope.
- C2 current mechanism checks after same-origin guard and failure injection:
  4/4 PASS. Foreign suspend retains its original increment; failed read, wrong
  origin/PC/SP, dead target and unknown SME query refuse before mutation. Neon,
  partial-general and resume failures terminate only isolated child with125.
  SME INVALID_ARGUMENT now counts as absent only with independently cached
  hardware sysctl proof, prepared outside suspension. Actual SME-instruction
  execution is NOT_RUN. Native Mac runtime activation remains off.
- User explicitly invoked code-style during execution; skill reread, applied
  with previously loaded common/C/verification profiles. PowerShell harness
  has no language-specific profile and follows the common rules only.
- Native Linux exact legacy-frame candidate executed successfully: synthetic
  parser checks, eighteen simultaneous per-G capsules, at least72 captures,
  exact FP/errno/mask/result, seven preparation failures, late-signal witness,
  stop/READY race and unsafe deferral. Host has no AVX/OSXSAVE; AVX and native
  multi-M remain NOT_RUN. Later guard-page512-byte test requires rerun.
- C3 first real native Mac gate: CMake Release experimental build; seven tests
  PASS (1.58s): native mechanism + three child API-failure cases; real mixed
  two-no-yield-G automatic scheduler, disabled baseline and two-run repeat.
  Trace establishes G1 capture -> READY -> capture-op end -> G2 capture before
  G1 first restore. Full GPR/32Q/FP/NZCV and separate task errno/USR2 masks match;
  debt/readers/retirement/finalizers/quota are zero, binding creates/closes match.
  This is one-M private qualification, NOT all-OS or public automatic support.
  Initial test compile error compared a void destroy API to zero; fixed test to
  use the actual API contract. Make registration updated, not executed yet.
- C3 adversary additions: source hooks hold only the real controller AFTER
  native resume or capture-op handoff, outside runtime locks; no new lifetime
  pins or manual capture. Six real-runtime tests PASS (1.54s), including
  CAPTURE-LATE-FINISH at GATE with capture reader still held,
  RESTORE-EARLY-EXIT detached G DEAD with retire debt while restore reader held,
  READY-ROOT-DRAIN with op0/pending0/READY1/execution-debt2 then stop.
  Both exact leaf results and state still pass; ledgers drain. These are bounded
  tests, not soak/latency measurements. Default-off/source-profile checks and
  startup abort, offload/isolation are next. Remaining platforms unchanged.
- C-LFX follow-up tightens only the new legacy branch to exact native SS +
  strict-SS flags6, rejecting unknown flags and missing SS semantics. Primary
  Linux v6.12 ucontext.h checked through original GitHub source after raw fetch
  timeout. Guard-page512 and malformed flags now await rebuilt native rerun.
- C-LFX rebuilt final branch passed the actual native Linux suite again, now
  including the guard-page512 buffer and unknown/missing frame-flag refusal.
  Resource precheck: load0.02, available1110MiB; serial nice19/timeout35s.
- C3 added managed offload park/wake, failed startup with never-started G storage
  cleanup, explicit unsafe-profile/invalid-range/fail-fast-ack refusal and two
  concurrently independent runtimes (one stopped while peer runs mixed leaves).
  Native Mac ten real-runtime cases PASS (1.98s). Mechanism cases were not
  repeated unchanged. Broader ABI/lifecycle/invariant gate is in progress.
- D3 DISCOVERY -> AC1/AC4: next-port environment and original Linux arm64
  signal ABI. Read-only: existing Docker image/host architectures and mounts,
  official Linux v6.12 arm64 sigcontext/ucontext/signal.c. No new containers,
  images, dependencies or product source ports admitted by this discovery.
  Decision: select an executable mechanism route after T-ALL-04 final review;
  do not force signal handlers through the synchronous remote-suspend vtable.
- MAC-CLOSE continuation -> AC2/AC3/AC4: same admitted C3 source/test allowlist.
  Release the existing bank reader before its independent task scan reader;
  no capsule access after bank release. A test-only controller pause retains
  that actual scan ref, not an extra pin, while M frees capsule/stack and owns
  counted task-backed finalizer. Add exact direct-witness/return race, pending
  signal handler (counter+errno only), and >=2 captures per original mixed G.
  Validation: forced boundaries, numerical/register state, one observation and
  zero final ledgers. Source qualification review follows actual outcomes.
- MAC-CLOSE final new gates:13/13 native runtime cases PASS,2.91s. Raw CTest
  output retained at `artifacts/mac-close-results.md`. One mixed run has57
  capture/restore/witness triples; repeated runs117, each original G>=2 captures.
  TASK-ONLY-READER and direct-WITNESS/RETURN now pass using only production refs;
  pending SIGUSR2 delivery preserves errno/mask and executes exactly once.
- T-ALL-05: large exact-source handoff packet auto-attached by ChatGPT UI;
  clicked Send after attachment completed, new user item/generation confirmed.
  Pro5/5 selected. Includes exact finish/arrival/READY/DEAD code and explicit
  descriptions of omitted functions (not claimed source-reviewed), actual new
  gates and Linux overlap correction. Complete response fully read. CHANGES
  REQUIRED: old OBSERVED restore identity must not be checked against a later
  cooperative redispatch of the same G. Require exact capture-reaper ordering
  and discard=true call sites; other shown lifetime transitions supported.
  Windows short capture transaction/G-owned continuation adaptation approved
  after preserving the concrete regression. Linux arm64 mechanism remains next.
- LEGACY-MIXED discovery: actual Linux cohort already requires six no-yield
  leaves started and none finished before releasing any leaf, plus rescue==0.
  scheduler_return clears per-M status immediately after G-owned READY publish.
  Native passing cohort therefore already proves the relevant same-M overlap.
  Windows differs: operation remains occupied through OBSERVED; controller
  admits a new capture only for empty operation and zero binding_ops. This is
  a source-backed same-M liveness defect, despite the earlier cohort's passes.
- C4 CHANGE / LEGACY-WIN-MIXED -> AC2/AC3/AC4: add same-M two-certified finite
  CPU-leaf oracle using the existing pinned LLVM/LLD LLPM DLL, no manually
  manufactured certification. Exact writes: existing
  `experiments/windows/llam_async_certified_runtime_oracle.c`, CMakeLists,
  this run's bounded Windows harness/evidence. Checkpoint captured before edits.
  Pass: G1 capture/READY -> G2 automatic capture BEFORE G2 return and G1 restore;
  both exact results and zero refs/debt. First run should expose persistent-op
  defect; do not label natural G2 completion a pass. Production Windows fixes
  require a separate admitted change after concrete protocol review.
- C4 actual native baseline FAIL reproduced: first capture0, second capture
  SIZE_MAX, first restore2, second return1, trace count5. G2 naturally completes
  before G1 restore; that does not satisfy the mixed no-yield property.
- IMPORTANT evidence correction: initial CMD START wrapper reported exit0 for
  this failing native child. Its own stderr correctly records the failure.
  Therefore earlier22/22 wrapper statuses are not reliable native exit-code
  evidence and are downgraded pending a direct-child rerun. Preserve all logs.
  Runner fixed to restrict its OWN dedicated PowerShell process before child
  creation (affinity inherited, below-normal default), then launch executable
  directly through Process. Native mixed retest now correctly reports
  directChildExit1 and harness failure; load54%,free12,377,540KiB. No runtime
  failure weakened. Microsoft SetProcessAffinityMask/CreateProcessW inheritance
  contracts independently checked. Previous nonempty stderr was only expected
  runtime-core progress output, not grounds to restore22-pass status by itself.
- MAC-CLOSE extra regression reproduced the exact later-dispatch false fail-stop:
  hold direct OBSERVED reader, same G yields/redispatches/returns under a fresh
  run generation. Before fix the native child failed (CTest,0.11s). Phase-first
  RESTORING-only validation fixes it; retest PASS,0.12s. Unexpected non-OBSERVED/
  non-RESTORING phases still fail closed. Source reaper blocks restore admission
  whenever capture_op!=NULL; only discard=true caller is CAPTURE with no native
  redirect commit, after balanced resume (or pre-owned-suspend refusal).
- C5 CHANGE / LEGACY-WIN-HANDOFF -> AC2/AC3/AC4: Pro T-ALL-05 selected typed
  capture-reader handoff at READY/GATE, fresh restore pins from protected gate,
  same native Windows context transport. Preconditions: preserved C4 failure
  and own source checkpoints. Exact writes admitted: existing
  `src/core/context/windows_async_lifetime.c`,
  `src/internal/windows_async_lifetime.h`,
  `experiments/windows/llam_async_multim_oracle.c` (affected copied-identity and
  independent-M overlap observations under the new nonpersistent READY op),
  C4 oracle/CMake/harness, matching docs/evidence.
  Do not replace compiler-certified DLL/ranges, broaden migrations, drop close
  debt or weaken uncertain-write fail-fast. Tests: new mixed PASS, prior bounded
  native cohort with corrected direct child runner, READY-root shutdown and
  late capture finish at gate. Status: admitted, product code not yet changed.
- C5 first implementation written: private operation kind CAPTURE/RESTORE;
  capture-reader completion at READY/GATE never frees saved CPU state; protected
  M pending root before assembly GATE; fresh restore scan/bank/binding refs;
  pending-slot queue guard; controller close-service return includes execution
  debt, not only occupied operations. Windows native context transport unchanged.
  Existing copied-identity test now holds actual capture controller before
  handoff, with same retained pin; no extra fabricated test reference.
  Cross build succeeded; bounded native new mixed-after gate in progress.
- T-ALL-06: Pro5/5, send confirmed; provided actual Mac later-dispatch failure /
  phase fix /21-case pass plus exact reaper gate and only discard call-flow.
  Corrected Windows harness statuses to Pro before any further acceptance.
  Response pending. Make default-off `make -j4 CC=clang all test` is running;
  own pre-Make root/object inventory retained for safe later cleanup.
- C4 failing native cohort identity: original mixed binary SHA256
  `29e6f0ecdb74ffcc8efe4aec0d96f74ca1faf00f47d802b608e5e7dae9dc64a1`;
  unchanged pinned compiler-certified DLL SHA256
  `56aa5e6d4763e421318ad1e98d8c48b19d24ebaceb3bbef61f6d813d1d706cd0`.
- C5 first actual native corrected-runner gate PASS: mixed directChildExit0,
  host load37%,free12,326,288KiB, inherited sparse affinity0x15. This follows
  the saved actual before-fix directChildExit1; not a relabelled natural finish.
  Remaining old bounded cohort needs fresh production binaries and rerun.
- C5 corrected direct-child cohort so far: capability, identity, runtime core,
  multi-runtime core and two-M positive all PASS. Overlap test FAILS its old
  assertion that two READY/active Gs require two controller operation refs.
  That is no longer the selected protocol. Do not waive the case: replace that
  internal-count assumption with actual independent G-owned continuation plus
  another M's repeated capture/restore progress while the first is held READY.
  Preserve distinct bank/native-M identity and no extra test pins.
- Updated independent-M overlap now PASS with actual M1 capture->restore->witness
  after held M0 capture, distinct native M/bank identity, M0 READY external_refs0.
  Migration, owners3 and delayed task-only-reader cases also direct-child PASS.
  Repeat oracle exposed its old test-only idle predicate reused from controller
  return value. New return must include execution debt; old-sweep pause must
  require NO native operations/READY/GATE roots, while finite Gs may still run.
  Added test-only READY-context observation count (not a pin/root) and locked
  protocol-idle predicate; production close still uses real debts. No weakened
  old-sweep token/finalizer assertions. Rebuild/rerun pending.
- T-ALL-06 complete response fully read: narrow MAC-CLOSE ACCEPT at private
  default-off one-primary scope; no full-repository or public support claim.
  Windows typed short-capture handoff approved to implement; all old statuses
  remain downgraded until real direct-child compatibility gates finish.
- C5 exact one-M adversaries added to the unchanged certified DLL oracle:
  READY ops0/pending0/debt1 followed by public stop and separately public destroy;
  late capture-controller handoff at a real fresh GATE with one actual reader.
  No manual capture or added test pin. Destroy case does not read consumed
  runtime/task handles after release; checks external exact finite result and
  module final unload after destroy thread joins. Native results pending.
- C5 all eight certified-DLL modes now direct-child PASS, including new mixed,
  exact READY op0/pending0/debt1 stop/destroy and late capture at GATE. Host
  loads4..23%,free>=16,203,404KiB. Full26-case final-source direct cohort started
  (four ordinary + fourteen multi-M + eight certified modes); no final pass
  count until actual completion. R8 root/destroy code keeps no runtime/task read
  after releasing the held root and public destroy begins consuming storage.
- Full26 cohort stopped at repeat exit78 (coverage, not a native computation or
  memory failure): old-sweep fixture paused after four GLOBAL captures, allowing
  a 3+1 per-M split although its own final oracle requires two chains on EACH M.
  Preserve the nonzero result. Injection now waits for two actual current-run
  direct-witness cycles on every M plus protocol-idle; it does not manufacture
  captures or weaken the two-chain oracle. Idle scan runs only when the tail
  hook is actually configured. Rebuilt final cohort rerun pending.
- Repeat coverage follow-up: direct-witness totals still did not ensure the
  final oracle's PEER-before-RESTORE ordering. Preserve c5_final2 exit78. Tail
  injection now waits for the EXACT existing ordered capture/peer/restore/
  witness condition on every M, using only immutable actual trace entries.
  No peer events, native operations or life pins are added by the predicate.
  Native service continues until its required evidence exists; final identity,
  numeric, close and old-sweep token checks remain unchanged. Rerun pending.
- Raw repeat log localizes remaining coverage shortage to SECOND run (first
  run already has two complete chains on both M0/M1). Its extra copied-identity
  hold plus the new separate handoff tick consumes the old finite leaf window.
  Only that second run now uses480M instead of240M iterations, with a matching
  independent unsigned reference. Other workloads remain240M. This lengthens
  a bounded short correctness case, not its timeout, limits or acceptance checks.
- C5 FINAL native direct-child cohort:26/26 exit0. Four ordinary API/identity/
  capability cases,14 multi-M modes,8 certified-DLL modes including mixed and
  three new root adversaries. Host loads0..24%,free>=15,470,740KiB, inherited
  sparse affinity0x15, below-normal dedicated parent, serial bounded cases.
  Prefix `c5_final3_` retained; earlier failures/wrapper statuses not erased.
  Correctness acceptance unchanged; repeat second call uses independently
  checked480M input to cover actual two-cycle/identity timing. Pro C5 review
  still required. All-OS status remains NOT_READY.
- T-ALL-07: Pro5/5 send confirmed after automatic text attachment completed.
  Packet contains actual26 direct-child results, all fixture adjustments and
  retained failures, exact Windows capture-handoff/restore-admission code plus
  scheduler/controller/close descriptions; asks for narrow source review, not
  full-repository approval. Raw output/hashes saved at
  `artifacts/windows-c5-results.md`. Response pending.
- T-ALL-07 complete response fully read: narrow C5 ACCEPT on supplied source
  and corrected native evidence, no further C5 source excerpt/change required.
  Protected GATE ownership justifies initial task access; new scan pins extend
  it. Advanced READY/GATE handoff and stop-root service accepted. All-OS and
  complete submission remain NOT_READY. Captured the visible final disposition
  with Pro composer for later local evidence save; no screenshot saved yet.
- D3P PROBE -> AC1/AC4: establish whether a native AArch64 static ELF can execute
  through the EXISTING x64-rootfs container on its aarch64 kernel. Scratch-only
  writes admitted: `artifacts/probe-linux-arm64.S` and this run's existing
  `build-async-all-os/probe-linux-arm64`. Use existing host Clang+LLD, no libs,
  installs, images, containers or mutable runtime code. Read primary Linux
  syscall numbers; print only machine architecture (not hostname/accounts).
  Pass: ELF AArch64 + actual Linux direct-syscall machine=aarch64; this is NOT
  signal capture, preemption or Linux-arm64 runtime qualification. Bounded run.
- D3P actual PASS, no downloads/install/container changes: existing host Clang
  with existing `/opt/homebrew/opt/lld/bin/ld.lld` emits static AArch64 ELF;
  `docker exec ... /work/build-async-all-os/probe-linux-arm64` prints
  `LLAM Linux AArch64 ELF probe: machine=aarch64`, exit0. File identifies ARM
  aarch64 ELF64. This proves an execution route on the native-ISA Linux guest,
  not native signal-frame/preemption correctness. Probe source retained.
- C6P PREPARE -> AC1/AC4: private Linux arm64 C11/pthread execution toolchain,
  prerequisite to the selected signal/fresh-frame fixture. No runtime edits.
  Exact writes: this run's `artifacts/bootstrap-arm64-sysroot.sh`,
  `artifacts/probe-linux-arm64-c.c`, and fresh owned
  `build-async-all-os/arm64-toolchain/` + corresponding probe executable.
  Existing Clang/LLD are reused. Four Ubuntu cross-libc/kernel-header/libgcc
  packages total6,816,818 compressed bytes, pinned from existing signed APT
  metadata with SHA512; HTTPS download then verify before private extraction.
  No global install, APT source/config update, Docker image/container, or new
  compiler build. Avoid extracting unused sanitizer archives; preserve package
  copyright files. Engineering footprint cap100MiB (not a user-given budget).
  Pass: a real statically linked C11 pthread/TLS errno probe executes on the
  already-proven native-ISA guest; this alone is not preemption qualification.
- C6P actual PASS with C11/Werror/static pthread/glibc: machine=aarch64,
  glibc2.39, parent/child errno isolated, ucontext4560, sigset128, exit0.
  Toolchain footprint34MiB. First compile rejected deprecated fuse-ld-path
  under Werror; switched to supported --ld-path, did not disable the warning.
  Preserving notices requires the libgcc doc symlink's46,536-byte official
  gcc-13-cross-base package; admitted that additional notice-only dependency,
  pinned with APT SHA512. No global installs or runtime-path activation.
- D3S READ-ONLY FRAME PROBE -> AC1/AC4: before writing a signal frontend,
  observe actual native aarch64 siginfo ucontext/PSTATE/typed record headers on
  an owned altstack, returning normally without modifying any saved context.
  Scratch-only writes: `artifacts/probe-linux-arm64-signal.c` and matching
  executable in existing owned build dir. Bounded record scan, lock-free
  observation fields, saved signal action/mask/altstack restored. Pass: actual
  frame shape known; not a CPU-capture/preemption/runtime integration claim.
- D3S actual read-only native frame: PSTATE0x60000000,HWCAP0xefb3ffff,
  HWCAP2=0x326181; one FPSIMD record magic0x46508001,size528 then terminator.
  Normal return and action/mask/altstack restoration, exit0. No saved context
  mutation, capture or preemption proof. Original user cloud file still unreadable.
- Saved and visually verified T7 approval screenshot `artifacts/pro-review-t7.jpg`
  (639x827,84,624bytes,SHA256ba358e0d2fcdafa3969f7a341e1b5778f68b3f68078b3eafefc79b104d90d081).
  Preserves narrow approval, remaining platforms and Pro selector. Its only
  temporary base64 transport file was deleted after lossless decode/verification.
  No other cleanup performed; in-use build/sysroot and original artifacts kept.
- User answered the missing-input question: will download docs/requirements.txt.
  Verify its actual materialization before resuming whole Make/license gates;
  do not assume the reply itself means the data is already available.
- D3P execution-route evidence extension: existing C probe gains a bounded
  `--inspect` mode so the container's parent can inspect the actual child
  `/proc/PID/exe` and mappings, alongside ELF ISA/static-link identity and daemon
  architecture. No interpreter or binfmt settings are changed. `uname` alone
  does not establish the native-ISA claim; no bare-metal claim is intended.
- D3P execution-route extension actual PASS: static ELF64/LSB/AArch64; native
  aarch64 Linux7.0.14-linuxkit daemon; parent readlink names the exact child ELF,
  whose complete mappings show its own static text/data, heap, pthread stack,
  VVAR/VDSO and main stack, with no user interpreter mapped. Actual child exit0.
  Existing /work remains read-only,2CPU/4GiB; no security setting changed.
- C6 CHANGE / LINUX-A64-MECHANISM -> AC1/AC2/AC3/AC4: implement the bounded
  signal-safe native frontend, initially mechanism-only/default-OFF. Exact
  writes: new `src/internal/async_context_linux_arm64.h`,
  `src/core/context/async_context_linux_arm64.c`,
  `src/asm/linux/arm64/async_context_arm64.S`,
  `tests/test_async_context_linux_arm64.c`,
  `tests/fixtures/async_linux_arm64_entry.S`; minimal preprocessor portability
  seam in existing Darwin leaf fixture to reuse the identical register oracle;
  Makefile/CMakeLists and this run's evidence. Existing files checkpointed.
  Handler transport has no allocation, ordinary locks, queue mutation, reclaim
  or retained ucontext pointer. Save interrupted GPR/FPSIMD/PSTATE, kernel mask
  and entry errno into pre-owned storage; redirect a genuine frame to saved M;
  restore into a NEW genuine gate signal frame; both return via OS restorer.
  Initial profile accepts FPSIMD only plus optional ESR metadata, rejects all
  unknown/duplicate/extended records and non-NZCV PSTATE before frame writes.
  Keep fresh uc_stack/uc_flags/uc_link/frame headers and reserved bytes intact.
  Pass: >=2 captures per original finite no-call invocation, independent peer
  progress off-G, exact arithmetic/all GPR+32Q/FP+flags/errno/masks, malformed
  frame no-write tests and same-thread descriptor lifetime. This is not the
  real scheduler two-no-yield-G integration gate or all-OS qualification.
- C6 first actual native PASS: static mechanism child0;8 capture/restore/later
  actual-signal witnesses and8 native-M peer actions during one finite300M
  no-call invocation. Exact result45000000150000000,31GPR+SP/32Q/NZCV/FPCR/FPSR,
  distinct M/G errno+USR2 masks, parser selective fresh-frame writes/no-write
  refusal. Tested malformed unknown/duplicate/oversized/missing/extended records
  and badPC/SP/PSTATE; guard-backed no-terminator case rejects duplicate ESR
  early, not a full4KiB successful scan. No runtime activation. Build registration
  added to both Make/CMake, default-off wrong-platform test returns77.
- T-ALL-08 Pro5/5 sent and generation confirmed: native/interpreter evidence,
  actual mechanism gate, exact restore source and detailed other transport/
  fixture descriptions, explicit incomplete runtime/all-OS coverage. Requested
  native-state/altstack/mask review and minimal pre-pinned signal-mailbox seam.
  Await complete response before selecting the production integration slice.
- C6 build: initial CMake Ninja attempt unavailable on host (no Ninja install).
  Reconfigured ONLY this run's new failed build with cmake --fresh and existing
  Unix Makefiles; no dependency install. Cross CMake target now builds Werror.
- C6 strengthened pure transport rejection tests: NULL frame/out/continuation/
  mask, skipped-SIMD M continuation, invalid snapshot and unsupported saved
  PSTATE refuse without frame mutation. Rebuilt registered cross CMake target,
  actual native child0 with8/8/8/8 again. New test/binary hashes retained in the
  mechanism report. Shared-leaf Mac regression3/3 PASS; Mac new test SKIP77.
- C6P extension / NATIVE-RUNTIME-PREREQUISITE -> AC1/AC3/AC4: obtain only the
  small official Ubuntu noble ARM64 liburing development/runtime packages into
  the already-owned private sysroot, not global install. Exact scratch writes:
  bootstrap-arm64-sysroot.sh and this run's arm64-toolchain packages/metadata/
  sysroot. Verify Ubuntu InRelease using existing installed archive keyring,
  Packages.xz against signed SHA256/size, then each .deb's published SHA256/size
  before extraction. Stream package metadata without a persistent expanded index.
  Existing100MiB footprint cap retained. Register no public capability. Pass:
  full native AArch64 ordinary runtime can build/execute before signal integration;
  refusal by existing kernel/container configuration remains NOT_RUN, not a
  reason to change security settings. No new container/image/VM or APT config.
- T-ALL-08 COMPLETE and fully read: narrow ACCEPT Linux AArch64 little-endian
  FPSIMD-only native-ISA guest mechanism, on excerpt/reported evidence, not a
  full implementation/LinuxKit-source audit. Real frontend APPROVED TO IMPLEMENT,
  not qualified. Select stable per-M sequence+state claimed mailbox with
  CAPTURE/RESTORE/WITNESS commands; pre-pinned immutable descriptors; result is
  final shared access, not signal-return proof. Actual arrival establishes READY;
  later signal/native return establishes OBSERVED. Cancel exact ARMED request or
  await CLAIMED result before reaping its underlying op; never generic-state ABA.
  Signal is only a doorbell. Signal capture sidecars override current M sampling;
  userspace masks fully initialized before copying kernel bits. Close retains
  service/debt, quiesces senders/requests, target blocks+drains private signal,
  detaches TLS before altstack/binding/process handler release. Handler epilogue
  resources outlive result. Next source review: claim/tail,cancel/reuse,arrival,
  close; tests include response-before-return,witness/return and late signal.
  No BSD/Darwin-x64/new-multi-M/all-OS acceptance. Added explicit compile-time
  little-endian/mask-layout/FPSIMD-layout assertions and old-valid-output warning.
- C6P ordinary AArch64 runtime core actual PASS: full tests through native task
  scheduler, ownership, timers, errno and AArch64 SIMD, not a compile-only gate.
  Cross-link setup initially exposed wrong sysroot nesting in libm linker script
  and cached host Mach-O archiver (empty96byte archive). Corrected ONLY owned
  CMake configuration: outer sysroot, explicit native liburing include/library
  paths, existing LLVM ar/ranlib archive-create/finish rules. No package source,
  linker script, warning or runtime contract was altered. Compile assertion first
  exposed8byte alignment padding AFTER the128byte mask reservation; corrected
  to exact native offsets40/176, not an assumed adjacent layout. Toolchain38MiB.
- C6S CHANGE / LINUX-A64-REAL -> AC1/AC2/AC3/AC4: connect the approved native
  signal transport to existing shared bank/READY/retirement ownership. Exact
  writes admitted: new `src/internal/async_context_signal.h`,
  `src/core/context/async_context_signal_linux_arm64.c`; shared header/C,
  runtime_platform.h, errno.c, scheduler.c; minimal platform selection/labels in
  existing shared arm64 real oracle (currently Darwin-named source), Make/CMake
  registrations and this run's evidence. Existing source checkpointed.
  Stable per-M stamped mailbox IDLE/ARMED/CLAIMED/RESULT; one exact bounded CAS,
  no handler retries/locks/alloc/queue/reclaim. Normal controller publishes only
  after real G/bank/binding pins; terminal result is LAST request/G/bank access.
  Dedicated initial-exec TLS routing and prewarmed errno pointer avoid dynamic
  TLS/first-use calls in the handler; audit emitted code before qualification.
  Reserved process-wide RT signal ownership is refcounted across runtimes;
  normal target binds only with unblocked signal and owned altstack, probes a
  real frame before G admission, blocks+drains/detaches before altstack close.
  Distinguish capture result from actual off-stack arrival and saved-G sidecars,
  restore result from later OBSERVED. Armed witness is canceled exactly or its
  claimed final result consumed before restore reader retirement. READY bank
  remains G-owned with idle mailbox; same-M mixed no-yield gate unchanged.
  Restore profile failure at committed GATE is terminal, not endless retry.
  Initial real-path profile remains private/default-OFF/deterministic one-primary;
  no public ABI/capability promotion or arbitrary-C interruption guarantee.
  Pass: actual two-no-yield mixed/sidecars, repeated generations, stop/destroy,
  late handler/doorbell/witness-return adversaries and ordinary platform regression;
  exact handler tail/claim/cancel/arrival/close source returned to Pro for review.
- User explicitly authorized disk/memory-limited LOCAL VMs for remaining OS
  execution (answer to environment question). Initial engineering limits:
  one active VM,1vCPU/2GiB RAM, preserve at least10GiB host free space; inspect
  installed virtualization tools and existing reusable read-only media first.
  No VM/image fleet, no changes/deletion of existing user VMs, no Docker cleanup.
  macOS-x64-on-ARM execution feasibility remains to establish; do not label
  emulated guest validation native bare-metal ISA qualification.
- C6S initial real native Linux AArch64 matrix14/14 actual child0: mixed59
  captures/restores/witnesses, repeated2runs117; default disabled,capture-late,
  restore-early-exit,READY-root stop,offload,probe-failure startup rollback,
  unsafe config/range/ack refusal,concurrent independent runtimes,task-only reader,
  direct witness/return,pendingSIGUSR2 and old-OBSERVED/cooperative redispatch.
  Every completed runtime reports exact31GPR/32Q/result/errno/mask and zero
  debt/refs/retire/finalizer/quota, balanced binding creation/close. This is not
  yet signal-specific handler-tail/late-doorbell qualification or Pro acceptance.
- New signal adversaries initially timed out waiting for an event: tests used
  one G with AUTO and no peer pressure, so the correct policy never requested
  preemption. Preserved this as fixture coverage failure, not a production
  lifetime failure or passing gate. One-G adversaries now explicitly select
  STRICT, as the prior observed-cooperative test does; real native context,
  request/claim/phase/result/retirement requirements and deadlines unchanged.
- C6S signal-specific gates now pass: capture/restore handler held AFTER RESULT
  but BEFORE normal return (no false READY/OBSERVED/altstack close); armed witness
  then actual G return+>=2 later cooperative dispatches; exact unclaimed capture
  cancellation before claim and actual late OS doorbell after binding close,
  repeated2runs,0 captures/0 restores/0 witnesses and zero ledgers. Test-only CAS
  cancellation counter confirms real withdrawals. One canceled single ordinary
  dispatch supplies1 scheduler errno check, not the original two-G minimum2;
  corrected only that case after its otherwise-zero-ledger fixture failure.
  Added pre-send stale envelope cancellation and closed NEW capture admission
  on stop/shutdown, retaining accepted READY/restore/witness service. These
  shared stop changes still need the Mac regression. Public READY-root consuming
  destroy also actual PASS; no RT/task reads after destroy consumed the handles.
- C6S dependent safety discovery/admission: a default signal disposition alone
  does not mean the RT number is unused: an existing LLAM signal wait-set owns
  numbers without installing a handler. Extend the existing signal registry's
  exclusion to the NEW Linux AArch64 frontend. Additional exact writes:
  `src/core/api/signal_api.c` (checkpointed), signal header/native frontend,
  matching conflict regression in the shared real oracle and build/evidence.
  One central claim spans process handler ownership; ordinary waiter creation
  and async selection serialize on the existing registry mutex. No handler
  takes that lock. Preserve existing waiter/owner/mask semantics and public ABI.
  Do not silently broaden this into an unreviewed rewrite of legacy Linux x64.
- C6S frozen-source final qualification: native Linux AArch64 runtime22/22
  ACTUAL child0 plus7/7 ordinary core/multi/cap/ABI/invariant/shutdown child0;
  native Mac arm64 shared regression26/26 CTest PASS8.81s (15 real-path cases,
  4 native mechanism/failfast,7 ordinary/ABI). Exact commands/raw outputs/source
  and critical binary SHA256 saved in `artifacts/linux-arm64-real-results.md`
  and `artifacts/mac-c6s-regression-results.md`. Werror CMake builds include
  both production and test-hook libraries. No Pro real-frontend approval yet.
  Existing docs/requirements.txt still compressed,dataless; no whole gate PASS.
- T9 local exact100KB source/raw-results packet saved as pro-review-t9.txt.
  Browser native file picker could not be controlled (Codex app surface denied;
  no alternate app/OS automation bypass attempted). The FULL local file was NOT
  uploaded. Instead pasted a smaller explicit excerpt/descriptive packet into
  the SAME Pro conversation, which auto-attaches text. It labels full handler
  middle/controller/lifecycle descriptions as omitted, not source-reviewed.
  Exact claim/tail,cancel/reuse,outer entry,signal-origin sidecars and native
  close supplied; asked for missing source/counterexamples or narrow C6S review.
  Attachment preparation visible; send/generation confirmation still pending.
- T-ALL-09 send CONFIRMED: pasted-excerpt auto-attachment completed, clicked Send,
  new user item `붙여넣은 텍스트(1)(3).txt` and generation visible in designated
  conversation (Pro selected). Await final response; do not claim local100KB
  packet itself delivered or whole-source approval. Continue VM discovery only
  until any critical source review conditions are resolved.
- T-ALL-09 COMPLETE and fully read: narrow C6S ACCEPT at static/private/default-
  OFF/little-endian/FPSIMD-only/one-primary/native-ISA guest scope. Excerpt-level
  review only: full100KB packet and handler middle/arm/chooser not reviewed.
  No further mailbox redesign/repeated generic test batch requested. Packed
  sequence must stop at62bits (implemented UINT64_MAX>>2). RESULT vs physical
  handler exit and sender quiescence remain separate. Draining close leaves
  errnoEAGAIN; this is not proof of post-run caller errno preservation and is
  not a demonstrated public contract regression. Check docs/wrapper narrowly.
  Pro requires separate legacy Linux x64 signal-number conflict audit, then
  carry the accepted signal frontend (not Mach locks) to BSD. No dynamic-DSO,
  SVE/SME,new-multi-M,Darwin-x64,BSD/all-OS acceptance; full gates still blocked.
- Saved/visually verified current T9 acceptance screenshot pro-review-t9.jpg,
  639x827,102196bytes,SHA2568275ee0fac8a4f2b714488fd6abf83c751fa156b556a6e7f2f9e9cdb540ee37e.
  Raw bytes matched CUA FNV9646edaa before decode; temporary .base64 removed.
- C-LSIG DISCOVERY -> AC1/AC3: read legacy Linux x64 select/install/release,
  waiter reservation predicate and post-run errno documentation/wrappers. No
  legacy production writes until exact omission and rollback obligations known.
- C7P VM-PREPARE -> AC1/AC4, user-authorized: bounded FreeBSD14.5 x86_64 ABI
  observation in full-system TCG emulation, NOT bare-metal/native-ISA hardware.
  Exact writes: research artifacts freebsd-seed/meta-data,user-data,
  probe-freebsd-x86_64.c,bootstrap-freebsd-vm.sh and owned
  build-async-all-os/bsd-vm.nosync/freebsd-14.5/ media/base/overlay/seed outputs.
  One active VM,1vCPU/2GiB, no network for initial probe, no host security/cloud
  setting changes, no existing user VM touched. Host24GiB RAM40%free,23GiB disk,
  no active QEMU/user VM found. Require10GiB host-disk floor, stage<700MB image,
  decompressed QCOW<=3GiB/virtual5GiB, monitor own overlay growth. Official
  FreeBSD BASIC-CLOUDINIT UFS image653136048bytes, SHA256a2a8cd84cd59077225a0e75ead852e8a08fe26f9ab251a96d5a4fd4fe1db4454
  from vendor HTTPS checksum. Native nuageinit supports on-disk CIDATA/script
  input; use local read-only seed/serial output, not new passwords/SSH exposure.
  Probe observes actual signal frame and returns normally, no context mutation
  or production BSD backend activation. Further porting needs distinct admission.
- C-LSIG discovery CONFIRMED: legacy install scans DFL only, never consults
  waiter reservations; waiter exclusion currently compiled only for new arm64
  frontend. All allocations/fallible preparation precede signal install; after
  successful install, runtime ownership publishes with no later failure branch.
  Shutdown is its unique release path under legacy owner mutex. No post-run
  caller-errno preservation promise found in public header/ABI/API docs; do not
  misreport drain's EAGAIN as a public regression or claim host errno preserved.
- C7P media verification: actual downloaded SHA256 matches vendor checksum.
  xz uncompressed2625699840bytes, actual allocated2637602816 (<3GiB).
  IMPORTANT actual qemu-img virtual-size6477709312bytes (~6.03GiB), NOT the5GiB
  inferred from vendor source configuration. Do not truncate the vendor disk.
  Update this owned VM's virtual ceiling to6.1GiB; monitor overlay<=1GiB and
  host free>=10GiB, RAM2GiB/1vCPU unchanged. Free host disk after unpack18.6GiB.
  New read-only seed serial probe, no network/authentication changes, created.
  Existing native Linux x64 SSH timed out before authentication; no password
  sent and no remote tests claimed. Namespace preparation oracle proceeds on
  local existing translated-x64 rootfs (not a native signal-frame proof).
- C7P actual VM observation PASS: FreeBSD14.5 kernel1405000,1CPU/2GiB,
  ucontext880/mcontext800/sigset16/fp512, machine offset16/fp224. Actual frame
  CS43/SS3b,flags1,len800,fpformat10002,ownedfp20001,no extended-state pointer/
  length. Owned altstack, interrupted USR1=blocked/USR2=unblocked, entryerrno33,
  CW37f/MXCSR1f80 and ordinary return all checked. Child0/script0, normal guest
  poweroff, overlay5.1MiB, no running QEMU after. NOT LLAM capture/restore proof.
  Vendor firstboot-update attempted without NIC and failed normally; no update
  or security setting disabled. T-ALL-10 Pro request SENT, actual generation
  confirmed same conversation, exact legacy select/release and central helpers,
  paired before/after statuses plus actual FreeBSD observation. Await design/
  namespace review; no BSD production mutation before transport admission.
- T-ALL-10 COMPLETE/fully read: C-LSIG namespace narrow ACCEPT. Normal ordering
  coherent, but required new child fault tests: install failure after claim
  preserves errno/reuse, installed-nonDFL deterministic rollback, required query/
  rollback/restore errors actually125. Foreign-action preservation is conditional,
  NOT atomic CAS or concurrent foreign mutation guarantee; corrected source
  comment. FreeBSD APPROVE TO IMPLEMENT standalone legacy-only fresh-context
  transport, NOT runtime activation: actual libthr wrapper may copy/defer context,
  normal handler return required; selectors/bases compatible native fingerprint,
  FP format+ownership tags required, native16-byte masks inclsignal>64, SI_LWP,
  red-zone and arbitrary interruptedRSP needn't16align. Initialone-G then2-G
  mechanism and source review before runtime. No BSD production write yet.
- C-LVM VALIDATE actual full-system Linux6.8.0-31 x64 guest PASS: namespacechild0
  and full changed-source legacy contextchild0 (3x6capsules/>=72captures,FP/errno/
  mask,7prep failures/retry,late witness,stopREADY,unsafe deferral). StaticELF
  a404445afbe187cba3f67b9a3c029eaad2175bcfe1aa3f7d9fa66a4f693e545e,
  initramfs62678b4d50bcd79350b5733ea3afe6fcae80d860afec84fc7dd2b5aa4ef57178.
  SignedUbuntu archive metadata+package hashes verified; allvendor notices kept.
  QEMUqemu64,+rdrand/noAVX,noXSAVE,1CPU/1GiB,noNIC/no writable disk;39MiBowned
  total,normalpoweroff/no remainingQEMU. Testphrase'supportedAVX' does NOT mean
  AVX exercised here. This is real guest-kernel ABI underTCG,NOT physicalhardware.
- C-LSIG-FAULT actual7 child cases PASS: expected0 forEIO/EINVAL/foreign rollback,
  expected125 forrequired rollback-query/rollback-restore/shutdown-query/shutdown-
  restore failures. StrictWerror oracle PASS; both namespace CTests PASS0.36s.
  Rebuilt static wrapped exactsource and repeated disklessx64 VM ONLY because
  the executable/link interception changed: namespacechild0,fault-suitechild0
  (7expectedstatuses),fulllegacycontextchild0,normalpoweroff3.76s. Source/hash/
  rawguest evidence in artifacts/linux-x64-signal-namespace-results.md. No AVX
  exercised, no baremetal/performance claim. New critical binary8574f14b...,new
  initramfs281ee853.... Own older initramfs retained as historical executed input.
  Removed only this run's temporary duplicated line logging and handler diagnostic
  storage/metadata/stdio snippets after exactSSBS root established; native parser
  and ordinary production semantics unchanged. Kept useful owner-oracle failure
  stats (ordinary test code, no predicate weakening). No source/userdata deletion.
- T-ALL-11 COMPLETE/fully read: APPROVE narrow SSBS correction. Admit ONLY
  NZCV|SSBS, capture/read exact PSTATE; redirect clears NZCV AFTER validation and
  preserves interruptedSSBS; fresh restore writes exact savedG PSTATE, no merge
  withfreshSSBS/no cachedM SSBS. REQUIRED invariant across WHOLE native thread:
  LLAM/G/peers/callbacks/signal/external code do not change speculation policy or
  manageSSBS while continuations exist; certleafalone insufficient. Do not promise
  monotonic 'no weakening' under concurrentpolicychanges (unsupported). NoPR_SET,
  MSR-SSBS,affinity/securitychanges,otherPSTATE/extension broadening. Keepterminal
  committed failure. Sourceupstream supportsrule, LinuxKitpatchesuninspected.
  Tests: copiedframe all4SSBS combinations/exactselectivewrites/old60001000 case,
  no syntheticframe returned toOS; nativepre-RESULT writeobservations and
  PR_GET on ACTUAL M before/after(errno-preserved,errorsnotpolicyfacts), whole
  22cohort/currentordinary gates, bounded loadedowner originalpredicate. Classify
  separatechild1. BSD integration paused until both resolved.
- C-SSBS current native qualification:22/22 actualruntimecases0 +7/7 ordinary/
  core/multi/cap/shutdown/invariant/ABI actual0. Explicit --isolation records
  capture93/restore93 with ONE actual freshgateSSBS1 and exactwritesPASS. Native
  corrected path executed originalformerlyfatalstate, notjustsynthetic. Existing
  trace512prefix cantruncate (repeatcasecapture129/restore127 observedprefix vs
  actual171/171 totals); do notclaimfulltracecoverage. Currentowneroracle later
  added errno/start/finishfailurediagnostics only; predicate/input unchanged.
- C-SSBS-CHILD1 classified in isolatedoldpuretransport reduction: attempt4 then
  errnoattempt3 actual1: peer0,run-1,run_errno35(EDEADLK),join_errno0,bound1,
  started1,finished1,errors0,capture0/restore0/witness0,debt0/refs0/charge0,
  binding1/1. This is runtime_run fatal, NOT justinsufficientcapturecoverage.
  sole runtimefatalEDEADLK writer is watchdog after4unchangedidle snapshots.
  Hypothesis: queue-dequeue→activeG publication, or return/drain publication gap,
  letsM-ownedRunnableG appearqueue-empty/currentNULL whileMdescheduled. Source
  helper only checksqueues+leaseactiveG, notregisteredrunnable/inflightstates.
  No productionwatchdog writes yet; rootcausefatalpointnotwitnessed yet.
- T-ALL-12 COMPLETE/fully read: SSBS correction ACCEPTED onreportedpaired+actual
  freshSSBS1 evidence, restrictedimmutablepolicyprofile. C-LSIG requestedfault
  gateclosed byactualchildstatuses. WatchdogCHANGESREQUIRED; approvedCOLDscan at
  existingstreak>=4, THREE outcomesactivity/inconclusive/parkedcandidate. Check
  actuallive-admission/listcoverage/free-order/park-wakepublication BEFOREscan.
  NEW/RUNNABLE/RUNNINGliveadmittedtasks veto; reconcileDEAD/livecounts,unknown/
  scanbudget/inconsistent =>inconclusive. Concretearrival/gate/claimedrestore
  serviceveto; NEVERgenericexecution_debt/refs/asyncenabled blanketexemption.
  Equalaggregateprogress isNOTpublicationepoch. Finalfatal mustuseexisting
  publicationexclusion, notify/wakeafterunlock; inspectcanonicalglobal/multi-
  shardorder, don'tinventlockordering. Reviewactualhelpers beforecallingwhole
  detectorfixed. WD1ordinarythenenabled realdequeuepause→recordeddecision,
  WD2clearactive→READYpause, fixedSSBSsameoracleBEFORE/AFTER. WD3genuineparked
  cycle withunchangedthreshold, inclasynclifetime-debt, externalwaiternegative.
  Originalloadedeventconsistentbutnotlocalizeduntilfatalpointrecord; don'tblock
  provencorrectiononreproducingeveryhistoricalinstance. BSDstillpaused.
- WD0 actual deterministic BEFORE reproduced on CURRENT fixedSSBS: ordinary
  eagerdequeue,ordinaryLAZYdequeue,shared-enableddequeue,actualcaptureRETURNgap
  allactual1 due torealFATAL35 at4probes. Eagerrecord live1/queue0/active0/
  listedRUNNABLE1; lazyrecord live1/listed0; returnrecord live1/RUNNING1/
  capture_op1/ready0/mailboxIDLE/debt1. MresumesONLYafteractualfatal/stopnotify
  decisionreadyrelease, finiteGcresultsexact, notsuccessfulrescue. Newtesthooks
  no production policychange, no newruntimework/progress. NativeWerrorbuild.
  FirstCMakebuild neededexplicitconfigure fornewtarget; newtarget include path
  corrected tosrc/internal after itscompileerror, no warningrelaxation. Added
  real2Gmutualmutexcycle WD3 (no priorrealcycletest found; self-locktestsarenot
  equivalent), normalNO_PREEMPTflagretainsactualasync lifetime debt2 while
  avoidingirrelevantcapturecommands; cancelcleanupreleasesownedmutexes. Diagnostic
  statesarray firstassumed5; corrected toDEAD+1 becauseBLOCKED_OPAQUE4/DEAD5,
  preservingtruncated==0 predicate. EarliercyclefixturefailureNOT runtimebug.
- T-ALL-13 COMPLETE/fullyread: APPROVEcoldpopulationreconciliation and canonical
  boundednonblocking allopaqueID→allshardID→overflow bundle. No global lifecycle
  lock,eagerlisting,dispatchhotroot orfakeprogress. P==L>0 onlyifcompleteactual
  admittedlivecounts (notliveShardcount/saturatedUINTMAX); missinglazyGc=>
  inconclusive. Unknown/budget/transitionalstates=>inconcl. Resolveropen0 isNOT
  exclusionorwaitmetadata lifetime; obtainexistingpins andTRYactualprimitive
  gate whenitcommitsgrant/removalbeforereinject. Narrowpositivequalifyuntimed
  mutexowner insidefiniteparkedset, allwaitsqueued/noresolutioninflight. Join/
  timed/IO/offload/unrecognizedwaitsINCONCL untilsourceproof, externalexemptions
  unchanged. Lockcontention releasesbundleimmediately. Concreteasyncrootmapping
  explicit(notdebt/refs). Useexistingdeferredfatal atomiclatchundergates; release
  gates/pinsbeforestopnotify; diagnosticreadyAFTERactualdecision+notification.
  Required newWD-Cnegative realresolverclaimed/closing BEFOREreinjection and
  actualmutexgrant-before-shard-publication interval ifpresent. No BSDyet.
- WD1-COLD CHANGE -> AC2/AC3/AC4, approvedT13: newproduction cold component
  src/engine/watchdog/watchdog_deadlock.c, existing watchdog_worker.c and
  Engineprivateheader; async_context.c/.h, sharedC/H andLinuxx64C/ABIheader for
  explicitlockedconcreteservicequeries (Windows usesexistingatomic slot_pending
  aftersourceaudit). Test-only boundedhooks/records inruntime_test_watchdog.h,
  scheduler.c andtargetedwait-resolution/unlock hook sites ifneeded; matching
  test_watchdog_liveness_internal.c andMake/CMake. Checkpointbeforeedits.
  Mutexowner/sourceaudited: atomicrawtaskowner, guardedwaiters/grantunderits
  existingmutex.lock; unlock POPSnode+assignsownerbeforeunlockingguard and
  waking/reinjecting. Thus service-shardlocks ALONE insufficient. Positive branch
  mustholdtheprimitiveguard, verifyNodequeued/uncompleted/armed andownerwithin
  stableparkedset (self-owner meansgrant,notclosedwait), withvalidresolver/task
  pins. Cold boundedone-shotclaims; releaseviaexisting APIs OUTSIDEalllocks.
- WD3 old-detectorpositive nowactualordinary+enabled0: real2Gmutualmutexcycle,
  decisionFATAL35 atstreak4, live2/PARKED2/mutexwaiters2, noresolver/sourceops,
  lifetimeDebt0ordinary/2enabled. Callbacksnormalcancelcleanupbothfinish0errors.
  Initial fixtureincorrectlyexpectedunmanagedjoin0afterfatal; existingjoin API
  reportsfatal35evenforcompletedG anddoesNOTconsumehandle. Correctedtest asserts
  joinfailure35 thennormalconsumingdetach0; no productionjoinchange. Frozen6case
  BEFOREoracle d51865cf71ca9477a981c21cb4d5f9bf5c8efac968c5da6ead1ccc3733236fe2,
  binaryeac92d750ca02015bdaf697141d3f1b8fc852b25f1a47a899294bd36e34605c3:
  fourfalse-fatalgapcases1, twogenuinedeadlockpositives0. Rawfunctionsstore
  wd0-before-full-log/hashes; preservebeforebinaryperPro beforecoldpolicyedit.
- T-ALL-13 SENT/currentlygenerationconfirmed (completeabove): decisive4old-detectorfatalrecords,
  lazycoverage/allocation-listmembership source, PARKED-before-resolver-open
  publication, canonicalmergeopaqueID→shardID andshard→overflow order, existing
  deferredfatal atomiconlyhelper. AskedPro toresolvefinalcoldtrylock+population
  reconciliation/wakepublication andfirst-fatalordering beforepolicyedit.
  No globalhotpathmutex/eagerlisting/policyrewrite assumed. Unknownroots/waits/
  budgetstayoutinconclusive; explicitcontractreviewpending.
- WD0 CHANGE/DIAG -> AC2/AC3/AC4, T12approved: test-only boundeddecisionrecord
  and Mpausehooks, no diagnosticsinprogresscounters/no extra productionroot.
  Exactwrites scheduler.c/testprototypes, watchdog_worker.c/Engineprivateheader,
  new tests/test_watchdog_liveness_internal.c, Make/CMake andevidence. Checkpoint
  beforeedits. FIRST reproduceoldfatal withcurrentfixedSSBS, thenimplementcold
  confirmation onlyaftermembership/publication/firstfatalhelpers areunderstood.
  Productioncoldwritesallowlistpendingactualgatedesign includeswatchdog worker/
  probe, privateheaders; no schedulerhotpathpolicychange or broadwaitrewrite.
- T-ALL-12 SENT/generationconfirmed (completeabove): supplied positiveSSBSsource/runs,namespace
  faultVMresults,exactchild1errno/state,watchdogpredicate/sourceandcandidatecold
  livenessrevalidation+deterministicnativeMpausebeforeactivepublication. Asked
  Pro toleadminimaldiagnosis/fix preserving realparkedmutex deadlock detection;
  no thresholdincrease,fakeprogress,asyncblanketexemption,unsafe taskscan/hotcost.
  Awaitdecisionbeforeproductionwatchdogchange. BSD remainsPAUSED fordependent
  correctness work (taskitselfcontinues, not a goal-tool pausedstatus).
- D-WATCH DISCOVERY -> AC2/AC3/AC4: exactaffectedreadpaths watchdog_worker.c,
  watchdog_probe.c, scheduler.c, Engineprivateheader, runtime taskstates/list/
  lifetimes and relevant invariants/tests. No additionalproductionwrites until
  deterministic defect reduction and Pro direction. Prepare bounded testonly
  publication/fatal diagnostics only if needed; preserve original fatal35.
  Own experimental observation artifacts/watchdog-queue-gap-probe.c and temporary
  build-async-all-os/ssbs-before.nosync/test_watchdog_gap_probe are admitted:
  exclusively-owned static RT/shard/task fixture, two initializedmutexes, no
  realRT/worker/publichandle. Actualproduction helperreports0 forNEW/RUNNABLE/
  RUNNING/PARKED/DEAD whenqueuesempty+leaseownerNULL. This showscurrentpredicate
  omission ONLY, not a real scheduler gap/fatal cause. No productionwrites.
- C-SSBS initial final native mechanism PASS: all4syntheticSSBS cases +8actual
  roundtrips/fullGPR/Q/errno/masks. Read-only policy query on explicit fixture
  workerM before/after=2/error0,unchanged; this is PR_SPEC_ENABLE report, NOT
  'mitigation enabled' and NOT proof rawSSBSconstant. Runtime prefix trace checks
  exact capture/redirect and saved-G restore writes keyed bytask/bank/run. Initial
  owner24/24 currentchild0, but load8s expired before latercases; not all24loaded.
  Originalchild1notexplained by thosepasses. Retain originalpredicate>=2.
- C-SSBS-DIAG dependent reduction -> AC3/AC4: isolated owned
  build-async-all-os/ssbs-before.nosync/ with checkpointed old puretransport only,
  one object/private archive copy/current diagnostic oracle relink, no live
  production-source rollback. Compare for original child1 statistics under bounded
  local20sCPUload, no profile weakening or normal-pass claim. Remove this own
  diagnostic source/object/archive/binary after preserving decisive raw evidence.
- C-SSBS paired deterministic oracle: unchanged synthetic-test source
  a4e81b49e0e7f9242813f2dbbda321c346bcb96a03e70f3a2fe0090e12a1e75f
  BEFOREpuretransporta8450d... failed134 specifically freshSSBS=1 restore;
  core dumps disabled only for this expected-failure child. BEFOREbinary92d54386...
  AFTERpuremask/redirectfix passed0, exact4SSBS/no-write+native8roundtrips,
  AFTERbinary8fd22368.... No testpredicatechange betweenpairedruns. Subsequent
  actual-M read-only policyquery and pre-RESULT native write traces added for
  finalqualification. Bounded refusal metadata in test-only nativeSignalH/sharedC
  replaces removed ad-hoc duplicatedlogging; no handlerstdio/locks/pointerroot.
- C-SSBS CHANGE -> AC1/AC3/AC4, approvedT11: exactwrites pureA64transportC/H,
  standalone mechanism test, small bounded test-only shared event/diagnostic
  declarations+implementation ifneeded, nativeM testpolicyquery+ownerstats,
  Make/CMake onlyifnewtestmode, andevidence. Checkpoint first. Productiondelta
  onlymask/redirect/comments; no mailbox/lifetime/ASM/contextlayoutchange.
- T-ALL-11 SENT/currently generating (now complete above): exact native ARM64 SSBS refusal, primary
  Linux7.0.14 kernel rules, proposed narrow NZCV|SSBS handling and need choose
  M-owned vsG-owned policy semantics before productionedit. Included newlyactual
  C-LVM positives and reopened C6S readiness. HoldBSD implementation until fixed.
- C-LSIG-FAULT CHANGE/VALIDATE -> AC3/AC4, ProT10 required: exact existingx64
  test source and Make/CMake, test-only --wrap=sigaction link interception and
  forked pre-runtime children. No new production error hooks or native-handler
  changes. Test actual production install/rollback/shutdown failures, errno/
  reusableclaim/disposition and125 children. Default-OFF/otherOS staysSKIP77.
- C6S-DIAG exact root evidence: bounded loaded diagnostic attempt8 child125,
  RESTORE error95/phaseGATE4; fresh pc294354/spffffb98eda90/PSTATE60001000,
  saved pc24fa74/spffffba91fb20/PSTATEa0000000; inline FPSIMD46508001:528 and
  terminator0:0. Fresh extra bit0x1000 is Linux arm64 PSR_SSBS_BIT (local pinned
  kernel UAPI confirms), NOT SVE/SME/unknown record/malformed SP. Current generic
  validator permits NZCV only, so rejects legitimate fresh M frame security-
  mitigation state under scheduling. Do not blindly expand PSTATE or change
  speculation-control policy. Read primary Linux sigreturn/register/SSBS rules
  and get Pro direction after T10 completes; preserve all other unknown-state
  refusals. Separate loaded child1 owner stats failure also remains to classify.
- C6S-DIAG progress: fixed42-case idle-host diagnostic batch did NOT reproduce
  original125, not considered closure. One local existing-container CPU hash
  process bounded8s reproduced125 on loaded attempt4 at shared complete's fatal
  refused-result branch. Further loaded batch showed a separate child1/no
  diagnostic (stats oracle), then another reproduced125: RESTORE kind2,error95
  ENOTSUP,bankGATE4,capture_opfalse,restore/pendingtrue. Thus genuine native
  profile refusal at committed GATE, not central signal claim failure. Preserve
  fail-stop and parser policy. Extend diagnostic allowlist to test-only native
  binding header fields and owner oracle failure output: copy bounded fresh
  general/inline-record metadata BEFORE RESULT, inspect from ordinary controller,
  no old-frame/extension pointers retained and no handler stdio/locks. Diagnose
  exact refusal before any profile broadening, and notify Pro after T10 completes.
- C-LSIG VALIDATE: focused translated-x64 CTest8/8 PASS19.48s, including
  namespace, core/multi-runtime/API-edges/connect-I/O/shutdown/invariants/caps.
  Rebuilt native arm64 namespace PASS and blocked-start PASS, but owner-isolation
  ACTUAL child125. Do NOT repeat-until-green or attribute to load without proof.
  Prior frozen C6S acceptance remains historical; current regression unresolved.
  Prioritize exact fail-stop site diagnosis before BSD production port. Added
  diagnostic-only test-hook line reporting in shared/native ordinary fail-stop
  paths is admitted C6S-DIAG -> AC3; exact shared/native C files (checkpoint first),
  bounded async-safe write only, no signal-handler locks, no contract relaxation.
- C-LSIG paired oracle: BEFORE production unchanged failed1, stolen waiter36;
  AFTER same test passed0, both orders and release/reuse. Before binary
  1bc2da3fd610169487d390121735c953e22bde017d1414cc4342465677e77059;
  after e849552d0d07f6d0c1b04699cee72cea18c61905133808727f8e450f688d3533;
  same oracle source0b7117a6327ada61eebf81db38efd1dc9ae172635de342e2011c3f7398ca776f.
  Changed3 C units strict Werror syntax PASS. Full old async actual77 due to
  translated-rootfs signal ABI rejection, NOT native pass. One accidentally
  overlapping build was interrupted; only one missing object rebuilt, then
  archive/link PASS. No source change or warning relaxation for that incident.
- C-LVM PREPARE -> AC1/AC4, within explicit bounded-local-VM authorization:
  diskless Linux x64 full-system TCG,1vCPU/1GiB,no NIC; official signed Ubuntu
  kernel+static busybox and freshly linked own static tests in private initramfs.
  Exact writes this run's linux-x64-vm/bootstrap/init artifacts and own
  build-async-all-os/bsd-vm.nosync/linux-x64/ media/initramfs (<200MiB total).
  Verify archive signature+package hashes, keep vendor notices, no host/global
  installs, no existing VM touched. Actual guest child statuses, VM0 not oracle.
  Supersedes inaccessible-server route only for emulated-kernel ABI evidence,
  not AVX/CET hardware/multi-M/performance qualification. One active VM at a time.
- C-LSIG CHANGE -> AC1/AC3/AC4: minimal shared signal-number namespace claim,
  no legacy capture/restore/ASM/lifetime rewrite. Exact writes: new private
  `src/internal/async_signal_registry.h`; existing legacy x64 context source,
  signal_api.c, signal header/native binder include; namespace two-order and
  post-release reuse regression in existing Linux x64 test/CLI; Make/CMake test
  and header registration; this run's evidence. Existing affected files saved.
  Claim before install, release on every install refusal/rollback and final
  owner shutdown, preserving errno on failure. Both waiters and old/new async
  frontends serialize on existing registry mutex; no handler locks. Keep old
  singleton runtime policy, native SS/XSTATE/CET qualification and all bank roots.
  Pass: unchanged oracle exposes BEFORE conflict, AFTER no stolen waiter/EBUSY
  reverse order/claim reuse and actual native preemption; focused ordinary I/O/
  lifecycle plus legacy async suite on bounded authorized native host. Do not
  label translator-frame skips native pass. Pro follow-up after exact evidence.
- Next-port read-only environment result: Docker kernel is native aarch64, while
  existing test rootfs/tooling is x64. Clang18 and Ubuntu signed APT metadata for
  small ARM64 cross libc/binutils packages already exist; no toolchain installed
  yet. A statically linked AArch64 executable can potentially run on the native
  ISA kernel through the existing container, without a new image/container.
  Verify this route before platform claims. Host project filesystem has22GiB
  available (98% used), so avoid VM image fleets and large compiler installs.
- License-policy gate also blocked on the SAME dataless docs/requirements.txt;
  interrupted only its own Python reader. Tests27+4 preceding that audit passed,
  but whole check-license-policy is NOT_RUN/unavailable, not PASS. Asked user
  asynchronously to materialize the existing file; no source replacement or
  cloud/security setting changes. Native implementation work continues.
- Full Make gate builds all default-off targets with Werror and export audit
  PASS (131 public symbols), then stalls in pre-existing package fixture copying
  `docs/requirements.txt`. Native metadata proves compressed,dataless,size40,
  allocatedBlocks0; Python fd5/6 are source and zero-byte fixture destination.
  The source's cloud materialization is unavailable, not a runtime test failure.
  Interrupted only this run's package Python with SIGINT; source untouched.
  Full Make gate cannot be claimed PASS until this input is readable.
