# Worker identity shutdown handback fix report

## Status

Complete for the active-helper shutdown interleaving diagnosed in
`worker-identity-flake-diagnosis.md`.

## Root-cause and TDD evidence

The new deterministic fixture in `tests/test_worker_identity_internal.c`
arms `LLAM_LEASE_TEST_AFTER_HANDBACK_ACK`, releases the primary, and waits for
the helper to publish a completed handback while the lease is still
`PRIMARY_OWNED`. It then starts destructive shutdown, waits for
`shutdown_requested` and `LLAM_LEASE_STOPPING`, and only then releases the
handback gate. The staged snapshots require a live primary owner, matching
nonzero transfer/ACK sequences, no pending return claimant, both worker
active-G values clear, an active helper, and no helper exit ACK.

The fixture was run before the production change against the current code:

```text
red_rc=1
test_worker_identity_internal: active-helper primary leave failed
```

After the production change, the same fixture passed. The focused worker test
also passed in 200 serial process runs:

```text
serial_runs=200 failures=0
```

## Production change

`llam_legacy_lease_handback_acknowledged_locked()` now distinguishes the two
valid `STOPPING` handback shapes:

1. An in-flight shutdown transfer retains the existing claimant, claimant
   incarnation, owner identity, and matching transfer/ACK token.
2. A handback completed before shutdown may have cleared the claimant. It is
   accepted only when transfer and ACK sequences match, the owner is the
   primary worker with the same incarnation, the return claimant and
   incarnation are both zero, and the primary binding is live.

The second shape is therefore a completed handback proof, not an ownerless
grant. `llam_legacy_lease_begin_stop_locked()`,
`llam_legacy_lease_complete_handback_locked()`,
`llam_legacy_lease_teardown_return_locked()`, and `llam_leave_blocking()` keep
their existing transfer, claimant-retention, teardown, and failure-boundary
semantics.

## Verification evidence (Darwin arm64)

* Focused Debug and Release worker/lease binaries passed.
* Full CMake Debug build and CTest passed: 29/29 tests.
* Full CMake Release build and CTest passed: 29/29 tests.
* `make test-fuzz-heavy` passed with 2048 runtime and 512 multi-runtime
  scenarios.
* `make test-asan` passed. Darwin emitted only the known ASan
  `__asan_handle_no_return` stack-switch warning; no ASan/UBSan failure.
* `make test-tsan` passed with no TSan reports.
* Worker boundary tests passed: 20/20. Legacy-lease boundary tests passed:
  13/13. The repository checker passed for 187 source/header files.
* `make analyze-cppcheck` passed for 206 files.
* Shared export audit passed with 130 public symbols; production test-hook
  audit passed with no test-hook exports.
* `git diff --check` passed. Public headers remained byte-identical to the
  Phase 0 baseline (`2cde642`).

Native Windows and BSD execution was unavailable on this Darwin arm64 host.
No public ABI, stats/P-fields, foreign detach, async path, or wrapper files
were changed.
