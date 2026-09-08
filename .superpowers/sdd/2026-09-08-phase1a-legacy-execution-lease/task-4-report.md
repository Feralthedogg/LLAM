# Task 4 report — publish current tasks through worker state

## Status

Complete for the current-G and execution-publication migration.

Worker-local `active_g`, run-start, safepoint, and idle-wake timestamps are now
the execution publication authority. Dispatch, direct yield/reinject, blocking
and redirect return, task park/exit, watchdog/debug sampling, queue load, and
dynamic merge/offline checks use the worker helpers or the validated lease-owner
accessor. The shard `current` and shard execution timestamps were removed.
The active-G clear remains under the shard reclaim barrier before task scan
references can be released, and helper handback/exit acknowledgements retain
their existing cleanup ordering.

## Commit

`refactor: publish current tasks through worker state`

## Tests

RED evidence:

* Before the helper declarations existed, `make test_legacy_lease_internal`
  failed to compile with undeclared `llam_shard_load_lease_current` and
  `llam_worker_load_active_g` calls.
* The focused publication test then failed against the helper-only interim
  implementation because the worker safepoint timestamp was not published;
  it passed after the scheduler migration.

GREEN evidence on Darwin arm64:

* `test_legacy_lease_internal`, `test_runtime_core`, `test_runtime_stress`,
  and `test_runtime_shutdown_internal` passed.
* CMake Debug and Release focused CTest: 4/4 passed in each configuration.
* `-DNDEBUG -Werror` focused build and all four focused binaries passed.
* `make test-asan`: passed with ASan/UBSan; only the known Darwin
  `__asan_handle_no_return` stack-switch warnings were emitted.
* `make test-tsan`: passed with no TSan report. One earlier run had an
  intermittent active-helper leave assertion; 20 focused TSan reruns plus the
  final full matrix passed.
* `make audit-production-test-hooks audit-shared-exports`: passed; shared
  export audit reports 130 public symbols.
* `test_abi_contract`, `test_abi_compat`, `test_worker_identity_boundary.py`,
  `git diff --check`, and public-header identity against `eb0c014` passed.
* The current/source audit finds no `shard->current` or `&shard->current`
  references under `src` or tests.
* `make test` reaches the expected pre-Task-6
  `scripts/check_worker_identity_boundary.py` failure: six `active_g`
  references are still rejected by the Phase 0 scanner. Updating that
  phase-aware rule belongs to Task 6.

## Concerns

* Native Windows/BSD execution is unavailable on this Darwin host; the
  platform-neutral source/build checks remain available.
* The accessor requires callers to hold `opaque_lock` followed by `shard->lock`
  and its returned pointer must not outlive the shard lock. This lock-protected
  lifetime contract avoids relying on an atomic pointer/epoch alone.
* No public headers, exported symbols, public stats schema, queues, timers,
  allocator/cache, I/O routing, wrappers, metadata, or asynchronous
  preemption behavior were changed.
