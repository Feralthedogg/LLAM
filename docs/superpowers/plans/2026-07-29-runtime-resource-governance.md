# Runtime Resource Governance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give embedders a prefix-safe way to bound scheduler capacity, runtime-owned threads, CPU placement, and runtime-total prewarm allocations before a runtime publishes resources.

**Architecture:** A pure `llam_runtime_resource_plan_resolve()` function converts a size-prefixed public option prefix plus discovered CPU capabilities into a complete fixed-size internal plan. Runtime initialization consumes that plan exactly once; blocking workers grow lazily inside configured bounds, and diagnostics report configured capacity separately from actual native threads and achieved prewarm totals.

**Tech Stack:** C11, pthreads/Windows threading compatibility layer, existing size-prefixed LLAM ABI, Make/CMake canonical source manifests, CTest and internal C regression suites.

## Global Constraints

- Preserve cancellation, generation, active-operation, owner-runtime, I/O retirement, and cross-runtime `EXDEV` invariants.
- Never read beyond a caller-supplied option or stats prefix.
- `llam_runtime_init()` consumes only the frozen 2.2 option prefix.
- Exact public requests fail before `initialized` is published and clean every partial allocation/thread.
- Support Linux, Darwin, Windows, and supported BSD targets without adding a dependency.
- Maximum scheduler capacity and selected CPU count are 256; maximum blocking workers are 256.
- Deterministic mode accepts only a `1/1/1` worker plan.
- Public task, stack, and timer prewarm requests are runtime totals.
- Research-off remains the default and installed stable/research ABI surfaces remain identical.

---

### Task 1: Append the public ABI and freeze the legacy prefix

**Files:**
- Modify: `include/llam/runtime.h`
- Modify: `src/core/base/abi.c`
- Modify: `src/core/lifecycle/init.c`
- Modify: `tests/test_abi_contract.c`
- Modify: `tests/test_runtime_core.c`

**Interfaces:**
- Produces: `llam_runtime_affinity_policy_t`
- Produces: `LLAM_RUNTIME_OPTS_V2_2_SIZE`
- Produces: appended `worker_min`, `worker_count`, `worker_max`, `blocking_min`, `blocking_max`, `affinity_policy`, `cpu_count`, `cpu_ids`, `task_prewarm_total`, `stack_prewarm_total`, and `timer_prewarm_total`
- Produces: appended resource-plan/thread/prewarm counters in `llam_runtime_stats_t`

- [x] **Step 1: Write ABI tests that name the compatibility breaks**

Add literal offset/width assertions and behavior tests proving:

```c
_Static_assert(LLAM_RUNTIME_OPTS_V2_2_SIZE ==
                   offsetof(llam_runtime_opts_t, preempt_quantum_ns) +
                       sizeof(((llam_runtime_opts_t *)0)->preempt_quantum_ns),
               "the legacy runtime option prefix must remain frozen");
_Static_assert(sizeof(((llam_runtime_opts_t *)0)->worker_count) == sizeof(uint32_t),
               "worker_count must be fixed width");
_Static_assert(sizeof(((llam_runtime_opts_t *)0)->task_prewarm_total) == sizeof(uint64_t),
               "task prewarm must be an aggregate fixed-width count");
```

In `tests/test_runtime_core.c`, pass a current struct containing an impossible
new worker request to `llam_runtime_init()` and prove the convenience wrapper
ignores the tail, then pass the same struct and current size to
`llam_runtime_init_ex()` and prove it fails with `EINVAL`.

- [x] **Step 2: Run the focused tests and verify RED**

Run:

```bash
cmake -S . -B object/resource-plan-red -DCMAKE_BUILD_TYPE=Debug
cmake --build object/resource-plan-red --target test_abi_contract test_runtime_core -j2
ctest --test-dir object/resource-plan-red --output-on-failure -R 'abi_contract|runtime_core'
```

Expected: compile failures for the missing enum/fields/macro, followed by the
new runtime behavior assertion failing once declarations exist without legacy
prefix handling.

- [x] **Step 3: Append the public declarations and initialize current defaults**

Use:

```c
typedef enum llam_runtime_affinity_policy {
    LLAM_RUNTIME_AFFINITY_NONE = 0,
    LLAM_RUNTIME_AFFINITY_PREFER = 1,
    LLAM_RUNTIME_AFFINITY_REQUIRE = 2,
} llam_runtime_affinity_policy_t;

#define LLAM_RUNTIME_OPTS_V2_2_SIZE \
    (offsetof(llam_runtime_opts_t, preempt_quantum_ns) + \
     sizeof(((llam_runtime_opts_t *)0)->preempt_quantum_ns))
```

Append fields only after `preempt_quantum_ns`. `llam_runtime_opts_init()` sets
the pointer/count and numeric resource requests to zero (automatic legacy
defaults) and affinity to `NONE`. Change `llam_runtime_init()` to pass
`LLAM_RUNTIME_OPTS_V2_2_SIZE`, while `llam_runtime_init_ex()` and
`llam_runtime_create()` continue honoring their explicit size.

- [x] **Step 4: Run the focused tests and verify GREEN**

Run the Task 1 command. Expected: both tests pass and older prefix fixtures
retain their tail bytes.

- [x] **Step 5: Commit**

```bash
git add include/llam/runtime.h src/core/base/abi.c src/core/lifecycle/init.c \
  tests/test_abi_contract.c tests/test_runtime_core.c
git commit -m "feat: expose runtime resource governance options"
```

### Task 2: Add the pure resource-plan resolver

**Files:**
- Create: `src/internal/runtime_resource_plan.h`
- Create: `src/core/lifecycle/resource_plan.c`
- Modify: `src/internal/runtime_internal.h`
- Modify: `config/llam-sources.json`
- Modify: `CMakeLists.txt`
- Modify: `Makefile`
- Modify: `tests/test_runtime_core.c`

**Interfaces:**
- Consumes: the public options and frozen prefix from Task 1
- Produces:

```c
#define LLAM_RUNTIME_MAX_WORKERS 256U
#define LLAM_RUNTIME_MAX_BLOCKING_WORKERS 256U

typedef struct llam_runtime_resource_plan_input {
    const llam_runtime_opts_t *opts;
    size_t opts_size;
    const unsigned *allowed_cpus;
    unsigned allowed_cpu_count;
    bool affinity_supported;
    bool sqpoll_supported;
} llam_runtime_resource_plan_input_t;

typedef struct llam_runtime_resource_plan {
    unsigned worker_min;
    unsigned worker_count;
    unsigned worker_max;
    unsigned blocking_min;
    unsigned blocking_max;
    unsigned affinity_policy;
    unsigned selected_cpu_count;
    unsigned selected_cpus[LLAM_RUNTIME_MAX_WORKERS];
    bool sqpoll_reserved;
    int sqpoll_cpu;
    uint64_t task_prewarm_total;
    uint64_t stack_prewarm_total;
    uint64_t timer_prewarm_total;
    uint64_t estimated_metadata_bytes;
    uint64_t estimated_stack_mapping_bytes;
} llam_runtime_resource_plan_t;

int llam_runtime_resource_plan_resolve(
    const llam_runtime_resource_plan_input_t *input,
    llam_runtime_resource_plan_t *out);
```

- [x] **Step 1: Write table-driven planner tests**

Use literal cases for 1, 8, and 64 CPUs; sparse caller order; explicit fixed
count; dynamic ranges; duplicate and disallowed CPUs; 257 CPUs; deterministic
conflicts; blocking bounds; SQPOLL reservation; stack total above 4096; and
checked arithmetic overflow. Each case asserts the exact plan or exact errno.

The break named by the test is accepting an invalid or ambiguous resource
graph before runtime allocation.

- [x] **Step 2: Run the planner tests and verify RED**

Run:

```bash
cmake --build object/resource-plan-red --target test_runtime_core -j2
ctest --test-dir object/resource-plan-red --output-on-failure -R runtime_core
```

Expected: compile failure because `runtime_resource_plan.h` and the resolver do
not exist.

- [x] **Step 3: Implement checked arithmetic and CPU validation**

Implement local helpers:

```c
static bool add_u64(uint64_t a, uint64_t b, uint64_t *out);
static bool mul_u64(uint64_t a, uint64_t b, uint64_t *out);
static bool cpu_is_allowed(unsigned cpu, const unsigned *allowed, unsigned count);
static bool cpu_is_duplicate(unsigned cpu, const unsigned *selected, unsigned count);
```

Resolve zero worker fields as legacy automatic defaults. A nonzero
`worker_count` with both bounds zero becomes fixed. A caller CPU list is copied
in caller order and must contain unique process-allowed IDs. Reserve an SQPOLL
CPU without allowing `worker_max` to exceed the remaining selected CPUs.

- [x] **Step 4: Register the source through the canonical build manifest**

Add `src/core/lifecycle/resource_plan.c` once to
`config/llam-sources.json::stable.common_sources`, then project it into Make and
CMake using the repository audit/update workflow rather than maintaining an
untracked source list.

- [x] **Step 5: Run planner and manifest tests and verify GREEN**

Run:

```bash
ctest --test-dir object/resource-plan-red --output-on-failure -R runtime_core
python3 scripts/audit_build_manifests.py --check
python3 scripts/test_audit_build_manifests.py
```

Expected: planner cases pass and source-set parity is clean.

- [x] **Step 6: Commit**

```bash
git add src/internal/runtime_resource_plan.h src/internal/runtime_internal.h \
  src/core/lifecycle/resource_plan.c config/llam-sources.json CMakeLists.txt \
  Makefile tests/test_runtime_core.c
git commit -m "feat: resolve immutable runtime resource plans"
```

### Task 3: Consume the plan before runtime resource publication

**Files:**
- Modify: `src/core/lifecycle/init.c`
- Modify: `src/core/lifecycle/run.c`
- Modify: `src/core/lifecycle/shutdown.c`
- Modify: `src/internal/runtime_types.h`
- Modify: `tests/test_runtime_core.c`
- Modify: `tests/test_multi_runtime_core.c`

**Interfaces:**
- Consumes: `llam_runtime_resource_plan_t`
- Produces: runtime-owned copies of the resolved bounds, selected CPUs, and plan estimates
- Preserves: `active_shards` as configured capacity, `online_shards` as current online target

- [x] **Step 1: Write initialization integration tests**

Create runtimes with fixed counts 1 and 2 on machines exposing at least two
CPUs, and pure injected planner cases for 8 and 64. Assert:

```c
stats.configured_worker_min == requested_min;
stats.active_workers == requested_max;
stats.online_workers == requested_count;
stats.selected_cpu_count == requested_max;
```

Add a failure case proving invalid exact resource options leave no registered
runtime, threads, descriptors, or changed default-runtime state.

- [x] **Step 2: Run and verify RED**

Run the focused runtime suites. Expected: current initialization still derives
`active_shards` from all allowed CPUs and ignores the new plan.

- [x] **Step 3: Normalize every appended field behind prefix guards**

Extend the local inbound option copy in `llam_runtime_init_impl()` using
`LLAM_RUNTIME_OPTS_PREFIX_HAS_FIELD`. Never dereference `cpu_ids` unless both
pointer and `cpu_count` fields are fully present.

- [x] **Step 4: Resolve and install the plan**

After CPU discovery and before shard/node/thread allocation:

```c
if (llam_runtime_resource_plan_resolve(&plan_input, &plan) != 0) {
    int saved_errno = errno;
    free(discovered_cpus);
    return llam_runtime_init_fail_registered(rt, saved_errno);
}
```

Copy only the resolved selected CPU array into `rt->allowed_cpus`, set capacity
to `worker_max`, initial online target to `worker_count`, and dynamic floor to
`worker_min`. Remove the independent SQPOLL CPU mutation so one plan owns the
decision.

- [x] **Step 5: Make run startup honor configured capacity safely**

Start scheduler pthreads for the configured capacity and retain online/offline
state inside the existing dynamic-worker protocol. When the range is fixed,
all configured workers are online. A failure on secondary worker N joins only
confirmed starts and restores the run token.

- [x] **Step 6: Run runtime/shutdown tests and verify GREEN**

Run:

```bash
cmake --build object/resource-plan-red --target test_runtime_core \
  test_multi_runtime_core test_runtime_shutdown_internal -j2
ctest --test-dir object/resource-plan-red --output-on-failure \
  -R 'runtime_core|multi_runtime_core|runtime_shutdown_internal'
```

- [x] **Step 7: Commit**

```bash
git add src/core/lifecycle/init.c src/core/lifecycle/run.c \
  src/core/lifecycle/shutdown.c src/internal/runtime_types.h \
  tests/test_runtime_core.c tests/test_multi_runtime_core.c
git commit -m "feat: apply runtime worker and CPU plans"
```

### Task 4: Convert prewarm controls to runtime-total authority

**Files:**
- Modify: `Makefile`
- Modify: `include/llam/runtime.h`
- Modify: `src/core/debug/debug.c`
- Modify: `src/core/lifecycle/resource_plan.c`
- Modify: `src/core/memory/alloc.c`
- Modify: `src/core/task/task_stack.c`
- Modify: `src/core/lifecycle/init.c`
- Modify: `src/internal/runtime_proto_core.h`
- Modify: `src/internal/runtime_resource_plan.h`
- Modify: `src/internal/runtime_types.h`
- Modify: `tests/test_abi_contract.c`
- Modify: `tests/test_runtime_core.c`
- Modify: `tests/test_runtime_shutdown_internal.c`
- Modify: `docs/reference/environment.md`
- Modify: `docs/reference/options.md`
- Modify: `docs/operations.md`
- Modify: `docs/guides/performance-tuning.md`

**Interfaces:**
- Consumes: exact total targets from the resource plan
- Produces:

```c
int llam_runtime_prewarm_task_allocators(
    llam_runtime_t *rt, uint64_t total, bool exact, uint64_t *achieved);
int llam_runtime_prewarm_stack_cache(
    llam_runtime_t *rt, uint64_t total, bool exact, uint64_t *achieved);
int llam_runtime_prewarm_timer_heaps(
    llam_runtime_t *rt, uint64_t total, bool exact, uint64_t *achieved);
```

- [x] **Step 1: Write aggregate-distribution and exact-failure tests**

For 1, 8, and synthetic 64-shard runtimes, assert quotient/remainder
distribution sums exactly to the requested task/stack/timer total. Add injected
allocation failure after N objects and assert exact requests fail and unwind,
while legacy environment inputs remain best-effort and are reported as legacy.

- [x] **Step 2: Run and verify RED**

Expected: task/timer prewarm multiplies the environment count by shard count
and exact public totals are ignored.

- [x] **Step 3: Implement a shared total distributor**

Use:

```c
static uint64_t share_for_index(uint64_t total, unsigned count, unsigned index) {
    return total / count + (index < total % count ? 1U : 0U);
}
```

Reject totals that exceed the documented ceiling before allocation. Return
achieved totals explicitly and unwind exact partial allocations through each
allocator's existing destroy/release path.

- [x] **Step 4: Add `_TOTAL` environment compatibility names**

Current names preserve historical per-shard/best-effort meaning. New
`LLAM_TASK_CACHE_PREWARM_TOTAL`, `LLAM_STACK_CACHE_PREWARM_TOTAL`, and
`LLAM_TIMER_HEAP_PREWARM_TOTAL` are runtime totals; an explicit public field
wins over either environment family.

- [x] **Step 5: Run focused tests and verify GREEN**

Run runtime core, init-failure, shutdown, manifest, and documentation link
checks.

- [x] **Step 6: Commit**

```bash
git add src/core/memory/alloc.c src/core/task/task_stack.c \
  src/core/lifecycle/init.c src/internal/runtime_proto_core.h \
  src/internal/runtime_types.h tests/test_runtime_core.c \
  docs/reference/environment.md docs/operations.md \
  docs/guides/performance-tuning.md
git commit -m "feat: govern runtime-total prewarm allocations"
```

### Task 5: Grow the blocking pool lazily within exact bounds

**Files:**
- Modify: `src/internal/runtime_types.h`
- Modify: `src/internal/runtime_proto_sched.h`
- Modify: `src/core/lifecycle/init.c`
- Modify: `src/core/api/blocking_api.c`
- Modify: `src/engine/scheduler/block.c`
- Modify: `src/core/lifecycle/shutdown.c`
- Modify: `src/core/sched/core_queue.c`
- Modify: `tests/test_runtime_core.c`
- Modify: `tests/test_runtime_shutdown_internal.c`

**Interfaces:**
- Produces:

```c
int llam_block_pool_start_min(llam_runtime_t *rt);
int llam_block_pool_ensure_capacity_locked(llam_runtime_t *rt,
                                           unsigned pending_after_enqueue);
```

- [x] **Step 1: Write blocking lifecycle tests**

Cover `min=0,max=2`: zero threads after init, one confirmed worker before the
first managed submission may park, growth to two under two concurrently held
callbacks, no growth past two, and exact started/entered/exited counters.
Inject failure on the first and second create and assert the job remains owned
by the caller and no task parks forever.

- [x] **Step 2: Run and verify RED**

Expected: current init eagerly creates the full platform default and exposes no
zero-min path.

- [x] **Step 3: Allocate capacity but start only the minimum**

Allocate `block_threads` for `blocking_max`; set configured min/max separately
from confirmed starts. `llam_block_pool_start_min()` creates exactly the
minimum and uses the existing confirmed-start cleanup rule.

- [x] **Step 4: Grow before publishing a job that needs another worker**

While holding `block_lock`, compare queued pressure with confirmed/idle
workers. Create at most one worker per enqueue and update the confirmed slot
only after `pthread_create` succeeds. If no worker exists and creation fails,
roll back pending accounting and return the unqueued job through the existing
failure path.

- [x] **Step 5: Count worker entry and exit**

Increment/decrement atomic actual-thread counters in
`llam_block_worker_main()`. Shutdown wakes and joins exactly confirmed slots,
including partially grown pools.

- [x] **Step 6: Run blocking and shutdown suites and verify GREEN**

Run:

```bash
cmake --build object/resource-plan-red --target test_runtime_core \
  test_runtime_shutdown_internal -j2
ctest --test-dir object/resource-plan-red --output-on-failure \
  -R 'runtime_core|runtime_shutdown_internal'
```

- [x] **Step 7: Commit**

```bash
git add src/internal/runtime_types.h src/internal/runtime_proto_sched.h \
  src/core/lifecycle/init.c src/core/api/blocking_api.c \
  src/engine/scheduler/block.c src/core/lifecycle/shutdown.c \
  src/core/sched/core_queue.c tests/test_runtime_core.c \
  tests/test_runtime_shutdown_internal.c
git commit -m "feat: grow blocking workers within configured bounds"
```

### Task 6: Enforce affinity policy and truthful thread diagnostics

**Files:**
- Modify: `src/core/platform/platform.c`
- Modify: `src/core/lifecycle/run.c`
- Modify: `src/core/sched/scheduler.c`
- Modify: `src/io/engine/io_engine.c`
- Modify: `src/engine/watchdog/watchdog.c`
- Modify: `src/core/debug/debug.c`
- Modify: `src/core/debug/debug_stats_json.c`
- Modify: `src/internal/runtime_types.h`
- Modify: `src/internal/runtime_proto_core.h`
- Modify: `tests/test_runtime_core.c`
- Modify: `tests/test_multi_runtime_core.c`

**Interfaces:**
- Produces:

```c
int llam_runtime_capture_driver_affinity(llam_runtime_t *rt);
int llam_runtime_apply_worker_affinity(llam_runtime_t *rt, unsigned cpu_id);
int llam_runtime_restore_driver_affinity(llam_runtime_t *rt);
```

- [x] **Step 1: Write preferred/required affinity tests**

Use internal seams to inject unsupported, apply-failure, and restore-failure
results. Assert `NONE` makes no platform call, `PREFER` continues and increments
the appropriate counter, and `REQUIRE` fails init/run with the exact errno.
Assert shard 0 restores the host driver's original affinity on normal drain,
worker-start failure, fatal worker error, and cooperative stop.

- [x] **Step 2: Write native-thread counter tests**

While a runtime is initialized and while it is running, assert configured
capacity separately from scheduler loops, blocking, I/O, controller,
opaque-helper, total runtime-owned, and host-inclusive execution threads. After
shutdown all actual runtime-owned counters must return to zero.

- [x] **Step 3: Run and verify RED**

Expected: affinity helpers ignore failures, restoration occurs only during
shutdown, and stats lack the new counters.

- [x] **Step 4: Implement policy-aware affinity and entry/exit accounting**

No platform call is made under a scheduler queue lock. Each OS thread main
publishes entry after TLS/runtime ownership is installed and exit before the
last runtime pointer can be released. Use saturating diagnostics on impossible
underflow and record a fatal invariant error in debug builds.

- [x] **Step 5: Run focused and JSON/text diagnostics tests and verify GREEN**

Run runtime core, multi-runtime, API edge, debug JSON, and shutdown suites.

- [x] **Step 6: Commit**

```bash
git add src/core/platform/platform.c src/core/lifecycle/run.c \
  src/core/sched/scheduler.c src/io/engine/io_engine.c \
  src/engine/watchdog/watchdog.c src/core/debug/debug.c \
  src/core/debug/debug_stats_json.c src/internal/runtime_types.h \
  src/internal/runtime_proto_core.h tests/test_runtime_core.c \
  tests/test_multi_runtime_core.c
git commit -m "feat: enforce affinity and native thread diagnostics"
```

### Task 7: Document, export-audit, and verify the resource contract

**Files:**
- Modify: `docs/reference/api.md`
- Modify: `docs/reference/environment.md`
- Modify: `docs/operations.md`
- Modify: `docs/guides/performance-tuning.md`
- Modify: `tests/test_abi_contract.c`
- Modify: `scripts/test_installed_contract.py`
- Modify: `config/c-structure-baseline.json` only through the ratchet workflow

**Interfaces:**
- Consumes: all public fields and diagnostics from Tasks 1-6
- Produces: FFI-facing ownership/default/error documentation and installed consumer coverage

- [ ] **Step 1: Add installed C and CMake consumer cases**

Compile consumers that use a short 2.2 option prefix and a current prefix with
fixed workers, sparse CPU IDs, zero-min blocking pool, and total prewarm.
Assert stable and research installations expose identical declarations,
symbols, ABI major, SONAME/install name, pkg-config, and imported target
metadata.

- [ ] **Step 2: Run and verify RED**

Expected: documentation/consumer checks identify missing resource fields and
old per-shard prewarm wording until the contract is updated.

- [ ] **Step 3: Document exact defaults and ownership**

State CPU-ID pointer copy timing, affinity error behavior, blocking lazy growth,
legacy versus `_TOTAL` environment semantics, exact public failure, diagnostic
meaning, and the 256/4096 hard caps.

- [ ] **Step 4: Run installed-contract, ABI, export, manifest, and structure checks**

Run:

```bash
python3 scripts/test_installed_contract.py
python3 scripts/audit_build_manifests.py --check
python3 scripts/audit_c_structure.py --strict
cmake --build object/resource-plan-red --target test_abi_contract -j2
ctest --test-dir object/resource-plan-red --output-on-failure -R abi_contract
```

- [ ] **Step 5: Commit**

```bash
git add docs/reference/api.md docs/reference/environment.md docs/operations.md \
  docs/guides/performance-tuning.md tests/test_abi_contract.c \
  scripts/test_installed_contract.py config/c-structure-baseline.json
git commit -m "docs: define runtime resource governance contract"
```

### Task 8: Resource-stage verification gate

**Files:**
- Modify: `docs/superpowers/plans/2026-07-29-runtime-resource-governance.md`

**Interfaces:**
- Consumes: every deliverable in Tasks 1-7
- Produces: checked completion boxes backed by fresh command output

- [ ] **Step 1: Run the complete stable and research build gates**

Run:

```bash
make clean
make -j2 all test
make clean
make -j2 LLAM_BUILD_RESEARCH=1 research-test
cmake -S . -B object/resource-stage-stable -DCMAKE_BUILD_TYPE=Debug -DLLAM_BUILD_RESEARCH=OFF
cmake --build object/resource-stage-stable -j2
ctest --test-dir object/resource-stage-stable --output-on-failure
cmake -S . -B object/resource-stage-research -DCMAKE_BUILD_TYPE=Debug -DLLAM_BUILD_RESEARCH=ON
cmake --build object/resource-stage-research -j2
ctest --test-dir object/resource-stage-research --output-on-failure
```

- [ ] **Step 2: Run sanitizer, manifest, supply-chain, structure, and whitespace gates**

Run the repository ASan/UBSan and TSan targets, build-manifest audit,
installed-contract parity, dependency policy, strict C-structure audit,
`actionlint`, `git diff --check`, and the full Python governance suite.

- [ ] **Step 3: Re-read the resource design requirement by requirement**

For every option, invariant, failure mode, diagnostic counter, platform, and
test seam in
`docs/superpowers/specs/2026-07-29-runtime-resource-governance-design.md`,
record the source/test command that proves it. Any missing or indirect evidence
keeps the task unchecked.

- [ ] **Step 4: Mark only evidenced boxes complete and commit the gate record**

```bash
git add docs/superpowers/plans/2026-07-29-runtime-resource-governance.md
git commit -m "test: close runtime resource governance gate"
```
