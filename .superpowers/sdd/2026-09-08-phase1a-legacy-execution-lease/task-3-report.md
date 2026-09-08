# Task 3 fix-round 3 report — abort shutdown-raced lease reservations

Status: complete for the scoped Task 3 transition paths.

The residual shutdown races are fixed without changing public interfaces:

* A destructive stop now aborts a primary-to-helper reservation before helper
  grant. If the primary publication was already cleared, the rollback restores
  the primary owner projection in `STOPPING`, restores `RUNNING` plus
  worker-local `active_g` and `shard->current` under the established lock order,
  clears the transient opaque-enter flags, and returns `ECANCELED`.
* The STOPPING return path now treats an existing return claimant as exclusive:
  only the exact worker/incarnation/sequence observation is idempotent. A live
  helper may reserve the primary return boundary only when no claimant exists.
  Mismatched rollback callers and return tokens are rejected.
* Deterministic regressions cover destruction at
  `AFTER_TRANSFER_REQUEST` and `AFTER_OWNER_CLEAR`, including task
  state/depth, active-G/current restoration, no helper grant, claimant cleanup,
  and bounded teardown. An ownerless-helper redirect test holds the handback
  and helper-exit ACK gates while a primary return races the retained claimant.

TDD RED/GREEN evidence:

* The owner-clear shutdown gate failed against `05722f7` with the stranded
  blocked task boundary.
* The ownerless STOPPING gate failed against `05722f7` because the primary
  overwrote the helper claimant.
* Both regressions pass after the minimal production changes.

Verification on Darwin arm64:

* `make test_legacy_lease_internal`; 20 consecutive direct runs passed.
* `test_runtime_core`, `test_runtime_stress`, and
  `test_runtime_shutdown_internal` passed.
* CMake Debug and Release focused configure/build/CTest passed (4/4 tests in
  each configuration).
* `make test-asan` passed with ASan/UBSan; only the known Darwin
  `__asan_handle_no_return` stack-switch warning was emitted.
* `make test-tsan` passed without a TSan report.
* `python3 scripts/test_worker_identity_boundary.py` passed (20 tests).
* `make audit-production-test-hooks audit-shared-exports` passed; the shared
  export audit reports 130 public symbols.
* `make test_abi_contract test_abi_compat`, both ABI binaries,
  `git diff --exit-code 2cde642 -- include/llam`, and `git diff --check`
  passed.

Expected boundary limitation:

* `make test` reaches the known Phase 0
  `scripts/check_worker_identity_boundary.py` failure: 17 `active_g`
  violations (the additional rollback publication reference is owned by the
  Phase 1A migration). Updating the phase-aware scanner belongs to Task 6; no
  unrelated scanner error was observed.

Native Windows/BSD execution was unavailable on this Darwin arm64 host.
Wrappers, P-field migration, ABI/stat schema, public wrappers, and asynchronous
preemption behavior remain unchanged.
