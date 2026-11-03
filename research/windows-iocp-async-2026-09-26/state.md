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

# Windows IOCP and async preemption integration, 2026-09-26

- Delivery target: INTEGRATE in `/Users/feral/Desktop/Programming/LLAM`.
- Current R8 adoption status: **experimentally integrated** only for the
  pinned LLVM 23.1.2 extension, qualified LLD `/opt:ref /opt:noicf`, WinGNU
  `scalar-leaf-v1`, one-primary-M expert-opt-in profile. Clean compiler
  reconstruction, final native Windows CTest and affected Linux tooling
  regression passed; T-PRO-WIN-40 accepted that narrow classification from
  our reported evidence. This is not release/general-Windows support,
  arbitrary-caller safety, a performance claim, or a commit/PR.
- User goal: locate measurable Windows runtime overhead by separating raw IOCP,
  LLAM I/O engine, completion-to-task wake, and end-to-end workloads; implement
  an opt-in Windows async-preemption backend and validate it on Windows.
- Acceptance: preserve the public C ABI and existing Windows I/O and lifecycle
  contracts; report a matched baseline and candidate comparison with raw samples;
  execute relevant native Windows correctness checks and document unsupported
  context/CPU/security modes. Do not claim a speedup without matched evidence.
- Host constraint: short, bounded tests on the user-provided Windows 10 x64 host.
  Initial observation: Ryzen 7 5800X3D, 16 logical CPUs, 32,677 MiB RAM with
  10,689 MiB free, about 27% CPU load, about 70.6 GiB free on C:. Interactive
  applications are active, including a roughly 5.9 GiB game process. Avoid
  sustained stress and use low build/test concurrency.
- Local starting state: `main` at `34dcc74a7e3cbb584967f6bf5f7816673ed5169b`;
  415 changed or untracked paths predate this task. Preserve these changes.
- Codex model: GPT-6 family, not confirmed as Astra. Researcher profile: general.
- ChatGPT model: UI in the specified existing in-app-browser conversation shows
  `6 Pro` selected on 2026-09-26. Official OpenAI model/reasoning guidance
  documents GPT-6 as the current family with Pro reasoning mode; the account's
  actual selection is the UI evidence. Conversation: existing IAB tab 1.
- Later in the same conversation the UI showed Pro disabled and Extra High
  selected. The user explicitly replied "Extra High로 계속해". Turns after
  `T-PRO-WIN-13` are therefore recorded as Extra High reviews, not Pro approval.
- Research modes: engineering hypothesis discrimination, bounded empirical
  comparison, and project integration. Pro leads priorities and design judgment;
  Codex owns source inspection, implementation, native execution, and checks.
- No external deployment, commit, push, release, or host system configuration
  change is part of this task. No credential is recorded in research files.

## Pro exchange

- `T-PRO-WIN-01`, content `kickoff-windows-baseline-preempt-v1`: send confirmed
  in IAB tab 1 on 2026-09-26; complete response read. Shared interface facts
  and task constraints only, without credentials or the host address. Pro's
  decision: proceed with separately gated A/B/C/D instrumentation and a
  Windows x64 native context-conformance fixture. No performance bottleneck or
  optimization has been established. Pro provisionally selected a serialized
  remote-suspension transaction for a qualified LLAM primary M, with full-state
  restoration through a controlled origin-M rendezvous; implementation depends
  on native state, mitigation, lock and lifetime checks. Ordinary APCs are not
  a busy-loop solution; special APCs are unsupported on the test host's Windows
  10 and introduce unsynchronized reentrancy.

## Current task batch

- WIN-01 Codex: same-protocol A/B/C/D Windows I/O measurements with private
  accounting and bounded repeated trials; determine whether any stage dominates.
- WIN-02 Codex: only after WIN-01, isolate one measured optimization and compare
  unchanged/candidate trials without lifetime or tail-latency regression.
- WIN-03 Codex: isolated native thread-context conformance fixture with full
  state, suspend-balance, mitigations and failure-path checks.
- WIN-04 Pro: review the returned transaction/lifetime trace after WIN-03.
- WIN-05 Codex: integrate a private Windows backend only if WIN-03/04 qualify it;
  native one-M, then two-M correctness and lifecycle tests.
- WIN-06 Codex: final combined build, ABI/export/stat, regression and bounded
  workload checks. Performance and async-preemption verdicts remain separate.

## Baseline execution

- Working-tree source snapshot transferred into a new, isolated Windows test
  folder. Main archive SHA-256:
  `126d752f3161793da1a87723d85ef115bc2def6fc21d7f2156232cf87b950e99`.
  Tooling/experiment overlay SHA-256:
  `7078e5b25e8ed2bdb930204228993f0d8f0a6ffff0e6f6938a2264d46bfba963`.
- Native baseline configuration: CMake 4.2.1, Ninja 1.13.2, MinGW GCC 15.1.0,
  Release, `LLAM_ENABLE_WINDOWS_BACKEND=ON`. Build capped at two jobs; targeted
  `bench` and four Windows test binaries linked. The PowerShell wrapper did not
  print a numeric build exit, so the linked binaries and subsequent CTest run
  are the authoritative confirmation.
- Serial CTest with a 60-second per-test cap passed 4/4:
  `test_windows_policy`, `test_windows_runtime_smoke`, `test_windows_iocp_io`,
  and `test_windows_handle_io` (0.63 seconds total).
- A short existing `bench.exe` diagnostic ran 3 measured rounds with one warmup,
  256 one-byte loopback echo exchanges per round, dynamic workers off, 16
  runtime workers: 214,429 ops/s and p50 round 1,196.6 microseconds. A separate
  channel case gave 930,937 ops/s with 1,024 exchanges per round. These are
  shared-host smoke observations, not a matched A/B/C/D comparison or a claim
  that IOCP is the measured path; current public `io_submits` remained zero.
- WIN-03 prototype in `experiments/windows/context_conformance.c/.S` compiled
  under MinGW GCC 15 with `-Werror`. On native Windows 10 x64, a controlled
  remote suspend/capture, alternate-stack landing, restore gate, and remote
  full-context restore returned exit 0. The sentinel check passed for RAX,
  YMM0 including upper AVX state, MXCSR, `errno`, and `GetLastError`.
  Enabled and captured-valid XSTATE masks were both `0x807`. This is an
  isolated one-thread conformance result, not a connected runtime backend.

## Second Pro exchange and native gates

- `T-PRO-WIN-02`, content `native-context-conformance-v1`: send confirmed in
  IAB tab 1 and complete response read. Pro judged the reported one-worker
  mechanism feasible while keeping WIN-03 conformance OPEN. Its required order:
  mitigation/context-IP classification; suspension ownership and failure
  balancing; landing stack/ABI/unwind; complete declared register/vector state.
- The native fixture now also passes an unsafe-PC refusal and a controlled
  second-suspender count test. A subsequent run reported process shadow-stack
  policy flags `0x100`, CFG flags `0x0`, and landing execution outside the
  system-allocated thread-stack limits; it still returned the sentinel match.
  Microsoft defines `0x100` as `CetDynamicApisOutOfProcOnly`, not enabled shadow
  stacks or context-IP validation. The context's enabled and valid XSTATE mask
  `0x807` includes CET_U component bit 11; its treatment remains open.
- Pro explicitly said that outside-system-stack execution is not by itself a
  failure, but stack probing, unwind metadata, local exception handling and
  target-thread bookkeeping need direct checks. The isolated fixture has not
  yet proved all nonvolatile GPR/XMM state or LLAM controller-operation
  lifetime after making the target runnable. No backend integration is approved.
- Later fixture revisions passed CET_U 16-byte CopyContext and post-restore
  comparison, a static assembly landing with x64 unwind metadata, a three-page
  stack probe/canary, landing-body and restore-gate `RtlVirtualUnwind` checks,
  and deliberate clobber/restoration of nonvolatile GPRs, XMM6-XMM15, upper
  YMM state, MXCSR and x87 control. A negative RBX corruption produced the
  expected exit 1. These remain isolated one-worker tests.
- WIN-03-STACK has a concrete failure: the same local C++ throw/catch helper
  returned 42 on the original worker stack but terminated the process with
  `0x20474343` when invoked on the manually allocated landing stack, after
  CET_U copy and before landing-ready publication. The bounded test process
  exited; the host and LLAM runtime were not modified. The cause is not yet
  isolated. Do not connect the alternate-stack landing backend on this evidence.
- Next discriminating check: compare the same helper on a Windows-managed Fiber
  stack and, if needed, model G execution on a private stack while returning to
  the M's native system stack. WIN-01 performance attribution remains open.

## Third Pro exchange and differential controls

- `T-PRO-WIN-03`, content `landing-exception-counterexample-v1`: send confirmed
  in the same IAB conversation; complete response read. Pro's decision was
  CHANGES REQUIRED: choose a saved, live native M scheduler activation for C
  landing and restore rendezvous. A hand-allocated C landing remains failed.
  The native-stack toy is not the real scheduler continuation, so WIN-03 remains
  OPEN and WIN-05 blocked. Pro requested a genuine Fiber control, an async-off
  test of actual LLAM G stacks, then a native-M landing fixture with exact
  continuation generation and G-local exception checks.
- Fiber control: the unchanged local C++ throw/catch helper returned 42 on a
  Windows-created Fiber stack, which `GetCurrentThreadStackLimits` classified
  inside its current bounds. Direct exit code 0.
- Same-process conformance A/B: G safe leaf on a private manually allocated
  stack in both modes. Native M-stack landing with C++ helper and full register/
  XSTATE restore exited 0; separate hand-allocated C landing mode exited
  `0x20474343`. This isolates the landing-stack choice in the ad-hoc fixture,
  but it is still not LLAM's saved scheduler continuation.
- Actual LLAM async-off G-stack control, deterministic one-worker Release
  library with MinGW GCC 15: the helper on native M returned 42; the first
  helper call inside the spawned G exited `0x20474343` before a yield. The same
  local throw/catch fails on LLAM's current Windows custom G stack even without
  the new preemption path. The public project description is a C11 runtime and
  does not explicitly promise C++ local exceptions, so classify this as an
  observed MinGW configuration limitation pending contract review, not yet a
  newly introduced async regression.
- WIN-01 A/B/C/D attribution and any performance optimization remain open.

## Fourth Pro exchange

- `T-PRO-WIN-04`, content `fiber-native-g0-discrimination-v1`: send confirmed in
  the same IAB conversation; complete response read. Pro approved only a
  guarded experimental Windows x64 backend for manually certified, non-throwing
  managed C execution using LLAM's *real saved native M scheduler continuation*.
  The hand-allocated C landing remains excluded. The actual LLAM G-local
  exception test is a separately retained async-off failure on this MinGW
  configuration; it does not establish a new async regression or broad
  exception support. WIN-03 remains open, and WIN-05 has implementation
  direction but is not accepted. The next conformance gates are real native M
  activation, scheduler-return generation turnover, full state preservation
  across scheduler work, and controller/worker shutdown lifetime.

## Matched-operation preliminary IOCP comparison

- Added two standalone experiments under `experiments/windows/`: raw IOCP A
  (`bench_iocp_raw.c`) and public LLAM D (`bench_iocp_full.c`). Both use 256
  one-credit, preconnected, 1 KiB TCP echo records per round; the first of four
  rounds is warmup. On Windows 10 with the runtime's default cooperative I/O
  yield, D reported zero IOCP submissions and completions. Forcing
  `LLAM_IO_COOP_YIELD=0` in a fresh process produced exactly 512 submissions
  and 512 completions per round. The 64-entry GQCSEx capacity is not a
  fill-before-return requirement.
- Raw A originally used four overlapped operations per record, unlike D's
  observed two reads via IOCP and two direct sends. Its first measurements
  were not comparable. An explicit raw reads-only/direct-send mode now yields
  512 submissions, 512 completions, and 512 direct sends per round, matching
  D's observed operation mix. Its measured rounds were 32,751, 32,377 and
  32,629 records/s; D forced-IOCP rounds immediately afterward were 24,403,
  24,198 and 23,584 records/s. Both runs completed under a 10-second limit
  at BelowNormal process priority. This is a short, shared-host preliminary
  A-vs-D observation, not additive B/C stage attribution or proof of a
  specific bottleneck. Raw A had exactly one completion per GQCSEx call in
  one-credit mode; this workload cannot demonstrate a batching defect.
- B now exercises the real LLAM IOCP node submission/completion code with a
  testhooks-only final completion ACK and `task=NULL`, so no managed G is
  reinjected. It reported 31,206, 31,328 and 30,523 records/s, with 512
  submissions, completions and direct sends per round. The ACK is emitted
  after the OVERLAPPED op is freed, so the benchmark can safely reuse its
  request storage. C uses the public two-G path and the same I/O mix but a
  minimal continuation without payload validation; it reported 23,131,
  24,709 and 24,223 records/s, 512 managed parks/wakes and 514 context
  switches per round. Re-run D with the same experiment binary reported
  24,256, 24,322 and 24,317 records/s. A/B/C/D are not algebraically
  additive because their driving/waiting mechanics differ; this is evidence
  that the larger end-to-end gap appears when managed wake/scheduling enters,
  not evidence yet of one exact lock or instruction bottleneck.
- A diagnostic D run enabled wake/run timing. Its fourth round was a 2x
  latency outlier under shared-host load. That round's node reported 511
  submit batches, 512 entries, max submit batch 2, completion depth 1 and
  max depth 2; no skip-on-success handles or immediate completions. The shard
  reported 512 parks/wakes, accumulated completion latency 18.37 ms across
  512 samples and wake latency 5.49 ms across 514 samples. These overlapping
  observations cannot be subtracted into stage costs, and the 2x outlier
  cautions against a speedup claim from three short rounds.
- After the test-only completion sink was changed to cache its flag pointer
  before publishing READY (so a quickly resumed G cannot make a post-wake
  request read stale), all four binaries were rebuilt with self-assigned
  BelowNormal priority and run in A/B/C/D/D/C/B/A order. Every 256-record
  measured round maintained 512 IOCP completions; A and B also had 512 direct
  sends, C and D had 514 task context switches. Six measured rounds per layer
  gave median records/s of A 31,137, B 30,468, C 23,483, D 21,430. The
  ranges were wide on the shared host (A 21,744–32,644; B 28,956–31,170;
  C 21,403–24,649; D 19,188–23,603). The stronger observation is that B
  generally tracks A more closely than C/D, pointing the next measurement
  toward task-bearing completion/wake/execution. These tests still cannot
  assign an additive cost to a particular stage or justify an optimization.
  Raw outputs remain in the isolated host's `build-baseline/layers-final/`.
  The paired layer and candidate-comparison logs, final CTest log, and selected
  native oracle/context diagnostics are also copied under `host-logs/` beside
  this report so the measurements can be inspected without SSH access.
- `T-PRO-WIN-05`, source-level production-path review, completed. Pro accepted
  a live scheduler-return approach in principle but found two concrete traps:
  `llam_ctx_switch` saves before restoring, so an async bridge needs a separate
  restore-only assembly entry and must not rewrite `G.ctx`; and current return
  processing restores scheduler errno/clears active G before the Linux async
  post-return hook, so Windows must classify arrival and capture target-local
  error state at the first native return seam. It also requires a completed
  context-save/dispatch association and a new still-live native continuation
  for every remote resume. No production Windows async backend is connected.
- `T-PRO-WIN-06`, B/C results and an atomic-metrics candidate, sent in the
  same conversation; response read. Pro approved measuring, not adopting, the
  removal of the first metrics-only shard lock. It cautioned that even atomic
  counters can lose a previously mutex-coherent snapshot, and requested a
  completion-entry-to-READY interval before claiming a causal bottleneck.
- The atomic-metrics candidate was built in an isolated Windows CMake tree
  with `-DLLAM_WINDOWS_TEST_ATOMIC_COMPLETION_METRICS=1`, interleaved with
  unchanged baseline runs. Twelve measured 256-record D samples per build
  did not show a benefit beyond shared-host variation; the candidate is
  rejected and its conditional source branch was removed. The default Windows
  completion path and public ABI were not changed.
- A short scheduler-only Windows benchmark measured roughly 550 ns/yield in
  one- and two-G deterministic runs (10k/20k yields per round). The existing
  `LLAM_WINDOWS_UNSAFE_SKIP_TASK_SIMD=1` ceiling setting did not show a
  consistent improvement and was not adopted. These per-yield timings cannot
  be subtracted from the A/B/C/D I/O times.
- The Windows scheduler probe's save-and-gate/restore-only assembly now lives
  in `src/asm/windows/x86_64/windows_async_probe_gate.S`, linked only into the
  testhooks library. The ordinary `windows_context_x86_64.S` switch is unchanged
  by the probe, so production context-switch hot-path cost is not affected.

## Real Windows scheduler qualification probe

- Added a testhooks-only probe connected to the actual Release LLAM dispatcher
  and its Windows context assembly; normal builds do not enable async capture.
  A controller suspends a primary M while G1 is inside a certified no-call
  assembly leaf, captures full `CONTEXT_ALL | CONTEXT_XSTATE`, redirects through
  a new restore-only assembly entry to the existing saved native scheduler
  continuation, classifies target-local errno/LastError at the first return
  seam, runs a native-stack local C++ throw/catch+destructor oracle, and
  requeues G1. G2 progresses while G1 is suspended. Redispatch saves a new
  live native scheduler return into an assembly gate; the controller restores
  the saved G CONTEXT from that gate. G1 performs a genuine cooperative yield,
  then repeats capture/restore on a different dispatch generation and exits.
- The CMake opt-in `LLAM_WINDOWS_ASYNC_SCHEDULER_ORACLE=ON` builds a standalone
  CTest target. On Windows 10 x64 / MinGW GCC 15 Release, the two-cycle oracle
  passed a focused CTest run and 10 consecutive serial repetitions (0.11–0.12
  seconds each). Capture generations were 1 then 4; native resume generations
  were 3 then 5 in one retained run. The native exception callback returned
  42 before, during each arrival, and after the run, with four destructor
  executions. Existing Windows focused tests passed 6/6 after building the
  previously absent `test_windows_iocp_dump` binary. The first broader CTest
  attempt was 5 passed/1 not-run solely because that target had not yet been
  built; rerun was 6/6.
- The oracle's first attempts timed out because the test compared a public
  encoded task handle to an internal active-G pointer. Resolving and promptly
  ending the public-op pin fixed that test error. A second initial refusal
  came from incorrectly requiring XSTATE's per-context valid mask to equal
  every enabled feature; the safe leaf left AVX absent from its valid mask.
  Requiring a supported subset instead made the two-cycle test pass. These
  failed attempts did not pass the runtime qualification gate.
- This remains a TEST-ONLY scheduler mechanism, not the requested production
  Windows async backend. It has not exercised multithreaded M ownership,
  stop/close races, delayed controller final access, injected stale-generation
  refusal, or deliberate vector/FP clobber through the real scheduler path.
  `T-PRO-WIN-07` with this result was sent for review; response pending.
  After isolating its assembly from the ordinary switch, the two-cycle CTest
  oracle again passed 10/10 serial repetitions.
- `T-PRO-WIN-07` response read. Pro accepted the actual native M return path
  (WIN-03-REAL-1) and the positive two-generation turnover sequence of
  WIN-03-REAL-2 at fixture scope. WIN-03 remains open and no production opt-in
  was approved. Pro identified three next gates on this same one-M oracle:
  stale/wrong-G/old-incarnation refusal before target mutation, two distinct
  G capsules held simultaneously with full register/vector/FP/error state,
  and controller-final-access versus task retirement/close/rebind races.
- Added three deliberately stale gate checks (wrong G, wrong restore run and
  mismatched M incarnation) to the first-cycle controller, each using a
  balanced suspend/GetThreadContext/resume without calling SetThreadContext.
  The valid controller then restored the still-spinning gate. The two-cycle
  CTest passed 10/10 serial repetitions with exactly four legitimate context
  mutations and three rejected probes. This is N1 synthetic negative coverage;
  it does not yet exercise a genuinely retired prior M incarnation.
- A second opt-in CTest oracle now captures two distinct Gs on the same M
  before restoring either. G1's READY publication is held off-queue so G2 can
  enter its independently certified leaf; only then are both requeued. Their
  separate `CONTEXT_ALL|CONTEXT_XSTATE` buffers, G.ctx snapshots and error
  sentinels are checked, with assembly recording RAX/RBX, lower/upper YMM0 and
  YMM6 lanes, MXCSR and x87 CW before C can overwrite volatile registers.
  Native scheduler MXCSR/CW and errno are checked on each capture return and
  each later G exit. The positive two-capsule test passed 10/10 short repeats.
  An environment-gated one-bit corruption of saved RBX produced exit 1, as
  expected, rather than a false pass. This is a tested register subset, not
  every supported register/vector component.
- In that same oracle, G1 is detached. The controller acquires a task scan
  reference while G1 is safely suspended and retains it through remote
  restoration. A test-only hook in the actual task-reclaim wait publishes when
  the scheduler encounters that positive reference. The controller waits for
  this event, performs its final capsule access, then releases the reference;
  the test passed 10/10. This proves the task-storage reclamation barrier in
  the fixture, not protection for hypothetical separately allocated production
  capsules or M-controller records.
- Bounded one-M stop-request variants at first capture-ready, restore-gate
  readiness and immediately after a successful remote resume each passed the
  two-capsule CTest. Concurrent public destroy/shutdown and a new caller-M
  incarnation are still untested. `T-PRO-WIN-08` was sent with N1/N2/retirement
  results; response pending.
- `T-PRO-WIN-08` response read. Pro accepted the fixture-scope N1 identity
  refusals, N2 simultaneous capsules and the genuine G-task reclaim overlap,
  while keeping WIN-03 OPEN. Existing `scan_refs` can be the controller
  operation's task-object pin, but it does not automatically protect a
  separately retired capsule bank, G stack, M arena or native binding. Acquire
  under a protected publication protocol before suspension when possible;
  never wait for run-end or worker exit before dropping a reference whose
  release the worker needs to progress. Do not promote the harness-owned
  storage/lifetime strategy as a production backend.
- Added a second run on a new native caller thread using the same default
  runtime and primary slot after the two-G run drains. A third G holds in the
  same assembly leaf while a bounded suspension checks that binding
  incarnation and run generation advanced and previously copied return
  generation cannot be treated as current. It balances the suspension without
  SetThreadContext; the new G then finishes and the runtime drains. This is a
  test-only old-identity check, not delayed operation service by a production
  controller. The CTest passed.
- Registered three opt-in CTest cases for stop requests at capture-ready,
  first restore gate and immediately after remote resume; all three pass
  with the later rebind check included. Final focused native Windows tests
  passed 7/7 before adding those three cases; the five async-specific tests
  then passed 5/5. The normal static Windows runtime archive contained no
  `llam_windows_async*` symbols, while the testhooks archive did. Disassembly
  of the `-O3` testhooks code showed an ordinary call to the save-and-gate
  routine followed by a live epilogue, without a `noreturn` assumption. The
  gate saves FP/nonvolatile/RSP state before the phase store; admitted spin
  labels begin after that store.
- Still open for production: independent capsule-bank/stack/M-operation
  retirement and quota; full declared volatile/nonvolatile/XSTATE coverage
  through the real path; public destroy/shutdown at each boundary; actual
  delayed controller operation against a re-bound worker; and an opt-in
  implementation with its own controller, safe-range admission and cleanup.
  The current code intentionally does **not** enable Windows async preemption.
- `T-PRO-WIN-09` was sent in the same specified Pro conversation to design the
  restricted one-M backend's task/capsule/stack/M/handle lifetime and close
  transaction. Complete response read. Pro approved *implementing* the next
  restricted backend step, not enabling or accepting it yet. The selected
  one-M model is existing task `scan_refs` plus an external-operation reference
  and generation/phase for each of two preallocated CONTEXT/XSTATE banks,
  protected by the existing run/M-binding lifetime gate. A nonblocking
  `task_release_stack()` barrier must return DEFERRED before capsule destruction
  or G-stack detachment if any exact bank/controller/restore use remains. The
  scheduler services deferred retirements while the controller remains live;
  it must not wait for the controller while holding a reference or M progress
  that controller needs. `run_end` drains executable/deferred obligations,
  closes binding admission, waits for outstanding operations without required
  locks, then releases M storage/handle and publishes run completion.
- Pro's proposed capture transaction acquires the task scan reference, FREE
  bank reservation and exact binding-operation reference under the existing
  protected active-G publication order *before* `SuspendThread`, then drops
  runtime locks, suspends, validates the actual PC/SP/generation, mutates only
  qualified context, balances suspension and releases its references after
  final controller access. Pre-mutation refusals immediately roll back banks;
  uncertain post-mutation outcomes remain fatal/unresolved, never replayed.
  Successful restore banks retire only after exact target reentry is observed
  **and** all external readers are done. The test-only fixture currently lacks
  these production-owned bank and release-barrier paths.
- Next implementation checkpoint: add private Windows control/capsule types,
  the early nonblocking stack-release/deferred-reclaim barrier, acquire-before-
  suspend controller operations, real one-M bind/run/stop drain, and reuse the
  existing native oracles against those backend-owned resources. The detached
  G oracle must observe DEFERRED with its bank/quota/stack intact until the
  controller's last access, then exactly-once retirement. Reject unqualified
  multi-M and mitigation modes before dispatch. Public ABI/default behavior
  must stay unchanged. WIN-03/WIN-05 remain open until this passes.
- Final focused Windows CTest, after the no-post-READY-dereference fix for the
  benchmark completion sink, passed 10/10 (two async oracles, three stop-phase
  variants, policy, runtime smoke, IOCP I/O and dump, HANDLE I/O). macOS
  `test_runtime_core` passed; `make check-license-policy` and `git diff --check`
  passed. A normal Windows static archive symbol scan found no
  `llam_windows_async*` symbols; they reside only in the testhooks archive.

## Continued implementation: production-owned lifetime slice

- `T-PRO-WIN-10` approved implementing R1–R4 as one inseparable lifetime
  slice: backend-owned preallocated banks and acquire-before-suspend refs,
  first-dispatch execution debt, complete DEAD retirement and nonblocking
  claimed finalization, pre-unbind drain, then a native oracle. It explicitly
  rejected changing only `task_release_stack()` to return a boolean or
  queueing debt after the last live-task decrement. This guidance caused the
  tentative boolean signature edit to be reverted before implementation.
- `LLAM_EXPERIMENTAL_ASYNC_PREEMPT=ON` on Win64/MinGW now compiles
  `src/core/context/windows_async_lifetime.c`. It has a private enable hook,
  two CONTEXT/XSTATE banks per G, a 64 MiB capsule budget, exact task/bank/
  binding operation references, first-dispatch execution debt, early
  nonblocking DEAD-task retirement, a separately counted claimed-finalizer
  list, and binding close before the borrowed M unbinds. No periodic request
  policy, public API, or default-path activation was added. The old test-only
  native save/restore gate remains the explicitly driven mechanism.
- Native Windows 10 / MinGW GCC 15.1 Release in an isolated `build-lifetime`
  tree built with two jobs. The new oracle exercised a detached last G,
  delayed post-restore controller bank read, DEFERRED with stack/quota intact,
  eventual release/reclaim, a runnable peer during defer, an extra task-only
  scan reader after bank release, stop during defer, four repeated
  pre-mutation refusals, and a second run on a different caller M. A separate
  never-started shutdown test passed. The main lifetime oracle passed 20
  serial repeats; the original scheduler and two-capsule oracles remained
  passing. Windows policy/smoke, IOCP I/O/dump, HANDLE I/O, runtime core and
  multi-runtime core focused CTests also passed in this opt-in build.
- `T-PRO-WIN-11` found three handoff risks from the reported protocol:
  publishing reclaim readiness before the scheduler's final G access,
  dropping the final binding ref before notifying the M, and making a refused
  bank FREE while its external ref remained. It did not accept the slice yet.
  The first was repaired with a short scheduler task scan pin spanning
  readiness and detached-claim processing. The second now performs the
  binding decrement and `llam_kick_shard()` under the same
  `opaque_lock -> shard->lock` gate; generic post-ref-release wake hooks were
  removed. The idle path honors retirement debt and uses the existing bounded
  10 ms wait fallback for task-only finalizers. The third already required
  `FREE && external_ops==0` under the admission/retirement gate; a concurrent
  G-exit/pre-mutation-refusal oracle was added to force that overlap.
- A testhooks-only gate at the first consumable `reclaim_ready` publication
  forced a real host consuming join to win the reclaim claim while the M still
  held its short task pin. It could not finish/free G until the M released the
  pin. This ready-race CTest passed 20 serial repetitions. The seven lifetime
  cases and core native Windows checks passed after the repairs.
- `make -j4 test` initially failed only at the repository's static lease
  boundary checker: its lexical parser rejected new direct `active_g` reads
  and lock receivers. The direct reads were replaced with the approved worker
  helper and the local lock receiver was made lexically unambiguous. A rerun
  passed the full macOS test gate, including ABI/export audits and runtime
  stress/fuzz; `make check-license-policy` and `git diff --check` passed.
- `T-PRO-WIN-12` accepted the three reported normal-path handoff repairs at
  protocol level, but required an explicit process-terminal disposition for
  uncertain remote context/suspension outcomes. It selected Windows
  `RaiseFailFastException`, not a CRT `abort()` or a stranded fatal flag, and
  asked for parent/child tests that distinguish the precise fail-fast exit
  from assertions, access violations and timeouts. Periodic preemption and
  WIN-03/WIN-05 remained open.
- Implemented the private nonreturning fail-fast branch. The actual lifetime
  oracle child succeeds at remote `SetThreadContext` capture or restore and
  then fault-injects lost controller certainty into production `op_finish`;
  it does not perform the termination itself. A parent process required exact
  `0xC0000602` status for both children and exit 0 for the same-runtime
  pre-mutation-refusal/valid-restore control. Native Windows CTest passed in
  0.35 seconds with no lingering `WerFault.exe` observed. Only this opt-in
  internal path has the process-terminal contract.
- During the final opt-in Windows matrix, `test_runtime_core` timed out twice
  while async lifetime was not enabled. Root cause: the new scheduler handoff
  scan pin was taken for every Windows task, while ordinary detached reclaim
  still synchronously waited for all scan refs on that same M. The wrapper
  now dispatches tasks **without** a Windows capsule through the unchanged
  release/readiness/reclaim path. After this repair, the native 17-case
  focused CTest matrix passed 17/17, including runtime core, multi-runtime
  core, IOCP/HANDLE tests, prior async oracles and the fail-fast parent.
  The final CTest log is retained under `host-logs/`.
- `T-PRO-WIN-13` accepted R1–R4 for the explicitly driven, one-M Win64/MinGW
  lifetime slice behind `LLAM_EXPERIMENTAL_ASYNC_PREEMPT=ON`. Pro judged the
  normal-path handoffs and process-terminal uncertain-outcome disposition
  closed at the reported protocol/test scope; it did not independently read
  source or run tests. Its acceptance does **not** cover a periodic requester,
  automatic Windows preemption, all context/XSTATE combinations, public
  close/destroy races or multi-M operation lifetimes. The final native focused
  matrix passed 17/17 after the disabled-path fix. Windows process
  fail-fast for an unrecoverable committed context transaction is documented
  in `docs/build.md` and `docs/reference/platform-support.md`.

## R5/R6 continuation in this task

- Production-private `windows_async_transition.S` now supplies one common
  restore-only/save-and-gate implementation for ordinary opt-in runtime and
  the testhooks library; the ordinary Windows context-switch assembly remains
  unchanged. The first native scheduler-return seam preserves G-local errno
  and LastError, and the scheduler alone publishes exact CAPTURED→READY after
  clearing active G. A bounded immutable safe-range registry validates the
  actual stopped RIP. The private production capture/restore transaction owns
  all suspend/context operations and uses fail-fast on uncertain committed
  results. Explicit no-testhook transaction and safe-PC native CTests passed.
- The only certified ranges admitted for automatic capture are PE image code.
  At enablement LLAM takes a loader reference for each registered image and
  retains it through runtime shutdown; MEM_PRIVATE/JIT ranges are rejected.
  A native test DLL remained mapped after its caller dropped the original
  handle and unloaded when LLAM released its reference; a private executable
  mapping was rejected. The registered code must not be modified during run.
- An exact read-only later suspension can witness the restored G context even
  outside a capture-certified PC. It checks pending bank/generation, task and
  worker dispatch generations, binding, active managed G, G/native stack
  bounds, and the saved native scheduler frame. Only after unchanged resume
  does the controller clear pending restore and recycle the bank. A four-cycle
  no-safepoint A/B chain passed 20 native repeats; an unsafe-PC witness also
  passed. `T-XHIGH-WIN-16` accepted this staged protocol in principle and
  distinguished read-only witness from capture eligibility.
- The existing single watchdog thread now owns the private operation record.
  It services restore/observation before new capture and probes no more than
  once per clamped minimum 5 ms quantum; AUTO requires runnable pressure,
  STRICT does not. Stop closes new automatic admission but preserves all
  already-admitted restore obligations. A certified infinite G remains
  preemptible for fairness, not asynchronously cancel-safe; after all async
  debt drains, stop still cannot guarantee termination if G never reaches a
  cancellation/safepoint boundary. `T-XHIGH-WIN-18` withdrew an earlier
  stop-after-witness blocker after inspecting that source-based distinction.
- Native automatic no-manual-controller four-cycle test passed 20 short
  repeats, with at least four actual captures/restores, three direct witnesses
  and peer progress. Automatic unsafe-PC, empty registry and NO_PREEMPT
  controls passed with no context mutation; the latter two made zero remote
  probes. Automatic stop after an admitted capture passed 20 repeats. Final
  focused native matrix after PE-image pinning passed 33/33. macOS `make -j4
  test` passed. The default-OFF Win64 cross DLL built with no async transition
  sources or exports; the native opt-in DLL exported no private async symbols.
- ChatGPT turns `T-XHIGH-WIN-15` through `T-XHIGH-WIN-19` are in the same
  user-designated conversation. Extra High did not inspect source or rerun
  tests. The user permitted it when Pro was unavailable. No commit, push,
  PR, deployment or host configuration change was made.
- `T-XHIGH-WIN-19` classified the reported and natively tested result as an
  experimental automatic Windows x64 backend only within the default-off,
  one-primary-M, MinGW, internally activated, manually certified immutable
  PE-image-code envelope. It found no further concrete correctness
  counterexample within that envelope, but this is not Pro review or a claim
  of broader Windows support or performance improvement.

## Public destroy overlap continuation

- `T-XHIGH-WIN-20` proposed a bounded A–E diagnostic sequence for public
  `llam_runtime_destroy()` overlapping the private one-M automatic backend.
  Source inspection confirmed that destroy holds the lifecycle lock while
  polling `exec_started`, but the borrowed run tail completes `run_end`, M
  unbind and final runtime access before release-clearing that token without
  needing the lifecycle lock. `shutdown_requested` and watchdog join occur
  only after the active run token clears.
- Added testhooks-only, no-production-lock pauses at A: pre-first-dispatch;
  B: scheduler CAPTURED→READY before reinjection; C: native GATE before
  controller restore; D: successful remote restore before witness. An
  independent host thread calls the real public destroy while the pause
  holds, and a host release word makes the certified leaf bounded. Private
  snapshots assert exact bank phase, binding and pending debt before release.
  D additionally keeps G busy until a direct witness clears pending restore
  and binding operations. A–D each passed 20 short native serial repeats.
- E keeps the first caller OS thread alive after run 1 completes an automatic
  capture/restore/witness and all backend debt reaches zero. A different
  simultaneous native caller M runs the same runtime again with increased
  worker incarnation/run generation and reaches D's committed-restore window;
  a third host thread calls public destroy. E passed ten short repeats. This
  is sequential rebind, not multi-M concurrent execution; the earlier
  scheduler oracle separately covers stale incarnation refusals.
- Native Windows 10 x64 MinGW Release final focused matrix after the new
  test-only hooks: **38/38 passed in 4.99 s** with at most two build jobs.
  The shared-library target also rebuilt successfully. The server was checked
  before tests at about 10–15% CPU and roughly 10–11 GiB free RAM. No heavy
  stress or soak run was made.
- `T-XHIGH-WIN-21` initially misstated B as READY with an idle controller and
  imagined a wait/notify lost-wake path for `exec_started`. We corrected both
  using actual source/test evidence: B has `op.shard != NULL` and
  `binding_ops==1` through READY; destroy spin-polls an acquire load and has
  no sleep/notification edge. `T-XHIGH-WIN-22` accepted the corrected narrow
  A–E qualification and found no concrete counterexample within it. This is
  only an Extra High protocol review, not independent source inspection or
  Pro approval. Registration/enablement races, controller startup failure,
  public activation, and simultaneous multi-M remain separate work.

## Private configuration admission continuation

- `T-XHIGH-WIN-23` identified the actual unpinned raw-runtime register/enable
  lifetime race with public destroy and the capsule-less host-spawn straddle.
  The Windows experimental code now pins the runtime before configuration
  dereferences, uses a per-runtime configuration mutex plus a temporary
  `exec_started` claim, and reserves host/foreign spawns from before task
  allocation until complete publication or failure cleanup. It freezes the
  safe-range registry on enable, does not add a public API, and leaves the
  default-OFF build unchanged.
- Six deterministic native CTests now cover both spawn/enable orderings,
  enable/destroy, run-first versus enable, cross-runtime managed-spawn
  trylock refusal, and control-thread startup failure/recovery. All six
  passed 20 short serial repeats. The complete rebuilt native Windows async
  suite passed **36/36**; runtime core, multi-runtime core and Windows smoke
  also passed. Mac runtime core/multi-runtime core and full `make test`,
  strict MinGW syntax, the default-OFF DLL, license policy and diff checks
  passed.
- `T-XHIGH-WIN-24` gave only a conditional Extra High protocol acceptance:
  cross-runtime managed spawn must return before allocation when trylock
  fails. Source inspection and the new native test establish that condition.
  The review did not execute code or substitute for Pro. Public activation
  semantics, multi-M, broader code/XSTATE qualification, bounded cancellation
  of an uncooperative G, and causal IOCP latency work remain outside this
  private configuration gate.

## Delayed controller entry continuation

- Rechecked that the existing `ctrl_thread` is the actual Windows async
  watchdog. A successful `pthread_create` precedes `initialized=true`, but
  the new OS thread can reach its loop later. Automatic capture originates
  only from that loop, so delayed scheduling creates no half-admitted async
  transaction; the already tested create-failure path still cleans up before
  any enabled runtime is published.
- A testhooks-only first-action gate now holds the watchdog before its first
  tick. A first certified G runs and completes on a borrowed M while the
  watchdog is held, with no capture or binding operation and normal
  first-dispatch execution debt that drains at run end. After the gate opens,
  several idle ticks cannot use that retired binding. A different caller M
  then drives a second run with real automatic capture/restore/direct witness
  and zero debt at completion. The new native CTest passed 20 repeats; the
  rebuilt Windows async matrix passed **37/37**, with runtime core,
  multi-runtime core and Windows smoke passing separately.
- `T-XHIGH-WIN-25` called this a useful startup/lifetime characterization,
  not a prerequisite for the existing private one-M safety claim. The
  remaining substantive choices are public activation/concurrency semantics
  or multi-M authority/controller design; no public ABI or runtime behavior
  changed in this continuation.

## Public expert opt-in continuation

- Added one host-only, one-way public `llam_runtime_enable_experimental_windows_async_preempt_ex()` transaction. It requires an explicit process-wide
  fail-fast acknowledgement and caller certification of up to 128 copied,
  nonoverlapping PE-image PC intervals. The existing private configuration
  pin/mutex/synthetic-run-token/host-spawn-reservation boundary now stages
  the whole array and rolls it back on any failure. LLAM validates address,
  PE image, module pin, mitigation and XSTATE properties, not code semantics.
  Public ABI minor advanced 2.0→2.1; the default-OFF symbol is present and
  returns `ENOTSUP`. No public disable, automatic metadata loader or multi-M
  support was added.
- The production-linked native public success/rejection oracle and four
  public-entry race cases each passed 20 short repeats; the rebuilt Windows
  async matrix passed **43/43**. Native ABI, runtime core, multi-runtime,
  capability and smoke checks passed. The shared-load test now uses the native
  Windows loader and passes on both opt-in and default-OFF DLLs, which expose
  only the new public symbol and not private async/test helpers. Local macOS
  shared-export audit reports 131 public symbols. One initial macOS full-test
  run failed a worker-identity `sigaltstack` expectation; its focused binary
  and the full retry passed.
- `T-XHIGH-WIN-26` in the designated ChatGPT conversation recommended the
  one-call expert-only contract; that is reasoning from our reported source
  facts, not independent source inspection or execution. Pro remained
  unavailable. Windows PE metadata/verifier support is the next provenance
  improvement; simultaneous multi-M remains a separate execution-model step.
- `T-XHIGH-WIN-27` raised two concrete source conditions: pre-pin checks must
  not dereference runtime storage, and lock-free readers must acquire the
  final enabled flag before seeing provisional configuration. Source audit
  verified TLS-only pre-pin refusal and strengthened the operation, reclaim
  and diagnostic read gates. It also found a prior-run reclamation interval;
  public enable is now first-worker-run-only and rejects a completed earlier
  run with `EBUSY`. That case is in the native negative oracle. The final
  Windows async 43/43 matrix and shared-load test passed after these changes.
- `T-XHIGH-WIN-28` accepted the reported source audit at the deliberately
  narrow first-run-only one-M public contract, while emphasizing that an
  enabled acquire load does not replace a runtime lifetime pin. Public stats
  retain their normal operation pin; private async diagnostics in tests are
  called only while the run/runtime remains rooted. Extra High did not
  independently inspect source or execute tests, and Pro remained unavailable.

## Loaded PE/LLPM provenance continuation

- `T-XHIGH-WIN-29` in the designated ChatGPT conversation recommended a
  private, two-stage collector/handoff after Pro remained unavailable. This
  was a review of reported design facts, not independent source inspection
  or native execution. The collector requires a caller-held loaded `HMODULE`,
  takes its own temporary loader reference before reading, validates bounded
  PE32+/AMD64 headers and one final read-only `.llampm` image section, then
  parses LLPM v1 records at their loaded addresses. It returns copied,
  canonical nonoverlapping `ASYNC_SAFE` executable-image ranges and the held
  reference, or a clean absent/error result. It does not activate a runtime.
- A hand-authored PE fixture demonstrates field-relative relocations in the
  loaded image. A production-linked native test passes the collector result
  to the existing public expert opt-in while module references overlap;
  after releasing the caller and collector references, the runtime retains
  the image through real automatic capture/restore/direct witness and unloads
  it at shutdown. The public acknowledgement and manual code-safety
  certification remain mandatory. No Windows compiler emitter or semantic
  verifier was added.
- Native Windows 10 x64/MinGW Release: all ten PE collector CTests passed,
  then all **53/53** rebuilt Windows async CTests passed serially in 3.47 s.
  A full unrelated all-target build remains blocked by the pre-existing
  Windows-incompatible `test_legacy_lease_internal` source; focused async
  targets and the production runtime built successfully with two jobs.
- Focused native ABI contract/compatibility, capability, core,
  multi-runtime, shutdown, smoke and shared-load CTests passed. The
  default-OFF Windows build also passed shared-load, capability and ABI
  contract checks. The shared-load oracle now forbids both new private
  collector symbols in the DLL export surface.
- `T-XHIGH-WIN-30` accepted the reported R7 structural scope conditional on
  full loaded metadata-span `VirtualQuery` validation and identified Windows
  LLVM/COFF emitter plus linker-retention qualification as the next gate, not
  multi-M. Source audit closed that condition: the collector checks
  `VirtualAddress + VirtualSize <= SizeOfImage` in widened arithmetic,
  chooses `metadata_bytes <= VirtualSize`, checked-adds base and RVA, then
  walks the **entire** payload through `llam_windows_pe_readable()` before
  parser dereferences. That helper requires committed, readable `MEM_IMAGE`
  with the pinned module's `AllocationBase` and no guard/no-access pages.
  The section name uses exact eight-byte `memcmp`; LLPM's parser checks
  relative-target arithmetic. Extra High did not inspect the source itself.

## R8 resumption with Pro, 2026-09-27

- `active_modes: empirical/computational toolchain validation; technology
  transfer and project integration`. Relevant original material is PE/COFF
  and LLVM toolchain documentation/source, not a scholarly literature survey.
- `delivery_target: INTEGRATE`; continue the existing Windows PE metadata
  path toward compiler-generated LLPM v1 COFF records and qualification of
  the final `.llampm` image section. Preserve the public ABI, R7 collector,
  default-OFF behavior, and the one-M certified-code contract. No automatic
  module discovery, multi-M, deployment, commit, or public safety upgrade is
  implied. Current repository base HEAD `34dcc74`; the shared working tree
  already contains extensive user/prior-task edits, including all R7 files.
- Acceptance focus: compiler-emitted live leaf metadata reaches the existing
  private PE collector with exact final PCs; section GC/COMDAT must not leave
  metadata for discarded code; native Windows integration must exercise the
  real collector→expert-enable path. Required regression checks include the
  existing Windows async matrix and target-relevant local LLVM/LLPM tests.
- Codex model could not be verified from reliable current-session metadata:
  `codex_model: unknown`, `profile: general`. Browser conversation is the
  user-designated existing ChatGPT project conversation; its model selector
  visibly displayed `6 Pro` at the fifth/highest power position on
  2026-09-27 06:03 UTC. Official OpenAI model guidance identifies GPT-6 as
  the current family; actual account selection was verified in the browser.
  `researcher` and `openai-docs` skills are active for this turn.
- `T-PRO-WIN-31`: send confirmed in the designated conversation at 2026-09-27
  06:04 UTC. Pro was asked to lead R8 risk and work selection; Codex will
  execute bounded LLVM/COFF/linker tests and local integration. Shared source
  facts: current LLVM pass emits Linux-only function sections and LLPM map
  with ELF `__start_/__stop_`; R7 private PE collector and hand-authored
  COFF fixture passed 53/53 Windows async tests. Decision requested:
  defensible COFF range emission and GC/COMDAT/ICF qualification before
  source changes.
- `T-PRO-WIN-31` response fully read: GPT-6 Pro selected per-function,
  codegen-bound complete LLPM records associated with their actual COFF code
  leader; no aggregate map, no independently rooted metadata, no ICF/LTO
  in the first supported build profile. It assigned three decisive small
  experiments to Codex (field-relative REL32, associative GC, genuine
  non-Debug codegen endpoints), then a Pro decision on one production path.
  Pro inspected upstream PE/LLVM material, not LLAM source or our runs.
  E80–E83 now include the first local probes; return their results to Pro
  before an LLVM production rewrite.
- `T-PRO-WIN-32`: send confirmed in the designated 6 Pro conversation.
  Decision packet E82/E83/E84: complete
  two-field IR LLPM COFF record resolves exactly after GNU link; GNU MinGW
  `--gc-sections` removes an associative `.llampm` follower even while its
  function is auto-exported/live; ordinary `-O2 -ffunction-sections` emits
  no end symbol, whereas `-g` creates debug-only `.Lfunc_end*`. Ask Pro to
  choose a supported linker/endpoint strategy or narrow the R8 deliverable.
  Response received; see the following decision entry.
- `T-PRO-WIN-32` response fully read: Pro marked current GNU ld 2.46 + GC
  profile failed and selected an LLD differential before LLVM codegen edits.
  Prospective profile is pinned LLVM codegen boundaries, per-function
  associative records, qualified LLD with GC on/ICF off, unchanged R7.
  It explicitly rejected disabling GC, rooting all metadata, debug-label
  scraping, and a section-size/neighboring-symbol guess. Pro reviewed
  upstream sources but not LLAM or our execution. Local E86 and native E87
  now show the LLD ownership and loaded-collector gates pass for hand-authored
  records; E3 non-debug endpoint generation remains open.
- `T-PRO-WIN-33`: send confirmed in the designated 6 Pro conversation.
  Decision packet for E2L: frozen one-only
  COFF object SHA-256 `0709d122...` loses metadata under GNU GC but retains
  it under LLD `/opt:ref`; two-function plus ordinary object SHA-256
  `cb205f9a...` links to one live record with dead code/metadata absent,
  or two gap-free records with both live; duplicate strong one-only
  definition fails. Native Windows R7 positive and pin oracles pass for an
  LLD-built DLL. Ask Pro to close or reject ownership gate and choose the
  minimal non-debug endpoint producer investigation.
- `T-PRO-WIN-33` response fully read: Pro closed E2L code/metadata ownership
  only for one-only associative COFF under LLD 23.1.2 with `/opt:ref
  /opt:noicf`; GNU ld profile remains failed. It selected a bounded E3
  endpoint probe via an actual X86 `AsmPrinterHandler`:
  `beginFunction` selects a probe, `markFunctionEnd` emits its own label
  without switching sections, and `endFunction` later emits metadata only
  after endpoints pass. Stock `-fpass-plugin` attachment must be checked once;
  otherwise use one option-gated X86 printer registration in a pinned LLVM
  23.1.2 toolchain extension. No production certification or R8 integration
  claim yet. E89 records the installed-interface source audit: stock plugin
  attachment was unavailable for this selected mechanism/profile.
- `T-PRO-WIN-34`: send confirmed in the designated 6 Pro conversation.
  E3-END result to review: Homebrew-patched
  LLVM 23.1.2 X86-only `llc` with one option-gated AsmPrinter handler built;
  `markFunctionEnd` emitted local probe symbols at exact section offsets
  8 and `0xb7` in `-O2 -g0` COFF, with a first `ret` at `0x16` and later
  machine blocks. Ordinary function had no marker; code bytes matched
  option-off for all three. Request Pro decision on marker gate and the
  next smallest generated-record/late-eligibility increment.
- `T-PRO-WIN-34` response fully read: Pro accepted E3-END only as the F/K/H
  marker proof. The next step is generated associative LLPM transport from
  the same handler, then a separate fail-closed late-machine eligibility
  gate before any compiler-certified runtime activation. It asked to replace
  production name-based `getOrCreateSymbol` with a unique compiler-owned
  MC symbol retained by pointer, capture the actual entry symbol/COFF leader,
  mark the endpoint without section switching, and emit one complete record
  from `endFunction` only in a developer fixture mode. Fixture records may
  use `ASYNC_SAFE` only with explicit human certification, not a general
  probe-attribute credential; K stays endpoint-only. R7 and public ABI stay
  unchanged. Pro reviewed reported results, not our source or execution.
- `T-PRO-WIN-35`: send confirmed in the designated 6 Pro conversation.
  Report E91 generated transport:
  developer-only manually certified fixture attribute, real X86 handler
  emits unique codegen end and complete per-function associative LLPM
  records; object shows one-only parents, 48-byte followers and two REL32
  fields; LLD GC retains one/two live records and removes dead leaves and
  metadata; native unchanged R7 collector accepts one/two exact ranges.
  Ask Pro to review transport without semantic promotion and select the
  smallest late-machine eligibility/fail-closed tests next.
- `T-PRO-WIN-35` response fully read: Pro accepted generated LLPM transport
  only for the developer-fixture profile and no new ownership/field defect
  from our report. It selected an internal `win64-scalar-leaf-v1` late
  checker: existing restrictive IR verification AND an exact opcode/operand
  allowlist on finalized MachineFunction AND qualified contiguous COFF
  layout. Initial accepted data registers are volatile integer RAX, RCX,
  RDX, R8–R11 (and 32-bit aliases), with special handling for numeric
  register-only LEA, local branches, EFLAGS, and canonical RET64. Refuse
  frame/stack objects, calls/tail transfers, memory, spills, unknown
  executable pseudos, split/instrumented layouts, unqualified build modes.
  It requested concrete N1–N6 negatives and useful small positives before
  any runtime activation; transport mode must stay separate. E92 already
  supplies a strong N1 counterexample (memory-free IR, incoming Win64 stack
  argument loaded by late ADD64rm); E93 contrasts stack-using default O2
  with a stackless `-O2 -fno-unroll-loops` loop candidate.
- `T-PRO-WIN-36`: send confirmed in the designated 6 Pro conversation.
  E94–E96 late-machine prototype diagnostics:
  exact volatile-integer register/opcode/operand allowlist accepted simple
  leaves and a no-alignment finite loop, refused N1 incoming stack read,
  N2 generated `__udivti3` helper, N3 actual callee-save stack pushes,
  N4 RSP write and unannotated memory operand, N5 tail/indirect/far return,
  N6 executable pseudo and split-layout preflight. Mixed good/bad/ordinary
  require-mode build fails and removes the prior output object. Ask Pro to
  challenge the reported predicate and decide whether to connect the real
  Windows early-verifier tag to this gate; no current certification claim.
  Response received; see the next decision entry.
- `T-PRO-WIN-36` response fully read: Pro accepted E3-LATE-1 as diagnostic
  evidence, not certification. It identified a specific post-MIR gap:
  SLS return hardening can append INT3 and X86 MC branch alignment can
  insert NOP/prefix padding after the MachineInstr walk. It authorized
  diagnostic Windows early-verifier→late-audit wiring immediately, but
  withheld production ASYNC_SAFE emission until those modes are explicitly
  refused/qualified and admitted MIR lowerings are checked against final
  bytes. E98–E100 now record those discriminating checks and the actual
  Windows GNU/COFF IR provenance tag with no metadata emission.
- `T-PRO-WIN-37`: send confirmed in the designated 6 Pro conversation.
  Decision packet: SLS/MC-padding failures
  reproduced and guarded, accepted long local JCC relaxation verified,
  exact opcode-shape whitelist trimmed to forms with representative final
  disassembly; first-party Win64 diagnostic mode now tags only early-accepted
  functions, and the custom late-audit recognizes that tag. Actual Win64
  N1/N2/N3 are early accepted then late refused; good scalar leaf and
  no-alignment recurrence pass. Ask Pro whether E3-LATE-CLOSURE is enough
  to start gated compiler-generated certificates or what single concrete
  safety counterexample remains. No runtime activation yet.
- `T-PRO-WIN-37` response fully read: Pro closed E3-LATE-CLOSURE for the
  reported pinned profile, conditional on rerunning the latest specialized
  operand negatives before certifying outputs. It selected one dedicated
  certification-emission mode whose commit predicate is Windows source
  annotation IR acceptance AND late body/profile acceptance AND actual
  endpoint/one-only COMDAT/associative ownership. Developer transport and
  certification modes must be mutually exclusive. The first generated
  one/two/dead/ordinary certificate matrix should stop at the unchanged
  native R7 collector; runtime activation follows only after a separate
  Pro review. Direct latest-checker MIR replays for RSP LEA, memory without
  MMO, tail/indirect/far return and invalid JCC condition have now reached
  the intended refusals. No general Win64 compiler or runtime support claim.
- `T-PRO-WIN-38`: prepared for review in the same designated 6 Pro
  conversation. The explicit certification mode is now present only in the
  isolated pinned LLVM 23.1.2 X86 prototype, with first-party Windows IR
  audit provenance. E101–E104 record current-checker refusals, three
  compiler-certified LLD images, exact fields/GC, native unchanged R7
  collector results for one/two/finite-loop records, and fail-closed
  negatives. Ask Pro to review the single record-emission predicate and
  decide whether the next bounded compiler-generated-metadata → expert
  enable → automatic capture/restore/witness integration oracle is admitted.
  No automatic runtime activation from the new records has yet occurred;
  the LLVM patch is not packaged in LLAM and R8 is not merge-ready.
- `T-PRO-WIN-38` response fully read: Pro approved E3-CERT-1 only for our
  reported pinned source→COFF→LLD→unchanged R7 collector profile. It found no
  demonstrated predicate, ownership or mode-separation counterexample in the
  report but did not inspect/execute LLAM independently and did not approve
  general semantic safety or automatic runtime integration. Next authorized
  bounded increment is R8-RUNTIME-1: use the **same compiler-certified finite
  recurrence DLL**; independently compute expected unsigned results; enable a
  fresh one-M runtime from exact collected ranges with explicit fail-fast
  acknowledgement and release all caller/collector module ownership; require
  two captures of the same invocation with peer-G progress, direct restored-G
  witness between captures, correct return and normal retirement. Add small
  default-disabled, unrecorded ordinary, precommit-failure and final-unload
  controls. Missing repeated-chain coverage is not a pass; use fixed finite
  budgets and a subprocess deadline. Pro also required the patched toolchain
  and tests be packaged before R8 can be called integrated or merge-ready.
- `T-PRO-WIN-39`: prepared for review in the same designated 6 Pro
  conversation. E105–E107 document R8-RUNTIME-1: the first fixed 30M count
  yielded one correct-result capture and therefore insufficient coverage;
  a single bounded 120M setting yielded exact repeated
  capture→peer→restore→direct-witness chains for one compiler-certified
  recurrence invocation with independent-result match and normal debt/module
  cleanup. Disabled, ordinary structural exclusion and precommit enable
  rollback controls passed. The newly packaged official-LLVM patch and
  source-to-DLL script reproduced a fresh collector-accepted image that also
  passed the same positive runtime oracle. Ask Pro to assess this exact gate
  and identify any concrete remaining issue before calling R8 experimentally
  integrated; no multi-M/general compiler claim or release approval.
- `T-PRO-WIN-39` response fully read: Pro accepted R8-RUNTIME-1 for the
  reported fixed 120M input, one-M pinned profile and accepted the disclosed
  single 30M→120M coverage calibration. It stressed that 30M remains a
  correct-result non-pass, not a context error; direct witness is an observed
  restored context, with peer execution/result supplying separate progress.
  It identified a test-precision improvement: pair events by task and actual
  bank identity/generation plus both task and worker restored dispatch
  identities, not just count or greater-than run generation. The test-only
  trace and validator have now been tightened accordingly; native rerun is
  pending at this entry. Pro also corrected the precommit control inference:
  a valid-first/MEM_PRIVATE-second input proves atomic rejection and no
  residual module ownership, but **not** that an acquired first loader ref
  was necessarily rolled back. Pro classified the end-to-end prototype as
  demonstrated, while experimental integration remains pending a clean
  compiler reconstruction from packaged inputs, final native CTest matrix,
  and an actual Linux ELF regression. Its review is based on our reports,
  not independent source inspection or execution.
- Post-review R8 gates: the exact packaged patch was applied to a fresh
  declared Homebrew-patched official LLVM 23.1.2 source base, a new X86-only
  `llc` and first-party pass were built, and a fresh DLL passed native R7
  collection and the tightened fixed-120M automatic-chain oracle. The final
  native Windows CMake/CTest async matrix with that DLL passed 57/57; both
  default-OFF and opt-in ABI/capability/shared-load groups passed 4/4. The
  offline packaged certifier script passed positive COFF/GC and specific
  refusals. See E109–E112. The actual Linux-host compiler/metadata/parser
  test remains pending: the available shared Linux host lacks build tools and
  has 1.9 GiB RAM; local Docker is installed but the Mac was locked when
  starting it. The user was asked to unlock the Mac. Until that regression
  runs, report R8 as an end-to-end demonstrated prototype, not fully
  experimentally integrated or merge-ready. No commit/push/PR/release.
- Linux gate closure after the earlier pending entry: once the user unlocked
  the Mac, a resource-capped Ubuntu 24.04 amd64 Docker container with the
  workspace mounted read-only built the matching LLVM 18 first-party pass,
  metadata inspector/parser and runtime registration target. Native Linux
  CTest passed the three affected tests 3/3 with no skip/failure (E113).
  The shared low-memory Linux server remained untouched. `T-PRO-WIN-40`
  subsequently submitted this and the clean-compiler/final-Windows evidence
  for scoped classification; no broader safety or performance claim.
- `T-PRO-WIN-40`: sent and fully read in the same designated 6 Pro
  conversation. Pro accepted **experimental integration** for the reported
  pinned LLVM 23.1.2 extension, qualified LLD settings and WinGNU
  scalar-leaf-v1 one-primary-M profile. It judged the clean compiler
  reconstruction, final native 57/57 async plus default-OFF/opt-in ABI and
  shared-load groups, actual Linux amd64-container 3/3 compatibility tests,
  and tightened bank/task/worker trace sufficient to close its three
  remaining gates. No further discriminating test was required by that
  review. Pro did not inspect or execute source independently and did not
  certify a release, arbitrary caller scope, native Linux hardware, wider
  LLVM/linker versions, XSTATE/mitigation, multi-M or performance. The
  unrelated Windows all-target `test_legacy_lease_internal` compile issue
  remains visible. No commit, push, PR, release or deployment occurred.
