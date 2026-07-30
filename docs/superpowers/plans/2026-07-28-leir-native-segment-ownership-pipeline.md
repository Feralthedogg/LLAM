# LEIR Native Segment Ownership and Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a cancellation-safe, reusable Linux native-segment backend that
can lower RECV → SEND, use fixed receive buffers and fixed files, and execute up
to eight segments through one task-owned batch ticket.

**Architecture:** LEIR remains the semantic planner. Linux segment tokens gain a
separate semantic-terminal and kernel-retirement protocol; a bounded batch
ticket owns one request, queue publication, pending unit, park, and wake for
1–8 segments. Fixed-mode instances attach cold descriptor duplicates and
anonymous scratch buffers to per-node sparse io_uring resource tables.

**Tech Stack:** C11, C atomics, pthread mutexes, liburing/io_uring, CMake,
Make, Python 3 `unittest`, GitHub Actions on Ubuntu 24.04.

## Global Constraints

- At most 8 operations per segment, 8 segments per batch, 64 operation SQEs,
  and 64 cancel SQEs per batch.
- No allocation in activation, submission, completion, or cancellation paths.
- Cold bind/attach may duplicate fds and allocate anonymous scratch buffers.
- A segment is reusable only after its final non-skipped target CQE and every
  submitted cancel CQE have been observed.
- One published batch owns exactly one `pending_ops` unit and releases it once.
- RECV fixed mode uses `READ_FIXED`; SEND retains `MSG_NOSIGNAL` and uses a
  fixed file but not `WRITE_FIXED` or `SEND_ZC`.
- Fixed-resource failure disables only the fixed candidate.
- Portable interpreter behavior and public LLAM APIs do not change.
- Platform-specific Linux evidence remains separate from portable LEIR
  evidence.
- Do not bump the version or release unless correctness, full CI, and the
  specialized-performance gate all pass.

---

## File map

### Runtime backend

- `src/io/linux/runtime_io_segment_linux_internal.h`
  defines operation, token, segment, batch, cancellation, and resource
  contracts.
- `src/io/linux/watch/linux_segment.c`
  validates operations, encodes SQEs, reduces operation CQEs, queues batches,
  and submits operation SQEs.
- `src/io/linux/watch/linux_segment_cancel.c`
  removes queued batches, publishes in-flight cancellation, encodes cancel
  SQEs, reduces cancel CQEs, and finalizes reusable batches.
- `src/io/linux/watch/linux_segment_resources.c`
  creates sparse tables, leases/updates slots, rolls back partial attachment,
  and releases slots.
- `src/io/linux/watch/linux_submit.c`
  drains operation batches before native cancellations in one worker pass.
- `src/io/linux/watch/cqe.c`
  dispatches operation and cancel token tags.
- `src/io/api/issue.c`
  issues width-one and width-N tickets and parks one task.
- `src/core/wait/wait_tracking.c`
  routes native request aborts to queued detach or in-flight cancellation.
- `src/internal/runtime_state.h`
  reserves the native-cancel user-data tag.
- `src/internal/runtime_types.h`
  stores node queues/resource arena and the request's active native ticket.
- `src/internal/runtime_proto_io.h`,
  `src/io/runtime_io_api_internal.h`, and
  `src/io/linux/runtime_io_watch_linux_internal.h`
  publish private cross-file interfaces.
- `src/core/lifecycle/init.c`, `src/core/lifecycle/shutdown.c`, and
  `src/io/engine/io_engine.c`
  initialize and retire node resource arenas around ring lifetime.

### LEIR experiment

- `experiments/leir/leir_native_plan.c`
  accepts bounded RECV → SEND success chains.
- `experiments/leir/leir_native_segment.h`
  exposes fixed candidates, batch-run inputs, and batch metrics.
- `experiments/leir/leir_native_segment.c`
  owns instance bind/attach/copy/publish behavior and width-one wrapper logic.
- `experiments/leir/test_leir_native_plan.c`
  tests pipeline planning.
- `experiments/leir/test_leir_native_linux.c`
  tests reducers, queues, cancellation races, fixed SQEs, and slot rollback.
- `experiments/leir/test_leir_native_segment.c`
  tests instance batching, copying, cancellation, and integration behavior.
- `experiments/leir/bench_leir_native_pipeline.c`
  measures public, native, fixed, and batch echo candidates.

### Evidence and build

- `scripts/bench_leir_native_pipeline.py`
  runs the Linux matrix, confidence intervals, and decision gates.
- `scripts/test_bench_leir_native_pipeline.py`
  locks the CLI/output/report contract and decision classifier.
- `.github/workflows/leir-native-research.yml`
  builds, sanitizes, stresses, benchmarks, and uploads the pipeline evidence.
- `Makefile` and `CMakeLists.txt`
  compile the new backend files, tests, benchmark, and Python contract.
- `docs/operations/benchmarks.md`
  documents the new report without mixing it into portable claims.

---

### Task 1: Admit bounded RECV → SEND plans

**Files:**
- Modify: `experiments/leir/leir_native_plan.c`
- Modify: `experiments/leir/test_leir_native_plan.c`

**Interfaces:**
- Consumes: existing `leir_native_plan_compile(const leir_phase0_program_t *,
  leir_native_plan_t *)`.
- Produces: the same function accepting a RECV node whose `on_success` names a
  later SEND node.

- [ ] **Step 1: Write the failing planner tests**

Add a two-operation program and assert the compiled order:

```c
static int test_accepts_recv_then_send(void) {
    leir_phase0_program_t *program = make_recv_send_program();
    leir_native_plan_t plan;
    int rc = leir_native_plan_compile(program, &plan);

    if (rc != 0 ||
        plan.step_count != 2U ||
        plan.steps[0].kind != LEIR_NATIVE_STEP_RECV ||
        plan.steps[1].kind != LEIR_NATIVE_STEP_SEND ||
        plan.return_node != 4U) {
        return 1;
    }
    leir_phase0_program_destroy(program);
    return 0;
}
```

Add a negative case where the SEND consumes the RECV result slot as its length:

```c
program->nodes[1].length_slot = program->nodes[0].result_slot;
errno = 0;
if (leir_native_plan_compile(program, &plan) == 0 || errno != EINVAL) {
    return 1;
}
```

- [ ] **Step 2: Run the planner test and observe the intended failure**

Run:

```bash
make -j4 test_leir_native_plan
./test_leir_native_plan
```

Expected: `accepts recv then send` fails because the current compiler requires
RECV success to name RETURN.

- [ ] **Step 3: Relax only the RECV terminal restriction**

Delete this condition from the effect-node validation:

```c
(kind == LEIR_NATIVE_STEP_RECV &&
 !node_is_terminal_return(program, node->on_success))
```

Keep the shared `on_success` range check, terminal EOF/error checks, visited
cycle check, prior-result alias check, and supported operation-count check.

- [ ] **Step 4: Run planner and portable LEIR tests**

Run:

```bash
make -j4 test_leir_native_plan test_leir_phase0
./test_leir_native_plan
./test_leir_phase0
```

Expected: both pass.

- [ ] **Step 5: Commit**

```bash
git add experiments/leir/leir_native_plan.c \
  experiments/leir/test_leir_native_plan.c
git commit -m "feat: compile LEIR receive-send segments"
```

---

### Task 2: Separate semantic completion from target retirement

**Files:**
- Modify: `src/io/linux/runtime_io_segment_linux_internal.h`
- Modify: `src/io/linux/watch/linux_segment.c`
- Modify: `experiments/leir/test_leir_native_linux.c`

**Interfaces:**
- Consumes: unique `llam_linux_native_token_t` per operation.
- Produces:

```c
typedef enum llam_linux_native_cqe_action {
    LLAM_LINUX_NATIVE_CQE_CONTINUE = 0,
    LLAM_LINUX_NATIVE_CQE_SEMANTIC = 1,
    LLAM_LINUX_NATIVE_CQE_RETIRED_OK = 2,
    LLAM_LINUX_NATIVE_CQE_RETIRED_ERROR = 3,
    LLAM_LINUX_NATIVE_CQE_FATAL = 4,
} llam_linux_native_cqe_action_t;
```

The segment gains:

```c
uint64_t observed_operation_mask;
int semantic_result;
atomic_uint semantic_claimed;
atomic_uint target_retired;
```

Replace the terminal state with:

```c
enum {
    LLAM_LINUX_NATIVE_SEGMENT_IDLE = 0U,
    LLAM_LINUX_NATIVE_SEGMENT_QUEUED = 1U,
    LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT = 2U,
    LLAM_LINUX_NATIVE_SEGMENT_RETIRING = 3U,
    LLAM_LINUX_NATIVE_SEGMENT_RETIRED = 4U,
};
```

- [ ] **Step 1: Replace the early-skip-error test with a retirement test**

Exercise the first visible error CQE for index 1. Linux omits the rest of a
soft-linked chain when a `CQE_SKIP_SUCCESS` request fails:

```c
action = llam_linux_native_segment_apply_cqe(
    &segment, &segment.tokens[1], -ECONNRESET, &result);
CHECK(action == LLAM_LINUX_NATIVE_CQE_RETIRED_ERROR);
CHECK(result == -ECONNRESET);
CHECK(atomic_load(&segment.target_retired) == 1U);
```

Add a duplicate-bit case. A duplicate token must return `FATAL`; target CQEs
within one soft-linked chain remain ordered by the kernel, while cancel-control
CQEs may interleave arbitrarily.

- [ ] **Step 2: Run the Linux unit test and observe the old early terminal**

Run:

```bash
make -j4 test_leir_native_linux
./test_leir_native_linux --unit-only
```

Expected: the new test fails because the first visible skip error currently
sets `TERMINAL` and rejects later target CQEs.

- [ ] **Step 3: Implement bitmap reduction and final-token retirement**

In `llam_linux_native_segment_apply_cqe()`:

```c
state = atomic_load_explicit(&segment->state, memory_order_acquire);
if (state != LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT &&
    state != LLAM_LINUX_NATIVE_SEGMENT_RETIRING) {
    return LLAM_LINUX_NATIVE_CQE_FATAL;
}
bit = UINT64_C(1) << index;
if ((segment->observed_operation_mask & bit) != 0U) {
    return LLAM_LINUX_NATIVE_CQE_FATAL;
}
segment->observed_operation_mask |= bit;
error = llam_linux_native_result_error(&segment->ops[index], result);
llam_linux_native_record_error(segment, index, error);

if (error != 0 && atomic_exchange_explicit(
        &segment->semantic_claimed, 1U, memory_order_acq_rel) == 0U) {
    segment->semantic_result = -error;
    atomic_store_explicit(&segment->state,
                          LLAM_LINUX_NATIVE_SEGMENT_RETIRING,
                          memory_order_release);
    action = LLAM_LINUX_NATIVE_CQE_SEMANTIC;
}
if (index + 1U == segment->op_count) {
    atomic_store_explicit(&segment->target_retired, 1U, memory_order_release);
    if (atomic_exchange_explicit(
            &segment->semantic_claimed, 1U, memory_order_acq_rel) == 0U) {
        segment->semantic_result =
            segment->first_error != 0 ? -segment->first_error : result;
    }
    atomic_store_explicit(&segment->state,
                          LLAM_LINUX_NATIVE_SEGMENT_RETIRED,
                          memory_order_release);
    return segment->first_error != 0
        ? LLAM_LINUX_NATIVE_CQE_RETIRED_ERROR
        : LLAM_LINUX_NATIVE_CQE_RETIRED_OK;
}
return action;
```

Initialize the mask, semantic fields, and target flag at each activation.
Calculate suppressed success CQEs from the first visible index and final
success without using an ordered completion cursor.

- [ ] **Step 4: Run the reducer suite**

Run:

```bash
make -j4 test_leir_native_linux
./test_leir_native_linux --unit-only
```

Expected: pass, including late linked-cancellation CQEs.

- [ ] **Step 5: Commit**

```bash
git add src/io/linux/runtime_io_segment_linux_internal.h \
  src/io/linux/watch/linux_segment.c \
  experiments/leir/test_leir_native_linux.c
git commit -m "fix: separate native semantic and kernel retirement"
```

---

### Task 3: Introduce one-owner native batch tickets

**Files:**
- Modify: `src/io/linux/runtime_io_segment_linux_internal.h`
- Modify: `src/internal/runtime_types.h`
- Modify: `src/internal/runtime_proto_io.h`
- Modify: `src/io/runtime_io_api_internal.h`
- Modify: `src/io/linux/watch/linux_segment.c`
- Modify: `src/io/linux/watch/linux_submit.c`
- Modify: `src/io/api/issue.c`
- Modify: `experiments/leir/test_leir_native_linux.c`

**Interfaces:**
- Produces:

```c
#define LLAM_LINUX_NATIVE_BATCH_MAX_SEGMENTS 8U

enum {
    LLAM_LINUX_NATIVE_BATCH_IDLE = 0U,
    LLAM_LINUX_NATIVE_BATCH_QUEUED = 1U,
    LLAM_LINUX_NATIVE_BATCH_INFLIGHT = 2U,
    LLAM_LINUX_NATIVE_BATCH_RETIRING = 3U,
    LLAM_LINUX_NATIVE_BATCH_RETIRED = 4U,
};

enum {
    LLAM_LINUX_NATIVE_CANCEL_NONE = 0U,
    LLAM_LINUX_NATIVE_CANCEL_QUEUED = 1U,
    LLAM_LINUX_NATIVE_CANCEL_SUBMITTED = 2U,
    LLAM_LINUX_NATIVE_CANCEL_RETIRED = 3U,
};

typedef struct llam_linux_native_batch {
    llam_runtime_t *owner_runtime;
    llam_node_t *owner_node;
    llam_io_req_t *req;
    llam_linux_native_segment_t
        *segments[LLAM_LINUX_NATIVE_BATCH_MAX_SEGMENTS];
    struct llam_linux_native_batch *next;
    struct llam_linux_native_batch *cancel_next;
    unsigned segment_count;
    unsigned retired_segments;
    int terminal_result;
    unsigned first_error_segment;
    atomic_uint state;
    atomic_uint terminal_claimed;
    atomic_uint cancel_state;
} llam_linux_native_batch_t;

int llam_issue_linux_native_batch(llam_linux_native_batch_t *batch,
                                  llam_io_req_t *req);
bool llam_linux_native_batch_enqueue(llam_node_t *node,
                                     llam_linux_native_batch_t *batch,
                                     llam_io_req_t *req);
llam_linux_native_batch_t *
llam_linux_native_batch_take_all(llam_node_t *node);
unsigned llam_linux_native_batch_submit_one(
    llam_node_t *node, llam_linux_native_batch_t *batch);
```

Each segment gains `llam_linux_native_batch_t *batch`; it is assigned before
publication and cleared only after queued detach or full ticket retirement.

- [ ] **Step 1: Write failing width-two queue tests**

Build two configured segments and one batch:

```c
batch.segments[0] = &first;
batch.segments[1] = &second;
batch.segment_count = 2U;
CHECK(llam_linux_native_batch_enqueue(&node, &batch, &req));
CHECK(node.native_batch_head == &batch);
CHECK(node.native_batch_tail == &batch);
CHECK(atomic_load(&node.pending_ops) == 1U);
CHECK(first.queue_publications + second.queue_publications == 1U);
```

After `take_all`, assert the request transitions to `INFLIGHT` once and the
shard in-flight waiter counter increments once. Reject a duplicate segment,
mixed runtime, mixed mode capability failure, and a count of 0 or 9 without
mutating any state.

- [ ] **Step 2: Run the queue suite and observe missing batch symbols**

Run:

```bash
make -j4 test_leir_native_linux
./test_leir_native_linux --queue
```

Expected: compile failure for `llam_linux_native_batch_t` and batch APIs.

- [ ] **Step 3: Replace the node segment queue with a batch queue**

In `llam_node_t`, replace:

```c
struct llam_linux_native_segment *native_segment_head;
struct llam_linux_native_segment *native_segment_tail;
```

with:

```c
struct llam_linux_native_batch *native_batch_head;
struct llam_linux_native_batch *native_batch_tail;
struct llam_linux_native_batch *native_cancel_head;
struct llam_linux_native_batch *native_cancel_tail;
```

Validate every segment before taking state. Under `submit_lock`, CAS each
segment `IDLE -> QUEUED`, then CAS the batch `IDLE -> QUEUED`, acquire one
pending unit, attach the request, append one batch node, and kick once. Roll
back segment states in reverse order on any pre-publication failure.

- [ ] **Step 4: Submit complete batches and keep width-one compatibility**

`llam_linux_native_batch_submit_one()` loops over segments and calls the
existing complete-chain SQE preparation for each. It reports the total SQE
count. `llam_issue_linux_native_segment()` creates a stack-local batch:

```c
llam_linux_native_batch_t batch;
memset(&batch, 0, sizeof(batch));
batch.segments[0] = segment;
batch.segment_count = 1U;
atomic_init(&batch.state, LLAM_LINUX_NATIVE_BATCH_IDLE);
atomic_init(&batch.terminal_claimed, 0U);
atomic_init(&batch.cancel_state, LLAM_LINUX_NATIVE_CANCEL_NONE);
return llam_issue_linux_native_batch(&batch, req);
```

The stack object remains valid because `llam_issue_linux_native_batch()` does
not return before retirement.

- [ ] **Step 5: Run queue, dispatch, shutdown, and width-one tests**

Run:

```bash
make -j4 test_leir_native_linux test_runtime_shutdown_internal \
  test_leir_native_segment
./test_leir_native_linux --queue
./test_leir_native_linux --dispatch
./test_runtime_shutdown_internal
./test_leir_native_segment
```

Expected: pass; existing width-one metrics remain one publication, park, and
wake per activation.

- [ ] **Step 6: Commit**

```bash
git add src/io/linux/runtime_io_segment_linux_internal.h \
  src/internal/runtime_types.h src/internal/runtime_proto_io.h \
  src/io/runtime_io_api_internal.h src/io/linux/watch/linux_segment.c \
  src/io/linux/watch/linux_submit.c src/io/api/issue.c \
  experiments/leir/test_leir_native_linux.c
git commit -m "feat: publish native segments through batch tickets"
```

---

### Task 4: Add allocation-free queued and in-flight cancellation

**Files:**
- Create: `src/io/linux/watch/linux_segment_cancel.c`
- Modify: `src/io/linux/runtime_io_segment_linux_internal.h`
- Modify: `src/internal/runtime_state.h`
- Modify: `src/core/base/io_udata.c`
- Modify: `src/internal/runtime_types.h`
- Modify: `src/internal/runtime_proto_io.h`
- Modify: `src/io/linux/runtime_io_watch_linux_internal.h`
- Modify: `src/core/wait/wait_tracking.c`
- Modify: `src/io/linux/watch/linux_submit.c`
- Modify: `src/io/linux/watch/cqe.c`
- Modify: `CMakeLists.txt`
- Modify: `Makefile`
- Modify: `experiments/leir/test_leir_native_linux.c`

**Interfaces:**
- Produces:

```c
typedef struct llam_linux_native_cancel_token {
    _Alignas(8) llam_linux_native_segment_t *owner;
    uint64_t generation;
    uint16_t operation_index;
    uint16_t reserved16;
    uint32_t reserved32;
} llam_linux_native_cancel_token_t;

bool llam_linux_native_batch_abort_queued(
    llam_node_t *node, llam_linux_native_batch_t *batch,
    llam_io_req_t *req);
bool llam_linux_native_batch_request_cancel(
    llam_node_t *node, llam_linux_native_batch_t *batch,
    llam_io_req_t *req);
llam_linux_native_batch_t *
llam_linux_native_cancel_take_all(llam_node_t *node);
unsigned llam_linux_native_batch_submit_cancel(
    llam_node_t *node, llam_linux_native_batch_t *batch);
void llam_linux_native_cancel_handle_cqe(
    llam_node_t *node, llam_linux_native_cancel_token_t *token,
    int result);
```

Each segment embeds:

```c
llam_linux_native_cancel_token_t
    cancel_tokens[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
```

Each batch records:

```c
unsigned cancel_sqes_prepared;
unsigned cancel_cqes_observed;
bool cancel_requested;
```

Reserve:

```c
LLAM_IO_UDATA_NATIVE_CANCEL = 6U
```

Add `struct llam_linux_native_batch *linux_native_batch;` to
`llam_io_req_t` on Linux.

- [ ] **Step 1: Write deterministic cancellation race tests**

Add table-driven reducer cases:

```c
static const cancel_event_t cancel_before_target[] = {
    {EVENT_CANCEL, 0U, 0},
    {EVENT_TARGET, 0U, -ECANCELED},
    {EVENT_CANCEL, 1U, -ENOENT},
    {EVENT_TARGET, 1U, -ECANCELED},
};
static const cancel_event_t target_before_cancel[] = {
    {EVENT_TARGET, 0U, 16},
    {EVENT_TARGET, 1U, 16},
    {EVENT_CANCEL, 0U, -ENOENT},
    {EVENT_CANCEL, 1U, -ENOENT},
};
```

For every prefix, assert no terminal completion until the final target and all
cancel CQEs are seen. Add stale-generation, duplicate-cancel-token,
`-EALREADY`, and partial-match cases.

Add a queue test that removes two adjacent batch segments before `take_all`,
leaves unrelated batches linked, decrements `pending_ops` once, and leaves the
request available to the generic immediate abort wake.

- [ ] **Step 2: Run tests and observe unsupported cancellation**

Run:

```bash
make -j4 test_leir_native_linux
./test_leir_native_linux --unit-only
./test_leir_native_linux --queue
```

Expected: compile failure for cancel types and helpers.

- [ ] **Step 3: Implement queued detach**

Under `node->submit_lock`, locate the exact batch and request, unlink it from
`native_batch_head/tail`, CAS `QUEUED -> RETIRED`, set every segment to
`RETIRED`, clear `req->linux_native_batch`, set request wait mode to `NONE`, and
release the batch's one pending unit. Return false without mutation when owner,
generation, request, or state no longer matches.

- [ ] **Step 4: Implement the native cancel queue and SQEs**

`llam_linux_native_batch_request_cancel()` CASes cancel state
`NONE -> QUEUED`, appends once under `submit_lock`, and kicks the node.

For every segment operation, prepare:

```c
io_uring_prep_cancel64(
    sqe,
    llam_io_udata_encode(&segment->tokens[i],
                         LLAM_IO_UDATA_NATIVE_SEGMENT),
    0);
io_uring_sqe_set_data64(
    sqe,
    llam_io_udata_encode(&segment->cancel_tokens[i],
                         LLAM_IO_UDATA_NATIVE_CANCEL));
```

Initialize all cancel-token generations before publication. Track
`cancel_sqes_prepared` and `cancel_cqes_observed`; a cancel result never marks
target retirement.

- [ ] **Step 5: Route generic aborts by request ownership**

In both `SUBMIT_QUEUE` and `INFLIGHT` branches of
`llam_abort_io_wait_impl()`:

```c
#if LLAM_RUNTIME_BACKEND_LINUX
batch = req->linux_native_batch;
if (batch != NULL) {
    if (mode == LLAM_IO_WAIT_MODE_SUBMIT_QUEUE) {
        removed = llam_linux_native_batch_abort_queued(
            node, batch, req);
        if (removed) {
            break;
        }
        continue;
    }
    atomic_store_explicit(&req->abort_reason, reason,
                          memory_order_release);
    (void)llam_linux_native_batch_request_cancel(
        node, batch, req);
    goto release_req;
}
#endif
```

Do not set or queue the ordinary `REQ_CANCEL` state for native tickets.

- [ ] **Step 6: Drain operations before cancellations**

In `llam_io_submit_batch()`, detach both lists under their respective helper
locks, submit every native operation batch first, then every native cancel
batch, then ordinary requests. Include both SQE kinds in `submitted`.

- [ ] **Step 7: Complete only fully reusable batches**

Both operation and cancel handlers call one internal
`llam_linux_native_batch_maybe_complete()`. It requires:

```c
all_segments_target_retired &&
(!cancel_requested ||
 cancel_cqes_observed == cancel_sqes_prepared)
```

The winner CASes `terminal_claimed`, stores `RETIRED`, clears
`req->linux_native_batch`, increments the first segment's legacy
`terminal_wakes` counter once, and calls `llam_io_complete_req(..., true)`
once. Per-segment activation/result counters remain per segment; the new batch
metrics are the authority for actual publications, parks, and wakes. This keeps
width-one metrics backward compatible without inventing `B` task wakes for a
width-`B` ticket.

Delete the existing `task->cancel_token != NULL -> ENOTSUP` rejection from the
native issue path. `llam_park_io_req()` then registers and unregisters task
cancellation exactly as it does for ordinary I/O.

- [ ] **Step 8: Run cancellation, sanitizer, and shutdown tests**

Run:

```bash
make -j4 test_leir_native_linux test_runtime_shutdown_internal
./test_leir_native_linux
./test_runtime_shutdown_internal
cmake -S . -B object/leir-cancel-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build object/leir-cancel-asan \
  --target test_leir_native_linux test_runtime_shutdown_internal -j4
ASAN_OPTIONS=detect_leaks=1 \
  ./object/leir-cancel-asan/test_leir_native_linux
```

Expected: all pass with no leaks, stale-token abort, duplicate wake, or pending
owner.

- [ ] **Step 9: Commit**

```bash
git add src/io/linux/watch/linux_segment_cancel.c \
  src/io/linux/runtime_io_segment_linux_internal.h \
  src/internal/runtime_state.h src/core/base/io_udata.c \
  src/internal/runtime_types.h src/internal/runtime_proto_io.h \
  src/io/linux/runtime_io_watch_linux_internal.h \
  src/core/wait/wait_tracking.c src/io/linux/watch/linux_submit.c \
  src/io/linux/watch/cqe.c CMakeLists.txt Makefile \
  experiments/leir/test_leir_native_linux.c
git commit -m "feat: cancel and retire native segment batches"
```

---

### Task 5: Expose width-N instance execution

**Files:**
- Modify: `experiments/leir/leir_native_segment.h`
- Modify: `experiments/leir/leir_native_segment.c`
- Modify: `experiments/leir/test_leir_native_segment.c`

**Interfaces:**
- Produces:

```c
typedef struct leir_native_batch_metrics {
    uint64_t activations;
    uint64_t segments;
    uint64_t queue_publications;
    uint64_t task_parks;
    uint64_t terminal_wakes;
    uint64_t operation_sqes;
    uint64_t operation_cqes;
    uint64_t cancel_sqes;
    uint64_t cancel_cqes;
    uint64_t hot_allocations;
} leir_native_batch_metrics_t;

int leir_native_batch_run(
    leir_native_instance_t *const *instances,
    leir_phase0_value_t *const *values_out,
    const size_t *value_counts,
    leir_native_metrics_t *metrics_out,
    size_t instance_count,
    leir_native_batch_metrics_t *batch_metrics_out);
```

- [ ] **Step 1: Write failing validation and metric tests**

Test `instance_count` 0 and 9, duplicate instance pointers, mixed mode, one
unbound instance, and a busy second instance. After each rejection, assert all
previously acquired instances returned to `IDLE`.

For width two, use two socket pairs and assert:

```c
CHECK(batch_metrics.segments == 2U);
CHECK(batch_metrics.queue_publications == 1U);
CHECK(batch_metrics.task_parks == 1U);
CHECK(batch_metrics.terminal_wakes == 1U);
CHECK(first_metrics.activations == 1U);
CHECK(second_metrics.activations == 1U);
```

- [ ] **Step 2: Run the instance test and observe the missing API**

Run:

```bash
make -j4 test_leir_native_segment
./test_leir_native_segment
```

Expected: compile failure for `leir_native_batch_run`.

- [ ] **Step 3: Acquire instances all-or-nothing**

Validate arrays and counts first. CAS every instance
`IDLE -> RUNNING`; on failure, restore acquired instances in reverse order.
Require one current task/runtime/node and acquire its embedded request once.
Increment `hot_allocations` and return `EAGAIN` if request acquisition returns
anything other than the embedded request.

- [ ] **Step 4: Build and issue one stack-local batch**

Increment every segment generation, set owner runtime, initialize a
`llam_linux_native_batch_t`, prepare the request from the last operation of the
last segment, and call `llam_issue_linux_native_batch()`.

After return, require every segment and the batch to be `RETIRED` or locally
`IDLE`; publish each instance's result slots and metrics, clear ownership
pointers, reset segments to `IDLE`, release the request, and restore instance
activity.

- [ ] **Step 5: Make width one a wrapper**

```c
return leir_native_batch_run(
    &instance, &values_out, &value_count, metrics_out, 1U,
    &ignored_batch_metrics);
```

Preserve the existing errno and output behavior.

- [ ] **Step 6: Run portable, integration, and metric tests**

Run:

```bash
make -j4 test_leir_native_segment test_leir_native_linux
./test_leir_native_segment
./test_leir_native_linux
```

Expected: pass on Linux; non-Linux keeps returning `ENOTSUP` from execution
while validation and compilation tests pass.

- [ ] **Step 7: Commit**

```bash
git add experiments/leir/leir_native_segment.h \
  experiments/leir/leir_native_segment.c \
  experiments/leir/test_leir_native_segment.c
git commit -m "feat: execute multiple LEIR segments per task ticket"
```

---

### Task 6: Create bounded sparse fixed-resource arenas

**Files:**
- Create: `src/io/linux/watch/linux_segment_resources.c`
- Modify: `src/io/linux/runtime_io_segment_linux_internal.h`
- Modify: `src/internal/runtime_types.h`
- Modify: `src/internal/runtime_proto_io.h`
- Modify: `src/io/linux/runtime_io_watch_linux_internal.h`
- Modify: `src/core/lifecycle/init.c`
- Modify: `src/core/lifecycle/shutdown.c`
- Modify: `src/io/engine/io_engine.c`
- Modify: `CMakeLists.txt`
- Modify: `Makefile`
- Modify: `experiments/leir/test_leir_native_linux.c`

**Interfaces:**
- Produces:

```c
#define LLAM_LINUX_NATIVE_FIXED_FILE_SLOTS 64U
#define LLAM_LINUX_NATIVE_FIXED_BUFFER_SLOTS 64U

typedef struct llam_linux_native_resource_lease {
    unsigned file_slots[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned buffer_slots[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned file_count;
    unsigned buffer_count;
    unsigned node_index;
    bool attached;
} llam_linux_native_resource_lease_t;

int llam_linux_native_resources_setup(llam_node_t *node);
void llam_linux_native_resources_before_ring_exit(llam_node_t *node);
void llam_linux_native_resources_after_ring_exit(llam_node_t *node);
int llam_linux_native_resources_attach(
    llam_node_t *node, const int *fds, unsigned fd_count,
    const struct iovec *buffers, unsigned buffer_count,
    llam_linux_native_resource_lease_t *lease);
int llam_linux_native_resources_detach(
    llam_node_t *node, llam_linux_native_resource_lease_t *lease);
```

For deterministic failure injection, `llam_node_t` gains:

```c
int (*native_files_update_override)(
    llam_node_t *node, unsigned off, const int *files,
    unsigned count, void *arg);
int (*native_buffers_update_override)(
    llam_node_t *node, unsigned off, const struct iovec *buffers,
    unsigned count, void *arg);
void *native_resource_update_override_arg;
```

Production helpers call liburing when the corresponding pointer is null.

- [ ] **Step 1: Write pure slot and rollback tests**

Use a node fixture whose update hooks count calls and fail at a selected index.
Assert:

```c
CHECK(lease.file_count == 2U);
CHECK(lease.buffer_count == 2U);
CHECK(popcount64(node.native_fixed_file_bitmap) == 2U);
CHECK(popcount64(node.native_fixed_buffer_bitmap) == 2U);
CHECK(llam_linux_native_resources_detach(&node, &lease) == 0);
CHECK(node.native_fixed_file_bitmap == 0U);
CHECK(node.native_fixed_buffer_bitmap == 0U);
```

For failures after the first file update and first buffer update, assert every
updated slot is cleared, both bitmaps return to zero, and the lease is empty.
Fill all 64 bits and require `ENOSPC`.

- [ ] **Step 2: Run and observe missing arena APIs**

Run:

```bash
make -j4 test_leir_native_linux
./test_leir_native_linux --unit-only
```

Expected: compile failure for resource types and helpers.

- [ ] **Step 3: Implement setup and capability isolation**

Initialize `node->native_resource_lock` before ring setup. After ring probing:

```c
files_rc = io_uring_register_files_sparse(
    &node->ring, LLAM_LINUX_NATIVE_FIXED_FILE_SLOTS);
buffers_rc = io_uring_register_buffers_sparse(
    &node->ring, LLAM_LINUX_NATIVE_FIXED_BUFFER_SLOTS);
node->supports_native_fixed_files = files_rc == 0;
node->supports_native_fixed_buffers = buffers_rc == 0;
```

If the second registration fails, unregister the first and disable both fixed
capabilities. Return zero so ordinary runtime initialization remains usable.

- [ ] **Step 4: Implement atomic lease/update/rollback**

Under `native_resource_lock`, reserve free bits first, then call
`io_uring_register_files_update()` and
`io_uring_register_buffers_update_tag(..., NULL, 1U)` per reserved slot.
Require each success result to equal 1. On failure, update successful file
slots to `-1`, successful buffer slots to a zero `iovec`, clear all reserved
bits, zero the lease, restore `errno = -rc`, and unlock.

- [ ] **Step 5: Implement ring-lifetime teardown**

Before `io_uring_queue_exit()`, unregister sparse buffer and file tables when
registered. Record deferred fatal state only for errors other than
`-ENXIO`/`-EINVAL` during partial initialization. After ring exit, clear
capability flags/bitmaps. Destroy the resource mutex only after this cleanup.

- [ ] **Step 6: Run unit, lifecycle, and real Linux feature tests**

Run:

```bash
make -j4 test_leir_native_linux test_runtime_shutdown_internal
./test_leir_native_linux --unit-only
./test_leir_native_linux --dispatch
./test_runtime_shutdown_internal
```

Expected: unit/lifecycle tests pass. On kernels before 5.19 or constrained
registrations, the real fixed feature probe reports unavailable while normal
native tests still pass.

- [ ] **Step 7: Commit**

```bash
git add src/io/linux/watch/linux_segment_resources.c \
  src/io/linux/runtime_io_segment_linux_internal.h \
  src/internal/runtime_types.h src/internal/runtime_proto_io.h \
  src/io/linux/runtime_io_watch_linux_internal.h \
  src/core/lifecycle/init.c src/core/lifecycle/shutdown.c \
  src/io/engine/io_engine.c CMakeLists.txt Makefile \
  experiments/leir/test_leir_native_linux.c
git commit -m "feat: manage native io_uring fixed resources"
```

---

### Task 7: Attach fixed instances and encode fixed RECV/fixed-fd SEND

**Files:**
- Modify: `src/io/linux/runtime_io_segment_linux_internal.h`
- Modify: `src/io/linux/watch/linux_segment.c`
- Modify: `experiments/leir/leir_native_segment.h`
- Modify: `experiments/leir/leir_native_segment.c`
- Modify: `experiments/leir/test_leir_native_linux.c`
- Modify: `experiments/leir/test_leir_native_segment.c`

**Interfaces:**
- Adds modes:

```c
LEIR_NATIVE_MODE_FIXED_LINK = 2,
LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP = 3,
```

- Adds operation flags:

```c
LLAM_LINUX_NATIVE_OP_FIXED_FILE = 1U << 0,
LLAM_LINUX_NATIVE_OP_FIXED_RECV_BUFFER = 1U << 1,
```

- [ ] **Step 1: Write fixed-SQE encoding tests**

Configure RECV then SEND with `fixed_file_slot = 7` and RECV
`fixed_buffer_slot = 9`. Assert:

```c
CHECK(recv_sqe.opcode == IORING_OP_READ_FIXED);
CHECK(recv_sqe.fd == 7);
CHECK(recv_sqe.buf_index == 9U);
CHECK(recv_sqe.off == UINT64_MAX);
CHECK((recv_sqe.flags & IOSQE_FIXED_FILE) != 0U);

CHECK(send_sqe.opcode == IORING_OP_SEND);
CHECK(send_sqe.fd == 7);
CHECK(send_sqe.msg_flags == (uint32_t)MSG_NOSIGNAL);
CHECK((send_sqe.flags & IOSQE_FIXED_FILE) != 0U);
```

Also assert link/CQE-skip bits coexist with `IOSQE_FIXED_FILE`.

- [ ] **Step 2: Write fixed scratch copy and detach tests**

Bind RECV → SEND to one external mutable buffer. After mocked attachment,
write a peer payload into the fixed scratch buffer, retire the segment, and
assert `values_out[buffer_slot]` points to the original external buffer whose
bytes now equal the payload.

Destroy after retirement and assert file/buffer slots are cleared and each
pinned fd is closed. Destroy while batch state is `INFLIGHT` must return
`EBUSY` without releasing anything.

- [ ] **Step 3: Run tests and observe normal RECV/SEND encoding**

Run:

```bash
make -j4 test_leir_native_linux test_leir_native_segment
./test_leir_native_linux --unit-only
./test_leir_native_segment
```

Expected: fixed-mode symbols are missing and current RECV encodes
`IORING_OP_RECV`.

- [ ] **Step 4: Allocate cold scratch buffers during bind**

Deduplicate by LEIR `buffer_slot`, not raw pointer. Allocate each scratch region
with `posix_memalign(&ptr, page_size, rounded_size)` and zero it. Store external
pointer, logical size, scratch pointer, and whether any compiled RECV writes
the slot. Allocation failure closes newly duplicated fds and frees every
scratch region before returning.

Before a successful rebind replaces pinned fds or scratch buffers, detach an
existing fixed lease while its owner ring is live. If the instance is attached
and its batch is not fully retired, return `EBUSY` without changing the old
binding. Destroy follows the same detach-before-close order.

- [ ] **Step 5: Attach on first fixed activation**

Require both node fixed capabilities. Build distinct pinned-fd and scratch
`iovec` arrays, call `llam_linux_native_resources_attach()`, then rewrite every
operation's fd/index/address/flag fields from the lease. Persist runtime and
node index; later execution on another node returns `EXDEV`.

- [ ] **Step 6: Copy at explicit ownership boundaries**

Before publication, copy external bytes to scratch for a buffer whose first
use is SEND. After full batch retirement, copy scratch back for every
RECV-written buffer. Never copy while segment tokens are kernel-reachable.

- [ ] **Step 7: Encode the hybrid fixed path**

Use:

```c
io_uring_prep_read_fixed(
    sqe, op->fixed_file_slot, op->buffer, op->length,
    UINT64_MAX, op->fixed_buffer_slot);
```

for fixed RECV, and:

```c
io_uring_prep_send(
    sqe, op->fixed_file_slot, op->buffer, op->length,
    MSG_NOSIGNAL);
```

for fixed SEND. OR `IOSQE_FIXED_FILE` with link and skip flags.

- [ ] **Step 8: Run unit, integration, ASan, and fd-reuse tests**

Run:

```bash
make -j4 test_leir_native_linux test_leir_native_segment
./test_leir_native_linux
./test_leir_native_segment
cmake --build object/leir-cancel-asan \
  --target test_leir_native_linux test_leir_native_segment -j4
ASAN_OPTIONS=detect_leaks=1 \
  ./object/leir-cancel-asan/test_leir_native_segment
```

Expected: fixed integration either succeeds with exact echo bytes or reports
feature-unavailable; no fallback is mislabeled as fixed.

- [ ] **Step 9: Commit**

```bash
git add src/io/linux/runtime_io_segment_linux_internal.h \
  src/io/linux/watch/linux_segment.c \
  experiments/leir/leir_native_segment.h \
  experiments/leir/leir_native_segment.c \
  experiments/leir/test_leir_native_linux.c \
  experiments/leir/test_leir_native_segment.c
git commit -m "feat: lower LEIR receive-send through fixed resources"
```

---

### Task 8: Stress real cancellation and batch ownership

**Files:**
- Modify: `experiments/leir/test_leir_native_segment.c`
- Modify: `experiments/leir/test_leir_native_linux.c`
- Modify: `tests/test_runtime_shutdown_internal.c`

**Interfaces:**
- Consumes: task cancel tokens, `leir_native_batch_run()`, and fixed/non-fixed
  RECV → SEND instances.
- Produces: integration proof for queued, in-flight, natural-completion, and
  runtime-stop races.

- [ ] **Step 1: Add cancel-token integration fixtures**

For 2 and 8 segments, spawn a task with a cancel token, delay peer input, cancel
the token, and assert:

```c
CHECK(llam_join(task) == 0);
CHECK(state.run_result == -1);
CHECK(state.run_errno == ECANCELED);
CHECK(state.batch_metrics.terminal_wakes == 1U);
CHECK(runtime_pending_ops_are_zero());
CHECK(runtime_active_io_waiters_are_zero());
```

Repeat with peer completion released immediately before cancellation; accept
either full success or `ECANCELED`, but require exact bytes on success and no
live ownership in both outcomes.

- [ ] **Step 2: Add randomized reducer interleavings**

For 10,000 deterministic PRNG seeds, shuffle unique target/cancel CQEs while
keeping the final target present. Assert one terminal claim, exact first
non-cancel error, no duplicate token acceptance, and reuse only after both
retirement predicates.

- [ ] **Step 3: Add shutdown teardown coverage**

Publish an in-flight native batch, request runtime stop, and assert ring teardown
occurs before fixed scratch/fd cleanup. Use existing shutdown test hooks to
record:

```c
CHECK(events.ring_exit < events.instance_storage_release);
CHECK(node.pending_ops == 0U);
CHECK(node.native_batch_head == NULL);
CHECK(node.native_cancel_head == NULL);
```

- [ ] **Step 4: Run stress repeatedly**

Run:

```bash
make -j4 test_leir_native_segment test_leir_native_linux \
  test_runtime_shutdown_internal
for i in 1 2 3 4 5; do
  ./test_leir_native_segment || exit 1
  ./test_leir_native_linux || exit 1
  ./test_runtime_shutdown_internal || exit 1
done
```

Expected: all 15 executions pass.

- [ ] **Step 5: Commit**

```bash
git add experiments/leir/test_leir_native_segment.c \
  experiments/leir/test_leir_native_linux.c \
  tests/test_runtime_shutdown_internal.c
git commit -m "test: stress native batch cancellation ownership"
```

---

### Task 9: Add the connected-pipeline benchmark

**Files:**
- Create: `experiments/leir/bench_leir_native_pipeline.c`
- Modify: `experiments/leir/leir_peer_process.c`
- Modify: `experiments/leir/leir_peer_process.h`
- Modify: `CMakeLists.txt`
- Modify: `Makefile`

**Interfaces:**
- Produces binary:

```text
bench_leir_native_pipeline
  --candidate link|link_skip|fixed_link_skip
  --batch-width 1|2|4|8
  --concurrency 1|4|16
  --payload 64|512|4096
  --activations N
  --min-mode-ms N
  --order ABBA|BAAB
```

- Produces one bounded machine-readable `RESULT` line containing:

```text
baseline_wall_ns candidate_wall_ns baseline_cpu_ns candidate_cpu_ns
baseline_p99_ns candidate_p99_ns activations segments logical_ops
batch_width queue_publications task_parks terminal_wakes
operation_sqes operation_cqes suppressed_success_cqes
cancel_sqes cancel_cqes fixed_file_attachments fixed_buffer_attachments
submit_calls submit_syscalls checksum status
```

- [ ] **Step 1: Write CLI rejection and smoke behavior first**

Implement parser tests through CTest/Python contract in Task 10, then make the
binary return 2 for invalid width/payload/count and 77 when the requested fixed
feature is unavailable. The default no-argument smoke uses link, width 1,
concurrency 1, payload 64, and 8 activations.

- [ ] **Step 2: Build and observe the missing target**

Run:

```bash
make -j4 bench_leir_native_pipeline
```

Expected: no rule for `bench_leir_native_pipeline`.

- [ ] **Step 3: Implement peer-driven echo transactions**

For every activation, the external peer writes one exact message to each
connection in the group, reads the echo, and checks the deterministic payload.
The public baseline task calls LLAM read then write for each connection. The
candidate task calls `leir_native_batch_run()` across `batch_width` bound
instances.

Group `concurrency` connections into `ceil(concurrency / batch_width)` tasks.
The final group may use a smaller ticket. Count one terminal latency per batch,
and calculate p99 after balanced blocks.

- [ ] **Step 4: Include cold setup outside measured activation blocks**

Initialize/bind instances and run one warmup echo per candidate instance before
starting wall/CPU counters. Reset metrics after warmup. Destroy/detach fixed
instances before `llam_runtime_shutdown()` so sparse slots are released while
the ring is valid.

- [ ] **Step 5: Enforce structural integrity**

Reject the block unless:

```c
queue_publications == batch_activations &&
task_parks == batch_activations &&
terminal_wakes == batch_activations &&
segments == activations &&
hot_allocations == 0U &&
pending_ops_are_zero() &&
active_io_waiters_are_zero() &&
peer_checksum == runtime_checksum;
```

For width `B`, require `batch_activations == ceil(activations / B)` for the
generated work distribution.

- [ ] **Step 6: Add Make/CMake targets and smoke CTest**

Link the same LEIR support objects as `bench_leir_native_segment`. Mark return
77 as CTest skip. Add the target to clean, Windows compile-only, and test target
lists without attempting Linux execution on non-Linux hosts.

- [ ] **Step 7: Run smoke candidates**

Run on Linux:

```bash
./bench_leir_native_pipeline \
  --candidate link_skip --batch-width 1 \
  --concurrency 4 --payload 64 --activations 32 \
  --min-mode-ms 1 --order ABBA
./bench_leir_native_pipeline \
  --candidate fixed_link_skip --batch-width 4 \
  --concurrency 4 --payload 512 --activations 32 \
  --min-mode-ms 1 --order BAAB
```

Expected: status `OK` with exact structural counters, or return 77 only for an
unsupported fixed kernel/resource feature.

- [ ] **Step 8: Commit**

```bash
git add experiments/leir/bench_leir_native_pipeline.c \
  experiments/leir/leir_peer_process.c \
  experiments/leir/leir_peer_process.h CMakeLists.txt Makefile
git commit -m "bench: measure connected native segment pipelines"
```

---

### Task 10: Add statistical evidence and decision gates

**Files:**
- Create: `scripts/bench_leir_native_pipeline.py`
- Create: `scripts/test_bench_leir_native_pipeline.py`
- Modify: `CMakeLists.txt`
- Modify: `Makefile`

**Interfaces:**
- Produces:
  `raw.csv`, `summary.csv`, `leir_native_pipeline_report.md`,
  `leir_native_pipeline_tracked_report.md`, and verdict
  `SPECIALIZED`, `INCONCLUSIVE`, or `REJECT`.

- [ ] **Step 1: Write classifier tests**

Construct synthetic samples and assert:

```python
self.assertEqual(classify(two_regions_over_5_percent), "SPECIALIZED")
self.assertEqual(classify(one_region_only), "INCONCLUSIVE")
self.assertEqual(classify(cpu_regression_over_3_percent), "REJECT")
self.assertEqual(classify(p99_regression_over_10_percent), "REJECT")
self.assertEqual(classify(no_fixed_or_batch_win), "INCONCLUSIVE")
```

Add parser tests for missing keys, duplicate keys, non-integers, negative
durations, output over 64 KiB, timeout, return 77, and inconsistent structural
counters.

- [ ] **Step 2: Run and observe the missing module**

Run:

```bash
python3 -m unittest scripts/test_bench_leir_native_pipeline.py -v
```

Expected: import failure.

- [ ] **Step 3: Implement bounded execution and parsing**

Use `scripts.process_utils.run_capture()` with a 120-second per-process timeout
and 64-KiB stdout/stderr caps. Accept exactly one `RESULT` line and the exact
key set from Task 9. Preserve raw stdout/stderr for every failed cell.

- [ ] **Step 4: Implement the matrix and confidence intervals**

Run payloads `64,512,4096`, concurrency `1,4,16`, candidates
`link_skip,fixed_link_skip`, widths `1,2,4,8`, both balanced orders, one warmup,
and at least 9 measured repetitions. Compute paired log ratios and bootstrap a
95% interval with a fixed seed.

- [ ] **Step 5: Implement exact gates**

`SPECIALIZED` requires:

```python
len(regions_with_wall_lcb_at_least_5_percent) >= 2
and median_cpu_ratio <= 1.03
and median_p99_ratio <= 1.10
and any(fixed_ratio < 1.0 for fixed_ratio in fixed_vs_nonfixed)
and any(batch_ratio < 1.0 for batch_ratio in width4_or_8_vs_width1)
and all(structural_ratios_match)
```

Use `REJECT` for statistically supported wall regression or correctness/gate
failure; otherwise use `INCONCLUSIVE`.

- [ ] **Step 6: Keep portable and Linux reports separate**

The report title and metadata must say `Linux/io_uring specialized evidence`.
Do not merge rows into Phase 0A CSV or reuse its verdict.

- [ ] **Step 7: Run Python and CTest contracts**

Run:

```bash
python3 -m unittest scripts/test_bench_leir_native_pipeline.py -v
cmake -S . -B object/leir-pipeline-contract
cmake --build object/leir-pipeline-contract \
  --target bench_leir_native_pipeline -j4
ctest --test-dir object/leir-pipeline-contract --output-on-failure \
  -R "leir_native_pipeline"
```

Expected: pass; unsupported Linux fixed mode is a skip, not fabricated data.

- [ ] **Step 8: Commit**

```bash
git add scripts/bench_leir_native_pipeline.py \
  scripts/test_bench_leir_native_pipeline.py \
  CMakeLists.txt Makefile
git commit -m "test: gate native pipeline performance evidence"
```

---

### Task 11: Wire Linux CI, sanitizers, stress, and artifacts

**Files:**
- Modify: `.github/workflows/leir-native-research.yml`
- Modify: `scripts/verify_linux.sh`
- Modify: `docs/operations/benchmarks.md`

**Interfaces:**
- Consumes: targets and scripts from Tasks 1–10.
- Produces: branch-protection test coverage and retained pipeline evidence.

- [ ] **Step 1: Expand workflow path filters and environment capture**

Add both new Python scripts and the benchmark source through existing
`experiments/leir/**`. Record:

```bash
uname -a
ulimit -l
pkg-config --modversion liburing
cat /proc/sys/kernel/io_uring_disabled
```

- [ ] **Step 2: Add correctness build and test commands**

The normal job runs:

```bash
make -j2 test_leir_native_plan test_leir_native_segment \
  test_leir_native_linux bench_leir_native_pipeline
./test_leir_native_plan
./test_leir_native_segment
./test_leir_native_linux
python3 -m unittest scripts/test_bench_leir_native_pipeline.py -v
```

- [ ] **Step 3: Add sanitizer and cancellation stress**

Build ASan/UBSan and TSan versions of the three native tests and pipeline smoke.
Run the cancellation integration five times under ASan/UBSan and once under
TSan. Preserve sanitizer logs even on failure.

- [ ] **Step 4: Add bounded evidence execution**

Invoke:

```bash
python3 scripts/bench_leir_native_pipeline.py \
  --binary ./bench_leir_native_pipeline \
  --output-dir "$OUT_DIR/pipeline" \
  --tracked-report "$OUT_DIR/leir_native_pipeline_tracked_report.md"
```

Upload raw CSV, summary CSV, both reports, environment, feature probe, and all
logs with `if: always()`.

- [ ] **Step 5: Document claim boundaries**

In `docs/operations/benchmarks.md`, add the command, artifact names, exact
verdict meanings, fixed-mode kernel/resource skip behavior, and the statement
that Linux results do not establish a portable LLAM speedup.

- [ ] **Step 6: Run local workflow-equivalent checks**

Run:

```bash
python3 -m unittest scripts/test_bench_leir_native.py \
  scripts/test_bench_leir_native_pipeline.py -v
make -j4 test-leir-native
git diff --check
```

Expected: pass.

- [ ] **Step 7: Commit**

```bash
git add .github/workflows/leir-native-research.yml \
  scripts/verify_linux.sh docs/operations/benchmarks.md
git commit -m "ci: validate native pipeline ownership and evidence"
```

---

### Task 12: Security review, full verification, evidence, and release decision

**Files:**
- Modify only files required by validated findings.
- Update: PR body and evidence artifacts through GitHub, not tracked benchmark
  raw data unless repository policy requires it.

**Interfaces:**
- Consumes: complete implementation.
- Produces: validated branch, pushed commits, green CI, exact verdict, and a
  release only when all release gates pass.

- [ ] **Step 1: Run focused static checks**

Run:

```bash
rg -n "malloc|calloc|realloc|free" \
  src/io/linux/watch/linux_segment.c \
  src/io/linux/watch/linux_segment_cancel.c
rg -n "WRITE_FIXED|SEND_ZC" \
  src/io/linux experiments/leir
git diff --check origin/leir-native-segment...HEAD
```

Expected: no hot-path allocation and no prohibited fixed send encoding.

- [ ] **Step 2: Run the standard repository security scan**

Use the normal single-pass security scan, validate every candidate against an
attacker-controlled lifetime or input path, fix validated/plausible findings,
and rerun their regression tests. Pay particular attention to stale
generation tokens, cancel/control result confusion, sparse-slot rollback,
integer multiplication, output bounds, and runtime teardown.

- [ ] **Step 3: Run full local verification**

Run:

```bash
make clean
make -j4
make test
python3 -m unittest discover -s scripts -p 'test_*.py' -v
cmake -S . -B object/leir-pipeline-release \
  -DCMAKE_BUILD_TYPE=Release
cmake --build object/leir-pipeline-release -j4
ctest --test-dir object/leir-pipeline-release \
  --output-on-failure
```

Expected: every applicable test passes.

- [ ] **Step 4: Push and monitor CI**

```bash
git status --short
git push origin leir-native-segment
gh pr checks 3 --watch
```

Do not interpret the docs deploy skip as a failure. Any failing applicable
check is fixed and repushed before continuing.

- [ ] **Step 5: Collect and audit Linux evidence**

Download the workflow artifact and verify:

```bash
test "$(cut -d= -f2 artifact/source.txt | head -1)" = \
  "$(git rev-parse HEAD)"
python3 scripts/bench_leir_native_pipeline.py \
  --audit-existing artifact/pipeline
```

Record exact matrix cell counts, feature availability, wall/CPU/p99 confidence
intervals, fixed-vs-nonfixed regions, batch-vs-width-one regions, CQE counts,
and structural ratios in the PR body.

- [ ] **Step 6: Apply the release gate literally**

If verdict is not `SPECIALIZED`, leave the project version unchanged, keep PR
draft/experimental, and report `INCONCLUSIVE` or `REJECT` with the limiting
gates.

If and only if verdict is `SPECIALIZED`, full CI is green, and the security
scan has no unresolved validated/plausible finding:

```bash
rg -n "2\\.2\\.0|v2\\.2\\.0|LLAM_VERSION" \
  CMakeLists.txt Makefile include src scripts .github README.md docs tests
```

Prepare `2.3.0` while keeping ABI `2.0`. Update version/package/install
references in `CMakeLists.txt`, `Makefile`, `include/llam/runtime.h`,
`src/core/base/abi.c`, `scripts/generate_sdk_metadata.sh`,
`scripts/package_release.sh`, `scripts/install.sh`, `scripts/install.ps1`,
`scripts/package_release_windows.ps1`, `.github/workflows/release.yml`,
`tests/test_shared_load.c`, `README.md`, `docs/abi.md`,
`docs/getting-started.md`, `docs/security.md`, and `CHANGELOG.md`. Run the full
verification again and commit:

```bash
git add CMakeLists.txt Makefile include/llam/runtime.h \
  src/core/base/abi.c scripts/generate_sdk_metadata.sh \
  scripts/package_release.sh scripts/install.sh scripts/install.ps1 \
  scripts/package_release_windows.ps1 .github/workflows/release.yml \
  tests/test_shared_load.c README.md docs/abi.md \
  docs/getting-started.md docs/security.md CHANGELOG.md
git commit -m "release: prepare 2.3.0"
git push origin leir-native-segment
gh pr checks 3 --watch
```

After the exact release-preparation commit is green, create tag/release
`v2.3.0` through the repository's release workflow and verify that every
published archive, installer, checksum, and source archive names `2.3.0`.

- [ ] **Step 7: Final handoff**

Report:

- implementation and ownership invariants;
- commits and PR URL;
- exact CI results;
- security scan result;
- Linux specialized verdict with confidence intervals;
- portable-vs-platform claim boundary;
- release/tag URL only if the release gate passed.
