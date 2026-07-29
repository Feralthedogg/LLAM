# Runtime Embedding Contract Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give language runtimes and host event loops an O(1) task context, exact switch hooks, hard task affinity, and a bounded external scheduler driver without weakening LLAM's existing ownership rules.

**Architecture:** Public option structs grow only by tail append. Task context lives directly in `llam_task`; all scheduler/task transitions pass the existing sanitizer-aware switch gateway; one common affinity predicate protects every migration and dispatch boundary; and external mode owns a dedicated coalesced readiness doorbell while reusing one extracted scheduler quantum. External mode has exactly one shard and never performs direct task-to-task handoff.

**Tech Stack:** C11 atomics, pthread-compatible scheduler core, Linux eventfd, Darwin/BSD nonblocking pipe, Windows manual-reset event, Make/CMake manifest parity, CTest, ASan/UBSan, TSan.

## Global Constraints

- Preserve cancellation, generation, active-operation, wait-owner, I/O retirement, and cross-runtime `EXDEV` invariants.
- Preserve `LLAM_RUNTIME_OPTS_V2_2_SIZE`; all new public fields are tail additions.
- Preserve caller-size reads and writes for every public ABI struct.
- Keep research-off and research-on installed headers and exported public symbols identical.
- Do not expose test hooks from production archives.
- `LLAM_SPAWN_F_PINNED` means execution only on the logical home shard; a same-shard opaque helper remains allowed.
- External driver mode has exactly one shard, no dynamic workers, no direct task-to-task handoff, and at most one task segment per successful drive call.
- The readiness object is borrowed from LLAM and must remain valid until runtime destruction.
- Switch callbacks are nonblocking, non-reentrant into LLAM, non-throwing, and may run concurrently for different tasks.
- No version bump, tag, or release occurs while the native performance gate remains `REJECT`.
- Darwin, Linux, Windows, and supported BSD builds remain first-class.

---

### Task 1: Append the task-context ABI

**Files:**
- Modify: `include/llam/runtime.h`
- Modify: `src/core/base/abi.c`
- Modify: `src/core/lifecycle/resource_plan.c`
- Modify: `src/internal/runtime_types.h`
- Modify: `src/core/task/spawn.c`
- Modify: `src/core/task/task_alloc.c`
- Create: `src/core/task/task_context.c`
- Modify: `config/llam-sources.json`
- Modify: `tests/test_abi_contract.c`
- Create: `tests/test_task_context_cases.inc`
- Modify: `tests/test_runtime_core.c`

**Interfaces:**
- Produces: `LLAM_TASK_CONTEXT_SLOT_COUNT == 4U`.
- Produces: `void *llam_task_user_context(void)`.
- Produces: `void *llam_task_context_slot_get(uint32_t slot)`.
- Produces: `int llam_task_context_slot_set(uint32_t slot, void *value)`.
- Produces: tail field `void *user_context` in `llam_spawn_opts_t`.
- Produces: ABI metadata fields `task_context_slot_count` and `runtime_readiness_size`; Task 4 fills the latter with the final readiness struct size.

- [ ] **Step 1: Write failing ABI-prefix and metadata tests**

Add a legacy spawn prefix ending at `cancel_token`, verify its poison tail is
not consumed, and require current metadata to advertise four inline slots:

```c
typedef struct legacy_spawn_opts {
    uint32_t task_class;
    uint32_t stack_class;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t deadline_ns;
    llam_cancel_token_t *cancel_token;
} legacy_spawn_opts_t;

if (info.task_context_slot_count != LLAM_TASK_CONTEXT_SLOT_COUNT ||
    info.task_context_slot_count != 4U) {
    return test_fail("ABI metadata did not advertise four task context slots");
}
```

- [ ] **Step 2: Run the focused ABI test and confirm RED**

Run:

```bash
make -j4 test_abi_contract
./test_abi_contract
```

Expected: compilation fails because the slot-count macro and metadata field do
not exist.

- [ ] **Step 3: Append public fields and accessors**

Append, without changing `LLAM_RUNTIME_OPTS_V2_2_SIZE`:

```c
#define LLAM_TASK_CONTEXT_SLOT_COUNT 4U

typedef struct llam_spawn_opts {
    /* frozen existing prefix */
    void *user_context;
} llam_spawn_opts_t;

LLAM_API void *llam_task_user_context(void);
LLAM_API void *llam_task_context_slot_get(uint32_t slot);
LLAM_API int llam_task_context_slot_set(uint32_t slot, void *value);
```

Append metadata only after the existing `platform_name` field:

```c
uint32_t task_context_slot_count;
uint32_t reserved1;
size_t runtime_readiness_size;
```

The option initializer keeps new pointer fields null. Older caller prefixes
remain unchanged.

- [ ] **Step 4: Add direct task storage and prefix-safe spawn copying**

Add to `llam_task`:

```c
void *user_context;
void *context_slots[LLAM_TASK_CONTEXT_SLOT_COUNT];
```

Copy `user_context` only behind `LLAM_SPAWN_OPTS_PREFIX_HAS_FIELD`. Reset the
context and all four slots before a recycled task can be published:

```c
task->user_context = NULL;
memset(task->context_slots, 0, sizeof(task->context_slots));
```

The runtime never dereferences or frees these pointers.

- [ ] **Step 5: Implement O(1) current-task accessors**

`src/core/task/task_context.c` uses `g_llam_tls_task` directly:

```c
void *llam_task_context_slot_get(uint32_t slot) {
    if (g_llam_tls_task == NULL) {
        errno = ENOTSUP;
        return NULL;
    }
    if (slot >= LLAM_TASK_CONTEXT_SLOT_COUNT) {
        errno = EINVAL;
        return NULL;
    }
    return g_llam_tls_task->context_slots[slot];
}
```

The setter follows the same checks and stores the pointer. Successful getters
must preserve the caller's `errno`, including when the stored value is null.

- [ ] **Step 6: Add lifecycle and migration tests**

`tests/test_task_context_cases.inc` must prove:

- spawn-time `user_context` is visible in the task;
- all four slots round-trip in O(1) accessors;
- invalid slots return `EINVAL`;
- unmanaged calls return `ENOTSUP`;
- values survive yield, park/wake, and a proven two-worker migration;
- recycled task objects start with null context and slots;
- LLAM never frees caller-owned context.

- [ ] **Step 7: Run focused and owning suites**

Run:

```bash
make -j4 test_abi_contract test_runtime_core test_multi_runtime_core
./test_abi_contract
./test_runtime_core
./test_multi_runtime_core
python3 scripts/audit_build_manifests.py
```

Expected: all pass and the source manifest contains `task_context.c`.

- [ ] **Step 8: Commit**

```bash
git add include/llam/runtime.h src/core/base/abi.c \
  src/core/lifecycle/resource_plan.c src/internal/runtime_types.h \
  src/core/task/spawn.c src/core/task/task_alloc.c \
  src/core/task/task_context.c config/llam-sources.json \
  tests/test_abi_contract.c tests/test_task_context_cases.inc \
  tests/test_runtime_core.c
git commit -m "feat: add constant-time task context"
```

---

### Task 2: Add exact suspend and resume hooks to the switch gateway

**Files:**
- Modify: `include/llam/runtime.h`
- Modify: `src/core/lifecycle/resource_plan.c`
- Modify: `src/internal/runtime_types.h`
- Modify: `src/core/lifecycle/init.c`
- Modify: `src/core/base/errno.c`
- Modify: `src/internal/runtime_proto_core.h`
- Create: `tests/test_switch_hook_cases.inc`
- Modify: `tests/test_runtime_core.c`
- Modify: `tests/test_runtime_shutdown_internal.c`

**Interfaces:**
- Consumes: Task 1's `llam_task_t::user_context`.
- Produces: `llam_task_switch_hook_fn`.
- Produces: tail runtime options `on_task_resume`, `on_task_suspend`, and `switch_hook_context`.
- Produces: internal `llam_task_hook_resume()` and `llam_task_hook_suspend()` called only by the common switch gateway.

- [ ] **Step 1: Write failing callback-order tests**

Record compact events:

```c
typedef enum hook_event_kind {
    HOOK_RESUME,
    TASK_BODY,
    HOOK_SUSPEND,
} hook_event_kind_t;

typedef struct hook_event {
    hook_event_kind_t kind;
    void *user_context;
    int observed_errno;
} hook_event_t;
```

Require exact sequences for:

- scheduler → task → scheduler;
- scheduler → task A → task B → scheduler using a proven direct handoff;
- task exit without a synthetic extra callback;
- migration between different scheduler pthreads.

- [ ] **Step 2: Run the focused test and confirm RED**

Run:

```bash
make -j4 test_runtime_core
./test_runtime_core
```

Expected: compilation fails because hook callback types and option fields do not
exist.

- [ ] **Step 3: Append callback types and runtime options**

Add:

```c
typedef void (*llam_task_switch_hook_fn)(void *hook_context,
                                         void *task_user_context);
```

Append to `llam_runtime_opts_t`:

```c
llam_task_switch_hook_fn on_task_resume;
llam_task_switch_hook_fn on_task_suspend;
void *switch_hook_context;
```

Copy each pointer only when the complete field is present. Store the resolved
callbacks and context on `llam_runtime`; their caller-owned lifetime extends
through runtime destruction.

- [ ] **Step 4: Put callback invocation inside the existing sanitizer gateway**

The gateway ordering is:

```c
/* scheduler -> task */
llam_task_restore_errno(task);
llam_task_hook_resume(task);
llam_sanitizer_before_scheduler_to_task(task);
llam_ctx_switch(scheduler_ctx, &task->ctx);

/* task -> scheduler */
llam_task_save_errno(task);
llam_task_hook_suspend(task);
llam_sanitizer_before_task_to_scheduler(task, terminal);
llam_ctx_switch(&task->ctx, scheduler_ctx);

/* task A -> task B */
llam_task_save_errno(from);
llam_task_hook_suspend(from);
llam_task_restore_errno(to);
llam_task_hook_resume(to);
llam_sanitizer_before_task_to_task(from, to);
llam_ctx_switch(&from->ctx, &to->ctx);
```

Save and restore the logical task errno around each callback so callback
implementation details cannot change the resumed task's `errno`.

- [ ] **Step 5: Remove the duplicate inline switch implementation**

Change `llam_switch_task_to_task_hot` into a thin call to
`llam_switch_task_to_task`. The common gateway remains the only non-assembly
switch caller. Update the existing switch-call audit so any direct
`llam_ctx_switch` outside context implementations and the gateway fails CI.

- [ ] **Step 6: Test callback restrictions and teardown**

Tests must prove:

- null callbacks add no events;
- callback context and task user context are exact;
- callback-side `errno` changes do not leak into the logical task;
- the same task is never hooked concurrently;
- different tasks may be hooked concurrently;
- runtime destroy emits no synthetic suspend/resume event;
- legacy option prefixes never read callback poison bytes.

- [ ] **Step 7: Run focused, sanitizer, and call-site gates**

Run:

```bash
make -j4 test_runtime_core test_runtime_shutdown_internal
./test_runtime_core
./test_runtime_shutdown_internal
make -j4 test-asan
make -j4 test-tsan
rg -n 'llam_ctx_switch\\(' src --glob '*.[ch]' --glob '!src/core/base/errno.c' \
  --glob '!src/core/context/**'
```

Expected: both suites and sanitizer gates pass; the final search has no
unauthorized call site.

- [ ] **Step 8: Commit**

```bash
git add include/llam/runtime.h src/core/lifecycle/resource_plan.c \
  src/internal/runtime_types.h src/core/lifecycle/init.c \
  src/core/base/errno.c src/internal/runtime_proto_core.h \
  tests/test_switch_hook_cases.inc tests/test_runtime_core.c \
  tests/test_runtime_shutdown_internal.c
git commit -m "feat: expose task switch hooks"
```

---

### Task 3: Enforce hard logical-shard affinity

**Files:**
- Modify: `include/llam/runtime.h`
- Modify: `src/internal/runtime_proto_core.h`
- Modify: `src/core/sched/core_queue.c`
- Modify: `src/core/sched/reinject.c`
- Modify: `src/core/sched/scheduler.c`
- Modify: `src/engine/scheduler/scheduler_engine.c`
- Modify: `src/engine/watchdog/watchdog_rehome.c`
- Modify: `src/engine/watchdog/watchdog_merge.c`
- Modify: `src/core/task/spawn.c`
- Modify: `src/core/task/yield_join_sleep.c`
- Create: `tests/test_hard_affinity_cases.inc`
- Modify: `tests/test_runtime_shutdown_internal.c`
- Modify: `docs/operations.md`

**Interfaces:**
- Produces: internal `unsigned llam_task_required_shard(const llam_runtime_t *, const llam_task_t *)`.
- Produces: internal `bool llam_task_may_run_on_shard(const llam_task_t *, const llam_shard_t *)`.
- Changes: `llam_take_overflow_task` to `llam_take_overflow_task_for_shard`.

- [ ] **Step 1: Write deterministic escape-route tests**

Use test hooks only in `llam_runtime_testhooks` to force:

- a pinned task into a full global overflow queue;
- a delayed thief to encounter a pinned task;
- an opaque redirect activation with pinned tasks in inject/hot/normal lanes;
- parked wait, submit queue, inflight I/O, and merge scans;
- a direct-handoff candidate on a foreign shard.

Each task records the scheduler shard id on every resume. Require every sample
to equal its immutable home shard.

- [ ] **Step 2: Run the focused test and confirm RED**

Run:

```bash
make -j4 test_runtime_shutdown_internal
./test_runtime_shutdown_internal
```

Expected: at least overflow or opaque redirect executes a pinned task on a
foreign shard before the production fix.

- [ ] **Step 3: Centralize the hard-affinity predicate**

Implement:

```c
bool llam_task_may_run_on_shard(const llam_task_t *task,
                                const llam_shard_t *shard) {
    return task != NULL && shard != NULL &&
           task->owner_runtime == shard->runtime &&
           (((task->flags & LLAM_TASK_FLAG_PINNED) == 0U) ||
            task->home_shard == shard->id);
}
```

An invalid pinned home is fail-closed and records a fatal runtime error instead
of modulo-remapping.

- [ ] **Step 4: Make overflow consumption shard-aware**

Under `overflow_lock`, inspect at most the original queue depth. Pop the first
task that may run on the requesting shard; rotate foreign pinned tasks to the
tail and kick their home shard. Preserve FIFO order among skipped pinned tasks.

- [ ] **Step 5: Reject migration at every producer path**

- opaque redirect keeps pinned tasks on the home queues;
- reinjection overrides an explicit foreign target with the home shard;
- stealing returns pinned tasks to their home inject queue;
- merge and rehome predicates reject pinned tasks defensively;
- spawn redirect never migrates a newly published pinned task;
- direct handoff requires both tasks to be runnable on the current shard.

Do not rewrite `home_shard` for a pinned task.

- [ ] **Step 6: Add a final dispatch guard**

Before `llam_set_task_running`, validate the selected task. A foreign pinned
task is requeued to its home and the current scheduler iteration continues.
This guard is a corruption containment boundary, not the normal routing path.

- [ ] **Step 7: Update the public contract**

Change the flag documentation from “Prefer keeping” to:

```c
/** Execute only on the task's logical home shard; same-shard helper threads are allowed. */
LLAM_SPAWN_F_PINNED = 1U << 0,
```

Document that it does not promise one stable pthread or CPU and that opaque
blocking can delay other pinned work on the same shard.

- [ ] **Step 8: Run affinity, stress, and sanitizer gates**

Run:

```bash
make -j4 test_runtime_shutdown_internal test_runtime_stress test_multi_runtime_core
./test_runtime_shutdown_internal
./test_runtime_stress
./test_multi_runtime_core
make -j4 test-asan
make -j4 test-tsan
```

Expected: all pinned escape-route receipts remain on their home shard.

- [ ] **Step 9: Commit**

```bash
git add include/llam/runtime.h src/internal/runtime_proto_core.h \
  src/core/sched/core_queue.c src/core/sched/reinject.c \
  src/core/sched/scheduler.c src/engine/scheduler/scheduler_engine.c \
  src/engine/watchdog/watchdog_rehome.c \
  src/engine/watchdog/watchdog_merge.c src/core/task/spawn.c \
  src/core/task/yield_join_sleep.c tests/test_hard_affinity_cases.inc \
  tests/test_runtime_shutdown_internal.c docs/operations.md
git commit -m "fix: enforce hard task affinity"
```

---

### Task 4: Add a portable external readiness doorbell

**Files:**
- Modify: `include/llam/runtime.h`
- Modify: `src/internal/runtime_types.h`
- Modify: `src/internal/runtime_proto_core.h`
- Create: `src/core/sched/external_doorbell.c`
- Modify: `src/core/lifecycle/init.c`
- Modify: `src/core/lifecycle/shutdown.c`
- Modify: `src/core/sched/wake.c`
- Modify: `src/core/base/abi.c`
- Modify: `config/llam-sources.json`
- Create: `tests/test_external_doorbell_cases.inc`
- Modify: `tests/test_runtime_shutdown_internal.c`
- Modify: `tests/test_windows_runtime_smoke.c`

**Interfaces:**
- Produces: `llam_runtime_readiness_kind_t`.
- Produces: caller-sized `llam_runtime_readiness_t`.
- Produces: internal doorbell init/destroy/signal/drain/rearm helpers.

- [ ] **Step 1: Write failing lifecycle and readiness tests**

Require:

- Linux returns a borrowed nonblocking CLOEXEC eventfd;
- Darwin/BSD returns the read side of a nonblocking CLOEXEC pipe;
- Windows returns a borrowed manual-reset `HANDLE`;
- adjacent signals coalesce;
- drain followed by a racing producer cannot lose readiness;
- destroy closes LLAM's native object exactly once.

These focused tests use private test-hook inspection of the runtime-owned
doorbell. The public readiness projection is wired in Task 5 after external
driver mode exists.

- [ ] **Step 2: Run focused tests and confirm RED**

Run:

```bash
make -j4 test_runtime_shutdown_internal
./test_runtime_shutdown_internal
```

Expected: compilation fails because readiness types and APIs do not exist.

- [ ] **Step 3: Define the caller-sized readiness projection**

Add:

```c
typedef enum llam_runtime_readiness_kind {
    LLAM_RUNTIME_READINESS_NONE = 0,
    LLAM_RUNTIME_READINESS_FD = 1,
    LLAM_RUNTIME_READINESS_WINDOWS_HANDLE = 2,
} llam_runtime_readiness_kind_t;

typedef struct llam_runtime_readiness {
    uint32_t kind;
    uint32_t reserved0;
    uintptr_t value;
} llam_runtime_readiness_t;

#define LLAM_RUNTIME_READINESS_CURRENT_SIZE \
    ((size_t)sizeof(llam_runtime_readiness_t))
```

`llam_abi_get_info` reports `runtime_readiness_size`.

- [ ] **Step 4: Implement the platform doorbell**

The runtime owns:

```c
typedef struct llam_external_doorbell {
    atomic_uint pending;
#if LLAM_PLATFORM_WINDOWS
    void *handle;
#else
    int read_fd;
    int write_fd;
#endif
} llam_external_doorbell_t;
```

Linux uses one eventfd for both directions. Darwin/BSD uses `pipe` plus
`fcntl(F_SETFL, O_NONBLOCK)` and `FD_CLOEXEC` on both ends. Windows uses
`CreateEventW(NULL, TRUE, FALSE, NULL)`.

- [ ] **Step 5: Make signaling coalesced and drain race-safe**

Signal only after changing `pending` from zero to one. Drain the native object,
exchange `pending` to zero, then re-check scheduler readiness. If work became
ready during the drain/reset window, claim and signal again.

EAGAIN from a full pipe/eventfd means readiness is already present and is not
a fatal runtime error.

- [ ] **Step 6: Connect every scheduler kick**

`llam_kick_shard` additionally signals the external doorbell when the target
runtime uses external mode. This covers host spawn, I/O completion, blocking
completion, cancellation, and runtime stop without changing internal-mode wake
behavior.

- [ ] **Step 7: Run platform-facing tests**

Run:

```bash
make -j4 test_runtime_shutdown_internal test_windows_runtime_smoke
./test_runtime_shutdown_internal
./test_windows_runtime_smoke
python3 scripts/audit_build_manifests.py
```

On a Windows CI runner, require the native value to work with
`WaitForSingleObject`.

- [ ] **Step 8: Commit**

```bash
git add include/llam/runtime.h src/internal/runtime_types.h \
  src/internal/runtime_proto_core.h src/core/sched/external_doorbell.c \
  src/core/lifecycle/init.c src/core/lifecycle/shutdown.c \
  src/core/sched/wake.c src/core/base/abi.c config/llam-sources.json \
  tests/test_external_doorbell_cases.inc \
  tests/test_runtime_shutdown_internal.c tests/test_windows_runtime_smoke.c
git commit -m "feat: add external runtime doorbell"
```

---

### Task 5: Add bounded external driving and deadline queries

**Files:**
- Modify: `include/llam/runtime.h`
- Modify: `src/core/lifecycle/resource_plan.c`
- Modify: `src/internal/runtime_resource_plan.h`
- Modify: `src/internal/runtime_types.h`
- Modify: `src/internal/runtime_proto_core.h`
- Modify: `src/core/lifecycle/init.c`
- Modify: `src/core/lifecycle/run.c`
- Create: `src/core/lifecycle/external_drive.c`
- Modify: `src/core/sched/scheduler.c`
- Modify: `src/core/sched/reinject.c`
- Modify: `src/core/task/yield_join_sleep.c`
- Modify: `src/core/wait/wait_tracking.c`
- Modify: `config/llam-sources.json`
- Create: `tests/test_external_drive_cases.inc`
- Modify: `tests/test_multi_runtime_core.c`
- Modify: `tests/test_runtime_shutdown_internal.c`
- Modify: `tests/test_abi_compat.c`
- Modify: `docs/operations.md`

**Interfaces:**
- Produces: `llam_runtime_driver_mode_t`.
- Produces: `llam_runtime_drive_result_t`.
- Produces: `int llam_runtime_drive_once(llam_runtime_t *, uint32_t *)`.
- Produces: `int llam_runtime_next_deadline(llam_runtime_t *, uint64_t *)`.
- Produces: `int llam_runtime_get_readiness(llam_runtime_t *, llam_runtime_readiness_t *, size_t)`.
- Produces: `int llam_runtime_wake(llam_runtime_t *)`.
- Produces: internal single-iteration scheduler quantum shared by full and external drivers.

- [ ] **Step 1: Write failing resource-plan tests**

External mode must resolve:

```text
worker_min = worker_count = worker_max = 1
dynamic workers = disabled
direct handoff = disabled
```

Reject explicit worker values other than zero or one, dynamic-worker flags,
and incompatible SQPOLL CPU reservation with `EINVAL`.

- [ ] **Step 2: Write failing behavior tests**

Require:

- `llam_runtime_run_handle` returns `ENOTSUP` for external mode;
- internal mode rejects external APIs with `ENOTSUP`;
- concurrent drive calls return `EBUSY`;
- managed or callback-reentrant drive returns `ENOTSUP`;
- one drive call executes at most one task segment;
- result is `PROGRESS`, `IDLE`, or `DONE` exactly;
- `DONE` runtime accepts a later spawn and can be driven again;
- `next_deadline` returns an absolute deadline or `UINT64_MAX`;
- a timer expiring between deadline query and drive is not lost;
- I/O and blocking completions make the readiness object observable;
- caller-sized readiness output leaves a future tail untouched.

- [ ] **Step 3: Run the focused tests and confirm RED**

Run:

```bash
make -j4 test_multi_runtime_core test_runtime_shutdown_internal test_abi_compat
./test_multi_runtime_core
./test_runtime_shutdown_internal
./test_abi_compat
```

Expected: compilation fails because external driver types and APIs do not
exist.

- [ ] **Step 4: Append the driver mode option**

Add:

```c
typedef enum llam_runtime_driver_mode {
    LLAM_RUNTIME_DRIVER_INTERNAL = 0,
    LLAM_RUNTIME_DRIVER_EXTERNAL = 1,
} llam_runtime_driver_mode_t;

typedef enum llam_runtime_drive_result {
    LLAM_RUNTIME_DRIVE_PROGRESS = 0,
    LLAM_RUNTIME_DRIVE_IDLE = 1,
    LLAM_RUNTIME_DRIVE_DONE = 2,
} llam_runtime_drive_result_t;
```

Append `uint32_t driver_mode` plus reserved padding after the callback pointer
tail created in Task 2. Resource planning resolves all external worker counts
to one or rejects incompatible explicit requests before allocation.

- [ ] **Step 5: Extract one scheduler quantum**

Move the body that drains inject work, processes due timers, selects one task,
switches once, and reclaims a dead task into:

```c
typedef enum llam_scheduler_quantum_result {
    LLAM_SCHEDULER_QUANTUM_PROGRESS,
    LLAM_SCHEDULER_QUANTUM_IDLE,
    LLAM_SCHEDULER_QUANTUM_DONE,
} llam_scheduler_quantum_result_t;

llam_scheduler_quantum_result_t
llam_scheduler_run_quantum(llam_shard_t *shard);
```

The ordinary loop repeatedly calls this helper and performs its existing idle
wait. External driving calls it once and never waits.

- [ ] **Step 6: Implement execution ownership**

`llam_runtime_drive_once`:

1. resolves and pins the runtime handle;
2. rejects managed/switch-hook context with `ENOTSUP`;
3. validates external mode;
4. CAS-claims `exec_started`, returning `EBUSY` on contention;
5. installs shard/scheduler TLS, affinity, native-thread accounting, and signal
   stack for this call;
6. drains the external doorbell and runs exactly one quantum;
7. clears TLS, restores host affinity/signal stack, rearms readiness, and only
   then releases `exec_started`.

No direct handoff policy may be enabled in external mode.

- [ ] **Step 7: Implement deadline, readiness, and explicit wake APIs**

All functions resolve a public runtime handle and validate external mode.
`next_deadline` locks shard 0 only long enough to copy the timer-heap root.
`get_readiness` writes only the overlapping caller prefix. `wake` signals the
dedicated doorbell from any unmanaged thread.

- [ ] **Step 8: Document host-loop pseudocode**

Add:

```c
for (;;) {
    uint32_t result;
    uint64_t deadline;
    llam_runtime_readiness_t ready;

    llam_runtime_drive_once(runtime, &result);
    if (result == LLAM_RUNTIME_DRIVE_DONE) {
        break;
    }
    llam_runtime_next_deadline(runtime, &deadline);
    llam_runtime_get_readiness(runtime, &ready, sizeof(ready));
    host_wait(ready, deadline);
}
```

State that the host borrows but never closes the readiness value, and that
task migration across successive host threads remains possible.

- [ ] **Step 9: Run focused, full, sanitizer, and platform gates**

Run:

```bash
make -j4 test_abi_contract test_abi_compat test_runtime_core \
  test_multi_runtime_core test_runtime_shutdown_internal
./test_abi_contract
./test_abi_compat
./test_runtime_core
./test_multi_runtime_core
./test_runtime_shutdown_internal
make -j4 test
make -j4 test-asan
make -j4 test-tsan
cmake -S . -B build-embedding -DLLAM_BUILD_RESEARCH=OFF
cmake --build build-embedding -j4
ctest --test-dir build-embedding --output-on-failure
```

Expected: all pass; external tests prove the one-segment bound and doorbell
race closure.

- [ ] **Step 10: Commit**

```bash
git add include/llam/runtime.h src/core/lifecycle/resource_plan.c \
  src/internal/runtime_resource_plan.h src/internal/runtime_types.h \
  src/internal/runtime_proto_core.h src/core/lifecycle/init.c \
  src/core/lifecycle/run.c src/core/lifecycle/external_drive.c \
  src/core/sched/scheduler.c src/core/sched/reinject.c \
  src/core/task/yield_join_sleep.c src/core/wait/wait_tracking.c \
  config/llam-sources.json tests/test_external_drive_cases.inc \
  tests/test_multi_runtime_core.c tests/test_runtime_shutdown_internal.c \
  tests/test_abi_compat.c docs/operations.md
git commit -m "feat: add bounded external driving"
```

---

### Task 6: Close the embedding-contract verification gate

**Files:**
- Modify: `tests/test_shared_load.c`
- Modify: `scripts/test_research_build_boundary.py`
- Modify: `docs/operations.md`
- Modify: `README.md`
- Modify: `config/c-structure-baseline.json` only when a justified split lowers or preserves every ratchet.

**Interfaces:**
- Consumes: all public APIs and invariants from Tasks 1–5.
- Produces: dynamic-load coverage, research-mode ABI parity, and final requirement evidence.

- [ ] **Step 1: Extend dynamic symbol loading coverage**

Resolve and call every new exported symbol from `test_shared_load.c`. Verify
the loaded library's metadata sizes before allocating caller-side structs.

- [ ] **Step 2: Extend research-mode ABI parity**

Require research-off and research-on builds to expose identical:

- installed `runtime.h`;
- ABI major and SONAME;
- task slot count and readiness struct size;
- dynamic `llam_*` export sets.

- [ ] **Step 3: Run repository-wide verification**

Run:

```bash
git diff --check
make -j4 audit-build-manifests audit-c-structure
make -j4 test
make -j4 test-asan
make -j4 test-tsan
python3 -m unittest scripts/test_research_build_boundary.py -v
python3 -m mkdocs build --strict
```

Also run the exact committed snapshot on the authorized Linux host and wait for
the GitHub Linux, macOS, Windows, BSD, stress, sanitizer, docs, and LEIR
research workflows for that same SHA.

- [ ] **Step 4: Review the completed diff**

Check:

- no public field was inserted into a frozen prefix;
- every task context pointer is cleared on allocator reuse;
- every real switch invokes hooks exactly once in the required order;
- no pinned task can be dispatched on a foreign logical shard;
- external driving cannot wait or chain task segments;
- doorbell drain/rearm has no reset-after-producer window;
- no test hook appears in production archives;
- no version, tag, or release mutation occurred.

- [ ] **Step 5: Commit final docs and gates**

```bash
git add tests/test_shared_load.c scripts/test_research_build_boundary.py \
  docs/operations.md README.md config/c-structure-baseline.json
git commit -m "test: close runtime embedding contract"
```

- [ ] **Step 6: Push without releasing**

Push `leir-native-segment` after all local gates pass. Keep PR #4 draft. CI
success does not override the current native performance `REJECT`, so do not
bump `2.2.0`, tag, or publish a release.
