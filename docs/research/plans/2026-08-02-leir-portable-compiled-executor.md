<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
-->

# LEIR Portable Compiled Executor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Execute the bounded LEIR `CONNECT -> terminal WRITE` segment through
one generated backend-neutral module, one portable direct-C Executor, and the
Linux linked-SQE specialization while both adapters publish and consume the
same 48-byte LCCF representation B.

**Architecture:** LEIR remains the semantic partitioning contract. A generated
module binds stable inputs and exposes fixed prepare/resume entrypoints; a
portable or Linux adapter owns effects and publishes one normalized terminal
event; a common LCCF-backed consumer validates generation and ownership before
calling generated resume. The interpreter remains the differential oracle and
the explicit `GRAPH_BREAK` fallback, never the per-completion path for this
eligible segment.

**Tech Stack:** C11 and C atomics, generated C/C++17 interoperability, LLAM
portable I/O, Linux io_uring, Make, CMake/CTest, Python 3 `unittest`,
ASan/UBSan/TSan, and GitHub Actions.

## Global Constraints

- All new code-bearing files use
  `SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0`.
- Existing modified files retain their existing license identifier.
- LEIR remains the compiler/planner semantic contract; adapters cannot change
  edge meanings.
- The generated normal path contains no opcode loop and no per-completion
  interpreter dispatch.
- Representation B is the exact 48-byte LCCF shared event; no LEIR-local event
  clone is permitted.
- The same common terminal consumer performs generated resume for portable and
  Linux paths.
- No allocation is allowed after ticket initialization and bind for module,
  event, continuation, or adapter state.
- Backend ownership cannot fall back to the interpreter after an effect has
  been issued.
- Portable and Linux verdicts are independent; SQE/CQE claims never count as a
  portable result.
- Research code stays behind `LLAM_BUILD_RESEARCH=1`, outside installed headers,
  shared-library exports, packages, and the stable ABI.
- Do not change the project version, create a tag, publish a package, release
  3.0.0, or merge the held relicense branch.
- Keep the current stacked branch based on `leir-native-segment`; do not modify
  the user's dirty main checkout.

---

## File map

### Selected LCCF dependency

- `experiments/lccf/lccf_portable_errno.h`
  supplies experiment-local portable error constants.
- `experiments/lccf/lccf_fact.h` and `experiments/lccf/lccf_fact.c`
  provide generation-tagged ownership, release/acquire publication, guard
  rechecks, queue transfer, failure cleanup, and explicit rearm.
- `experiments/lccf/lccf_representation.h` and
  `experiments/lccf/lccf_representation.c`
  provide the selected 48-byte immutable shared-event representation.

### Generated-module contract

- `experiments/leir/leir_aot_module.h`
  distinguishes a backend-neutral module from concrete portable and Linux
  adapter kinds.
- `experiments/leir/leir_aot_connect_write_template.inc`
  validates either supported backend without adding an opcode interpreter.
- `scripts/gen_leir_aot_fixture.py`
  emits the checked-in module under the corrected research ABI.
- `scripts/test_gen_leir_aot_fixture.py`,
  `experiments/leir/test_leir_aot_module.c`, and
  `experiments/leir/fixtures/leir_aot_c_consumer.c`
  lock deterministic generation and C/C++ consumption.

### Common completion path

- `experiments/leir/leir_aot_completion.h` and
  `experiments/leir/leir_aot_completion.c`
  wrap the selected LCCF lifecycle and provide backend-independent terminal
  publication and generated resume.
- `experiments/leir/test_leir_aot_completion.c`
  covers success, malformed input, stale/duplicate delivery, queue transfer,
  module disable, cancellation, callback failure, and rearm.

### Portable Executor

- `experiments/leir/leir_aot_portable.h` and
  `experiments/leir/leir_aot_portable.c`
  implement caller-owned ticket state, injectable deterministic effect hooks,
  and the real `llam_connect`/`llam_write` adapter.
- `experiments/leir/test_leir_aot_portable.c`
  tests direct compiled control with deterministic hooks.
- `experiments/leir/test_leir_aot_integration.c`
  runs the real portable adapter and the Linux adapter where available against
  controlled peers and the interpreter oracle.

### Linux specialization

- `experiments/leir/leir_aot_linux.h` and
  `experiments/leir/leir_aot_linux.c`
  publish Linux terminal results through the common completion path instead of
  directly resuming generated code.
- `experiments/leir/test_leir_aot_linux_unit.c`,
  `experiments/leir/test_leir_aot_ownership.c`, and
  `experiments/leir/test_leir_aot_ring_profile.c`
  preserve linked-SQE, CQE suppression, cancellation, and independent ticket
  ownership contracts.

### Evidence, build, and documentation

- `experiments/leir/bench_leir_aot_connect.c`
  emits oracle, portable compiled, and Linux specialized observations.
- `scripts/bench_leir_aot_connect.py` and
  `scripts/test_bench_leir_aot_connect.py`
  classify portable and Linux axes independently.
- `Makefile`, `CMakeLists.txt`, and
  `.github/workflows/leir-aot-research.yml`
  expose only research targets and cross-platform checks.
- `scripts/test_research_build_boundary.py`
  proves default/package isolation.
- `docs/research/reports/2026-08-02-leir-portable-compiled-executor-results.md`
  records exact correctness and measurement receipts.
- `docs/research/reports/2026-08-02-leir-portable-compiled-executor-decision.md`
  records independent portable/Linux advancement decisions.
- `docs/operations/benchmarks.md`
  links the evidence without making a production or release claim.

---

### Task 1: Integrate the selected LCCF representation and lifecycle

**Files:**

- Create: `experiments/lccf/lccf_portable_errno.h`
- Create: `experiments/lccf/lccf_fact.h`
- Create: `experiments/lccf/lccf_fact.c`
- Create: `experiments/lccf/lccf_representation.h`
- Create: `experiments/lccf/lccf_representation.c`
- Test: `experiments/leir/test_leir_aot_completion.c` in Task 3

**Interfaces:**

- Consumes: the reviewed files at commit `5dd5b0343ca2f540daa519ab5bf1c368780c2dd4`.
- Produces: `lccf_event_core_t`, exactly 48 bytes;
  `lccf_fact_cell_init_representation()`;
  `lccf_fact_try_publish_configured()`; `lccf_fact_consume()`;
  `lccf_fact_invoke()`; `lccf_fact_finish()`; `lccf_fact_abort()`; and
  `lccf_fact_cell_arm()`.

- [x] **Step 1: Record and verify the exact dependency blobs**

Use these immutable Git blob identities before applying the files:

```text
experiments/lccf/lccf_fact.c              c9bb30c06148343f23f90b3eba1b5f2b208edcac
experiments/lccf/lccf_fact.h              397787294ac6989f36fc5936ef14ea55b9e4921c
experiments/lccf/lccf_representation.c    6d93d6fbeb1757508d4c4ca90349678645837e22
experiments/lccf/lccf_representation.h    f7586d75fb0401cbb525d44efe856c223f2f0465
experiments/lccf/lccf_portable_errno.h    ab152ecdf13e137da510c4d7de9ea18ae3cc410e
```

Run:

```bash
for file in \
  experiments/lccf/lccf_fact.c \
  experiments/lccf/lccf_fact.h \
  experiments/lccf/lccf_representation.c \
  experiments/lccf/lccf_representation.h \
  experiments/lccf/lccf_portable_errno.h; do
  git rev-parse "origin/research/lccf-cfs-phase0:$file"
done
```

Expected: the five hashes above, in order.

- [x] **Step 2: Apply only those five files**

Use file patches from the reviewed commit. Do not merge the older LCCF model,
benchmark, report, Makefile, CMake, or workflow deltas into this stacked branch.
After applying, run:

```bash
for file in \
  experiments/lccf/lccf_fact.c \
  experiments/lccf/lccf_fact.h \
  experiments/lccf/lccf_representation.c \
  experiments/lccf/lccf_representation.h \
  experiments/lccf/lccf_portable_errno.h; do
  printf '%s ' "$file"
  git hash-object "$file"
done
```

Expected: each local file has the corresponding blob identity from Step 1.

- [x] **Step 3: Prove default and package isolation**

Run:

```bash
python3 -m unittest scripts/test_research_build_boundary.py -v
make clean
make -j4
find object -path '*experiments/lccf/lccf_fact.o' -o \
            -path '*experiments/lccf/lccf_representation.o'
```

Expected: boundary tests pass and the final `find` prints nothing.

- [x] **Step 4: Commit the exact dependency import**

```bash
git add experiments/lccf/lccf_portable_errno.h \
  experiments/lccf/lccf_fact.h \
  experiments/lccf/lccf_fact.c \
  experiments/lccf/lccf_representation.h \
  experiments/lccf/lccf_representation.c
git commit -m "research: reuse shared completion representation"
```

---

### Task 2: Make the generated module backend-neutral

**Files:**

- Modify: `experiments/leir/leir_aot_module.h`
- Modify: `experiments/leir/leir_aot_connect_write_template.inc`
- Modify: `experiments/leir/leir_aot_linux.c`
- Modify: `experiments/leir/test_leir_aot_module.c`
- Modify: `experiments/leir/fixtures/leir_aot_c_consumer.c`
- Modify: `scripts/test_gen_leir_aot_fixture.py`
- Verify unchanged: `scripts/gen_leir_aot_fixture.py`
- Verify unchanged: `experiments/leir/generated/leir_aot_connect_write.h`
- Verify unchanged: `experiments/leir/generated/leir_aot_connect_write.c`

**Interfaces:**

- Produces concrete adapter identities while keeping the generated descriptor
  independent of either implementation:

```c
#define LEIR_AOT_MODULE_BACKEND_AGNOSTIC 0U
#define LEIR_AOT_BACKEND_LINUX_IO_URING 1U
#define LEIR_AOT_BACKEND_PORTABLE 2U
```

- `leir_aot_module_v1_t.backend_kind` is
  `LEIR_AOT_MODULE_BACKEND_AGNOSTIC`.
- `leir_aot_backend_v1_t.backend_kind` is one of the two concrete adapter
  identities.

- [x] **Step 1: Write failing backend-neutral module tests**

In `test_leir_aot_module.c`, run the same bound instance through two capture
backends and assert that both preparations carry identical fd, address,
payload, generation, and continuation values:

```c
CHECK(module->backend_kind == LEIR_AOT_MODULE_BACKEND_AGNOSTIC);
CHECK(prepare_with_kind(LEIR_AOT_BACKEND_PORTABLE) == 0);
CHECK(prepare_with_kind(LEIR_AOT_BACKEND_LINUX_IO_URING) == 0);
CHECK(prepare_with_kind(UINT32_MAX) == -1 && errno == EINVAL);
```

The production change that makes this pass is removing the Linux-only module
check while retaining exact backend validation.

- [x] **Step 2: Run the focused test and observe RED**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 test_leir_aot_module
./test_leir_aot_module
```

Expected: the module still advertises Linux and rejects the portable adapter.

- [x] **Step 3: Implement the minimal research ABI correction**

Add the three identities above. In generated prepare, accept only portable or
Linux concrete backends and keep all ABI version, structure size, callback,
bound-instance, and cancellation checks unchanged. Do not add a platform
header, opcode switch, or operation loop.

- [x] **Step 4: Update generator and standalone consumer contracts**

The generator test must compile both C11 and C++17 consumers, compare generated
bytes to the checked-in files, and reject generated output containing any of:

```text
switch (opcode)
for (node
leir_phase0_execute
malloc(
calloc(
realloc(
```

The standalone consumer prepares once with each concrete adapter and verifies
identical captured values.

- [x] **Step 5: Regenerate and verify GREEN**

Run:

```bash
python3 scripts/gen_leir_aot_fixture.py --root .
make LLAM_BUILD_RESEARCH=1 -j4 \
  test-leir-aot-module test-leir-aot-c-consumer
python3 -m unittest scripts/test_gen_leir_aot_fixture.py -v
```

Expected: module, C consumer, C++17 compilation, deterministic generation, and
absence checks pass.

- [x] **Step 6: Commit**

```bash
git add experiments/leir/leir_aot_module.h \
  experiments/leir/leir_aot_connect_write_template.inc \
  experiments/leir/leir_aot_linux.c \
  experiments/leir/test_leir_aot_module.c \
  experiments/leir/fixtures/leir_aot_c_consumer.c \
  scripts/test_gen_leir_aot_fixture.py
git commit -m "research: make generated LEIR module backend neutral"
```

---

### Task 3: Add the common LCCF-backed terminal consumer

**Files:**

- Create: `experiments/leir/leir_aot_completion.h`
- Create: `experiments/leir/leir_aot_completion.c`
- Create: `experiments/leir/test_leir_aot_completion.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`
- Modify: `config/llam-sources.json`
- Modify: `scripts/audit_build_manifests.py`

**Interfaces:**

```c
typedef enum leir_aot_completion_route {
    LEIR_AOT_COMPLETION_DIRECT = 0,
    LEIR_AOT_COMPLETION_QUEUE = 1,
} leir_aot_completion_route_t;

typedef struct leir_aot_completion_record {
    uint64_t generation;
    uint64_t stable_flags;
    int64_t result;
    uint64_t payload_word;
    uint32_t captured_home_shard;
    uint32_t source_node;
    uint32_t continuation;
    uint32_t source_kind;
    uint32_t event_kind;
    int32_t error_code;
} leir_aot_completion_record_t;

typedef struct leir_aot_completion_metrics {
    uint64_t publications;
    uint64_t normalizations;
    uint64_t site_lookups;
    uint64_t direct_consumes;
    uint64_t queued_consumes;
    uint64_t duplicate_rejections;
    uint64_t stale_rejections;
    uint64_t aborts;
} leir_aot_completion_metrics_t;

typedef struct leir_aot_completion leir_aot_completion_t;

size_t leir_aot_completion_size(void);
size_t leir_aot_completion_alignment(void);
int leir_aot_completion_init(
    void *storage, size_t storage_size,
    const leir_aot_module_v1_t *module,
    void *module_instance, size_t module_instance_size);
int leir_aot_completion_arm(
    leir_aot_completion_t *completion, uint64_t generation);
int leir_aot_completion_publish(
    leir_aot_completion_t *completion,
    const leir_aot_completion_record_t *record);
int leir_aot_completion_consume(
    leir_aot_completion_t *completion,
    uint64_t generation,
    leir_aot_completion_route_t route,
    uint64_t guard_flags,
    leir_phase0_value_t *values_out,
    size_t value_count,
    leir_aot_resume_result_v1_t *resume_out);
int leir_aot_completion_cancel(
    leir_aot_completion_t *completion, uint32_t continuation);
int leir_aot_completion_set_module_available(
    leir_aot_completion_t *completion, bool available);
int leir_aot_completion_destroy(leir_aot_completion_t *completion);
void leir_aot_completion_metrics(
    const leir_aot_completion_t *completion,
    leir_aot_completion_metrics_t *out);
```

The opaque object contains one `lccf_fact_cell_t`, one aligned
`lccf_event_core_t`, a fixed descriptor table for the generated continuations,
the module binding, atomics for module availability/cancellation, and
metrics. There is no heap ownership.

Output storage belongs to each consume transaction rather than the completion
object, matching the portable and Linux ticket APIs that receive their output
buffers at run time.

Cancellation also carries an explicit generated continuation; the common
consumer therefore never assumes that a module's continuation numbering starts
at a particular value.

The explicit consume generation is part of the queue-delivery contract: a
delayed delivery from an older generation must not claim a newly armed cell.
Queue transfer or deferral returns `EAGAIN`; duplicate publication or consume
returns `EBUSY` and increments the duplicate-rejection metric.

- [x] **Step 1: Write RED tests for exact B publication**

Add a spy module and assert:

```c
CHECK(leir_aot_completion_init(...) == 0);
CHECK(leir_aot_completion_arm(completion, 7U) == 0);
CHECK(leir_aot_completion_publish(completion, &record) == 0);
CHECK(metrics.publications == 1U);
CHECK(metrics.normalizations == 1U);
CHECK(sizeof(lccf_event_core_t) == 48U);
```

Publish must not invoke resume. The test fails initially because the common
completion API and target do not exist.

- [x] **Step 2: Run RED**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 test_leir_aot_completion
```

Expected: the target or required symbols are missing.

- [x] **Step 3: Add research-only object groups**

Add this common source group to Make and CMake without adding an installed or
runtime-library source:

```make
LEIR_AOT_COMPLETION_OBJS = \
	$(OBJDIR)/experiments/lccf/lccf_fact.o \
	$(OBJDIR)/experiments/lccf/lccf_representation.o \
	$(OBJDIR)/experiments/leir/leir_aot_completion.o
```

```cmake
add_executable(test_leir_aot_completion
    experiments/lccf/lccf_fact.c
    experiments/lccf/lccf_representation.c
    experiments/leir/leir_aot_completion.c
    experiments/leir/test_leir_aot_completion.c
)
```

Only completion, portable, Linux, integration, and benchmark research targets
link this group. Production targets do not.

- [x] **Step 4: Implement init, arm, and publication**

`init` records module/output bindings but leaves the cell unarmed. The first
`arm(generation)` initializes the LCCF cell with `LCCF_REP_SHARED_EVENT`, one
backend ticket, and caller-owned event storage; later arms require a consumed
terminal generation and call `lccf_fact_cell_arm()`. Build an
`lccf_fact_ticket_t` with
`lccf_fact_ticket_from_logical()`, then publish only through
`lccf_fact_try_publish_configured()`. Copy LCCF normalization and site-lookup
counters into completion metrics after each operation.

- [x] **Step 5: Write RED tests for common direct/queue resume**

Cover these literal behaviors:

```text
direct success       resume once, copy outputs once, terminal reusable
forced queue         direct returns queue route, queued consume resumes once
duplicate consume    second claim rejected, resume count remains one
stale generation     publish/consume rejected, newer event bytes unchanged
module unavailable   queued/deferred, resume not called until re-enabled
cancel before claim  generated cancel observed and ECANCELED result retained
callback failure     abort drops every owned reference and permits rearm
malformed event      immutable FAIL/EPROTO reaches generated failure result
```

Expected values are literal counters and output slots, not values computed by
the implementation helper.

- [x] **Step 6: Implement the one consumer transaction**

Use `lccf_fact_consume()` for direct or queued admission, invoke only through
`lccf_fact_invoke()`, copy outputs only after generated resume succeeds, then
call `lccf_fact_finish()`. On resume or copy failure call `lccf_fact_abort()`.
Queue transfer uses the LCCF decision and queue reference; it does not rebuild
or renormalize the event.

- [x] **Step 7: Verify GREEN and sanitizers**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 test-leir-aot-completion

clang -Iinclude -Isrc/internal -Isrc -Iexperiments/lccf \
  -Iexperiments/leir -DLLAM_BUILD_RESEARCH=1 \
  -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE \
  -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g \
  -fno-omit-frame-pointer -fsanitize=address,undefined \
  experiments/lccf/lccf_fact.c \
  experiments/lccf/lccf_representation.c \
  experiments/leir/leir_aot_completion.c \
  experiments/leir/test_leir_aot_completion.c \
  -pthread -o /tmp/llam-leir-aot-completion-sanitize
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/llam-leir-aot-completion-sanitize
```

Expected: every lifecycle case passes with no sanitizer finding.

- [x] **Step 8: Commit**

```bash
git add experiments/leir/leir_aot_completion.h \
  experiments/leir/leir_aot_completion.c \
  experiments/leir/test_leir_aot_completion.c Makefile CMakeLists.txt \
  config/llam-sources.json scripts/audit_build_manifests.py
git commit -m "research: add common compiled completion consumer"
```

---

### Task 4: Implement the portable direct-C Executor

**Files:**

- Create: `experiments/leir/leir_aot_portable.h`
- Create: `experiments/leir/leir_aot_portable.c`
- Create: `experiments/leir/test_leir_aot_portable.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`
- Modify: `config/c-structure-baseline.json`
- Modify: `config/llam-sources.json`
- Modify: `scripts/audit_build_manifests.py`

**Interfaces:**

```c
typedef int (*leir_aot_portable_connect_fn)(
    void *context, llam_fd_t fd,
    const struct sockaddr *address, socklen_t address_length);
typedef ssize_t (*leir_aot_portable_write_fn)(
    void *context, llam_fd_t fd, const void *payload, size_t payload_length);

typedef struct leir_aot_portable_effects {
    void *context;
    leir_aot_portable_connect_fn connect;
    leir_aot_portable_write_fn write;
} leir_aot_portable_effects_t;

typedef struct leir_aot_portable_metrics {
    uint64_t activations;
    uint64_t logical_operations;
    uint64_t effect_calls;
    uint64_t terminal_publications;
    uint64_t interpreter_dispatches;
    uint64_t normalizations;
    uint64_t site_lookups;
    uint64_t hot_allocations;
    uint64_t generation;
    uint32_t resumed_continuation;
} leir_aot_portable_metrics_t;

typedef struct leir_aot_portable_ticket leir_aot_portable_ticket_t;

size_t leir_aot_portable_ticket_size(void);
size_t leir_aot_portable_ticket_alignment(void);
int leir_aot_portable_ticket_init(
    void *ticket_storage, size_t ticket_storage_size,
    const leir_aot_module_v1_t *module,
    void *module_instance, size_t module_instance_size,
    const leir_aot_portable_effects_t *effects);
int leir_aot_portable_ticket_bind(
    leir_aot_portable_ticket_t *ticket,
    const leir_phase0_value_t *values, size_t value_count);
int leir_aot_portable_ticket_run(
    leir_aot_portable_ticket_t *ticket,
    leir_phase0_value_t *values_out, size_t value_count,
    leir_aot_resume_result_v1_t *resume_out,
    leir_aot_portable_metrics_t *metrics_out);
int leir_aot_portable_ticket_cancel(
    leir_aot_portable_ticket_t *ticket);
int leir_aot_portable_ticket_destroy(
    leir_aot_portable_ticket_t *ticket);
```

Passing `effects == NULL` selects adapters that call `llam_connect()` and
`llam_write()` directly. Tests inject deterministic hooks below those wrappers.

- [x] **Step 1: Write RED state-machine and hook tests**

Use caller-owned aligned buffers and fixed hook scripts. Cover:

```text
connect success, partial write    connect then write, terminal WRITE result
connect refusal                   no write call, CONNECT_ERROR continuation
write failure                     WRITE_RESULT with negative errno result
zero non-empty write              WRITE_RESULT with EIO
oversized write result            WRITE_RESULT with EPROTO
cancel while bound                no effect call, ECANCELED terminal result
cancel from connect hook          no write call, ECANCELED terminal result
destroy while running             EBUSY
bind after consumed               generation advances and old delivery stale
```

Every successful row asserts `interpreter_dispatches == 0`,
`hot_allocations == 0`, one terminal publication, and one normalization.

- [x] **Step 2: Run RED**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 test_leir_aot_portable
```

Expected: target or portable ticket symbols are missing.

- [x] **Step 3: Implement prepare capture and direct control**

The portable backend callback copies no payload or address: generated bind
already owns those bytes. It records the generated pointers, lengths,
generation, and two continuations while the ticket is in `RUNNING`.

Execute exactly:

```c
connect_result = effects.connect(...);
if (connect_result != 0) {
    continuation = connect_error_continuation;
    result = -(int64_t)saved_errno;
} else {
    write_result = effects.write(...);
    continuation = write_continuation;
    result = normalize_write_result(write_result, payload_length);
}
```

There is no loop over LEIR nodes. Once an effect hook is called, every outcome
publishes a terminal record; none transfers to the interpreter.

Cancellation is encoded in the ticket state (`BOUND_CANCELLED` and
`RUNNING_CANCELLED`). The execution owner closes the acceptance window with
`RUNNING[_CANCELLED] -> COMPLETING`, then performs the generated module cancel
and terminal publication itself. A cancelling thread never mutates generated
module state concurrently with resume.

- [x] **Step 4: Implement common publication and consume**

Arm the embedded completion using the generation supplied by generated
prepare. Publish a portable-source IO or cancel record, consume it through the
common direct path, and obtain output slots only through that consumer. Copy
normalization and lookup counts into portable metrics.

- [x] **Step 5: Verify GREEN and mutation boundaries**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 \
  test-leir-aot-module test-leir-aot-completion test-leir-aot-portable
rg -n "switch[[:space:]]*\(.*opcode|leir_phase0_execute" \
  experiments/leir/leir_aot_portable.c \
  experiments/leir/leir_aot_connect_write_template.inc
```

Expected: tests pass and the search returns no match.

- [x] **Step 6: Commit**

```bash
git add experiments/leir/leir_aot_portable.h \
  experiments/leir/leir_aot_portable.c \
  experiments/leir/test_leir_aot_portable.c Makefile CMakeLists.txt \
  config/c-structure-baseline.json config/llam-sources.json \
  scripts/audit_build_manifests.py
git commit -m "research: execute generated LEIR effects portably"
```

---

### Task 5: Prove real portable LLAM integration and oracle parity

**Files:**

- Modify: `experiments/leir/test_leir_aot_integration.c`
- Modify: `experiments/leir/leir_test_support.h`
- Modify: `experiments/leir/leir_test_support.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: `leir_aot_portable_ticket_*`, the existing generated module,
  controlled listener/peer fixtures, and the Phase 0 interpreter oracle.
- Produces: one platform-portable integration matrix; Linux additionally runs
  the specialized adapter over equivalent inputs.

- [ ] **Step 1: Write a failing portable integration row**

Inside an LLAM task, bind a portable ticket with `effects == NULL`, connect to
the controlled listener, write a literal payload, and assert:

```text
resume action          RETURN
write result slot      exact payload length
peer bytes             exact payload bytes
interpreter dispatches 0
normalizations         1
hot allocations        0
```

Run the same initial slots through the interpreter oracle and compare action,
error, every output slot, and peer-visible bytes.

- [ ] **Step 2: Run RED**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 test_leir_aot_integration
./test_leir_aot_integration
```

Expected: the test fails because only the current Linux ticket is wired.

- [ ] **Step 3: Add cross-platform success and refusal fixtures**

Reuse the existing listener lifetime and Winsock setup helpers. Add separate
connections for oracle, portable compiled, and Linux specialized paths so no
candidate reuses a side effect. On non-Linux platforms, require oracle and
portable compiled rows; only the Linux row may report `ENOTSUP`.

- [ ] **Step 4: Add cancellation and out-of-order ownership rows**

Run two portable tickets with distinct module/event storage. Complete the
second first, cancel the first, and assert independent generation, output,
module lifetime, publication, and terminal wake counters. A stale delivery for
the cancelled ticket cannot mutate the successful ticket.

- [ ] **Step 5: Verify the real integration matrix**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 test-leir-aot-integration

cmake -S . -B build-leir-portable \
  -DLLAM_BUILD_RESEARCH=ON -DLLAM_BUILD_TESTS=ON
cmake --build build-leir-portable -j4 --target \
  test_leir_aot_integration test_leir_aot_portable
ctest --test-dir build-leir-portable --output-on-failure \
  -R 'test_leir_aot_(portable|integration)'
```

Expected: portable success, refusal, cancellation, and ownership pass; Linux
runs where supported and otherwise skips only its own row.

- [ ] **Step 6: Commit**

```bash
git add experiments/leir/test_leir_aot_integration.c \
  experiments/leir/leir_test_support.h \
  experiments/leir/leir_test_support.c Makefile CMakeLists.txt
git commit -m "test: prove portable compiled LEIR parity"
```

---

### Task 6: Route Linux terminal results through the common consumer

**Files:**

- Modify: `experiments/leir/leir_aot_linux.h`
- Modify: `experiments/leir/leir_aot_linux.c`
- Modify: `experiments/leir/test_leir_aot_linux_unit.c`
- Modify: `experiments/leir/test_leir_aot_ownership.c`
- Modify: `experiments/leir/test_leir_aot_ring_profile.c`
- Modify: `experiments/leir/test_leir_aot_integration.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

**Interfaces:**

- `leir_aot_linux_ticket_t` embeds caller-owned common completion state.
- `leir_aot_linux_metrics_t` adds:

```c
uint64_t terminal_publications;
uint64_t normalizations;
uint64_t site_lookups;
uint64_t interpreter_dispatches;
```

- Existing SQE/CQE, park/wake, queue publication, allocation, continuation,
  and timing fields retain their meanings.

- [ ] **Step 1: Write RED tests that forbid direct Linux resume**

Use a spy module whose resume verifies that a published B event and common
consumer claim already exist. Cover connect error, write success, write error,
cancellation, and duplicate terminal delivery. Assert:

```text
terminal publications  1
normalizations          1
resume calls            1
interpreter dispatches  0
successful linked SQEs  2
successful observed CQEs 1
suppressed CQEs         1
```

- [ ] **Step 2: Run RED**

Run on Linux or the existing Linux unit fixture:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 \
  test_leir_aot_linux_unit test_leir_aot_ownership
./test_leir_aot_linux_unit
./test_leir_aot_ownership
```

Expected: direct `module->resume()` bypasses the required publication receipt.

- [ ] **Step 3: Replace direct resume with publish/consume**

After the segment reaches semantic terminal and ownership is retired, create a
Linux-source completion record from `first_error_index`, `first_error`, and
`semantic_result`. Publish it once, release request/segment resources, then use
the same common consumer as the portable adapter. Remove the direct
`module->resume()` and `module->copy_outputs()` calls from the Linux adapter.

- [ ] **Step 4: Preserve Linux retirement and cancellation contracts**

Do not alter SQE flags or kernel retirement semantics. Connect remains
`IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS`; the final write keeps its CQE. Early
connect failure selects its continuation; successful connect remains
unobservable. Cancellation is terminal only after all required target and
cancel CQEs retire.

- [ ] **Step 5: Run Linux regression and sanitizer tests**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 \
  test-leir-native-linux test-leir-aot-integration \
  test-leir-aot-ring-profile

cmake --build build-leir-portable -j4 --target \
  test_leir_aot_linux_unit test_leir_aot_ownership \
  test_leir_aot_ring_profile test_leir_aot_integration
ctest --test-dir build-leir-portable --output-on-failure \
  -R 'test_leir_(native_linux|aot_linux_unit|aot_ownership|aot_ring_profile|aot_integration)'
```

Expected: ownership and ring-profile receipts remain exact, while every Linux
terminal row records one B publication and zero interpreter dispatches.

- [ ] **Step 6: Commit**

```bash
git add experiments/leir/leir_aot_linux.h \
  experiments/leir/leir_aot_linux.c \
  experiments/leir/test_leir_aot_linux_unit.c \
  experiments/leir/test_leir_aot_ownership.c \
  experiments/leir/test_leir_aot_ring_profile.c \
  experiments/leir/test_leir_aot_integration.c Makefile CMakeLists.txt
git commit -m "research: share terminal consumption across LEIR adapters"
```

---

### Task 7: Produce independent portable and Linux evidence

**Files:**

- Modify: `experiments/leir/bench_leir_aot_connect.c`
- Modify: `scripts/bench_leir_aot_connect.py`
- Modify: `scripts/test_bench_leir_aot_connect.py`
- Create: `docs/research/reports/2026-08-02-leir-portable-compiled-executor-results.md`
- Create: `docs/research/reports/2026-08-02-leir-portable-compiled-executor-decision.md`
- Modify: `docs/operations/benchmarks.md`

**Interfaces:**

Every raw row contains these common fields:

```text
candidate, transport, workload, process, block, order, seed,
activations, logical_operations, result_checksum, peer_checksum,
wall_ns, cpu_ns, p50_ns, p99_ns, interpreter_dispatches,
normalizations, site_lookups, parks, wakes, hot_allocations
```

Linux rows additionally contain:

```text
prepared_sqes, observed_cqes, suppressed_success_cqes,
queue_publications, submit_syscalls
```

- [ ] **Step 1: Write failing parser and classifier tests**

Add literal fixtures that prove:

```text
portable result ignores every SQE/CQE field
Linux result cannot upgrade a failed portable result
checksum, output, or ownership mismatch is FAIL
compiled rows require interpreter_dispatches == 0
B rows require normalizations == activations
successful Linux rows require SQEs == 2 * activations
successful Linux rows require observed CQEs == activations
successful Linux rows require suppressed CQEs == activations
missing/duplicate/unknown fields and non-finite ratios are rejected
any non-PASS enforcement decision exits nonzero
```

- [ ] **Step 2: Run RED**

Run:

```bash
python3 -m unittest scripts/test_bench_leir_aot_connect.py -v
```

Expected: the current single-axis schema cannot express the portable compiled
candidate or independent decisions.

- [ ] **Step 3: Extend the C benchmark without changing workloads**

Run the same deterministic activation inputs through the interpreter oracle,
portable compiled B, and Linux linked B. Candidate setup, listener creation,
payload allocation, ticket initialization, and calibration stay outside the
measured window. Emit exact work and ownership counters for every row.

- [ ] **Step 4: Implement two classifiers**

The portable classifier compares oracle to portable compiled B and reports
correctness, dispatch removal, normalization, site lookup, park/wake,
allocation, wall, CPU, p50, and p99 evidence. The Linux classifier compares
portable compiled B to Linux linked B and separately reports SQE/CQE,
submission, park/wake, wall, CPU, p50, and p99 evidence.

Both use predeclared ABBA/BAAB paired windows, a minimum measured duration,
ratio-spread/confidence checks, and exact machine-readable rejection reasons.

- [ ] **Step 5: Run contract tests and bounded evidence**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 bench_leir_aot_connect
python3 -m unittest scripts/test_bench_leir_aot_connect.py -v
python3 scripts/bench_leir_aot_connect.py \
  --binary ./bench_leir_aot_connect \
  --output-dir object/leir-portable-compiled-executor \
  --samples 5
python3 scripts/bench_leir_aot_connect.py \
  --audit-only \
  --output-dir object/leir-portable-compiled-executor
```

Expected: audit reproduces every summary byte for byte. A negative or
inconclusive performance verdict is retained honestly and does not invalidate
correctness receipts.

- [ ] **Step 6: Write separate result and decision records**

Record exact commit, host/kernel/compiler, raw artifact path, matrix size,
minimum window, correctness counts, independent verdicts, and limitations.
State explicitly that neither decision authorizes production, version change,
tagging, packaging, or a 3.0.0 release.

- [ ] **Step 7: Verify documentation and commit**

Run:

```bash
git diff --check
python3 -m mkdocs build --strict --site-dir /tmp/llam-leir-portable-site
```

Then commit:

```bash
git add experiments/leir/bench_leir_aot_connect.c \
  scripts/bench_leir_aot_connect.py \
  scripts/test_bench_leir_aot_connect.py \
  docs/research/reports/2026-08-02-leir-portable-compiled-executor-results.md \
  docs/research/reports/2026-08-02-leir-portable-compiled-executor-decision.md \
  docs/operations/benchmarks.md
git commit -m "bench: separate portable and Linux LEIR evidence"
```

---

### Task 8: Lock research boundaries and complete verification

**Files:**

- Modify: `Makefile`
- Modify: `CMakeLists.txt`
- Modify: `.github/workflows/leir-aot-research.yml`
- Modify: `scripts/test_research_build_boundary.py`
- Modify: `docs/research/plans/2026-08-02-leir-portable-compiled-executor.md`

**Interfaces:**

- Produces Make targets:

```text
test-leir-aot-completion
test-leir-aot-portable
test-leir-aot-integration
test-leir-aot-ring-profile
test-leir-aot-connect-screen
```

- Produces matching CMake/CTest targets only when
  `LLAM_BUILD_RESEARCH=ON`.

- [ ] **Step 1: Write failing build-boundary tests**

Add behavioral tests that configure/build default and research trees and
assert:

```text
default build has no new research executable or object
research build exposes every new test target
package with research enabled is rejected
installed headers contain no LCCF/LEIR research ABI
shared-library exports contain no new research symbol
generated output uses the new license identifier
```

- [ ] **Step 2: Run RED**

Run:

```bash
python3 -m unittest scripts/test_research_build_boundary.py -v
```

Expected: new targets are not yet fully represented in both build systems and
workflow receipts.

- [ ] **Step 3: Complete Make, CMake, and CI integration**

Add the new tests to `research-test`, CTest, sanitizer, Windows compile/run,
macOS, BSD, Linux x86_64, and Linux aarch64 jobs. Linux-only execution may skip
only on explicit kernel/sandbox capability errors; portable completion and
portable Executor tests never skip by platform.

- [ ] **Step 4: Run the complete local verification matrix**

Run from a clean tree after committing implementation batches:

```bash
make clean
make -j4 LLAM_BUILD_RESEARCH=1 research-test
python3 -m unittest discover -s scripts -p 'test_*.py' -v
python3 scripts/audit_build_manifests.py --root . --check
python3 scripts/audit_license_headers.py --root .
python3 scripts/check_c_structure.py --root .
git diff --check

cmake --fresh -S . -B build-leir-portable-final \
  -DLLAM_BUILD_RESEARCH=ON -DLLAM_BUILD_TESTS=ON
cmake --build build-leir-portable-final -j4
ctest --test-dir build-leir-portable-final --output-on-failure
```

Expected: all applicable checks pass with no warning treated as an ignored
failure.

- [ ] **Step 5: Run sanitizer and race receipts**

Run the focused completion/portable/generated tests under ASan+UBSan on the
local host and TSan on Linux. Exercise at least 2,000 generations for
publish/consume/rearm and at least two concurrently completing tickets.
Expected: zero sanitizer findings, stale mutations, duplicate resumes,
reference imbalance, or hot allocations.

- [ ] **Step 6: Re-run build boundary and strict documentation**

Run:

```bash
python3 -m unittest scripts/test_research_build_boundary.py -v
python3 -m mkdocs build --strict --site-dir /tmp/llam-leir-portable-final-site
```

Expected: research isolation and all documentation links pass.

- [ ] **Step 7: Update this plan's checkboxes and commit verification wiring**

Mark only executed and evidenced steps complete. Then commit:

```bash
git add Makefile CMakeLists.txt .github/workflows/leir-aot-research.yml \
  scripts/test_research_build_boundary.py \
  docs/research/plans/2026-08-02-leir-portable-compiled-executor.md
git commit -m "ci: verify portable compiled LEIR research"
```

- [ ] **Step 8: Push a stacked draft PR and wait for green CI**

Push `research/leir-portable-executor` and open a draft PR targeting
`leir-native-segment`. The title and body describe the portable compiled path,
common B event, Linux specialization, independent verdicts, and the explicit
3.0.0 hold. Do not merge while its base chain is held.

Monitor every required check. Fix any repository-caused failure on this branch,
rerun its focused test locally, push the fix, and wait until all required checks
are green or an external infrastructure failure is conclusively identified.

---

## Completion audit

Before declaring this phase complete, map each requirement to direct evidence:

| Requirement | Required evidence |
|---|---|
| One module binds both adapters | module and standalone C/C++ tests |
| No per-completion interpreter | source absence gate plus runtime counter zero |
| Exact representation B reused | blob identity, 48-byte assertion, publication test |
| One normalization per terminal activation | unit, integration, and raw-row equations |
| Common portable/Linux consumer | spy receipt and absence of direct Linux resume |
| Generation and single-consumer safety | stale, duplicate, race, and sanitizer tests |
| Portable semantic parity | oracle/portable action, errno, slots, and peer bytes |
| Linux semantic parity | oracle/Linux action, errno, slots, and peer bytes |
| Linux CQE reduction | two SQEs, one observed CQE, one suppressed CQE per success |
| Portable/Linux result separation | parser fixtures and two independent verdicts |
| No public or package exposure | default/package/install/export boundary tests |
| No release | unchanged version/tag/package state and explicit report statement |

Completion is unproven if any row lacks its named artifact or passing command.
