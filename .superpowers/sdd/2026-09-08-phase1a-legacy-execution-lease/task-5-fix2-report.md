# Task 5 fix2 report — stale ACK admission and watchdog handoff races

## Status

Added the remaining deterministic Task 5 verification coverage in
`tests/test_legacy_lease_internal.c`. Production sources and public interfaces
were not changed.

### Stale merge ACK after a real helper handoff

`test_stale_merge_ack_rejected_after_helper_handoff()` now:

* requests a real merge pause and captures the scheduler-published request,
  lease-epoch, and worker-incarnation tuple;
* releases the pause and drives the real primary-to-helper enter path and
  helper-to-primary handback path, observing the live helper owner and the
  incremented lease epoch;
* requests a replacement merge pause after the handback and verifies that the
  replacement tuple matches while the captured tuple does not;
* under the production shard-id ordering (`opaque_lock` for both shards,
  followed by each `shard->lock`), replays the old epoch/incarnation in the ACK
  projection and proves `llam_shard_can_start_merge_locked`,
  `llam_shard_can_offline_locked`, and
  `llam_merge_runnable_queues_locked` all reject it; and
* restores the scheduler-published tuple and proves the same eligible,
  controlled-empty predicates accept it without migrating any queue entries.

The helper/root task is held at a runtime-owned channel boundary so the source
has no active-G or runnable queue while the predicates are checked. The test
returns the existing skip code when the host exposes only one online shard;
the matcher and handoff portions remain covered by the existing single-shard
fixtures.

### Watchdog helper handback race

`test_watchdog_signal_suppressed_after_helper_handoff()` holds the real
`BEFORE_WATCHDOG_SIGNAL` gate after the watchdog has selected a live overdue
primary task. While the sample is paused, the task performs a real
helper-backed `llam_enter_blocking()`/`llam_leave_blocking()` handback, which
advances the lease epoch while retaining the primary binding/native handle.
The task then retires before the gate is released so both the explicit test
monitor and the runtime controller must revalidate a stale or empty active-G
tuple. The test uses bounded joins and asserts that preempt-request and
preempt-signal counters do not increase after the handback.

The prior task-exit/reclaim watchdog race remains in the test list.

### Shutdown-side coverage decision

`tests/test_runtime_shutdown_internal.c` was intentionally left unchanged.
That binary exercises the process-default singleton teardown path, while the
lease pause fields and handoff gates are owned by explicit test-hook runtime
instances. There is no existing safe hook in that shutdown fixture to stage a
live ACK, perform a handoff, and restore the singleton without introducing a
second teardown authority or manually mutating shutdown state. The stronger
real-handoff predicate fixture above covers the admission contract without
changing shutdown behavior.

## TDD evidence

* Initial registration/compile RED: the new stale-ACK test was rejected as an
  unused function until it was added to the focused test list.
* Fixture RED: the first run exposed that host-side blocking channel send is
  not a valid API operation; channel creation was moved into the managed task
  and host wakeup uses the documented nonblocking try-send path.
* Watchdog RED: repeated runs exposed the independent controller's later
  sample after the explicit monitor gate was released. The fixture now retires
  the post-handoff task before releasing the gate, preserving the production
  signal path while making every concurrent sample stale/empty.
* GREEN: all focused runs pass with production sources unchanged.

## Verification

Fresh checks on Darwin arm64:

* `make test_legacy_lease_internal` passed; 20 consecutive serial executions
  passed.
* `test_runtime_shutdown_internal`, `test_runtime_stress`, and
  `test_multi_runtime_core` passed.
* CMake Debug focused CTest passed 4/4 for the lease, shutdown, stress, and
  multi-runtime binaries.
* CMake Release focused CTest passed 4/4 for the same four binaries.
* `make test-asan` passed its supported ASan/UBSan matrix, including the new
  lease fixture. Only the known Darwin stack-switch
  `__asan_handle_no_return` warnings were emitted.
* `make test-tsan` passed its supported TSan matrix, including the new lease
  fixture, without a TSan report.
* `python3 scripts/test_worker_identity_boundary.py` passed 20/20.
* `make audit-shared-exports audit-production-test-hooks` passed; the shared
  library still reports 130 public symbols and no production test-hook
  exports.
* `git diff --exit-code 2cde642 -- include/llam` passed.
* `git diff --check` passed for the test/report changes.

Native Windows/BSD execution was unavailable on this Darwin arm64 host.
