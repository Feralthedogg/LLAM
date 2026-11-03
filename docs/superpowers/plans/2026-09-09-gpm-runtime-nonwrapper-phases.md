# G–P–M Runtime (Non-Wrapper Phases) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the supplied LLAM G–P–M architecture through Phases 1B, 2, 3, 4, 5, and 7 while explicitly excluding the Phase 6 Rust/Python/JVM/.NET/Go language wrappers.

**Architecture:** Preserve the Phase 1A legacy lease as the single execution-publication authority while making `llam_processor_t` own runnable resources and adding generation-checked M/P ownership. Add foreign detach and fail-closed relief only after that boundary is real. Keep async preemption, compiler metadata, and platform backends opt-in/experimental until their capability and fallback contracts are proven; retain cooperative behavior and public ABI compatibility by default.

**Tech Stack:** C11, hand-written platform assembly where already present, POSIX/Windows platform guards, GNU Make, CMake/CTest, optional Meson, Python audit scripts, and the repository's existing sanitizer/fuzz test matrix.

**Spec:** `/Users/feral/.codex/attachments/fcf2a580-fb55-404c-a348-2dc3a2c42d00/pasted-text.txt` plus the read-only audits in `.superpowers/sdd/2026-09-08-phase1a-legacy-execution-lease/audit-phase*.md`.

## Global Constraints

- Keep the public C headers and existing exported symbols byte-compatible unless an additive ABI-minor change is explicitly required and tested.
- Preserve `LLAM_PREEMPT_OFF`, `LLAM_PREEMPT_COOPERATIVE`, `LLAM_PREEMPT_AUTO`, `LLAM_PREEMPT_STRICT`, existing blocking-pool semantics, and all Phase 1A lease/watchdog invariants.
- One writable authority per P resource; compatibility accessors may read but may not maintain mutable shard mirrors.
- `opaque_lock -> shard->lock` remains the lock order; never wait, join, context-switch, allocate user memory, or run user/foreign code while either scheduler lock is held.
- The Task 1 P-owner record was inert metadata before Task 2A. Task 2A composes it with every lease writer under `opaque_lock -> shard->lock` and validates non-owning tickets containing processor identity, worker incarnation, lease epoch, and P generation. The lease remains the sole execution-publication authority. Task 2B now gates queue/timer owner mutation; Task 3 now keeps allocator/cache ownership in P while preserving independent allocation and I/O-node identities.
- Unknown code/PC/metadata/state is unsafe; unsupported async or foreign behavior must downgrade or return a deterministic error, never guess safe.
- Phase 3 is Linux x86-64 experimental and manually registered safe ranges only; non-Linux async backends remain explicit unsupported/cooperative-only until Phase 5 gates pass.
- Phase 6 language wrappers are out of scope; completion queue/dispatcher work is private or C-only unless needed as a prerequisite and separately ABI-reviewed.
- Every implementation task follows TDD: write a focused failing test, observe the expected failure, implement the smallest change, rerun focused and regression tests, then commit.
- Use a fresh `gpt-5.6-luna` subagent with `max` reasoning for each implementation/review task as requested; no implementer may dispatch its own subagents.

---

### Task 1: Freeze the Phase 1B authority contract and migrate topology/wake ownership

**Files:**
- Create: `docs/superpowers/specs/2026-09-09-phase1b-processor-resources.md`
- Create: `tests/test_processor_resource_internal.c`
- Modify: `src/internal/runtime_types.h`, `src/internal/runtime_state.h`, `src/core/lifecycle/init.c`, `src/core/lifecycle/run.c`, `src/core/lifecycle/shutdown.c`, `src/core/sched/wake.c`, `src/core/sched/scheduler_engine.c`, `src/core/debug/debug.c`, `Makefile`, `CMakeLists.txt`, and the Phase 1A boundary scripts/docs.

**Interfaces:**
- Produce private accessors `llam_processor_cpu_id`, `llam_processor_node_index`, `llam_processor_io_node_index`, `llam_processor_online`, `llam_processor_event_fd`, and `llam_processor_event_pending` with documented lock/atomic contracts.
- Produce a versioned P-owner record containing owner M pointer, worker incarnation, P generation, and `FREE/OWNED/TRANSFERRING/STOPPING` state; the Phase 1A lease remains the owner/epoch truth until a later task explicitly composes the projection.
- Keep `processor.runtime`, `processor.compat_shard`, and `processor.id` stable; retain opaque-helper wake state, node backend state, task registry, metrics, trace, and lease coordinator on their existing owners.

- [x] **Step 1: Write failing ownership and wake tests.** Task 1 initially tested the provisional one-owner CAS/generation helper alongside P identity, topology accessors, wake coalescing, offline/reactivation descriptor lifetime, and duplicate-route boundaries. Task 2A supersedes that standalone CAS API/test with lease-composed ticket lifecycle and malformed-current-tuple tests; no generic P-owner CAS remains.
- [x] **Step 2: Run `make test-processor-resource` (or the focused CTest target) and verify the new tests fail for the identity-only processor.** The implementation report notes that the exact historical RED output was not retained; the focused GREEN target, subsequent malformed-record RED/GREEN fix evidence, and later regression runs are recorded there.
- [x] **Step 3: Implement P authority/accessors and move topology plus the ordinary scheduler wake pair in place.** Do not copy allocator/queue/timer objects; retain one payload lock and update init, partial-init unwind, idle wait, stop/fatal wake, diagnostics, and shutdown. Task 1 implementation was completed, then Task 2A composed the P ticket with the lease before resource consumers were enabled.
- [x] **Step 4: Run the focused target, Phase 0/1A boundary suites, normal Make tests, and fresh Debug CTest; verify no public header/export change.** Focused, boundary, Make/CMake, sanitizer, ABI, export, and hook gates are recorded in the Task 1/2A reports.
- [ ] **Step 5: Commit `feat: establish processor resource authority and wake ownership`.**

### Task 2: Move runnable queues and timers transactionally into P

**Files:**
- Create: `tests/test_processor_queue_timer_internal.c`
- Modify: `src/internal/runtime_types.h`, `src/core/sched/core_queue.c`, `src/core/sched/norm_queue.c`, `src/core/sched/norm_queue_depth.c`, `src/core/sched/reinject.c`, `src/core/task/spawn.c`, `src/core/task/yield_join_sleep.c`, `src/core/wait/wait_tracking.c`, `src/core/time/timer.c`, `src/core/time/timer_heap.c`, `src/engine/scheduler/scheduler_engine.c`, `src/engine/watchdog/watchdog_merge.c`, `src/engine/watchdog/watchdog_rehome.c`, `src/engine/watchdog/watchdog_scale.c`, and diagnostics/tests.

**Interfaces:**
- First define and implement the live lease/P-owner composition contract. Keep the Phase 1A lease as execution-publication authority while the P record is a versioned projection; publish matching owner, worker-incarnation, lease-epoch, and P-generation state in one `opaque_lock -> shard->lock` transaction. The `TRANSFERRING` state is a non-operational, unobservable intermediate while both locks remain held; no partial transfer may escape the transaction.
- Produce an owner-only P ticket that binds worker pointer, worker incarnation, lease epoch, and P generation; P queue/timer accessors revalidate it at the operation boundary. Remote producers use inject/overflow and never gain owner rights from TLS association or a bare pointer.
- Produce `llam_processor_rehome_timers_locked(source, target)` with pre-reservation, callback quiescence, wait-generation validation, rollback on allocation failure, and one heap owner per active timer.

Task 2A completed the composition and ticket-validation gate before resource
consumers were added. The original Task 2A RED run observed a `FREE` P record
after primary bind; the implementation publishes a matching ticket. Task 2B
now moves runnable queues and timers to P behind operation-boundary access
checks; foreign producers remain on inject/overflow, and timer rehome preserves
one heap owner with generation-checked rollback.

- [x] **Step 1: Add failing lease/P-owner consistency tests for primary bind, helper transfer/handback, redirect/reacquire, STOPPING, offline-vs-claim, wake/close quiescence, stale worker incarnation (including pointer-address reuse), stale lease epoch, stale P generation, and rejection of owner use while uncomposed or transferring.**
- [x] **Step 2: Add failing queue/timer tests for owner-only mutation, remote spawn routing, timer rehome/rollback, immediate expiry, cancel-vs-migration, and dynamic offline with runnable/timed work.**
- [x] **Step 3: Run the focused lease/P-owner tests before implementation; confirm the primary-bind assertion fails because the Task 1 record is inert. Queue/timer storage remains shard-owned at this gate.**
- [x] **Step 4: Implement the ordered lease/P-owner transaction and ticket validation before any production consumer can use the record. Ensure ordinary unbind clears its P link under the same lock order, while STOPPING retains the terminal link until post-join finalization.**
- [x] **Step 5: Move queue/deque storage and depth/fairness state to P without changing intrusive links; gate lock-free bottom access on a current ticket and route controller merges to target inject.**
- [x] **Step 6: Implement transactional timer movement and update every wait/cancel/fire/idle/merge path to publish `parked_shard` only after generation validation.**
- [x] **Step 7: Run focused queue/timer tests, stress/fuzz, ASan/UBSan/TSan (including the owner race test), Debug CMake/CTest, ABI/export, and boundary gates.**
- [ ] **Step 8: Commit `feat: make runnable queues and timers processor-owned` after the user's explicit DCO signoff.**

### Task 3: Move allocator/caches and complete logical I/O routing

**Files:**
- Create: `tests/test_processor_allocator_io_internal.c`
- Modify: `src/internal/runtime_types.h`, `src/core/memory/alloc.c`, `src/core/memory/allocator_quiescent.c`, `src/core/memory/io_object_alloc.c`, `src/core/task/task_stack.c`, `src/core/task/task_alloc.c`, `src/core/wait/wait_timer_alloc.c`, `src/io/api/issue.c`, `src/io/api/blocking_ops.c`, `src/io/api/direct_tuning.c`, `src/io/engine/io_engine.c`, `src/io/watch/*.c`, `src/engine/watchdog/watchdog_rehome.c`, `src/core/debug/debug.c`, and build/test registration.

**Interfaces:**
- Produce in-place P allocator/cache ownership and accessors; preserve slab allocation-owner identity separately from task home/live placement and runtime-wide stack fallback caches.
- Produce P logical I/O route accessors while keeping `llam_node_t` backend ownership, request owner/inflight/attached-node generations, watch migration state, and public stats schema unchanged.

- [x] **Step 1: Add failing cross-shard/cross-runtime remote-free, stack-cache, partial-init, worker-ring/non-ring route, completion/rehome, and shutdown tests.** The focused test verifies allocation-owner return across P and runtime IDs, request routing while attached versus after logical rehome, stack-cache selection after a home-P change, and cleanup after an injected failure following stack-cache prewarm.
- [x] **Step 2: Run focused tests and verify failures identify old shard route/cache fields or non-copyable allocator movement.** The initial focused object compile failed on missing P allocator and stack-cache members, as expected.
- [x] **Step 3: Move allocator and per-P stack caches in place; update all allocation/free/quiescent paths and diagnostics without `memcpy`/assignment of mutex-bearing state.** Allocation-owner IDs remain the same; the runtime-wide stack fallback remains runtime-owned.
- [x] **Step 4: Replace remaining direct route reads with P accessors and validate node/request/watch generation behavior for ring and non-ring modes.** Source audit confirms logical route reads use `llam_processor_io_node_index` and requests use `llam_io_req_node_index`, which prioritizes an attached node.
- [x] **Step 5: Run full Make/CMake/sanitizer/ABI/export/boundary gates.** `make -j4 test`, `make -j4 test-asan`, and `make -j4 test-tsan` passed; fresh Debug CMake/CTest passed 33/33. The focused allocator/I/O test passed again under Make, ASan/UBSan, TSan, and CTest after the final route assertion update; boundary, header/ABI, export/hook, docs, and diff checks passed.
- [ ] **Step 6: Commit `feat: migrate processor allocator caches and io routes` after the user's explicit DCO signoff.**

### Task 4: Add foreign detach/reattach and fail-closed relief M

**Files:**
- Create: `docs/superpowers/specs/2026-09-09-phase2-foreign-detach.md`, `tests/test_foreign_detach_internal.c`, and `tests/test_relief_worker_internal.c`.
- Modify: `src/internal/llam_internal.h`, `src/internal/runtime_types.h`, `src/internal/runtime_state.h`, `src/core/api/blocking_api.c`, `src/core/sched/scheduler.c`, `src/core/sched/safepoint.c`, `src/core/sched/spawn.c`, `src/core/task/yield_join_sleep.c`, `src/core/sched/reinject.c`, `src/engine/watchdog/watchdog_probe.c`, `src/engine/watchdog/watchdog_worker.c`, `src/engine/watchdog/watchdog_scale.c`, `src/core/lifecycle/shutdown.c`, `src/core/debug/debug.c`, and internal metrics/build registration.

**Interfaces:**
- Produce internal M execution states (`SCHEDULER`, `MANAGED_SAFE`, `RUNTIME_UNSAFE`, `FOREIGN`, `IDLE`, `STOPPING`), nested runtime-critical/preempt-disable depth, current-G/current-P publication, and foreign token/run-generation fields.
- Route outermost `llam_enter_blocking`/`llam_leave_blocking` through a P release/reacquire transition while preserving no-op/outside-task, nested-depth, errno, cancellation, stop, and thread-affinity compatibility; keep `llam_call_blocking_result` on its existing pool.
- Produce one-owner relief activation/handback protocol and internal counters; unknown or runtime-unsafe stalls defer with an explicit reason.

- [x] **Step 1: Add failing deterministic tests for P release/reacquire, nested/cross-thread leave, foreign spawn remote path, stop/cancel, relief handback, unsafe deferral, and shutdown lifetime.** The new tests initially failed to compile against the missing M/current-P/token contract; the lease suite supplies the stop, return, and shutdown handback cases.
- [x] **Step 2: Run focused tests and confirm no `FOREIGN` state/token or P owner transfer exists.** Initial focused builds lacked the new state/token members and relief snapshot; focused tests then passed after implementation.
- [x] **Step 3: Implement M state/current-G/P and foreign token publication, then convert outermost enter/leave to generation-checked P release/reacquire without scheduler locks across waits.** Direct scheduler dispatch, reinject, direct yield, blocking-return, and failed-enter recovery all advance G/M run generations before active-G publication.
- [x] **Step 4: Generalize the helper lifecycle into a relief M bridge; activate only after a safe-state/P-ticket handshake and preserve redirect as bounded fallback.** The existing helper remains the only relief M. Watchdog and foreign transitions require exact current P tickets; overdue no-safepoint Gs receive only cooperative requests and are counted as deferred.
- [x] **Step 5: Integrate merge/offline/shutdown barriers and run full regression plus sanitizers/CPU-spike announced-vs-safe-unannounced evidence.** Full Make, ASan/UBSan, TSan, fresh Debug CMake/CTest (35/35), focused announced foreign-helper progress, deterministic/dynamic stress, no-safepoint P-retention, 133 boundary fixtures, 187-file source audit, export/hook audits, and strict MkDocs passed. The existing Apache-license-policy files were not re-licensed.
- [ ] **Step 6: Commit `feat: add foreign detach and fail-closed relief` only after explicit user DCO sign-off.** No files are staged or committed.

### Task 5: Build the Linux x86-64 experimental async context backend

**Files:**
- Create: `src/core/context/async_context_linux_x86_64.c`, `src/asm/linux/x86_64/async_preempt_trampoline.S`, `tests/test_async_preempt_linux_x86_64.c`, and `tests/fixtures/async_safe_leaf.S`.
- Modify: `src/core/debug/fault.c`, `src/core/platform/platform.c`, `src/internal/runtime_types.h`, `src/internal/llam_internal.h`, `CMakeLists.txt`, `Makefile`, and experimental docs.

**Interfaces:**
- Produce an opt-in `LLAM_EXPERIMENTAL_ASYNC_PREEMPT` backend guarded by Linux+x86-64, with preallocated full signal/XSTATE capture, generation-bearing request/ack, immutable manually registered safe ranges, signal conflict refusal, and an assembly trampoline.
- Unknown/unsafe PCs, nested/disabled signals, stale generations, task reclaim, shutdown, unsupported XSTATE/CET, and non-Linux requests return a documented skip/defer/unsupported result; normal builds retain the one-store cooperative signal handler.

- [x] **Step 1: Add a Linux-only focused test target and a negative test that proves the current cooperative handler cannot capture/resume a no-poll leaf loop.**
- [x] **Step 2: Run it on all hosts; expect code 77 outside Linux x86-64 and a failure before the backend exists on Linux x86-64.** TDD RED and Darwin code-77 skip were observed before implementation.
- [x] **Step 3: Implement preallocated context/capture records, `SA_SIGINFO|SA_ONSTACK` dedicated-signal setup, safe-range lookup, and generation checks; keep handler operations lock-free and allocation-free.**
- [x] **Step 4: Implement the trampoline/scheduler resume path and register/FP/XSTATE conformance assertions for the hand-authored leaf only.**
- [x] **Step 5: Run focused ASan/UBSan, TSan attempt, and cooperative regressions; document experimental limits.** TSan cannot initialize under the available linux/amd64 emulator (`unexpected memory mapping`); native Darwin CTest passes and the emulated Linux I/O park/io_buffers cases are environment-limited.
- [ ] **Step 6: Commit `feat: add fail-closed linux async preemption experiment` only after explicit user DCO sign-off.**

### Task 6: Add metadata format, LLVM tooling, `llam-clang`, and build integration

**Files:**
- Create: `tools/preempt/metadata.h`, `tools/preempt/metadata.c`, `tools/preempt/metadata_inspect.c`, `tools/preempt/llam_preempt_pass.cpp`, `tools/llam-clang`, parser/driver/plugin/SDK/package fixtures under `tests/` and `scripts/`, `meson.build`, `meson_options.txt`, and tooling docs.
- Modify: `CMakeLists.txt`, `Makefile`, `scripts/package_release.sh`, `scripts/generate_sdk_metadata.sh`, install/export tests, and capability/registration glue if required.

**Interfaces:**
- Produce a versioned `.llam.preempt` record/header with checked magic/version/size/flags/ranges and fail-closed malformed/unknown-required handling; absent optional metadata preserves legacy behavior.
- Produce an opt-in LLVM pass that inserts cooperative safepoints only at proven legal sites, an explicit `llam-clang` argument-forwarding wrapper, and CMake/Meson/Make `off/auto/required` tooling switches. Do not instrument the runtime itself or add plugin symbols to `llam_runtime_shared`.
- `ASYNC_REQUIRED` must fail compile/link/registration when the requested proof/capability is absent; it must never silently downgrade.

- [x] **Step 1: Add parser/fixture tests for valid, short, bad magic/version/size, unknown optional/required flags, duplicate/overflow ranges, section absence, and required-mode failure.**
- [x] **Step 2: Run parser tests and record the format/tool boundary.** The parser suite passes under the normal Make/CMake/Meson builds; it runs without depending on LLVM.
- [x] **Step 3: Implement metadata producer/parser and section-retention fixtures for the supported object format; keep records out of the stable C ABI.** Linux x86-64 link smoke verifies concatenated records from separate translation units and final ELF section retention; the private `llam_preempt_module_register` hook validates image-relative records and copies only async-safe ranges before enable.
- [x] **Step 4: Implement the LLVM pass and `llam-clang` driver with fake-compiler forwarding and real-Clang smoke tests; enforce required mode at the final link.** Async-safe functions are restricted to pure leaf IR; compile-only translation units may omit ranges, but required final links must contain a verified range.
- [x] **Step 5: Add CMake/Meson/Make opt-in integration, install/package checks, and run legacy ABI/export/runtime tests.** Linux x86-64 CMake and Meson `required` builds and tests pass with LLVM/clang 14; CMake/Make `auto` on Darwin falls back cleanly; Meson `off` tests pass; PreemptTools CMake install, Meson install, and `off`/`auto`/`required` release SDK fixtures pass. Full Darwin `make -j4 test` passed, including ABI/export/runtime checks. `make check-license-policy` still reports the seven pre-existing Apache-manifest files without current LicenseRef notices; no existing Apache notices were changed. Commit remains pending explicit DCO sign-off.

### Task 7: Complete platform capability/fallback matrix and Phase 7 policy

**Files:**
- Create: `src/core/preempt/capabilities.c`, `tests/test_preempt_capabilities.c`, `tests/test_default_policy_internal.c`, platform build fixtures, and the final non-wrapper architecture/spec report.
- Modify: `src/internal/runtime_types.h`, `src/core/platform/platform.c`, `src/core/platform/windows_policy.c`, `src/core/base/abi.c`, `src/core/debug/debug_stats_json.c`, `src/core/debug/debug.c`, `src/engine/watchdog/*.c`, `CMakeLists.txt`, `Makefile`, CI workflow/build docs, and release notes.

**Interfaces:**
- Produce private capability/failure records and, only if necessary after ABI review, a tail-size-versioned C capability query; unsupported async platforms report cooperative/hybrid/`ENOTSUP` deterministically.
- Keep `AUTO` conservative: instrumented safe code may use async, uninstrumented code is hybrid/deferred, and `COOPERATIVE` remains permanent compatibility mode. Do not enable research-track policies or wrappers by default.
- Add internal observability for async requests/captures/resumes/deferred/unsafe/stale/collision/context bytes, foreign detach/reacquire, relief activation, and max acknowledgement latency; public stats grow only through an explicit ABI-minor decision.

- [x] **Step 1: Add failing capability/default-policy/unsupported-platform and stats-schema prefix tests.** Added private capability/fallback matrix and default-policy tests; public ABI/JSON prefix remains unchanged.
- [x] **Step 2: Run focused tests and confirm the effective policy distinction.** `test_preempt_capabilities` and `test_default_policy_internal` pass on native Darwin and Linux x86-64 emulation, including the async-enabled feature build.
- [x] **Step 3: Implement platform normalization (Linux arm64 spelling parity, Darwin/BSD cooperative-only guards, Windows context-policy refusal/cleanup) and capability/failure recording.** Compiled-backend bits, async state, safe-range count, and typed failure reasons are private and fail closed.
- [x] **Step 4: Implement default AUTO/HYBRID/COOPERATIVE policy, diagnostics/dump fields, and merge/shutdown observability without changing public defaults.** Internal dumps now expose effective policy/fallback and async capture/collision/context-byte/ack-latency metrics; OFF/COOPERATIVE never activate async capture.
- [x] **Step 5: Run the complete Make/CMake/Meson/tooling/ABI/export/boundary/sanitizer/fuzz matrix.** Native Darwin CMake/Make full suites pass; Linux x86-64 emulated async and ASan/UBSan focused suites pass; CMake/Meson tooling and package gates pass. Known license-policy baseline and unavailable native Linux/Windows/BSD execution are documented. Commit remains pending explicit user DCO sign-off.

## Whole-Plan Verification

From a clean isolated worktree, run `make -j1 test`, `make -j1 test-hardening`, fresh CMake Debug and Release CTest, Meson `auto`/`required`/`disabled` fixtures when available, all focused processor/foreign/async/tooling/capability targets, ASan/UBSan/TSan/fuzz targets supported by the host, source boundary audits, production test-hook/export audits, `git diff --check`, and public-header/ABI compatibility checks against the Phase 1A baseline. Record Windows/BSD/Linux execution gaps as explicit environment limitations rather than passing claims.
