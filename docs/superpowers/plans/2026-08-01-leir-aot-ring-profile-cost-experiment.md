# LEIR AOT Ring Profile and Cost Experiment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Attribute generated CONNECT-WRITE activation cost and compare three
strict Linux io_uring profiles against same-profile portable LEIR baselines.

**Architecture:** A research-only Linux selector converts an exact process
request into io_uring setup flags and makes explicit setup fail closed instead
of falling back. The existing generated ticket records direct module and ring
subphase timings, while the benchmark driver pairs and classifies samples by
profile without changing LEIR semantics or completion ownership.

**Tech Stack:** C11, liburing, LLAM private research runtime, Python 3
`unittest`, Make, CMake/CTest, GitHub Actions.

## Global Constraints

- Keep LEIR as the portable semantic contract and preserve all current
  completion barriers.
- Compile profile selection only in research-enabled Linux runtime targets.
- Do not add a public API, ABI promise, runtime option, or production layout
  field.
- Explicit profiles are exact: no silent setup-flag fallback.
- Pair generated-native samples only with the same-profile portable samples.
- Preserve independent completion ownership, one terminal wake, and zero hot
  allocations.
- Every new code-bearing file uses
  `LicenseRef-LLAM-Commercial-Reciprocity-1.0`; modified Apache files retain
  their existing notice.
- Do not add the prohibited branch/source marker requested by the user.
- Do not change a version, create a tag, package, publish, or release 3.0.0.
- Execute inline in the existing linked worktree; do not dispatch subagents.

---

### Task 1: Strict Linux ring-profile selector

**Files:**
- Create: `src/io/linux/runtime_io_ring_profile_linux_internal.h`
- Create: `src/io/linux/research_ring_profile.c`
- Create: `experiments/leir/test_leir_aot_ring_profile.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`
- Modify: `config/llam-sources.json`

**Interfaces:**
- Produces:
  `uint32_t llam_linux_research_ring_profile_compiled_capabilities(void)`.
- Produces:
  `int llam_linux_research_ring_profile_select(const char *requested,
  bool sqpoll_requested, uint32_t capabilities,
  llam_linux_research_ring_profile_config_t *out)`.
- Produces:
  `int llam_linux_research_ring_profile_setup_errno(int setup_result)`.
- The config contains canonical `name`, enum `kind`, and exact
  `setup_flags`.

- [ ] **Step 1: Write the selector contract test**

Create a Linux-only test whose table verifies:

```c
static const struct selector_case cases[] = {
    {"submit_all", LLAM_LINUX_RING_CAP_SUBMIT_ALL,
     LLAM_LINUX_RING_PROFILE_SUBMIT_ALL, IORING_SETUP_SUBMIT_ALL},
    {"coop_taskrun",
     LLAM_LINUX_RING_CAP_SUBMIT_ALL | LLAM_LINUX_RING_CAP_COOP_TASKRUN,
     LLAM_LINUX_RING_PROFILE_COOP_TASKRUN,
     IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN},
    {"defer_taskrun",
     LLAM_LINUX_RING_CAP_SUBMIT_ALL |
         LLAM_LINUX_RING_CAP_SINGLE_ISSUER |
         LLAM_LINUX_RING_CAP_DEFER_TASKRUN,
     LLAM_LINUX_RING_PROFILE_DEFER_TASKRUN,
     IORING_SETUP_SUBMIT_ALL | IORING_SETUP_SINGLE_ISSUER |
         IORING_SETUP_DEFER_TASKRUN},
};
```

Also assert that null, empty, and unknown strings fail with `EINVAL`; missing
capability bits fail with `ENOTSUP`; any explicit profile with SQPOLL fails
with `EINVAL`; setup `-EINVAL` and `-EOPNOTSUPP` normalize to `ENOTSUP`; and
other negative setup results preserve their positive errno.

- [ ] **Step 2: Add the test target and verify RED**

Add `test_leir_aot_ring_profile` to the research target lists and manifest,
then run in the Linux test container:

```bash
make clean
make -j2 LLAM_BUILD_RESEARCH=1 test_leir_aot_ring_profile
```

Expected: compilation fails because the header and selector functions do not
exist.

- [ ] **Step 3: Implement the minimal selector**

The header defines:

```c
#define LLAM_LINUX_RESEARCH_RING_PROFILE_ENV \
    "LLAM_RESEARCH_IO_URING_PROFILE"

typedef enum llam_linux_research_ring_profile {
    LLAM_LINUX_RING_PROFILE_SUBMIT_ALL = 1,
    LLAM_LINUX_RING_PROFILE_COOP_TASKRUN = 2,
    LLAM_LINUX_RING_PROFILE_DEFER_TASKRUN = 3,
} llam_linux_research_ring_profile_t;

typedef struct llam_linux_research_ring_profile_config {
    uint32_t kind;
    unsigned setup_flags;
    const char *name;
} llam_linux_research_ring_profile_config_t;
```

Capability bits are independent of setup-flag numeric values so tests can
simulate older headers. `compiled_capabilities()` exposes only flags present
under `#if defined(...)`. `select()` requires the exact bits for the requested
profile, rejects SQPOLL, zeros `out` on entry, and sets `errno` deterministically.

- [ ] **Step 4: Verify GREEN and manifest parity**

Run:

```bash
make -j2 LLAM_BUILD_RESEARCH=1 test-leir-aot-ring-profile
python3 scripts/audit_build_manifests.py --root . --check
```

Expected: selector tests pass and Make/CMake/manifest projections agree.

- [ ] **Step 5: Commit the selector**

```bash
git add src/io/linux/runtime_io_ring_profile_linux_internal.h \
  src/io/linux/research_ring_profile.c \
  experiments/leir/test_leir_aot_ring_profile.c \
  Makefile CMakeLists.txt config/llam-sources.json
git commit -m "research: select strict io_uring profiles"
```

---

### Task 2: Exact research ring initialization

**Files:**
- Modify: `src/io/engine/io_engine.c`
- Modify: `experiments/leir/test_leir_aot_ring_profile.c`

**Interfaces:**
- Consumes the selector interfaces from Task 1.
- Produces exact ring initialization when
  `LLAM_RESEARCH_IO_URING_PROFILE` is nonempty.
- Preserves the existing SQPOLL/default setup path when the setting is absent.

- [ ] **Step 1: Extend the test with real setup probes**

For every compiled profile, fork a child process that sets the exact profile,
initializes a one-worker runtime, and exits 0 on success or 77 only for
`ENOTSUP`, `EPERM`, or `EACCES`. Add a control child with no setting and assert
ordinary initialization still succeeds on the test host.

- [ ] **Step 2: Verify RED**

Run the selector test in Linux with an invalid explicit profile:

```bash
LLAM_RESEARCH_IO_URING_PROFILE=invalid \
  ./test_leir_aot_ring_profile
```

Expected: the integration assertion fails because runtime initialization has
not yet consumed the profile.

- [ ] **Step 3: Add the strict initialization branch**

Under `LLAM_BUILD_RESEARCH && LLAM_RUNTIME_BACKEND_LINUX`, read the setting
before the existing SQPOLL/default branches. If nonempty:

1. select exact flags with the current SQPOLL request state;
2. call `io_uring_queue_init_params()` once;
3. map only kernel `EINVAL`/`EOPNOTSUPP` to `ENOTSUP`;
4. initialize the same ring-ready, feature, eventfd, and `linux_submit_all`
   state used by the current successful path;
5. return failure without retrying fewer flags when exact setup fails.

Do not cache the profile in a public or production structure. The benchmark
already owns and prints the exact request, and strict success proves the
active setup.

- [ ] **Step 4: Verify exact behavior**

Run:

```bash
make -j2 LLAM_BUILD_RESEARCH=1 test-leir-aot-ring-profile
LLAM_RESEARCH_IO_URING_PROFILE=invalid \
  ./test_leir_aot_ring_profile
```

Expected: the normal test passes; the explicit invalid request is rejected in
the child contract without falling back.

- [ ] **Step 5: Commit ring initialization**

```bash
git add src/io/engine/io_engine.c \
  experiments/leir/test_leir_aot_ring_profile.c
git commit -m "research: require exact ring profile setup"
```

---

### Task 3: C benchmark schema and cost attribution

**Files:**
- Modify: `experiments/leir/leir_aot_linux.h`
- Modify: `experiments/leir/leir_aot_linux.c`
- Modify: `experiments/leir/leir_aot_connect_bench_support.h`
- Modify: `experiments/leir/leir_aot_connect_bench_support.c`
- Modify: `experiments/leir/bench_leir_aot_connect.c`
- Modify: `experiments/leir/test_leir_aot_linux_unit.c`

**Interfaces:**
- Extends `leir_aot_linux_metrics_t` with `prepare_ns`, `ring_ns`, and
  `resume_ns`.
- Extends benchmark options with canonical `ring_profile`.
- Emits schema-2 fields `ring_profile`, `bind_ns`, `execute_ns`,
  `aot_prepare_ns`, `aot_ring_ns`, and `aot_resume_ns`.

- [ ] **Step 1: Write failing option and metrics tests**

Add table-driven parsing cases for all three `--ring-profile` values and
rejections for missing/unknown values. Extend the Linux AOT unit fixture so a
successful ticket reports positive aggregate subphase timing and so portable
metrics require all `aot_*` values to remain zero.

- [ ] **Step 2: Verify RED**

Run in Linux:

```bash
make -j2 LLAM_BUILD_RESEARCH=1 \
  test_leir_aot_linux_unit bench_leir_aot_connect
./bench_leir_aot_connect --candidate portable --ring-profile submit_all \
  --family unix --concurrency 1 --payload 64 --activations 8
```

Expected: the binary rejects `--ring-profile` or omits the schema-2 fields.

- [ ] **Step 3: Implement monotonic elapsed accumulation**

Use `llam_now_ns()` and a saturating helper:

```c
static uint64_t elapsed_ns(uint64_t start, uint64_t finish) {
    return finish > start ? finish - start : 1U;
}
```

In `leir_aot_linux_ticket_run()`, time only:

- `module->prepare()` as `prepare_ns`;
- `llam_issue_linux_native_segment()` as `ring_ns`;
- `module->resume()` plus `module->copy_outputs()` as `resume_ns`.

In the benchmark, time bind and outer execute for both candidates. Add the
native ticket subphase values to worker-local totals. Set the profile process
setting before `llam_runtime_init_ex()` and print schema version 2.

- [ ] **Step 4: Enforce structural timing invariants**

Before printing a successful native sample, require:

```c
metrics.aot_prepare_ns > 0U &&
metrics.aot_ring_ns > 0U &&
metrics.aot_resume_ns > 0U &&
metrics.aot_prepare_ns + metrics.aot_ring_ns +
    metrics.aot_resume_ns <= metrics.execute_ns
```

Require the portable `aot_*` fields to equal zero. Use checked addition or
subtractive bounds so malformed counters cannot overflow the invariant.

- [ ] **Step 5: Verify GREEN**

Run portable and native Unix smoke samples for `submit_all`, then repeat for
each supported profile. Expected: one schema-2 line per process, exact profile
identity, unchanged structural counters, and valid timing decomposition.

- [ ] **Step 6: Commit benchmark instrumentation**

```bash
git add experiments/leir/leir_aot_linux.h \
  experiments/leir/leir_aot_linux.c \
  experiments/leir/leir_aot_connect_bench_support.h \
  experiments/leir/leir_aot_connect_bench_support.c \
  experiments/leir/bench_leir_aot_connect.c \
  experiments/leir/test_leir_aot_linux_unit.c
git commit -m "research: attribute AOT activation cost"
```

---

### Task 4: Profile-aware evidence classifier

**Files:**
- Modify: `scripts/test_bench_leir_aot_connect.py`
- Modify: `scripts/bench_leir_aot_connect.py`

**Interfaces:**
- `benchmark_command(..., ring_profile: str, ...)` passes the exact profile.
- Sample cell identity becomes
  `(ring_profile, family, concurrency, payload, activations)`.
- `run_screen(..., profiles: Sequence[str], ...)` returns per-profile
  capability, verdict, ratios, and recommendation.

- [ ] **Step 1: Write parser and pairing RED tests**

Update the sample fixture to schema 2 and add all six fields. Assert that
schema 1, an unknown profile, missing timing fields, portable nonzero AOT
subphases, native zero subphases, and a profile-mismatched pair are rejected.

- [ ] **Step 2: Write profile-isolation RED tests**

Use a fake executable fixture or patched `_run_sample()` to model:

1. all three profiles continuing;
2. `coop_taskrun` unavailable for both candidates while `submit_all`
   continues;
3. only native unavailable, which must be `INCOMPLETE`;
4. `defer_taskrun` correctness failure, which rejects only that profile;
5. recommendation ranking by median same-profile native CPU ratio, then p99,
   then wall ratio.

- [ ] **Step 3: Verify RED**

Run:

```bash
python3 -m unittest scripts/test_bench_leir_aot_connect.py -v
```

Expected: failures identify the absent schema-2 fields, profile key, and
per-profile verdict layer.

- [ ] **Step 4: Implement schema-2 validation**

Add `ring_profile` and timing fields to `FIELD_ORDER`; include the profile in
`CELL_FIELDS`; validate profile spelling; validate nonnegative clocks and the
portable/native subphase invariants; and make the formatter deterministic.

- [ ] **Step 5: Implement isolated collection and verdicts**

Nest matrix collection by profile. Record skip reasons separately from failed
samples. A profile is `UNAVAILABLE` only when every candidate/cell attempt
returns the same skip class and no sample exists. Preserve the existing
mechanism classifier within completed cells.

Keep `submit_all` as the control mechanism verdict. Optional profile results
appear under `profiles` and cannot turn a successful control into a release
authorization.

- [ ] **Step 6: Implement recommendation and projections**

Among profiles whose cells all `CONTINUE`, compute median native/portable CPU,
p99, and wall ratios. Sort by `(cpu_ratio, p99_ratio, wall_ratio, name)` and
write the first profile as `recommended_profile`. Include profile in raw CSV,
summary CSV, metadata parameters, verdict JSON, and Markdown rows.

- [ ] **Step 7: Verify GREEN**

Run the Python test module and a one-cell, one-sample Linux screen for all
profiles. Expected: deterministic artifacts, unavailable optional profiles
isolated, `release_authorized: false`, and no output directory reuse.

- [ ] **Step 8: Commit the classifier**

```bash
git add scripts/bench_leir_aot_connect.py \
  scripts/test_bench_leir_aot_connect.py
git commit -m "research: classify ring profiles independently"
```

---

### Task 5: CI, documentation, and research boundary

**Files:**
- Modify: `.github/workflows/leir-aot-research.yml`
- Modify: `docs/operations/benchmarks.md`
- Modify: `scripts/test_research_build_boundary.py`
- Modify: `scripts/audit_build_manifests.py` only if the audited Make recipe
  digest changes.

**Interfaces:**
- CI runs a smoke profile matrix and uploads schema-2 evidence.
- Default builds and packages contain no selector source/header or target.

- [ ] **Step 1: Write boundary and workflow RED tests**

Extend the research-boundary fixture to require the selector only in Linux
research projections and forbid it from stable package artifacts. Extend
manifest/workflow assertions so CI passes an explicit profile list.

- [ ] **Step 2: Verify RED**

Run:

```bash
python3 -m unittest scripts/test_research_build_boundary.py -v
python3 scripts/audit_build_manifests.py --root . --check
```

Expected: failure until the workflow and audited projections include the new
target/profile arguments.

- [ ] **Step 3: Update CI and operator documentation**

The research workflow first runs a one-cell smoke for all profiles, then runs
the frozen full matrix. Document exact CLI examples, `UNAVAILABLE` semantics,
cost-field meaning, and that profile recommendation is Linux-only research.
Do not change release jobs.

- [ ] **Step 4: Regenerate the audited recipe digest if required**

Use the audit helper's reported exact digest and update only its expected
constant. Re-run until the full manifest audit passes without warnings about
missing or shadowed targets.

- [ ] **Step 5: Commit CI and docs**

```bash
git add .github/workflows/leir-aot-research.yml \
  docs/operations/benchmarks.md \
  scripts/test_research_build_boundary.py \
  scripts/audit_build_manifests.py
git commit -m "ci: record AOT ring profile evidence"
```

---

### Task 6: Verification and evidence decision

**Files:**
- Create: `docs/superpowers/reports/2026-08-01-leir-aot-ring-profile-results.md`
- Generated locally only: `artifacts/leir-aot-ring-profiles/**`

**Interfaces:**
- Produces a decision of `CONTINUE`, `NARROW`, or `STOP` for ring-profile
  tuning and explicitly selects the next compiler/runtime experiment.

- [ ] **Step 1: Run focused local verification**

Run stable and research builds on macOS, then focused Make/CMake tests and
ASan/UBSan in Linux. Required checks include selector, generated module,
integration, ownership, benchmark parser, license, structure, build manifests,
shared exports, and production test-hook audits.

- [ ] **Step 2: Run the frozen Linux profile matrix**

On each available Linux machine, run:

```bash
python3 scripts/bench_leir_aot_connect.py \
  --binary ./bench_leir_aot_connect \
  --output-dir artifacts/leir-aot-ring-profiles/<machine> \
  --profiles submit_all,coop_taskrun,defer_taskrun \
  --families tcp,unix --concurrency 1,16 --payloads 64,4096 \
  --activations 256 --samples 5
```

Record unavailable profiles without weakening the control matrix.

- [ ] **Step 3: Write the tracked decision report**

The report contains environment identity, per-profile capability, medians and
confidence intervals, cost shares, structural counters, recommendation, and
one of:

- `CONTINUE`: a safe optional profile improves same-profile CPU/p99 without
  violating any cell;
- `NARROW`: the control remains valid but profile tuning does not dominate;
- `STOP`: the control mechanism or cost instrumentation fails its frozen
  contract.

Regardless of decision, state `3.0.0 release gate: BLOCKED` and
`Release authorized: no`.

- [ ] **Step 4: Run final repository verification**

Run:

```bash
git diff --check
make -s audit-build-manifests audit-license-headers audit-c-structure
make clean
make -s -j4 LLAM_BUILD_RESEARCH=0 check
make clean
make -s -j4 LLAM_BUILD_RESEARCH=1 research-test
```

Then repeat the relevant Linux CI commands in the Linux environment. Verify
the worktree contains no generated binaries or evidence directories.

- [ ] **Step 5: Commit the decision report**

```bash
git add docs/superpowers/reports/2026-08-01-leir-aot-ring-profile-results.md
git commit -m "docs: record AOT ring profile decision"
```

---

### Task 7: Publish research changes and monitor CI

**Files:**
- Update existing draft pull request metadata only.

**Interfaces:**
- Pushes the existing `leir-native-segment` branch.
- Keeps the pull request draft and dependent on the license-transition change.

- [ ] **Step 1: Verify publish scope**

Check `git status`, the complete base-to-head diff, commit messages, current
tree, and pull-request body. Confirm only intended research changes exist and
the prohibited marker is absent from current content and added lines.

- [ ] **Step 2: Push and update the draft pull request**

Push the branch, update the current head hash and research results, and retain
the explicit no-release and license dependency statements.

- [ ] **Step 3: Monitor every required check**

Wait for all Linux, macOS, Windows, BSD, sanitizer, stress, security, docs, and
research checks. Reproduce and fix any failure with a failing regression test
before another push.

- [ ] **Step 4: Report without releasing**

Report commit hashes, pull-request URL, profile decision, validation counts,
and the next research branch. Do not bump a version, create or move a tag,
publish a package, or trigger a release workflow.
