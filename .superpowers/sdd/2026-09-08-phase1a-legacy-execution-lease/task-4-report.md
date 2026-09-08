# Task 4 report — current-G publication fix round 1

## Status

Complete for the reported current-G publication gaps. I/O and sleep park paths
now retire the current worker's active-G under `shard->lock` before switching
to the scheduler. Both direct reinject paths stamp the worker's monotonic
`last_safepoint_ns` before release-publishing active-G. Test-hook-only reached
gates cover state preparation/publication, direct yield/reinject, I/O/sleep
parking, and task-exit sampling; production builds compile those calls as
no-ops. The Make build now links test-hook variants of `reinject.c` and
`yield_join_sleep.c`, so the new gates are active in focused tests.

## TDD evidence

* RED: after adding the gates/tests but before production gate calls, the
  focused binary failed at `task state preparation gate was not reached before
  active-G publication`.
* GREEN: after the scoped fixes, the focused lease binary passed, including
  deterministic checks that PARKED I/O/sleep tasks have NULL active-G/current,
  direct handoff timestamps are monotonic, and exit/reclaim sampling sees a
  DEAD task with cleared publication.

## Commit

`fix: retire active tasks before park and reinject`

## Tests

Passed on Darwin arm64:

* `make test_legacy_lease_internal`; focused lease binary 20/20 consecutive
  runs.
* `make test_runtime_core test_runtime_stress test_runtime_shutdown_internal
  test_runtime_io_dump test_connect_io`; all binaries passed. `test_io_buffers`
  and `test_worker_identity_internal` also passed in standalone reruns.
* CMake Debug and Release focused CTest: 4/4 passed in each configuration.
* Separate `-DNDEBUG -Werror` build and focused lease/core/stress/shutdown
  binaries passed.
* `make test-asan`: full ASan/UBSan matrix passed; only the known Darwin
  `__asan_handle_no_return` stack-switch warnings appeared.
* `make test-tsan`: full TSan matrix passed with no TSan report.
* `make audit-production-test-hooks audit-shared-exports`: passed; 130 public
  shared symbols and no production test-hook symbols.
* ABI contract/compatibility tests, worker-boundary unit tests (20/20),
  `git diff --check`, and source audit for `shard->current` passed.

`make test` ran all preceding suites successfully and stopped only at the
expected pre-Task-6 Phase 0 scanner. The deterministic scanner reports these
six known violations:

```
src/core/lifecycle/init.c:891,904
src/core/sched/scheduler.c:534,542,550
src/internal/runtime_types.h:1143
```

All six are the scanner's current prohibition on the worker-local `active_g`
member; Task 6 owns the phase-aware scanner update.

## Concerns

Native Windows/BSD execution was unavailable on this Darwin host. No public
headers, public ABI/stat schema, queues, timers, allocator/I/O ownership,
wrappers, metadata, watchdog target/preempt-thread migration, or asynchronous
preemption behavior was changed.
