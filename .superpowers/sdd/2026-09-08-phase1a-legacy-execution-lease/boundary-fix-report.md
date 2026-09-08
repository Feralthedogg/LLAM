# Phase 1A boundary-audit hardening report

Status: complete for the scanner/build/documentation scope.

Fix commit: `018d920c8b393fbf19b04638162f084a367d4837`
(`fix: harden phase 1a boundary audit`).

## What changed

`scripts/check_legacy_lease_boundary.py` now validates token and call
relationships instead of allowing an entire source line or function body from
one matching substring.  The Phase 0 scanner remains unchanged and its five
explicit wrapper families (Rust, Python, JVM, .NET, and Go sources) remain out
of scope because this audit only walks native `src/**/*.c` and `src/**/*.h`.

The active-G allowlist requires the exact `_Atomic(llam_task_t *) active_g;`
field at direct depth in `struct llam_worker`, the two
`atomic_init(&rt->shards[index].{primary_worker,opaque_helper_worker}.active_g,
NULL)` forms, and direct `worker->active_g` arguments to the approved atomic
load/store helpers.  A second receiver on the same line is independently
rejected.

Watchdog `pthread_kill` calls are bound to the native-thread output of the
preceding lease snapshot and require matching owner/incarnation binding proof,
non-tautological `pthread_equal` revalidation, and the shard's
`opaque_lock -> lock` lifetime held through the call.  ACK matchers require one
live expression containing atomic boolean and all three tuple comparisons;
publication helpers require tuple stores before a nonzero ACK, while init,
shutdown, and named reset paths are zero-only.  Lease owner writes require the
audited local lease origin, claimant/sequence and live-binding proof; owner
aliases and indirect owner slots fail closed.  Lock tracking is per function,
recognizes lock/trylock/timedlock/unlock and simple shard aliases, and retains
the valid two-shard `first`/`second` ordering.

Make uses `$(PYTHON)` throughout the `test` recipe.  The production hook audit
uses a broad `llam_*_test_*` symbol policy, with the existing
`llam_broker_client_self_test_*` production self-test symbols explicitly
excluded; a scheduler-hook symbol fixture proves the policy catches
`llam_scheduler_test_rebind_primary`.  CMake keeps Python audit registration
conditional on `Python3_FOUND`, documented in the file, and the architecture
page now marks Phase 1A complete while pointing to Phase 1B P-field migration
and Phase 2 foreign-detach boundaries.

## Regression fixtures

`scripts/test_legacy_lease_boundary.py` retains all previous positive/negative
fixtures and adds 19 total tests covering:

* wrong active-G declaration type, wrong init receiver/value, non-atomic
  helper assignment, same-line extra receiver, and alternate atomic receiver;
* watchdog arbitrary operand, kill-before-snapshot, tautological equality,
  original-handle kill, and the actual validated-native-thread positive shape;
* dead-branch, aliased/bare, wrong-variable, and ACK-before-tuple matcher or
  publication forms;
* direct lease aliases, owner slots, atomic owner-slot stores, and unknown
  indirect owner writes;
* lock/trylock/timedlock reverse order through literals, a shard alias, and
  `first`/`second`, plus a valid two-shard ID-ordered merge shape; and
* the production scheduler test-hook symbol regression fixture.

The malformed fixtures were run RED against the pre-fix checker before the
checker changes, then GREEN after the implementation.  The unchanged Phase 0
fixture suite remains 20/20.

## Verification evidence (Darwin arm64, 2026-09-09)

* `python3 scripts/test_legacy_lease_boundary.py`: 19/19 passed.
* `python3 scripts/test_worker_identity_boundary.py`: 20/20 passed.
* `python3 scripts/check_legacy_lease_boundary.py --root .`: 187 native
  source/header files clean.
* `PYTHON=python3.14 make test_legacy_lease_boundary
  check_legacy_lease_boundary`: passed, confirming the override is honored.
* `make test`: passed, including normal runtime tests, no-owner checks,
  boundary tests, export audit, and production test-hook audit.
* `make test-asan`: passed, including `asan-test_legacy_lease_internal`; the
  known Darwin stack-switch `__asan_handle_no_return` warning was the only
  sanitizer warning.
* `make test-tsan`: passed, including `tsan-test_legacy_lease_internal`.
* `make analyze-cppcheck`: passed for 206 C/H test/runtime/example files.
* `make audit-production-test-hooks audit-shared-exports`: passed; 130 public
  shared exports and no disallowed production test-hook symbols.
* CMake Debug configure/build passed; full CTest passed 29/29, and a final
  post-edit boundary/export CTest subset passed 4/4.
* `git diff --check` passed, and public headers are byte-identical to the
  pre-fix `d47c87dbaf3a06477133ef25388675f374a0598d` tree.

Native Windows/BSD runtime and sanitizer execution was unavailable on this
Darwin host.  The scanner is intentionally lexical/conservative rather than a
full C AST/dataflow engine: unsupported macro-hidden or complex pointer
aliases are rejected or remain outside the recognized audited shapes, trylock
success is conservatively treated as held, and watchdog proof recognition is
limited to the native watchdog source path.  Generated build binaries in the
shared worktree were not staged or deleted.

The independent review's historical active-helper `llam_leave_blocking()`
failure did not reproduce in the fresh full `make test` or CMake/CTest runs
after the existing runtime tree was rebuilt; this scanner-only fix
does not modify runtime production code.  A separate 500-run characterization
did observe four unrelated existing sigaltstack-EINVAL test failures, so
cross-platform/repeated runtime flake follow-up remains a separate concern.
