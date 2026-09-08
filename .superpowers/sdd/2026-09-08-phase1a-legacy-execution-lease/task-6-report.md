# Task 6 report — integration audits, documentation, and whole-branch verification

Status: complete for the Phase 1A integration boundary.

Integration commit: `ea7fb79` (`test: enforce phase 1a lease boundaries`).

## Boundary audit

The new `scripts/check_legacy_lease_boundary.py` delegates all existing Phase 0
worker-identity checks to `check_worker_identity_boundary.py`.  The Phase 0
scanner is otherwise unchanged.  Only these active-G references are allowed:

* the `active_g` member declaration inside `struct llam_worker`;
* initialization of the embedded `primary_worker.active_g` and
  `opaque_helper_worker.active_g` slots in `init.c`; and
* atomic access inside `llam_worker_publish_active_g`,
  `llam_worker_load_active_g`, and `llam_worker_clear_active_g` in
  `scheduler.c`.

The Phase 1A layer rejects direct shard `current`/`preempt_thread` authority,
non-identity processor members and copied P resources, `owns_p`, unversioned
merge/steal ACK reads or writes, watchdog target use without the lease snapshot
and binding revalidation, reverse `shard->lock`/`opaque_lock` acquisition, and
owner writes outside the validated lease transition helpers.  It scans only
native C/H sources, so the explicitly excluded Rust, Python, JVM, .NET, and Go
wrappers are not pulled into this phase's dependency or authority boundary.

`scripts/test_legacy_lease_boundary.py` contains 13 fixture tests covering:

* the three allowed active-G surfaces and rejection of active-G elsewhere;
* direct and array-indexed second `shard->current` writers;
* `processor->executor`, `worker->owns_p`, `worker->preempt_target`, and
  `processor->current_g`;
* processor queue/timer/allocator copies and copied-resource struct fields;
* independent `shard->preempt_thread` reads and writes;
* bare merge/steal pause ACK decisions and valid tuple-versioned publication/
  matcher code;
* watchdog native-thread/target reads and unvalidated `pthread_kill()` calls;
* ownerless lease grants, including a guard that appears after the write; and
* reverse scheduler lock order plus CLI repository failure reporting.

The fixture test first produced the expected RED `ModuleNotFoundError` before
the phase-aware checker existed.  After the checker and fixtures were added,
the suite passed all 13 tests.

## Build and registration

Make now provides `test_legacy_lease_boundary` and
`check_legacy_lease_boundary`, runs both from `make test`, and runs both in the
ASan/UBSan and TSan recipes.  The Windows CTest regex includes both names.
CMake/CTest registers both Python tests and keeps the existing Phase 0 unit
test (`test_worker_identity_boundary`) intact; the old repository-level check
is replaced by the phase-aware repository check so the full CTest matrix is
green.  Generated Python caches are covered by `scripts/__pycache__` in
`CLEAN_DIRS`; the scripts use temporary directories and leave no persistent
fixture files.  Existing native and sanitizer test binaries remain in
`CLEAN_FILES`.

The focused lease test received two test-only bounded failure returns for
defensive `shard == NULL` branches, eliminating cppcheck's nullability
warnings without changing runtime code or ABI.

## Verification evidence (Darwin arm64)

* `make test`: passed, including the new boundary test/audit, all normal
  runtime tests, no-owner tests, export audit, and production test-hook audit.
* `make test-hardening`: completed after the focused-test nullability fix;
  cppcheck, cargo-audit, ASan/UBSan, TSan, boundary tests, and the 2048/512
  runtime fuzz matrix passed.  The first hardening attempt stopped only on the
  two now-fixed cppcheck warnings.
* `make test-fuzz-heavy`: passed (`2048` runtime and `512` multi-runtime
  scenarios).
* CMake Debug and Release full builds plus CTest: `29/29` tests passed in each
  configuration.  New `test_legacy_lease_boundary` and
  `check_legacy_lease_boundary` entries passed in both.
* `make test-asan` and `make test-tsan`: passed, including the focused lease
  binaries and the new Python boundary checks.  ASan emitted only the known
  Darwin stack-switch `__asan_handle_no_return` warning.
* `python3 scripts/test_worker_identity_boundary.py`: 20/20 passed.
* `python3 scripts/test_legacy_lease_boundary.py`: 13/13 passed.
* `python3 scripts/check_legacy_lease_boundary.py --root .`: passed for 187
  source/header files.
* `make audit-production-test-hooks audit-shared-exports`: passed; 130 public
  exports and no production test-hook symbols.
* `git diff --check`: passed.
* `git diff --exit-code 2cde642 -- include/llam`: passed; public headers are
  byte-identical.

Native Windows and BSD execution was unavailable on this Darwin arm64 host;
platform-neutral CMake/CTest registration and existing compatibility sources
remain covered.  `make clean` was not run in the shared worktree while other
task artifacts may still be owned by sibling work; sanitizer recipes removed
their own binaries through their existing traps.  No unrelated files were
deleted.

The verified design spec required no clarification edits; its lease state
machine, ownerless claimant, handback acknowledgement, lock order, watchdog
lifetime snapshot, pause generations, Phase 1B/Phase 2 boundaries, and wrapper
exclusion already match the implementation.  The architecture page now
records the exact audit allowlist and prohibited-authority rules.
