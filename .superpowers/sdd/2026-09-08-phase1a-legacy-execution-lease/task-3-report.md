# Task 3 fix-round report — lease stop, handback, and return validation

Status: complete for the scoped Task 3 transition paths.

The interrupted handback test placeholder was removed and replaced with
deterministic clean-stop, failed-validation, active-helper shutdown, and
idle-helper shutdown coverage. Gate arrival is recorded by a test-only
reached flag; all new task joins use the existing bounded helpers and the test
main preserves explicit CTest skip status 77.

Implementation corrections:

* Cooperative `stop_requested` no longer blocks a recorded handback. Hard
  teardown via `shutdown_requested` prevents new grants, moves the coordinator
  to `STOPPING` without erasing owner/claimant/sequence state, and only clears
  that state after helper exit acknowledgement and join.
* `llam_leave_blocking()` now requires the exact return sequence, claimant
  incarnation, and published handback acknowledgement before restoring
  current/RUNNING. Failed validation keeps the task on the opaque return
  boundary and returns `ECANCELED`.
* Worker-local active-G is published at scheduler dispatch, cleared on return
  and both opaque transfer boundaries, and cleared before handback ACK. The
  helper exit acknowledgement is published only after active-G/current cleanup.
* Shutdown waits for helper exit acknowledgement without scheduler locks before
  joining. Helper target restoration validates the live primary binding and
  native handle instead of restoring a stale target.

Verification:

* TDD RED/GREEN regressions: clean-stop ACK gate and invalid handback
  validation both failed against the old behavior and pass with the fix.
* `make test_legacy_lease_internal`; direct focused binary: passed, plus 20
  consecutive direct runs.
* `test_runtime_core`, `test_runtime_stress`, and
  `test_runtime_shutdown_internal`: passed.
* CMake Debug and Release focused configure/build/CTest:
  `test_legacy_lease_internal` passed in each.
* Focused ASan/UBSan and TSan builds/runs: passed. ASan emitted only the
  existing Darwin `__asan_handle_no_return` stack-switch warning.
* `test_worker_identity_internal` and
  `python3 scripts/test_worker_identity_boundary.py`: passed.
* `make audit-production-test-hooks audit-shared-exports`: passed; 130 public
  shared symbols and no production test-hook symbols.
* `git diff --check`: passed.
* `make test` built and ran the reachable suite, then stopped at the existing
  Phase 0 worker-boundary scanner. It reports the six pre-existing Task 2
  `active_g` references plus the ten Task 3 active-G boundary references;
  updating that scanner is owned by Task 6.

Concerns: native Windows/BSD execution was unavailable on this Darwin/arm64
host. No wrapper, P-field migration, or asynchronous preemption behavior was
introduced.
