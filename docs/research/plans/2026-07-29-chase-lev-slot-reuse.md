# Chase-Lev Slot-Reuse Safety Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent a delayed Chase-Lev thief from erasing a wrapped replacement task and lock the invariant in with a deterministic regression.

**Architecture:** Pause a thief through a test-only callback immediately after it advances `top`, then drive the single owner through one complete bounded-ring wrap. Production code retains the claimed slot's stale pointer outside `[top, bottom)` until the owner overwrites it, matching the Chase-Lev access protocol.

**Tech Stack:** C11 atomics, pthread-compatible mutexes/condition variables, Make, CMake/CTest, ASan/UBSan

## Global Constraints

- Limit production behavior changes to the opt-in `LOCKFREE_NORMQ` Chase-Lev path.
- Keep the test hook out of production objects with `LLAM_ENABLE_TEST_HOOKS`.
- Do not alter the public ABI or promote the experimental queue.
- Do not change the LEIR performance decision, version, tag, or release state.
- Preserve unrelated changes in the main checkout.

---

### Task 1: Reproduce and eliminate delayed-thief slot erasure

**Files:**
- Modify: `Makefile`
- Modify: `src/internal/runtime_proto_sched.h`
- Modify: `src/core/sched/norm_queue.c`
- Modify: `tests/test_runtime_shutdown_internal.c`
- Test: `test_runtime_shutdown_internal`

**Interfaces:**
- Consumes: `llam_cldeque_init`, `llam_norm_queue_push_owner_locked`, `llam_norm_queue_pop_owner_locked`, `llam_norm_queue_steal`, and `LLAM_NORM_QUEUE_CAP`.
- Produces: test-only `llam_cldeque_steal_claimed_hook_fn` with signature `void (*)(void *context)` and `llam_sched_test_set_cldeque_steal_claimed_hook(llam_cldeque_steal_claimed_hook_fn hook, void *context)`.

- [x] **Step 1: Declare the test-only claim hook**

Add this beside the normal queue prototypes in
`src/internal/runtime_proto_sched.h`:

```c
#if defined(LLAM_ENABLE_TEST_HOOKS)
typedef void (*llam_cldeque_steal_claimed_hook_fn)(void *context);
void llam_sched_test_set_cldeque_steal_claimed_hook(
    llam_cldeque_steal_claimed_hook_fn hook,
    void *context);
#endif
```

- [x] **Step 2: Add a post-CAS pause point without changing queue behavior**

In `src/core/sched/norm_queue.c`, add test-build-only hook storage, its setter,
and a helper that invokes the captured callback:

```c
#if defined(LLAM_ENABLE_TEST_HOOKS)
static llam_cldeque_steal_claimed_hook_fn
    g_cldeque_steal_claimed_hook;
static void *g_cldeque_steal_claimed_hook_context;

void llam_sched_test_set_cldeque_steal_claimed_hook(
    llam_cldeque_steal_claimed_hook_fn hook,
    void *context) {
    g_cldeque_steal_claimed_hook = hook;
    g_cldeque_steal_claimed_hook_context = context;
}

static void llam_cldeque_test_after_steal_claim(void) {
    llam_cldeque_steal_claimed_hook_fn hook =
        g_cldeque_steal_claimed_hook;

    if (hook != NULL) {
        hook(g_cldeque_steal_claimed_hook_context);
    }
}
#else
static void llam_cldeque_test_after_steal_claim(void) {
}
#endif
```

Call `llam_cldeque_test_after_steal_claim()` immediately after the successful
`top` compare-and-swap and before the existing slot clear.

Add `src/core/sched/norm_queue.c` to both
`TESTHOOK_RUNTIME_OVERRIDE_OBJS` and the corresponding `filter-out` list used
to construct `RUNTIME_TESTHOOK_OBJS`. Extend `audit-production-test-hooks` to
reject `llam_sched_test_set_cldeque_steal_claimed_hook` if it appears in
`libllam_runtime.a`.

- [x] **Step 3: Add the deterministic wraparound regression**

In `tests/test_runtime_shutdown_internal.c`, add a fixture containing a
`pthread_mutex_t`, `pthread_cond_t`, `hook_reached`, `release_hook`, the victim
shard pointer, and the thief's returned task. The callback must signal
`hook_reached` and wait for `release_hook`; the thief thread must call
`llam_norm_queue_steal`.

Add `exercise_cldeque_delayed_thief_preserves_wrapped_task()` beside the normal
queue depth test. It must:

```c
llam_task_t *tasks =
    calloc(LLAM_NORM_QUEUE_CAP + 1U, sizeof(*tasks));

runtime.experimental_lockfree_normq = 1U;
shard.runtime = &runtime;
atomic_init(&runtime.fatal_errno, 0);
atomic_init(&shard.norm_depth, 0U);
llam_cldeque_init(&shard.norm_cldeque);
```

Then push `&tasks[0]`, start the thief, wait for the hook, push
`&tasks[1]` through `&tasks[LLAM_NORM_QUEUE_CAP]`, release/join the thief, and
pop exactly `LLAM_NORM_QUEUE_CAP` tasks. The first pop must equal
`&tasks[LLAM_NORM_QUEUE_CAP]`; every subsequent pop must match descending
indices, and final `norm_depth` must be zero.

Every failure path after thread creation must release the hook and join the
thread before destroying the condition variable, mutex, and allocated tasks.
Clear the global hook after the join. Invoke the regression from `main`
immediately after `exercise_norm_depth_counter_wrap_is_rejected()`.

- [x] **Step 4: Run the focused test and prove the old behavior fails**

Run:

```bash
make -j4 test_runtime_shutdown_internal
./test_runtime_shutdown_internal
```

Expected: build succeeds and the test exits 1 with the wrapped replacement task
missing after the delayed thief resumes.

- [x] **Step 5: Remove the unsafe thief-side slot clear**

Delete the post-CAS `atomic_store_explicit(..., NULL, ...)` in
`llam_cldeque_steal_top`. Replace it with a comment explaining that advancing
`top` makes the slot reusable and therefore only the owner may overwrite it
before publishing a wrapped logical index.

Keep `llam_cldeque_test_after_steal_claim()` after the successful CAS so the
regression still forces the same scheduling window.

- [x] **Step 6: Run focused and repeated regression checks**

Run:

```bash
make -j4 test_runtime_shutdown_internal
./test_runtime_shutdown_internal
for run in 1 2 3 4 5; do ./test_runtime_shutdown_internal; done
```

Expected: all six executions exit 0 and print
`test_runtime_shutdown_internal ok`.

- [x] **Step 7: Verify production hook isolation and adjacent correctness**

Run:

```bash
make -j4 test
make audit-production-test-hooks
make -j4 test-asan
git diff --check
```

Expected: the full host suite, production-hook audit, sanitizer regression, and
diff check all exit 0.

- [x] **Step 8: Commit and push**

```bash
git add \
  docs/research/plans/2026-07-29-chase-lev-slot-reuse.md \
  Makefile \
  src/internal/runtime_proto_sched.h \
  src/core/sched/norm_queue.c \
  tests/test_runtime_shutdown_internal.c
git commit -m "fix: preserve wrapped Chase-Lev tasks"
git push origin leir-native-segment
```

Expected: the commit and push succeed on `leir-native-segment`, after which all
required CI workflows are monitored to completion.
