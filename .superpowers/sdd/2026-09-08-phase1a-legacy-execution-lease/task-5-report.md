# Task 5 report — lease-derived watchdog targets and versioned pause ACKs

## Status

Complete for watchdog target derivation and pause acknowledgement publication.

* Added the exact watchdog lease snapshot and pause-ACK matcher interfaces.
* Removed `shard->preempt_thread` as an independent target authority. The
  watchdog now snapshots the lease owner, worker incarnation, lease epoch, and
  live native binding under `opaque_lock`, acquires a task scan reference under
  `shard->lock`, releases locks for the optional test gate, revalidates the
  tuple, and sends only the existing advisory cooperative signal.
* Added request sequences plus lease epoch/worker incarnation tuples for merge
  and runtime-wide steal pause acknowledgements. Lease transfer and merge
  mutation require an active, matching pause publication; stale ACKs cannot
  authorize offlining.
* Helper handback/exit no longer restores a mutable signal target. A live
  matching primary binding is revalidated by the lease snapshot instead.

## Commit

`fix: derive watchdog targeting from lease lifetime`

## Tests

* TDD RED: the new focused test first failed to compile because the watchdog
  snapshot/matcher interfaces and versioned fields were absent.
* TDD GREEN: `make test_legacy_lease_internal`; focused lease checks passed,
  including stale worker incarnation, request-generation reuse, task-exit
  overlap, and dynamic pause behavior.
* `test_runtime_core`, `test_runtime_stress`,
  `test_runtime_shutdown_internal`, and `test_multi_runtime_core` passed.
* CMake Debug and Release focused CTest passed (4/4 each).
* Serial `-DNDEBUG -Werror` focused/core/stress/shutdown/multi-runtime checks
  passed. A concurrent run of multiple binaries produced one transient dynamic
  pause ACK failure because they share process-global signal/runtime state; 20
  serial focused reruns passed.
* `make test-asan` passed the ASan/UBSan matrix; only the known Darwin
  `__asan_handle_no_return` stack-switch warnings appeared.
* `make test-tsan` passed the TSan matrix without a TSan report.
* `python3 scripts/test_worker_identity_boundary.py` passed (20 tests).
* `git diff --exit-code 2cde642 -- include/llam`, ABI contract/compatibility
  tests, `git diff --check`, `make audit-shared-exports`, and
  `make audit-production-test-hooks` passed; the export audit reports 130
  public symbols and no production test-hook symbols.
* `make test` reaches the expected Phase 0 scanner failure only: six
  `active_g` violations in `init.c`, `scheduler.c`, and `runtime_types.h`.
  These are the worker-local active-G migration findings owned by Task 6; no
  unrelated scanner finding remains.

## Concerns

Native Windows/BSD execution was unavailable on this Darwin arm64 host. Public
headers, exports, stats schema, wrappers, P-field ownership, compiler metadata,
foreign detach, and asynchronous context capture remain unchanged.
