# Task 3 fix-round report — retain lease claimants through shutdown handback

Status: complete for the scoped Task 3 transition paths.

The second re-review fixes the remaining shutdown claimant, publication-gate,
and ownerless-episode assertion gaps:

* Destructive stop publishes `STOPPING` before waiting for the active run to
  drain. A STOPPING handback publishes its acknowledgement and claimant owner
  but retains `return_worker`/`return_incarnation` until the helper exit ACK and
  join; `llam_legacy_lease_finalize_stop_locked()` remains the only cleanup
  boundary. Cooperative stop without `shutdown_requested` still clears the
  claimant during normal handback.
* STOPPING handback validation and the narrowly scoped teardown-return fallback
  require the matching retained claimant, incarnation, transfer sequence, and
  acknowledgement.
* Every enabled handback gate now checks both worker-local `active_g` values and
  legacy `shard->current` under `opaque_lock -> shard->lock` before release.
* `test_ownerless_return_ordering` snapshots the exact helper claimant token in
  `REDIRECTED_NO_EXECUTOR`, the exact helper handback episode, the exact primary
  handback episode, and the exact post-run sequence/epoch cleanup.

Verification:

* TDD RED/GREEN: the retained-claimant shutdown assertion failed against the
  previous implementation and passed after the coordinator/shutdown fix.
* `make test_legacy_lease_internal`; direct focused binary: passed, plus 20
  consecutive direct runs.
* `test_runtime_core`, `test_runtime_stress`, and
  `test_runtime_shutdown_internal`: passed.
* CMake Debug and Release focused configure/build/CTest:
  `test_legacy_lease_internal` passed in each.
* `make test-asan` (ASan/UBSan matrix): passed. ASan emitted only the known
  Darwin stack-switch `__asan_handle_no_return` warning.
* `make test-tsan` (TSan matrix): passed.
* `python3 scripts/test_worker_identity_boundary.py`: 20 tests passed.
* `make audit-production-test-hooks audit-shared-exports`: passed; 130 public
  shared symbols and no production test-hook symbols.
* `git diff --check`: passed; no public headers changed.

Expected boundary limitation:

* `make test` reaches `python3 scripts/check_worker_identity_boundary.py` and
  stops on the known 16 `active_g` references (six Task 2 references plus ten
  Task 3 references). Updating this phase-aware scanner belongs to Phase 1A
  Task 6; no unrelated scanner error was observed.
* Native Windows/BSD execution was unavailable on this Darwin/arm64 host.

No wrapper, P-field migration, ABI/stat change, or asynchronous preemption
behavior was introduced.
