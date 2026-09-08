# Task 5 fix report — close pause-boundary lease races

## Status

Complete for the reviewed Task 5 findings.

* Primary scheduler binding now publishes a live owner tuple under
  `opaque_lock`, records an atomic post-pause reacquire deferral, and refuses
  task selection until the released pause boundary is revalidated. Existing
  stopping owners remain schedulable long enough to cancel parked waits and
  drain shutdown.
* New redirect transitions are rejected while a merge pause is requested;
  already-reserved handoffs and idempotent existing redirect observations are
  preserved.
* Pause ACK documentation now describes the request-sequence,
  lease-epoch, and worker-incarnation contract.
* Focused tests now exercise startup during an active pause, redirect
  serialization, a real watchdog scan-reference/task-exit race, and real
  runtime steal-pause generation reuse. The Make test-hook target compiles
  `watchdog_probe.c` with hooks so the pre-signal gate is active.

## TDD evidence

The startup regression failed before the implementation with:
`live startup binding did not acknowledge the merge pause`. It passed after
the lease publication/deferral change. The redirect test likewise targets the
previous mutating path; watchdog and ACK tests use real scheduler transitions,
task scan references, and pause ACK publication rather than manual field
equality alone.

## Verification

* `make test_legacy_lease_internal` passed, including 10 serial reruns.
* `test_runtime_shutdown_internal`, `test_runtime_stress`, and
  `test_multi_runtime_core` passed.
* CMake Debug and Release focused CTest passed (4/4 each).
* Focused ASan/UBSan and TSan lease binaries passed. ASan emitted only the
  known Darwin stack-switch `__asan_handle_no_return` warning.
* `python3 scripts/test_worker_identity_boundary.py` passed (20 tests).
* `make audit-production-test-hooks audit-shared-exports` passed; exports
  remain at 130 public symbols and no production test-hook symbols appeared.
* `git diff --check` and the public-header comparison against `2cde642`
  passed.
* `make test` reaches only the expected pre-Task-6 Phase 0 scanner failure
  for six existing `active_g` references; no unrelated failure appeared.

## Concerns

Native Windows/BSD execution was unavailable on this Darwin arm64 host. Public
ABI/stat schema, wrappers, P-fields, foreign detach, async context capture,
and compiler metadata were not changed.

## Commit

Pending final commit hash.
