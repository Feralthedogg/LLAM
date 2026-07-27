# LEIR Linux Native Segment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compile the approved static LEIR subset into one Linux `io_uring` linked segment, optionally suppress successful intermediate CQEs, and measure it against ordinary LLAM task I/O without changing the public ABI.

**Architecture:** A portable planner reduces an already validated Phase 0 program to at most eight immutable RECV/SEND steps. A Linux-private segment object binds concrete `SOCK_SEQPACKET` resources, enters the existing node worker through a dedicated intrusive queue, prepares the complete linked chain in one ring batch, and completes one existing task request only at the terminal observation. A paired process-isolated benchmark reports link-only mechanics separately from link-plus-skip performance.

**Tech Stack:** C11, LLAM internal task/I/O ownership protocol, liburing/io_uring, Unix `SOCK_SEQPACKET`, GNU Make, CMake/CTest, Python 3 `unittest`, GitHub Actions on Ubuntu 24.04.

## Global Constraints

- Keep LEIR as the compiler/planner semantic contract; never dispatch another LEIR node from a CQE.
- Accept exactly 1, 2, 4, or 8 alternating `READ_EXACT`/`WRITE_ALL` steps.
- Reject branches, cycles, dynamic result dependencies, timeout, cancellation, fork/join, callbacks, parsers, and native escapes.
- Bind only connected Unix `SOCK_SEQPACKET` descriptors and lengths in `[1, UINT_MAX]`.
- Map reads to `IORING_OP_RECV`, writes to `IORING_OP_SEND`, and writes to `MSG_NOSIGNAL`.
- Link every non-final SQE with `IOSQE_IO_LINK`; add `IOSQE_CQE_SKIP_SUCCESS` only to non-final SQEs in skip mode.
- Require `IORING_FEAT_CQE_SKIP` for skip mode and retain link-only mode when it is absent.
- Publish one backend queue item, count one `pending_ops` owner, park once, and wake once per segment.
- Allocate no memory between timed activation start and terminal return.
- Keep all new runtime entry points hidden and all installed headers unchanged.
- Make non-Linux behavior an explicit `ENOTSUP` stub with no runtime behavior change.
- Classify positive evidence as `SPECIALIZED`, never `CATEGORY`.
- Do not change version `2.2.0`, create a release, or merge the research branch.

---

### Task 1: Portable Native Planner

**Files:**
- Create: `experiments/leir/leir_native_plan.h`
- Create: `experiments/leir/leir_native_plan.c`
- Create: `experiments/leir/test_leir_native_plan.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `leir_phase0_program_t`, `leir_phase0_node_desc_t`, and slot metadata from `experiments/leir/leir_phase0_internal.h`.
- Produces:

```c
#define LEIR_NATIVE_MAX_OPS 8U

typedef enum leir_native_step_kind {
    LEIR_NATIVE_STEP_RECV = 0,
    LEIR_NATIVE_STEP_SEND = 1,
} leir_native_step_kind_t;

typedef struct leir_native_step {
    uint16_t kind;
    uint16_t fd_slot;
    uint16_t buffer_slot;
    uint16_t length_slot;
    uint16_t result_slot;
} leir_native_step_t;

typedef struct leir_native_plan {
    leir_native_step_t steps[LEIR_NATIVE_MAX_OPS];
    uint16_t step_count;
    uint16_t return_node;
    uint16_t result_slot;
} leir_native_plan_t;

int leir_native_plan_compile(
    const leir_phase0_program_t *program,
    leir_native_plan_t *out);
```

- [ ] **Step 1: Write planner acceptance and exact-lowering tests**

Create table-driven tests named:

```c
static int test_compile_exact_linear_lengths(void);
static int test_compile_can_start_with_send(void);
static int test_compile_copies_exact_slot_indices(void);
```

For operation counts `1, 2, 4, 8`, construct Phase 0 programs whose success path alternates exact read/all-write nodes and ends in `RETURN`; both failure edges of every effect end in a terminal `FAIL`. Assert `step_count`, `return_node`, `result_slot`, every step kind, and all four slot indices.

- [ ] **Step 2: Write planner rejection tests**

Create one descriptor fixture per rejection and assert `-1` with `errno == EINVAL`:

```c
static int test_rejects_non_power_of_two_operation_count(void);
static int test_rejects_read_and_write_non_exact_opcodes(void);
static int test_rejects_non_alternating_steps(void);
static int test_rejects_success_cycle(void);
static int test_rejects_success_branch_to_fail(void);
static int test_rejects_nonterminal_eof_or_error_edge(void);
static int test_rejects_prior_result_as_later_length(void);
static int test_rejects_prior_result_as_return_control_dependency(void);
```

The dynamic dependency fixture gives the first effect an `I64` result slot and reuses that slot as the second effect's length slot.

- [ ] **Step 3: Run the new test and observe the red state**

Run:

```bash
make -j4 test_leir_native_plan
```

Expected: build failure because `leir_native_plan_compile` and the new target do not exist.

- [ ] **Step 4: Implement the minimal deterministic compiler**

Walk only `program->entry_node` and `on_success`, with a fixed 32-entry visited bitmap. For each effect:

```c
switch (node->opcode) {
case LEIR_PHASE0_OP_READ_EXACT:
    kind = LEIR_NATIVE_STEP_RECV;
    break;
case LEIR_PHASE0_OP_WRITE_ALL:
    kind = LEIR_NATIVE_STEP_SEND;
    break;
default:
    return fail_plan(EINVAL);
}
```

Require the operation count to be one of `1, 2, 4, 8`, require alternating kinds, require `on_eof` and `on_error` to name terminal `FAIL` nodes, reject a repeated success node, and reject every later fd/buffer/length slot that aliases a prior effect result slot. Copy only the five static fields into `leir_native_plan_t`; do not retain a generic node table.

- [ ] **Step 5: Add portable build targets and run them**

Add `test_leir_native_plan` to Make clean/link/test lists and CMake/CTest. Link only `leir_program.c`, `leir_native_plan.c`, the test, and the minimum runtime objects already required by `leir_phase0.h`.

Run:

```bash
make -j4 test_leir_native_plan
./test_leir_native_plan
cmake -S . -B object/leir-native-plan-cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build object/leir-native-plan-cmake --target test_leir_native_plan -j4
ctest --test-dir object/leir-native-plan-cmake --output-on-failure -R test_leir_native_plan
```

Expected: all commands pass on macOS and Linux.

- [ ] **Step 6: Commit the portable planner**

```bash
git add experiments/leir/leir_native_plan.h experiments/leir/leir_native_plan.c experiments/leir/test_leir_native_plan.c Makefile CMakeLists.txt
git commit -m "feat: compile static LEIR native plans"
```

### Task 2: Linux Segment State Machine and SQE Encoder

**Files:**
- Create: `src/io/linux/runtime_io_segment_linux_internal.h`
- Create: `src/io/linux/watch/linux_segment.c`
- Create: `experiments/leir/test_leir_native_linux.c`
- Modify: `src/internal/runtime_state.h`
- Modify: `src/core/base/io_udata.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: liburing SQE helpers and the existing low-three-bit user-data encoding.
- Produces:

```c
#define LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS 8U

typedef enum llam_linux_native_op_kind {
    LLAM_LINUX_NATIVE_OP_RECV = 0,
    LLAM_LINUX_NATIVE_OP_SEND = 1,
} llam_linux_native_op_kind_t;

typedef enum llam_linux_native_segment_mode {
    LLAM_LINUX_NATIVE_SEGMENT_LINK = 0,
    LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP = 1,
} llam_linux_native_segment_mode_t;

typedef enum llam_linux_native_cqe_action {
    LLAM_LINUX_NATIVE_CQE_CONTINUE = 0,
    LLAM_LINUX_NATIVE_CQE_COMPLETE_OK = 1,
    LLAM_LINUX_NATIVE_CQE_COMPLETE_ERROR = 2,
    LLAM_LINUX_NATIVE_CQE_FATAL = 3,
} llam_linux_native_cqe_action_t;

typedef struct llam_linux_native_op {
    uint16_t kind;
    uint16_t result_slot;
    llam_fd_t fd;
    void *buffer;
    uint32_t length;
} llam_linux_native_op_t;

typedef struct llam_linux_native_segment
    llam_linux_native_segment_t;

typedef struct llam_linux_native_token {
    _Alignas(8) llam_linux_native_segment_t *owner;
    uint64_t generation;
    uint16_t operation_index;
    uint16_t reserved16;
    uint32_t reserved32;
} llam_linux_native_token_t;

struct llam_linux_native_segment {
    llam_runtime_t *owner_runtime;
    llam_node_t *owner_node;
    llam_io_req_t *req;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_linux_native_token_t
        tokens[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_linux_native_segment_t *next;
    uint64_t generation;
    uint64_t activations;
    uint64_t logical_operations;
    uint64_t queue_publications;
    uint64_t prepared_sqes;
    uint64_t observed_cqes;
    uint64_t suppressed_success_cqes;
    uint64_t task_parks;
    uint64_t terminal_wakes;
    uint64_t hot_allocations;
    unsigned op_count;
    unsigned completed_cqes;
    unsigned first_error_index;
    int first_error;
    llam_linux_native_segment_mode_t mode;
    atomic_uint state;
    atomic_uint terminal_claimed;
};

int llam_linux_native_segment_configure(
    llam_linux_native_segment_t *segment,
    const llam_linux_native_op_t *ops,
    unsigned op_count,
    llam_linux_native_segment_mode_t mode);
void llam_linux_native_segment_prepare_sqe(
    const llam_linux_native_segment_t *segment,
    unsigned index,
    struct io_uring_sqe *sqe);
llam_linux_native_cqe_action_t llam_linux_native_segment_apply_cqe(
    llam_linux_native_segment_t *segment,
    const llam_linux_native_token_t *token,
    int result,
    int *terminal_result_out);
```

- [ ] **Step 1: Write pure SQE encoding tests**

Without creating a kernel ring, configure segments with 1, 2, 4, and 8 operations, zero a stack `struct io_uring_sqe`, call `llam_linux_native_segment_prepare_sqe`, and assert:

```c
assert(sqe.opcode == IORING_OP_RECV);       /* read */
assert(sqe.opcode == IORING_OP_SEND);       /* write */
assert(sqe.fd == op.fd);
assert(sqe.addr == (uintptr_t)op.buffer);
assert(sqe.len == op.length);
assert(sqe.msg_flags == MSG_NOSIGNAL);       /* send only */
assert(sqe.flags == IOSQE_IO_LINK);          /* link mode, non-final */
assert(sqe.flags ==
       (IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS)); /* skip mode */
assert(final_sqe.flags == 0U);
```

Decode `sqe.user_data` and assert tag `LLAM_IO_UDATA_NATIVE_SEGMENT`, token owner, operation index, and generation.

- [ ] **Step 2: Write pure completion-state tests**

Cover:

```c
static int test_link_waits_for_final_cqe_and_preserves_first_error(void);
static int test_skip_success_completes_on_final_cqe(void);
static int test_skip_intermediate_failure_waits_for_tail(void);
static int test_short_success_becomes_emsgsize_at_exact_boundary(void);
static int test_stale_generation_is_fatal(void);
static int test_duplicate_terminal_is_fatal(void);
```

Link mode must observe all operation CQEs and return terminal only for the
final token. Skip mode may skip successful intermediate tokens, but it must
drain any visible intermediate failure and dependent cancellations through
the final token. A result different from the declared exact length becomes
`-EMSGSIZE` when it is observable. A stale generation, foreign owner, invalid
index, or event after terminal claim returns `FATAL`.

Correction from kernel-path validation: skip mode may observe an intermediate
error followed by one `-ECANCELED` CQE for every dependent linked operation.
It must retain the first non-cancel error and wait for the final token before
claiming terminal state. Only a short result on the unsuppressed final
operation can be checked directly in skip mode; exact intermediate message
sizes remain part of the trusted `SOCK_SEQPACKET` protocol envelope.

- [ ] **Step 3: Run the Linux test target and observe the red state**

Run on Linux:

```bash
make -j4 test_leir_native_linux
```

Expected: build failure because the header, implementation, tag, and target do not exist.

- [ ] **Step 4: Define the fixed-size segment object**

The private header defines concrete operations, cache-aligned tokens, cumulative metrics, and these states:

```c
enum {
    LLAM_LINUX_NATIVE_SEGMENT_IDLE = 0U,
    LLAM_LINUX_NATIVE_SEGMENT_QUEUED = 1U,
    LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT = 2U,
    LLAM_LINUX_NATIVE_SEGMENT_TERMINAL = 3U,
};
```

Each token contains an `_Alignas(8)` owner pointer, generation, and operation index. Add:

```c
LLAM_IO_UDATA_NATIVE_SEGMENT = 5U,
```

and static assertions that the tag fits three bits, token alignment is at least eight, and token size is a multiple of eight.

- [ ] **Step 5: Implement configuration, encoding, and completion reduction**

`configure` validates mode, count, kinds, nonnegative fds, non-null buffers, and lengths in `[1, UINT_MAX]`; it copies operations and initializes token owner/index fields. `prepare_sqe` uses `io_uring_prep_recv` or `io_uring_prep_send`, assigns flags only according to index/mode, and encodes the token. `apply_cqe` performs generation/state checks before mutating counters and records the first non-`ECANCELED` failure.

- [ ] **Step 6: Build and run the pure Linux tests**

Run:

```bash
make -j4 test_leir_native_linux
./test_leir_native_linux --unit-only
```

Expected: all pure tests pass without requiring an io_uring-capable kernel.

- [ ] **Step 7: Commit the backend-native value semantics**

```bash
git add src/io/linux/runtime_io_segment_linux_internal.h src/io/linux/watch/linux_segment.c experiments/leir/test_leir_native_linux.c src/internal/runtime_state.h src/core/base/io_udata.c Makefile CMakeLists.txt
git commit -m "feat: encode Linux native effect segments"
```

### Task 3: Node Queue, Ring Features, and Whole-Chain Submission

**Files:**
- Modify: `src/internal/runtime_types.h`
- Modify: `src/internal/runtime_proto_io.h`
- Modify: `src/io/engine/io_engine.c`
- Modify: `src/io/linux/runtime_io_watch_linux_internal.h`
- Modify: `src/io/linux/watch/linux_segment.c`
- Modify: `src/io/linux/watch/linux_submit.c`
- Modify: `experiments/leir/test_leir_native_linux.c`

**Interfaces:**
- Consumes: the segment object from Task 2 and existing `submit_lock`, `pending_ops`, ring-submit metrics, fd lifecycle lock, and worker wake path.
- Produces:

```c
bool llam_linux_native_segment_enqueue(
    llam_node_t *node,
    llam_linux_native_segment_t *segment,
    llam_io_req_t *req);
llam_linux_native_segment_t *llam_linux_native_segment_take_all(
    llam_node_t *node);
unsigned llam_linux_native_segment_submit_one(
    llam_node_t *node,
    llam_linux_native_segment_t *segment);
```

- [ ] **Step 1: Write queue ownership and capacity tests**

Add tests that create a test-owned node and assert:

```c
static int test_enqueue_publishes_once_and_counts_one_pending_owner(void);
static int test_enqueue_rejects_foreign_runtime_with_exdev(void);
static int test_enqueue_rejects_non_idle_segment_with_ebusy(void);
static int test_take_all_marks_request_inflight_once(void);
static int test_chain_capacity_failure_consumes_no_sqe(void);
static int test_skip_mode_rejects_missing_feature_with_enotsup(void);
```

For the capacity test, initialize a small userspace ring fixture, fill until fewer than `op_count` SQEs remain, snapshot `ring.sq.sqe_tail`, call the segment submit helper with a submit override that leaves the ring full, and assert the tail is unchanged and the task request completes with `EAGAIN`.

- [ ] **Step 2: Run the focused tests and verify the intended failures**

Run on Linux:

```bash
./test_leir_native_linux --queue
```

Expected: tests fail because the node has no segment queue or feature bitmap.

- [ ] **Step 3: Add private node ownership fields**

Forward-declare `struct llam_linux_native_segment` in `runtime_types.h` and, only for `LLAM_RUNTIME_BACKEND_LINUX`, add:

```c
struct llam_linux_native_segment *native_segment_head;
struct llam_linux_native_segment *native_segment_tail;
uint32_t linux_ring_features;
```

The existing `submit_lock` owns both native queue pointers. Do not add a mutex.

- [ ] **Step 4: Record ring features for normal and SQPOLL initialization**

Use `io_uring_queue_init_params` for the normal ring too:

```c
memset(&params, 0, sizeof(params));
rc = io_uring_queue_init_params(
    LLAM_IO_RING_DEPTH, &node->ring, &params);
if (rc == 0) {
    node->linux_ring_features = params.features;
    node->ring_ready = true;
}
```

Also copy `params.features` after successful SQPOLL setup. Clear the field on setup failure and teardown.

- [ ] **Step 5: Implement atomic queue admission and detachment**

Under `submit_lock`, validate node/runtime/request ownership, reject a non-idle segment, reject skip mode without `IORING_FEAT_CQE_SKIP`, increment `pending_ops` once, set request attachment, append the segment, and publish state `QUEUED` with release ordering. Detachment moves the complete list out, changes each request from `SUBMIT_QUEUE` to `INFLIGHT`, sets one `inflight_owner_shard`, and increments that shard's waiter count once.

- [ ] **Step 6: Prepare complete chains before the common submit call**

In `llam_io_submit_batch`, drain controls, then segments, then ordinary requests while holding the existing fd lifecycle lock. `llam_linux_native_segment_submit_one` must:

```c
if (io_uring_sq_space_left(&node->ring) < segment->op_count) {
    int rc = llam_node_submit_ring(node);
    if (rc < 0 ||
        io_uring_sq_space_left(&node->ring) < segment->op_count) {
        complete_segment_locally(node, segment, -EAGAIN);
        return 0U;
    }
}
```

Only after the second capacity check may it call `io_uring_get_sqe` in a loop. It sets state `INFLIGHT` before a CQE can be visible and returns the exact SQE count for batch metrics.

- [ ] **Step 7: Run queue, build, and existing regression tests**

Run on Linux:

```bash
make -j4 test_leir_native_linux test_runtime_shutdown_internal
./test_leir_native_linux --queue
./test_runtime_shutdown_internal
```

Expected: all pass; every local failure restores `pending_ops` and inflight waiter accounting to zero.

- [ ] **Step 8: Commit node integration**

```bash
git add src/internal/runtime_types.h src/internal/runtime_proto_io.h src/io/engine/io_engine.c src/io/linux/runtime_io_watch_linux_internal.h src/io/linux/watch/linux_segment.c src/io/linux/watch/linux_submit.c experiments/leir/test_leir_native_linux.c
git commit -m "feat: submit native segments through io_uring"
```

### Task 4: Terminal CQE Dispatch and One-Park Issue Path

**Files:**
- Modify: `src/io/runtime_io_api_internal.h`
- Modify: `src/io/api/issue.c`
- Modify: `src/io/linux/runtime_io_watch_linux_internal.h`
- Modify: `src/io/linux/watch/linux_segment.c`
- Modify: `src/io/linux/watch/cqe.c`
- Modify: `experiments/leir/test_leir_native_linux.c`

**Interfaces:**
- Consumes: `llam_prepare_io_wait`, `llam_park_io_req`, `llam_io_complete_req`, and Task 3 queue functions.
- Produces:

```c
int llam_issue_linux_native_segment(
    llam_linux_native_segment_t *segment,
    llam_io_req_t *req);
void llam_linux_native_segment_handle_cqe(
    llam_node_t *node,
    llam_linux_native_token_t *token,
    int result);
```

- [ ] **Step 1: Write synthetic dispatch tests**

Add:

```c
static int test_link_dispatch_wakes_only_after_final_cqe(void);
static int test_skip_dispatch_wakes_once_on_final_success(void);
static int test_skip_dispatch_drains_tail_before_error_wake(void);
static int test_dispatch_balances_pending_and_inflight_once(void);
static int test_dispatch_records_fatal_for_stale_token(void);
```

Use a fixture with one runtime, node, shard, parked task, request, and segment; inject tokens through the same function called by `cqe.c`. Assert request result/error, task reinjection count, segment terminal wake count, `pending_ops == 0`, and no second wake.

- [ ] **Step 2: Run the dispatch tests and observe the red state**

Run on Linux:

```bash
./test_leir_native_linux --dispatch
```

Expected: failure because tag 5 has no CQE switch case and no terminal bridge.

- [ ] **Step 3: Expose wait preparation as a hidden internal helper**

Remove `static` from:

```c
int llam_prepare_io_wait(
    llam_io_req_t *req,
    llam_io_wait_mode_t wait_mode,
    uint64_t deadline_ns);
```

Declare it `LLAM_INTERNAL_API` in `runtime_io_api_internal.h`. It remains absent from installed headers and hidden in shared builds.

- [ ] **Step 4: Implement the no-cancellation issue wrapper**

`llam_issue_linux_native_segment` validates managed task/shard context, matching runtime, an idle request, no task cancel token, no runtime stop, ring readiness, RECV/SEND support, and skip feature support. It initializes the request, calls:

```c
llam_prepare_io_wait(
    req,
    LLAM_IO_WAIT_MODE_SUBMIT_QUEUE,
    UINT64_C(0));
```

It then enqueues the segment and calls:

```c
return llam_park_io_req(req, false, 0U, node);
```

On enqueue failure, call `llam_cleanup_io_wait_setup`, preserve the original errno, and leave the segment reusable.

- [ ] **Step 5: Dispatch tag 5 and finish through the existing request path**

Add a `LLAM_IO_UDATA_NATIVE_SEGMENT` switch case that decodes the token and calls the segment handler. The handler uses `apply_cqe`; `CONTINUE` returns without touching the request. A terminal action claims exactly once, then calls:

```c
llam_io_complete_req(
    node,
    segment->req,
    terminal_result,
    0U,
    true);
```

The generic function owns pending decrement, inflight accounting, wait-mode clearing, latency metrics, and task reinjection.

- [ ] **Step 6: Run the focused tests**

Run on Linux:

```bash
make -j4 test_leir_native_linux
./test_leir_native_linux --dispatch
```

Expected: all dispatch tests pass with one terminal wake and balanced ownership.

- [ ] **Step 7: Commit the terminal protocol**

```bash
git add src/io/runtime_io_api_internal.h src/io/api/issue.c src/io/linux/runtime_io_watch_linux_internal.h src/io/linux/watch/linux_segment.c src/io/linux/watch/cqe.c experiments/leir/test_leir_native_linux.c
git commit -m "feat: complete native segments with one task wake"
```

### Task 5: LEIR Binding and Reusable Experimental Instance

**Files:**
- Create: `experiments/leir/leir_native_segment.h`
- Create: `experiments/leir/leir_native_segment.c`
- Create: `experiments/leir/test_leir_native_segment.c`
- Modify: `experiments/leir/leir_test_support.h`
- Modify: `experiments/leir/leir_test_support.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `leir_native_plan_t`, Phase 0 values/slots, embedded LLAM request storage, and the Linux issue path.
- Produces:

```c
typedef enum leir_native_mode {
    LEIR_NATIVE_MODE_LINK = 0,
    LEIR_NATIVE_MODE_LINK_CQE_SKIP = 1,
} leir_native_mode_t;

typedef struct leir_native_instance leir_native_instance_t;

typedef struct leir_native_metrics {
    uint64_t activations;
    uint64_t logical_operations;
    uint64_t queue_publications;
    uint64_t prepared_sqes;
    uint64_t observed_cqes;
    uint64_t suppressed_success_cqes;
    uint64_t task_parks;
    uint64_t terminal_wakes;
    uint64_t hot_allocations;
    /* UINT16_MAX means that the activation has no failed operation. */
    uint16_t first_error_operation;
} leir_native_metrics_t;

size_t leir_native_instance_size(void);
int leir_native_instance_init(
    void *storage,
    size_t storage_size,
    const leir_phase0_program_t *program,
    const leir_native_plan_t *plan,
    leir_native_mode_t mode);
int leir_native_instance_bind(
    leir_native_instance_t *instance,
    const leir_phase0_value_t *values,
    size_t value_count);
int leir_native_instance_run(
    leir_native_instance_t *instance,
    leir_phase0_value_t *values_out,
    size_t value_count,
    leir_native_metrics_t *metrics_out);
```

- [ ] **Step 1: Write portable bind validation tests**

Assert rejection for null/undersized/misaligned storage, wrong value count, negative signed length, zero length, length beyond buffer capacity, length above `UINT_MAX`, null nonempty buffer, invalid fd, and rebind while active. Assert repeated use of the same valid fd across alternating steps is accepted.

- [ ] **Step 2: Write Linux socket-boundary tests**

On Linux, assert:

```c
static int test_bind_rejects_regular_file_with_enotsock(void);
static int test_bind_rejects_stream_socket_with_eprototype(void);
static int test_bind_rejects_unconnected_seqpacket_with_enotconn(void);
static int test_bind_accepts_connected_unix_seqpacket(void);
```

Add `leir_test_socketpair_type(int socket_type, llam_fd_t pair_out[2])`; retain `leir_test_socketpair` as a `SOCK_STREAM` wrapper so Phase 0A behavior is unchanged.

- [ ] **Step 3: Run tests and verify red**

Run:

```bash
make -j4 test_leir_native_segment
./test_leir_native_segment
```

Expected: build failure because the instance API does not exist.

- [ ] **Step 4: Implement fixed storage, binding, and the non-Linux stub**

Store a copy of values and a Linux segment inside the instance; never allocate. Validate every concrete value before configuring the backend. On Linux use `getsockopt(SOL_SOCKET, SO_TYPE)` and `getpeername`; on non-Linux let initialization/binding validate portable data but return `ENOTSUP` from `run`.

- [ ] **Step 5: Implement one-activation run**

Wait cooperatively until the task's embedded request is reusable, acquire it through `llam_api_io_req_acquire`, set the final operation kind/fd/buffer/count, advance a nonzero segment generation, and call `llam_issue_linux_native_segment`. After wake, infer every skipped successful exact result from its declared length, publish the first failing result slot on error, copy values/metrics, clear request pointers, transition `TERMINAL -> IDLE`, and release the embedded request.

- [ ] **Step 6: Prove repeated success and failure behavior with real sockets**

Run a managed task over connected Unix sequence-packet pairs and assert:

- link-only 1/2/4/8-step success;
- skip success when the kernel feature exists;
- link-only observed CQEs equal logical operations;
- skip observed CQEs equal activations and suppressed CQEs equal `(ops - 1) * activations`;
- peer-close failure reports the exact first operation;
- 1,000 instance reuses advance generation and leave `pending_ops` zero.

If ring setup returns `EPERM`, `ENOSYS`, or skip support is absent, print an explicit `SKIP` record for only the affected integration case; pure tests must still run.

- [ ] **Step 7: Run portable and Linux suites**

Run:

```bash
make -j4 test_leir_native_plan test_leir_native_segment test_leir_native_linux
./test_leir_native_plan
./test_leir_native_segment
./test_leir_native_linux
```

Expected: all applicable cases pass and skips are feature-specific.

- [ ] **Step 8: Commit the LEIR-to-backend adapter**

```bash
git add experiments/leir/leir_native_segment.h experiments/leir/leir_native_segment.c experiments/leir/test_leir_native_segment.c experiments/leir/leir_test_support.h experiments/leir/leir_test_support.c Makefile CMakeLists.txt
git commit -m "feat: bind LEIR plans to Linux native segments"
```

### Task 6: Process-Isolated Three-Mode Benchmark

**Files:**
- Modify: `experiments/leir/leir_peer_process.h`
- Modify: `experiments/leir/leir_peer_process.c`
- Create: `experiments/leir/bench_leir_native_segment.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: the existing external peer, balanced block runner patterns from `bench_leir_phase0.c`, public LLAM read/write baseline, and native instance API.
- Produces one strict line:

```text
LEIR_NATIVE_PAIR version=1 candidate=link_skip ops=4 concurrency=64 payload=64 activations=4096 min_mode_ns=100000000 blocks_per_mode=16 baseline_wall_ns=200000000 candidate_wall_ns=100000000 baseline_cpu_ns=170000000 candidate_cpu_ns=100000000 wall_speedup=2.000000000 cpu_ratio=0.588235294 baseline_ctx_switches=8192 candidate_ctx_switches=2048 logical_operations=16384 queue_publications=4096 prepared_sqes=16384 ring_submit_calls=256 ring_submit_syscalls=256 expected_cqes=4096 observed_cqes=4096 suppressed_success_cqes=12288 baseline_task_parks=16384 candidate_task_parks=4096 terminal_wakes=4096 resumes_avoided=12288 hot_allocations=0 baseline_checksum=0123456789abcdef candidate_checksum=0123456789abcdef pending_path_valid=1 baseline_service_gap_p99_ns=10000 candidate_service_gap_p99_ns=10500 baseline_terminal_p99_ns=40000 candidate_terminal_p99_ns=42000 peer=process cpu_scope=server platform=linux_io_uring order=ABBA
```

- [ ] **Step 1: Write CLI and smoke-contract tests in the benchmark**

Add `--candidate link|link_skip`, `--ops 1|2|4|8`, `--concurrency 1..512`, `--payload 64..16384`, `--activations`, `--min-mode-ms`, and `--order ABBA|BAAB`. Reject duplicates, missing options, unknown fields, overflow, unsupported counts, and non-Linux execution.

- [ ] **Step 2: Extend the peer without changing Phase 0A defaults**

Add:

```c
typedef enum leir_peer_socket_kind {
    LEIR_PEER_SOCKET_STREAM = 0,
    LEIR_PEER_SOCKET_SEQPACKET = 1,
} leir_peer_socket_kind_t;
```

and fields `socket_kind` and `operations_per_activation` to `leir_peer_config_t`. Zero/default retains stream request-response behavior. Native mode sets sequence-packet and `1,2,4,8` operations. For one operation, the peer sends and records the request checksum without waiting for a response; even counts retain one send/receive transaction per operation pair.

- [ ] **Step 3: Run a smoke build and observe failure**

Run on Linux:

```bash
make -j4 bench_leir_native_segment
./bench_leir_native_segment --candidate link --ops 2 --concurrency 4 --payload 64 --activations 8 --min-mode-ms 1 --order ABBA
```

Expected before implementation: target or CLI failure.

- [ ] **Step 4: Implement identical baseline and candidate workloads**

Build one LEIR program per operation count. Baseline tasks execute alternating `llam_read`/`llam_write` and require every result to equal payload length. Candidate tasks reuse one bound native instance. Each measured block starts a fresh peer and runtime, signals the peer only after all tasks are ready, and measures server process CPU with `CLOCK_PROCESS_CPUTIME_ID`.

- [ ] **Step 5: Implement balanced sampling and correctness counters**

Use 16 measured blocks: ABBA repeats baseline/candidate/candidate/baseline; BAAB reverses it. Calibrate activation count outside measured blocks, require at least `min_mode_ns` per mode, and record p99 service gap plus activation terminal latency. Snapshot node submit-call/syscall counters around candidate execution. Reject output if checksums differ, duration is short, the peer is not a process, any native counter is inconsistent, or `pending_ops` is nonzero.

- [ ] **Step 6: Run link and skip smoke cells**

Run on Linux:

```bash
./bench_leir_native_segment --candidate link --ops 4 --concurrency 4 --payload 64 --activations 8 --min-mode-ms 1 --order ABBA
./bench_leir_native_segment --candidate link_skip --ops 4 --concurrency 4 --payload 64 --activations 8 --min-mode-ms 1 --order BAAB
```

Expected: exactly one strict result line per command; link observes one CQE per operation, skip observes one per activation.

- [ ] **Step 7: Commit the benchmark harness**

```bash
git add experiments/leir/leir_peer_process.h experiments/leir/leir_peer_process.c experiments/leir/bench_leir_native_segment.c Makefile CMakeLists.txt
git commit -m "bench: compare linked LEIR native segments"
```

### Task 7: Evidence Parser, Matrix, and Precommitted Classification

**Files:**
- Create: `scripts/bench_leir_native.py`
- Create: `scripts/test_bench_leir_native.py`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: the exact `LEIR_NATIVE_PAIR` schema from Task 6.
- Produces: `raw.csv`, `summary.csv`, `leir_native_report.md`, optional tracked report, and one of `SPECIALIZED`, `REJECT`, or `INCONCLUSIVE`.

- [ ] **Step 1: Write parser rejection tests**

Starting from one valid literal row, reject reordered fields, duplicate/missing/unknown fields, negative or overflow integers, NaN/Inf ratios, wrong platform/peer/CPU scope, checksum mismatch, duration below minimum, nonzero allocation, invalid CQE arithmetic, invalid queue/park/wake counts, and printed ratios that do not match raw times.

- [ ] **Step 2: Write matrix and classifier tests**

Assert the screen contains both candidates across:

```python
OPS = (1, 2, 4, 8)
CONCURRENCY = (1, 64, 512)
PAYLOADS = (64, 1024, 16384)
SAMPLES = 5
```

Assert a `SPECIALIZED` fixture requires every length-4/8, concurrency-64/512, payload-64/1024 `link_skip` cell to meet wall `>= 1.50`, CPU `<= 0.70`, terminal p99 ratio `<= 1.10`, paired spread `<= 1.10`, exact counters, zero allocation, and checksum equality. Length-1 controls require wall `>= 0.95`, CPU `<= 1.05`, and service-gap p99 ratio `<= 1.10`. Missing features/rows, invalid paths, short duration, or unstable evidence yields `INCONCLUSIVE`; a complete stable miss yields `REJECT`.

- [ ] **Step 3: Run tests and observe red**

Run:

```bash
python3 -m unittest scripts/test_bench_leir_native.py -v
```

Expected: import failure because the runner does not exist.

- [ ] **Step 4: Implement strict parsing, subprocess bounds, and reports**

Reuse only safe process helpers from `scripts/process_utils.py`; invoke the benchmark with argument arrays, cap stdout/stderr, enforce timeouts, parse exactly one line, use median paired ratios, and write deterministic sorted evidence. Never interpret shell text from benchmark output.

- [ ] **Step 5: Add smoke and screen Make targets**

Add:

```make
test-leir-native: test_leir_native_plan test_leir_native_segment bench_leir_native_segment
	./test_leir_native_plan
	./test_leir_native_segment
	LEIR_NATIVE_TEST_BINARY=./bench_leir_native_segment \
		python3 -m unittest scripts/test_bench_leir_native.py -v

leir-native-screen: test-leir-native
	python3 scripts/bench_leir_native.py \
		--binary ./bench_leir_native_segment \
		--phase screen --samples 5 --min-mode-ms 100 \
		--output-dir object/leir-native-screen
```

- [ ] **Step 6: Run parser and smoke contracts**

Run:

```bash
python3 -m unittest scripts/test_bench_leir_native.py -v
make -j4 test-leir-native
```

Expected: all contract and smoke tests pass on Linux; non-Linux native integration is reported as a platform skip rather than a positive classification.

- [ ] **Step 7: Commit evidence tooling**

```bash
git add scripts/bench_leir_native.py scripts/test_bench_leir_native.py Makefile CMakeLists.txt
git commit -m "test: gate LEIR native segment evidence"
```

### Task 8: Linux Research Workflow

**Files:**
- Create: `.github/workflows/leir-native-research.yml`
- Modify: `scripts/verify_linux.sh`

**Interfaces:**
- Consumes: all native targets and the evidence runner.
- Produces: source-pinned Linux build/sanitizer/test logs and uploaded screen evidence.

- [ ] **Step 1: Add workflow-structure tests to the Python contract**

Parse the workflow as text and assert it pins `ubuntu-24.04`, installs `liburing-dev`, verifies `$GITHUB_SHA`, runs Make and CMake native targets, runs ASan/UBSan and TSan integration tests, runs the five-sample 100 ms screen, and uploads artifacts even after failure.

- [ ] **Step 2: Run the workflow contract and observe red**

Run:

```bash
python3 -m unittest scripts/test_bench_leir_native.py -v
```

Expected: workflow contract fails because the file does not exist.

- [ ] **Step 3: Create the source-pinned workflow**

The workflow must:

- trigger on relevant runtime, experiment, script, Make/CMake, and workflow paths;
- use read-only contents permission;
- use `actions/checkout@v6`, `actions/setup-python@v6`, and `actions/upload-artifact@v6`;
- run exact-source verification;
- build with GCC Make and Clang CMake;
- run pure and real-socket tests;
- run ASan/UBSan and TSan native tests;
- run `make test`, shared-export audit, and `scripts/verify_linux.sh`;
- pin the benchmark process with `taskset`;
- run screen evidence with five samples and 100 ms minimum;
- record kernel, CPU, compiler, liburing, source SHA, affinity, and feature status.

- [ ] **Step 4: Add native targets to Linux verification**

Run the portable planner always and Linux segment/integration tests when liburing and io_uring are available. Treat a missing kernel feature as an explicit native experiment skip, not as a generic runtime failure.

- [ ] **Step 5: Validate syntax and contracts locally**

Run:

```bash
python3 -m unittest scripts/test_bench_leir_native.py -v
git diff --check
```

Expected: all pass.

- [ ] **Step 6: Commit CI coverage**

```bash
git add .github/workflows/leir-native-research.yml scripts/verify_linux.sh scripts/test_bench_leir_native.py
git commit -m "ci: validate LEIR Linux native segments"
```

### Task 9: Full Verification, Security Review, and Evidence Handoff

**Files:**
- Create after Linux evidence: `docs/superpowers/reports/2026-07-27-leir-linux-native-segment-results.md`
- Modify only if verification exposes a defect: files owned by Tasks 1-8.

**Interfaces:**
- Consumes: the complete branch, CI artifacts, and precommitted thresholds.
- Produces: a reviewed draft PR based on `codex/leir-phase0` and a result report that does not bump or release `2.2.0`.

- [ ] **Step 1: Run local portable verification**

Run:

```bash
make clean
make -j4 test
python3 -m unittest scripts/test_bench_leir_native.py -v
cmake -S . -B object/leir-native-local -DCMAKE_BUILD_TYPE=Debug
cmake --build object/leir-native-local -j4
ctest --test-dir object/leir-native-local --output-on-failure
git diff --check
```

Expected: full local suite and CMake suite pass; native execution is an explicit non-Linux skip on macOS.

- [ ] **Step 2: Audit ABI and hot-path ownership**

Run:

```bash
make -j4 shared audit-shared-exports audit-production-test-hooks
nm -g libllam_runtime.a | rg 'leir|native_segment' || true
rg -n 'malloc|calloc|realloc|free' src/io/linux/watch/linux_segment.c experiments/leir/leir_native_segment.c
```

Expected: shared export set is unchanged; new symbols are hidden/internal; timed run/submission/completion functions contain no allocation.

- [ ] **Step 3: Run a normal Codex Security diff scan**

Review pointer-tag alignment, user-controlled lengths/pointers, fd type and connectedness checks, generation/stale-CQE handling, queue ownership, counter underflow/overflow, task/request lifetime, shell-free subprocess use, bounded output, and workflow permissions. Fix every validated high/medium finding with a failing regression test before proceeding.

- [ ] **Step 4: Push and open a draft PR**

```bash
git push -u origin codex/leir-native-segment
gh pr create \
  --base codex/leir-phase0 \
  --head codex/leir-native-segment \
  --draft \
  --title "research: compile LEIR effects into io_uring segments" \
  --body-file .artifacts/leir-native-pr-body.md
```

The PR body states the semantic envelope, platform boundary, exact tests, current evidence status, and why no version/release is authorized.

- [ ] **Step 5: Wait for every required CI check and repair failures**

Inspect each failing log, reproduce where possible, add a regression test, fix, recommit, push, and wait again. Do not classify evidence while any required build, sanitizer, portability, shared-export, security, or native workflow check is red.

- [ ] **Step 6: Classify the uploaded evidence without moving thresholds**

Download the exact-SHA native workflow artifact, rerun the parser against its raw rows, and write the tracked report with environment, feature status, matrix summary, failure reasons, and one of:

```text
SPECIALIZED
REJECT
INCONCLUSIVE
```

A `SPECIALIZED` result authorizes the next private design for cancellation/timeouts/teardown/source maps/generated C. It does not authorize `CATEGORY`, a version bump, or a release.

- [ ] **Step 7: Commit the exact-SHA report and re-run CI**

```bash
git add docs/superpowers/reports/2026-07-27-leir-linux-native-segment-results.md
git commit -m "docs: record LEIR Linux native segment evidence"
git push
```

Wait until the report commit's required checks pass. Leave the PR draft unless the user separately authorizes integration.
