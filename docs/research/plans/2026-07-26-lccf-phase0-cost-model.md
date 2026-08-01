# LCCF Phase 0 Paired Cost Model Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and run a standalone, allocation-free C11 experiment that decides whether LLAM Causal Completion Fusion can remove enough wake-publication and scheduler-reentry cost to justify a production prototype.

**Architecture:** The experiment gives a conventional waker queue and every LCCF candidate the same immutable completion tickets, frames, event formulas, resume-site callbacks, and command validation. Local queued, local fused, bounded-chain, and real producer-thread remote paths reduce to the same canonical state, while a strict Python runner executes baseline and candidate in one process with alternating `ABBA`/`BAAB` blocks and classifies only process-local ratios.

**Tech Stack:** C11 atomics, pthreads on POSIX, Win32 threads and condition variables on Windows, project GCC/Clang/MSVC warning policy, CMake 3.20+, GNU Make compatibility, Python 3 standard library, ASan/UBSan and TSan where supported.

## Global Constraints

- Follow `docs/research/specs/2026-07-26-llam-causal-completion-fusion-design.md`.
- Keep this phase standalone: do not modify any file under `src/` or `include/llam/`, do not install an Executor ABI, and do not change the existing stackful runtime.
- Preserve the user's root-worktree changes in `docs/operations/benchmarks.md` and `scripts/bench_deep_compare.py`; never stage either file.
- Add no third-party dependency and use only C11, platform system APIs, and Python standard-library or existing repository helpers.
- Allocate every frame, causal cell, waker, ticket, queue slot, producer team, timing histogram, and scratch buffer before measured rounds; every mode must report `hot_allocations=0`.
- Test frame footprints `64`, `128`, and `256` bytes, causal-cell footprints `64`, `96`, and `128` bytes, and site distributions `1` and `8`.
- Run the functional workloads `completion_io_pipeline`, `completion_rpc_state`, and `completion_timer_cancel`, plus `completion_mixed_fairness`.
- Compare six exact modes: `waker_queue`, `causal_cell_queue`, `fused_causal_cell`, `budgeted_fused_chain`, `remote_waker_queue`, and `remote_causal_cell`.
- Give the conventional and LCCF modes the same generation claim and callback work. Only wake-object lookup, queue representation, dispatch, and direct continuation threading may differ.
- Use a persistent two-producer remote team for both remote modes. Producer creation, teardown, and idle waiting are outside measured blocks; process CPU time includes active producer CPU.
- Verify field-by-field canonical equality and a stable checksum after every measured block. A mismatch stops that process with nonzero status.
- Use the same calibrated round count for the two modes in a pair. Each mode accumulates at least `250 ms` of measured time in every full-run process.
- Use nine fresh processes per matrix cell. Odd samples run `ABBA`; even samples run `BAAB`.
- Calculate wall speedup and CPU ratio inside each process before aggregation. Paired spread is `maximum / minimum` across the nine process-local ratios.
- Retain every raw sample. Never silently remove an outlier.
- On Linux, pin the owner to the lowest available CPU and the two remote producers to other available CPUs when possible. Record affinity, process CPU source, compiler, architecture, OS, and active processor count. On macOS, also record the current pthread QoS class and `hw.activecpu` without changing either.
- A `PROMISING` verdict requires one causal-cell footprint to satisfy all of these:
  - each functional workload has worst-case wall speedup `>= 1.25x` and worst-case candidate CPU ratio `<= 0.80x` across all frame footprints and both site distributions;
  - at least two functional workloads have worst-case wall speedup `>= 1.50x`;
  - `causal_cell_queue` has worst-case throughput `>= 0.95x` the local waker baseline;
  - `remote_causal_cell` has worst-case throughput `>= 0.95x` the remote waker baseline;
  - `budgeted_fused_chain` mixed-fairness p99 degradation is `<= 10%`;
  - every correctness and forced-escape check passes;
  - every gate-driving wall-speedup and CPU-ratio spread is `<= 1.10x`.
- A `NARROW` verdict requires at least one explicitly named functional workload to pass its `1.25x` wall and `0.80x` CPU gates across every frame and site dimension on one cell footprint, with the queue, remote, fairness, correctness, and spread controls still passing.
- Use `REJECT` for a reproducible cost-model failure and `INCONCLUSIVE` only for missing data, insufficient measured duration, checksum or identity mismatch, nonzero hot allocation, unsupported required execution, or spread above `1.10x`.
- Do not describe Phase 0 as production validation. A passing result authorizes only the internal synthetic-completion phase.

---

## File Map

### New C Experiment Files

- `experiments/lccf/lccf_model.h`
  - Test/benchmark interface: configuration, modes, workloads, metrics, batch lifecycle, equality, checksum, and name parsing.
- `experiments/lccf/lccf_model_internal.h`
  - Private frame core, baseline waker, causal-cell hot fields, immutable tickets, queue slots, remote team, callback table, and batch storage.
- `experiments/lccf/lccf_platform.h`
  - Opaque thread/team synchronization plus monotonic/process-CPU clocks, CPU affinity, and host-description interfaces.
- `experiments/lccf/lccf_platform.c`
  - POSIX and Win32 implementations; no production-runtime dependency.
- `experiments/lccf/lccf_model.c`
  - Validation, deterministic initialization/reset, generation claims, queue algorithms, six execution modes, command validation, canonical equality, checksum, and metrics.
- `experiments/lccf/lccf_workloads.c`
  - Four fixed scalar resume-site formulas and deterministic event/command derivation.
- `experiments/lccf/test_lccf_model.c`
  - Parser, layout, lifecycle, stale-ticket, cancellation-race, differential, exact-budget, remote, fairness, and allocation tests.
- `experiments/lccf/bench_lccf_model.c`
  - Strict CLI, pair calibration, alternating paired blocks, timing, equality checks, and one machine-readable result row.

### New Python Files

- `scripts/bench_lccf_model.py`
  - Strict row parser, full/quick matrices, fresh-process runner, paired summaries, integrity gates, layout selection, CSV/JSON/Markdown evidence, and verdict.
- `scripts/test_bench_lccf_model.py`
  - Parser rejection, command construction, median-ratio, spread, layout gate, `NARROW`, `REJECT`, `INCONCLUSIVE`, and report tests.

### Build Files

- `CMakeLists.txt`
  - Add standalone `test_lccf_model` and `bench_lccf_model` targets and CTest coverage.
- `Makefile`
  - Add objects, targets, clean entries, Windows delegation, `test-lccf-model`, and `lccf-model-report`.
- `.gitignore`
  - Ignore only root-level `/test_lccf_model` and `/bench_lccf_model`; evidence already falls under `object/`.

### Evidence

- `object/lccf-phase0/lccf_phase0_samples.csv`
- `object/lccf-phase0/lccf_phase0_summary.csv`
- `object/lccf-phase0/lccf_phase0_metadata.json`
- `object/lccf-phase0/lccf_phase0_report.md`
- `docs/research/reports/2026-07-26-lccf-phase0-results.md`

The `object/` files remain untracked. The tracked report is generated from the
same in-memory rows and records the exact command, full matrix, selected or
rejected layout, raw-evidence paths, host/compiler metadata, verdict reasons,
and production limitations.

---

### Task 1: Canonical Frames, Workloads, And Conventional Waker Baseline

**Files:**

- Create: `experiments/lccf/lccf_model.h`
- Create: `experiments/lccf/lccf_model_internal.h`
- Create: `experiments/lccf/lccf_model.c`
- Create: `experiments/lccf/lccf_workloads.c`
- Create: `experiments/lccf/test_lccf_model.c`

**Interfaces:**

- Produces:

```c
#define LCCF_MODEL_MAX_SITES 8U
#define LCCF_MODEL_MIN_FRAME_BYTES 64U
#define LCCF_MODEL_MAX_FRAME_BYTES 256U
#define LCCF_MODEL_MIN_CELL_BYTES 64U
#define LCCF_MODEL_MAX_CELL_BYTES 128U

typedef enum lccf_model_mode {
    LCCF_MODEL_WAKER_QUEUE = 0,
    LCCF_MODEL_CAUSAL_CELL_QUEUE = 1,
    LCCF_MODEL_FUSED_CAUSAL_CELL = 2,
    LCCF_MODEL_BUDGETED_FUSED_CHAIN = 3,
    LCCF_MODEL_REMOTE_WAKER_QUEUE = 4,
    LCCF_MODEL_REMOTE_CAUSAL_CELL = 5,
} lccf_model_mode_t;

typedef enum lccf_model_workload {
    LCCF_MODEL_COMPLETION_IO_PIPELINE = 0,
    LCCF_MODEL_COMPLETION_RPC_STATE = 1,
    LCCF_MODEL_COMPLETION_TIMER_CANCEL = 2,
    LCCF_MODEL_COMPLETION_MIXED_FAIRNESS = 3,
} lccf_model_workload_t;

typedef enum lccf_model_command_kind {
    LCCF_MODEL_COMMAND_CONTINUE = 1,
    LCCF_MODEL_COMMAND_WAIT_IO = 2,
    LCCF_MODEL_COMMAND_WAIT_TIMER = 3,
    LCCF_MODEL_COMMAND_YIELD = 4,
    LCCF_MODEL_COMMAND_COMPLETE = 5,
    LCCF_MODEL_COMMAND_FAIL = 6,
} lccf_model_command_kind_t;

typedef struct lccf_model_config {
    lccf_model_workload_t workload;
    lccf_model_mode_t mode;
    size_t instance_count;
    size_t frame_bytes;
    size_t cell_bytes;
    unsigned site_count;
    unsigned direct_budget;
    unsigned chain_length;
    unsigned remote_producers;
    uint64_t seed;
} lccf_model_config_t;

typedef struct lccf_model_metrics {
    uint64_t completions;
    uint64_t claims;
    uint64_t stale_tickets;
    uint64_t queue_pushes;
    uint64_t queue_pops;
    uint64_t resume_calls;
    uint64_t direct_calls;
    uint64_t forced_escapes;
    uint64_t remote_pushes;
    uint64_t fairness_samples;
    uint64_t fairness_p99_ns;
    uint64_t hot_allocations;
} lccf_model_metrics_t;

typedef struct lccf_model_batch lccf_model_batch_t;

int lccf_model_batch_create(const lccf_model_config_t *config,
                            lccf_model_batch_t **out_batch);
void lccf_model_batch_destroy(lccf_model_batch_t *batch);
int lccf_model_batch_reset(lccf_model_batch_t *batch);
int lccf_model_run_round(lccf_model_batch_t *batch,
                         lccf_model_metrics_t *metrics);
bool lccf_model_batch_equal(const lccf_model_batch_t *lhs,
                            const lccf_model_batch_t *rhs);
uint64_t lccf_model_checksum(const lccf_model_batch_t *batch);
const char *lccf_model_mode_name(lccf_model_mode_t mode);
const char *lccf_model_workload_name(lccf_model_workload_t workload);
int lccf_model_parse_mode(const char *text, lccf_model_mode_t *out);
int lccf_model_parse_workload(const char *text,
                              lccf_model_workload_t *out);
```

- The frame allocation uses `frame_bytes` as its stride. Its first 64 bytes
  are:

```c
typedef struct lccf_model_frame_core {
    uint64_t state0;
    uint64_t state1;
    uint64_t state2;
    uint64_t output;
    uint64_t generation;
    uint64_t command_word;
    uint32_t site;
    uint32_t steps;
    uint32_t terminal;
    uint32_t reserved;
} lccf_model_frame_core_t;
```

- The conventional baseline owns one `lccf_model_waker_t` per instance. A
  completion claims its packed `(generation,state)` word, publishes the waker
  pointer to a bounded local ring, the owner dequeues it, loads
  `waker->instance`, loads the resume-site callback from the module table, and
  validates the returned command. A `CONTINUE` command republishes the waker,
  so the baseline pays one scheduler-queue cycle per compiler segment.
- Task 1 accepts only `LCCF_MODEL_WAKER_QUEUE`; all other valid modes return
  `ENOTSUP` until their tasks implement them.
- `lccf_model_batch_reset()` restores the exact seed-derived initial state
  without freeing or allocating storage.
- Workloads use unsigned arithmetic with defined wraparound. Every resume
  formula consumes `frame`, `event`, and `site`, mutates at least two frame
  words, and returns one command:
  - I/O pipeline alternates `WAIT_IO` read/write commands.
  - RPC state branches across success, retry, route, and failure-shaped input.
  - Timer/cancel consumes deterministic completion, timeout, and cancel event
    permutations.
  - Mixed fairness uses the I/O formula and marks every 32nd admission as a
    due timer or stackful placeholder.

- [ ] **Step 1: Write the failing public-contract and baseline tests**

Add tests that assert:

```c
static const size_t FRAME_BYTES[] = {64U, 128U, 256U};
static const size_t CELL_BYTES[] = {64U, 96U, 128U};

/* Exact accepted names round-trip; unknown names and null outputs are EINVAL. */
/* Counts zero, sites outside {1,8}, unsupported footprints, budget zero,
   chain_length outside 1..1024, remote_producers above 2, and null output
   are EINVAL. */
/* Reset restores the initial checksum after 19 baseline rounds. */
/* Baseline metrics: claims == resume_calls == instances * rounds,
   queue_pushes == queue_pops, direct_calls == 0, hot_allocations == 0. */
```

The test creates `37` and `257` instances for every functional workload,
frame footprint, and site distribution so partial queue wraps are covered.

- [ ] **Step 2: Compile to verify the tests fail**

Run:

```sh
mkdir -p object/lccf-tdd
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g \
  -Iexperiments/lccf \
  experiments/lccf/lccf_model.c \
  experiments/lccf/lccf_workloads.c \
  experiments/lccf/test_lccf_model.c \
  -pthread -o object/lccf-tdd/test_lccf_model
```

Expected: compile or link failure because the public model functions do not
exist yet.

- [ ] **Step 3: Implement the canonical storage and waker queue**

Use one power-of-two queue capacity strictly greater than `instance_count`.
The local ring is owner-only in Task 1. Pack state in the low three bits and
generation in the remaining bits:

```c
static uint64_t pack_state(uint64_t generation, unsigned state) {
    return (generation << 3U) | (uint64_t)state;
}

static uint64_t unpack_generation(uint64_t word) {
    return word >> 3U;
}
```

Use `memory_order_acq_rel` for the winning generation claim and
`memory_order_acquire` for an observed loss. Refuse generation overflow before
mutating canonical state.

- [ ] **Step 4: Run the baseline tests**

Run:

```sh
object/lccf-tdd/test_lccf_model
```

Expected: `[test_lccf_model] all checks passed`.

- [ ] **Step 5: Commit the baseline**

```sh
git add experiments/lccf
git commit -m "test: add conventional completion baseline"
```

---

### Task 2: Causal Cell, Immutable Tickets, And Queued Differential Path

**Files:**

- Modify: `experiments/lccf/lccf_model_internal.h`
- Modify: `experiments/lccf/lccf_model.c`
- Modify: `experiments/lccf/test_lccf_model.c`

**Interfaces:**

- Consumes the batch, frame, event, command, callback, reset, equality, and
  checksum contracts from Task 1.
- Produces `LCCF_MODEL_CAUSAL_CELL_QUEUE`.
- The first 64 bytes of each `cell_bytes` stride are:

```c
typedef struct lccf_model_cell_hot {
    _Atomic uint64_t state_generation;
    struct lccf_model_instance *instance;
    uint64_t event_word0;
    uint64_t event_word1;
    uint64_t command_word;
    uint32_t home_shard;
    uint32_t next_site;
    uint32_t event_kind;
    uint32_t queue_owned;
    uint32_t backend_refs;
    uint32_t reserved;
} lccf_model_cell_hot_t;
```

Add:

```c
_Static_assert(sizeof(lccf_model_cell_hot_t) <= 64U,
               "LCCF causal hot fields exceed one cache line");
```

- Every instance owns three immutable tickets:

```c
typedef struct lccf_model_ticket {
    void *target;
    uint64_t generation;
    uint64_t event_word0;
    uint64_t event_word1;
    uint32_t instance_index;
    uint32_t expected_event_kind;
    uint32_t ticket_index;
    uint32_t reserved;
} lccf_model_ticket_t;
```

Only timer/cancel arms all three. I/O and RPC arm one and leave the other two
retired.

- The causal queued path performs:

```text
ARMED(g) --CAS--> CLAIMED(g)
publish event in causal cell
CLAIMED(g) -> QUEUED(g)
push cell on intrusive local ring
pop cell
QUEUED(g) -> RUNNING(g)
direct cell-to-instance-to-site callback
validate command
RUNNING(g) -> ARMED(g+1) or TERMINAL(g)
retire each losing immutable ticket without calling module code
```

- [ ] **Step 1: Write failing lifecycle and differential tests**

Add exact assertions for:

```c
/* Completion/timeout/cancel ticket order is shuffled for 3 seeds.
   Exactly one ticket claims each generation, two are stale, one callback runs,
   and backend_refs returns to zero. */
/* Replaying the previous generation changes only stale_tickets. */
/* A baseline batch and causal queue batch are field-equal after each of
   19 rounds for 3 workloads x 3 seeds x 3 frame sizes x 3 cell sizes
   x 2 site distributions x {37,257} instances. */
/* Cell stride addresses differ by exactly config.cell_bytes. */
```

- [ ] **Step 2: Run to verify candidate tests fail**

Run:

```sh
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g \
  -Iexperiments/lccf \
  experiments/lccf/lccf_model.c \
  experiments/lccf/lccf_workloads.c \
  experiments/lccf/test_lccf_model.c \
  -pthread -o object/lccf-tdd/test_lccf_model
object/lccf-tdd/test_lccf_model
```

Expected: failure identifying `causal_cell_queue` as unsupported.

- [ ] **Step 3: Implement causal cells, ticket retirement, and canonical comparison**

Do not compare mode-specific queue pointers or packed lifecycle words in
`lccf_model_batch_equal()`. Compare frame core fields, current generation,
current site, terminal state, last validated command, winner event, and
backend-reference count. Include the same canonical fields in checksum order.

- [ ] **Step 4: Run the complete C model test**

Run:

```sh
object/lccf-tdd/test_lccf_model
```

Expected: all baseline, lifecycle, stale-ticket, and queued differential checks
pass with `hot_allocations=0`.

- [ ] **Step 5: Commit the causal-cell queue**

```sh
git add experiments/lccf
git commit -m "feat: model generation-checked causal cells"
```

---

### Task 3: Fused Dispatch, Exact Causal Budgets, And Fairness

**Files:**

- Modify: `experiments/lccf/lccf_model.c`
- Modify: `experiments/lccf/lccf_workloads.c`
- Modify: `experiments/lccf/test_lccf_model.c`

**Interfaces:**

- Produces `LCCF_MODEL_FUSED_CAUSAL_CELL` and
  `LCCF_MODEL_BUDGETED_FUSED_CHAIN`.
- `FUSED_CAUSAL_CELL` claims a local ticket and invokes the compiler site
  without ready publication or dequeue.
- `BUDGETED_FUSED_CHAIN` handles `CONTINUE` in an iterative owner loop. It
  never recursively calls a resume site.
- The direct budget counts callbacks, not effects. With budget `B`, the first
  `B` callbacks are direct; callback `B + 1` enters the preallocated escape
  ring and increments `forced_escapes` exactly once.
- Mixed fairness inserts a timestamped due item every 32 continuation
  admissions. The owner services due timers and stackful placeholders at each
  budget boundary. A fixed 64-bucket logarithmic histogram stores dispatch
  latency without allocating.

- [ ] **Step 1: Write failing fused and budget tests**

Cover:

```c
/* Fused vs baseline equality after every round for the complete Task 2
   differential matrix. */
/* Fused metrics have queue_pushes == queue_pops == 0 and
   direct_calls == resume_calls. */
/* chain length 1 and budget 1: no escape. */
/* chain length 9 and budget 8: exactly one escape, then queued execution. */
/* chain length 18 and budget 8: exactly two escape boundaries. */
/* maximum callback nesting observed by an instrumented site is exactly 1. */
/* mixed fairness produces nonzero samples and services every placeholder. */
```

- [ ] **Step 2: Run to verify fused tests fail**

Run the Task 2 compile command and binary.

Expected: failure identifying `fused_causal_cell` as unsupported.

- [ ] **Step 3: Implement direct owner loops and escape admission**

Use a loop shaped as below. Set `direct_count` to `1` when the current callback
was entered directly from a completion and to `0` when it was entered from the
escape queue:

```c
while (command.kind == LCCF_MODEL_COMMAND_CONTINUE &&
       direct_count < batch->config.direct_budget &&
       !fairness_due(batch)) {
    ++direct_count;
    resume_and_validate(instance, cell, metrics);
}
if (command.kind == LCCF_MODEL_COMMAND_CONTINUE) {
    escape_push(batch, cell, metrics);
}
```

Check the budget before every subsequent callback so no off-by-one direct
execution is possible. Poll the fairness source before re-entering direct
execution.

- [ ] **Step 4: Run differential and fairness tests**

Run:

```sh
object/lccf-tdd/test_lccf_model
```

Expected: every canonical equality, exact escape count, nesting, and fairness
assertion passes.

- [ ] **Step 5: Commit the fused modes**

```sh
git add experiments/lccf
git commit -m "feat: model bounded completion fusion"
```

---

### Task 4: Persistent Remote Producers And Cross-Thread Claims

**Files:**

- Create: `experiments/lccf/lccf_platform.h`
- Create: `experiments/lccf/lccf_platform.c`
- Modify: `experiments/lccf/lccf_model_internal.h`
- Modify: `experiments/lccf/lccf_model.c`
- Modify: `experiments/lccf/test_lccf_model.c`

**Interfaces:**

- Produces:

```c
typedef int (*lccf_platform_thread_fn)(void *context);
typedef struct lccf_platform_thread lccf_platform_thread_t;
typedef struct lccf_platform_event lccf_platform_event_t;

int lccf_platform_thread_start(lccf_platform_thread_t **out,
                               lccf_platform_thread_fn fn,
                               void *context);
int lccf_platform_thread_join(lccf_platform_thread_t *thread,
                              int *out_result);
int lccf_platform_event_create(lccf_platform_event_t **out);
void lccf_platform_event_destroy(lccf_platform_event_t *event);
int lccf_platform_event_signal(lccf_platform_event_t *event);
int lccf_platform_event_wait(lccf_platform_event_t *event,
                             uint64_t observed_epoch);
uint64_t lccf_platform_event_epoch(lccf_platform_event_t *event);
uint64_t lccf_platform_monotonic_ns(void);
uint64_t lccf_platform_process_cpu_ns(void);
int lccf_platform_pin_current_thread(unsigned cpu);
int lccf_platform_describe(char *buffer, size_t buffer_size);
```

- POSIX uses `pthread_create`, `pthread_mutex_t`, `pthread_cond_t`,
  `clock_gettime(CLOCK_MONOTONIC)`, and
  `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)`.
- Windows uses `CreateThread`, `SRWLOCK`, `CONDITION_VARIABLE`,
  `QueryPerformanceCounter`, and `GetProcessTimes`.
- Darwin host description uses `pthread_get_qos_class_np` and
  `sysctlbyname("hw.activecpu", &active_cpu, &active_cpu_size, NULL, 0U)`;
  absence of either diagnostic is recorded explicitly and does not alter
  execution policy.
- The remote ring is a bounded sequence-number queue. Every slot owns
  `_Atomic size_t sequence` and one pointer. Producers reserve positions with
  `atomic_fetch_add_explicit(&queue->enqueue_pos, 1U,
  memory_order_relaxed)`, wait for the slot sequence with acquire, publish the
  pointer, then store the next sequence with release. The single owner performs
  the matching acquire/release dequeue.
- The same ring and producer partition are used by both remote modes:
  `remote_waker_queue` publishes waker pointers; `remote_causal_cell` publishes
  claimed cell pointers.
- Two persistent producers receive an epoch, process disjoint even/odd ticket
  indices, publish all items, and signal completion. The owner drains until it
  has resumed exactly `instance_count` instances. No producer spins while no
  measured remote round is active.

- [ ] **Step 1: Write failing platform, race, and remote differential tests**

Add:

```c
/* Platform event wait observes ten monotonically increasing epochs. */
/* Three threads race completion/timeout/cancel tickets for 10,000 reset
   generations; claims == generations, stale == 2 * generations, callbacks
   == generations, backend_refs == 0. */
/* remote_waker_queue and remote_causal_cell are field-equal after each of
   19 rounds for 37 and 257 instances and three seeds. */
/* Both remote modes report remote_pushes == instances * rounds and
   hot_allocations == 0. */
/* Destroy joins producers even after an injected round error. */
```

- [ ] **Step 2: Compile to verify remote tests fail**

Run:

```sh
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g \
  -Iexperiments/lccf \
  experiments/lccf/lccf_platform.c \
  experiments/lccf/lccf_model.c \
  experiments/lccf/lccf_workloads.c \
  experiments/lccf/test_lccf_model.c \
  -pthread -o object/lccf-tdd/test_lccf_model
```

Expected: compile or assertion failure because platform and remote contracts
are not implemented.

- [ ] **Step 3: Implement the platform layer and persistent remote team**

Return errno-style positive error codes. Treat affinity failure as a recorded
capability result, not a correctness failure. Treat clock failure, thread
creation failure, lost epoch, queue overrun, or join failure as a hard error.

- [ ] **Step 4: Run correctness plus race stress**

Run:

```sh
object/lccf-tdd/test_lccf_model
```

Expected: all local, concurrent race, remote differential, teardown, and
allocation checks pass.

- [ ] **Step 5: Commit remote execution**

```sh
git add experiments/lccf
git commit -m "feat: model remote causal admission"
```

---

### Task 5: Paired Native Benchmark Driver

**Files:**

- Create: `experiments/lccf/bench_lccf_model.c`
- Modify: `experiments/lccf/test_lccf_model.c`

**Interfaces:**

- The driver accepts exactly:

```text
--workload NAME
--candidate NAME
--instances DECIMAL
--frame-bytes 64|128|256
--cell-bytes 64|96|128
--sites 1|8
--budget 1..1024
--chain 1..1024
--producers 2
--min-mode-ms 1..60000
--seed UINT64
--order abba|baab
--owner-cpu DECIMAL|none
```

- Candidates are `causal_cell_queue`, `fused_causal_cell`,
  `budgeted_fused_chain`, and `remote_causal_cell`. The driver selects
  `waker_queue` for the first three and `remote_waker_queue` for the last.
- Calibration resets both batches, doubles a common rounds-per-block value,
  and stops only when each mode takes at least half of `min-mode-ms`. Measured
  `ABBA` or `BAAB` then gives each mode two blocks and at least the full
  minimum duration.
- Reset, checksum, and equality time is outside measured wall and CPU
  intervals.
- `wall_speedup = baseline_wall_ns_per_op / candidate_wall_ns_per_op`.
- `cpu_ratio = candidate_cpu_ns_per_op / baseline_cpu_ns_per_op`.
- Emit exactly one line with these exact keys:

```text
LCCF_PAIR version=1 workload=completion_io_pipeline candidate=fused_causal_cell baseline=waker_queue instances=65536 frame_bytes=128 cell_bytes=96 sites=8 budget=8 chain=1 producers=2 min_mode_ns=250000000 rounds_per_block=128 ops_per_mode=16777216 baseline_wall_ns=1 candidate_wall_ns=1 baseline_cpu_ns=1 candidate_cpu_ns=1 wall_speedup=1.000000000 cpu_ratio=1.000000000 baseline_fair_p99_ns=0 candidate_fair_p99_ns=0 checksum=0000000000000000 hot_allocations=0 forced_escapes=0 direct_calls=1 order=ABBA affinity=none
```

Numeric values above are parser fixtures, not expected performance.

- [ ] **Step 1: Add a CLI smoke test before the driver exists**

Extend `test_lccf_model.c` only with reusable name/baseline mapping tests.
Create the driver with argument parsing first, then verify:

```sh
object/lccf-tdd/bench_lccf_model \
  --workload completion_io_pipeline \
  --candidate fused_causal_cell \
  --instances 4096 \
  --frame-bytes 128 \
  --cell-bytes 96 \
  --sites 8 \
  --budget 8 \
  --chain 1 \
  --producers 2 \
  --min-mode-ms 20 \
  --seed 7810762890074515045 \
  --order abba \
  --owner-cpu none
```

The initial run must fail to build because the driver is absent.

- [ ] **Step 2: Implement strict parsing, calibration, paired execution, and output**

Reject missing, duplicate, unknown, signed, overflowing, zero, or
out-of-domain arguments. Reject `NaN`, infinity, a zero duration, unequal
states, unequal checksums, insufficient measured time, nonzero hot allocation,
or an unexpected metric invariant.

- [ ] **Step 3: Compile and run all four candidate smoke pairs**

Run:

```sh
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O3 -DNDEBUG \
  -Iexperiments/lccf \
  experiments/lccf/lccf_platform.c \
  experiments/lccf/lccf_model.c \
  experiments/lccf/lccf_workloads.c \
  experiments/lccf/bench_lccf_model.c \
  -pthread -o object/lccf-tdd/bench_lccf_model
```

Run the smoke command once for each candidate. Expected: one valid
`LCCF_PAIR` line and exit status zero each time.

- [ ] **Step 4: Commit the benchmark driver**

```sh
git add experiments/lccf
git commit -m "bench: add paired LCCF cost driver"
```

---

### Task 6: Strict Runner, Paired Aggregation, And Verdict

**Files:**

- Create: `scripts/bench_lccf_model.py`
- Create: `scripts/test_bench_lccf_model.py`

**Interfaces:**

- Produces:

```python
@dataclass(frozen=True)
class PairRow:
    workload: str
    candidate: str
    baseline: str
    instances: int
    frame_bytes: int
    cell_bytes: int
    sites: int
    budget: int
    chain: int
    producers: int
    min_mode_ns: int
    rounds_per_block: int
    ops_per_mode: int
    baseline_wall_ns: int
    candidate_wall_ns: int
    baseline_cpu_ns: int
    candidate_cpu_ns: int
    wall_speedup: float
    cpu_ratio: float
    baseline_fair_p99_ns: int
    candidate_fair_p99_ns: int
    checksum: str
    hot_allocations: int
    forced_escapes: int
    direct_calls: int
    order: str
    affinity: str

@dataclass(frozen=True)
class SampleRow:
    process_sample: int
    row: PairRow

@dataclass(frozen=True)
class SummaryRow:
    workload: str
    candidate: str
    baseline: str
    frame_bytes: int
    cell_bytes: int
    sites: int
    budget: int
    chain: int
    producers: int
    sample_count: int
    wall_speedup: float
    cpu_ratio: float
    wall_ratio_spread: float
    cpu_ratio_spread: float
    baseline_wall_ns_per_op: float
    candidate_wall_ns_per_op: float
    baseline_cpu_ns_per_op: float
    candidate_cpu_ns_per_op: float
    fairness_p99_ratio: float
    checksum: str

REQUIRED_SIGNATURES = (
    "parse_output(text: str) -> PairRow",
    "summarize(rows: list[SampleRow]) -> list[SummaryRow]",
    "classify(rows: list[SummaryRow], expected_samples: int) "
    "-> tuple[str, list[str], int | None]",
    "write_evidence(out_dir: Path, tracked_report: Path | None, "
    "samples: list[SampleRow], summary: list[SummaryRow], verdict: str, "
    "reasons: list[str], selected_cell_bytes: int | None, "
    "metadata: dict[str, object]) -> None",
)
```

- Median selection sorts by the process-local `wall_speedup` and keeps the
  complete middle `PairRow`; it never combines unrelated fields from different
  processes.
- Recompute both ratios from raw durations and operation counts. Reject a row
  when its printed ratio differs by more than `1e-9` relative error.
- For non-fairness rows, require both p99 fields to be zero and record
  `fairness_p99_ratio=1.0`. For mixed fairness, require both fields positive
  and calculate candidate p99 divided by baseline p99.
- Full matrix:
  - three functional workloads × four candidates as applicable;
  - `causal_cell_queue` and `fused_causal_cell` across `3` frame footprints ×
    `3` cell footprints × `2` site distributions;
  - `budgeted_fused_chain` across the same dimensions with budget `8` and
    chain length `18`;
  - `remote_causal_cell` across three functional workloads × three cell
    footprints at frame `128`, sites `8`;
  - `completion_mixed_fairness` × `budgeted_fused_chain` × three cell
    footprints at frame `128`, sites `8`, budget `8`, chain length `18`.
- Quick matrix uses one process, `4096` instances, `20 ms` per mode, frame
  `128`, cell `96`, sites `8`, and all four candidates. It emits
  `SMOKE_ONLY`, never a research verdict.

- [ ] **Step 1: Write parser and classifier tests first**

Tests must reject:

```python
BAD_ROWS = (
    "",
    "noise\nLCCF_PAIR version=1",
    SAMPLE_ROW + "\n" + SAMPLE_ROW,
    SAMPLE_ROW.replace("version=1", "version=2"),
    SAMPLE_ROW.replace("wall_speedup=1.000000000", "wall_speedup=nan"),
    SAMPLE_ROW.replace("hot_allocations=0", "hot_allocations=1"),
    SAMPLE_ROW.replace("order=ABBA", "order=AABB"),
)
```

Build synthetic matrices that prove:

- the smallest fully passing cell footprint is selected;
- one delimited workload produces `NARROW`;
- reproducible gate failure produces `REJECT`;
- missing rows, sample count other than nine, duration below `250 ms`, checksum
  mismatch, ratio disagreement, and either spread above `1.10x` produce
  `INCONCLUSIVE`;
- a fast absolute outlier does not dominate median process-local ratios;
- quick mode returns `SMOKE_ONLY`.

- [ ] **Step 2: Run tests to verify the module is missing**

Run:

```sh
LCCF_MODEL_TEST_BINARY=object/lccf-tdd/bench_lccf_model \
  python3 scripts/test_bench_lccf_model.py
```

Expected: import failure because `bench_lccf_model.py` does not exist.

- [ ] **Step 3: Implement strict parsing and evidence logic**

Use `scripts/process_utils.py` for timeout-safe process capture and
`scripts/safe_output.py` for atomic UTF-8 evidence writes. The runner rotates
matrix order by sample index and alternates `ABBA`/`BAAB`. It writes evidence
even for `REJECT` and `INCONCLUSIVE`, but exits nonzero only on runner or
integrity failure, not on a valid negative research verdict.

- [ ] **Step 4: Run Python tests and the quick native matrix**

Run:

```sh
LCCF_MODEL_TEST_BINARY=object/lccf-tdd/bench_lccf_model \
  python3 scripts/test_bench_lccf_model.py
python3 scripts/bench_lccf_model.py \
  --binary object/lccf-tdd/bench_lccf_model \
  --out-dir object/lccf-phase0-quick \
  --quick
```

Expected: parser/classifier tests pass and quick report says `SMOKE_ONLY` with
all checksums equal and all hot-allocation counts zero.

- [ ] **Step 5: Commit the runner**

```sh
git add scripts/bench_lccf_model.py scripts/test_bench_lccf_model.py
git commit -m "bench: classify paired LCCF evidence"
```

---

### Task 7: Make, CMake, Windows Routing, And Sanitizers

**Files:**

- Modify: `.gitignore`
- Modify: `Makefile:34-112`
- Modify: `Makefile:534-540`
- Modify: `Makefile:573-611`
- Modify: `Makefile:626-686`
- Modify: `Makefile:826-829`
- Modify: `Makefile:1949-1957`
- Modify: `Makefile:2020-2024`
- Modify: `Makefile:2119-2123`
- Modify: `CMakeLists.txt:499-531`
- Modify: `CMakeLists.txt:730-738`

**Interfaces:**

- Add targets `test_lccf_model` and `bench_lccf_model`.
- Add Make phony targets:

```make
test-lccf-model: test_lccf_model bench_lccf_model
	./test_lccf_model
	LCCF_MODEL_TEST_BINARY=./bench_lccf_model \
		python3 scripts/test_bench_lccf_model.py

lccf-model-report: test-lccf-model
	python3 scripts/bench_lccf_model.py \
		--binary ./bench_lccf_model \
		--cc "$(CC)" \
		--out-dir object/lccf-phase0 \
		--tracked-report \
			docs/research/reports/2026-07-26-lccf-phase0-results.md
```

- Add CTest names `test_lccf_model` and `test_bench_lccf_model`, passing the
  target-file path through `LCCF_MODEL_TEST_BINARY`.
- Both native targets link only the standalone model/platform files and the
  platform thread library. They do not link `llam_runtime`.
- Add the two test names and targets to Windows CMake delegation and its
  regular-expression filter.

- [ ] **Step 1: Write the Make/CMake target references before definitions**

Add clean entries, object lists, and test invocations first.

- [ ] **Step 2: Verify the build fails at the missing target rules**

Run:

```sh
make -n test-lccf-model
cmake -S . -B /tmp/llam-lccf-build -DCMAKE_BUILD_TYPE=Release
```

Expected: Make or CMake identifies incomplete target/source integration.

- [ ] **Step 3: Complete Make and CMake integration**

Mirror the existing LCWE standalone-target warning and platform-definition
policy. Add `/test_lccf_model` and `/bench_lccf_model` to `.gitignore`.

- [ ] **Step 4: Run native, CMake, sanitizer, and Windows-routing verification**

Run:

```sh
make test-lccf-model
cmake -S . -B /tmp/llam-lccf-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/llam-lccf-build \
  --target test_lccf_model bench_lccf_model
ctest --test-dir /tmp/llam-lccf-build --output-on-failure \
  -R 'test_lccf_model|test_bench_lccf_model'
```

On Clang/GCC, also run:

```sh
mkdir -p object/lccf-sanitize
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iexperiments/lccf \
  experiments/lccf/lccf_platform.c \
  experiments/lccf/lccf_model.c \
  experiments/lccf/lccf_workloads.c \
  experiments/lccf/test_lccf_model.c \
  -pthread -o object/lccf-sanitize/test_lccf_model
ASAN_OPTIONS=detect_leaks=1 \
  UBSAN_OPTIONS=halt_on_error=1 \
  object/lccf-sanitize/test_lccf_model
```

When the compiler supports TSan on the host, build the same sources with
`-fsanitize=thread` and run the remote/race suite. If the platform runtime
cannot start TSan, record that exact runtime error in the report rather than
claiming a pass.

Check Windows routing without executing Windows binaries:

```sh
make -n OS=Windows_NT test-lccf-model
```

Expected: it configures CMake and requests both LCCF targets and CTest names.

- [ ] **Step 5: Commit build integration**

```sh
git add .gitignore Makefile CMakeLists.txt
git commit -m "build: integrate LCCF research harness"
```

---

### Task 8: Full Paired Experiment, Evidence Review, And Decision

**Files:**

- Generate: `object/lccf-phase0/lccf_phase0_samples.csv`
- Generate: `object/lccf-phase0/lccf_phase0_summary.csv`
- Generate: `object/lccf-phase0/lccf_phase0_metadata.json`
- Generate: `object/lccf-phase0/lccf_phase0_report.md`
- Generate: `docs/research/reports/2026-07-26-lccf-phase0-results.md`
- Modify only if evidence exposes a harness defect:
  `experiments/lccf/*`, `scripts/bench_lccf_model.py`,
  `scripts/test_bench_lccf_model.py`, `Makefile`, or `CMakeLists.txt`

**Interfaces:**

- The report verdict is exactly one of `PROMISING`, `NARROW`, `REJECT`, or
  `INCONCLUSIVE`.
- A valid negative verdict is a successful experiment and must not be rewritten
  as a preferred result.
- Any harness correction requires a regression test, a focused fix commit, a
  fresh full matrix, and replacement of all evidence from the defective run.

- [ ] **Step 1: Record the optimized binary and host metadata**

Run:

```sh
make clean
make test-lccf-model
cc --version
uname -a
```

Do not close user applications or change system-wide power settings. The
paired protocol is responsible for tolerating an interactive host.

- [ ] **Step 2: Run the full matrix once**

Run:

```sh
python3 scripts/bench_lccf_model.py \
  --binary ./bench_lccf_model \
  --samples 9 \
  --instances 65536 \
  --min-mode-ms 250 \
  --budget 8 \
  --chain 18 \
  --producers 2 \
  --cc "${CC:-cc}" \
  --out-dir object/lccf-phase0 \
  --tracked-report \
    docs/research/reports/2026-07-26-lccf-phase0-results.md
```

Expected: complete raw and summary files plus one evidence-derived verdict.

- [ ] **Step 3: Audit the evidence mechanically**

Run:

```sh
python3 scripts/test_bench_lccf_model.py
rg -n 'PROMISING|NARROW|REJECT|INCONCLUSIVE|MISMATCH|hot_allocations|spread' \
  object/lccf-phase0/lccf_phase0_report.md \
  docs/research/reports/2026-07-26-lccf-phase0-results.md
git diff --check
```

Confirm that the tracked and raw reports name the same verdict, selected cell
footprint, process count, matrix count, and gate failures or passes.

- [ ] **Step 4: Run repository regression verification**

Run:

```sh
make test-quick
cmake -S . -B /tmp/llam-lccf-full -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/llam-lccf-full
ctest --test-dir /tmp/llam-lccf-full --output-on-failure
```

If a pre-existing timing-sensitive test fails, reproduce the exact failing
binary repeatedly and compare the branch diff before modifying unrelated
runtime code.

- [ ] **Step 5: Commit the evidence without changing its verdict**

```sh
git add docs/research/reports/2026-07-26-lccf-phase0-results.md
git commit -m "docs: record LCCF phase zero evidence"
```

- [ ] **Step 6: Apply the decision**

- `PROMISING`: write the next plan for an internal scalar instance with a
  synthetic completion source; keep the experimental ABI private.
- `NARROW`: add the report's exact workload/site eligibility predicate to the
  next internal prototype plan.
- `REJECT`: stop implementation and derive the next Executor-plane hypothesis
  from the measured queue, claim, dispatch, remote, and fairness components.
- `INCONCLUSIVE`: rerun only after the report identifies and repairs the
  integrity or environment cause; do not relax a gate.

---

## Final Verification Checklist

- [ ] `git diff --check` is clean.
- [ ] No path under `src/` or `include/llam/` changed.
- [ ] `make test-lccf-model` passes.
- [ ] ASan/UBSan passes; TSan status is recorded accurately.
- [ ] CMake builds both targets and their CTests pass.
- [ ] Windows dry-run routes both targets and tests through CMake.
- [ ] Every full matrix cell has nine `ABBA`/`BAAB` process-local pairs.
- [ ] Every mode meets its minimum measured duration.
- [ ] All canonical checksums agree and `hot_allocations` is zero.
- [ ] The report derives its verdict from the committed gates with no omitted
  samples.
- [ ] `make test-quick` passes.
- [ ] Full CTest passes or an independently reproduced unrelated transient is
  documented with evidence.
- [ ] Only experiment, runner, build-integration, plan, and report files are
  staged.
