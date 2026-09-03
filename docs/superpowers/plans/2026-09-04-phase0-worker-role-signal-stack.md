# Phase 0 Worker-Role and Signal-Stack Isolation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add explicit legacy scheduler-role identity and isolate primary/helper alternate signal stacks without moving scheduling authority out of `llam_shard_t`.

**Architecture:** Each shard remains the authoritative scheduler object and embeds one descriptive processor identity plus stable primary/helper worker slots. New TLS and narrowly named predicates replace context-address role tokens, while a single eager mapping per shard supplies two non-overlapping worker-owned alternate-stack slices with independent restore state.

**Tech Stack:** C11, pthreads, C11 atomics, POSIX `sigaltstack`/`mmap`, Windows compatibility layer, Make, CMake/CTest, Python 3 source audits.

**Spec:** `docs/superpowers/specs/2026-09-04-phase0-worker-role-signal-stack-design.md`

## Global Constraints

- Implement from clean baseline `79f1c03e9bd81a9a38233e935df698e86d7fd427` in branch `₩₩phase0-worker-role-signal-stack`; do not import the dirty main worktree.
- Public files under `include/llam/`, public structure sizes, exported symbols, stats/dump keys, trace identity, errno behavior, task placement, and pinned-task semantics must not change.
- `llam_shard_t` remains the sole authority for `current`, `preempt_thread`, native-thread lifecycle, opaque state, queues, timers, allocator/cache, wake/I/O routing, online/pause state, metrics, and trace state.
- Do not add `worker->active_g`, `worker->owns_processor`, `worker->preempt_target`, `processor->executor`, `processor->current_g`, `processor->lease_epoch`, or duplicate P-local resources.
- Worker/P association describes topology only. It never proves execution ownership or grants a scheduling fast path.
- Worker incarnation is used only for diagnostics and signal-stack lifecycle.
- Existing lost-current publication behavior is not fixed and must not be characterized as expected behavior.
- The primary/helper alternate-stack slices must be disjoint, and previous-stack/install state must have exactly one worker authority from the migration commit onward.
- `previous_sigaltstack`, installed state, and installed incarnation are read only by their owner thread or after the native thread has exited/joined. Tests publish owner-thread snapshots through atomics or a completed task; they never sample these plain fields concurrently.
- Distinguish helper `pthread_create` failure, helper `sigaltstack` `EAGAIN`, and helper non-`EAGAIN` `sigaltstack` failure in tests. Preserve their existing redirect/nonfatal/fatal meanings.
- Predicate conversion must reproduce each call site's old context-address truth value without adding unrelated preconditions.
- Use separate checked arithmetic for slice sizing and total mapping sizing. Assert that both slices lie completely within the combined mapping.
- Test-only hooks exist only under `LLAM_ENABLE_TEST_HOOKS`; production static/shared artifacts must not contain their symbols.
- Any subagent used for execution or review must use `gpt-5.6-luna` with `max` reasoning and an isolated context.

---

## File Map

- Create `tests/test_worker_identity_internal.c`: deterministic single-shard characterization, TLS/role snapshots, repeated-run checks, helper takeover/failure checks, and POSIX alternate-stack assertions.
- Modify `Makefile`: build, run, clean, sanitizer-build, and production-hook audit integration for the new internal test.
- Modify `CMakeLists.txt`: testhooks-linked executable, platform definitions, and CTest registration.
- Modify `src/internal/runtime_types.h`: internal processor/worker types, embedded identity cells, worker signal-stack state, and combined mapping storage.
- Modify `src/internal/runtime_state.h`: new TLS declarations and narrowly scoped legacy-role predicates.
- Modify `src/core/lifecycle/runtime.c`: TLS definitions.
- Modify `src/core/lifecycle/init.c`: identity initialization, nested-TLS preservation, checked paired mapping creation, and partial-init state.
- Modify `src/core/lifecycle/shutdown.c`: new TLS clearing and paired mapping destruction after worker/helper termination.
- Modify `src/core/sched/scheduler.c`: primary/helper identity bind/clear, incarnation changes, and worker-specific signal-stack installation.
- Modify `src/internal/runtime_proto_core.h`: worker-based signal-stack signatures and test-only signal-install fault hook.
- Modify `src/internal/runtime_proto_sched.h`: test-only helper-create fault hook.
- Modify `src/core/platform/platform.c`: worker-local `sigaltstack` implementation and one-shot test fault injection.
- Modify `src/core/time/timer.c`: one-shot helper-thread-create fault injection.
- Modify `src/core/api/blocking_api.c`: explicit primary/helper predicates.
- Modify `src/core/task/spawn.c`: explicit legacy-primary predicates for local spawn and task-ID fast paths.
- Modify `src/core/task/yield_join_sleep.c`: explicit legacy-direct-context predicates.
- Modify `src/core/wait/wait_tracking.c`: explicit legacy-direct-context predicate.
- Modify `src/core/sched/reinject.c`: explicit legacy-direct-context predicate.
- Create `scripts/check_worker_identity_boundary.py`: repository boundary scanner.
- Create `scripts/test_worker_identity_boundary.py`: scanner unit tests.
- Modify `docs/internals/architecture.md`: document Phase 0 identity/authority boundaries.

### Task 1: Characterize Legacy Scheduler Roles and Helper Lifecycle

**Files:**
- Create: `tests/test_worker_identity_internal.c`
- Modify: `src/internal/runtime_proto_sched.h`
- Modify: `src/core/time/timer.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: existing `g_llam_tls_shard`, `g_llam_tls_task`, `g_llam_tls_scheduler_ctx`, `llam_enter_blocking()`, `llam_leave_blocking()`, and explicit-runtime lifecycle APIs.
- Produces: test target `test_worker_identity_internal`; test-only `void llam_worker_test_force_helper_create_error(int error_code)`; deterministic takeover/re-run harness used by later tasks.

- [ ] **Step 1: Add build plumbing and the first failing helper-create injection test**

Add a standard licensed C test file that includes `runtime_internal.h` and defines a deterministic runtime helper:

```c
static int create_deterministic_runtime(llam_runtime_t **out) {
    llam_runtime_opts_t opts;

    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return -1;
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    return llam_runtime_create(&opts, sizeof(opts), out);
}
```

Declare the test-only API in `runtime_proto_sched.h`:

```c
#if defined(LLAM_ENABLE_TEST_HOOKS)
void llam_worker_test_force_helper_create_error(int error_code);
#endif
```

Add a test that arms `EAGAIN`, enters/leaves opaque blocking, and asserts the
runtime remains drainable while `opaque_redirect_activations` increases. Build
the executable against `llam_runtime_testhooks` in CMake and
`$(RUNTIME_TESTHOOK_OBJS)` in Make. The Make integration must add the binary to
`CLEAN_FILES`, `LINK_TARGETS`, and `test`; add a dedicated test object variable
and compile rule with `-DLLAM_ENABLE_TEST_HOOKS=1`; and add
`src/core/time/timer.o` to `TESTHOOK_RUNTIME_OVERRIDE_OBJS` plus the matching
`filter-out` entry so the hook-bearing timer object replaces the production
object. CMake must add the executable, include `src`, define
`LLAM_ENABLE_TEST_HOOKS=1`, link `llam_runtime_testhooks`, include it in the
common test configuration loop, and register it with CTest.

- [ ] **Step 2: Run the focused target and verify the link failure**

Run:

```bash
make test_worker_identity_internal
```

Expected: FAIL with an undefined reference to
`llam_worker_test_force_helper_create_error`.

- [ ] **Step 3: Implement one-shot helper-create failure injection without production symbols**

In `src/core/time/timer.c`, add:

```c
#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_int g_llam_test_helper_create_error;

void llam_worker_test_force_helper_create_error(int error_code) {
    atomic_store_explicit(&g_llam_test_helper_create_error,
                          error_code,
                          memory_order_release);
}
#endif
```

Immediately before `pthread_create()` in `llam_ensure_opaque_helper_locked()`,
consume the value with `atomic_exchange_explicit(..., 0, memory_order_acq_rel)`.
For a nonzero value, set `opaque_helper_failed`, set `errno` to the injected
value, and return `-1` through the same branch as a real create failure.

- [ ] **Step 4: Add legacy owner-thread observations and takeover tests**

Define an owner-thread-only snapshot in the test file:

```c
typedef struct legacy_worker_observation {
    uintptr_t shard;
    uintptr_t scheduler_context;
    unsigned primary_by_context;
    unsigned helper_by_context;
} legacy_worker_observation_t;

static void observe_legacy_worker(legacy_worker_observation_t *out) {
    llam_shard_t *shard = g_llam_tls_shard;

    memset(out, 0, sizeof(*out));
    out->shard = (uintptr_t)shard;
    out->scheduler_context = (uintptr_t)g_llam_tls_scheduler_ctx;
    if (shard != NULL) {
        out->primary_by_context =
            g_llam_tls_scheduler_ctx == &shard->scheduler_ctx;
        out->helper_by_context =
            g_llam_tls_scheduler_ctx == &shard->opaque_scheduler_ctx;
    }
}
```

Use a root task that spawns a same-shard observer, calls
`llam_enter_blocking()`, waits on an atomic until the helper observer has copied
its snapshot, and calls `llam_leave_blocking()`. Add assertions for:

- root observes primary only;
- helper-dispatched child observes helper only;
- nested primary enter/leave increments depth without creating another helper;
- helper-dispatched child enter/leave activates redirect rather than recursively
  creating a helper;
- helper-create failure activates redirect and does not publish helper-ready.

All waits use an absolute 3-second deadline or a bounded spin/yield loop so a
regression fails instead of hanging.

- [ ] **Step 5: Add repeated-run and shutdown characterization**

Create two host pthreads that sequentially call
`llam_runtime_run_handle(runtime)` on the same runtime. Keep the first pthread
alive on a condition variable until the second run has captured its observation,
so the host cannot recycle the first `pthread_t`. Spawn one observation task
before each run and assert:

```c
first.scheduler_context == second.scheduler_context;
first.shard == second.shard;
pthread_equal(first_driver, second_driver) == 0;
```

Also cover runtime destruction after an idle helper and shutdown requested
while a helper is active. In the active case, a third synchronization thread
releases the blocked primary after the destroy caller has published its start;
the destroy must wait and complete without accessing freed shard state.

- [ ] **Step 6: Run characterization and regression gates**

Run:

```bash
make test_worker_identity_internal
./test_worker_identity_internal
for run in 1 2 3 4 5 6 7 8 9 10; do ./test_worker_identity_internal; done
make test_runtime_api_edges test_runtime_shutdown_internal test_runtime_stress
./test_runtime_api_edges
./test_runtime_shutdown_internal
./test_runtime_stress
```

Expected: every command exits 0; the focused test reports all legacy checks
passed.

- [ ] **Step 7: Commit the characterization layer**

```bash
git add Makefile CMakeLists.txt tests/test_worker_identity_internal.c \
  src/internal/runtime_proto_sched.h src/core/time/timer.c
git commit -m "test: characterize scheduler worker roles"
```

### Task 2: Add Embedded Identity Shells and TLS

**Files:**
- Modify: `src/internal/runtime_types.h`
- Modify: `src/internal/runtime_state.h`
- Modify: `src/core/lifecycle/runtime.c`
- Modify: `src/core/lifecycle/init.c`
- Modify: `src/core/lifecycle/shutdown.c`
- Modify: `src/core/sched/scheduler.c`
- Modify: `tests/test_worker_identity_internal.c`

**Interfaces:**
- Consumes: Task 1 takeover/repeated-run harness.
- Produces: `llam_processor_t`, `llam_worker_t`, `LLAM_WORKER_ROLE_PRIMARY`, `LLAM_WORKER_ROLE_OPAQUE_HELPER`, `g_llam_tls_worker`, and `g_llam_tls_processor`.

- [ ] **Step 1: Add failing identity and incarnation assertions**

Extend the test observation with:

```c
typedef struct worker_identity_observation {
    uintptr_t shard;
    uintptr_t worker;
    uintptr_t processor;
    uint64_t incarnation;
    unsigned role;
} worker_identity_observation_t;

static void observe_worker_identity(worker_identity_observation_t *out) {
    memset(out, 0, sizeof(*out));
    out->shard = (uintptr_t)g_llam_tls_shard;
    out->worker = (uintptr_t)g_llam_tls_worker;
    out->processor = (uintptr_t)g_llam_tls_processor;
    out->incarnation = atomic_load_explicit(
        &g_llam_tls_worker->incarnation, memory_order_acquire);
    out->role = (unsigned)g_llam_tls_worker->role;
}
```

Assert that primary/helper worker addresses differ, processor addresses match,
the primary slot address is stable across repeated runs, and the primary
incarnation increases between those runs.

- [ ] **Step 2: Run the focused test and verify compilation fails**

Run:

```bash
make test_worker_identity_internal
```

Expected: FAIL because the worker/processor types and TLS declarations do not
exist.

- [ ] **Step 3: Define minimal internal types and embed stable slots**

In `runtime_types.h`, add forward declarations and these minimal structures
before `struct llam_shard`:

```c
typedef struct llam_processor llam_processor_t;
typedef struct llam_worker llam_worker_t;

typedef enum llam_worker_role {
    LLAM_WORKER_ROLE_PRIMARY = 1,
    LLAM_WORKER_ROLE_OPAQUE_HELPER = 2,
} llam_worker_role_t;

struct llam_processor {
    llam_runtime_t *runtime;
    llam_shard_t *compat_shard;
    unsigned id;
};

struct llam_worker {
    llam_shard_t *compat_shard;
    llam_processor_t *associated_processor;
    llam_worker_role_t role;
    atomic_uint_fast64_t incarnation;
};
```

Embed `processor`, `primary_worker`, and `opaque_helper_worker` in
`llam_shard_t`. Do not add thread, active-task, lease, online, or P-resource
mirrors.

- [ ] **Step 4: Define, initialize, preserve, and clear the TLS tuple**

Declare and define:

```c
_Thread_local llam_worker_t *g_llam_tls_worker;
_Thread_local llam_processor_t *g_llam_tls_processor;
```

In the shard initialization loop, initialize processor/shard backpointers,
roles, and incarnation atomics. In `scheduler.c`, use small private helpers:

```c
static llam_worker_t *llam_scheduler_bind_identity(
    llam_shard_t *shard, llam_worker_t *worker, llam_ctx_t *ctx) {
    atomic_fetch_add_explicit(&worker->incarnation, 1U,
                              memory_order_acq_rel);
    g_llam_tls_shard = shard;
    g_llam_tls_worker = worker;
    g_llam_tls_processor = &shard->processor;
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = ctx;
    return worker;
}

static void llam_scheduler_clear_identity(void) {
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = NULL;
    g_llam_tls_processor = NULL;
    g_llam_tls_worker = NULL;
    g_llam_tls_shard = NULL;
}
```

Call the bind helper at primary/helper entry and the clear helper at normal and
early-failure exits. Extend `llam_runtime_init_rt()`'s managed-caller save/clear/
restore sequence and shutdown's caller-TLS clearing with both new TLS fields.

- [ ] **Step 5: Run identity, nested-runtime, and lifecycle tests**

Run:

```bash
make test_worker_identity_internal test_runtime_core test_multi_runtime_core
./test_worker_identity_internal
./test_runtime_core
./test_multi_runtime_core
```

Expected: all pass; nested runtime creation preserves all five TLS values and
all values are cleared on scheduler exit.

- [ ] **Step 6: Commit identity scaffolding**

```bash
git add src/internal/runtime_types.h src/internal/runtime_state.h \
  src/core/lifecycle/runtime.c src/core/lifecycle/init.c \
  src/core/lifecycle/shutdown.c src/core/sched/scheduler.c \
  tests/test_worker_identity_internal.c
git commit -m "refactor: add scheduler worker identity slots"
```

### Task 3: Replace Context-Address Role Tokens

**Files:**
- Modify: `src/internal/runtime_state.h`
- Modify: `src/core/api/blocking_api.c`
- Modify: `src/core/task/spawn.c`
- Modify: `src/core/task/yield_join_sleep.c`
- Modify: `src/core/wait/wait_tracking.c`
- Modify: `src/core/sched/reinject.c`
- Modify: `tests/test_worker_identity_internal.c`

**Interfaces:**
- Consumes: Task 2 worker/P TLS and immutable worker roles.
- Produces: `llam_tls_is_primary_scheduler_for()`, `llam_tls_is_opaque_helper_for()`, and `llam_tls_has_legacy_direct_handoff_context()` as internal static-inline predicates.

- [ ] **Step 1: Add failing predicate truth-table tests**

For every primary/helper observation, compare the new predicate result with the
legacy context identity captured in Task 1:

```c
obs->primary_predicate = llam_tls_is_primary_scheduler_for(g_llam_tls_shard);
obs->helper_predicate = llam_tls_is_opaque_helper_for(g_llam_tls_shard);
obs->direct_predicate =
    llam_tls_has_legacy_direct_handoff_context(g_llam_tls_shard);

if (obs->primary_predicate != obs->primary_by_context)
    return fail_msg("primary predicate changed the legacy truth value");
if (obs->helper_predicate != obs->helper_by_context)
    return fail_msg("helper predicate changed the legacy truth value");
if (obs->direct_predicate != obs->primary_by_context)
    return fail_msg("direct-handoff predicate changed the legacy truth value");
```

Retain Task 1 metrics checks proving helper execution rejects primary-only
direct paths.

- [ ] **Step 2: Run the focused test and verify compilation fails**

Run:

```bash
make test_worker_identity_internal
```

Expected: FAIL because the three predicates are undefined.

- [ ] **Step 3: Implement narrowly scoped predicates**

Add private static-inline matching logic in `runtime_state.h`:

```c
static inline bool llam_tls_matches_worker_role(
    const llam_shard_t *shard, const llam_worker_t *worker,
    llam_worker_role_t role) {
    return shard != NULL && worker != NULL &&
           g_llam_tls_shard == shard &&
           g_llam_tls_worker == worker &&
           g_llam_tls_processor == &shard->processor &&
           shard->processor.runtime == shard->runtime &&
           shard->processor.compat_shard == shard &&
           worker->compat_shard == shard &&
           worker->associated_processor == &shard->processor &&
           worker->role == role;
}
```

`llam_tls_is_primary_scheduler_for()` matches `primary_worker`,
`llam_tls_is_opaque_helper_for()` matches `opaque_helper_worker`, and
`llam_tls_has_legacy_direct_handoff_context()` delegates to the primary
predicate. Assertion builds compare the result with the corresponding legacy
context address whenever either representation claims a match.

- [ ] **Step 4: Convert role decisions one call site at a time**

Replace only the role comparison in each existing condition:

```c
from_primary = llam_tls_is_primary_scheduler_for(shard);
from_opaque_helper = llam_tls_is_opaque_helper_for(shard);
```

Use `llam_tls_has_legacy_direct_handoff_context()` in spawn, yield, join,
wake-handoff, and reinjection direct paths. Keep every surrounding runtime,
task, trace, timing, queue, timer, and policy condition byte-for-byte equivalent
where formatting allows. Do not change expressions that choose a concrete
`llam_ctx_t *` for a context switch.

- [ ] **Step 5: Verify raw role comparisons are gone and behavior is stable**

Run:

```bash
rg -n 'g_llam_tls_scheduler_ctx\s*[!=]=\s*&[^;]*(opaque_)?scheduler_ctx' \
  src --glob '*.c' --glob '*.h'
make test_worker_identity_internal test_runtime_core test_runtime_stress
./test_worker_identity_internal
./test_runtime_core
./test_runtime_stress
```

Expected: `rg` reports only the deliberate debug-equivalence assertions in
`runtime_state.h`; all tests pass and direct-path metrics retain their Task 1
truth table.

- [ ] **Step 6: Commit the predicate migration**

```bash
git add src/internal/runtime_state.h src/core/api/blocking_api.c \
  src/core/task/spawn.c src/core/task/yield_join_sleep.c \
  src/core/wait/wait_tracking.c src/core/sched/reinject.c \
  tests/test_worker_identity_internal.c
git commit -m "refactor: make scheduler worker roles explicit"
```

### Task 4: Isolate Primary and Helper Signal Stacks

**Files:**
- Modify: `src/internal/runtime_types.h`
- Modify: `src/internal/runtime_proto_core.h`
- Modify: `src/core/lifecycle/init.c`
- Modify: `src/core/lifecycle/shutdown.c`
- Modify: `src/core/platform/platform.c`
- Modify: `src/core/sched/scheduler.c`
- Modify: `tests/test_worker_identity_internal.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: Task 2 stable worker slots/incarnations and Task 1 deterministic helper harness.
- Produces: `int llam_install_thread_signal_stack(llam_worker_t *worker)`, `void llam_uninstall_thread_signal_stack(llam_worker_t *worker)`, and test-only `void llam_worker_test_force_sigaltstack_error(llam_worker_role_t role, int error_code)`.

- [ ] **Step 1: Add failing owner-thread alternate-stack observations**

Under `#if !LLAM_PLATFORM_WINDOWS`, extend the owner-thread snapshot:

```c
typedef struct worker_signal_observation {
    uintptr_t worker_stack_base;
    size_t worker_stack_size;
    uintptr_t installed_stack_base;
    size_t installed_stack_size;
    uint64_t worker_incarnation;
    uint64_t installed_incarnation;
    unsigned installed;
    int query_errno;
} worker_signal_observation_t;

static void observe_current_signal_stack(worker_signal_observation_t *out) {
    stack_t active;
    llam_worker_t *worker = g_llam_tls_worker;

    memset(out, 0, sizeof(*out));
    if (worker == NULL) {
        out->query_errno = EINVAL;
        return;
    }
    if (sigaltstack(NULL, &active) != 0) {
        out->query_errno = errno;
        return;
    }
    out->worker_stack_base = (uintptr_t)worker->signal_stack;
    out->worker_stack_size = worker->signal_stack_size;
    out->installed_stack_base = (uintptr_t)active.ss_sp;
    out->installed_stack_size = active.ss_size;
    out->worker_incarnation = atomic_load_explicit(
        &worker->incarnation, memory_order_acquire);
    out->installed_incarnation = worker->signal_stack_incarnation;
    out->installed = worker->sigaltstack_installed ? 1U : 0U;
}
```

Publish each snapshot from its owner task before another thread reads it. Assert
that primary and helper installed ranges match their worker ranges and do not
overlap.

- [ ] **Step 2: Run the focused test and verify it fails on shared authority**

Run:

```bash
make test_worker_identity_internal
./test_worker_identity_internal
```

Expected: compile failure because signal-stack fields are not on the worker, or
the non-overlap assertion fails before the migration is complete.

- [ ] **Step 3: Add worker-local fields and checked paired mapping storage**

Extend `llam_worker_t` with:

```c
void *signal_stack;
size_t signal_stack_size;
stack_t previous_sigaltstack;
bool sigaltstack_installed;
uint64_t signal_stack_incarnation;
```

Replace shard install-state fields with storage-only:

```c
void *signal_stack_mapping;
size_t signal_stack_mapping_size;
```

In `init.c`, calculate:

```c
slice_size = LLAM_ALTSTACK_BYTES;
if (slice_size < (size_t)SIGSTKSZ) slice_size = (size_t)SIGSTKSZ;
if (slice_size > SIZE_MAX / 2U) {
    return llam_runtime_init_fail_registered(rt, ENOMEM);
}
mapping_size = slice_size * 2U;
```

Map `mapping_size` once. Assign the first slice to `primary_worker` and the
second to `opaque_helper_worker`. Check pointer/range arithmetic before storing
the slices and assert both end addresses are within the combined mapping.

- [ ] **Step 4: Move install/restore authority to the current worker**

Change the prototypes and implementation to accept `llam_worker_t *`. Install
validates:

```c
worker != NULL;
g_llam_tls_worker == worker;
g_llam_tls_processor == worker->associated_processor;
worker->signal_stack != NULL;
worker->signal_stack_size >= (size_t)SIGSTKSZ;
atomic_load_explicit(&worker->incarnation, memory_order_acquire) != 0U;
!worker->sigaltstack_installed;
```

After successful `sigaltstack`, store the saved previous stack, set installed,
and copy the current incarnation into `signal_stack_incarnation`. Uninstall
verifies the same worker/incarnation, restores that worker's previous stack,
and clears both installed fields. A wrong worker/incarnation calls `abort()`
before attempting a restore.

- [ ] **Step 5: Add and test distinct one-shot signal-install failures**

Under `LLAM_ENABLE_TEST_HOOKS`, implement:

```c
void llam_worker_test_force_sigaltstack_error(
    llam_worker_role_t role, int error_code);
```

Use separate atomic slots per role and consume the selected error once inside
`llam_install_thread_signal_stack()`. Add tests proving:

- helper `EAGAIN`: helper runs, runtime does not record fatal, and no installed
  flag is published for that incarnation;
- helper `EINVAL`: helper publishes failed/not-ready, wakes the primary, records
  runtime fatal, and exits;
- Task 1 `pthread_create` failure remains a separate redirect-only path.

Add `src/core/platform/platform.o` to Make's
`TESTHOOK_RUNTIME_OVERRIDE_OBJS` and its matching `filter-out` list so the
testhooks-linked executable receives the injected implementation without
putting the hook into production objects.

- [ ] **Step 6: Update scheduler and teardown sequencing**

Pass `primary_worker` or `opaque_helper_worker` into the scheduler install/
uninstall helpers. On every helper install failure, clear the complete TLS tuple
before the thread returns. In shutdown, stop/join all primary/helper users
before checking installed flags and unmapping the combined range once. Partial
init cleanup only unmaps storage; it never calls `sigaltstack()` on behalf of a
different thread.

- [ ] **Step 7: Verify focused, lifecycle, and sanitizer gates**

Add `asan-test_worker_identity_internal` and
`tsan-test_worker_identity_internal` to the Make sanitizer target lists, using
`$(RUNTIME_TESTHOOK_OBJS)` and the focused test object. Run:

```bash
make test_worker_identity_internal test_runtime_shutdown_internal
./test_worker_identity_internal
for run in 1 2 3 4 5 6 7 8 9 10; do ./test_worker_identity_internal; done
./test_runtime_shutdown_internal
make test-asan
make test-tsan
```

Expected: focused and shutdown tests pass; ASan/UBSan pass; TSan either passes
or produces only an already-documented platform/runtime incompatibility and is
reported rather than hidden.

- [ ] **Step 8: Commit signal-stack isolation**

```bash
git add Makefile src/internal/runtime_types.h src/internal/runtime_proto_core.h \
  src/core/lifecycle/init.c src/core/lifecycle/shutdown.c \
  src/core/platform/platform.c src/core/sched/scheduler.c \
  tests/test_worker_identity_internal.c
git commit -m "fix: isolate scheduler signal stacks by worker"
```

### Task 5: Enforce Boundaries, Document, and Run Final Verification

**Files:**
- Create: `scripts/check_worker_identity_boundary.py`
- Create: `scripts/test_worker_identity_boundary.py`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`
- Modify: `docs/internals/architecture.md`
- Modify: any Phase 0 file only to resolve a failing final invariant

**Interfaces:**
- Consumes: Tasks 1-4 completed object model and behavior.
- Produces: persistent source-boundary audit and final Phase 0 evidence.

- [ ] **Step 1: Write failing unit tests for the boundary scanner**

Create scanner tests around a pure function
`scan_source(relative_path: str, text: str) -> list[str]`:

```python
def test_rejects_raw_context_role_check():
    errors = scan_source(
        "src/core/task/example.c",
        "if (g_llam_tls_scheduler_ctx == &shard->scheduler_ctx) {}",
    )
    assert any("raw scheduler-context role check" in error for error in errors)

def test_allows_context_routing_expression():
    errors = scan_source(
        "src/core/task/example.c",
        "ctx = g_llam_tls_scheduler_ctx != NULL ? "
        "g_llam_tls_scheduler_ctx : &shard->scheduler_ctx;",
    )
    assert errors == []

def test_rejects_future_execution_authority_names():
    errors = scan_source(
        "src/internal/runtime_state.h",
        "bool worker_is_executor(llam_worker_t *worker);",
    )
    assert any("Phase 0 execution authority" in error for error in errors)
```

- [ ] **Step 2: Run the scanner tests and verify import failure**

Run:

```bash
python3 scripts/test_worker_identity_boundary.py
```

Expected: FAIL because `check_worker_identity_boundary` does not exist.

- [ ] **Step 3: Implement the repository scanner**

Implement the pure scanner and a CLI that visits `src/**/*.c` and
`src/**/*.h`. It must reject:

- raw equality/inequality role checks between `g_llam_tls_scheduler_ctx` and a
  primary/helper scheduler context outside the debug-equivalence block in
  `runtime_state.h`;
- identifiers `worker_is_executor`, `worker_owns_processor`,
  `processor_current_worker`, `worker->active_g`, `worker->owns_processor`,
  `worker->preempt_target`, `processor->executor`, `processor->current_g`, and
  `processor->lease_epoch`;
- exact obsolete shard member accesses `shard->signal_stack`,
  `shard->signal_stack_size`, `shard->previous_sigaltstack`, and
  `shard->sigaltstack_installed`.

The matcher must not reject context-routing `!= NULL ?` expressions or the
combined storage names `signal_stack_mapping` and
`signal_stack_mapping_size`.

- [ ] **Step 4: Integrate audits and documentation**

Add both Python scripts to `make test`; register the scanner test and repository
scan in CTest when Python is available. Extend
`audit-production-test-hooks` to reject `llam_worker_test_` symbols in
production artifacts. Update `docs/internals/architecture.md` with:

- shard remains scheduling authority in Phase 0;
- processor/worker cells are identity scaffolding, not an executor lease;
- primary/helper have distinct alternate-stack slices;
- Phase 1A is the first phase allowed to introduce executor/current/watchdog
  authority.

- [ ] **Step 5: Run plan/spec consistency and source audits**

Run:

```bash
python3 scripts/test_worker_identity_boundary.py
python3 scripts/check_worker_identity_boundary.py
make audit-production-test-hooks audit-shared-exports
git diff --exit-code 79f1c03 -- include/llam
rg -n 'worker_is_executor|worker_owns_processor|processor_current_worker|active_g|lease_epoch' src
```

Expected: both Python commands pass; production/shared audits pass; public
header diff is empty; `rg` reports no forbidden Phase 0 authority identifiers.

- [ ] **Step 6: Run full Make and CMake verification**

Run:

```bash
make test
cmake -S . -B build-phase0 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-phase0 -j4
ctest --test-dir build-phase0 --output-on-failure
```

Expected: all commands exit 0, including the new focused test and boundary
audit. Do not reuse a CMake cache from the dirty main worktree.

- [ ] **Step 7: Inspect the final patch and verify commit boundaries**

Run:

```bash
git status --short
git diff --check 79f1c03..HEAD
git log --oneline --decorate 79f1c03..HEAD
git diff --stat 79f1c03..HEAD
```

Expected: no uncommitted production/test changes before the final documentation
commit, no whitespace errors, and one independently testable commit per task.

- [ ] **Step 8: Commit boundary enforcement and documentation**

```bash
git add Makefile CMakeLists.txt scripts/check_worker_identity_boundary.py \
  scripts/test_worker_identity_boundary.py docs/internals/architecture.md
git commit -m "test: enforce phase zero worker boundaries"
```

## Final Completion Checklist

- [ ] `make test` passes from a clean tree.
- [ ] Focused test passes ten consecutive runs.
- [ ] ASan/UBSan focused and shutdown coverage passes.
- [ ] TSan outcome is recorded honestly.
- [ ] CMake build and CTest pass.
- [ ] Public headers and shared exports are unchanged.
- [ ] Production artifacts contain no worker test-hook symbols.
- [ ] No raw scheduler-context role decisions remain outside explicit debug equivalence checks.
- [ ] No execution-authority fields or APIs entered Phase 0.
- [ ] Primary/helper signal-stack ranges are disjoint and independently restored.
- [ ] All scheduler TLS is preserved across nested runtime creation and cleared at exits.
- [ ] `shard->current`, `shard->preempt_thread`, and all P-like resources remain authoritative.
