# LEIR Phase 0A Userspace Advancement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Determine with real LLAM tasks and real kernel I/O whether a typed
effect instance can reuse one I/O request and advance multiple runtime effects
from the completion owner without resuming a language task between effects.

**Architecture:** Add one private completion-sink seam to `llam_io_req_t`, then
build a research-only LEIR validator and sequential AoS instance engine under
`experiments/leir/`. A managed task parks once through the existing
`llam_issue_io()` path; intermediate io_uring, IOCP, or kqueue completions run
only trusted LEIR advancement, reuse the request, and submit the next effect.
The native paired benchmark compares that path with ordinary stackful LLAM
code against the same socket peer and validates the actual submit, completion,
and context-switch counters.

**Tech Stack:** C11, C11 atomics, the existing LLAM internal I/O and scheduler
interfaces, io_uring/liburing, kqueue, IOCP, POSIX fork for gate-driving peers,
Win32 threads for non-gating smoke, CMake 3.20+, GNU Make compatibility, Python
3 standard library, ASan/UBSan, TSan where supported, and GitHub Actions.

## Global Constraints

- Follow
  `docs/research/specs/2026-07-27-llam-effect-ir-design.md`.
- This plan implements **Phase 0A userspace advancement only**. It does not
  implement linked SQEs, `IOSQE_CQE_SKIP_SUCCESS`, a public ABI, graph fanout,
  channels/select, arbitrary predicates, or compiler integration.
- Phase 0A cannot award the spec's final `CATEGORY` verdict. It may return
  `ADVANCE_PASS`, `ADVANCE_REJECT`, or `INCONCLUSIVE` and decide whether to
  execute a separate Phase 0B linked-segment plan.
- Keep all program, instance, benchmark, and report APIs private under
  `experiments/leir/`. The only production-source change is the generic
  internal completion-sink seam.
- Do not change public headers under `include/llam/`, ABI metadata, package
  version, changelog, default runtime policy, or release configuration.
- Preserve the user's root-worktree changes in
  `docs/operations/benchmarks.md` and `scripts/bench_deep_compare.py`; never
  stage, overwrite, or format either file.
- Do not add a third-party dependency.
- A completion sink is trusted runtime-internal code. No public function
  pointer or arbitrary user callback may execute on an I/O backend thread.
- Program nodes may express only `READ`, `READ_EXACT`, `WRITE`, `WRITE_ALL`,
  `RETURN`, and `FAIL` in Phase 0A.
- Keep per-instance state contiguous AoS. Do not introduce planes, tiles,
  pointer packing, runtime cohort formation, or per-completion heap objects.
- Reuse the task's existing embedded `llam_io_req_t` where available. Every
  gate-driving row must report `heap_requests=0` and `hot_allocations=0`.
- Program validation and allocation happen before measurement. Instance
  binding, activation, first submission, terminal publication, and cleanup
  remain inside measurement.
- Intermediate advancement must preserve the same request generation, task
  wait tracking, cancellation ownership, backend pending-op accounting,
  result normalization, and terminal errno behavior as the ordinary path.
- Every graph activation has exactly one language-task park and no more than
  one terminal task publication. Returning `false` from the completion sink
  delegates that publication to the existing reinjection path. A consumed
  intermediate completion may never clear task wait tracking.
- Direct nonblocking probes may execute only built-in read/write effects. At
  inline-budget exhaustion, submit the current built-in effect through the
  normal backend instead of executing more inline work.
- Use one semantically active node per instance. Kernel-linked multi-request
  ownership is reserved for Phase 0B.
- The POSIX gate-driving peer runs in a child process so server process CPU
  time excludes peer work. Windows thread-peer rows are correctness/smoke
  evidence and must report `cpu_scope=combined`, so they cannot drive a CPU
  verdict.
- Test program lengths `1`, `2`, `4`, and `8`; concurrency `1`, `64`, and
  `512`; payloads `64`, `1024`, and `16384` bytes; inline budgets `1`, `8`,
  and `32`.
- Gate-driving program lengths `4` and `8` use concurrency `64` and `512`,
  payloads `64` and `1024`, and inline budget `8`.
- Use two core workloads and one short-region control:
  - `socket_relay`: each read result supplies the following write length;
  - `framed_rpc`: exact header/body reads and exact fixed response writes;
  - `graph_break`: a one-node LEIR `READ_EXACT -> RETURN` segment, a
    deterministic C header transform, then an ordinary public
    `llam_write()` loop. Its baseline performs the same read, transform, and
    write entirely through ordinary public APIs.
- The peer reads each response before sending the next request on that
  connection. This makes the following server read naturally pending without
  injecting an artificial sleep into every operation.
- A core gate-driving row is path-valid only when ordinary task-level I/O
  submit/completion counts prove that the baseline parked, candidate
  backend-submit and effect-completion counts balance, candidate task-level
  submit/completion counts equal activations, candidate terminal publications
  equal activations, and candidate intermediate task publications remain zero.
  A `graph_break` row instead requires one LEIR terminal publication per
  activation and balanced task-level submit/completion counts after including
  its ordinary public write.
- Measure each pair in one fresh native process with sixteen balanced ABBA or
  BAAB blocks, calibrated to at least `100 ms` per mode for screening and
  `250 ms` per mode for a full Phase 0A gate.
- Use five fresh processes per screening cell and nine per gate cell. Compute
  wall speedup and CPU ratio inside the native process; aggregate only those
  paired ratios.
- Retain all samples. Paired spread is `max / min`; never discard an outlier
  or silently rerun a failing cell.
- Keep the final spec thresholds unchanged. The Phase 0A continuation gate is:
  - `ADVANCE_PASS`: both workloads have worst-cell wall speedup `>= 1.25x`
    and CPU ratio `<= 0.85x` in every gate-driving cell;
  - length-1 and graph-break controls remain `>= 0.95x` baseline throughput
    with CPU ratio `<= 1.05x`;
  - p99 terminal latency and unrelated-task service-gap degradation are each
    `<= 10%`;
  - all correctness, path, allocation, and stability controls pass;
  - every gate-driving wall and CPU paired spread is `<= 1.10x`.
- `ADVANCE_REJECT` requires stable valid evidence that either core workload has a
  gate-driving wall floor `< 1.10x` and CPU ratio `> 0.95x`, or that short
  controls regress beyond their guard. Results between the pass and reject
  bounds are `INCONCLUSIVE` and require mechanism evidence before another run.

## File Map

### Internal completion seam

- `src/internal/runtime_types.h`
  - private completion-sink callback type and two request fields.
- `src/internal/runtime_proto_io.h`
  - declaration of the sink dispatcher.
- `src/core/memory/io_object_alloc.c`
  - reset both sink fields whenever request storage is activated.
- `src/io/watch/watch_queue.c`
  - common trusted sink dispatcher.
- `src/io/linux/watch/linux_control.c`
- `src/io/darwin/watch/darwin_completion.c`
- `src/io/windows/watch/windows_completion.c`
  - offer normalized completions to the sink before ordinary task reinjection.

### LEIR experiment

- `experiments/leir/leir_phase0.h`
  - fixed research descriptor, slot, option, result, and metric contract.
- `experiments/leir/leir_phase0_internal.h`
  - compiled program and mutable instance representation.
- `experiments/leir/leir_program.c`
  - descriptor copying plus structural, type, edge, and reachability checks.
- `experiments/leir/leir_engine.c`
  - binding, direct built-in effect execution, request preparation, completion
    advancement, resubmission, terminal publication, and cancellation.
- `experiments/leir/leir_test_support.h`
- `experiments/leir/leir_test_support.c`
  - portable connected socket pair, deterministic peer task, clocks, and
    assertions shared by unit and native benchmark code.
- `experiments/leir/test_leir_phase0.c`
  - seam, validator, real-I/O, partial progress, cancellation, stale
    generation, fairness, and allocation tests.
- `experiments/leir/leir_peer_process.h`
- `experiments/leir/leir_peer_process.c`
  - POSIX child-process peer and Windows thread-peer protocol driver.
- `experiments/leir/bench_leir_phase0.c`
  - strict one-cell paired driver and machine-readable result.

### Evidence tooling

- `scripts/bench_leir_phase0.py`
  - strict parser, screen/gate matrices, subprocess runner, summaries,
    integrity classification, and evidence output.
- `scripts/test_bench_leir_phase0.py`
  - parser, matrix, command, verdict, spread, and malformed-row tests.
- `.github/workflows/leir-research.yml`
  - manually dispatched authoritative Linux x86-64 evidence.

### Build integration

- `CMakeLists.txt`
- `Makefile`
- `.gitignore`

### Evidence

- `object/leir-phase0a-screen/leir_phase0a_samples.csv`
- `object/leir-phase0a-screen/leir_phase0a_summary.csv`
- `object/leir-phase0a-screen/leir_phase0a_metadata.json`
- `object/leir-phase0a-screen/leir_phase0a_report.md`
- `object/leir-phase0a-gate/leir_phase0a_samples.csv`
- `object/leir-phase0a-gate/leir_phase0a_summary.csv`
- `object/leir-phase0a-gate/leir_phase0a_metadata.json`
- `object/leir-phase0a-gate/leir_phase0a_report.md`
- `docs/research/reports/2026-07-27-leir-phase0a-results.md`
- `docs/research/reports/2026-07-27-leir-phase0a-decision.md`

Raw `object/` evidence remains ignored. A tracked report is generated from the
same parsed rows and must be byte-identical to the raw report copied into the
artifact.

---

### Task 1: Trusted I/O Completion-Sink Seam

**Files:**

- Create: `experiments/leir/test_leir_phase0.c`
- Modify: `src/internal/runtime_types.h`
- Modify: `src/internal/runtime_proto_io.h`
- Modify: `src/core/memory/io_object_alloc.c`
- Modify: `src/io/watch/watch_queue.c`
- Modify: `src/io/linux/watch/linux_control.c`
- Modify: `src/io/darwin/watch/darwin_completion.c`
- Modify: `src/io/windows/watch/windows_completion.c`
- Modify: `CMakeLists.txt`
- Modify: `Makefile`
- Modify: `.gitignore`

**Interfaces:**

- Consumes: normalized `llam_io_req_t::result`, `error_code`, completion owner,
  and `llam_wait_reason_t` produced by existing backend completion functions.
- Produces:

```c
typedef bool (*llam_io_completion_sink_fn)(
    llam_node_t *node,
    llam_io_req_t *req,
    unsigned completion_owner,
    llam_wait_reason_t *wake_reason,
    void *context);

bool llam_io_dispatch_completion_sink(
    llam_node_t *node,
    llam_io_req_t *req,
    unsigned completion_owner,
    llam_wait_reason_t *wake_reason);
```

- `true` means trusted internal code retained the parked task and either
  resubmitted the request or arranged a later terminal completion.
- `false` means the backend must execute its existing ordinary reinjection
  path unchanged.

- [ ] **Step 1: Write the failing sink-dispatch test**

Add a local callback and the first test to
`experiments/leir/test_leir_phase0.c`:

```c
#include "runtime_internal.h"

#include <stdio.h>
#include <string.h>

static bool test_sink(llam_node_t *node,
                      llam_io_req_t *req,
                      unsigned completion_owner,
                      llam_wait_reason_t *wake_reason,
                      void *context) {
    unsigned *calls = context;

    (void)node;
    if (req == NULL || completion_owner != 7U ||
        wake_reason == NULL || *wake_reason != LLAM_WAIT_IO) {
        return false;
    }
    *calls += 1U;
    return true;
}

static int test_completion_sink_dispatch(void) {
    llam_io_req_t req;
    llam_wait_reason_t wake_reason = LLAM_WAIT_IO;
    unsigned calls = 0U;

    memset(&req, 0, sizeof(req));
    req.completion_sink = test_sink;
    req.completion_sink_context = &calls;
    if (!llam_io_dispatch_completion_sink(
            NULL, &req, 7U, &wake_reason) ||
        calls != 1U) {
        return 1;
    }
    req.completion_sink = NULL;
    return llam_io_dispatch_completion_sink(
               NULL, &req, 7U, &wake_reason)
               ? 1
               : 0;
}

int main(void) {
    if (test_completion_sink_dispatch() != 0) {
        fputs("test_completion_sink_dispatch failed\n", stderr);
        return 1;
    }
    puts("LEIR Phase 0 tests passed");
    return 0;
}
```

- [ ] **Step 2: Compile the test translation unit and verify it fails**

Run:

```bash
cc -Iinclude -Isrc -Isrc/internal \
  -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE \
  -std=c11 -Wall -Wextra -Wpedantic -Werror \
  -c experiments/leir/test_leir_phase0.c \
  -o /tmp/test_leir_phase0.o
```

Expected: compilation fails because `completion_sink`,
`completion_sink_context`, and `llam_io_dispatch_completion_sink` do not
exist.

- [ ] **Step 3: Add and reset the private sink fields**

Add the callback typedef before `struct llam_io_req` and append these fields
after `platform_data`:

```c
llam_io_completion_sink_fn completion_sink;
void *completion_sink_context;
```

In `llam_io_req_reset()` set both fields before publishing
`lifetime_refs = 0`:

```c
req->completion_sink = NULL;
req->completion_sink_context = NULL;
```

- [ ] **Step 4: Implement the dispatcher and backend calls**

Implement in `src/io/watch/watch_queue.c`:

```c
bool llam_io_dispatch_completion_sink(
    llam_node_t *node,
    llam_io_req_t *req,
    unsigned completion_owner,
    llam_wait_reason_t *wake_reason) {
    llam_io_completion_sink_fn sink;

    if (req == NULL || wake_reason == NULL) {
        return false;
    }
    sink = req->completion_sink;
    return sink != NULL &&
           sink(node,
                req,
                completion_owner,
                wake_reason,
                req->completion_sink_context);
}
```

In all three platform completion functions insert this exact semantic point
after result normalization and latency accounting, immediately before
`llam_reinject_task_on_shard()`:

```c
if (llam_io_dispatch_completion_sink(
        node, req, completion_owner, &wake_reason)) {
    return;
}
```

Do not move cancellation cleanup, pending-op decrement, result normalization,
or metrics around the new call.

- [ ] **Step 5: Add the test target to both build systems**

Define `test_leir_phase0` as a private executable linked with `llam_runtime`,
with include paths `src`, `src/internal`, and `experiments/leir`. Add the same
target to Make's program lists, clean lists, `.PHONY` test grouping, and
`.gitignore`.

The first CMake block is:

```cmake
add_executable(test_leir_phase0
    experiments/leir/test_leir_phase0.c
)
target_include_directories(test_leir_phase0 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
    ${CMAKE_CURRENT_SOURCE_DIR}/experiments/leir
)
target_link_libraries(test_leir_phase0 PRIVATE llam_runtime)
add_test(NAME test_leir_phase0 COMMAND test_leir_phase0)
```

- [ ] **Step 6: Run seam and existing I/O tests**

Run:

```bash
make -j4 test_leir_phase0 test_io_buffers test_runtime_api_edges
./test_leir_phase0
./test_io_buffers
./test_runtime_api_edges
```

Expected: all three executables pass.

- [ ] **Step 7: Audit request size and baseline performance**

Record `sizeof(llam_io_req_t)` before and after in the commit message body or
review notes, then run:

```bash
make -j4 bench
for repeat in 1 2 3; do
env LLAM_BENCH_ONLY=io_echo \
  LLAM_BENCH_ROUNDS=11 \
  LLAM_BENCH_WARMUP_ROUNDS=2 \
  LLAM_BENCH_IO_MESSAGES=4096 \
  LLAM_BENCH_REUSE_SOCKETPAIR=1 \
  LLAM_RUNTIME_PROFILE=release-fast \
  ./bench
done
```

The seam is rejected before further work if three repeated baseline medians
show a stable throughput loss greater than `5%`.

- [ ] **Step 8: Commit the seam**

```bash
git add src/internal/runtime_types.h \
  src/internal/runtime_proto_io.h \
  src/core/memory/io_object_alloc.c \
  src/io/watch/watch_queue.c \
  src/io/linux/watch/linux_control.c \
  src/io/darwin/watch/darwin_completion.c \
  src/io/windows/watch/windows_completion.c \
  experiments/leir/test_leir_phase0.c \
  CMakeLists.txt Makefile .gitignore
git commit -m "test: add internal I/O completion sink"
```

---

### Task 2: Typed LEIR Program Descriptor And Validator

**Files:**

- Create: `experiments/leir/leir_phase0.h`
- Create: `experiments/leir/leir_phase0_internal.h`
- Create: `experiments/leir/leir_program.c`
- Modify: `experiments/leir/test_leir_phase0.c`
- Modify: `CMakeLists.txt`
- Modify: `Makefile`

**Interfaces:**

- Consumes: `llam_fd_t` and platform macros from `runtime_internal.h`.
- Produces:

```c
#define LEIR_PHASE0_MAX_NODES 32U
#define LEIR_PHASE0_MAX_SLOTS 16U
#define LEIR_PHASE0_NODE_NONE UINT16_MAX

typedef enum leir_phase0_opcode {
    LEIR_PHASE0_OP_READ = 0,
    LEIR_PHASE0_OP_READ_EXACT = 1,
    LEIR_PHASE0_OP_WRITE = 2,
    LEIR_PHASE0_OP_WRITE_ALL = 3,
    LEIR_PHASE0_OP_RETURN = 4,
    LEIR_PHASE0_OP_FAIL = 5,
} leir_phase0_opcode_t;

typedef enum leir_phase0_slot_kind {
    LEIR_PHASE0_SLOT_FD = 0,
    LEIR_PHASE0_SLOT_MUT_BUFFER = 1,
    LEIR_PHASE0_SLOT_CONST_BUFFER = 2,
    LEIR_PHASE0_SLOT_U64 = 3,
    LEIR_PHASE0_SLOT_I64 = 4,
} leir_phase0_slot_kind_t;

typedef struct leir_phase0_buffer {
    void *data;
    size_t size;
} leir_phase0_buffer_t;

typedef union leir_phase0_value {
    llam_fd_t fd;
    leir_phase0_buffer_t buffer;
    uint64_t u64;
    int64_t i64;
} leir_phase0_value_t;

typedef struct leir_phase0_node_desc {
    uint16_t opcode;
    uint16_t fd_slot;
    uint16_t buffer_slot;
    uint16_t length_slot;
    uint16_t result_slot;
    uint16_t on_success;
    uint16_t on_eof;
    uint16_t on_error;
} leir_phase0_node_desc_t;

typedef struct leir_phase0_program_desc {
    const leir_phase0_node_desc_t *nodes;
    const leir_phase0_slot_kind_t *slot_kinds;
    size_t node_count;
    size_t slot_count;
    uint16_t entry_node;
} leir_phase0_program_desc_t;

typedef struct leir_phase0_program leir_phase0_program_t;

int leir_phase0_program_create(
    const leir_phase0_program_desc_t *desc,
    leir_phase0_program_t **out);
void leir_phase0_program_destroy(leir_phase0_program_t *program);
```

- [ ] **Step 1: Write validator failure tests**

Add tests for:

```c
static const leir_phase0_slot_kind_t valid_slots[] = {
    LEIR_PHASE0_SLOT_FD,
    LEIR_PHASE0_SLOT_MUT_BUFFER,
    LEIR_PHASE0_SLOT_U64,
    LEIR_PHASE0_SLOT_I64,
};

static const leir_phase0_node_desc_t valid_nodes[] = {
    {
        .opcode = LEIR_PHASE0_OP_READ_EXACT,
        .fd_slot = 0U,
        .buffer_slot = 1U,
        .length_slot = 2U,
        .result_slot = 3U,
        .on_success = 1U,
        .on_eof = 2U,
        .on_error = 2U,
    },
    {
        .opcode = LEIR_PHASE0_OP_RETURN,
        .fd_slot = LEIR_PHASE0_NODE_NONE,
        .buffer_slot = LEIR_PHASE0_NODE_NONE,
        .length_slot = LEIR_PHASE0_NODE_NONE,
        .result_slot = 3U,
        .on_success = LEIR_PHASE0_NODE_NONE,
        .on_eof = LEIR_PHASE0_NODE_NONE,
        .on_error = LEIR_PHASE0_NODE_NONE,
    },
    {
        .opcode = LEIR_PHASE0_OP_FAIL,
        .fd_slot = LEIR_PHASE0_NODE_NONE,
        .buffer_slot = LEIR_PHASE0_NODE_NONE,
        .length_slot = LEIR_PHASE0_NODE_NONE,
        .result_slot = 3U,
        .on_success = LEIR_PHASE0_NODE_NONE,
        .on_eof = LEIR_PHASE0_NODE_NONE,
        .on_error = LEIR_PHASE0_NODE_NONE,
    },
};
```

Assertions:

- the descriptor above succeeds;
- `node_count=0`, `node_count=33`, and `entry_node=3` fail with `EINVAL`;
- a read using a const-buffer slot fails with `EINVAL`;
- a write whose result slot is `U64` instead of `I64` fails with `EINVAL`;
- an out-of-range successor fails with `EINVAL`;
- a nonterminal node with any required edge set to
  `LEIR_PHASE0_NODE_NONE` fails with `EINVAL`;
- a `RETURN` or `FAIL` node with a non-`NONE` successor fails with `EINVAL`;
- a read self-cycle whose EOF and error edges terminate succeeds, proving that
  an await-capable cycle is legal;
- allocation failure leaves `*out == NULL`.

- [ ] **Step 2: Run the validator tests and verify link failure**

Run:

```bash
make -j4 test_leir_phase0
```

Expected: link fails because `leir_phase0_program_create` and
`leir_phase0_program_destroy` do not exist.

- [ ] **Step 3: Implement immutable program copying and validation**

Define the private program as fixed copied arrays:

```c
struct leir_phase0_program {
    leir_phase0_node_desc_t nodes[LEIR_PHASE0_MAX_NODES];
    leir_phase0_slot_kind_t slot_kinds[LEIR_PHASE0_MAX_SLOTS];
    uint16_t entry_node;
    uint16_t node_count;
    uint16_t slot_count;
    uint16_t io_node_count;
};
```

Implement these focused helpers in `leir_program.c`:

```c
static bool leir_phase0_node_index_valid(
    const leir_phase0_program_t *program,
    uint16_t index);
static int leir_phase0_validate_slots(
    const leir_phase0_program_t *program,
    const leir_phase0_node_desc_t *node);
static int leir_phase0_validate_edges(
    const leir_phase0_program_t *program);
static int leir_phase0_validate_reachability(
    const leir_phase0_program_t *program);
```

In Phase 0A every reachable nonterminal opcode is await-capable, so the design
spec's SCC rule reduces to rejecting missing edges while accepting I/O cycles.
Do not invent a non-await branch opcode merely to exercise `ELOOP`; fairness
for immediately ready I/O cycles is enforced dynamically by `inline_budget`
in Tasks 4 and 5.

`program_create` must set `*out = NULL`, validate sizes before multiplying,
allocate exactly one program object with `calloc`, copy descriptors, validate,
and free on every failure.

- [ ] **Step 4: Run unit tests and sanitizers**

Run:

```bash
make -j4 test_leir_phase0
./test_leir_phase0
cmake -S . -B object/leir-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build object/leir-asan --target test_leir_phase0 -j4
ASAN_OPTIONS=detect_leaks=1 \
  ./object/leir-asan/test_leir_phase0
```

Expected: all validator and sink tests pass without sanitizer output.

- [ ] **Step 5: Commit the program contract**

```bash
git add experiments/leir/leir_phase0.h \
  experiments/leir/leir_phase0_internal.h \
  experiments/leir/leir_program.c \
  experiments/leir/test_leir_phase0.c \
  CMakeLists.txt Makefile
git commit -m "test: validate private LEIR programs"
```

---

### Task 3: One-Park Instance Lifecycle On A Real Backend

**Files:**

- Create: `experiments/leir/leir_engine.c`
- Create: `experiments/leir/leir_test_support.h`
- Create: `experiments/leir/leir_test_support.c`
- Modify: `experiments/leir/leir_phase0.h`
- Modify: `experiments/leir/leir_phase0_internal.h`
- Modify: `experiments/leir/test_leir_phase0.c`
- Modify: `CMakeLists.txt`
- Modify: `Makefile`

**Interfaces:**

- Consumes:
  - `llam_api_io_req_acquire()`;
  - `llam_issue_io()`;
  - `llam_node_submit_io_req()`;
  - `llam_try_direct_rw()`;
  - the completion sink from Task 1.
- Produces:

```c
typedef struct leir_phase0_run_opts {
    unsigned inline_budget;
    uint64_t deadline_ns;
    bool has_deadline;
    bool force_backend;
} leir_phase0_run_opts_t;

typedef struct leir_phase0_metrics {
    uint64_t activations;
    uint64_t effect_completions;
    uint64_t backend_submits;
    uint64_t direct_completions;
    uint64_t task_parks;
    uint64_t terminal_publications;
    uint64_t task_resumes_avoided;
    uint64_t fairness_resubmits;
    uint64_t stale_completions;
    uint64_t heap_requests;
    uint64_t hot_allocations;
} leir_phase0_metrics_t;

typedef struct leir_phase0_instance leir_phase0_instance_t;

size_t leir_phase0_instance_size(void);
int leir_phase0_instance_init(
    void *storage,
    size_t storage_size,
    const leir_phase0_program_t *program);
int leir_phase0_instance_bind(
    leir_phase0_instance_t *instance,
    const leir_phase0_value_t *values,
    size_t value_count,
    const leir_phase0_run_opts_t *opts);
int leir_phase0_instance_run(
    leir_phase0_instance_t *instance,
    leir_phase0_value_t *values_out,
    size_t value_count,
    leir_phase0_metrics_t *metrics_out);
```

- [ ] **Step 1: Write a real pending-read terminal test**

Create a connected pair with `leir_test_socketpair()`. Spawn a peer LLAM task
that sleeps for 2 ms and writes 64 deterministic bytes. The tested task binds
a `READ_EXACT -> RETURN` program and runs one instance.

Assert:

```c
result == 0
values[result_slot].i64 == 64
memcmp(received, expected, 64U) == 0
metrics.activations == 1U
metrics.effect_completions == 1U
metrics.task_parks == 1U
metrics.terminal_publications == 1U
metrics.task_resumes_avoided == 0U
metrics.heap_requests == 0U
```

- [ ] **Step 2: Run the test and verify link failure**

Run:

```bash
make -j4 test_leir_phase0
```

Expected: link fails on the new instance functions.

- [ ] **Step 3: Implement fixed-storage instance initialization and binding**

Define the private instance with no flexible array:

```c
struct leir_phase0_instance {
    const leir_phase0_program_t *program;
    leir_phase0_value_t slots[LEIR_PHASE0_MAX_SLOTS];
    leir_phase0_run_opts_t opts;
    leir_phase0_metrics_t metrics;
    _Atomic(llam_io_req_t *) req;
    _Atomic(llam_task_t *) task;
    atomic_uint cancel_requested;
    atomic_uint running;
    atomic_uint terminal;
    atomic_uint_fast64_t activation_generation;
    atomic_uint_fast64_t request_generation;
    uint16_t current_node;
    size_t node_progress;
    unsigned inline_left;
    int terminal_error;
};
```

`instance_bind` validates every bound FD, buffer pointer/size, requested
length, and value count; resets mutable state; increments a nonzero activation
generation; and performs no allocation.

`instance_run` stores the acquired request generation into
`request_generation`, then publishes `req` and `task` with release stores
before the first submission. Cancellation uses acquire loads of those pointers
and the task's atomic `wait_generation`. Cleanup publishes both pointers as
`NULL` only after `llam_issue_io()` returns and the request has no backend
owner.

- [ ] **Step 4: Implement one backend operation and terminal sink behavior**

Prepare `llam_io_req_t` from the current read/write node:

```c
req->kind = read_node ? LLAM_IO_KIND_READ : LLAM_IO_KIND_WRITE;
req->fd = instance->slots[node->fd_slot].fd;
req->buf = (unsigned char *)
               instance->slots[node->buffer_slot].buffer.data +
           instance->node_progress;
req->count =
    (size_t)instance->slots[node->length_slot].u64 -
    instance->node_progress;
req->completion_sink = leir_phase0_completion_sink;
req->completion_sink_context = instance;
```

The initial call uses `llam_issue_io()` so the existing task state,
cancellation registration, deadline, park, and early-completion race remain
authoritative. Count that accepted path as the first `backend_submits` entry
and the activation's only `task_parks` entry; resubmissions never increment
`task_parks`. For a terminal completion the sink writes the declared result
slot, sets `terminal`, increments `terminal_publications`, clears no task
tracking, and returns `false`; the platform completion function then performs
its ordinary reinjection.

- [ ] **Step 5: Clear private pointers before request release**

After `llam_issue_io()` returns:

```c
req->completion_sink = NULL;
req->completion_sink_context = NULL;
atomic_store_explicit(&instance->req, NULL, memory_order_release);
atomic_store_explicit(&instance->task, NULL, memory_order_release);
llam_api_io_req_release(g_llam_tls_shard, req);
```

Copy result slots and metrics out only after the request is no longer
backend-owned.

- [ ] **Step 6: Run real-I/O tests repeatedly**

Run:

```bash
make -j4 test_leir_phase0
for i in 1 2 3 4 5 6 7 8 9 10; do
  ./test_leir_phase0
done
```

Expected: all ten runs pass; the pending-read test reports one park and one
terminal publication followed by the existing reinjection path.

- [ ] **Step 7: Commit the instance lifecycle**

```bash
git add experiments/leir/leir_engine.c \
  experiments/leir/leir_test_support.h \
  experiments/leir/leir_test_support.c \
  experiments/leir/leir_phase0.h \
  experiments/leir/leir_phase0_internal.h \
  experiments/leir/test_leir_phase0.c \
  CMakeLists.txt Makefile
git commit -m "test: run a LEIR instance through native I/O"
```

---

### Task 4: Multi-Node Direct Advancement And Partial I/O

**Files:**

- Modify: `experiments/leir/leir_engine.c`
- Modify: `experiments/leir/leir_phase0_internal.h`
- Modify: `experiments/leir/leir_test_support.c`
- Modify: `experiments/leir/test_leir_phase0.c`

**Interfaces:**

- Consumes: the instance and completion sink from Task 3.
- Produces these focused private helpers:

```c
typedef enum leir_phase0_advance_result {
    LEIR_PHASE0_ADVANCE_TERMINAL = 0,
    LEIR_PHASE0_ADVANCE_NEEDS_BACKEND = 1,
    LEIR_PHASE0_ADVANCE_ERROR = 2,
} leir_phase0_advance_result_t;

static int leir_phase0_apply_result(
    leir_phase0_instance_t *instance,
    ssize_t result,
    int error_code);
static leir_phase0_advance_result_t leir_phase0_advance_direct(
    leir_phase0_instance_t *instance);
static bool leir_phase0_resubmit(
    llam_node_t *node,
    llam_io_req_t *req,
    unsigned completion_owner,
    leir_phase0_instance_t *instance);
```

- [ ] **Step 1: Write four-node and eight-node differential tests**

For each program length, build alternating nodes:

```text
READ or READ_EXACT
WRITE_ALL using the preceding read-result slot as its length
READ or READ_EXACT
WRITE_ALL
...
RETURN
```

Run an ordinary baseline task and a LEIR task against peers that read every
response before sending the next request. Assert byte-for-byte equality and:

```c
candidate.effect_completions == program_length
candidate.task_parks == 1U
candidate.terminal_publications == 1U
candidate.task_resumes_avoided >=
    expected_pending_completions - 1U
candidate.backend_submits <= program_length
```

- [ ] **Step 2: Verify the multi-node test fails**

Run:

```bash
./test_leir_phase0
```

Expected: failure because the sink terminates after the first node.

- [ ] **Step 3: Implement result-edge and exact-progress semantics**

For `READ_EXACT` and `WRITE_ALL`:

```c
if (result > 0) {
    instance->node_progress += (size_t)result;
    instance->slots[node->result_slot].i64 =
        (int64_t)instance->node_progress;
    if (instance->node_progress == requested_length) {
        instance->current_node = node->on_success;
        instance->node_progress = 0U;
    }
} else if (result == 0 && read_node) {
    instance->current_node = node->on_eof;
    instance->node_progress = 0U;
} else {
    instance->terminal_error = error_code != 0 ? error_code : EIO;
    instance->current_node = node->on_error;
    instance->node_progress = 0U;
}
```

Plain `READ` and `WRITE` take their edge after one non-error result. Reject a
positive result larger than the requested remainder with `EPROTO`.

- [ ] **Step 4: Implement trusted direct advancement**

At completion-sink entry, reject any non-`LLAM_WAIT_IO` wake as terminal for
the whole instance, then count the backend effect completion and reset:

```c
instance->inline_left =
    instance->opts.inline_budget > 0U
        ? instance->opts.inline_budget - 1U
        : 0U;
```

The backend completion itself consumes the first unit of that callback turn.
After applying an intermediate completion, increment
`task_resumes_avoided` immediately because that backend completion did not
publish the task. Then inspect only the next built-in read/write node and call
`llam_try_direct_rw()`. Completed direct operations increment both
`effect_completions` and `direct_completions`, consume one remaining
inline-budget unit, and loop. Would-block returns
`LEIR_PHASE0_ADVANCE_NEEDS_BACKEND`.

When `inline_left == 0`, do not attempt another syscall:

```c
instance->metrics.fairness_resubmits += 1U;
return LEIR_PHASE0_ADVANCE_NEEDS_BACKEND;
```

This makes the existing backend queue the fairness boundary.

- [ ] **Step 5: Implement request resubmission without task reinjection**

Before `llam_node_submit_io_req()`:

```c
atomic_store_explicit(
    &req->owner_shard, completion_owner, memory_order_release);
atomic_store_explicit(
    &req->attached_node_index, node->index, memory_order_release);
atomic_store_explicit(
    &req->wait_mode,
    LLAM_IO_WAIT_MODE_SUBMIT_QUEUE,
    memory_order_release);
req->submit_ts_ns = llam_now_ns();
```

If submission fails, restore `wait_mode=NONE`, set the instance terminal error
to `errno != 0 ? errno : EIO`, update the pointed-to wake reason to
`LLAM_WAIT_CANCEL` when the failure is cancellation, and return `false` so the
parked task wakes. On success increment `backend_submits`, call
`llam_kick_node(node)`, and return `true`.

- [ ] **Step 6: Add forced partial-progress tests**

Use a 16 KiB payload, set socket send/receive buffers to 1024 bytes where the
platform permits, and make the peer consume 257-byte slices. Verify:

- `READ_EXACT` returns the full 16 KiB;
- `WRITE_ALL` sends the full 16 KiB;
- result slots contain the aggregate length;
- EOF after a partial exact read takes `on_eof`;
- a closed peer takes `on_error` with a nonzero normalized errno;
- no task resumes between partial chunks.

- [ ] **Step 7: Run correctness and sanitizer suites**

Run:

```bash
make -j4 test_leir_phase0
./test_leir_phase0
cmake --build object/leir-asan --target test_leir_phase0 -j4
ASAN_OPTIONS=detect_leaks=1 \
  ./object/leir-asan/test_leir_phase0
```

Expected: all direct, backend, exact, partial, EOF, and error paths pass.

- [ ] **Step 8: Commit multi-node advancement**

```bash
git add experiments/leir/leir_engine.c \
  experiments/leir/leir_phase0_internal.h \
  experiments/leir/leir_test_support.c \
  experiments/leir/test_leir_phase0.c
git commit -m "feat: advance LEIR effects without task resumes"
```

---

### Task 5: Cancellation, Stale Identity, Fairness, And Lifetime Races

**Files:**

- Modify: `experiments/leir/leir_phase0.h`
- Modify: `experiments/leir/leir_phase0_internal.h`
- Modify: `experiments/leir/leir_engine.c`
- Modify: `experiments/leir/test_leir_phase0.c`

**Interfaces:**

- Consumes:
  - `llam_abort_io_wait()`;
  - task `wait_generation`;
  - request `operation_generation`;
  - instance `activation_generation`.
- Produces:

```c
int leir_phase0_instance_cancel(leir_phase0_instance_t *instance);

bool leir_phase0_test_inject_completion(
    leir_phase0_instance_t *instance,
    uint64_t activation_generation,
    ssize_t result,
    int error_code);
```

- [ ] **Step 1: Write pre-submit and in-flight cancellation tests**

Test these exact cases:

1. set `cancel_requested` before `instance_run`; return `-1/ECANCELED` with
   zero backend submits;
2. cancel while `wait_mode=SUBMIT_QUEUE`;
3. cancel while `wait_mode=INFLIGHT`;
4. race cancellation with a real successful read for 10,000 activations;
5. reuse the same instance storage after every terminal result.

Each activation must produce one of the permitted complete results, exactly
one terminal publication, balanced `pending_ops`, and no hang.

- [ ] **Step 2: Run the cancellation tests and verify failure**

Run:

```bash
env LLAM_LEIR_RACE_ITERS=100 ./test_leir_phase0
```

Expected: the new cancellation cases fail because
`leir_phase0_instance_cancel` is undefined.

- [ ] **Step 3: Implement cancellation with gap closure**

The cancel API first publishes:

```c
atomic_store_explicit(
    &instance->cancel_requested, 1U, memory_order_release);
```

If a parked task is published, load its `wait_generation` and call:

```c
(void)llam_abort_io_wait(
    task, LLAM_IO_ABORT_CANCEL, wait_generation);
```

The completion sink checks `cancel_requested` before every direct advance and
immediately before every resubmit. When it terminates a canceled activation,
it writes `LLAM_WAIT_CANCEL` through the sink's `wake_reason` pointer before
returning `false`. This closes the short interval in which the ordinary abort
resolver may observe `wait_mode=NONE` between a completed operation and its
next queue publication without losing cancellation wake semantics.

- [ ] **Step 4: Validate activation and request generations**

At sink entry, compare the instance's captured request operation generation
with the request's current generation and require `running=1` and
`terminal=0`. A mismatch increments `stale_completions`, records `EPROTO` on a
live activation, and never submits another effect.

The test-only injection function applies no platform state; it exercises only
the generation/terminal arbitration helper.

- [ ] **Step 5: Add duplicate and stale injection tests**

Verify:

- an old activation generation cannot mutate reused slot state;
- a second terminal result does not increment terminal publications;
- a duplicate intermediate completion cannot submit two next operations;
- cancel/success races never leave `running=1` after join.

- [ ] **Step 6: Add fairness and companion-service tests**

Create a 32-node always-ready socket program with inline budgets `1`, `8`, and
`32`. Run a companion LLAM task that records service timestamps. Require:

```c
budget_1_metrics.fairness_resubmits > 0U
companion_runs > 0U
p99_service_gap_ns <= baseline_p99_service_gap_ns * 110U / 100U
```

If a platform cannot keep every node immediately ready, the test validates
budget accounting but marks the latency subcase non-gating.

- [ ] **Step 7: Run race and TSan checks**

Run:

```bash
env LLAM_LEIR_RACE_ITERS=10000 ./test_leir_phase0
cmake -S . -B object/leir-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
cmake --build object/leir-tsan --target test_leir_phase0 -j4
env LLAM_LEIR_RACE_ITERS=1000 \
  ./object/leir-tsan/test_leir_phase0
```

Expected: all race cases pass and TSan emits no report. If the host toolchain
cannot run TSan, preserve the exact compiler/runtime error and defer the TSan
gate to Linux CI.

- [ ] **Step 8: Commit race-safe lifecycle behavior**

```bash
git add experiments/leir/leir_phase0.h \
  experiments/leir/leir_phase0_internal.h \
  experiments/leir/leir_engine.c \
  experiments/leir/test_leir_phase0.c
git commit -m "test: harden LEIR cancellation and lifetime"
```

---

### Task 6: Native Paired Boundary Benchmark

**Files:**

- Create: `experiments/leir/leir_peer_process.h`
- Create: `experiments/leir/leir_peer_process.c`
- Create: `experiments/leir/bench_leir_phase0.c`
- Modify: `experiments/leir/leir_test_support.h`
- Modify: `experiments/leir/leir_test_support.c`
- Modify: `CMakeLists.txt`
- Modify: `Makefile`
- Modify: `.gitignore`

**Interfaces:**

- Consumes: validated programs and instances from Tasks 2–5.
- Produces one strict line per process:

```text
LEIR_PAIR version=1 workload=socket_relay nodes=4 concurrency=64 payload=64 inline_budget=8 activations=4096 min_mode_ns=100000000 blocks_per_mode=16 baseline_wall_ns=1 candidate_wall_ns=1 baseline_cpu_ns=1 candidate_cpu_ns=1 wall_speedup=1.000000000 cpu_ratio=1.000000000 baseline_ctx_switches=1 candidate_ctx_switches=1 baseline_task_io_submits=1 candidate_task_io_submits=1 baseline_task_io_completions=1 candidate_task_io_completions=1 candidate_backend_submits=1 effect_completions=1 direct_completions=0 terminal_publications=1 resumes_avoided=0 fairness_resubmits=0 heap_requests=0 hot_allocations=0 baseline_checksum=0000000000000000 candidate_checksum=0000000000000000 pending_path_valid=1 service_gap_p99_ns=0 terminal_p99_ns=0 peer=process cpu_scope=server order=ABBA
```

- CLI:

```text
--workload socket_relay|framed_rpc|graph_break
--nodes 1|2|4|8
--concurrency 1..512
--payload 64..16384
--inline-budget 1..32
--activations positive
--min-mode-ms positive
--order ABBA|BAAB
```

- [ ] **Step 1: Write CLI and native-row smoke tests**

Add Python-free native smoke cases to `test_leir_phase0` that invoke the
benchmark parser helper with:

- every valid enum/value;
- node count `3`, zero concurrency, zero payload, and budget `0`;
- missing options and duplicate options.

Invalid input must return exit code `2` and emit no `LEIR_PAIR` line.

- [ ] **Step 2: Implement the external peer protocol**

On POSIX:

1. create all socket pairs and two control pipes;
2. `fork()` before runtime threads start;
3. the child closes server ends, waits for a start byte, services all peer
   descriptors with `poll()`, verifies payload sequence numbers, and returns a
   checksum/status through the result pipe;
4. the parent closes peer ends and measures only its own process CPU clock;
5. the parent waits for and validates child exit after every mode.

On Windows, use the existing loopback connected-pair technique from
`examples/bench_support.c` and one peer thread. Mark rows
`peer=thread cpu_scope=combined`.

The peer state machine per connection is:

```text
send request[index]
read exact response[index]
validate response
increment index
repeat until activations are complete
```

- [ ] **Step 3: Implement credible ordinary and LEIR modes**

The ordinary mode calls the same semantic sequence with public LLAM APIs:

```c
if (bench_read_exact(fd, buffer, requested) != 0) {
    fail_mode(errno);
}
if (bench_write_all(fd, buffer, produced) != 0) {
    fail_mode(errno);
}
```

The candidate binds the same descriptor, buffer, length, and result values,
then calls `leir_phase0_instance_run()`.

For `graph_break`, the candidate returns from a one-node
`READ_EXACT -> RETURN` LEIR segment, applies the same fixed C transform to the
8-byte sequence header as the baseline, and completes the response with the
ordinary public write loop. This control is never fused across the C transform
and must satisfy the short-region guard even if the core workloads pass.

Both modes create a fresh runtime, fresh sockets, fresh peer, and identical
payload sequence for each block.

- [ ] **Step 4: Implement balanced block timing and calibration**

Use 16 blocks:

```c
static const unsigned abba_order[16] = {
    0U, 1U, 1U, 0U,
    0U, 1U, 1U, 0U,
    0U, 1U, 1U, 0U,
    0U, 1U, 1U, 0U,
};
```

BAAB swaps zero and one. Calibrate `activations` upward until each accumulated
mode time reaches `min_mode_ns`; rerun both modes with that frozen count.
Measure monotonic wall time and process CPU time around runtime execution only.

- [ ] **Step 5: Implement path and checksum validation**

Reject the row unless:

- peer, baseline, and candidate checksums match;
- every task and peer completed;
- requested activation/effect counts match;
- `heap_requests == 0` and `hot_allocations == 0`;
- candidate terminal publications equal candidate activations;
- candidate task resumes avoided equal validated intermediate completions;
- baseline task I/O submits/completions are nonzero and balanced in core cells;
- candidate task I/O submits/completions equal activations in core rows; a
  `graph_break` row balances both counters and permits additional ordinary
  write parks;
- candidate backend submits equal
  `effect_completions - direct_completions` in successful core cells;
- runtime pending I/O is zero after teardown.

Return exit code `3` on integrity failure and print diagnostics to stderr
without a machine-readable result line.

- [ ] **Step 6: Add benchmark targets and run native smoke**

Run:

```bash
make -j4 test_leir_phase0 bench_leir_phase0
./test_leir_phase0
./bench_leir_phase0 \
  --workload socket_relay \
  --nodes 4 \
  --concurrency 64 \
  --payload 64 \
  --inline-budget 8 \
  --activations 128 \
  --min-mode-ms 20 \
  --order ABBA
```

Expected: one `LEIR_PAIR version=1` line, matching checksums,
`pending_path_valid=1`, and no hot allocation.

- [ ] **Step 7: Commit the native benchmark**

```bash
git add experiments/leir/leir_peer_process.h \
  experiments/leir/leir_peer_process.c \
  experiments/leir/bench_leir_phase0.c \
  experiments/leir/leir_test_support.h \
  experiments/leir/leir_test_support.c \
  CMakeLists.txt Makefile .gitignore
git commit -m "bench: measure LEIR across real I/O boundaries"
```

---

### Task 7: Strict Evidence Runner And Phase 0A Classifier

**Files:**

- Create: `scripts/bench_leir_phase0.py`
- Create: `scripts/test_bench_leir_phase0.py`
- Modify: `CMakeLists.txt`
- Modify: `Makefile`

**Interfaces:**

- Consumes: exactly one `LEIR_PAIR version=1` row from each fresh native
  process.
- Produces:

```text
leir_phase0a_samples.csv
leir_phase0a_summary.csv
leir_phase0a_metadata.json
leir_phase0a_report.md
```

- Verdict values: `ADVANCE_PASS`, `ADVANCE_REJECT`, `INCONCLUSIVE`.

- [ ] **Step 1: Write strict parser tests**

Use this minimal valid fixture:

```python
VALID_ROW = (
    "LEIR_PAIR version=1 workload=socket_relay nodes=4 "
    "concurrency=64 payload=64 inline_budget=8 activations=4096 "
    "min_mode_ns=100000000 blocks_per_mode=16 "
    "baseline_wall_ns=200 candidate_wall_ns=100 "
    "baseline_cpu_ns=170 candidate_cpu_ns=100 "
    "wall_speedup=2.000000000 cpu_ratio=0.588235294 "
    "baseline_ctx_switches=8 candidate_ctx_switches=2 "
    "baseline_task_io_submits=4 candidate_task_io_submits=1 "
    "baseline_task_io_completions=4 candidate_task_io_completions=1 "
    "candidate_backend_submits=4 effect_completions=8 "
    "direct_completions=4 terminal_publications=1 "
    "resumes_avoided=3 fairness_resubmits=0 heap_requests=0 "
    "hot_allocations=0 baseline_checksum=0123456789abcdef "
    "candidate_checksum=0123456789abcdef pending_path_valid=1 "
    "service_gap_p99_ns=100 terminal_p99_ns=100 peer=process "
    "cpu_scope=server order=ABBA"
)
```

Reject missing, duplicate, unknown, reordered schema-version, nonfinite,
negative, checksum-mismatch, path-invalid, allocation-bearing, and
`cpu_scope=combined` gate rows.

- [ ] **Step 2: Run parser tests and verify import failure**

Run:

```bash
python3 -m unittest scripts/test_bench_leir_phase0.py -v
```

Expected: import fails because `bench_leir_phase0.py` does not exist.

- [ ] **Step 3: Implement parser and deterministic matrices**

Screen matrix:

```python
CORE_WORKLOADS = ("socket_relay", "framed_rpc")
CONTROL_WORKLOADS = ("graph_break",)
NODES = (1, 2, 4, 8)
CONCURRENCY = (1, 64, 512)
PAYLOADS = (64, 1024, 16384)
INLINE_BUDGETS = (1, 8, 32)
```

The screen runs the Cartesian core matrix. The `graph_break` control uses
`nodes=1`, every screen concurrency and payload, and `inline_budget=8`; node
count denotes the one-node LEIR segment, not the following ordinary write.

Full Phase 0A gate matrix:

```python
GATE_NODES = (1, 4, 8)
GATE_CONCURRENCY = (64, 512)
GATE_PAYLOADS = (64, 1024)
GATE_INLINE_BUDGET = 8
GATE_CONTROL_WORKLOADS = ("graph_break",)
GATE_CONTROL_NODES = 1
```

The full gate includes the `graph_break` control at every gate concurrency and
payload. Only node counts `4` and `8` of the two core workloads drive the
`1.25x/0.85x` continuation threshold; core node count `1` and all
`graph_break` rows drive the `0.95x/1.05x` short-region guard.

Odd fresh-process samples use ABBA; even samples use BAAB. Every subprocess
gets one cell and one order.

- [ ] **Step 4: Implement summaries and verdicts**

For each cell compute:

- median wall speedup;
- median CPU ratio;
- wall and CPU `max/min` spread;
- minimum path-valid/allocation/checksum controls;
- median p99 terminal latency and service gap.

The report must print final spec targets and Phase 0A continuation targets
side-by-side and explicitly state that `ADVANCE_PASS` is not `CATEGORY`.

- [ ] **Step 5: Add runner failure-mode tests**

Cover:

- timeout and nonzero child exit;
- two result lines;
- stderr diagnostics with a valid stdout row;
- insufficient `min_mode_ns`;
- missing sample;
- spread `1.100000` pass and `1.100001` fail;
- pass, reject, and inconclusive bounds;
- Windows `cpu_scope=combined` excluded from CPU verdict;
- byte-identical raw and tracked report output.

- [ ] **Step 6: Integrate Make and CTest targets**

Add:

```make
test-leir-phase0: test_leir_phase0 bench_leir_phase0
	./test_leir_phase0
	LEIR_PHASE0_TEST_BINARY=./bench_leir_phase0 \
		python3 scripts/test_bench_leir_phase0.py

leir-phase0a-screen: test-leir-phase0
	python3 scripts/bench_leir_phase0.py \
		--binary ./bench_leir_phase0 \
		--phase screen \
		--samples 5 \
		--min-mode-ms 100 \
		--output-dir object/leir-phase0a-screen \
		--tracked-report \
			docs/research/reports/2026-07-27-leir-phase0a-results.md
```

CMake registers the Python test only when a Python interpreter is available
and passes `$<TARGET_FILE:bench_leir_phase0>` through
`LEIR_PHASE0_TEST_BINARY`.

- [ ] **Step 7: Run native and Python contract suites**

Run:

```bash
make -j4 test-leir-phase0
python3 -m unittest scripts/test_bench_leir_phase0.py -v
```

Expected: all C and Python tests pass.

- [ ] **Step 8: Commit the evidence runner**

```bash
git add scripts/bench_leir_phase0.py \
  scripts/test_bench_leir_phase0.py \
  CMakeLists.txt Makefile
git commit -m "test: classify LEIR userspace advancement"
```

---

### Task 8: Cross-Platform Verification And Authoritative Workflow

**Files:**

- Create: `.github/workflows/leir-research.yml`
- Modify: `CMakeLists.txt`
- Modify: `Makefile`
- Modify: `experiments/leir/leir_test_support.c`
- Modify: `experiments/leir/leir_peer_process.c`

**Interfaces:**

- Consumes: `test-leir-phase0` and the Phase 0A runner.
- Produces a manual workflow artifact named
  `leir-phase0a-<phase>-linux-x86_64`.

- [ ] **Step 1: Run local warning-clean and CMake checks**

Run:

```bash
make clean
make -j4 test-leir-phase0 \
  CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g -fno-omit-frame-pointer"
cmake -S . -B object/leir-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build object/leir-cmake \
  --target test_leir_phase0 bench_leir_phase0 -j4
ctest --test-dir object/leir-cmake \
  --output-on-failure \
  -R "test_leir_phase0|test_bench_leir_phase0"
```

Expected: all builds and tests pass.

- [ ] **Step 2: Run project regression verification**

Run the host-appropriate full command:

```bash
make verify-darwin
```

On Linux use:

```bash
make verify-linux CC=gcc
```

No LEIR change may weaken or skip an existing test.

- [ ] **Step 3: Run sanitizer and TSan gates**

Run:

```bash
cmake --build object/leir-asan --target test_leir_phase0 -j4
ASAN_OPTIONS=detect_leaks=1 \
  ./object/leir-asan/test_leir_phase0
cmake --build object/leir-tsan --target test_leir_phase0 -j4
env LLAM_LEIR_RACE_ITERS=1000 \
  ./object/leir-tsan/test_leir_phase0
```

Expected: no sanitizer finding. Unsupported local TSan is recorded and run on
the Linux workflow.

- [ ] **Step 4: Add manual Linux research workflow**

The workflow:

- checks out the exact branch commit;
- installs GCC, Clang, liburing development headers, and Python 3.13;
- runs `make -j2 test-leir-phase0` with `-O3 -Werror`;
- runs ASan/UBSan and TSan tests;
- records `uname -a`, `lscpu`, compiler versions, Python version, and source
  SHA;
- selects the minimum allowed CPU and pins the server process with `taskset`;
- runs screen with five samples/100 ms or gate with nine samples/250 ms;
- compares raw and tracked reports byte-for-byte;
- uploads CSV, JSON, Markdown, environment, and stderr logs even on failure.

Use:

```yaml
on:
  workflow_dispatch:
    inputs:
      phase:
        required: true
        default: screen
        type: choice
        options: [screen, gate]
```

Set `permissions: contents: read` and `cancel-in-progress: false`.

- [ ] **Step 5: Verify native Windows compile and smoke**

On a native Windows host, use the repository's Windows verification path,
then select the LEIR tests from the generated CMake build:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/verify_windows.ps1 -Native
ctest --test-dir build-windows-native `
  --output-on-failure --timeout 180 -C Release `
  -R "test_leir_phase0|test_bench_leir_phase0"
```

Windows rows must remain non-gating until a child-process peer and server-only
CPU clock are implemented.

- [ ] **Step 6: Commit workflow and portability fixes**

```bash
git add .github/workflows/leir-research.yml \
  experiments/leir/leir_test_support.c \
  experiments/leir/leir_peer_process.c \
  CMakeLists.txt Makefile
git commit -m "ci: add LEIR Phase 0A evidence workflow"
```

---

### Task 9: Execute Evidence, Decide, And Continue

**Files:**

- Create: `docs/research/reports/2026-07-27-leir-phase0a-results.md`
- Create: `docs/research/reports/2026-07-27-leir-phase0a-decision.md`
- Create on pass:
  `docs/research/plans/2026-07-27-leir-phase0b-linux-linked-segments.md`
- Modify only if the evidence requires an integrity repair:
  `scripts/bench_leir_phase0.py`
- Modify only if the evidence requires an integrity repair:
  `scripts/test_bench_leir_phase0.py`
- Modify only if the evidence requires an integrity repair:
  `experiments/leir/bench_leir_phase0.c`

**Interfaces:**

- Consumes: complete Phase 0A samples and metadata.
- Produces one engineering decision and the next executable research step.

- [ ] **Step 1: Run the local screening matrix**

Run:

```bash
make leir-phase0a-screen
cmp \
  object/leir-phase0a-screen/leir_phase0a_report.md \
  docs/research/reports/2026-07-27-leir-phase0a-results.md
```

Expected: complete evidence or an explicit `INCONCLUSIVE` integrity reason.

- [ ] **Step 2: Review mechanism counters before interpreting ratios**

For every core cell verify:

- candidate task parks equal activations;
- candidate terminal publications equal activations;
- candidate intermediate task publications are zero;
- resumes avoided match intermediate pending completions;
- baseline and candidate checksums match;
- `pending_path_valid=1`;
- no heap request or hot allocation;
- no residual runtime pending I/O.

Do not classify performance if any mechanism counter fails.

- [ ] **Step 3: Commit and push the exact screened source**

```bash
git add docs/research/reports/2026-07-27-leir-phase0a-results.md
git commit -m "docs: record LEIR Phase 0A screening"
git push -u origin codex/leir-phase0
```

- [ ] **Step 4: Dispatch and collect authoritative Linux evidence**

Dispatch `.github/workflows/leir-research.yml` for `phase=screen`, wait for the
exact source SHA, download its artifact, and verify:

```bash
git rev-parse HEAD
shasum -a 256 .artifacts/leir-phase0a-screen-linux-x86_64/*
cmp \
  .artifacts/leir-phase0a-screen-linux-x86_64/leir_phase0a_report.md \
  .artifacts/leir-phase0a-screen-linux-x86_64/leir_phase0a_tracked_report.md
```

If the screen returns `ADVANCE_PASS`, dispatch `phase=gate` on the same frozen
implementation and thresholds. Do not tune after seeing the screen.

- [ ] **Step 5: Write the decision report**

The decision report records:

- source SHA, workflow run URL, artifact name, runner, kernel, CPU, compiler,
  flags, and exact command;
- formal Phase 0A verdict and integrity status;
- worst-cell wall, CPU, spread, latency, service-gap, and short-control values;
- task resumes, context switches, submissions, and completions removed or
  added;
- whether userspace advancement or direct built-in effects caused the result;
- why the result does or does not justify Phase 0B.

- [ ] **Step 6A: Continue to Phase 0B on `ADVANCE_PASS`**

Write the Phase 0B plan before touching linked-SQE code. It must isolate:

```text
LEIR userspace advance
versus
the identical LEIR program with eligible static segments lowered through
IOSQE_IO_LINK
versus
the same linked plan with safe IOSQE_CQE_SKIP_SUCCESS
```

It must retain the final `1.50x` wall, `0.70x` CPU, short-control, latency,
correctness, and `1.10x` spread gates from the design spec.

- [ ] **Step 6B: Continue to the next boundary on `ADVANCE_REJECT`**

Do not implement linked SQEs as LLAM's cross-platform identity. Preserve the
private seam only if it has an independently measured use; otherwise remove
it in a dedicated revert commit while retaining experiment evidence. The next
research design must cross a different cost boundary, such as compiler-owned
structured cancellation/resource regions, rather than another completion
object or continuation dispatch variant.

- [ ] **Step 6C: Repair integrity only on `INCONCLUSIVE`**

Change measurement code only when the report identifies a concrete integrity
defect such as insufficient duration, path misclassification, clock scope, or
unstable block granularity. Keep performance thresholds, workloads, and
candidate semantics unchanged. Add a regression test that fails on the
observed defect before changing the harness.

- [ ] **Step 7: Commit and push the decision**

```bash
git add docs/research/reports/2026-07-27-leir-phase0a-results.md \
  docs/research/reports/2026-07-27-leir-phase0a-decision.md \
  docs/research/plans/2026-07-27-leir-phase0b-linux-linked-segments.md
git commit -m "docs: decide LEIR Phase 0A advancement"
git push
```

Omit the Phase 0B path from `git add` when the verdict is not
`ADVANCE_PASS`.
