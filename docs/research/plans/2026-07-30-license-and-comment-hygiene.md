# License and Comment Hygiene Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every tracked LLAM code-bearing file machine-auditable as Apache-2.0 and document the ownership contracts of the highest-risk runtime and LEIR state machines without changing executable behavior.

**Architecture:** Add one dependency-free Python audit with focused fixture tests, wire it into Make, CMake, and Linux CI, then close every current marker gap with syntax-native SPDX headers. Add only file-level state maps and cross-boundary ownership contracts to selected runtime/LEIR files; public signatures, executable tokens, ABI, and build membership remain unchanged.

**Tech Stack:** Python 3 standard library, Git tracked-file inventory, C11/Doxygen comments, Make, CMake/CTest, GitHub Actions YAML.

## Global Constraints

- The repository license remains Apache-2.0.
- Existing full Apache boilerplate remains valid and is not rewritten.
- Strict JSON and other non-comment data formats are excluded.
- Vendored third-party files retain upstream licensing and are never relabeled.
- Shebangs remain the first line.
- Comments describe ownership, synchronization, generation, retirement, reuse, or public behavior; they do not restate C syntax.
- Runtime behavior, public ABI, build output, version `2.2.0`, performance verdicts, tags, and release state do not change.
- The unrelated working tree must be clean before each commit; every commit stages only the files named by its task.

---

### Task 1: License-header audit and fixture tests

**Files:**
- Create: `scripts/audit_license_headers.py`
- Create: `scripts/test_audit_license_headers.py`

**Interfaces:**
- Consumes: `git -C ROOT ls-files -z --cached` as the authoritative tracked-file inventory.
- Produces: `audit_license_headers.py --root ROOT --check`, returning `0` only when every eligible tracked file has either `SPDX-License-Identifier:` or the existing full Apache notice in its first 45 lines.
- Produces: deterministic diagnostics of the form `missing license marker: PATH`, sorted by path.

- [ ] **Step 1: Write fixture tests for eligibility and accepted headers**

Create tests that import the audit module and assert:

```python
self.assertTrue(module.is_code_bearing("src/core.c"))
self.assertTrue(module.is_code_bearing(".github/workflows/linux.yml"))
self.assertTrue(module.is_code_bearing("docker/linux/Dockerfile.ubuntu24"))
self.assertFalse(module.is_code_bearing("config/llam-version.json"))
self.assertTrue(
    module.has_license_marker(
        "#!/usr/bin/env python3\n"
        "# SPDX-License-Identifier: Apache-2.0\n"
    )
)
self.assertTrue(
    module.has_license_marker(
        "/* Licensed under the Apache License, Version 2.0 */\n"
    )
)
```

Also cover an identifier after line 45, malformed `Apache 2`, an unsupported
extension, and a strict JSON file.

- [ ] **Step 2: Write fixture tests for tracked-tree diagnostics**

Create a temporary Git repository containing one marked C file, one unmarked C
file, one shebang Python file, one JSON file, and one unmarked workflow. Add all
files to the index, run the CLI, and require sorted diagnostics for only the
unmarked C and workflow files. Add a second fixture in which all eligible files
are marked and require:

```text
license header audit passed: 3 code-bearing files
```

- [ ] **Step 3: Run the tests and observe the expected failure**

Run:

```bash
python3 -m unittest scripts/test_audit_license_headers.py -v
```

Expected: failure because `scripts/audit_license_headers.py` does not exist.

- [ ] **Step 4: Implement the dependency-free audit**

Implement:

```python
AUDITED_PREFIXES = (
    ".github/workflows/",
    "cmake/",
    "docker/",
    "examples/",
    "experiments/",
    "include/",
    "scripts/",
    "src/",
    "tests/",
)
CODE_SUFFIXES = {
    ".S", ".asm", ".c", ".cmake", ".go", ".h", ".inc",
    ".ps1", ".py", ".rs", ".s", ".sh", ".yaml", ".yml",
}
SPECIAL_NAMES = {"CMakeLists.txt", "Makefile"}
HEADER_LINE_LIMIT = 45
```

`is_code_bearing()` accepts the special names, files with a listed suffix under
an audited prefix, and names beginning with `Dockerfile` under `docker/`.
`tracked_paths()` invokes Git without a shell, rejects a failed inventory, and
decodes paths with the filesystem encoding. `audit()` uses `lstat()` and reports
missing, unreadable, non-regular, and unmarked eligible paths without following
tracked symlinks.

The CLI accepts only `--root` and `--check`. It emits all diagnostics to stderr,
prints the success receipt to stdout, and returns `1` for findings or `2` for an
unusable root/inventory.

- [ ] **Step 5: Run focused tests**

Run:

```bash
python3 -m unittest scripts/test_audit_license_headers.py -v
```

Expected: all fixture tests pass.

- [ ] **Step 6: Run the audit against the current tree and preserve the red result**

Run:

```bash
python3 scripts/audit_license_headers.py --root . --check
```

Expected: exit `1` with the current 37 unmarked tracked paths, proving the
repository gate detects the existing debt.

- [ ] **Step 7: Commit the audit and tests**

```bash
git add scripts/audit_license_headers.py scripts/test_audit_license_headers.py
git commit -m "test: audit repository license headers"
```

---

### Task 2: Close every current license-marker gap

**Files:**
- Modify: `.github/workflows/docs.yml`
- Modify: `docker/linux/Dockerfile.ubuntu22`
- Modify: `docker/linux/Dockerfile.ubuntu24`
- Modify: all 19 tracked files under `experiments/leir/`
- Modify: `include/llam/runtime_stats.h`
- Modify: `src/internal/runtime_stack_cache_types.h`
- Modify: `src/internal/runtime_trace_types.h`
- Modify: `tests/test_asan_fiber_positive.c`
- Modify: `tests/test_multi_runtime_stack_cache.inc`
- Modify: `tests/test_runtime_stack_cache_authority.inc`
- Modify: `tests/test_runtime_stack_cache_plan.inc`
- Modify: `tests/test_stack_vm_cases.inc`
- Modify: `tests/test_tsan_fiber_positive.c`
- Modify: `scripts/audit_build_manifests.py`
- Modify: `scripts/install.ps1`
- Modify: `scripts/package_release_windows.ps1`
- Modify: `scripts/test_audit_build_manifests.py`
- Modify: `scripts/test_audit_c_structure.py`
- Modify: `scripts/verify_windows.ps1`

**Interfaces:**
- Consumes: the eligibility and header-location policy from Task 1.
- Produces: zero unmarked files in the complete tracked inventory.

- [ ] **Step 1: Add syntax-native markers without moving shebangs**

Use these exact logical lines:

```text
SPDX-License-Identifier: Apache-2.0
Copyright 2026 Feralthedogg
```

Use `//` for C/C++-style research/test files, `#` for Python/YAML/Dockerfile,
and `#` for PowerShell. For Python and shell files, place both lines immediately
after the shebang. Do not alter any other token.

- [ ] **Step 2: Verify complete repository coverage**

Run:

```bash
python3 scripts/audit_license_headers.py --root . --check
```

Expected:

```text
license header audit passed: 401 code-bearing files
```

The exact count includes the two new Task 1 scripts. If the tracked inventory
changes before execution, record and verify the fresh count rather than
hard-coding it in the audit.

- [ ] **Step 3: Verify comment placement did not break parsers**

Run:

```bash
python3 -m py_compile \
  scripts/audit_build_manifests.py \
  scripts/audit_c_structure.py \
  scripts/audit_license_headers.py \
  scripts/test_audit_build_manifests.py \
  scripts/test_audit_c_structure.py \
  scripts/test_audit_license_headers.py
cmake -S . -B /tmp/llam-license-cmake \
  -DLLAM_BUILD_RESEARCH=OFF
```

Expected: Python compilation and CMake configuration succeed.

- [ ] **Step 4: Commit marker coverage**

Stage only the paths listed by the audit:

```bash
git add .github/workflows/docs.yml docker experiments/leir include/llam/runtime_stats.h \
  src/internal/runtime_stack_cache_types.h src/internal/runtime_trace_types.h \
  tests scripts/audit_build_manifests.py scripts/install.ps1 \
  scripts/package_release_windows.ps1 scripts/test_audit_build_manifests.py \
  scripts/test_audit_c_structure.py scripts/verify_windows.ps1
git commit -m "docs: complete source license markers"
```

Before committing, inspect `git diff --cached --word-diff=porcelain` and require
that every non-comment token is unchanged.

---

### Task 3: Integrate the audit into local and CI verification

**Files:**
- Modify: `Makefile`
- Modify: `CMakeLists.txt`
- Modify: `.github/workflows/linux.yml`
- Modify: `scripts/test_audit_build_manifests.py` only if the existing
  build/workflow audit requires its canonical fixture to include the new gate.
- Modify: `scripts/audit_build_manifests.py` only if a precommitted canonical
  hash must be refreshed for the exact Make/CMake/workflow additions.

**Interfaces:**
- Consumes: `scripts/audit_license_headers.py --root . --check`.
- Produces: Make target `audit-license-headers`.
- Produces: CTest tests `license_header_fixtures` and `license_headers`.
- Produces: Linux security-gate step `Verify license headers`.

- [ ] **Step 1: Add a Make target and make it part of `test`/`check`**

Add `audit-license-headers` to `.PHONY` and define:

```make
audit-license-headers:
	python3 -m unittest scripts/test_audit_license_headers.py -v
	python3 scripts/audit_license_headers.py --root . --check
```

Add it alongside `audit-build-manifests` and `audit-c-structure` in both
platform-specific `test check` prerequisite lists.

- [ ] **Step 2: Add CTest coverage**

Inside the existing `Python3_Interpreter_FOUND` block, add:

```cmake
add_test(
    NAME license_header_fixtures
    COMMAND ${Python3_EXECUTABLE}
            -m unittest scripts/test_audit_license_headers.py -v
)
add_test(
    NAME license_headers
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/audit_license_headers.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}
            --check
)
```

Give both tests the repository root as their working directory.

- [ ] **Step 3: Add the Linux security-gate step**

Immediately after the structure ratchet in `.github/workflows/linux.yml`, add:

```yaml
- name: Verify license headers
  run: make audit-license-headers
```

- [ ] **Step 4: Run integration gates and resolve only exact audit drift**

Run:

```bash
make audit-license-headers
python3 scripts/audit_build_manifests.py --root . --check
python3 -m unittest scripts/test_audit_build_manifests.py -v
cmake -S . -B /tmp/llam-license-cmake -DLLAM_BUILD_RESEARCH=OFF
ctest --test-dir /tmp/llam-license-cmake --output-on-failure \
  -R 'license_header'
```

If the canonical build-manifest audit reports an expected Make/CMake/workflow
projection change, update only the corresponding fixture or exact canonical
hash and rerun all four commands. Do not weaken command parsing or allow
unknown workflow structures.

- [ ] **Step 5: Commit integration**

```bash
git add Makefile CMakeLists.txt .github/workflows/linux.yml \
  scripts/audit_build_manifests.py scripts/test_audit_build_manifests.py
git commit -m "ci: enforce license header coverage"
```

Omit the two build-manifest paths from staging when they did not require a
change.

---

### Task 4: Document high-risk ownership state machines

**Files:**
- Modify: `src/core/wait/wait_tracking.c`
- Modify: `src/io/linux/watch/linux_segment.c`
- Modify: `src/io/linux/watch/linux_segment_cancel.c`
- Modify: `src/io/linux/watch/cqe.c`
- Modify: `src/io/windows/watch/windows_submit.c`
- Modify: `src/io/windows/watch/windows_completion.c`
- Modify: `src/io/darwin/watch/darwin_worker.c`
- Modify: `src/io/darwin/watch/darwin_completion.c`
- Modify: `experiments/leir/leir_program.c`
- Modify: `experiments/leir/leir_native_plan.c`
- Modify: `experiments/leir/leir_engine.c`
- Modify: `experiments/leir/leir_native_segment.c`

**Interfaces:**
- Consumes: existing runtime state names and enforced ordering.
- Produces: file-level contracts that identify owners, transitions, terminal
  conditions, retirement fences, and reuse points without changing code.

- [ ] **Step 1: Add the generic wait-owner contract**

Extend the `wait_tracking.c` file details with:

```text
publication -> exactly one active wait owner
completion/cancellation -> detach owner and result
reinjection -> runnable publication only after detach
reuse -> wait generation advanced and all owner pointers cleared
```

State that the atomic in-flight owner, not a concurrently rewritten advisory
shard field, is authoritative for completion routing.

- [ ] **Step 2: Add Linux native-segment lifecycle and cancellation contracts**

Document:

```text
IDLE -> QUEUED -> INFLIGHT -> RETIRING -> RETIRED -> IDLE
```

Distinguish semantic terminal, target retirement, cancel retirement, and
reusable. In `linux_segment_cancel.c`, state that a cancel CQE does not prove
target retirement and that operation SQEs must become visible before their
cancel SQEs. In `cqe.c`, state that tagged user data is only dereferenced after
the owning pin/generation checks and that task wakeup occurs after backend lock
release.

- [ ] **Step 3: Add Windows association and completion contracts**

In `windows_submit.c`, document that association plus generation pins the
HANDLE/SOCKET authority across close/reuse, and distinguish synchronous success
with suppressed IOCP from pending/posted completion. In
`windows_completion.c`, document that the completion path owns request cleanup
until the stale cancel control is removed, result fields are published, and
reinjection transfers ownership back to the task.

- [ ] **Step 4: Add Darwin event-pin and follow-up syscall contracts**

In `darwin_worker.c`, document the batch pin interval from `kevent()` return
through dispatch and unpin, including suppression when parent storage cannot be
pinned. In `darwin_completion.c`, document that readiness is not semantic I/O
completion: the follow-up syscall establishes the result, then watch/request
ownership is detached before task reinjection.

- [ ] **Step 5: Add LEIR program/plan/instance contracts**

Document:

- `leir_program.c`: validation copies an immutable descriptor; caller arrays may
  be released after creation.
- `leir_native_plan.c`: compilation accepts only a bounded acyclic static
  success chain; failure leaves `out` untouched.
- `leir_engine.c`: bind is an all-or-nothing publication from IDLE through
  BINDING; RUNNING excludes rebind and storage reuse.
- `leir_native_segment.c`: caller values are borrowed, duplicated descriptors
  and fixed scratch are instance-owned, kernel-visible tokens remain live until
  target and cancel retirement, and destroy is the final release boundary.

- [ ] **Step 6: Inspect for comment/code contradictions**

For every new assertion, locate the enforcing field, atomic transition, lock,
or test. Remove any sentence that relies only on design intent. Run:

```bash
git diff --check
git diff --word-diff=porcelain -- \
  src/core/wait \
  src/io/linux/watch \
  src/io/windows/watch \
  src/io/darwin/watch \
  experiments/leir
```

Expected: only comment tokens and the Task 2 license markers differ.

- [ ] **Step 7: Compile platform-visible syntax**

Run:

```bash
make clean all LLAM_BUILD_RESEARCH=0
make research LLAM_BUILD_RESEARCH=1
cmake -S . -B /tmp/llam-license-research -DLLAM_BUILD_RESEARCH=ON
cmake --build /tmp/llam-license-research
```

Expected: default and research builds succeed. Platform-specific native
Windows/Darwin compilation remains covered by their existing CI because the
change is comment-only.

- [ ] **Step 8: Commit comment contracts**

```bash
git add src/core/wait/wait_tracking.c src/io/linux/watch \
  src/io/windows/watch src/io/darwin/watch experiments/leir
git commit -m "docs: clarify runtime ownership state machines"
```

Review the staged diff and exclude every file without a deliberate comment
change.

---

### Task 5: Full verification and completion receipt

**Files:**
- Modify: no source files unless a verification failure proves an error in this
  change.

**Interfaces:**
- Consumes: all tasks above.
- Produces: reproducible license coverage and build/test receipts.

- [ ] **Step 1: Run focused audits**

```bash
make audit-license-headers
make audit-c-structure
python3 scripts/audit_build_manifests.py --root . --check
```

Expected: all pass.

- [ ] **Step 2: Run default and research CTest gates**

```bash
cmake -S . -B /tmp/llam-license-default -DLLAM_BUILD_RESEARCH=OFF
cmake --build /tmp/llam-license-default
ctest --test-dir /tmp/llam-license-default --output-on-failure
cmake -S . -B /tmp/llam-license-research -DLLAM_BUILD_RESEARCH=ON
cmake --build /tmp/llam-license-research
ctest --test-dir /tmp/llam-license-research --output-on-failure \
  -R 'license_header|leir_native_plan|leir_native_segment'
```

Expected: all selected tests pass.

- [ ] **Step 3: Prove executable source equivalence for comment-only files**

Use the C preprocessor on each comment-only C file before and after its task
commit where platform headers are available, or compare Git word diffs where
the host cannot preprocess a platform file. Require zero non-comment semantic
changes.

- [ ] **Step 4: Verify repository state**

```bash
git diff --check origin/leir-native-segment...HEAD
git status --short --branch
git log --oneline --decorate -5
```

Expected: no uncommitted changes and only the deliberate documentation/audit
commits ahead of the remote branch.

- [ ] **Step 5: Report exact results**

Report:

- tracked code-bearing file count and zero missing markers;
- focused unit-test count;
- Make/CMake/CTest results;
- the state-machine files reviewed;
- any platform validation deferred to existing native CI;
- commit hashes created by this plan.
