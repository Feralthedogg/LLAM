# Research and Build Governance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make stable LLAM builds research-free by default, preserve an
explicit research build with identical public ABI, and produce immutable,
source-bound performance evidence and enforceable build/structure audits.

**Architecture:** A private `LLAM_BUILD_RESEARCH` value controls one
repository-wide internal layout and target graph. Declarative manifests and
executable audits keep Make, CMake, packages, and source lists consistent.
Benchmark producers finalize fresh hash-bound bundles, while a separate hard
gate converts the stored classifier verdict into release eligibility.

**Tech Stack:** C11, POSIX/Windows platform C, GNU Make, CMake/CTest, Python 3
standard library, `nm`/`dumpbin`-compatible export audits, SHA-256.

## Global Constraints

- `LLAM_BUILD_RESEARCH` defaults to `0`/`OFF`, is private, and must never
  appear in public headers or installed target usage requirements.
- Research-on and research-off builds must expose identical installed headers,
  ABI metadata, ABI major `2`, SONAME, pkg-config/CMake interfaces, and public
  dynamic symbols.
- A research-enabled build must fail packaging before creating an artifact.
- Toggling research mode in the same Make object directory must force a
  complete layout-compatible rebuild.
- Default `all`, `test`, CMake default build, and package paths must not compile
  or run research code.
- Evidence destinations are create-once; audit mode is read-only and validates
  raw data, provenance, derived files, verdict schema, and `MANIFEST.sha256`.
- A release/manual gate exits nonzero unless the required verdict is exactly
  `SPECIALIZED`.
- The current library version remains `2.2.0`; this plan does not tag or
  release because the native performance verdict is `REJECT`.
- Existing cancellation, generation, wait-owner, I/O retirement,
  cross-runtime `EXDEV`, public-prefix ABI, and production/test-hook separation
  invariants remain unchanged.
- Darwin, Linux, Windows, and supported BSD build graphs remain valid.

---

### Task 1: Make blocking accept result-disposal coverage deterministic

**Files:**

- Modify: `tests/test_runtime_shutdown_internal.c`
- Verify only: `src/io/api/blocking_ops.c`

**Interfaces:**

- Consumes: `LLAM_BLOCKING_RESULT_TEST_BEFORE_CREATE`,
  `LLAM_BLOCKING_RESULT_TEST_CREATED`, and
  `LLAM_BLOCKING_RESULT_TEST_DISCARDED`.
- Produces: a deterministic test protocol in which the TCP connection exists
  before accept retries, and cancellation occurs only after a result was
  created but before it is returned.

- [ ] **Step 1: Preserve a failing baseline**

Build and repeatedly run the current internal shutdown test:

```bash
make -j4 test_runtime_shutdown_internal
for i in $(seq 1 80); do
  ./test_runtime_shutdown_internal || exit 1
done
```

Expected on the unmodified test: at least one run may fail with a diagnostic
containing `kind=3`, `gate=1`, `created=0`, `discarded=0`. If scheduling does
not reproduce it in 80 runs, preserve the previously captured failure as the
RED evidence and continue; do not alter production timing to induce it.

- [ ] **Step 2: Change the test state to represent both gates**

Replace the ambiguous `release_gate` meaning with atomics named
`connection_ready`, `created`, `release_created`, and `discarded`. Initialize
all four to zero:

```c
atomic_init(&state.connection_ready, 0U);
atomic_init(&state.created, 0U);
atomic_init(&state.release_created, 0U);
atomic_init(&state.discarded, 0U);
```

- [ ] **Step 3: Make the hook enforce the production boundary**

For accept `BEFORE_CREATE`, publish `gate_reached`, wait for
`connection_ready`, and return. For accept `CREATED`, publish the value and
`created`, then wait for `release_created`. Other result kinds retain their
existing created-result gate:

```c
if (kind == LLAM_BLOCKING_RESULT_TEST_ACCEPT &&
    event == LLAM_BLOCKING_RESULT_TEST_BEFORE_CREATE) {
    atomic_store_explicit(&state->gate_reached, 1U, memory_order_release);
    while (atomic_load_explicit(&state->connection_ready,
                                memory_order_acquire) == 0U) {
        sched_yield();
    }
    return;
}
if (event == LLAM_BLOCKING_RESULT_TEST_CREATED) {
    state->created_value = value;
    atomic_store_explicit(&state->created, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release_created,
                                memory_order_acquire) == 0U) {
        sched_yield();
    }
}
```

Preserve the existing single-entry protection for events that may repeat.

- [ ] **Step 4: Connect before cancellation and cancel after creation**

In the canceller, wait for `gate_reached`, connect the loopback client, publish
`connection_ready`, wait for `created`, cancel the token, and publish
`release_created`:

```c
state->connect_result = connect(
    state->client,
    (const struct sockaddr *)&state->listener_address,
    sizeof(state->listener_address));
state->connect_errno = errno;
atomic_store_explicit(&state->connection_ready, 1U, memory_order_release);
while (atomic_load_explicit(&state->created,
                            memory_order_acquire) == 0U) {
    llam_yield();
}
state->cancel_result = llam_cancel_token_cancel(state->token);
state->cancel_errno = errno;
atomic_store_explicit(&state->release_created, 1U, memory_order_release);
```

For getaddrinfo/open, keep cancellation after `CREATED` and release that same
created-result gate. Every cleanup/error path must publish all gates before
joining tasks so a failed socket setup cannot deadlock the test.

- [ ] **Step 5: Prove deterministic disposal**

Run:

```bash
make -j4 test_runtime_shutdown_internal
for i in $(seq 1 100); do
  ./test_runtime_shutdown_internal || exit 1
done
```

Expected: 100/100 pass; accept asserts connect success, created/discarded
identity, closed descriptor, and caller `ECANCELED`.

- [ ] **Step 6: Commit**

```bash
git add tests/test_runtime_shutdown_internal.c
git commit -m "test: stabilize canceled accept disposal"
```

### Task 2: Add the private research target boundary

**Files:**

- Modify: `CMakeLists.txt`
- Modify: `Makefile`
- Create: `scripts/test_research_build_boundary.py`
- Modify: `.github/workflows/ci.yml`
- Modify: `.github/workflows/leir-native-research.yml`
- Modify: `docs/build.md`

**Interfaces:**

- Produces: CMake option `LLAM_BUILD_RESEARCH` and Make variable
  `LLAM_BUILD_RESEARCH`, both default off; explicit `research` and
  `research-test` Make targets; Python CLI
  `test_research_build_boundary.py --source ROOT --work DIR`.
- Later tasks consume the same numeric compile definition to guard private
  runtime layout.

- [ ] **Step 1: Write the boundary test**

Create a `unittest` that uses temporary Make and CMake build directories and
asserts:

```python
class ResearchBoundaryTests(unittest.TestCase):
    def test_default_graph_has_no_experiment_objects(self):
        self.assertNotIn("/experiments/", default_make_trace)
        self.assertNotIn("test_leir_", default_cmake_targets)

    def test_explicit_graph_has_research_targets(self):
        self.assertIn("test_leir_native_plan", research_cmake_targets)
        self.assertIn("/experiments/leir/", research_make_trace)

    def test_package_rejects_research_mode(self):
        self.assertNotEqual(package.returncode, 0)
        self.assertIn("research-enabled builds cannot be packaged",
                      package.stderr)
        self.assertEqual(created_archives, [])
```

Use command arrays and
`subprocess.run(command, cwd=root, check=False, text=True,
capture_output=True)` with a test-owned temp directory. Treat missing
Make/CMake as `skipTest`, not success.

- [ ] **Step 2: Run the new test and record RED**

Run:

```bash
python3 -m unittest scripts/test_research_build_boundary.py -v
```

Expected: FAIL because the option does not exist and default CMake exposes
research targets.

- [ ] **Step 3: Add one private build value**

In CMake:

```cmake
option(LLAM_BUILD_RESEARCH
       "Build private LLAM research implementations and experiments" OFF)
set(LLAM_RESEARCH_DEFINE
    "LLAM_BUILD_RESEARCH=$<BOOL:${LLAM_BUILD_RESEARCH}>")
target_compile_definitions(llam_runtime PRIVATE ${LLAM_RESEARCH_DEFINE})
target_compile_definitions(llam_runtime_shared PRIVATE
                           ${LLAM_RESEARCH_DEFINE})
```

Apply the same definition to internal test-hook runtime objects. It must not
be `PUBLIC` or `INTERFACE`.

In Make:

```make
LLAM_BUILD_RESEARCH ?= 0
RESEARCH_CPPFLAGS = -DLLAM_BUILD_RESEARCH=$(LLAM_BUILD_RESEARCH)
CPPFLAGS := $(CPPFLAGS) $(RESEARCH_CPPFLAGS)
```

Reject values other than `0` or `1`. Add the value to ordinary, shared, and
test-hook build-signature content.

- [ ] **Step 4: Guard all research target definitions**

Place the complete existing block from `set(LLAM_LEIR_PHASE0_SOURCES` through
the last SREM model target/test registration between
`if(LLAM_BUILD_RESEARCH)` and `endif()`. This includes every
`experiments/leir`, `experiments/lcwe`, `experiments/lccf`, and
`experiments/srem` executable, target property, CTest registration, and
benchmark contract test.

In Make, exclude research objects and links from default `BUILD_OBJS`,
`LINK_TARGETS`, `all`, and `test`. Define explicit research lists and:

```make
research: $(RESEARCH_LINK_TARGETS)
research-test: test-leir-phase0 test-leir-native test-leir-native-linux \
	test-lcwe-model test-lccf-model test-srem-model
```

When off, an explicit research target prints how to enable the mode and exits
nonzero rather than mixing object layouts.

- [ ] **Step 5: Fail packaging at entry**

At the first recipe line of every package entry point and inside
`scripts/package_release.sh`, check the normalized value and fail before
creating package directories:

```sh
if [ "${LLAM_BUILD_RESEARCH:-0}" != 0 ]; then
    echo "research-enabled builds cannot be packaged" >&2
    exit 2
fi
```

- [ ] **Step 6: Make CI intent explicit**

Standard platform workflows configure `LLAM_BUILD_RESEARCH=OFF` and do not
name research binaries. The dedicated LEIR workflow configures
`LLAM_BUILD_RESEARCH=ON` and uses `research-test`.

- [ ] **Step 7: Run boundary and build tests**

Run:

```bash
python3 -m unittest scripts/test_research_build_boundary.py -v
make clean
make -j4 all test
make clean
make -j4 LLAM_BUILD_RESEARCH=1 research research-test
cmake -S . -B build-off -DLLAM_BUILD_RESEARCH=OFF
cmake --build build-off -j4
ctest --test-dir build-off --output-on-failure
cmake -S . -B build-on -DLLAM_BUILD_RESEARCH=ON
cmake --build build-on -j4
ctest --test-dir build-on --output-on-failure
```

Expected: all pass. Default build logs contain no experiment compilation.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt Makefile scripts/test_research_build_boundary.py \
  .github/workflows/ci.yml .github/workflows/leir-native-research.yml \
  docs/build.md
git commit -m "build: isolate research targets by default"
```

### Task 3: Remove native research internals from production layouts

**Files:**

- Modify: `src/internal/runtime_types.h`
- Modify: `src/internal/runtime_state.h`
- Modify: `src/internal/runtime_proto_io.h`
- Modify: `src/io/runtime_io_api_internal.h`
- Modify: `src/io/linux/runtime_io_watch_linux_internal.h`
- Modify: `src/core/base/io_udata.c`
- Modify: `src/core/memory/io_object_alloc.c`
- Modify: `src/core/lifecycle/init.c`
- Modify: `src/core/lifecycle/shutdown.c`
- Modify: `src/core/wait/wait_tracking.c`
- Modify: `src/io/api/issue.c`
- Modify: `src/io/watch/watch_queue.c`
- Modify: `src/io/linux/watch/linux_submit.c`
- Modify: `src/io/linux/watch/cqe.c`
- Modify: `src/io/darwin/watch/kqueue_watch.c`
- Modify: `src/io/windows/watch/iocp_watch.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`
- Modify: `scripts/test_research_build_boundary.py`

**Interfaces:**

- Consumes: the exact numeric macro `LLAM_BUILD_RESEARCH`.
- Produces: research-off internal layouts and dispatch tables with no LEIR
  completion sink or Linux native segment state; research-on behavior remains
  unchanged.

- [ ] **Step 1: Add layout/symbol RED assertions**

Extend the boundary test to compile a small internal size probe in both modes,
inspect object/dependency lists, and assert:

```python
self.assertNotIn("linux_segment.c", off_compile_commands)
self.assertNotIn("llam_issue_linux_native_segment", off_static_symbols)
self.assertNotIn("LLAM_IO_UDATA_LINUX_NATIVE", off_preprocessed_state)
self.assertGreater(on_runtime_type_size, off_runtime_type_size)
```

Also assert installed public header hashes and dynamic exports are equal.

- [ ] **Step 2: Run the focused test and record RED**

Run:

```bash
python3 -m unittest \
  scripts.test_research_build_boundary.ResearchBoundaryTests.test_native_internals_absent_when_off \
  -v
```

Expected: FAIL because production Linux sources and members are unconditional.

- [ ] **Step 3: Guard declarations and layouts**

Wrap native segment forward declarations, request/node fields, CQE tags,
prototypes, and Linux internal includes with:

```c
#if LLAM_BUILD_RESEARCH
/* native research-only declaration or field */
#endif
```

Use numeric `#if`, not `#ifdef`, so mode `0` excludes code. Apply the same
guard to the LEIR completion-sink private test path.

- [ ] **Step 4: Guard initialization and generic dispatch**

Guard initialization, reset, teardown, queued/inflight abort, issue,
completion, and test-sink branches. Research-off `switch` statements must have
no unreachable native tag values and must retain fail-closed defaults for
unknown userdata.

- [ ] **Step 5: Exclude platform implementation sources**

Exclude:

```text
src/io/linux/watch/linux_segment.c
src/io/linux/watch/linux_segment_cancel.c
src/io/linux/watch/linux_segment_resources.c
```

from runtime source/object lists when research is off. Include them, their
private header signatures, and every dependent object only when on.

- [ ] **Step 6: Verify both layouts and behavior**

Run:

```bash
python3 -m unittest scripts/test_research_build_boundary.py -v
make clean
make -j4 LLAM_BUILD_RESEARCH=0 all test
make clean
make -j4 LLAM_BUILD_RESEARCH=1 research-test
cmake --build build-off -j4
ctest --test-dir build-off --output-on-failure
cmake --build build-on -j4
ctest --test-dir build-on --output-on-failure
```

Expected: all pass; off symbols/sources/tags are absent and public ABI parity
holds.

- [ ] **Step 7: Commit**

```bash
git add src Makefile CMakeLists.txt scripts/test_research_build_boundary.py
git commit -m "build: exclude native research internals"
```

### Task 4: Establish canonical version and source manifests

**Files:**

- Create: `config/llam-version.json`
- Create: `config/llam-sources.json`
- Create: `scripts/audit_build_manifests.py`
- Create: `scripts/test_audit_build_manifests.py`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`
- Modify: `include/llam/runtime.h`
- Modify: `src/core/base/abi.c`
- Modify: `scripts/package_release.sh`
- Modify: `.github/workflows/ci.yml`

**Interfaces:**

- Produces: JSON manifests with schema `llam.build-manifest.v1`; CLI
  `audit_build_manifests.py --root PATH [--check]`.
- Consumers: Make/CMake/headers/package scripts continue using generated or
  audited projections and cannot silently drift.

- [ ] **Step 1: Write fixture-based RED tests**

Create temporary mini repositories and assert:

```python
def test_rejects_missing_and_extra_members(self):
    result = run_audit(fixture("source-drift"))
    self.assertEqual(result.returncode, 1)
    self.assertIn("missing from Make", result.stderr)
    self.assertIn("extra in CMake", result.stderr)

def test_rejects_version_drift(self):
    result = run_audit(fixture("version-drift"))
    self.assertEqual(result.returncode, 1)
    self.assertIn("2.2.1 != 2.2.0", result.stderr)
```

Also cover duplicates, platform mismatch, wrong link dependency, malformed
JSON, and a clean fixture.

- [ ] **Step 2: Run RED**

Run:

```bash
python3 -m unittest scripts/test_audit_build_manifests.py -v
```

Expected: FAIL because the manifest audit does not exist.

- [ ] **Step 3: Define exact manifest schemas**

`config/llam-version.json` contains:

```json
{
  "schema": "llam.build-manifest.v1",
  "version": "2.2.0",
  "abi_major": 2
}
```

`config/llam-sources.json` contains ordered arrays for stable common sources,
each stable platform set, internal tests, public tests, and each research
target with its source and link dependencies. Record the current repository
members without changing behavior.

- [ ] **Step 4: Implement a fail-closed audit**

The audit parses manifests and extracts Make/CMake declarations without
evaluating arbitrary code. It emits sorted diagnostics and exits `0` only when
version, source membership, target membership, platform classification, and
link dependencies match. Include known fixes:

```text
test_leir_native_plan links llam_runtime in Make and CMake
Windows IOCP/HANDLE test objects are in Make BUILD_OBJS/signatures
runtime_io_segment_linux_internal.h is in research-on private signatures
```

- [ ] **Step 5: Wire normal build and dependency files**

Add `-MMD -MP` to Make compile rules and include generated `.d` files. Add
`audit-build-manifests` to Make test and a CTest with the same command. Header,
ABI, package, and CMake version constants must be audited against the version
manifest.

- [ ] **Step 6: Verify and commit**

Run:

```bash
python3 -m unittest scripts/test_audit_build_manifests.py -v
python3 scripts/audit_build_manifests.py --root . --check
make -j4 audit-build-manifests
ctest --test-dir build-off -R build_manifest --output-on-failure
ctest --test-dir build-on -R build_manifest --output-on-failure
```

Expected: all pass.

```bash
git add config/llam-version.json config/llam-sources.json \
  scripts/audit_build_manifests.py scripts/test_audit_build_manifests.py \
  Makefile CMakeLists.txt include/llam/runtime.h src/core/base/abi.c \
  scripts/package_release.sh .github/workflows/ci.yml
git commit -m "build: audit canonical source manifests"
```

### Task 5: Add create-once evidence bundles

**Files:**

- Create: `scripts/evidence_bundle.py`
- Create: `scripts/test_evidence_bundle.py`
- Modify: `scripts/bench_leir_native.py`
- Modify: `scripts/bench_leir_native_pipeline.py`

**Interfaces:**

- Produces:
  `EvidenceBundle.create(final_dir, metadata)`,
  `write_bytes(name, data)`, `finalize()`, and
  `audit_bundle(path, recompute)`.
- Bundle schema: `llam.performance-evidence.v1`; verdict schema:
  `llam.performance-verdict.v1`.

- [ ] **Step 1: Write security and crash-consistency RED tests**

Use temporary directories and assert:

```python
def test_refuses_existing_destination(self):
    final.mkdir()
    with self.assertRaises(FileExistsError):
        EvidenceBundle.create(final, metadata())

def test_finalize_writes_manifest_last_and_is_read_only(self):
    bundle = build_valid_bundle()
    before = snapshot(bundle)
    audit_bundle(bundle, recompute=fake_recompute)
    self.assertEqual(before, snapshot(bundle))

def test_rejects_tamper_and_provenance_mismatch(self):
    mutate(bundle / "raw.csv")
    with self.assertRaises(EvidenceError):
        audit_bundle(bundle, recompute=fake_recompute)
```

Also test path traversal, symlink/non-regular targets, duplicate entry names,
partial writer failure, unknown schema, missing raw cells, invalid hash lines,
source commit mismatch, and a valid bundle.

- [ ] **Step 2: Run RED**

Run:

```bash
python3 -m unittest scripts/test_evidence_bundle.py -v
```

Expected: FAIL because the module does not exist.

- [ ] **Step 3: Implement exclusive staging and atomic finalization**

The API creates a sibling temporary directory with mode `0700` and opens each
artifact with:

```python
fd = os.open(
    staging_dir / entry_name,
    os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
    0o600,
)
```

It fsyncs regular files and the staging directory, writes sorted SHA-256 lines
for every artifact except the manifest, writes `MANIFEST.sha256` last, fsyncs,
and performs one rename that fails if the final path exists. Clean only the
uniquely created staging directory on failure.

Reject absolute names, `..`, separators, symlinks, non-regular files, unknown
entries, duplicate names, and malformed UTF-8/JSON where applicable.

- [ ] **Step 4: Define and validate provenance**

`metadata.json` binds:

```json
{
  "schema": "llam.performance-evidence.v1",
  "source_commit": "40 hex characters",
  "source_dirty_digest": "sha256 hex or clean",
  "architecture": "normalized string",
  "kernel": "string",
  "toolchain": "string",
  "commands": [],
  "cpu_policy": {},
  "matrix": {},
  "sample_schedule": {},
  "classifier": {
    "schema": "llam.native-classifier.v1",
    "thresholds": {}
  }
}
```

Audit compares caller-required commit/digest and recomputes summary and verdict
from `raw.csv`; derived bytes must match stored files and manifest hashes.

- [ ] **Step 5: Migrate both benchmark scripts**

Remove overwrite helpers and direct final-directory writes. Both scripts use
the shared API and emit exactly `raw.csv`, `summary.csv`, `metadata.json`,
`verdict.json`, report, and manifest. Their `--audit-existing` path opens
read-only and never calls a write function.

- [ ] **Step 6: Verify and commit**

Run:

```bash
python3 -m unittest scripts/test_evidence_bundle.py -v
python3 -m unittest scripts/test_bench_leir_native.py -v
python3 -m unittest scripts/test_bench_leir_native_pipeline.py -v
```

Expected: all pass, including malformed exit-77 and tamper cases.

```bash
git add scripts/evidence_bundle.py scripts/test_evidence_bundle.py \
  scripts/bench_leir_native.py scripts/bench_leir_native_pipeline.py
git commit -m "feat: seal benchmark evidence bundles"
```

### Task 6: Separate benchmark screening from the promotion gate

**Files:**

- Modify: `scripts/bench_leir_native_pipeline.py`
- Modify: `scripts/test_bench_leir_native_pipeline.py`
- Modify: `.github/workflows/leir-native-research.yml`
- Modify: `docs/operations/benchmarks.md`

**Interfaces:**

- Produces CLI option `--require-verdict SPECIALIZED` and verdict JSON fields
  `portable_verdict`, `platform_verdict`, `required_cells`, and
  `classifier_thresholds`.

- [ ] **Step 1: Add RED classifier/gate tests**

Add table-driven fixtures that assert:

```python
cases = [
    ("SPECIALIZED", "SPECIALIZED", 0),
    ("REJECT", "SPECIALIZED", 1),
    ("INCONCLUSIVE", "SPECIALIZED", 1),
]
```

Add missing/non-trivial-cell, insufficient-sample, CPU regression, p99
regression, fixed-resource, batch-width, and structural-accounting cases.
Assert a platform-specific win cannot change a portable rejection.

- [ ] **Step 2: Run RED**

Run:

```bash
python3 -m unittest scripts/test_bench_leir_native_pipeline.py -v
```

Expected: new hard-gate tests fail because current REJECT exits zero.

- [ ] **Step 3: Freeze classification and gate semantics**

Keep the existing precommitted thresholds in metadata/verdict. The collector
always completes a valid bundle and exits zero for a valid measured `REJECT`.
After finalization or in `--audit-existing`, the optional hard gate compares
the exact verdict and exits `1` for mismatch, `2` for invalid evidence.

Filter promotion cells using the explicit non-trivial matrix predicate rather
than truthiness or row presence. Report portable and Linux/io_uring verdicts
independently.

- [ ] **Step 4: Wire workflows**

Pull-request research jobs collect and audit without requiring promotion.
Manual/release jobs run:

```bash
python3 scripts/bench_leir_native_pipeline.py \
  --audit-existing "$EVIDENCE_DIR" \
  --require-source "$GITHUB_SHA" \
  --require-verdict SPECIALIZED
```

- [ ] **Step 5: Verify and commit**

Run:

```bash
python3 -m unittest scripts/test_bench_leir_native_pipeline.py -v
python3 -m unittest scripts/test_evidence_bundle.py -v
```

Expected: all pass; a stored REJECT is valid evidence but fails the hard gate.

```bash
git add scripts/bench_leir_native_pipeline.py \
  scripts/test_bench_leir_native_pipeline.py \
  .github/workflows/leir-native-research.yml docs/operations/benchmarks.md
git commit -m "ci: enforce native promotion verdict"
```

### Task 7: Make the C structure audit scope-aware and ratcheted

**Files:**

- Modify: `scripts/audit_c_structure.py`
- Create: `scripts/test_audit_c_structure.py`
- Create: `config/c-structure-baseline.json`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`
- Modify: `.github/workflows/ci.yml`
- Modify: `docs/testing.md`

**Interfaces:**

- Produces CLI:
  `audit_c_structure.py --root PATH --mode report|ratchet|strict
  --baseline FILE`; structured categories `size_budget`,
  `split_candidate`, and `growth`.

- [ ] **Step 1: Write fixture RED tests**

Build fixture roots and assert:

```python
def test_strict_promotes_explicit_budget(self):
    result = audit("explicit-budget", "--mode", "strict")
    self.assertEqual(result.returncode, 1)
    self.assertIn('"category": "size_budget"', result.stdout)

def test_ratchet_scans_tests_and_experiments(self):
    result = audit("growth", "--mode", "ratchet")
    self.assertEqual(result.returncode, 1)
    self.assertIn("tests/large_test.c", result.stdout)
    self.assertIn("experiments/model.c", result.stdout)
```

Also test normal report success, new oversized file, unchanged grandfathered
file, one-line growth, reduced file, malformed baseline, and deterministic
ordering.

- [ ] **Step 2: Run RED**

Run:

```bash
python3 -m unittest scripts/test_audit_c_structure.py -v
```

Expected: FAIL because strict mode currently uses message-text matching and
tests/experiments are absent.

- [ ] **Step 3: Return structured findings**

Represent each finding as:

```python
Finding(
    path=relative_path,
    line_count=line_count,
    limit=limit,
    category="size_budget",
    severity="error" if mode == "strict" else "warning",
)
```

Scan `include`, `src`, `examples`, `tests`, and `experiments`. Strict mode
promotes both explicit budget and generic split findings. Ratchet mode fails a
new oversized file or growth beyond the checked-in line-count/limit baseline.

- [ ] **Step 4: Record visible existing debt**

Generate `config/c-structure-baseline.json` with schema
`llam.c-structure-baseline.v1`, exact relative paths, line counts, and limits.
Do not raise generic limits to hide current files. The known
`test_security_capability.c`, shutdown internal test, LEIR tests,
`wait_tracking.c`, `issue.c`, and `linux_segment.c` debt remains explicit.

- [ ] **Step 5: Wire CI**

Run fixture tests plus repository ratchet in Make `test`, CTest, and standard
CI. Keep `--mode report` available locally; reserve strict for completed
split/budget scopes.

- [ ] **Step 6: Verify and commit**

Run:

```bash
python3 -m unittest scripts/test_audit_c_structure.py -v
python3 scripts/audit_c_structure.py --root . --mode ratchet \
  --baseline config/c-structure-baseline.json
make -j4 audit-c-structure
ctest --test-dir build-off -R c_structure --output-on-failure
```

Expected: all pass.

```bash
git add scripts/audit_c_structure.py scripts/test_audit_c_structure.py \
  config/c-structure-baseline.json Makefile CMakeLists.txt \
  .github/workflows/ci.yml docs/testing.md
git commit -m "build: ratchet C structure budgets"
```

### Task 8: Prove stable/research parity and document the boundary

**Files:**

- Modify: `scripts/test_research_build_boundary.py`
- Modify: `docs/abi.md`
- Modify: `docs/testing.md`
- Modify: `docs/operations/benchmarks.md`
- Modify: `README.md`

**Interfaces:**

- Consumes all prior tasks.
- Produces one end-to-end parity receipt from the existing boundary test and
  user-facing stable/research build instructions.

- [ ] **Step 1: Add end-to-end parity RED assertions**

Build/install both modes into separate temporary prefixes. Compare:

```python
assert sha256_tree(off / "include") == sha256_tree(on / "include")
assert public_exports(off_library) == public_exports(on_library)
assert abi_probe(off_library) == abi_probe(on_library)
assert pkg_config_contract(off) == pkg_config_contract(on)
assert cmake_package_contract(off) == cmake_package_contract(on)
assert soname(off_library) == soname(on_library)
```

Assert the off package has no experiment files and the on package command
fails before any archive appears.

- [ ] **Step 2: Run the focused test and record RED or first green**

Run:

```bash
python3 -m unittest \
  scripts.test_research_build_boundary.ResearchBoundaryTests.test_installed_contract_parity \
  -v
```

Expected before completing integration: FAIL with the first uncovered parity
gap. If it passes immediately, record it as first-green evidence because prior
tasks already delivered the behavior.

- [ ] **Step 3: Close only observed parity gaps**

Fix build/install metadata that the comparison identifies. Do not add public
research symbols or change ABI/version to make the comparison pass.

- [ ] **Step 4: Document exact commands and release decision**

Document:

```bash
make -j4 all test
make -j4 LLAM_BUILD_RESEARCH=1 research-test
cmake -S . -B build -DLLAM_BUILD_RESEARCH=OFF
cmake -S . -B build-research -DLLAM_BUILD_RESEARCH=ON
```

State that research-on cannot package, evidence is create-once, the current
native verdict is `REJECT`, and no version/tag/release follows from this plan.

- [ ] **Step 5: Run the plan completion gate**

Run:

```bash
python3 -m unittest \
  scripts/test_research_build_boundary.py \
  scripts/test_audit_build_manifests.py \
  scripts/test_evidence_bundle.py \
  scripts/test_bench_leir_native.py \
  scripts/test_bench_leir_native_pipeline.py \
  scripts/test_audit_c_structure.py -v
make clean
make -j4 all test
make clean
make -j4 LLAM_BUILD_RESEARCH=1 research-test
cmake --build build-off -j4
ctest --test-dir build-off --output-on-failure
cmake --build build-on -j4
ctest --test-dir build-on --output-on-failure
```

Expected: all pass; research-off remains the stable default and research-on
retains the complete experimental suite.

- [ ] **Step 6: Commit**

```bash
git add scripts/test_research_build_boundary.py docs/abi.md docs/testing.md \
  docs/operations/benchmarks.md README.md
git commit -m "docs: define stable and research build contracts"
```
