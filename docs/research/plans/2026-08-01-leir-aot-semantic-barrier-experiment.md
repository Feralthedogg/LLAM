# LEIR AOT Semantic-Barrier Experiment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove that LLAM can compile a LEIR `CONNECT -> WRITE` path into a
real backend-native AOT module, preserve portable semantics and independent
completion ownership, and reduce successful Linux io_uring completion traffic
without promoting the rejected fixed receive-send pipeline.

**Architecture:** The LEIR compiler validates typed effect nodes, partitions
success edges at semantic barriers, and emits a versioned C module with direct
bind, prepare, and resume functions. The Linux module emits a two-SQE linked
connect-write segment, while every invocation retains its own completion
owner. The existing portable engine remains the fallback and correctness
oracle.

**Tech Stack:** C11, C atomics, LLAM private I/O request ABI, liburing/io_uring,
CMake, Make, Python 3 `unittest`, GitHub Actions on Ubuntu 24.04.

## Global Constraints

- Research builds remain behind `LLAM_BUILD_RESEARCH=1` and cannot be packaged.
- Do not change the public LLAM ABI, version files, release notes, tags, or
  release workflows.
- Every new source file uses
  `SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0`.
- Generated code has no opcode loop or per-completion node-table interpreter.
- Link only edges whose kernel success predicate equals the LEIR predicate.
- `READ_EXACT`, `WRITE_ALL`, result-dependent control flow, and ownership
  transitions are completion barriers.
- Independent invocations have independent terminal claims and wakes; only SQE
  submission is coalesced.
- The final SQE always retains a CQE and is the normal retirement fence.
- No allocation in bind-after-activation, prepare, completion, cancellation,
  or resume paths.
- Portable fallback and Linux specialization results are reported separately.
- The mechanism screen cannot authorize 3.0.0. Promotion requires the stricter
  gate in the design specification and successful full CI.

---

## File map

### Contract and portable semantics

- `experiments/leir/leir_phase0.h`
  adds the experimental connect opcode.
- `experiments/leir/leir_program.c`
  validates connect slots and edges.
- `experiments/leir/leir_engine.c`
  prepares and reduces portable connect requests.
- `experiments/leir/test_leir_phase0.c`
  verifies descriptor, result, error, cancellation, and fallback behavior.

### AOT compiler and fixture

- `experiments/leir/leir_aot_plan.h`
  defines edge classes, segments, module ABI version, and compile diagnostics.
- `experiments/leir/leir_aot_plan.c`
  classifies edges and creates maximal safe segments.
- `experiments/leir/test_leir_aot_plan.c`
  locks semantic-barrier behavior.
- `scripts/gen_leir_aot_fixture.py`
  emits deterministic C for the bounded connect-write fixture.
- `experiments/leir/generated/leir_aot_connect_write.c`
  is the checked-in generated interoperability artifact.
- `experiments/leir/generated/leir_aot_connect_write.h`
  publishes its module descriptor.
- `experiments/leir/fixtures/leir_aot_c_consumer.c`
  builds and runs against the generated ABI without the LLAM runtime library.
- `scripts/test_gen_leir_aot_fixture.py`
  verifies deterministic generation, the new license identifier, stale
  checked-in output, and C++17 consumption without private headers.

### Linux execution

- `src/io/linux/runtime_io_segment_linux_internal.h`
  adds connect operation metadata and per-owner AOT ticket state.
- `src/io/linux/watch/linux_segment.c`
  validates a connect segment and routes completion by generation.
- `src/io/linux/watch/linux_segment_queue.c`
  coalesces independent tickets without joining retirement.
- `src/io/linux/watch/linux_segment_submit.c`
  emits connect and send SQEs with the required link/skip flags.
- `src/io/linux/watch/linux_segment_reducer.c`
  maps early connect error or final write CQE to the generated resume point.
- `src/io/linux/watch/linux_segment_complete.c`
  wakes exactly one independent owner after full retirement.
- `experiments/leir/leir_aot_linux.c`
  binds a generated module to the private Linux segment API.
- `experiments/leir/test_leir_native_linux.c`
  verifies SQE flags, reducer results, generation, cancellation, and ownership.
- `experiments/leir/test_leir_aot_integration.c`
  runs portable and Linux modules against controlled peers.

### Benchmark and evidence

- `experiments/leir/bench_leir_aot_connect.c`
  measures controlled AF_UNIX and loopback TCP connect-write workloads.
- `scripts/bench_leir_aot_connect.py`
  runs the matrix and emits raw, summary, metadata, and verdict artifacts.
- `scripts/test_bench_leir_aot_connect.py`
  locks parsing and verdict rules.
- `.github/workflows/leir-aot-research.yml`
  runs correctness, sanitizer, stress, mechanism screen, and artifact upload.
- `docs/research/reports/2026-08-01-leir-native-pipeline-decision.md`
  records the rejected receive-send candidate.
- `docs/operations/benchmarks.md`
  links the new specialized experiment without making a portable claim.

---

### Task 1: Freeze the rejected pipeline decision

**Files:**
- Create: `docs/research/reports/2026-08-01-leir-native-pipeline-decision.md`
- Modify: `docs/operations/benchmarks.md`

- [ ] **Step 1: Write the decision record**

Record the immutable verdict, environment identifiers, representative ratios,
structural counters, and exact reason that all non-fixed `link_skip` cells were
unavailable. State that it rejects only the fixed receive-send candidate.

- [ ] **Step 2: Link the record from benchmark documentation**

Add a short “LEIR native pipeline” entry that labels both portable and platform
verdicts `REJECT` and points to the decision record. Do not generalize the
result to AOT or to non-Linux platforms.

- [ ] **Step 3: Check documentation integrity**

Run:

```bash
git diff --check
rg -n "REJECT|exact_result_semantic_barrier|3\.0\.0" \
  docs/research/reports/2026-08-01-leir-native-pipeline-decision.md \
  docs/operations/benchmarks.md
```

Expected: no whitespace errors and every decision term is present.

---

### Task 2: Add typed portable CONNECT semantics

**Files:**
- Modify: `experiments/leir/leir_phase0.h`
- Modify: `experiments/leir/leir_program.c`
- Modify: `experiments/leir/leir_engine.c`
- Modify: `experiments/leir/test_leir_phase0.c`

**Contract:**

```c
typedef enum leir_phase0_opcode {
    /* existing values remain stable */
    LEIR_PHASE0_OP_CONNECT = 6,
} leir_phase0_opcode_t;
```

Connect uses `FD`, `CONST_BUFFER`, `U64`, and `I64` for its fd, address,
address-length, and result slots. `on_eof` must be `LEIR_PHASE0_NODE_NONE`.

- [ ] **Step 1: Write failing descriptor tests**

Add a valid connect-write graph, then mutate each slot kind and edge. Verify
that the valid descriptor is copied and every malformed descriptor returns
`EINVAL` without publishing a program.

- [ ] **Step 2: Run the focused test and observe failure**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 test_leir_phase0
./test_leir_phase0
```

Expected: the valid connect descriptor is rejected because the opcode is not
yet recognized.

- [ ] **Step 3: Add program validation**

Implement a dedicated validator rather than weakening read/write validation:

```c
static bool validate_connect_node(
    const leir_phase0_program_t *program,
    const leir_phase0_node_desc_t *node) {
    return slot_is(program, node->fd_slot, LEIR_PHASE0_SLOT_FD) &&
           slot_is(program, node->buffer_slot,
                   LEIR_PHASE0_SLOT_CONST_BUFFER) &&
           slot_is(program, node->length_slot, LEIR_PHASE0_SLOT_U64) &&
           slot_is(program, node->result_slot, LEIR_PHASE0_SLOT_I64) &&
           edge_is_valid(program, node->on_success) &&
           node->on_eof == LEIR_PHASE0_NODE_NONE &&
           edge_is_valid(program, node->on_error);
}
```

- [ ] **Step 4: Write failing portable execution tests**

Use a controlled local listener. Assert connect success stores `0`, refusal
stores `-1` and selects `on_error`, cancellation publishes once, and a stale
completion cannot mutate a rebound activation.

- [ ] **Step 5: Implement request preparation and reduction**

Prepare `LLAM_IO_KIND_CONNECT` directly in the existing request object, copy
the address into request-owned storage if that is required by the private ABI,
and reduce completion without calling a blocking public wrapper from the
engine. Connect must bypass read/write direct probing and enter the backend.

- [ ] **Step 6: Run the portable suite**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 test_leir_phase0
./test_leir_phase0
```

Expected: all descriptor, execution, cancellation, and existing I/O tests pass.

---

### Task 3: Partition LEIR at semantic barriers

**Files:**
- Create: `experiments/leir/leir_aot_plan.h`
- Create: `experiments/leir/leir_aot_plan.c`
- Create: `experiments/leir/test_leir_aot_plan.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

**Interfaces:**

```c
#define LEIR_AOT_MAX_SEGMENTS LEIR_PHASE0_MAX_NODES

typedef enum leir_aot_edge_class {
    LEIR_AOT_EDGE_KERNEL_CHAIN = 0,
    LEIR_AOT_EDGE_COMPLETION_BARRIER = 1,
    LEIR_AOT_EDGE_GRAPH_BREAK = 2,
} leir_aot_edge_class_t;

typedef struct leir_aot_segment_desc {
    uint16_t first_node;
    uint16_t last_node;
    uint16_t operation_count;
    uint16_t continuation;
} leir_aot_segment_desc_t;

int leir_aot_plan_compile(const leir_phase0_program_t *program,
                          leir_aot_plan_t *out);
```

- [ ] **Step 1: Write classification tests first**

Lock these cases:

```text
CONNECT -> WRITE -> RETURN       one two-op kernel segment
CONNECT -> WRITE_ALL -> RETURN   connect segment, completion barrier, loop
READ_EXACT -> WRITE              barrier before write
READ(result used as length)      barrier before consumer
unsupported/cyclic graph         graph break or compile error
```

Also assert that a terminal ordinary write keeps an observable result and that
the current exact receive-send graph cannot become one kernel segment.

- [ ] **Step 2: Run the new test and observe link/build failure**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 test_leir_aot_plan
```

Expected: target or symbols are absent.

- [ ] **Step 3: Implement a pure classifier**

The classifier must only inspect the immutable program. It must not inspect
kernel version, ring state, or benchmark configuration. Linux capability
selection occurs after semantic partitioning.

- [ ] **Step 4: Add Make and CMake research targets**

Keep the target inside existing research-only guards. Confirm that a default
build neither compiles nor packages the AOT experiment.

- [ ] **Step 5: Run planner and build-boundary tests**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 test_leir_aot_plan test_leir_native_plan
./test_leir_aot_plan
./test_leir_native_plan
python3 -m unittest scripts/test_research_build_boundary.py -v
```

Expected: all pass.

---

### Task 4: Emit a real generated C module

**Files:**
- Modify: `experiments/leir/leir_aot_plan.h`
- Create: `scripts/gen_leir_aot_fixture.py`
- Create: `scripts/test_gen_leir_aot_fixture.py`
- Create: `experiments/leir/generated/leir_aot_connect_write.h`
- Create: `experiments/leir/generated/leir_aot_connect_write.c`
- Create: `experiments/leir/fixtures/leir_aot_c_consumer.c`
- Create: `experiments/leir/test_leir_aot_module.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write generator contract tests**

Run the generator in a temporary directory. Assert byte-for-byte deterministic
output, a stable semantic digest, the new license identifier, and the absence
of `switch (opcode)`, node loops, allocation calls, and interpreter symbols.

- [ ] **Step 2: Define module ABI v1**

Use the module descriptor from the design spec with explicit size and version
checks. Every callback receives caller-owned storage; none may retain borrowed
descriptor arrays.

- [ ] **Step 3: Generate direct fixture functions**

The generated `bind` validates five typed inputs: fd, sockaddr bytes, addrlen,
payload bytes, and result storage. Its `prepare` emits a fixed continuation
identity. Its `resume` has explicit branches for connect failure and terminal
write result.

- [ ] **Step 4: Compile the fixture as C and C++ consumer input**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 \
  test_leir_aot_module test_leir_aot_c_consumer
./test_leir_aot_module
./test_leir_aot_c_consumer
python3 -m unittest scripts/test_gen_leir_aot_fixture.py -v
```

Expected: generated source is current, both consumers link, and ABI rejection
tests pass.

---

### Task 5: Execute connect-write as an independent Linux segment

**Files:**
- Modify: `src/io/linux/runtime_io_segment_linux_internal.h`
- Modify: `src/io/linux/watch/linux_segment.c`
- Modify: `src/io/linux/watch/linux_segment_queue.c`
- Modify: `src/io/linux/watch/linux_segment_submit.c`
- Modify: `src/io/linux/watch/linux_segment_reducer.c`
- Modify: `src/io/linux/watch/linux_segment_complete.c`
- Create: `experiments/leir/leir_aot_linux.c`
- Modify: `experiments/leir/test_leir_native_linux.c`
- Create: `experiments/leir/test_leir_aot_integration.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write SQE encoder tests**

Assert the exact two-SQE shape:

```c
CHECK(first->opcode == IORING_OP_CONNECT);
CHECK((first->flags & IOSQE_IO_LINK) != 0U);
CHECK((first->flags & IOSQE_CQE_SKIP_SUCCESS) != 0U);
CHECK(second->opcode == IORING_OP_SEND);
CHECK((second->flags & IOSQE_CQE_SKIP_SUCCESS) == 0U);
```

Reject invalid sockaddr length, mutable address aliasing, missing final CQE,
and any exact-write lowering.

- [ ] **Step 2: Write reducer and ownership tests**

Cover successful final write, short write, early connect refusal, stale and
duplicate tokens, queued cancellation, in-flight cancellation, natural
completion races, shutdown retirement, and two independent tickets where the
second finishes before the first. The second owner must wake immediately.

- [ ] **Step 3: Add connect operation storage and encoding**

Cold bind copies the bounded socket address into instance-owned storage. The
hot path references only that storage. Use unique generation-tagged tokens for
both operations.

- [ ] **Step 4: Separate submission drain from semantic ownership**

The queue may detach several tickets under one lock and prepare them before one
ring enter, but each ticket decrements its own pending unit and independently
claims its task wake. Do not reuse the width-N grouped terminal batch API.

- [ ] **Step 5: Run unit, sanitizer, and integration tests**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 \
  test_leir_native_linux test_leir_aot_integration
./test_leir_native_linux --unit-only
./test_leir_aot_integration
make LLAM_BUILD_RESEARCH=1 asan
make LLAM_BUILD_RESEARCH=1 ubsan
```

Expected: all supported tests pass; unavailable io_uring integration is an
explicit skip with a reason, never a synthetic pass.

---

### Task 6: Build the connect-write mechanism screen

**Files:**
- Create: `experiments/leir/bench_leir_aot_connect.c`
- Create: `scripts/bench_leir_aot_connect.py`
- Create: `scripts/test_bench_leir_aot_connect.py`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write parser and verdict tests**

Test missing cells, duplicate cells, non-finite values, counter mismatch,
correctness failure, allocation failure, confidence interval overlap, and the
5% mechanism stop threshold. The final 3.0.0 gate is a separate classifier and
must not be replaced by the mechanism threshold.

- [ ] **Step 2: Implement controlled peers and ABBA ordering**

Measure portable reference and native candidate in alternating order for
AF_UNIX and loopback TCP. Calibrate duration without changing work per
invocation. Record environment and feature probes.

- [ ] **Step 3: Assert mechanism counters**

Successful native connect-write must report two prepared SQEs and one visible
CQE per invocation. A failed connect must report the early error and no
successful write result.

- [ ] **Step 4: Run the local contract**

Run:

```bash
make LLAM_BUILD_RESEARCH=1 -j4 bench_leir_aot_connect
python3 -m unittest scripts/test_bench_leir_aot_connect.py -v
python3 scripts/bench_leir_aot_connect.py \
  --binary ./bench_leir_aot_connect \
  --output-dir artifacts/leir-aot-connect/local-screen
```

Expected: a complete evidence bundle and an explicit `CONTINUE`, `REJECT`, or
`INCOMPLETE` mechanism verdict.

---

### Task 7: Evaluate ring profiles without confounding the baseline

**Files:**
- Modify: `src/io/linux/watch/linux_state.c`
- Modify: `experiments/leir/bench_leir_aot_connect.c`
- Modify: `scripts/bench_leir_aot_connect.py`
- Modify: `scripts/test_bench_leir_aot_connect.py`

- [ ] **Step 1: Add a probe-only profile enum**

Support `submit_all`, `coop_taskrun`, and `defer_taskrun`. Refuse the defer
profile unless a single issuer owns submit and wait.

- [ ] **Step 2: Compare each candidate to the same-profile baseline**

Do not compare a tuned candidate ring to an untuned portable baseline. Emit
profile capability and rejection reasons in metadata.

- [ ] **Step 3: Stop unsafe or losing profiles**

Any ownership violation, missing feature, correctness mismatch, or greater
than 5% screen regression rejects only that profile.

---

### Task 8: Add CI evidence and preserve the non-release boundary

**Files:**
- Create: `.github/workflows/leir-aot-research.yml`
- Modify: `docs/operations/benchmarks.md`
- Modify: `scripts/test_research_build_boundary.py`

- [ ] **Step 1: Add correctness and sanitizer jobs**

Build with research enabled, run portable, planner, generated-module, Linux
unit/integration, Python contracts, ASan, UBSan, and a cancellation stress loop.

- [ ] **Step 2: Add an immutable benchmark artifact**

Upload raw CSV, summary CSV, metadata JSON, verdict JSON, and a generated report
with commit, toolchain, kernel, CPU, liburing, and workflow identifiers.

- [ ] **Step 3: Prove research cannot ship**

Extend the boundary test so default packages contain no AOT fixture objects,
research symbols, or evidence configuration. Confirm packaging rejects
`LLAM_BUILD_RESEARCH=1`.

- [ ] **Step 4: Run the complete local verification set**

Run:

```bash
make clean
make -j4 check
make LLAM_BUILD_RESEARCH=1 -j4 check
python3 -m unittest discover -s scripts -p 'test_*.py' -v
git diff --check
```

Expected: all supported checks pass. Record explicit platform skips.

- [ ] **Step 5: Commit and push research only**

Use research-scoped commit messages and push the existing research branch.
Do not tag, bump a version, publish a package, trigger a release workflow, or
mark 3.0.0 ready. CI success means the experiment is reviewable, not releasable.
