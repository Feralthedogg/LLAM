# Phase 1A Legacy Execution Lease Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert the existing primary/helper/redirect behavior into one epoch-bearing legacy execution lease, migrate current-G and watchdog publication safely, and preserve the public ABI and cooperative runtime behavior.

**Architecture:** Keep `llam_shard_t` as the composition root for queues, timers, allocator/cache, I/O, online state, and public metrics. Use the existing per-shard `opaque_lock` as the handoff/lease lock, add an embedded lease coordinator plus worker-local active-G/binding state, and remove independent `current`/`preempt_thread` writers only after all readers are migrated.

**Tech Stack:** C11 atomics, pthreads/Windows pthread compatibility layer, POSIX condition/futex wake paths, Make, CMake/CTest, Python boundary/audit scripts, ASan/UBSan, and TSan.

**Spec:** `docs/superpowers/specs/2026-09-08-phase1a-legacy-execution-lease-design.md`

## Global Constraints

- Base the work on clean Phase 0 commit `2cde642` from the `₩₩phase0-worker-role-signal-stack` line.
- Keep public headers under `include/llam` byte-identical and keep the exported symbol set and public stats/dump schema unchanged.
- `llam_shard_t` remains authoritative for queues, timers, allocator/cache, wake/I/O routing, online/pause policy, task registry, and public metrics.
- Use `shard->opaque_lock` as the single lease/handoff lock; when both locks are required, acquire `opaque_lock` before `shard->lock`.
- Never hold a scheduler lock while waiting, joining, switching context, or running user/foreign code; `_locked` helpers must not reacquire their documented lock.
- The lease coordinator is the only execution-authority truth. `opaque_helper_active`, redirect flags, and any temporary legacy snapshots are compatibility projections only.
- `owner == NULL` is not an automatic grant. Ownerless redirect retains a return worker/incarnation claimant until its foreign-return boundary completes.
- No generic `is_executor()` API, no arbitrary worker pool, no async context capture, no compiler metadata, no foreign detach, and no Phase 1B P-field migration in this plan.
- Rust, Python, JVM, .NET, and Go wrappers are explicitly excluded from Phase 6 and from this plan.
- Every implementation subagent and reviewer uses `gpt-5.6-luna` with `max` reasoning, as requested by the user.
- Each production behavior change follows a visible TDD RED → GREEN cycle and ends with a focused test plus the existing full suite.

---

### Task 1: Characterization, lease test hooks, and ownership inventory

**Files:**
- Create: `tests/test_legacy_lease_internal.c`
- Modify: `Makefile` test object/target lists and `CLEAN_FILES`
- Modify: `CMakeLists.txt` internal test target and CTest registration
- Modify: `src/internal/runtime_proto_sched.h` test-only lease observation declarations
- Modify: `docs/internals/architecture.md` Phase 1A boundary note

**Interfaces:**
- Consumes: Phase 0 worker slots, TLS identity, signal-stack lifecycle, helper/redirect behavior, and existing test-hook build split.
- Produces, only under `LLAM_ENABLE_TEST_HOOKS`:

```c
typedef enum llam_lease_test_point {
    LLAM_LEASE_TEST_AFTER_TRANSFER_REQUEST = 1,
    LLAM_LEASE_TEST_AFTER_OWNER_CLEAR = 2,
    LLAM_LEASE_TEST_AFTER_GRANT = 3,
    LLAM_LEASE_TEST_AFTER_HANDBACK_ACK = 4,
    LLAM_LEASE_TEST_BEFORE_WATCHDOG_SIGNAL = 5
} llam_lease_test_point_t;

typedef struct llam_lease_test_snapshot {
    unsigned state;
    uintptr_t owner_worker;
    uint64_t owner_incarnation;
    uint64_t epoch;
    uintptr_t return_worker;
    uint64_t return_incarnation;
    uint64_t transfer_sequence;
    uint64_t handback_ack_sequence;
} llam_lease_test_snapshot_t;

void llam_lease_test_set_gate(llam_lease_test_point_t point, bool enabled);
bool llam_lease_test_wait_gate(llam_lease_test_point_t point, uint64_t timeout_ns);
int llam_lease_test_snapshot(llam_runtime_t *runtime,
                             unsigned shard_id,
                             llam_lease_test_snapshot_t *out);
```

- The hook must block only at test points, must not read worker-local plain fields from another thread, and must be compiled out of production artifacts.

- [ ] **Step 1: Write the failing characterization tests**

  Add named tests that exercise the existing runtime without relying on the new lease fields: primary/helper activation and return, helper re-blocking through redirect, ownerless return ordering, repeated shard-0 runs on two caller threads, helper shutdown while idle/active, dynamic merge pause, watchdog/task-exit overlap, and partial helper-start failure. Each test must assert task completion, errno/fatal semantics, and that no second helper is created.

- [ ] **Step 2: Run the focused test to verify the intended RED state**

  Run `make test_legacy_lease_internal` and expect compilation failure for the test-only snapshot/gate declarations. If the test fails for any unrelated reason, correct the harness before continuing.

- [ ] **Step 3: Add the test-only hook plumbing and baseline inventory**

  Implement the declarations in a test-hook-only translation unit or existing test-hook override, add the Make/CMake targets, and record every current writer/reader of `shard->current`, `shard->preempt_thread`, `opaque_helper_active`, `opaque_compensation_depth`, and pause ACK fields in `docs/internals/architecture.md`. Production code must not change behavior in this task.

- [ ] **Step 4: Run the focused tests and baseline suite**

  Run `make test_legacy_lease_internal`, `python3 scripts/test_worker_identity_boundary.py`, and `make test`. The characterization test must pass against the unchanged Phase 0 behavior, and the hook artifact audit must find no production test symbols.

- [ ] **Step 5: Commit**

```bash
git add tests/test_legacy_lease_internal.c Makefile CMakeLists.txt \
  src/internal/runtime_proto_sched.h docs/internals/architecture.md
git commit -m "test: characterize legacy execution lease behavior"
```

### Task 2: Embedded lease coordinator and worker binding/lifetime state

**Files:**
- Modify: `src/internal/runtime_types.h` lease/worker/shard structures
- Modify: `src/internal/runtime_state.h` internal lease accessors and assertions
- Modify: `src/internal/runtime_proto_sched.h` lease/binding prototypes
- Modify: `src/core/lifecycle/init.c` initialization and failure unwind
- Modify: `src/core/lifecycle/shutdown.c` stop/unbind cleanup
- Modify: `src/core/sched/scheduler.c` scheduler entry/exit binding publication
- Modify: `src/core/lifecycle/run.c` shard-0 borrowed binding publication
- Test: `tests/test_legacy_lease_internal.c`

**Interfaces:**
- Consumes: Task 1 test gates and Phase 0 stable worker/processor slots.
- Produces the following internal types and functions:

```c
typedef enum llam_legacy_lease_state {
    LLAM_LEASE_PRIMARY_OWNED = 1,
    LLAM_LEASE_TRANSFER_TO_HELPER = 2,
    LLAM_LEASE_HELPER_OWNED = 3,
    LLAM_LEASE_TRANSFER_TO_PRIMARY = 4,
    LLAM_LEASE_REDIRECTED_NO_EXECUTOR = 5,
    LLAM_LEASE_STOPPING = 6
} llam_legacy_lease_state_t;

typedef struct llam_legacy_lease {
    llam_worker_t *owner;
    uint64_t owner_incarnation;
    uint64_t epoch;
    llam_worker_t *return_worker;
    uint64_t return_incarnation;
    uint64_t transfer_sequence;
    uint64_t handback_ack_sequence;
    llam_legacy_lease_state_t state;
} llam_legacy_lease_t;

void llam_legacy_lease_init(llam_shard_t *shard);
void llam_legacy_lease_begin_stop_locked(llam_shard_t *shard);
bool llam_worker_binding_live(const llam_worker_t *worker, uint64_t incarnation);
bool llam_legacy_lease_snapshot_locked(const llam_shard_t *shard,
                                       llam_lease_test_snapshot_t *out);
```

- Extend `llam_worker_t` with worker-local native-thread handle/liveness/incarnation binding metadata and atomic `active_g` plus worker-local run/safepoint/idle timestamps. Do not add processor queues, timers, allocator, or an `owns_p` field.
- Initialize lease state to `PRIMARY_OWNED` only after the primary binding is live; initialize non-running shards to an unbound/ownerless state that cannot grant work.
- The lease coordinator is protected by `opaque_lock`; its fields are plain under that lock. Worker active-G and timestamp fields are atomic where watchdog readers require cross-thread acquire loads.

- [ ] **Step 1: Write the failing type/binding tests**

  Extend the focused test to snapshot lease state before/after scheduler entry, verify shard-0 borrowed bindings use a fresh incarnation on each caller thread, verify helper binding liveness is false after join, and verify shutdown cannot grant an unbound worker. Compile the test against the current code to observe missing types/accessors.

- [ ] **Step 2: Implement the coordinator and binding metadata**

  Add the exact enum/struct/prototypes above, initialize every field in `init.c`, clear them in every early-failure path and `shutdown.c`, and bind/unbind worker native handles in scheduler entry/exit without changing which thread executes queues.

- [ ] **Step 3: Run the focused tests and force a RED check**

  Run `make test_legacy_lease_internal` and use a temporary test-only mutation that omits binding clear or incarnation increment to confirm the new assertions fail; restore the implementation after the RED observation.

- [ ] **Step 4: Run GREEN verification**

  Run `make test_legacy_lease_internal`, `make test_runtime_shutdown_internal`, `make test_multi_runtime_core`, and `make test`. Verify the worker lifetime fields are never read from another thread without the documented lock/atomic access.

- [ ] **Step 5: Commit**

```bash
git add src/internal/runtime_types.h src/internal/runtime_state.h \
  src/internal/runtime_proto_sched.h src/core/lifecycle/init.c \
  src/core/lifecycle/shutdown.c src/core/sched/scheduler.c \
  src/core/lifecycle/run.c tests/test_legacy_lease_internal.c
git commit -m "refactor: add legacy lease coordinator and worker bindings"
```

### Task 3: Convert helper, redirect, and return paths to the lease protocol

**Files:**
- Modify: `src/core/api/blocking_api.c`
- Modify: `src/core/sched/scheduler.c`
- Modify: `src/core/sched/core_queue.c`
- Modify: `src/core/time/timer.c`
- Modify: `src/core/lifecycle/shutdown.c`
- Modify: `src/core/lifecycle/init.c`
- Modify: `src/internal/runtime_proto_sched.h`
- Test: `tests/test_legacy_lease_internal.c`

**Interfaces:**
- Consumes: Task 2 lease coordinator, binding lifetime, and test gates.
- Produces these internal transition helpers, all requiring `opaque_lock` unless named otherwise:

```c
bool llam_legacy_lease_request_helper_locked(llam_shard_t *shard,
                                             llam_worker_t *primary,
                                             uint64_t *sequence_out);
bool llam_legacy_lease_grant_helper_locked(llam_shard_t *shard,
                                           llam_worker_t *helper,
                                           uint64_t sequence);
bool llam_legacy_lease_request_return_locked(llam_shard_t *shard,
                                             llam_worker_t *claimant,
                                             uint64_t claimant_incarnation,
                                             uint64_t *sequence_out);
bool llam_legacy_lease_complete_handback_locked(llam_shard_t *shard,
                                                llam_worker_t *worker,
                                                uint64_t sequence);
bool llam_legacy_lease_enter_redirect_locked(llam_shard_t *shard,
                                             llam_worker_t *return_worker,
                                             uint64_t return_incarnation);
bool llam_legacy_lease_can_reacquire_locked(const llam_shard_t *shard,
                                            const llam_worker_t *worker,
                                            uint64_t incarnation);
```

- [ ] **Step 1: Add RED tests for the known publication and ownerless races**

  Use the Task 1 gate to pause after transfer request, after owner clear, after helper grant, and after handback. Assert that a helper dispatch cannot be followed by a late primary clear, that `owner == NULL` does not let an unrelated primary claim, that helper re-blocking uses redirect without creating a second helper, and that helper stop waits for a handback acknowledgement rather than only depth reaching zero.

- [ ] **Step 2: Implement primary→helper transfer**

  In `llam_enter_blocking()`, reserve a transfer sequence under `opaque_lock`, acquire `shard->lock` in the established order, mark the task blocked and clear the primary current publication before granting the helper, then publish the helper owner/incarnation and increment the epoch under `opaque_lock`. Update compatibility booleans only as projections within the same lock.

- [ ] **Step 3: Implement helper→primary handback and ownerless redirect**

  In `llam_opaque_helper_main()` and `llam_leave_blocking()`, stop fetching work before handback, clear helper active state under `shard->lock`, publish a matching handback sequence under `opaque_lock`, and let only the recorded return claimant reacquire. Preserve helper re-blocking as `REDIRECTED_NO_EXECUTOR` with a return token; never grant from a bare NULL owner observation.

- [ ] **Step 4: Run GREEN tests and inspect lock order**

  Run `make test_legacy_lease_internal`, `make test_runtime_stress`, `make test_runtime_shutdown_internal`, and `make test`. Run a source check that all nested lease/shard lock sites use `opaque_lock` before `shard->lock`, and confirm no wait, join, context switch, or user callback occurs while either lock is held.

- [ ] **Step 5: Commit**

```bash
git add src/core/api/blocking_api.c src/core/sched/scheduler.c \
  src/core/sched/core_queue.c src/core/time/timer.c \
  src/core/lifecycle/shutdown.c src/core/lifecycle/init.c \
  src/internal/runtime_proto_sched.h tests/test_legacy_lease_internal.c
git commit -m "refactor: serialize helper and redirect with a legacy lease"
```

### Task 4: Move current-G and run publication to worker-local state

**Files:**
- Modify: `src/internal/runtime_types.h`
- Modify: `src/internal/runtime_state.h`
- Modify: `src/core/sched/scheduler.c`
- Modify: `src/core/task/yield_join_sleep.c`
- Modify: `src/core/task/spawn.c`
- Modify: `src/core/sched/reinject.c`
- Modify: `src/core/wait/wait_tracking.c`
- Modify: `src/core/api/blocking_api.c`
- Modify: `src/core/debug/debug.c`
- Modify: `src/core/sched/core_queue.c`
- Modify: `src/engine/watchdog/watchdog_probe.c`
- Modify: `src/engine/watchdog/watchdog_scale.c`
- Modify: `src/engine/watchdog/watchdog_merge.c`
- Modify: `src/engine/runtime_watchdog_internal.h`
- Modify: `src/core/lifecycle/init.c`
- Modify: `src/core/lifecycle/shutdown.c`
- Test: `tests/test_legacy_lease_internal.c`, `tests/test_runtime_core.c`

**Interfaces:**
- Consumes: Task 3's single lease and handback barriers.
- Produces:

```c
void llam_worker_publish_active_g(llam_worker_t *worker, llam_task_t *task);
llam_task_t *llam_worker_load_active_g(const llam_worker_t *worker);
void llam_worker_clear_active_g(llam_worker_t *worker);
llam_task_t *llam_shard_load_lease_current(llam_shard_t *shard);
```

- `llam_worker_publish_active_g()` is called only after task state, shard association, and timestamps are complete. `llam_worker_clear_active_g()` executes under the existing reclaim barrier before releasing task scan references.
- Replace every direct `shard->current` writer with the worker helpers and remove the field after all readers use `llam_shard_load_lease_current()`. The accessor derives the lease owner's active-G and is not a second writable authority.
- Preserve public stats and trace aggregation by continuing to update the existing shard metrics under the existing locks.

- [ ] **Step 1: Add RED tests for publication ordering and reclaim lifetime**

  Add tests that pause after task-state preparation and after active-G publication, sample the lease owner/current accessor, exercise direct yield and reinject, and race watchdog/debug sampling with task exit/reclaim. The tests must fail to compile or fail assertions before the worker helpers are present.

- [ ] **Step 2: Implement worker active-G publication and migrate scheduler paths**

  Update `llam_set_task_running()`, `llam_clear_current_task()`, direct yield, spawn/wake/reinject, blocking leave, and wait tracking to use the worker active-G. Preserve the existing task state transitions and lock boundaries; do not move queues or timers.

- [ ] **Step 3: Migrate diagnostics and remove independent shard current writes**

  Convert watchdog load/merge/debug readers to `llam_shard_load_lease_current()`, delete direct `shard->current` stores, and remove the field only when the source audit finds no remaining writer. Keep the accessor's object-lifetime protection through `shard->lock` and task scan references.

- [ ] **Step 4: Run GREEN verification**

  Run `make test_legacy_lease_internal`, `make test_runtime_core`, `make test_runtime_stress`, `make test_runtime_shutdown_internal`, and `make test`. Run both debug and `-DNDEBUG -Werror` builds and verify no public header/stat/export changes.

- [ ] **Step 5: Commit**

```bash
git add src/internal/runtime_types.h src/internal/runtime_state.h \
  src/core/sched/scheduler.c src/core/task/yield_join_sleep.c \
  src/core/task/spawn.c src/core/sched/reinject.c \
  src/core/wait/wait_tracking.c src/core/api/blocking_api.c \
  src/core/debug/debug.c src/core/sched/core_queue.c \
  src/engine/watchdog/watchdog_probe.c src/engine/watchdog/watchdog_scale.c \
  src/engine/watchdog/watchdog_merge.c src/engine/runtime_watchdog_internal.h \
  src/core/lifecycle/init.c src/core/lifecycle/shutdown.c \
  tests/test_legacy_lease_internal.c tests/test_runtime_core.c
git commit -m "refactor: publish current tasks through worker state"
```

### Task 5: Derive watchdog targets from the lease and version pause acknowledgements

**Files:**
- Modify: `src/internal/runtime_types.h`
- Modify: `src/internal/runtime_proto_sched.h`
- Modify: `src/core/sched/scheduler.c`
- Modify: `src/core/lifecycle/run.c`
- Modify: `src/core/lifecycle/shutdown.c`
- Modify: `src/engine/watchdog/watchdog_probe.c`
- Modify: `src/engine/watchdog/watchdog_scale.c`
- Modify: `src/engine/watchdog/watchdog_merge.c`
- Modify: `src/engine/watchdog/watchdog_worker.c`
- Modify: `src/engine/runtime_watchdog_internal.h`
- Modify: `src/core/lifecycle/init.c`
- Test: `tests/test_legacy_lease_internal.c`, `tests/test_runtime_shutdown_internal.c`

**Interfaces:**
- Consumes: Task 4 worker active-G and Task 3 lease transitions.
- Produces:

```c
bool llam_legacy_lease_snapshot_watchdog_locked(
    const llam_shard_t *shard,
    llam_worker_t **owner_out,
    uint64_t *incarnation_out,
    uint64_t *epoch_out,
    pthread_t *native_thread_out);
bool llam_shard_pause_ack_matches_locked(const llam_shard_t *shard,
                                         uint64_t request_sequence,
                                         uint64_t lease_epoch,
                                         uint64_t worker_incarnation);
```

- Remove `preempt_thread` after all readers are migrated. The watchdog holds `opaque_lock` while taking the owner/native-thread snapshot, then takes `shard->lock` in order, acquires a task scan reference, revalidates owner/epoch/incarnation, sets the existing advisory request, and calls `pthread_kill()` before releasing the protected lifetime.
- A stale signal remains advisory and cooperative; no async context capture is introduced. Helper exit may restore a primary target only after checking a live matching primary binding.
- Extend merge/steal pause acknowledgement with request sequence and lease epoch/incarnation. While pause is requested, lease transfer and merge mutation are mutually exclusive. Waiting and joins occur without locks; final validation occurs under lock.

- [ ] **Step 1: Add RED watchdog/pause tests**

  Test a watchdog sample racing helper handback, task reclaim, worker incarnation replacement, and late signal delivery. Test pause ACK reuse after an executor handoff and assert a stale ACK cannot permit offlining. Before migration these tests must fail because they cannot observe a lease-derived target or versioned ACK.

- [ ] **Step 2: Implement lease-derived watchdog snapshot**

  Replace direct `shard->preempt_thread` and `shard->current` access in `llam_watchdog_check_shard()` with the lease snapshot and worker active-G accessor. Keep pressure policy and metrics unchanged; only target/lifetime validation changes.

- [ ] **Step 3: Version pause acknowledgements and dynamic scaling gates**

  Update pause request/ack publication in `watchdog_scale.c`, `watchdog_merge.c`, and scheduler pause handling. Preserve shard-ID lock ordering and existing online counts; reject a merge if the request sequence, lease epoch, or worker incarnation changed.

- [ ] **Step 4: Run GREEN verification**

  Run `make test_legacy_lease_internal`, `make test_runtime_shutdown_internal`, `make test_runtime_stress`, `make test_multi_runtime_core`, and `make test`. Run focused ASan/UBSan and TSan targets, plus `git diff --check` and the boundary/audit scripts.

- [ ] **Step 5: Commit**

```bash
git add src/internal/runtime_types.h src/internal/runtime_proto_sched.h \
  src/core/sched/scheduler.c src/core/lifecycle/run.c \
  src/core/lifecycle/shutdown.c src/engine/watchdog/watchdog_probe.c \
  src/engine/watchdog/watchdog_scale.c src/engine/watchdog/watchdog_merge.c \
  src/engine/watchdog/watchdog_worker.c \
  src/engine/runtime_watchdog_internal.h src/core/lifecycle/init.c \
  tests/test_legacy_lease_internal.c tests/test_runtime_shutdown_internal.c
git commit -m "fix: derive watchdog targeting from lease lifetime"
```

### Task 6: Integration audits, documentation, and whole-branch verification

**Files:**
- Modify: `Makefile` sanitizer/audit/test lists and clean files
- Modify: `CMakeLists.txt` test registration and sanitizer-compatible target setup
- Modify: `scripts/check_worker_identity_boundary.py` or a new `scripts/check_legacy_lease_boundary.py`
- Create: `scripts/test_legacy_lease_boundary.py` when the source audit needs fixtures separate from Phase 0
- Modify: `docs/internals/architecture.md`
- Modify: `docs/superpowers/specs/2026-09-08-phase1a-legacy-execution-lease-design.md` only for verified clarifications
- Modify: `tests/test_legacy_lease_internal.c` and relevant existing stress tests

**Interfaces:**
- Consumes: all five implementation commits and their focused tests.
- Produces: a reproducible boundary audit that rejects `processor->executor`, `worker->owns_p`, `worker->preempt_target`, `processor->current_g`, copied P resources, independent `shard->current`/`preempt_thread` writers, and unversioned pause ACK decisions; no wrapper code or wrapper dependencies.

- [ ] **Step 1: Write failing audit fixtures**

  Add fixtures for a direct second `shard->current` writer, an unvalidated ownerless grant, an unversioned pause ACK, a watchdog target read outside the lease lifetime, and a prohibited future-authority field. Run the fixture test and confirm each violation is detected.

- [ ] **Step 2: Implement the production audit and build registration**

  Extend the scanner to cover the exact prohibited names and lock/order markers, add the new internal test to `make test`, CMake/CTest, ASan/UBSan, TSan, and `CLEAN_FILES`, and keep test-hook symbols absent from static and shared production artifacts.

- [ ] **Step 3: Update documentation from the verified implementation**

  Document the lease state machine, ownerless return claimant, handback ACK, lock order, watchdog lifetime snapshot, pause generations, and the explicit Phase 1B/Phase 2 boundary. State that language wrappers are excluded.

- [ ] **Step 4: Run the complete verification matrix**

  Run, from a clean worktree:

```bash
make test
make test-hardening
cmake -S . -B build-phase1a-verify -DCMAKE_BUILD_TYPE=Debug
cmake --build build-phase1a-verify -j4
ctest --test-dir build-phase1a-verify --output-on-failure
make asan-test_legacy_lease_internal
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_legacy_lease_internal
make tsan-test_legacy_lease_internal
TSAN_OPTIONS=halt_on_error=1 ./tsan-test_legacy_lease_internal
python3 scripts/test_legacy_lease_boundary.py
python3 scripts/check_legacy_lease_boundary.py
make audit-production-test-hooks audit-shared-exports
git diff --check
git diff --exit-code 2cde642 -- include/llam
```

  Confirm public headers and exported symbols are unchanged, all tests pass, sanitizer artifacts are cleaned, and the implementation worktree is clean. Record native Windows/BSD execution as an environment limitation if those hosts are unavailable.

- [ ] **Step 5: Commit**

```bash
git add Makefile CMakeLists.txt scripts docs tests
git commit -m "test: enforce phase 1a lease boundaries"
```

