# LCWE Phase 1 Cost Model Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and run a deterministic C11 cost-model harness that decides whether LCWE's realistic pointer-array or causal-capsule execution can meet the 1.5x throughput and 30% CPU-cost research threshold before LLAM's runtime ABI or I/O paths are changed.

**Architecture:** A standalone experiment models completion-ready tickets, per-site stable cohort formation, scalar dispatch, scalar cohort dispatch, pointer-array waves, causal-capsule waves including pack/unpack, and a site-resident AoSoA upper bound. A strict Python runner repeats each process, records raw samples, compares every realistic layout against the scalar reference, and emits a non-product feasibility verdict; no production runtime source or public header is changed in Phase 1.

**Tech Stack:** C11, project GCC/Clang/MSVC warning policy, CMake 3.20+, GNU Make compatibility, Python 3 standard library, ASan/UBSan where supported.

## Global Constraints

- Preserve the existing stackful task implementation and ABI 2.0; do not modify files under `src/` or `include/llam/`.
- Preserve the user's existing changes in `docs/operations/benchmarks.md` and `scripts/bench_deep_compare.py`; never stage either file.
- Use no hot-path allocation: all tickets, grouping arrays, capsules, commands, and timing samples are allocated before measured rounds.
- Support exactly 1 through 32 sites and lane widths `1, 2, 4, 8, 16, 32`; reject every other lane width.
- Run the three fixed workloads `exec_io_pipeline`, `exec_rpc_state`, and `exec_event_fanout`.
- Treat `wave_aosoa` as an upper bound only. It may run only with one site and can never make the Phase 1 verdict `PROMISING`.
- Include causal-capsule pack and unpack work in measured execution time.
- Require forced-scalar equivalence for every workload, seed, partial wave, realistic mode, and supported width.
- A realistic workload passes only at throughput `>= 1.50x` scalar and CPU ns/op `<= 0.70x` scalar in the same matrix cell.
- Phase 1 is `PROMISING` only when at least two of the three fixed workloads pass in `wave_pointers` or `wave_capsule`, and scalar cohort shows at least a `1.05x` throughput win on one workload.
- Report `LAYOUT_BLOCKED` when fewer than two workloads pass in a realistic layout but at least two pass in `wave_aosoa`; report `REJECT` for every other measured gate failure, including a failed cohort gate; report `INCONCLUSIVE` when required rows are missing, checksums disagree, or process spread exceeds `1.15x`.
- Do not describe a Phase 1 verdict as production validation. Final LCWE gates still require real I/O, mixed fiber load, p99 latency, all primary platforms, and existing stackful regression tests.
- Use only standard-library Python modules and repository helpers already under `scripts/`.

---

## File Map

### New C Experiment Files

- `experiments/lcwe/lcwe_model.h`
  - Stable interface shared by the model test and benchmark executable.
  - Defines modes, workloads, lifecycle values, metrics, batch ownership, parsing, equality, and round execution.
- `experiments/lcwe/lcwe_model_internal.h`
  - Private frame, event, command, ticket, capsule, AoSoA, site callback, and batch layouts.
  - Never included outside `experiments/lcwe/`.
- `experiments/lcwe/lcwe_model.c`
  - Argument validation, deterministic initialization, stable site grouping, lifecycle transitions, mode dispatch, equality, checksum, and teardown.
- `experiments/lcwe/lcwe_workloads.c`
  - Scalar and wave implementations of the three fixed workload formulas.
  - Owns pointer-array, capsule pack/vector/unpack, and AoSoA callbacks.
- `experiments/lcwe/test_lcwe_model.c`
  - Differential correctness, lifecycle, partial-wave, invalid-input, metrics, and allocation-free-round tests.
- `experiments/lcwe/bench_lcwe_model.c`
  - Portable clock helpers, strict CLI, warmup/measured rounds, checksum validation, and one machine-readable result row.

### New Python Files

- `scripts/bench_lcwe_model.py`
  - Strict result parser, process runner, median selection, spread calculation, CSV/Markdown output, and verdict logic.
- `scripts/test_bench_lcwe_model.py`
  - Parser, median, gate, native-only, missing-row, checksum, and report regression tests.

### Build Files

- `CMakeLists.txt`
  - Add `test_lcwe_model`, `bench_lcwe_model`, CTest registration, includes, warnings, and platform definitions.
- `Makefile`
  - Add object lists, link targets, clean entries, Windows CMake delegation, `test-lcwe-model`, and `lcwe-model-report`.

### Generated Evidence

- `object/lcwe-phase1/lcwe_phase1_samples.csv`
- `object/lcwe-phase1/lcwe_phase1_summary.csv`
- `object/lcwe-phase1/lcwe_phase1_report.md`
- `docs/superpowers/reports/2026-07-26-lcwe-phase1-results.md`

The `object/` files remain untracked raw evidence. The final report is reviewed
and committed with its exact command, host/compiler identity, sample count,
matrix, verdict, and limitations.

---

### Task 1: Scalar Ticket Model And Reference Workloads

**Files:**

- Create: `experiments/lcwe/lcwe_model.h`
- Create: `experiments/lcwe/lcwe_model_internal.h`
- Create: `experiments/lcwe/lcwe_model.c`
- Create: `experiments/lcwe/lcwe_workloads.c`
- Create: `experiments/lcwe/test_lcwe_model.c`

**Interfaces:**

- Produces:

```c
#define LCWE_MODEL_MAX_LANES 32U
#define LCWE_MODEL_MAX_SITES 32U

typedef enum lcwe_model_mode {
    LCWE_MODEL_SCALAR = 0,
    LCWE_MODEL_COHORT = 1,
    LCWE_MODEL_WAVE_POINTERS = 2,
    LCWE_MODEL_WAVE_CAPSULE = 3,
    LCWE_MODEL_WAVE_AOSOA = 4,
} lcwe_model_mode_t;

typedef enum lcwe_model_workload {
    LCWE_MODEL_EXEC_IO_PIPELINE = 0,
    LCWE_MODEL_EXEC_RPC_STATE = 1,
    LCWE_MODEL_EXEC_EVENT_FANOUT = 2,
} lcwe_model_workload_t;

typedef enum lcwe_model_lifecycle {
    LCWE_MODEL_WAITING = 1,
    LCWE_MODEL_READY = 2,
    LCWE_MODEL_RUNNING = 3,
    LCWE_MODEL_TERMINAL = 4,
} lcwe_model_lifecycle_t;

typedef struct lcwe_model_metrics {
    uint64_t admissions;
    uint64_t tickets;
    uint64_t scalar_calls;
    uint64_t wave_calls;
    uint64_t pointer_lanes;
    uint64_t capsule_lanes;
    uint64_t aosoa_lanes;
    uint64_t hot_allocations;
} lcwe_model_metrics_t;

typedef struct lcwe_model_batch lcwe_model_batch_t;

int lcwe_model_batch_create(lcwe_model_workload_t workload,
                            lcwe_model_mode_t mode,
                            size_t instance_count,
                            unsigned site_count,
                            uint64_t seed,
                            lcwe_model_batch_t **out_batch);
void lcwe_model_batch_destroy(lcwe_model_batch_t *batch);
int lcwe_model_run_round(lcwe_model_batch_t *batch,
                         unsigned lane_width,
                         lcwe_model_metrics_t *metrics);
uint64_t lcwe_model_checksum(const lcwe_model_batch_t *batch);
bool lcwe_model_batch_equal(const lcwe_model_batch_t *lhs,
                            const lcwe_model_batch_t *rhs);
const char *lcwe_model_mode_name(lcwe_model_mode_t mode);
const char *lcwe_model_workload_name(lcwe_model_workload_t workload);
int lcwe_model_parse_mode(const char *text, lcwe_model_mode_t *out);
int lcwe_model_parse_workload(const char *text, lcwe_model_workload_t *out);
```

- Task 1 initially returns `ENOTSUP` from `lcwe_model_batch_create()` for every
  mode except `LCWE_MODEL_SCALAR`. Task 2 removes that restriction.

- The private canonical structures are:

```c
typedef struct lcwe_model_frame {
    uint64_t state0;
    uint64_t state1;
    uint64_t state2;
    uint64_t state3;
    uint64_t output;
    uint64_t generation;
    uint32_t site;
    uint32_t steps;
} lcwe_model_frame_t;

typedef struct lcwe_model_event {
    uint64_t word0;
    uint64_t word1;
    uint32_t kind;
    uint32_t reserved;
} lcwe_model_event_t;

typedef struct lcwe_model_command {
    uint64_t output;
    uint32_t next_site;
    uint32_t kind;
} lcwe_model_command_t;

typedef struct lcwe_model_ticket {
    lcwe_model_frame_t *frame;
    lcwe_model_event_t event;
    lcwe_model_command_t command;
    uint64_t generation;
    uint32_t site;
    uint32_t lifecycle;
    uint32_t frame_index;
    uint32_t reserved;
} lcwe_model_ticket_t;
```

- `lcwe_model_batch_t` owns one contiguous frame array and a deterministically
  shuffled ticket pointer array. All memory is allocated in create and freed in
  destroy.

- Each scalar round performs, in order:

```text
READY(g) -> RUNNING(g)
resume_one(frame, event, command)
validate command.next_site < site_count
frame.generation = ticket.generation = g + 1
ticket.site = frame.site = command.next_site
derive the next event from seed, frame index, generation, and output
RUNNING(g) -> READY(g + 1)
```

- Generation value zero is never emitted. Overflow returns `EOVERFLOW` before
  mutating the ticket.

- Workload formulas use only unsigned arithmetic so scalar and vector forms
  have defined wraparound:

```c
static uint64_t rotl64(uint64_t value, unsigned shift) {
    shift &= 63U;
    return (value << shift) | (value >> ((64U - shift) & 63U));
}

/* exec_io_pipeline */
x = event0 ^ rotl64(event1 + state0, 13U);
state0 = (x * UINT64_C(0x9E3779B185EBCA87)) ^ state1;
state1 = rotl64(state1 + x + UINT64_C(0xC2B2AE3D27D4EB4F), 17U);
output = state0 ^ rotl64(state1, 7U);

/* exec_rpc_state */
opcode = (uint32_t)(event0 ^ state0) & 3U;
candidate0 = state0 + event1 + UINT64_C(0x165667B19E3779F9);
candidate1 = (state0 ^ event1) * UINT64_C(0xD6E8FEB86659FD93);
state0 = (opcode & 1U) != 0U ? candidate1 : candidate0;
state1 = rotl64(state1 ^ state0, (opcode + 5U) & 63U);
output = state0 + state1 + opcode;

/* exec_event_fanout */
state0 = state0 * UINT64_C(6364136223846793005) +
         event0 + UINT64_C(1442695040888963407);
state1 ^= rotl64(state0 + event1, 23U);
state2 += state0 ^ state1;
output = state2 ^ rotl64(state1, 11U);
```

For all workloads:

```c
command.kind = 1U; /* synthetic YIELD */
command.output = output;
command.next_site = (frame->site + 1U + (unsigned)(output & 1U)) % site_count;
frame->steps += 1U;
```

- Event derivation is centralized in `lcwe_model.c` and shared by all modes:

```c
event.word0 = mix64(seed ^ ((uint64_t)frame_index << 32U) ^ generation);
event.word1 = mix64(event.word0 ^ frame.output ^ UINT64_C(0xA0761D6478BD642F));
event.kind = (uint32_t)(event.word1 & 3U);
```

- `mix64()` is the SplitMix64 finalizer with constants
  `0xBF58476D1CE4E5B9` and `0x94D049BB133111EB`.

- `lcwe_model_batch_equal()` compares canonical frame fields, ticket
  generation/site/lifecycle, current events, and current commands; checksum
  equality alone is never used as the correctness oracle.

- [ ] **Step 1: Write the failing scalar reference test**

Create `test_lcwe_model.c` with a table over all three workloads. For each
workload, create 37 scalar instances across three sites, run 11 rounds at lane
width 1, and assert:

```c
if (metrics.tickets != UINT64_C(37) * 11U ||
    metrics.scalar_calls != metrics.tickets ||
    metrics.wave_calls != 0U ||
    metrics.hot_allocations != 0U) {
    return fail("scalar metric invariant");
}
if (lcwe_model_checksum(batch) == 0U) {
    return fail("scalar checksum must be nonzero");
}
```

Also assert invalid create inputs return `EINVAL`, non-scalar modes return
`ENOTSUP`, lane widths 0, 3, and 33 return `EINVAL`, and null outputs are
rejected.

- [ ] **Step 2: Compile to verify the test fails**

Run:

```bash
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
  -Iexperiments/lcwe \
  experiments/lcwe/test_lcwe_model.c \
  experiments/lcwe/lcwe_model.c \
  experiments/lcwe/lcwe_workloads.c \
  -o /tmp/test_lcwe_model
```

Expected: compilation fails because the new files or declarations do not yet
exist.

- [ ] **Step 3: Implement the private layouts, scalar callbacks, and lifecycle**

Create the two headers and two implementation files using the exact types and
formulas above. Keep every allocation in `lcwe_model_batch_create()`. Store the
seed, round number, instance count, site count, mode, frames, tickets, and
shuffled ready pointer array in the opaque batch.

Use a deterministic Fisher-Yates shuffle driven by SplitMix64 during create.
Do not call a random library function or read process-global state.

In `lcwe_model_run_round()`, copy the caller's metrics only through additive
updates after all arguments pass validation. Return `EPROTO` for an invalid
command or lifecycle transition.

- [ ] **Step 4: Compile and run the scalar reference test**

Run the compile command from Step 2, then:

```bash
/tmp/test_lcwe_model
```

Expected:

```text
[test_lcwe_model] all checks passed
```

- [ ] **Step 5: Commit the scalar model**

```bash
git add experiments/lcwe/lcwe_model.h \
        experiments/lcwe/lcwe_model_internal.h \
        experiments/lcwe/lcwe_model.c \
        experiments/lcwe/lcwe_workloads.c \
        experiments/lcwe/test_lcwe_model.c
git commit -m "test: add scalar LCWE cost model"
```

---

### Task 2: Cohort, Pointer, Capsule, And AoSoA Execution

**Files:**

- Modify: `experiments/lcwe/lcwe_model_internal.h`
- Modify: `experiments/lcwe/lcwe_model.c`
- Modify: `experiments/lcwe/lcwe_workloads.c`
- Modify: `experiments/lcwe/test_lcwe_model.c`

**Interfaces:**

- Consumes all Task 1 interfaces unchanged.
- Produces support for all five `lcwe_model_mode_t` values.
- Adds the following private callback table:

```c
typedef void (*lcwe_resume_one_fn)(lcwe_model_frame_t *,
                                   const lcwe_model_event_t *,
                                   lcwe_model_command_t *,
                                   unsigned site_count);
typedef void (*lcwe_resume_pointers_fn)(lcwe_model_ticket_t *const *,
                                        unsigned lane_count,
                                        unsigned site_count);
typedef void (*lcwe_resume_capsule_fn)(lcwe_model_capsule_t *,
                                       unsigned lane_count,
                                       unsigned site_count);
typedef void (*lcwe_resume_aosoa_fn)(lcwe_model_aosoa_t *,
                                     size_t first,
                                     unsigned lane_count,
                                     unsigned site_count);

typedef struct lcwe_model_workload_ops {
    lcwe_resume_one_fn resume_one;
    lcwe_resume_pointers_fn resume_pointers;
    lcwe_resume_capsule_fn resume_capsule;
    lcwe_resume_aosoa_fn resume_aosoa;
} lcwe_model_workload_ops_t;
```

- The capsule contains one 64-byte-aligned array of 32 elements for every live
  frame/event/command value touched by the fixed workload formulas:

```c
typedef struct lcwe_model_capsule {
    _Alignas(64) uint64_t state0[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint64_t state1[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint64_t state2[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint64_t state3[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint64_t event0[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint64_t event1[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint64_t output[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint32_t site[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint32_t next_site[LCWE_MODEL_MAX_LANES];
    _Alignas(64) uint32_t steps[LCWE_MODEL_MAX_LANES];
} lcwe_model_capsule_t;
```

- `lcwe_model_aosoa_t` contains the same fields as heap arrays sized to
  `instance_count`, initialized before measured rounds. It is accepted only
  when `site_count == 1`.

- Stable site grouping uses counting sort with preallocated storage:

```c
memset(site_counts, 0, site_count * sizeof(*site_counts));
for (i = 0; i < instance_count; ++i) {
    ++site_counts[ready[i]->site];
}
site_offsets[0] = 0U;
for (site = 0; site < site_count; ++site) {
    site_offsets[site + 1U] = site_offsets[site] + site_counts[site];
    site_cursor[site] = site_offsets[site];
}
for (i = 0; i < instance_count; ++i) {
    grouped[site_cursor[ready[i]->site]++] = ready[i];
}
```

- Scalar mode does not run counting sort. Cohort and all wave modes include
  grouping cost in `lcwe_model_run_round()`.

- Cohort mode caches one site descriptor and calls `resume_one` in FIFO order
  for each group. It increments `scalar_calls` per lane and never increments
  `wave_calls`.

- Pointer mode calls `resume_pointers` once per full or partial group chunk and
  includes all scattered loads/stores in the callback.

- Capsule mode performs, within each callback:

```text
pack scattered frame and event fields into the capsule
execute the workload loop over contiguous arrays
scatter frame and command results back to their tickets
```

- AoSoA mode initializes persistent arrays before the first round and executes
  directly on them. Equality and checksum canonicalize from AoSoA without
  including canonicalization in measured rounds.

- Clang/GCC vectorization hints are advisory and guarded:

```c
#if defined(__clang__)
#define LCWE_MODEL_VECTORIZE _Pragma("clang loop vectorize(enable) interleave(enable)")
#elif defined(__GNUC__)
#define LCWE_MODEL_VECTORIZE _Pragma("GCC ivdep")
#else
#define LCWE_MODEL_VECTORIZE
#endif
```

- [ ] **Step 1: Add failing differential tests**

For seeds `1`, `0x6c6c616d77617665`, and `UINT64_MAX - 17`, create a fresh
scalar/candidate pair for every workload, non-scalar mode, and width. Use 257
instances and eight sites. Run both members of each pair for 19 rounds at the
selected width. After every round:

```c
if (!lcwe_model_batch_equal(scalar, candidate)) {
    return fail_case(workload, mode, width, seed, round);
}
```

Create a separate one-site AoSoA batch and compare it with scalar. Add cases for
37 instances to force partial waves. Assert AoSoA creation with two sites
returns `EINVAL`.

Check metric totals:

```c
if (candidate_metrics.tickets != instance_count * rounds ||
    candidate_metrics.hot_allocations != 0U) {
    return fail("wave metric invariant");
}
if (mode >= LCWE_MODEL_WAVE_POINTERS &&
    (candidate_metrics.wave_calls == 0U ||
     candidate_metrics.scalar_calls != 0U)) {
    return fail("wave dispatch accounting");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Compile and run with the Task 1 command.

Expected: `ENOTSUP` or differential failures for the four unimplemented modes.

- [ ] **Step 3: Implement stable grouping and the four execution paths**

Add preallocated `grouped`, `site_counts`, `site_offsets`, `site_cursor`,
capsule, and optional AoSoA ownership to `lcwe_model_batch_t`.

Implement one pointer, capsule, and AoSoA callback for each fixed workload using
the exact Task 1 arithmetic. Do not call the scalar callback from inside a wave
callback; the vector loop must be visible to the compiler.

Process commands and derive the next event only after each callback returns.
For a partial chunk, pass its exact lane count and never read inactive capsule
lanes.

- [ ] **Step 4: Run differential and sanitizer tests**

Run:

```bash
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O3 \
  -Iexperiments/lcwe \
  experiments/lcwe/test_lcwe_model.c \
  experiments/lcwe/lcwe_model.c \
  experiments/lcwe/lcwe_workloads.c \
  -o /tmp/test_lcwe_model
/tmp/test_lcwe_model
```

Then, where Clang sanitizers are available:

```bash
clang -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iexperiments/lcwe \
  experiments/lcwe/test_lcwe_model.c \
  experiments/lcwe/lcwe_model.c \
  experiments/lcwe/lcwe_workloads.c \
  -o /tmp/test_lcwe_model_asan
/tmp/test_lcwe_model_asan
```

Expected: both print `[test_lcwe_model] all checks passed` with no sanitizer
diagnostics.

- [ ] **Step 5: Commit the wave model**

```bash
git add experiments/lcwe/lcwe_model_internal.h \
        experiments/lcwe/lcwe_model.c \
        experiments/lcwe/lcwe_workloads.c \
        experiments/lcwe/test_lcwe_model.c
git commit -m "feat: model LCWE cohort and wave layouts"
```

---

### Task 3: Parseable C Benchmark Driver

**Files:**

- Create: `experiments/lcwe/bench_lcwe_model.c`
- Create: `scripts/bench_lcwe_model.py`
- Create: `scripts/test_bench_lcwe_model.py`

**Interfaces:**

- Consumes the Task 1 public C interface.
- The benchmark accepts:

```text
--workload exec_io_pipeline|exec_rpc_state|exec_event_fanout
--mode scalar|cohort|wave_pointers|wave_capsule|wave_aosoa
--instances 1..16777216
--sites 1..32
--lanes 1|2|4|8|16|32
--rounds 1..1000000
--warmup 0..100000
--seed 0..18446744073709551615
```

- It rejects duplicate options, unknown options, trailing characters,
  overflow, `warmup >= rounds`, unsupported widths, and AoSoA with more than one
  site.

- It emits exactly one result row to stdout:

```text
[lcwe-model] workload=exec_io_pipeline mode=wave_capsule instances=65536 sites=1 lanes=8 rounds=31 warmup=5 ops=1703936 wall_ns=123456789 cpu_ns=120000000 wall_ns_per_op=72.45 cpu_ns_per_op=70.42 p50_ns_per_op=71.90 p99_ns_per_op=78.10 checksum=0123456789abcdef
```

- Informational compiler/host text goes to stderr, not stdout.
- `scripts/bench_lcwe_model.py` initially provides only:

```python
@dataclass(frozen=True)
class ModelRow:
    workload: str
    mode: str
    instances: int
    sites: int
    lanes: int
    rounds: int
    warmup: int
    ops: int
    wall_ns: int
    cpu_ns: int
    wall_ns_per_op: float
    cpu_ns_per_op: float
    p50_ns_per_op: float
    p99_ns_per_op: float
    checksum: str

def parse_output(output: str) -> ModelRow: ...
```

- [ ] **Step 1: Write failing parser and CLI smoke tests**

In `test_bench_lcwe_model.py`, add tests that:

- parse the exact sample row above;
- reject a missing field;
- reject duplicate fields;
- reject `nan`, `inf`, zero ops, a non-hex checksum, and two result rows;
- when `LCWE_MODEL_TEST_BINARY` is set, run that binary with a small scalar case
  and verify its row, then run invalid `--lanes 3` and verify a nonzero exit
  plus a concise diagnostic without a result row;
- when `LCWE_MODEL_TEST_BINARY` is unset, skip only those executable smoke
  checks so parser-only TDD can run before the C binary exists.

The test script has a `main()` that calls every test and prints:

```text
[test_bench_lcwe_model] all checks passed
```

- [ ] **Step 2: Run tests to verify failure**

Compile the model test as before, then attempt:

```bash
python3 scripts/test_bench_lcwe_model.py
```

Expected: import or missing-binary failure because the parser and benchmark do
not exist.

- [ ] **Step 3: Implement the benchmark CLI and strict parser**

In C, allocate timing samples before warmup. For each measured round:

```c
start_wall = wall_now_ns();
start_cpu = clock();
rc = lcwe_model_run_round(batch, lane_width, &metrics);
end_cpu = clock();
end_wall = wall_now_ns();
```

Use `mach_continuous_time()` on Darwin, `QueryPerformanceCounter()` on Windows,
and `clock_gettime(CLOCK_MONOTONIC)` elsewhere. Convert `clock()` ticks with
`CLOCKS_PER_SEC`. Abort if a clock goes backward or if any multiplication or
addition would overflow `uint64_t`.

Sort a copy of measured wall ns/op samples for nearest-rank p50 and p99. Print
all floating values with two decimal places and checksum as exactly 16 lowercase
hex digits.

In Python, use a single `[lcwe-model] ` prefix, reject duplicate keys, require
the exact field set, validate finite positive metrics, and reject extra result
rows.

- [ ] **Step 4: Compile and run parser/CLI tests**

Run:

```bash
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O3 \
  -Iexperiments/lcwe \
  experiments/lcwe/bench_lcwe_model.c \
  experiments/lcwe/lcwe_model.c \
  experiments/lcwe/lcwe_workloads.c \
  -o /tmp/bench_lcwe_model
LCWE_MODEL_TEST_BINARY=/tmp/bench_lcwe_model \
  python3 scripts/test_bench_lcwe_model.py
```

Expected: `[test_bench_lcwe_model] all checks passed`.

- [ ] **Step 5: Commit the driver and parser**

```bash
git add experiments/lcwe/bench_lcwe_model.c \
        scripts/bench_lcwe_model.py \
        scripts/test_bench_lcwe_model.py
git commit -m "bench: add LCWE cost model driver"
```

---

### Task 4: Repeated-Sample Runner, Reports, And Verdict

**Files:**

- Modify: `scripts/bench_lcwe_model.py`
- Modify: `scripts/test_bench_lcwe_model.py`

**Interfaces:**

- Adds:

```python
@dataclass(frozen=True)
class SampleRow:
    process_sample: int
    row: ModelRow

@dataclass(frozen=True)
class SummaryRow:
    workload: str
    mode: str
    sites: int
    lanes: int
    sample_count: int
    wall_ns_per_op: float
    cpu_ns_per_op: float
    p50_ns_per_op: float
    p99_ns_per_op: float
    spread: float
    checksum: str

def select_median(rows: list[ModelRow]) -> SummaryRow: ...
def classify(summary: list[SummaryRow]) -> tuple[str, list[str]]: ...
def write_samples_csv(path: Path, rows: list[SampleRow]) -> None: ...
def write_summary_csv(path: Path, rows: list[SummaryRow]) -> None: ...
def write_markdown(path: Path,
                   rows: list[SummaryRow],
                   verdict: str,
                   reasons: list[str],
                   metadata: dict[str, str]) -> None: ...
```

- CLI:

```text
--binary PATH
--samples 1..31                 default 7
--instances 1..16777216         default 65536
--rounds 1..1000000             default 31
--warmup 0..100000              default 5
--seed UINT64                   default 7810762890074515045
--widths LIST                   default 1,2,4,8,16,32
--cc PATH                       default $CC or cc
--out-dir PATH                  default object/lcwe-phase1
--quick                         samples=3, instances=8192, rounds=9, warmup=2
```

- The main homogeneous matrix runs:

```text
3 workloads
x scalar once
x cohort once
x wave_pointers at all requested widths
x wave_capsule at all requested widths
x wave_aosoa at all requested widths
x 7 process samples
```

All main rows use `sites=1`. A secondary mixed-site diagnostic uses
`sites=8`, widths `1,8,32`, and excludes AoSoA. Secondary rows inform the report
but cannot create `PROMISING`.

- Median selection sorts by wall ns/op and selects the middle process row. Its
  CPU and percentile values come from the same process row; metrics from
  different process samples are never mixed.

- Spread is `max(wall_ns_per_op) / min(wall_ns_per_op)`.
- Checksums must be identical across process samples for one cell and across
  all modes representing the same workload/sites/round count.

- Classification uses realistic best rows independently per workload:

```python
throughput_speedup = scalar.wall_ns_per_op / candidate.wall_ns_per_op
cpu_ratio = candidate.cpu_ns_per_op / scalar.cpu_ns_per_op
passes = throughput_speedup >= 1.50 and cpu_ratio <= 0.70
```

`PROMISING` additionally requires one cohort row with scalar wall/cohort wall
`>= 1.05`. `wave_aosoa` is excluded from the realistic pass count. If realistic
layouts pass the workload gate but scalar cohort misses `1.05x`, classify the
measured result as `REJECT` and identify the failed cohort economics explicitly.

- [ ] **Step 1: Write failing aggregation and verdict tests**

Construct in-memory rows for these exact cases:

1. two realistic workloads at `1.60x` wall and `0.65x` CPU plus cohort at
   `1.08x` produces `PROMISING`;
2. only AoSoA passes two workloads produces `LAYOUT_BLOCKED`;
3. no mode passes two workloads produces `REJECT`;
4. spread `1.1501`, a missing scalar row, or checksum mismatch produces
   `INCONCLUSIVE`;
5. a fastest outlier does not win median selection;
6. the selected row's CPU metric comes from the same process sample;
7. Markdown includes the verdict, realistic/native distinction, exact command,
   and the warning that Phase 1 is not production validation.

- [ ] **Step 2: Run tests to verify failure**

Run:

```bash
python3 scripts/test_bench_lcwe_model.py
```

Expected: missing aggregation, classification, or report functions.

- [ ] **Step 3: Implement matrix execution and reports**

Use `scripts/process_utils.py::run_capture` for subprocess timeout and captured
output handling. Use `scripts/safe_output.py::open_text_for_write` for CSV and
Markdown destinations. Pass every benchmark parameter as a separate argv item;
do not invoke a shell.

Write deterministic CSV column orders. Sort summary rows by
`(sites, workload, mode, lanes)`. The report contains:

- host platform, machine, Python version, compiler identity captured by invoking
  `--cc PATH --version`, binary path, seed, samples, instances, rounds, and
  warmup;
- one table per workload with scalar-relative wall speedup and CPU reduction;
- mixed-site diagnostic table;
- verdict and every reason;
- the final-gate limitations copied from Global Constraints.

- [ ] **Step 4: Run unit tests and a quick matrix**

Run:

```bash
python3 scripts/test_bench_lcwe_model.py
python3 scripts/bench_lcwe_model.py \
  --binary /tmp/bench_lcwe_model \
  --quick \
  --out-dir /tmp/lcwe-phase1-quick
```

Expected: unit tests pass and the quick command creates nonempty samples CSV,
summary CSV, and Markdown report. The quick verdict may be any of the four
defined verdicts, but must include reasons and no traceback.

- [ ] **Step 5: Commit the runner**

```bash
git add scripts/bench_lcwe_model.py scripts/test_bench_lcwe_model.py
git commit -m "bench: classify LCWE phase one evidence"
```

---

### Task 5: Make, CMake, And Test Integration

**Files:**

- Modify: `CMakeLists.txt:409`
- Modify: `CMakeLists.txt:640`
- Modify: `CMakeLists.txt:681`
- Modify: `Makefile:22`
- Modify: `Makefile:464`
- Modify: `Makefile:536`
- Modify: `Makefile:568`
- Modify: `Makefile:595`
- Modify: `Makefile:611`
- Modify: `Makefile:683`
- Modify: `Makefile:759`
- Modify: `Makefile:1971`
- Modify: `Makefile:2219`
- Modify: `Makefile:2287`

**Interfaces:**

- CMake targets:

```cmake
set(LLAM_LCWE_MODEL_SOURCES
    experiments/lcwe/lcwe_model.c
    experiments/lcwe/lcwe_workloads.c
)

add_executable(test_lcwe_model
    ${LLAM_LCWE_MODEL_SOURCES}
    experiments/lcwe/test_lcwe_model.c
)
add_executable(bench_lcwe_model
    ${LLAM_LCWE_MODEL_SOURCES}
    experiments/lcwe/bench_lcwe_model.c
)
```

Both targets include `experiments/lcwe`, use `_GNU_SOURCE`, and receive the
existing Darwin/BSD/Windows feature definitions. Both are ordinary build
targets, but neither is installed or added to production packages. Register
`test_lcwe_model` with CTest. When `Python3_FOUND`, register
`test_bench_lcwe_model` with
`LCWE_MODEL_TEST_BINARY=$<TARGET_FILE:bench_lcwe_model>` through
`cmake -E env`.

- Make variables:

```make
LCWE_MODEL_CORE_OBJS = \
	$(OBJDIR)/experiments/lcwe/lcwe_model.o \
	$(OBJDIR)/experiments/lcwe/lcwe_workloads.o
LCWE_MODEL_TEST_OBJS = \
	$(OBJDIR)/experiments/lcwe/test_lcwe_model.o
LCWE_MODEL_BENCH_OBJS = \
	$(OBJDIR)/experiments/lcwe/bench_lcwe_model.o
```

- Make targets:

```make
test_lcwe_model: $(LCWE_MODEL_CORE_OBJS) $(LCWE_MODEL_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(SERVER_FLOOD_LDLIBS)

bench_lcwe_model: $(LCWE_MODEL_CORE_OBJS) $(LCWE_MODEL_BENCH_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(SERVER_FLOOD_LDLIBS)

test-lcwe-model: test_lcwe_model bench_lcwe_model
	./test_lcwe_model
	LCWE_MODEL_TEST_BINARY=./bench_lcwe_model python3 scripts/test_bench_lcwe_model.py

lcwe-model-report: test-lcwe-model
	python3 scripts/bench_lcwe_model.py \
	  --binary ./bench_lcwe_model \
	  --out-dir object/lcwe-phase1
```

- Add `test-lcwe-model` and `lcwe-model-report` to `.PHONY`. Add both binaries
  and `.exe` forms to `CLEAN_FILES`, all experiment objects to
  `BUILD_OBJS`, both linked programs to `LINK_TARGETS`, both Windows target
  names to `WINDOWS_CMAKE_TARGETS`, and the C test plus Python parser test to
  the normal `test` recipe.

- Add one explicit experiment object pattern:

```make
$(OBJDIR)/experiments/lcwe/%.o: experiments/lcwe/%.c \
		experiments/lcwe/lcwe_model.h \
		experiments/lcwe/lcwe_model_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iexperiments/lcwe -c -o $@ $<
```

- [ ] **Step 1: Add a failing build integration check**

Before editing build files, run:

```bash
cmake -S . -B /tmp/llam-lcwe-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/llam-lcwe-cmake --target test_lcwe_model bench_lcwe_model
```

Expected: target-not-found failure.

- [ ] **Step 2: Add CMake and Make target definitions**

Insert the exact target and object definitions above. Keep both research
executables as ordinary CMake build targets so a default build followed by
CTest has both binaries available.

Do not add the benchmark executable to production install or package lists.
In the Makefile's Windows delegation branch, make `test-lcwe-model` build both
CMake targets and run the matching CTest regex. Route `lcwe-model-report` to the
existing Windows-unsupported path because the report is host-specific research
evidence; the registered C and Python tests remain part of ordinary Windows
test coverage.

- [ ] **Step 3: Verify Make integration**

Run:

```bash
make test-lcwe-model
```

Expected: C and Python tests both pass.

- [ ] **Step 4: Verify CMake and CTest integration**

Run:

```bash
cmake -S . -B /tmp/llam-lcwe-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/llam-lcwe-cmake --target test_lcwe_model bench_lcwe_model
ctest --test-dir /tmp/llam-lcwe-cmake \
  --output-on-failure \
  -R 'test_lcwe_model|test_bench_lcwe_model'
```

Expected: two tests pass, zero fail.

- [ ] **Step 5: Commit build integration**

```bash
git add CMakeLists.txt Makefile
git commit -m "build: integrate LCWE research harness"
```

---

### Task 6: Vectorization Audit, Full Experiment, And Evidence Report

**Files:**

- Create: `docs/superpowers/reports/2026-07-26-lcwe-phase1-results.md`
- Generated: `object/lcwe-phase1/lcwe_phase1_samples.csv`
- Generated: `object/lcwe-phase1/lcwe_phase1_summary.csv`
- Generated: `object/lcwe-phase1/lcwe_phase1_report.md`

**Interfaces:**

- Consumes the `lcwe-model-report` target and all Task 4 output contracts.
- Produces a reviewed research result containing no stronger conclusion than
  the classifier's exact verdict.

- [ ] **Step 1: Verify compiler vectorization evidence**

Build the benchmark with Clang remarks:

```bash
mkdir -p object
make bench_lcwe_model \
  OBJDIR=object-lcwe \
  CC=clang \
  CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O3 -g -fno-omit-frame-pointer -Rpass=loop-vectorize -Rpass-missed=loop-vectorize' \
  2>object/lcwe-vectorization.log
```

Inspect:

```bash
rg -n 'vectorized loop|loop not vectorized' object/lcwe-vectorization.log
```

The result report must state which pointer, capsule, and AoSoA workload loops
were vectorized. A missing vectorization remark is evidence against that layout,
not permission to assume vectorization.

- [ ] **Step 2: Run correctness and sanitizer verification**

Run:

```bash
make test-lcwe-model CC=clang CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O3 -g -fno-omit-frame-pointer'
clang -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iexperiments/lcwe \
  experiments/lcwe/test_lcwe_model.c \
  experiments/lcwe/lcwe_model.c \
  experiments/lcwe/lcwe_workloads.c \
  -o /tmp/test_lcwe_model_asan
/tmp/test_lcwe_model_asan
```

Expected: all C/Python checks pass and sanitizer output is clean.

- [ ] **Step 3: Run the full repeated-process matrix**

Run:

```bash
python3 scripts/bench_lcwe_model.py \
  --binary ./bench_lcwe_model \
  --samples 7 \
  --instances 65536 \
  --rounds 31 \
  --warmup 5 \
  --seed 7810762890074515045 \
  --widths 1,2,4,8,16,32 \
  --cc clang \
  --out-dir object/lcwe-phase1
```

Expected: raw samples, summary, and Markdown report are created. Do not rerun
only to obtain a preferred verdict. Rerun solely for a documented harness bug,
machine interruption, thermal instability, or `INCONCLUSIVE` spread, and retain
the reason in the final report.

- [ ] **Step 4: Run existing-project regression verification**

Because production runtime code is unchanged, use the focused project gate plus
build validation:

```bash
make test-quick
cmake -S . -B /tmp/llam-lcwe-full -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/llam-lcwe-full -j2
ctest --test-dir /tmp/llam-lcwe-full --output-on-failure
```

Expected: every command exits zero. If an unrelated pre-existing failure occurs,
record its exact command and output; do not label the harness verified from a
partial run.

- [ ] **Step 5: Create the tracked evidence report**

Use the generated Markdown as the numeric source. Create
`docs/superpowers/reports/2026-07-26-lcwe-phase1-results.md` with:

```markdown
# LCWE Phase 1 Cost Model Results

## Verdict

`PROMISING`, `LAYOUT_BLOCKED`, `REJECT`, or `INCONCLUSIVE`, exactly as emitted.

## Reproduction

Exact commit, compiler identity, host identity, and full command.

## Correctness

Differential seed/width/workload coverage and sanitizer result.

## Vectorization

Per-layout compiler remarks, including missed-vectorization evidence.

## Performance

The generated realistic-layout and AoSoA tables without omitted workloads.

## Interpretation

What the verdict permits as the next research phase and what it does not prove.

## Required Next Action

- `PROMISING`: plan the Scalar Executor ABI and completion-target phase.
- `LAYOUT_BLOCKED`: redesign frame/capsule layout before touching runtime I/O.
- `REJECT`: stop LCWE implementation and preserve the report as a falsified hypothesis.
- `INCONCLUSIVE`: fix only the documented measurement defect and repeat the same matrix.
```

Replace the four-way examples with the one actual verdict and its exact next
action. Do not hand-edit generated performance values.

- [ ] **Step 6: Verify and commit only the report**

Run:

```bash
git diff --check -- \
  docs/superpowers/reports/2026-07-26-lcwe-phase1-results.md
rg -n 'TODO|FIXME|TBD|PLACEHOLDER' \
  docs/superpowers/reports/2026-07-26-lcwe-phase1-results.md
git status --short
```

Confirm the user's two dirty files remain unstaged. Then:

```bash
git add docs/superpowers/reports/2026-07-26-lcwe-phase1-results.md
git commit -m "docs: record LCWE phase one evidence"
```

---

## Completion Audit

Before declaring Phase 1 complete, map evidence to every requirement:

- Scalar reference and all realistic modes are byte-for-field equivalent:
  `test_lcwe_model` differential output.
- Partial waves and every supported width are covered:
  test loop over 37/257 instances and widths 1/2/4/8/16/32.
- No measured-round allocation:
  model ownership review, `hot_allocations == 0`, and sanitizer run.
- Strict process output cannot silently accept corrupt metrics:
  `test_bench_lcwe_model`.
- Median and verdict cannot cherry-pick:
  aggregation tests and raw samples CSV.
- Capsule packing cost is included:
  capsule callback boundary and timing placement inspection.
- AoSoA cannot create `PROMISING`:
  native-only classifier regression test.
- The exact three fixed workloads are all reported:
  summary CSV and tracked report.
- Existing project behavior is unchanged:
  fresh `make test-quick`, CMake build, and full CTest evidence.
- User-owned dirty files remain unchanged and unstaged:
  final `git status --short` plus scoped commit contents.

Only after every item has fresh evidence should the next Executor ABI plan be
created.
