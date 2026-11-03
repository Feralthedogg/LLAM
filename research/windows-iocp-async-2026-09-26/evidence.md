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

# Evidence and decision boundaries

- C1, supported: `GetQueuedCompletionStatusEx` receives up to the requested
  entry count, so LLAM's 64-entry default is a capacity, not a wait-for-64
  threshold. E1: [Microsoft API contract](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getqueuedcompletionstatusex),
  parameters and return value; inspected 2026-09-26. Local source:
  `src/io/windows/watch/iocp.c:124-163`, `src/io/windows/watch/windows_completion.c:272-300`.
- C2, supported only under the documented conditions: skip-on-success omits an
  IOCP packet when an associated asynchronous handle's operation succeeds
  immediately; the mode cannot be removed from that handle. E2:
  [Microsoft notification-mode contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setfilecompletionnotificationmodes),
  flags and conditions; inspected 2026-09-26. LLAM already has an immediate
  finalizer in `src/io/windows/watch/windows_completion.c:253-269`. Whether the
  path helps actual traffic remains unmeasured.
- C3, supported: ordinary user APCs need an alertable target wait; special
  user APCs can interrupt user execution, but may nest, are unsynchronized,
  and require Windows 11 build 22000 or newer. E3:
  [QueueUserAPC](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-queueuserapc)
  and [QueueUserAPC2](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-queueuserapc2),
  remarks and requirements; inspected 2026-09-26. The test host is Windows 10
  build 19045, so QueueUserAPC2 is outside its documented support.
- C4, supported API boundary but not yet a working LLAM mechanism: Microsoft
  says a valid remote `GetThreadContext` requires suspending the target, setting
  a running thread's context is unpredictable, and `SuspendThread` can deadlock
  when the target holds a lock the controller then needs. E4:
  [SuspendThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-suspendthread),
  [GetThreadContext](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadcontext),
  [SetThreadContext](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadcontext),
  remarks; inspected 2026-09-26.
- C5, supported implementation fact: current Windows context assembly saves
  the Windows nonvolatile XMM6-XMM15 set, whereas async interruption can occur
  between calls and needs the otherwise volatile machine state too. E5: local
  `src/asm/windows/x86_64/windows_context_x86_64.S:35-130` and Microsoft's
  [x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention).
- Baseline B0: the source snapshot and CMake build described in `state.md` pass
  four focused Windows tests. This is correctness smoke evidence, not a stage
  cost measurement or a Windows async-preemption pass.
- E6, observed native conformance: the isolated two-file fixture at
  `experiments/windows/context_conformance.c/.S` (SHA-256 C
  `8c74020d46f2d2b9a3487b4003f16fa3abd4ecf53d4ac929de70b52c2bb5ff85`,
  assembly `5ada2c475e2c0b9f48b70a586dea462571efc233e61cbb356aefb2f06e2b8595`)
  compiled with MinGW GCC 15.1.0 `-std=c11 -Wall -Wextra -Wpedantic -Werror
  -O2 -g`. A bounded Windows 10 x64 run and a direct exit-code rerun reached a
  known safe leaf, captured with `CONTEXT_ALL | CONTEXT_XSTATE`, redirected to
  an alternate-stack landing, reached a controller-observed restore gate, and
  restored the original CONTEXT. Direct rerun returned 0 and reported:

      flags=0x10005f enabled_xstate=0x807 valid_xstate=0x807
      remote landing and RAX/YMM0/MXCSR/error-state restore passed

  This tests one controlled worker, one GPR, one YMM register (including upper
  AVX state), MXCSR and target-local `errno`/`GetLastError`. It does not yet
  establish every GPR/nonvolatile vector, external suspend interference,
  exception/unwind safety, mitigation refusal, lifecycle concurrency, or LLAM
  scheduler integration.
- E7, bounded baseline diagnostics: serial native CTest passed 4/4 Windows
  policy/runtime/IOCP/HANDLE tests. Existing 1-byte `io_echo` smoke reported
  214,429 ops/s, three rounds of 256 exchanges after one warmup, with 16
  runtime workers and dynamic workers off; `channel_pingpong` reported
  930,937 ops/s with three rounds of 1,024 exchanges. Both exited zero.
  The host was shared and `io_submits=0` for `io_echo`, so these runs cannot
  attribute any time to the IOCP path or support a before/after claim.

Pro's T-PRO-WIN-01 judgment is a research decision based on the supplied LLAM
interface facts and Microsoft's/Go's/libuv's documentation. Pro did not inspect
the working tree or run the Windows host. WIN-01 and WIN-03 results must return
to Pro before an optimization or backend is adopted.

- E8, revised native fixture: C SHA-256
  `a2835236d424c54f74395b08218817e7b53ad9185c6d2e054fae6627c7478ae0`
  (same assembly as E6). The worker first waits outside the certified leaf;
  one bounded suspension attempt refuses that PC and balances the count.
  A scripted second owner then holds one suspension; the candidate sees prior
  count 1, withdraws only its own increment, and the first owner releases its
  count. The later full-context positive path still returned exit 0 with the
  same state sentinels. This checks two recovery cases, not arbitrary debugger
  interference or failure after context mutation.
- E9, native mitigation and stack observations: a read-only
  `GetProcessMitigationPolicy` query returned user shadow-stack flags `0x100`
  and CFG flags `0x0`. The code queries the policy before redirect and refuses
  enabled shadow stack or context-IP validation. [Microsoft's policy field
  definitions](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-process_mitigation_user_shadow_stack_policy)
  identify bit 8 (`0x100`) as `CetDynamicApisOutOfProcOnly`, affecting the two
  named dynamic CET metadata APIs; the execution and context-IP validation
  bits were clear. The landing's stack marker was outside the bounds returned
  by `GetCurrentThreadStackLimits`. [Microsoft's stack-limits contract](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getcurrentthreadstacklimits)
  explicitly allows user-mode execution outside the system-allocated stack;
  the result does not prove stack-probing or exception/unwind safety.
- E10, CET XSTATE classification gap: the enabled/valid mask `0x807` contains
  bit 11, defined as `XSTATE_CET_U` in Microsoft's published `winnt.h` metadata.
  This is distinct from the process mitigation policy queried in E9. The
  fixture currently requests and copies the entire enabled mask; it has not
  yet inspected the CET_U component or tested component-policy mismatch.
- E11, updated WIN-03 fixture: after explicit mitigation refusal for active or
  audit/relaxed shadow-stack/context-IP policy, all-enabled-XSTATE admission,
  CET_U bytewise copy and post-restore comparison, the Windows 10 test host
  reported 16 matching CET_U bytes. Static landing unwind metadata and the
  bounded three-page stack probe/canary passed. `RtlVirtualUnwind` from the
  landing C body reached the synthetic assembly boundary, and from the
  restore gate reached the landing body. Deliberately clobbered RBX/RBP/RSI/
  RDI/R12-R15, XMM6-XMM15 with YMM upper halves, RAX/YMM0, MXCSR and x87
  control all passed after full restore. An intentionally corrupted saved RBX
  was detected as `nonvolatile GPR 0 mismatch`, exit 1. This is still one
  controlled thread, not a LLAM worker or lifecycle test.
- E12, fatal stack counterexample under test: adding a C++ local throw/catch
  helper (source hashes C
  `0bf0b144c359c1de77420b135d472b95aea01a863d6a66c46a547ca71a57dd89`,
  assembly `627ed480e39e3b60c27ebf42e85ddd93c730971349bf707dd2ac0b41b6a0d4de`,
  C++ `92c76a507459255ae1052035f6447f27b21b7ebc1cdfbd40dd7f3d2d2bd4ad12`)
  produced `original worker stack exception control passed` before redirect.
  After `CET_U copy matched`, the isolated process exited with decimal
  `541541187` (`0x20474343`) before landing-ready publication. The helper's
  exception was not caught on the hand-allocated alternate stack in this
  configuration. This does not yet isolate TEB stack limits, unwind metadata,
  or MinGW runtime behavior as the cause. A Windows-managed Fiber control is
  the next discriminating check. No Windows backend integration is justified
  while this gate fails.
- E13, genuine Windows Fiber control: the same compiled helper object returned
  42 on the original stack and after `ConvertThreadToFiber`/`CreateFiber`/
  `SwitchToFiber`; the Fiber-local marker was inside the current stack limits.
  Direct exit code 0. Source:
  `experiments/windows/fiber_exception_control.cpp` SHA-256
  `d7441a009a737e33a98ff5e76f6b39181a9657b69ce4e07cb3340bca49b24b8b`.
  This is a passing alternative stack-management path, not proof that one TEB
  field caused E12.
- E14, same-binary landing A/B: an ad-hoc assembly transition put the safe G
  leaf on a private stack. With the landing switched to a reserved frame inside
  the worker's live native stack, the unchanged C++ helper caught locally,
  full register/XSTATE and canary checks passed, and direct exit was 0. With
  the same G/private-stack origin but the manual 64 KiB C landing, exit was
  `541541187` after CET_U copy and before landing-ready. Source hashes: C
  `5be13cf1986160cb9ccfe5665b44ec06262db7a90e49e99ef6451e2b631687ba`,
  assembly `60861478f883cf1f2a6271eb79b66e92cb014738c1dc065aea73ba20cbf02243`,
  helper `92c76a507459255ae1052035f6447f27b21b7ebc1cdfbd40dd7f3d2d2bd4ad12`.
  The passing native mode uses a test-only wrapper, not LLAM's production
  scheduler continuation; it does not satisfy WIN-03-N's integration oracle.
- E15, actual LLAM async-off G0 counterexample: linked the same helper with the
  current Release `libllam_runtime.a` from the isolated working-tree snapshot.
  A deterministic one-worker LLAM task invoked the helper on its ordinary G
  stack, planned a yield and second invocation. Native M before task start
  printed `42`; the process exited `541541187` at the first G-local invocation,
  before yielding. Direct run repeated the result. Source:
  `experiments/windows/llam_g_exception_control.c` SHA-256
  `0eee843ee9990bc4808f2ef947e62feb7ade83f60d7a8c2336602c25ec820e8d`.
  This is scoped to Windows 10 x64, the current MinGW GCC 15 build, and this
  C++ exception runtime. It predates any Windows async-preemption source edit.
- E16, fourth Pro decision: restricted implementation may proceed only with
  manually certified immutable safe ranges for non-throwing/non-unwinding C,
  capture of a qualified primary M and managed G, complete supported state,
  and a live native M scheduler-return activation. The manually allocated C
  landing is excluded. Pro specifically requires four native production-path
  gates: local native-stack exception oracle after actual arrival, two-G
  scheduler-return generation turnover, full state through real scheduling,
  and external controller-operation lifetime through stop/close. This is a
  review decision, not implementation evidence or test acceptance.
- E17, preliminary same-operation A/D Windows run: the raw experiment with
  `LLAM_RAW_IOCP_READS_ONLY=1` uses overlapped reads and direct nonblocking
  sends. Each 256-record round completed 512 overlapped read submissions,
  512 IOCP completions, 512 direct sends, and 1,048,576 total transferred
  bytes (half through IOCP). Measured throughput was 32,751, 32,377, and
  32,629 records/s, with one completion per GQCSEx call. The adjacent public
  LLAM experiment with `LLAM_IO_COOP_YIELD=0` likewise reported 512 IOCP
  submissions/completions and measured 24,403, 24,198, and 23,584 records/s.
  Both used one credit, 1 KiB preconnected loopback echo, four rounds with
  first warmup, and BelowNormal priority. This quantifies an end-to-end gap on
  a shared active host, not an individual stage cost or causal optimization.
  The raw run's current-thread CPU time is coarsely quantized to 15.625 ms,
  so it is not a meaningful per-round CPU comparison at this duration.
- E18, B/C differential: `experiments/windows/bench_iocp_engine.c` drives the
  actual LLAM node queue, association, submit, GQCSEx and finalizer with no
  managed task. The request is stable until a testhooks-only release/ACK
  posted after the OVERLAPPED op is recycled. Three measured rounds were
  31,206, 31,328 and 30,523 records/s; every round had 512 reads completed
  and 512 direct sends. `LLAM_BENCH_MINIMAL=1` in the public experiment runs
  two ordinary Gs with the same I/O but no payload validation, returning
  23,131, 24,709 and 24,223 records/s with 512 IOCP completions and 514
  context switches. The adjacent validated D run was 24,256, 24,322 and
  24,317 records/s. Driver synchronization and exact user work differ, so
  these rates cannot be subtracted into exact engine/wake costs. They support
  prioritizing managed completion-to-dispatch over unobserved batch tuning.
- E19, existing runtime diagnostics in a separate instrumented D run: one
  fourth-round shared-host outlier was 21.429 ms versus about 10.6 ms in
  other rounds. Its node counted 511 submit batches, 512 entries, max batch
  2, CQ depth 1/max 2 and zero immediate completions. Shard diagnostics
  counted 512 parks/wakes and 18.3675 ms accumulated I/O completion latency
  over 512 samples, plus 5.4887 ms accumulated runnable wake latency over
  514 samples. These are different/overlapping scopes, not additive stage
  durations; the host's background load makes short-trial outliers plausible.
- E20, fifth Pro source-path critique: a remote bridge cannot call
  `llam_ctx_switch(&task->ctx, scheduler_ctx)` because it would replace the
  interrupted G continuation with the bridge's cooperative SP/registers.
  It must restore the saved scheduler call boundary without saving into either
  G or scheduler destination context. Windows target-local errno and last
  error must be classified before the current wrapper's scheduler-errno
  restoration; the existing after-clear hook is too late. Save-complete
  dispatch generation and live return-slot lifetime remain unproven in the
  production dispatcher. This is a code-review hypothesis, not a tested
  Windows backend.
- E21, rejected metrics-only optimization: Pro approved a reversible test
  conditional on source/lifetime/snapshot checks. An isolated candidate CMake
  build replaced only the first Windows `complete_req()` metrics mutex with
  relaxed atomic increments; it left ownership, finalization and READY locks
  intact. Eight short processes in baseline/baseline/candidate/baseline/
  candidate/candidate/baseline/candidate order produced 12 measured 256-record
  rounds per build with exact 512 IOCP submissions/completions and 514 context
  switches. Baseline and candidate distributions overlapped; the candidate
  gave no improvement beyond unchanged-build variation. The conditional code
  was removed. No Windows I/O hot-path change was adopted. The raw samples
  are retained in the isolated host's `build-baseline/comparison/` logs.
- E22, scheduler-only ceiling test: `experiments/windows/bench_context_windows.c`
  completed deterministic one-G 10k-yield and two-G 20k-yield runs. Measured
  ordinary rounds were about 545–573 ns/yield; task-SIMD-save omission, an
  explicitly unsafe ceiling option, varied similarly with an outlier and did
  not demonstrate a reproducible gain. It is not a substitute for a matched
  cross-OS context test and was not enabled.
- E23, real-dispatcher native Windows oracle: the testhooks-only source
  `src/core/context/windows_async_probe.c` plus a restore-only/save-and-gate
  testhooks assembly `src/asm/windows/x86_64/windows_async_probe_gate.S` exercised
  two remote captures and full-context restorations of the same G across a
  genuine cooperative yield in LLAM's actual Release scheduler. G2 progressed
  while G1 was suspended. The controller required zero previous suspend
  count, certified assembly PC, private G SP, active-G/storage equality,
  native scheduler RSP in the original M stack, supported XSTATE subset,
  and gate PC/new generation before each context mutation. The native-stack
  local C++ exception/destructor oracle returned 42 at both arrivals; the
  test checked target-local errno/LastError, unchanged G.ctx before each
  restore, and four cleanup executions before/during/during/after. CMake
  option `LLAM_WINDOWS_ASYNC_SCHEDULER_ORACLE=ON` is off by default. Native
  CTest passed, then `--repeat until-fail:10` passed 10/10 serial repeats;
  focused Windows policy/runtime/IOCP/HANDLE tests later passed 6/6. This
  qualifies a bounded integration mechanism, not a production async backend
  or lifecycle/vector/race acceptance. The initially timed-out test versions
  used an encoded public handle as an internal pointer and overconstrained
  valid XSTATE features; both test errors were corrected before a passing
  result was reported.
- E24, ordinary switch isolation: the first oracle implementation shared save
  and restore labels with the production Windows x64 context switch, adding
  a jump and branch to that hot path. Before delivery the probe assembly was
  split into its own testhooks-only object and the ordinary switch restored to
  its prior instruction flow. A repeated native CTest run of the refactored
  two-cycle oracle passed 10/10; `test_windows_runtime_smoke` relinked against
  the unchanged ordinary switch. This removes an avoidable production
  performance regression from an experimental safety test.
- E25, seventh Pro decision: the actual LLAM native-stack activation and
  positive two-generation sequence are accepted at fixture scope. Pro did not
  inspect emitted assembly and did not approve a production opt-in. Remaining
  qualification requires stale identity refusal, two simultaneously captured
  G capsules with deliberate complete-state checks, and controller/task
  retirement plus stop/rebind races. Pro also cautioned that the assembly gate
  call is one outstanding ordinary call, not a C `noreturn` or `returns_twice`
  contract, and that accepted controller PCs must be in the post-save spin
  interior. The test declaration is `void` without either attribute, and its
  PC admission uses the exported spin-begin/end labels, but emitted code has
  not yet been independently inspected.
- E26, bounded N1-style negative extension: while the legitimate restore gate
  remained pending, the controller tried three stale identities in sequence:
  wrong G pointer, wrong restore run generation, and mismatched M binding
  incarnation. Each path balanced a successful zero-prior-count suspension,
  read the gate CONTEXT, rejected before SetThreadContext, and resumed the
  original gate. A separate mutation counter required exactly four positive
  SetThreadContext calls for two captures and two restores, with no additional
  negative mutation. The same two-cycle CTest passed 10/10 after this change.
  The incarnation mismatch is synthetic, not yet a real prior-run rebind.
- E27, two-capsule one-M oracle: two Gs ran different certified assembly
  loops, with unique RAX/RBX, YMM0/YMM6 low and upper lanes, MXCSR, x87 CW,
  errno and GetLastError sentinels. Their separate CONTEXT/XSTATE records and
  cooperative G.ctx snapshots coexisted before either remote restore. The
  test-only scheduler gate kept G1 off-queue until G2 was also captured, then
  restored each G from its own record. Assembly recorded volatile and
  nonvolatile state before C executed. The native M oracle independently
  checked its expected FP environment and errno on both original-wrapper and
  delayed-gate return routes. The positive CTest passed 10/10 serial repeats;
  corrupting G1's saved RBX bit caused a deterministic test exit 1. This does
  not cover every GPR/vector component or all XSTATE initialized-state cases.
- E28, controller/task retirement overlap: the first G was detached before
  run. While its actual OS thread was suspended in the safe leaf, the
  controller acquired LLAM's scan reference on the resolved internal task.
  After remote restoration and immediate G exit, a test-only hook in the real
  scan-ref quiescence loop published that reclaim was waiting on the positive
  ref. The controller only then made its final capsule access and released the
  ref; the scheduler completed cleanup. This passed 10/10 repeated CTests.
  It demonstrates one task-storage barrier, not production capsule/M record
  lifetime; the fixture's buffers remained in harness-owned state.
- E29, bounded stop-request variants: the same two-capsule CTest returned
  success with a nonblocking runtime stop requested after G1 capture-ready,
  at the first native restore gate, and just after the first successful remote
  context restoration. Each variant finished without rescue, timeout or
  outstanding test-controller references. Concurrent public destroy/shutdown
  and new-M rebind remain separate open gates. Final focused native Windows
  tests including both oracles, policy, runtime smoke, IOCP I/O and dump, and
  HANDLE I/O passed 7/7 on the supplied Windows 10 host.
- E30, eighth Pro qualification decision: N1 negative identity probes, N2
  two-continuation positives and N3 task-object reclaim overlap are accepted
  at fixture scope only. Pro explicitly separated task `scan_refs` from
  capsule-bank generation, G stack, M restore storage, native binding and
  runtime controller-op lifetimes. It recommended acquiring the task reference
  under protected publication before suspension and releasing it after the
  controller's final access without waiting for a worker progress milestone.
  Production opt-in remains unapproved pending actual storage-retirement,
  public close/destroy and real rebind/delayed-operation gates.
- E31, new caller binding check: after the two-G run drained, a second native
  caller drove the same runtime's primary slot with a third G in the certified
  assembly leaf. A bounded suspend/GetThreadContext/resume observed that the
  worker binding incarnation and run generation advanced beyond copied old
  values; the previous restore generation was not current. The test made no
  SetThreadContext call for this old identity, then released the G, joined it
  and drained the runtime. This is a useful rebind smoke and balanced refusal,
  but no pending production controller operation crossed the incarnation.
- E32, optimized instruction-boundary inspection: MinGW `objdump -dr` of the
  Release testhooks archive showed `llam_windows_async_probe_resume` calling
  `llam_windows_async_save_gate` at its ordinary call site, followed by a
  return-value setup and saved-register/stack epilogue. The gate object saved
  FP/XMM and nonvolatile GPR/RSP state before the aligned 32-bit phase store;
  the accepted spin-begin label followed that store. These facts corroborate
  the live-return and save-complete fixture invariants on this build. They do
  not substitute for test of a later production compiler configuration.
- E33, final paired A/B/C/D diagnostic: a source-controlled PowerShell runner
  executed the rebuilt self-BelowNormal binaries in A/B/C/D/D/C/B/A order,
  four rounds per process with first warmup and six measured rounds per
  layer. Each round used 256 one-credit 1 KiB loopback records with 512 IOCP
  reads; A/B used 512 direct sends, C/D reported 514 context switches. The
  measured records/s medians were A 31,137, B 30,468, C 23,483, D 21,430;
  min–max intervals were A 21,744–32,644, B 28,956–31,170, C 21,403–24,649,
  D 19,188–23,603. The host was shared and samples drifted visibly, so the
  experiment supports prioritizing managed completion/wake/dispatch
  instrumentation, not an exact per-stage cost or an accepted speedup.
- E34, testhooks completion ACK lifetime fix: the B-only completion flag
  pointer is now read from `req` before the normal completion path can make
  its G runnable, and the post-finalizer ACK uses only that cached pointer.
  The earlier version read the request after the task could have recycled it.
  The rebuilt Windows testhooks library and B benchmark passed exact operation
  accounting; no public Windows completion behavior changed.
- E35, ninth Pro lifecycle decision: a restricted one-M Windows backend may be
  implemented behind default-off admission, but is not accepted by the current
  fixture. The smallest sufficient G-side lifetime model is the existing task
  scan ref plus an exact external-operation reference and immutable
  generation/phase for each preallocated CONTEXT/XSTATE bank, with M/run/binding
  ownership separately retained. The present task-stack release order can
  destroy the async capsule before the later scan-ref quiescence check, so an
  early nonblocking DONE/DEFERRED barrier must precede capsule destruction
  and stack detachment. On DEFERRED the scheduler owns a retirement-list entry
  and keeps running; it does not block on the controller. The controller
  acquires task/bank/binding protections under existing active-G publication
  locks before suspending the target, releases those locks before remote
  context work, balances the suspension, and drops references after its final
  access without waiting for task reclamation or run-end. Successful banks
  become FREE only after exact target reentry **and** external-reader drain.
  Run-end keeps controller service available until deferred retirements and
  pending restores finish, then closes the native binding/handle. Pro explicitly
  said WIN-03 and WIN-05 remain open until those production-owned paths pass a
  detached-G DEFERRED/retirement oracle plus stop/close/rebind checks. This is
  an implementation design, not a source- or host-validated backend.
- E36, tenth Pro source-boundary decision: Pro approved a first Windows
  lifetime implementation only if bank ownership, first-dispatch execution
  debt, the complete DEAD-transition wrapper, nonblocking claimed finalizer,
  pre-unbind drain and a real owned-resource oracle arrived together. The
  existing `task_release_stack()` signature stayed void; the tentative bool
  signature was reverted. This prevented a silent DEFERRED return followed by
  unconditional `reclaim_ready` publication.
- E37, implementation/native baseline: a new private Win64 lifetime source
  built with MinGW GCC 15.1 Release in the isolated Windows host folder with
  two build jobs. The explicitly driven real scheduler oracle used the
  backend's CONTEXT/XSTATE bank buffers and acquired task/bank/binding
  references before suspension. Its detached last G reached a real scheduler
  DEFERRED state with the bank quota and G stack retained; the controller
  released after final bank access, and run/end reclaimed them. Four lifetime
  variants—last-G, peer progress, extra task-only scan, stop during defer—
  passed, as did never-started cleanup, the older real scheduler oracle and
  the two-capsule oracle. The last-G lifetime case passed 20 serial repeats.
- E38, first review counterexamples: `T-PRO-WIN-11` identified a host-join
  UAF window after `reclaim_ready`, a binding/wake UAF after the last op ref,
  and the need to prove that `FREE` with a positive external ref could not
  permit reuse or destruction. Pro explicitly withheld acceptance of the
  lifetime slice despite the passing fixture. Code inspection confirmed the
  first two hazards in the initial implementation; they were changed.
- E39, race repairs and native discriminating tests: the DEAD wrapper now
  takes a short scheduler scan ref before publishing readiness, finishes debt
  bookkeeping and detached-claim handling, then drops that ref as its final G
  access. The op finalizer now decrements binding refs and signals the shard
  while holding `opaque_lock -> shard->lock`; close cannot retire wake storage
  until the signal completes. Generic wake calls after task scan/public refs
  were removed. The idle path now waits with its existing 10 ms fallback even
  at zero live Gs while finalizer debt remains. `FREE && external_ops==0` is
  checked by both bank admission and destruction under the gate. A concurrent
  G-exit/pre-mutation-refusal CTest and a testhooks-only ready-publication
  host-join race CTest passed; the latter passed 20 short repeats. The gate
  only widens the race window and does not supply the task pin.
- E40, regression gates: native Windows opt-in CTests passed all seven new
  lifetime cases after the repairs, plus the original scheduler/two-capsule
  oracles and focused Windows policy, runtime smoke, IOCP I/O/dump, HANDLE I/O,
  runtime core and multi-runtime core tests. macOS `make -j4 test` passed after
  satisfying the repository's static lock/worker-boundary checker, including
  ABI/export audits and runtime stress/fuzz. `make check-license-policy` and
  `git diff --check` passed. These checks validate the exercised lifetime
  paths, not automatic Windows capture or all supported register/XSTATE bits.
- E41, final review pending: `T-PRO-WIN-12` sent the precise repairs, bounded
  finalizer wait decision and unresolved-mutation policy question to the same
  Pro conversation. Do not treat silence or fixture success as approval of
  the lifetime slice or the broader Windows backend.
- E42, twelfth Pro decision: the reported readiness pin, binding-gated
  decrement+notification, and `FREE && external_ops==0` two-part predicate
  closed WIN-11's three normal-path counterexamples at protocol level. Pro
  accepted the normal-path lifetime structure but required a production-owned
  process-terminal outcome for uncertain remote context/suspension state;
  retained refs plus `llam_record_fatal()` alone could strand run/destroy.
  Pro selected `RaiseFailFastException` and a bounded parent/child oracle.
- E43, fail-fast implementation and native child evidence: the private
  lifetime source now calls `RaiseFailFastException(NULL, NULL,
  FAIL_FAST_GENERATE_EXCEPTION_ADDRESS)` from its unresolved committed-op
  branch, with no fabricated reference drain or normal shutdown. The CTest
  parent launches the real lifetime oracle in control, post-capture-mutation
  and post-restore-mutation modes. Both terminal children exited with exact
  Windows `STATUS_FAIL_FAST_EXCEPTION` (`0xC0000602`); the control completed
  four pre-mutation refusals and a valid restore with status 0. Parent test
  passed in 0.35 seconds. A timeout or a different crash status fails the
  test. The [Microsoft API contract](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raisefailfastexception)
  documents process termination and bypass of frame/vectored handlers.
- E44, disabled-path regression reduced and repaired: after a native opt-in
  rebuild, `test_runtime_core` timed out at both 10 and 5 seconds, while 13
  other focused cases passed. The new handoff scan ref had been taken for
  capsule-less ordinary tasks, but their M-side detached reclaim still took
  the original synchronous scan-ref wait, deadlocking on that self ref.
  Capsule-less Gs now use the original release/reclaim path. Native rerun
  passed `test_runtime_core` in 0.20 seconds and retained the lifetime oracle
  and ready-race passes.
- E45, final native matrix after that fix: 17/17 focused Windows CTests passed
  serially in 2.84 seconds in the isolated Release `build-lifetime` tree with
  two-job builds. This includes the fail-fast subprocess test, seven lifetime
  positive/race cases, two prior scheduler/two-capsule oracles, runtime core,
  multi-runtime core, Windows policy/smoke, IOCP I/O/dump and HANDLE I/O.
  `host-logs/windows-lifetime-final-17-ctest.log` retains the raw CTest log.
- E46, thirteenth Pro review: `T-PRO-WIN-13` accepted the R1–R4 lifetime slice
  for explicitly driven one-M Win64/MinGW operations behind the experimental
  flag, based on the reported source protocol and native oracles. It found no
  further concrete lifetime blocker at that scope. Pro did not independently
  inspect the source or run the tests, and explicitly withheld any claim that
  automatic Windows preemption is implemented or accepted. Full context,
  mitigation, public destroy and multi-M qualification remain open.
- E47, model handoff: the specified existing ChatGPT conversation later showed
  Pro unavailable and Extra High selected. The user explicitly authorized
  continuing with Extra High. `T-PRO-WIN-14` was named in the message body but
  was in fact an Extra High response; subsequent turns are `T-XHIGH-WIN-15`
  through `T-XHIGH-WIN-18`. None independently inspected source or ran tests.
- E48, R5 production transaction: the private Win64 transition assembly is
  now compiled once per opt-in runtime library, not duplicated in testhooks;
  ordinary context-switch assembly is untouched. The production early-return
  seam preserves G errno/LastError before restoring scheduler-local state,
  and scheduler return alone publishes CAPTURED→READY/requeues. The exact
  suspended RIP is admitted only in an immutable registered safe range.
  An explicitly driven native transaction linked against the *ordinary*
  `llam_runtime` library (not testhooks) passed capture→M return→peer progress→
  fresh gate→full G restore, both ordinary and stop-at-gate variants; each
  passed ten serial repeats. The production safe-PC oracle linked against the
  same ordinary library passed registered interval, two adjacent unsafe
  regions, ordinary C, gate, generation mismatch and empty registry cases.
- E49, repeatable context witness: after successful restore, the controller
  can suspend the same M again, read its exact live G context without changing
  it, publish RESTORING→OBSERVED, resume unchanged, and only then clear the
  pending bank under `opaque_lock -> shard->lock` and drop bank/task/binding
  references. The witness checks exact pending bank/generation, restored
  task/worker dispatch generations, binding, active managed G, G/native stack
  bounds and saved scheduler RSP. Capture still requires certified RIP;
  read-only witness does not. A native four-cycle no-safepoint fixture proved
  alternating banks, preserved cooperative G.ctx, target-local errno/
  LastError, peer progress on each capture and no scheduler return between
  restore and direct witness. It passed 20 serial repeats. A separate
  post-restore unregistered-PC witness passed without a new context mutation.
- E50, automatic controller: the existing single watchdog thread owns one
  private operation and services OBSERVED/GATE/RESTORING before considering a
  new timeslice. It makes at most one capture attempt per minimum 5 ms
  quantum, with no catch-up burst, and skips only its ordinary cooperative
  signal while a remote operation is pending. A host *monitor* using a
  channel-parked peer never called capture/restore; nevertheless the same
  no-safepoint G was redispatched at least four times. Private counters
  confirmed at least four real capture/restore transactions and three direct
  witnesses, with peer progress and zero remaining capsule quota after run.
  The native automatic CTest passed 20 serial repeats. An automatic stop
  requested immediately after the first committed capture still serviced its
  restore obligation and passed 20 repeats.
- E51, automatic refusals: a native unsafe-PC fixture with STRICT policy
  sampled a registered-image G only after it moved to an unregistered
  assembly interval. Its private metrics observed at least one unsafe RIP
  refusal, a bounded 60 ms attempt increment (at most 16), zero captures and
  restores, and complete rollback. Empty-registry and NO_PREEMPT controls
  made zero remote context probes or mutations. The unsafe and empty tests
  each passed ten serial repeats; all three passed in the final matrix.
- E52, immutable code backing: `VirtualQuery` now requires MEM_IMAGE and
  matching `AllocationBase` for each certified range. At enablement
  `GetModuleHandleExA(FROM_ADDRESS)` increments the PE image's loader ref;
  runtime shutdown releases it with `FreeLibrary` only after operation and
  capsule debt drain. A native DLL oracle dropped its own LoadLibrary handle,
  still found the image mapped while LLAM held its ref, then observed it
  unloaded after LLAM shutdown. The same oracle rejected executable
  MEM_PRIVATE/JIT allocation with ENOTSUP. It passed ten serial repeats.
  The [Microsoft module-handle contract](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandleexa)
  documents the ref increment and required FreeLibrary balance. Intentional
  in-place modification of registered code remains outside this private
  certification contract.
- E53, final gates: native Windows 10 x64 MinGW GCC 15.1 Release, isolated
  folder, `cmake --build build-lifetime --target ... -j 2`, followed by serial
  `ctest --test-dir build-lifetime --output-on-failure --timeout 20` selecting
  Windows async, runtime core/multi-runtime/shutdown, Windows policy/smoke,
  IOCP I/O/dump and HANDLE I/O: **33/33 passed in 3.92 s** after the final
  image pinning change. Cross-MinGW `-Wall -Wextra -Wpedantic -Werror`
  syntax check of touched Windows C files passed. A default-OFF Win64 cross
  shared build passed with neither async source in the shared target nor
  private async exports; the opt-in native DLL likewise exported no private
  `llam_windows_async*` symbols while retaining public `llam_run` exports.
  macOS `make -j4 test` passed after the Windows-only code changes. The
  shared Windows host was checked before short runs: observed CPU about
  7–19%, free physical memory about 10–16 GiB. No large stress or soak run.
- E54, Extra High review boundary: `T-XHIGH-WIN-15` identified the one-shot
  pending-restore coupling; `T-XHIGH-WIN-16` accepted the simpler staged
  witness in principle and required separating read-only witness eligibility
  from certified capture eligibility; `T-XHIGH-WIN-17` raised stop-after-
  witness, per-gate identity and code-mapping lifetime questions. The actual
  scheduler dispatches runnable Gs even after stop and does not abandon
  their stacks, so an extra drain recapture could not terminate an infinite
  no-safepoint leaf. `T-XHIGH-WIN-18` withdrew that blocker and accepted the
  existing cooperative cancellation limit. It also agreed that the exact
  pending bank generation is itself the one-to-one save-and-gate token while
  bank reuse and simultaneous pending restores are forbidden. The final
  mapping pin/negative tests and 33-case matrix were completed locally after
  those reviews, not independently performed by ChatGPT.
- E55, final narrow Extra High classification: `T-XHIGH-WIN-19`, in the same
  user-designated conversation after Pro became unavailable, found no further
  concrete correctness counterexample for the **reported** default-off,
  one-primary-M, MinGW, manually certified immutable PE-image-code automatic
  backend. It explicitly did not claim source inspection, independent test
  execution, Pro-model approval, general Windows support or a performance
  gain. Residual exclusions include in-place code patching, arbitrary C/C++
  safety, bounded cancellation of an uncooperative G, multi-M and wider
  XSTATE/mitigation qualification.
- E56, separate static-check caveat: `make check-license-policy`, the lease
  boundary checker and `git diff --check` passed. A standalone invocation of
  `scripts/check_worker_identity_boundary.py` reported six direct `active_g`
  references in existing `src/core/lifecycle/init.c`,
  `src/core/sched/scheduler.c` and `src/internal/runtime_types.h` lines not
  edited by this Windows continuation. This is a failed repository diagnostic,
  not a passing Windows gate; it was not papered over by changing unrelated
  Phase 0 ownership code. `mkdocs build --strict` was unavailable locally
  because `mkdocs` is not installed.
- E57, public destroy source order: host `llam_runtime_destroy_rt` claims the
  explicit handle under the global lifecycle lock, requests stop and polls
  `exec_started` with acquire loads and `llam_pause_cpu()`; it does not sleep
  on a notification. The borrowed run clears that token with a release store
  only after scheduler-loop unbind, Windows `run_end`, and its final runtime
  access. `shutdown_unlocked` does not publish `shutdown_requested` or join
  the watchdog until the active run has left. Neither the run tail nor
  controller `op_finish` takes the global lifecycle lock. This was checked
  from `shutdown.c`, `run.c` and `watchdog_worker.c` before constructing the
  native test.
- E58, bounded destroy oracle A–D: new testhooks-only gates hold no production
  lock and stop at pre-first-dispatch, CAPTURED→READY before reinjection,
  native GATE before restore, and after successful remote restore before
  witness. A separate host thread calls real public `llam_runtime_destroy()`
  on an explicit runtime, and the test verifies destroy requests stop but
  cannot finish through the held run. The G is a certified no-call assembly
  leaf with an independent host release word. Private snapshots require
  bank READY/GATE/RESTORING, `binding_ops==1`, exact pending presence and
  execution debt where appropriate. In D the G stays busy until the real
  direct witness has cleared pending restore and the final binding operation,
  then exits. All four CTests passed; each passed 20 serial `--repeat
  until-fail:20` iterations (5.29 s total for the repeat batch).
- E59, second native M and destroy: the same explicit runtime completed an
  automatic capture/restore/direct-witness run with zero capsule charge,
  binding/pending/execution/retirement/finalizer debt. Its original borrowed
  caller thread remained alive but idle. A different concurrent OS caller
  then drove run 2, with distinct thread ID and strictly increased worker
  incarnation/run generation, reached a new post-restore pre-witness gate,
  and survived public destroy from a third host thread. This fifth CTest
  passed ten serial repeats in 2.60 s. It does not inject a stale first-run
  operation; the older native scheduler oracle covers stale incarnation
  refusal separately. It is sequential rebind, not simultaneous multi-M.
- E60, final focused Windows matrix after A–E: native Windows 10 x64, MinGW
  GCC 15.1 Release, isolated `build-lifetime`, `cmake --build ... -j 2`, then
  serial `ctest --test-dir build-lifetime --output-on-failure --timeout 20`
  selecting Windows async tests plus runtime core/multi-runtime/shutdown,
  Windows policy/smoke, IOCP I/O/dump and HANDLE I/O: **38/38 passed in
  4.99 s**. The opt-in shared DLL also rebuilt. The server was checked first
  at about 10% CPU with about 10 GiB free physical memory; no large stress
  or soak case was run.
- E61, Extra High review correction: `T-XHIGH-WIN-20` proposed A–E;
  `T-XHIGH-WIN-21` initially called B an idle-controller READY case and
  suggested a notification lost wake for `exec_started`. Actual test/source
  shows B still owns its watchdog-stack operation with `binding_ops==1`,
  while destroy acquire-polls `exec_started` without a sleep/notification
  handshake. `T-XHIGH-WIN-22` withdrew both premises and accepted only the
  corrected one-M A–E public-destroy overlap qualification. Neither review
  inspected source independently or executed tests. Activation registration
  races, failed controller startup, and concurrent multi-M remain untested.
- E62, final continuation checks: after the A–E changes, local macOS
  `make -j4 test_runtime_core && ./test_runtime_core` passed. The Win64
  default-OFF cross shared target reconfigured and built with no private
  Windows async export, `make check-license-policy` passed with untracked
  notices included, the lease-boundary checker passed, and
  `git diff --check` was clean. The native opt-in DLL did not export the new
  test-only phase-gate symbol. The shared Windows host was checked after the
  short runs at about 9% CPU and 15 GiB free physical memory. The earlier
  standalone worker-identity checker caveat and unavailable MkDocs tool
  remain unchanged.
- E63, private pre-run configuration race: direct raw-runtime dereferences in
  Windows safe-range registration and enablement previously overlapped public
  destroy; the no-live-task check also had a gap before host spawn published
  its task. The Win64 experimental path now takes an existing public-op pin
  before touching runtime storage, serializes register/enable through a
  per-runtime mutex, and temporarily CAS-claims `exec_started` to exclude
  run start. Host/foreign spawn reserves under that same mutex before task
  allocation and releases its reservation only after complete publication or
  failed cleanup. Same-runtime managed spawn remains lock-free on this gate;
  cross-runtime managed spawn returns `EBUSY` before allocation when its
  nonblocking trylock fails. Enabled first dispatch asserts capsule presence.
- E64, deterministic Win64 configuration oracle: six independent CTests cover
  spawn-first, enable-first (including run refusal), enable/destroy,
  run-first, cross-runtime managed trylock refusal, and an injected control
  thread startup failure followed by successful fresh create/enable. Each
  passed 20 short serial repeats on native Windows 10 x64 MinGW GCC 15.1.
  The test-only pauses do not create public activation or ABI; the enable
  pause intentionally owns the configuration mutex while exercising the
  exclusion protocol. The shared Windows host was at about 10% CPU and had
  about 11 GiB free physical memory before the two-job build.
- E65, final focused regressions after configuration hardening: every Windows
  async target was rebuilt, then the native async CTest matrix passed **36/36
  in 3.00 s**. Native runtime core, multi-runtime core and Windows smoke each
  passed separately. Local macOS runtime core and multi-runtime core passed,
  followed by the full `make test` gate. Strict cross-MinGW
  `-Wall -Wextra -Wpedantic -Werror` syntax checks passed.
  A default-OFF Win64 shared DLL rebuilt, with no Windows async symbol in
  its export table. The opt-in native shared DLL also rebuilt, and its PE
  export table contained no private Windows async/configuration or testhook
  symbol. `make check-license-policy` and `git diff --check` passed. After
  the short validations the shared host reported about 37% CPU and 8.9 GiB
  free memory; no further load was run and the SSH control session was
  closed.
- E66, review boundary: in the user-designated ChatGPT conversation,
  `T-XHIGH-WIN-23` specified the narrow pin/config-mutex/synthetic-run-token/
  host-reservation protocol. `T-XHIGH-WIN-24` accepted the reported private
  implementation conditionally on cross-runtime managed trylock failure
  aborting before allocation; the source and sixth deterministic test verify
  that condition. Extra High did not inspect source or execute the tests, and
  Pro remained unavailable. Neither review approves public activation,
  simultaneous multi-M, arbitrary code capture, or a performance claim.
- E67, actual controller startup ordering: `init.c` creates `ctrl_thread`
  with `llam_ctrl_worker_main`, checks its creation result, marks it started,
  then publishes `initialized=true`. Successful creation does not prove the
  OS has scheduled its first loop. A source search found the only production
  call to `llam_windows_async_lifetime_try_capture_primary()` in
  `llam_watchdog_windows_async_tick()`; no other production thread can create
  a half-admitted automatic capture before the controller begins ticking.
- E68, delayed-controller native oracle: a testhooks-only first-action gate
  pauses the controller before tuning or loop entry, with no production lock
  or async operation owned. A certified bounded G runs and exits on its
  independently released host word while the controller has executed zero
  ticks. First dispatch legitimately has `execution_debt==1`; capture
  attempts, binding operations and pending restore remain zero. Run 1 drains
  to zero debt before the gate opens. After at least three real controller
  ticks with no run, capture/restore counts remain zero; a different borrowed
  caller M then drives run 2 and gets a real automatic capture, restore and
  direct witness, followed by normal zero-debt completion/destroy. This
  single CTest passed 20 serial native Windows repeats, and all freshly
  rebuilt Windows async CTests passed **37/37 in 3.15 s**. Native runtime
  core, multi-runtime core and Windows smoke also passed.
- E69, final checks and caveat: strict changed-file MinGW warnings-as-errors
  syntax, default-OFF cross DLL, opt-in native DLL and its absence of private
  controller-gate exports, Apache license policy and diff check passed. The
  first local macOS full `make test` failed only the legacy-lease internal
  helper deadline while a separate local cross build was running; that
  focused binary passed alone and the complete `make test` passed on retry
  after the cross build ended. The shared Windows host was checked at 13% CPU
  and 10.4 GiB free before the two-job build and 16% CPU/10.2 GiB free after;
  no large stress or soak was run, and SSH was closed. `T-XHIGH-WIN-25` in
  the user-designated conversation classified this as useful startup/
  lifetime characterization, not a newly required one-M correctness proof;
  it did not independently inspect or execute the code. Pro remained
  unavailable.
- E70, public expert opt-in contract: one additive ABI-2.1 symbol now takes a
  caller-sized v1 options struct with an explicit process-wide fail-fast
  acknowledgement and up to 128 caller-certified PE-image PC intervals.
  Windows experimental enablement is one transaction under the existing
  runtime public-op pin, configuration mutex, synthetic run token and
  host-spawn reservation check. It copies/validates all ranges, reuses PE
  executable backing and loader-reference checks, and rolls back the entire
  staged range table/generation on any error. Managed callers fail before
  configuration lock acquisition. Success freezes the registry until runtime
  shutdown; default-OFF and non-Windows builds retain the public symbol but
  return `ENOTSUP`. No Windows metadata emitter or arbitrary-C safety claim
  was added.
- E71, native public entry qualification: the production-linked public oracle
  obtained a real automatic capture, restore and direct witness from a
  manually certified bounded PE-image leaf, then drained and destroyed the
  runtime. Negative cases covered absent acknowledgement, short options,
  unknown flags, empty/overlapping ranges, oversize count, executable
  `MEM_PRIVATE` mapping, OFF policy, atomic retry after rejection and
  `EALREADY`. Four separate testhooks-only public-entry races exercised
  spawn-first, enable-first/run exclusion, enable/destroy and managed caller
  refusal. Both public-oracle modes and all four public race modes passed 20
  short serial repeats. Every native Windows async CTest was rebuilt and the
  combined matrix passed **43/43 in 3.50 s**; native ABI contract/compat,
  preemption capability, runtime core, multi-runtime core and Windows smoke
  also passed.
- E72, dynamic-load/default-OFF verification: `test_shared_load` previously
  used POSIX `dlfcn.h` and could not compile on native Windows. Its loader
  path now uses `LoadLibraryA`/`GetProcAddress`/`FreeLibrary` on Windows and
  remains `dlopen`-based elsewhere; it requires the new public symbol and
  forbids private Windows async/test-gate exports. It passed on native
  experimental and default-OFF Windows builds and on macOS. PE export-table
  inspection found the new public symbol in both DLLs and no private helper.
  Native default-OFF ABI, preemption capability and Windows smoke tests passed;
  the default-OFF capability test specifically exercised the `ENOTSUP` stub.
- E73, local and host gates: strict MinGW changed-file compilation, macOS ABI,
  preemption capability, shared-load and exact shared-export audit passed.
  The first local full `make test` attempt failed an unrelated worker-identity
  helper `sigaltstack EINVAL` expectation; its isolated binary passed and the
  complete `make test` passed on retry. The shared Windows host was at about
  29% CPU/9.7 GiB free before the two-job build and 31% CPU/9.2 GiB free
  after. No large stress/soak, commit, push, PR or deployment was performed.
  `make check-license-policy` and `git diff --check` passed; MkDocs remained
  unavailable locally.
- E74, public publication-gate source audit: `T-XHIGH-WIN-27` conditionally
  accepted the reported public implementation and asked for proof that no
  runtime storage is dereferenced before `begin_public_op`, and that every
  lock-free range consumer acquires one final `async_preempt_enabled` flag.
  Source inspection confirmed the pre-pin managed check reads TLS only.
  Watchdog, run-begin and task-init already acquire-gated their registry
  reads. Operation begin, task reclaim and private diagnostics now also
  acquire-gate control/range access; the enable release store is documented
  as the final publication edge. Run-end/shutdown readers are lifecycle-
  exclusive. This audit found a narrower prior-run issue: after live tasks
  drain, an older task handle can still be reclaimed concurrently. Public
  opt-in now requires `primary_worker.run_generation==0` under its config
  claim, making its documented first-run-only contract enforceable. A native
  negative oracle drains an ordinary first run, then requires `EBUSY` with
  no partial ranges; the final rebuilt async matrix again passed **43/43**,
  public success/rejection each passed 20 repeats, and native runtime core,
  multi-runtime core and shared DLL load passed. Post-validation shared-host
  reading was about 31% CPU with 8.8 GiB free; SSH was closed.
- E75, corrected Extra High disposition: `T-XHIGH-WIN-28` accepted both
  reported source conditions and the first-run-only restriction for the
  narrow expert public contract. It distinguished enabled's acquire/release
  publication guarantee from runtime object lifetime: public observers still
  need a public-op pin or an exclusive run/lifecycle interval before reading
  control storage. The actual public stats API uses the existing lifetime
  gate; private test diagnostics are used only while the run/runtime remains
  rooted. This was a protocol review from our source audit and test reports,
  not independent inspection/execution or Pro approval. PE metadata tooling,
  concurrent multi-M, arbitrary-code certification and performance benefit
  remain unqualified.
- E76, PE relocation and bounded collector: a MinGW cross-build emitted two
  `IMAGE_REL_AMD64_REL32` relocations in a hand-authored `.llampm` LLPM v1
  record. The linked DLL kept a 48-byte read-only final section, and its
  range-start field displacement was anchored at the actual loaded field
  address. The native positive oracle decoded the exact exported begin/end
  PCs. The collector holds a loader reference before any image read, bounds
  PE headers/section counts and bytes, checks each mapped span with
  `VirtualQuery`, invokes the existing LLPM v1 parser in place, rejects
  non-executable or overlapping selected ranges, and copies the result.
- E77, native collector negatives and handoff: ten serial CTests on the
  supplied Windows 10 x64 MinGW Release host passed (positive, absent,
  malformed magic/version/length/relative address/overlap, invalid handle,
  temporary pin and public handoff). The handoff test dropped caller and
  collector refs, observed a real watchdog capture, restore and direct
  witness within the bounded PE leaf, then observed module unload after
  runtime destruction. All rebuilt Windows async CTests passed **53/53 in
  3.47 s**. This is structural metadata provenance, not semantic proof of
  code safety or an automatic public activation path. The two-job Windows
  full all-target build stopped in the unrelated Windows-incompatible
  `test_legacy_lease_internal` test; the runtime and all selected async
  targets built successfully. No large host load or stress test was run.
- E78, focused ABI/default-off/local gates: native opt-in ABI contract and
  compatibility, preemption capability, runtime core, multi-runtime core,
  runtime shutdown, Windows smoke and shared-load CTests each passed. The
  native default-OFF build passed shared-load, preemption capability and ABI
  contract. The shared-load oracle now checks that both private PE collector
  functions are absent from the DLL export table. A strict MinGW
  `-Wall -Wextra -Wpedantic -Werror` syntax check of the new collector and
  fixture/oracle C sources passed. On macOS the existing LLPM parser and
  registration binaries, shared-load check, 131-symbol export audit,
  license-policy check and `git diff --check` passed. MkDocs validation was
  unavailable because `mkdocs` is not installed locally.
- E79, Extra High conditional and source audit: `T-XHIGH-WIN-30` saw no
  concrete loader-ref/handoff counterexample from our report but conditioned
  R7 on checking the entire loaded metadata span as readable committed
  `MEM_IMAGE` in the pinned module. In
  `src/core/context/windows_async_pe_metadata.c`, the widened
  `VirtualAddress + VirtualSize` comparison bounds the section against
  `SizeOfImage`; `metadata_bytes=min(VirtualSize,SizeOfRawData)` cannot exceed
  that span; checked base-plus-RVA and `llam_windows_pe_readable()` walk the
  whole selected payload before any parser read. `memcmp` uses all eight name
  bytes, and the reused LLPM parser checks signed relative address
  resolution. This closes the reported condition by local source audit, not
  by independent Extra High execution or Pro approval.

## R8 compiler/linker probes, 2026-09-27

- E80, isolated LLVM IR COFF relocation probe: Apple clang 21 cross-targeted
  a four-byte constant `trunc(ptrtoint(@leaf)-ptrtoint(@map))` to
  `x86_64-w64-windows-gnu`. `x86_64-w64-mingw32-objdump -r` showed one
  `IMAGE_REL_AMD64_REL32` relocation in `.llampm` with initial field bytes
  `04 00 00 00`. Without section GC, the linked DLL held `.llampm` at
  `0x35d585000`, leaf at `0x35d5822f0`, and field bytes `f0 d2 ff ff`:
  interpreted as signed `-0x2d10`, exactly leaf minus field address.
  This validates that particular IR difference expression and GNU MinGW
  final link for a start field, not function-end discovery or the full pass.
  Reproduce from `/tmp/llam-r8-relative-probe.ll` with
  `clang -target x86_64-w64-windows-gnu -c`, `objdump -r`, and
  `x86_64-w64-mingw32-gcc -shared`.
- E81, fatal GC control for an unassociated map: linking the same object
  with `x86_64-w64-mingw32-gcc -shared -Wl,--gc-sections` removed the
  `.llampm` output section, while the leaf remained in this specific DLL
  because MinGW auto-exported its external symbol. The object did contain
  `@llvm.used` for the map. Thus `llvm.used` alone did not make this map
  survive the observed GNU PE section GC; a compiler/linker association or
  explicit retention strategy needs a separate qualification test. This
  does not yet establish behavior for the full LLAM pass, MSVC link, or a
  genuinely dead leaf.
- E82, COFF associative COMDAT probe under GNU MinGW section GC: an isolated
  LLVM IR file put the leaf in `$llam_comdat_leaf = comdat any` and an
  internal `.llampm` map in `comdat($llam_comdat_leaf)`. Apple clang 21
  emitted a PE object whose `.text$llam_comdat_leaf` was select-any COMDAT
  and whose `.llampm` section had COFF associative selection 5 and parent
  section 4. Its record retained `IMAGE_REL_AMD64_REL32`. Despite that
  object-level association, GNU MinGW ld 2.46 with `--gc-sections` removed
  `.llampm[llam_comdat_map]` while the leaf remained auto-exported in the
  DLL; `--print-gc-sections` named the removed section. This disproves a
  simple claim that LLVM IR COMDAT association automatically preserves the
  map for a live leaf under this tested GNU PE linker. It may behave
  differently with LLD/MSVC, which are not installed locally. Reproduce
  from `/tmp/llam-r8-comdat-probe.ll` with Apple clang COFF cross-target,
  `x86_64-w64-mingw32-objdump -t`, and MinGW GCC `-shared -Wl,--gc-sections`.
- E83, ordinary Release codegen endpoint control: Apple clang 21
  cross-targeted `/tmp/llam-r8-codegen-probe.c` to x86-64 COFF at `-O2`
  and at `-O2 -ffunction-sections`. Both emitted actual leaf/branch machine
  bodies, and the latter made one `.text$<function>` COMDAT per body, but
  neither emitted a usable `.Lfunc_end*` label in assembly. Repeating
  `-O2 -g -ffunction-sections` did emit `.Lfunc_end0` and
  `.Lfunc_end1` for debug info. Thus a plain optimized Release build cannot
  rely on those labels without a separate supported codegen mechanism;
  simply guessing the debug-generated spelling is not a valid R8 emitter.
- E84, both LLPM fields in a complete IR record: Apple clang 21 compiled
  `/tmp/llam-r8-two-fields.ll` into a 48-byte `.llampm` record with
  `IMAGE_REL_AMD64_REL32` relocations at offsets `0x20` and `0x24`; both
  object addends were `4`. A separate tiny assembly object defined exact
  begin/end symbols. GNU MinGW linked the DLL without GC: map
  `0x379335000`, begin `0x379331360`, end `0x379331366`; final fields were
  `40 c3 ff ff` and `42 c3 ff ff`. Field address plus signed field value
  resolved exactly to begin and end, respectively. This is linked-image
  static evidence for LLVM IR expression lowering, not yet a native loaded
  R7 collector run and not a compiler-generated end-label solution.
- E85, stronger duplicate-policy control: changing the isolated function
  COMDAT from `any` to LLVM IR `nodeduplicate` emitted a COFF leader with
  selection 1 (`one_only`) and the same section-4 associative metadata
  follower (selection 5). GNU MinGW ld 2.46 `--gc-sections` still removed
  the final `.llampm` while auto-export kept the function live. Therefore
  the E82 failure is not limited to select-any's permissive duplicate
  semantics. This remains an isolated test of one compiler/linker version.
- E86, qualified-linker candidate experiment: Homebrew LLVM/LLD 23.1.2
  were installed locally from official Homebrew bottles (LLVM 1.9 GB,
  LLD 6 MB; Homebrew also upgraded `xz`, installed `z3`, and automatically
  cleaned older caches and the prior `xz` cellar version). A minimal
  LLVM COFF assembly object contained two one-only function leaders, each
  followed by a distinct `associative` `.llampm$M` contribution with a
  complete 48-byte LLPM record, two REL32 relocations, and alignment four.
  `lld-link /dll /noentry /opt:ref /export:llam_gc_live` produced one six-byte
  `.text` function and exactly one 48-byte final `.llampm` record; the dead
  function and its metadata were absent. Exporting both produced twelve
  bytes of code and exactly 96 adjacent `.llampm` bytes, with no inter-record
  gap. This is a decisive LLD object/link control for associative ownership
  and record concatenation; it is not compiler-generated codegen metadata or
  a native loaded-collector test. Inputs: `/tmp/llam-r8-two-comdat.S`;
  compiler `/opt/homebrew/opt/llvm/bin/clang -target x86_64-w64-windows-gnu`,
  linker `/opt/homebrew/opt/lld/bin/lld-link`.
- E87, native loaded-image R7 control for LLD: a reassembled version of the
  same associative two-function fixture exposed tiny begin/end getter
  functions and was linked with LLD 23.1.2 `/dll /noentry /opt:ref
  /opt:noicf`, exporting only the getters. The DLL had one 48-byte final
  `.llampm` record (SHA-256
  `2f861991f092e20a6f41f1d558b0497adec61496f88ef9e93affc3d25fb72130`).
  On the supplied native Windows 10 x64 host, the existing production-linked
  `test_windows_async_pe_metadata_oracle.exe` returned `positive passed`
  and `pin passed` when loading that LLD DLL. The positive mode checked the
  collected range against the DLL's exported begin/end getters; pin mode
  checked loader-ref survival and unload. Host check before transfer showed
  17% CPU load and 11,683,860 KiB free memory; no stress test was run.
  This validates LLD-built loaded PE acceptance and lifetime, but the LLPM
  bytes still came from hand-written assembly, not the LLVM pass.
  After the short tests the shared host showed 40% CPU and 10,889,216 KiB
  free; no further remote workload was started and the SSH session closed.
- E88, matching LLVM 23 tooling baseline: after installing Homebrew
  LLVM/LLD 23.1.2, the existing LLAM IR pass initially failed to build
  because LLVM 23 moved `PassPlugin.h`, changed the optimizer-last callback
  signature, and deprecated/replaced the old branch class. A small
  first-party compatibility edit in `tools/preempt/llam_preempt_pass.cpp`
  now builds with `clang++` under `-Wall -Wextra -Wpedantic -Werror` against
  LLVM 23.1.2. The resulting host plugin loaded into matching clang 23 and
  cross-compiled the existing restrictive annotated leaf to Linux x64 IR
  and ELF object. IR contained the expected `.llam.preempt` LLPM record,
  `__start_/__stop_` references, and safe function section; `llvm-readobj`
  confirmed both ELF sections in the object. This checks current Linux
  behavior under LLVM 23, not Windows emission or older LLVM versions.
- E89, stock plugin attachment audit against installed LLVM 23.1.2
  headers and its Homebrew-patched source: `PassPluginLibraryInfo` exposes
  `RegisterPassBuilderCallbacks(PassBuilder&)` and
  `PreCodeGenCallback(Module&,TargetMachine&,CodeGenFileType,stream&)`, but
  neither receives the actual `AsmPrinter`; the latter runs before normal
  codegen and can replace output, which would duplicate the pipeline.
  `CodeGenPassBuilder::buildPipeline` creates the actual printer internally,
  registers it as `AsmPrinterAnalysis`, then calls a target-specific
  `addAsmPrinterBegin`; ordinary IR plugin callbacks do not expose that
  point. Source audit found no stock `-fpass-plugin` entry path that attaches
  an `AsmPrinterHandler` before the standard X86 function emission while
  preserving that pipeline. This is a bounded negative for the selected
  LLVM 23.1.2 interface/profile, not a universal claim about LLVM plugins.
  The matched source places `markFunctionEnd()` after
  `emitFunctionBodyEnd()` and before jump tables, while
  `endFunction()` comes later. A temporary, option-gated marker-only
  registration probe was added to the unpacked X86AsmPrinter source outside
  LLAM; no production endpoint or metadata has yet been qualified.
- E90, E3-END marker-only non-Debug proof: Homebrew-patched LLVM 23.1.2
  source was unpacked from the official release archive with SHA-256
  `c98bbef08a2b4c2613cd50e9aa9ae7b69b1fe6c16b2c40373bc0ab6116fdf78a`.
  A temporary X86AsmPrinter-only experimental option registers an
  `AsmPrinterHandler` before functions emit; it selects an explicit
  `llam.preempt.probe` function attribute and emits a unique local label
  from `markFunctionEnd()`, without changing sections or producing LLPM.
  The patched X86-only `llc` built successfully in Release with LLVM
  assertions, SHA-256
  `ecc618ac801d64ec281e30f93867596440300aaf71ecc50134de0226acc1df26`.
  On optimized `-g0` x86-64 COFF IR at
  `/tmp/llam-r8-e3-fixture.ll`, the selected register leaf had its local
  endpoint at offset 8 in `.text$llam_e3_leaf`; a selected multiblock
  function had an early `ret` at offset `0x16`, later emitted code through
  a final `jmp` ending at `0xb7`, and its endpoint was at `0xb7` in the
  same `.text$llam_e3_multiblock` section. No endpoint appeared for the
  ordinary third function, and option-off output had no probe endpoint.
  Extracted machine-code bytes of all three sections had identical
  SHA-256 on/off (leaf `73eacf3f...`, multiblock `99ffb38e...`, ordinary
  `c826640c...`). Generated assembly used no `.Lfunc_end*` debug label.
  This is a matched-source, option-gated marker proof only: no compiler-
  generated LLPM record, late machine eligibility, or runtime integration.
  As an option-gate control, targeting x86-64 Linux with the same selected
  IR and option emitted no LLAM marker.
- E91, E3-LLPM-TRANSPORT prototype from actual codegen: the temporary
  option-gated LLVM 23.1.2 X86 handler now captures each manually selected
  fixture leaf's `CurrentFnSym` and actual one-only COFF code-section leader,
  emits a unique compiler-owned end symbol at `markFunctionEnd`, then from
  `endFunction` emits a complete 48-byte LLPM v1 record into an associative
  `.llampm$M` contribution. This developer-only `transport_fixture`
  attribute is explicitly manual fixture certification, not a general
  `ASYNC_SAFE` admission path. No runtime activation was invoked.
  The generated COFF input object SHA-256
  `4788bde51929fb2e76d044aeb50f7608dc4343fbfd933bf5038c660c2c9acd81`
  had three one-only code leaders for manually selected simple leaves,
  each with a 48-byte associative metadata follower (selection 5) referencing
  its actual parent sections 4/5/6, two `IMAGE_REL_AMD64_REL32` relocations
  per record, and one ordinary code section with no follower. The generated
  end relocations referenced their code-section symbols with offsets, not a
  guessed global name. A developer flag-off build had no `.llampm`, and all
  four extracted function-code byte hashes matched flag-on exactly.
  `lld-link /dll /noentry /opt:ref /opt:noicf` exporting only first leaf plus
  ordinary code retained one 48-byte final record and removed the second
  and third leaves plus metadata (map/output inspection). Exporting first,
  second and ordinary produced two contiguous records in one 96-byte
  `.llampm`; field-relative decoding matched exact final starts and
  8/9-byte ends. The first linked DLL SHA-256 is
  `efe0af339bbfe191b5172cb04570274ac8770b04d2a13d69f01ea0f53ccbfd10`,
  the two-record DLL SHA-256
  `255ca2a8e2812a26c10b473e0630ef8939ba11a11206a8f9a58927331dc4483b`.
  A short native Windows 10 x64 oracle linked to LLAM's production static
  library loaded each DLL and called the unchanged R7 collector; it reported
  `1 compiler-generated record(s) accepted` and `2 compiler-generated
  record(s) accepted`, checking exact exported starts, compiled 8/9-byte
  ends and exclusion of ordinary code. Host observations were 34% CPU and
  9,250,360 KiB free before, 29% CPU and 9,159,420 KiB free afterward;
  SSH was closed. This qualifies only manually certified fixture transport;
  late-machine eligibility, production emission, and automatic-preemption
  integration remain unverified.
  A separate same-name, different-body strong function object failed the
  qualified LLD link with `duplicate symbol: llam_transport_first`, so the
  compiler-generated one-only leader did not silently acquire select-any
  duplicate semantics in this test.
- E92, decisive late-machine counterexample: Homebrew clang 23.1.2
  compiled a pure annotated Win64 C function with five `uint64_t` arguments.
  Its optimized LLVM IR at `/tmp/llam-r8-late-negative.ll` contains only
  SSA integer additions and a return—no IR `load`, `store`, call, or
  alloca—so the current IR instruction predicate would not reject it for
  memory access if the Windows target were enabled. The actual optimized
  x86-64 Windows assembly at `/tmp/llam-r8-late-negative.s` contains
  `addq 40(%rsp), %rax`, reading the fifth argument from the stack.
  This directly shows why IR-only certification would be unsound for that
  ABI and supplies a targeted late-machine negative fixture. It does not
  show a spill introduced by register allocation; the source is the ABI
  argument location, so label the failure `incoming stack argument`.
  The matched patched `llc -stop-before=x86-asm-printer` MIR confirms
  `ADD64rm ... $rsp ... 40 :: (load (s64) from %fixed-stack.0)`, so the
  proposed handler's stage can inspect the actual memory opcode and fixed
  incoming stack object rather than guessing from assembly text.
- E93, finite CPU-loop candidate versus an adverse optimization shape:
  a one-argument integer recurrence loop in
  `/tmp/llam-r8-cpu-leaf.c` is IR-pure (no source memory/call). At ordinary
  `-O2`, the matched Win64 codegen unrolled it and introduced callee-save
  `PUSH64r`/`POP64r` sequences, which a strict late profile must reject.
  At optimized `-O2 -fno-unroll-loops -g0`, its final MIR before
  X86AsmPrinter had `stackSize: 0`, empty `fixedStack` and `stack`, no
  calls or memory operations, and a bounded set of integer register
  operations, local branches and returns (`TEST64rr`, `MOV32ri`,
  `IMUL64rri32`, `LEA64r`, `CMP64ri32`, `JCC_1`, `RET64`, plus the
  shown register arithmetic). This is a candidate for the later finite
  runtime integration workload after positive-opcode/operand review and
  late rejection tests; it is not yet compiler-certified or run under LLAM.
- E94, late-machine read-only/require prototype: the pinned X86 handler
  gained a diagnostic `win64-scalar-leaf-v1`-style predicate for explicitly
  marked probe functions. It checks scalar 32/64-bit signatures; a small
  exact X86 opcode allowlist and operand types; volatile integer registers
  only; numeric register-only LEA; local direct branch targets; canonical
  `RET64`; no unexpected stack/frame objects, calls, memory access, special
  register writes, unknown executable pseudos, split layouts, jump tables
  or prefix/prologue data. This is not yet connected to production
  `ASYNC_SAFE` emission. The latest temporary `llc` SHA-256 is
  `c149a4e04011c039f0670151ba8c810c079cab7f45a87bb1285185e6abf07783`.
  Read-only audit passed all three simple transport leaves. It refused N1
  as `MEMORY_OR_EFFECT ADD64rm`, N2 wide `udiv i128` as
  `CALL_OR_TAIL CALL64pcrel32` (MIR confirms a real `__udivti3` helper and
  72-byte frame), and N3 twelve-state recurrence as
  `MEMORY_OR_EFFECT PUSH64r` (MIR shows a 48-byte frame and six
  callee-save push/pop pairs; this is not evidence of a general allocator
  spill). The existing restrictive IR pass accepted the actual N1/N2/N3
  annotated inputs on Linux x64 with `llam.preempt.async_safe`; matched
  Win64 optimized IR bodies had no explicit load/store/call, while their
  target machine output exposed those new hazards. The Win64 IR pass target
  guard is still unchanged, so this is not an end-to-end Windows
  early-verifier→late-checker integration claim.
- E95, direct late-checker oracles: resuming generated MIR immediately before
  X86AsmPrinter with a `MOV64rr` writing RSP produced
  `SPECIAL_REGISTER MOV64rr`; inserting a real `ADD64rm` with no
  MachineMemOperand descriptor produced `MEMORY_OR_EFFECT ADD64rm`.
  Direct MIR substitutions of external `TAILJMPd64`, register-indirect
  `JMP64r` and `LRET64` produced `CALL_OR_TAIL` or `CONTROL_FLOW`
  refusals, not parser failures. An executable `PATCHABLE_FUNCTION_ENTER`
  pseudo was refused before its unsupported target lowering when the
  fail-closed require option was used; read-only audit alone logged refusal
  but let that unsupported target path abort, so only the require-mode
  result is a valid fail-closed gate. With `-basic-block-sections=all`,
  stock X86AsmPrinter failed before per-function callbacks; a constructor
  preflight under the require option now returns the explicit
  `LAYOUT basic-block sections` error before that failure. This is an
  option-level layout refusal, not proof of handling a split MIR function.
- E96, positive and mixed build checks: three straight-line leaves passed
  the body audit. A finite unsigned recurrence using register-only moves,
  XOR, constant shifts, LEA, comparisons, local branches and canonical
  returns passed with pinned `-O2 -fno-unroll-loops`,
  `-align-loops=0` and
  `-x86-experimental-pref-innermost-loop-alignment=0`; without the latter
  two settings it was refused for an in-range aligned block, so the
  no-padding profile is explicit. Its final `.text` is 0x45 bytes and
  disassembly shows no memory or stack instruction. In a mixed source with
  a good selected leaf, a bad selected fifth-stack-argument leaf, and an
  ordinary unselected function, audit reported PASS then REFUSE and did
  not report the ordinary function. Require mode exited nonzero on the bad
  leaf; when targeting a path holding an earlier successful object, `llc`
  removed that stale output. These tests cover the selected prototype
  objects, not a released compiler driver, semantic call-scope contract or
  real LLAM automatic-preemption workload.
- E97, operand-shape tightening after the initial Pro packet: the temporary
  checker now requires exact explicit operand counts and kinds for each
  admitted opcode, numeric-only target flags, an RAX/EAX value operand for
  canonical `RET64`, and the reviewed LEA tuple. The rebuilt `llc` SHA-256
  above reflects this revision. The three straight-line leaves and the
  no-alignment finite loop still pass the read-only audit; N1 stack
  `ADD64rm`, N2 helper `CALL64pcrel32`, and N3 callee-save `PUSH64r`
  still refuse with the same categories. The N4/N5 direct-MIR results
  were obtained before this tightening and need rerun on the final checker
  before any certification claim. No runtime output used the tightened
  checker; the transport-only fixture remains separately gated.
- E98, post-MIR emission closure controls for the pinned prototype:
  on an admitted leaf, `-mattr=+harden-sls-ret` left the inspected MIR
  register-only and initially logged `PASS_BODY`, but matched X86
  AsmPrinter output added `int3` immediately after `retq`. The predicate
  now rejects `Subtarget.hardenSlsRet()`/`hardenSlsIJmp()` as
  `EMISSION_PROFILE SLS hardening`; both global `-mattr` and a per-function
  `target-features=+harden-sls-ret` fixture produced that refusal. A direct
  accepted-MIR test positioned a two-byte JCC at offset `0x3f` by adding
  an otherwise allowed `MOV32ri`; LLVM's MC
  `-x86-align-branch-boundary=32 -x86-align-branch=jcc` inserted one
  executable `nop` at `0x3f` and moved the branch to `0x40`, growing the
  section from 0x4b to 0x4c while the prior body-only audit had passed.
  A narrowly added accessor to matched X86 MC options now makes the late
  profile reject branch/prefix padding modes before certification; audit
  reports `EMISSION_PROFILE MC branch/prefix padding`, and require mode
  fails. The alternate `-x86-branches-within-32B-boundaries` option also
  refused. This is a conservative pinned-profile guard, not qualification
  of padding as safe.
- E99, accepted local branch relaxation and bounded lowering map: an
  otherwise accepted single-function MIR inserted twenty harmless,
  allowlisted numeric `MOV32ri` forms in the loop body to force local JCC
  displacement outside 8-bit range. The checker still returned
  `PASS_BODY`; final COFF `.text` was 0xc5 bytes, with a near conditional
  branch at offset `0x3` targeting `0xbf` and another at `0xb5` targeting
  `0x17`. Both targets are decoded instruction boundaries inside the
  emitted interval; disassembly covers the section without unexplained
  gaps/NOPs. Matched X86 source's ordinary path lowers these non-special
  MIs via `X86MCInstLower::Lower` and `EmitAndCountInstruction`; the lowerer
  may use documented short-immediate/INC/MOV optimizations and MC relaxes
  local branches. Representative final disassembly covers the actually
  allowed forms: `MOV32ri`/`MOV64rr`→`mov`, `ADD64ri32`→`add`,
  `SUB64rr`→`sub`, `INC64r`/`DEC64r`→`inc`/`dec`,
  `XOR32rr`/`XOR64rr`→`xor`, constant `SHL64ri`/`SHR64ri`→`shl`/`shr`,
  numeric-register `LEA64r`→`lea`, `TEST64rr`/`CMP64ri32`→`test`/`cmp`,
  `JCC_1`→short or relaxed near `je`/`jne`, and `RET64`→`ret`.
  Unexercised `JMP_1`, `ADD64rr` and `CMP64rr` were removed from the
  prototype allowlist rather than treating their similar mnemonics as
  qualified. These tests do not cover every value/register combination or
  another LLVM/linker version.
- E100, first-party Windows IR diagnostic wiring: with
  `LLAM_PREEMPT_WIN64_AUDIT=1`, the existing restrictive LLVM pass now
  accepts annotated x86-64 Windows GNU/COFF input for IR checking only,
  attaches `llam.preempt.win64_ir_accepted`, and deliberately emits no
  `.llam.preempt` or `.llampm` record. The matching LLVM 23 plugin built
  warning-clean (SHA-256
  `7128ed3ab3296410f3b1f5066bd16ed270668b4e04ad83e37a10031ab72bc543`).
  The custom pinned `llc` diagnostic handler recognized that exact tag
  on the unchanged Win64 IR: a 64-bit register leaf and explicitly
  no-alignment finite loop reported `PASS_BODY`; actual Win64 N1 fifth
  stack argument reported `MEMORY_OR_EFFECT ADD64rm`, N2 wide division
  reported `CALL_OR_TAIL CALL64pcrel32`, and N3 callee-save pressure
  reported `MEMORY_OR_EFFECT PUSH64r`. This is now an actual Windows
  early-accepted→late-refused diagnostic path, not a Linux-target proxy.
  With the audit environment variable absent, required mode still rejects
  Win64; even with it set, an MSVC-target triple is unsupported. Linux x64
  still emitted its existing safe-function and `.llam.preempt` sections.
  A mixed Win64 source produced good PASS then bad REFUSE with ordinary code
  silent; require-mode failure removed the prior object at that output path.
  One old `unsigned long` example lowered to 32-bit Win64 `LEA64_32r` and
  was conservatively refused as an unknown form; an explicit `unsigned
  long long` fixture passed. No path from this diagnostic tag to a
  production `ASYNC_SAFE` certificate or runtime activation exists yet.
- E101, first compiler-certified LLPM emission through the actual pinned
  Windows pipeline: following T-PRO-WIN-37, the current late checker was
  rerun against direct MIR negatives. A register move or LEA writing RSP,
  a memory-form ADD with no MachineMemOperand, external tail transfer,
  register-indirect jump, far return and invalid JCC condition each reached
  the intended refusal. The temporary LLVM 23.1.2 X86 handler then gained
  explicit `-llam-preempt-certify-v1`, mutually exclusive with developer
  `-llam-preempt-probe-llpm`. The new mode requires both the actual source-
  annotation `llam.preempt.async_requested` and the first-party Windows IR
  verifier's `llam.preempt.win64_ir_accepted` tag; it recomputes the late
  body/profile result, retains actual entry/code-section/one-only leader,
  marks the unique final end in that code contribution, and emits one
  complete associative 48-byte LLPM v1 record only at `endFunction`.
  Refused selected functions fail compilation, not silently omit coverage.
  This handler modification remains in isolated temporary LLVM source,
  **not** yet a reproducible LLAM toolchain patch or runtime integration.
- E102, actual source-to-object/link matrix: matched clang 23 with the
  first-party pass compiled annotated WinGNU/COFF source into IR; the pinned
  custom `llc` SHA-256 is
  `7eda5cd7cf7b40a6d42ccc4a3f722b850f1db71b51937f49787dce0e183443c8`.
  A scalar 64-bit leaf and no-alignment finite recurrence produced certified
  COFF objects with one-only code contributions and 48-byte associative
  followers, each with two REL32 fields. A third annotated source contained
  accepted F1/F2/dead G plus ordinary H; H had no follower. LLD 23.1.2
  `/dll /noentry /opt:ref /opt:noicf` produced one-live and two-live DLLs
  with `.llampm` lengths 48 and 96 bytes; unrooted G code and metadata both
  disappeared, while live H remained outside the recorded ranges. The
  compiler-certified linked DLL SHA-256 values are:
  `394370867c487b3a8119b710d3a20821e6d26020919678d3fbbd3135608c7487`
  (one), `38a75be41ca8f38d5dce36e6847540ce4c40039b1c8e1e835f5d60e3813c5bf8`
  (two), and `9a8db93c58bef80339790fb5101e7ff9a18b846b6e30bd14dcc0ec84791b27fc`
  (finite loop plus scalar leaf). Exact linked field arithmetic resolved
  F1 `[0x180001000,0x180001008)`, F2 `[0x180001020,0x180001029)`,
  and the loop `[0x180001000,0x180001045)` in their respective images;
  the loop DLL's scalar leaf was `[0x180001060,0x180001068)`.
- E103, bounded native Windows R7 collector oracle for the new certified
  DLLs: the user-provided host reported 5,218,212 KiB free physical memory
  and 22% CPU before transfer. Four tiny artifacts were copied to the
  existing isolated validation directory. A strict MinGW executable linked
  with the current LLAM static runtime loaded each DLL, called the **unchanged**
  R7 PE collector, checked exact exported begin/end and ordinary-H exclusion,
  released the result, unloaded the DLL and checked its module handle was
  gone. Serial outputs were `1 compiler-generated record(s) accepted`,
  `2 compiler-generated record(s) accepted`, and
  `2 compiler-generated record(s) accepted` for one/two/loop respectively,
  all exit 0. Afterwards the host reported 5,462,052 KiB free and 24% CPU;
  SSH control connection was closed. This is collector-only evidence: no
  automatic preemption was activated on these generated ranges.
- E104, certification refusal and non-emission checks: actual Windows
  annotated N1 fifth-stack-argument, N2 generated `__udivti3` helper and
  N3 callee-save stack traffic were early accepted then late refused with
  `MEMORY_OR_EFFECT ADD64rm`, `CALL_OR_TAIL CALL64pcrel32` and
  `MEMORY_OR_EFFECT PUSH64r`; all certification builds exited nonzero and
  removed an existing output object at the same path. Good/bad selected
  ordering in both directions failed as a whole; ordinary functions did not
  inherit selected state. Audit-only emitted no LLPM; combined cert/transport
  modes, missing function sections/one-only owner, MSVC target and a selected
  function without the true early-accepted tag refused. Linux ELF emission
  remained observable. SLS return hardening, per-function SLS and both MC
  branch-padding option spellings remained explicit profile refusals; the
  qualified relaxed near-JCC positive remained accepted. These are narrow
  pinned-toolchain checks, not a claim about arbitrary LLVM/LLD versions,
  unqualified linking, dynamic caller scope or general Windows preemption.
- E105, R8-RUNTIME-1 bounded numerical and chain oracle: an executable linked
  to the current LLAM Windows test-hooks runtime loaded the compiler-certified
  finite-recurrence DLL, collected two exact LLPM ranges, and enabled a fresh
  one-M runtime through the public expert API with explicit process-fail-fast
  acknowledgement. It then released both the caller's and collector's loader
  references, retaining only the runtime's intended image ownership. Compute
  G called the certified recurrence **once** with a fixed runtime-supplied
  count, while ordinary peer G cooperatively yielded. A test-only bounded
  128-event observation log records capture, peer progress, restore, direct
  witness and function return by task key/bank generation/dispatch identity;
  it neither initiates a context transaction nor changes queue order. All
  trace slots are written once, release-published and acquire-read; overflow
  fails the oracle. The first chosen 30,000,000-iteration input returned the
  correct result `12664458136320821888`, but produced only **one** capture,
  restore and witness. It was recorded as insufficient preemption coverage,
  not a pass or context-corruption finding; a same-count diagnostic replay
  confirmed `captures=1 restores=1 witnesses=1`, no binding/bank/pending/
  execution/charged-byte debt.
- E106, single bounded longer input and controls: the fixed 120,000,000-step
  input returned independent-reference result
  `10386373181075607123` in both default-disabled and enabled modes. On the
  same compiler-generated image, the enabled run had event order
  `capture/peer/restore/witness = 0/1/2/3`, then again `4/5/6/7`, for
  distinct bank generations 1 and 2 and the **same one-call compute task**.
  Every captured RIP was inside the collector's 0x45-byte recurrence range,
  not the separately certified scalar leaf; both restore generations exceeded
  their capture generations. Three additional short positive repeats and a
  final four-mode run passed; reported RIP offsets varied within the range.
  The unannotated ordinary-H control returned 16 from input 7 with zero
  captures; because H is only five bytes long, this is structural exclusion
  plus zero observed mutation, **not** proof of a dynamic stopped-PC refusal.
  A deterministic second MEM_PRIVATE range after one valid collected range
  made public enable fail before publication; no active configuration or
  module pin remained and the DLL unloaded. After positive run, binding,
  captured-bank, pending-restore, execution and charged-byte debts were zero;
  joined G handles and public runtime destroy released the last image owner.
  A separate six-second process deadline makes nontermination a test failure.
  Native outputs for final matrix: `disabled passed iterations=120000000
  expected=10386373181075607123 actual=10386373181075607123`, `ordinary
  passed iterations=7 expected=16 actual=16`, `precommit failure/unload
  passed`, and `positive passed iterations=120000000
  expected=10386373181075607123 actual=10386373181075607123` with the
  exact two four-event chains above. Rebuilt existing production-linked
  Windows PE handoff and watchdog automatic-transaction oracles also passed.
- E107, first reproducible source/patch packaging: the generated LLVM patch
  `tools/preempt/patches/llvm-23.1.2-win64-llpm.patch` (SHA-256
  `fde021e2f578a70c9426474aaf414bb50170906c99f02e90628e23eb2ba2628d`)
  applies cleanly to the two unmodified official LLVM 23.1.2 X86 files and
  reverse-applies to the patched source. Original release tar SHA-256 is in
  the adjacent README. The three annotated/ordinary test source files now
  live under `experiments/windows/`; the opt-in
  `scripts/build_windows_async_certified_fixture.sh` built a new DLL from
  those files using matched clang/plugin, patched llc and LLD with pinned
  no-alignment and `/opt:ref /opt:noicf` settings. The new DLL SHA-256 was
  `302153f25410870ab5a7d0f3891992093456a0cbb71d560440edc58c3b389fa5`.
  Its code intervals and LLPM fields match the earlier artifact although the
  PE bytes are not identical (object paths/export data differ). On native
  Windows the unchanged R7 collector accepted its two exact records and the
  same bounded R8 positive automatic-preemption oracle passed with two
  capture/peer/restore/witness chains and the same numerical result. This is
  a reproducible **experimental fixture**, not a normal/release compiler
  toolchain or general Windows async-preemption support.
- E108, native Windows build-system verification after packaging: on the
  shared host the isolated validation source snapshot received only the
  changed CMake file, private lifetime header/implementation, and new test
  source; prior versions of the three existing files were retained as
  `.r8-before-20260927` backups in that snapshot. The native CMake/Ninja
  build used one job and compiled the updated production and test-hooks
  lifetime objects plus the new oracle. With
  `LLAM_WINDOWS_ASYNC_CERTIFIED_DLL` pointing to the newly packaged-image
  copy, native CTest's four registered cases passed **4/4 in 0.54 s**:
  certified positive, disabled, ordinary and precommit. The two existing
  affected production-linked tests were also rebuilt natively and CTest
  passed **2/2 in 0.38 s**: automatic watchdog transaction and PE handoff.
  This is a focused native regression, not a rerun of the entire 57-test
  Windows async matrix or a Linux backend validation. The remote source
  backups and generated DLL/test binaries were not deleted.
- E109, T-PRO-WIN-39 disposition and trace refinement: Pro accepted the
  reported one-M fixed-120M end-to-end prototype and the disclosed one-time
  30M→120M coverage calibration, but not experimental integration until
  clean compiler reconstruction, final native tests and actual Linux
  regression. It noted that a partial-input public-enable refusal does not
  prove a loader pin was first acquired, so the control is labelled atomic
  prepublication rejection/no residual ownership only. It also requested
  exact bank and restored worker-dispatch identity in the diagnostic trace.
  The test-only log now records immutable bank pointer identity plus capture
  and restore task **and worker** generations; the validator pairs capture,
  restore and witness on task/bank/generation, compares both restored
  generations exactly across restore/witness, requires both to exceed their
  capture counterparts, and rejects partial publication/overflow. A native
  rebuild and final four-mode CTest rerun passed **4/4 in 0.55 s** with this
  tightened validator. Pro reviewed the prior reported results, not this
  subsequent source change independently.
- E110, clean patched compiler reconstruction: a fresh Homebrew
  `brew unpack --patch llvm` tree (official LLVM 23.1.2 release SHA-256
  `c98bbef08a2b4c2613cd50e9aa9ae7b69b1fe6c16b2c40373bc0ab6116fdf78a`,
  plus Homebrew patch SHA-256 values
  `f6dafd762737eb79761ab7ef814a9fc802ec4bb8d20f46691f07178053b0eb36`
  and `e41e300eb6f5cca9172ab344e572c3fb24f0d05885ae23dd7cb4f9c2528839f7`)
  had only the packaged LLAM patch applied. Its Git diff SHA-256 equalled the
  packaged patch exactly (`fde021e2...`), excluding edits left in the older
  temporary LLVM tree. A new X86-only Release/assertions CMake build produced
  `llc` SHA-256
  `768f71c7e25a99e91f11463a71807182e78a2e693753ee36945fd2ab20e56fe0`.
  The first-party IR plugin was freshly rebuilt warning-clean against the
  matching installed LLVM 23.1.2 clang/libraries (plugin SHA-256
  `97a242b21ffd9c0eb01b72dcefb121b28ab86e871dfce49bbf1f4e44b59dcf85`).
  With explicit paths to that clang/plugin/new llc and LLD 23.1.2, the
  packaged fixture script regenerated a DLL SHA-256
  `56aa5e6d4763e421318ad1e98d8c48b19d24ebaceb3bbef61f6d813d1d706cd0`.
  Native Windows R7 collection accepted its two exact records; the tightened
  120M repeated-chain runtime oracle passed on it, as did its registered
  native four-mode CTest matrix **4/4 in 0.48 s**. No old patched `llc` or
  old plugin was used to build this final image. Different binary hashes from
  the earlier prototype are not a byte-reproducibility failure; source,
  certificate shape, code interval and native behavior were rechecked.
- E111, packaged offline compiler checks on the clean toolchain: the new
  `scripts/test_windows_async_certifier.sh` built positive COFF records,
  confirmed one-only parent plus associative 48-byte follower and two REL32
  fields, linked one/two GC variants with `.llampm` virtual sizes 0x30/0x60,
  and tested N1 incoming stack read, N2 generated helper, N3 callee-save
  traffic, both selected-function orderings, SLS hardening, MC branch padding,
  cert/developer-mode exclusivity, missing function sections and MSVC target.
  Each selected late refusal was diagnostic and removed a deliberately
  pre-existing object at that path; audit-only emitted no LLPM and audit-off
  Windows compilation refused. The script also observed Linux ELF metadata
  in a cross-compiled object. Its first version falsely expected the missing
  function-section error while loop alignment still blocked earlier; the
  fixture was corrected to disable alignment, then the final script passed.
  Linux cross-target emission is **not** a supported-Linux execution/parser
  regression. Docker on this Mac was unavailable because the machine was
  locked (user was asked to unlock it); the user-provided Linux host had only
  1.9 GiB RAM and no clang, llvm-config, GCC or CMake, so no heavy toolchain
  install was attempted. That Linux regression remains pending.
- E112, final native Windows matrix and ABI/default-OFF gates with the
  clean-reconstructed certified DLL configured: the affected async targets
  were rebuilt in the isolated native CMake/Ninja validation tree with one
  job; serial native CTest passed **57/57 in 4.28 s**, zero skips/failures.
  The default-OFF build's ABI contract/compatibility, preemption capability
  and shared DLL-load tests passed **4/4 in 0.39 s**. The experimental opt-in
  build's corresponding four tests passed **4/4 in 0.26 s** after rebuilding
  the shared library. The shared Windows host was checked around these short
  runs (roughly 21–51% reported CPU, 4.98–5.83 million KiB free); no stress,
  soak or sustained benchmark was run. The prior unrelated
  `test_legacy_lease_internal` Windows all-target compile failure is not
  represented by these focused builds; no all-target Windows build claim.
- E113, actual Linux-host compiler/metadata/parser regression: after the user
  unlocked the Mac, Docker Desktop ran an official Ubuntu 24.04 amd64 image
  (pulled digest
  `sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3`)
  under Apple Silicon emulation. The container was capped at two CPUs, 4 GiB
  RAM and 512 PIDs; the LLAM workspace was mounted read-only. Ubuntu packages
  included clang/LLVM 18.1.3, liburing development files, CMake, Python and
  binutils. With `LLAM_PREEMPT_TOOLING=required` and matching LLVM 18 CMake
  files, the first-party pass, metadata inspector, metadata tests and runtime
  registration test built (160 bounded Ninja steps, two jobs). Native-to-the-
  container x86-64 Linux CTest then passed **3/3 in 3.34 s**, zero skips or
  failures: `test_preempt_metadata`, `test_preempt_metadata_registration` and
  `test_llam_preempt_llvm_plugin`. The plugin test links/runs its Linux ELF
  fixture, inspects the final two-TU metadata, checks unsafe/missing required
  refusal, and exercises the new WinGNU audit-tag/no-certificate and audit-off
  rejection paths. This is the previously missing **actual Linux regression**,
  not merely a Mac cross-target ELF object observation. The shared 1.9-GiB
  Linux server was not modified or loaded. The test container was stopped and
  Docker Desktop was quit after the run; the stopped container/image remain
  available for reproduction until intentionally removed.
- E114, final designated ChatGPT 6 Pro review T-PRO-WIN-40: after we reported
  the bank/task/worker trace refinement, clean compiler/DLL reconstruction,
  final native Windows 57/57 plus both ABI/shared-load groups, and Linux
  amd64-container 3/3, Pro accepted the wording **experimentally integrated
  for the pinned LLVM 23.1.2 extension, qualified LLD, WinGNU
  `scalar-leaf-v1`, one-primary-M expert-opt-in profile**. It named no
  remaining discriminating test for that limited integration. This is a
  design assessment of Codex-reported evidence, not Pro's independent source
  inspection/execution or release certification. It explicitly excluded
  arbitrary caller/native-lock safety, code immutability against external
  modification, exception/unwind or cancellation safety, broader XSTATE/
  mitigations, multi-M, other compiler/linker versions, native Linux hardware
  performance and any speedup claim. The unrelated Windows all-target
  `test_legacy_lease_internal` compile issue remains outside the passing
  subset. No commit, push, PR, release or deployment was made.
