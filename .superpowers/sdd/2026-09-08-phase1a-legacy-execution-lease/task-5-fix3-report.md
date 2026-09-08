# Task 5 fix3 report — worker-incarnation watchdog invalidation

## Status

Closed both remaining Task 5 re-review gaps. The focused lease test now
requires the helper-backed handoff path, and a deterministic pre-signal gate
test replaces the primary worker binding with a fresh incarnation while the
watchdog owns a real task scan reference. Production behavior, public headers,
statistics, P-fields, foreign detach, asynchronous context capture, and
language wrappers remain unchanged.

## Changes

### Helper-path assertion

`test_watchdog_signal_suppressed_after_helper_handoff()` now checks
immediately after the real `llam_enter_blocking()` return, under
`source->opaque_lock`, that:

* `opaque_helper_thread_started` and `opaque_helper_active` are true;
* the lease is `LLAM_LEASE_HELPER_OWNED` and its owner is
  `&opaque_helper_worker`; and
* the helper's incarnation is nonzero and
  `llam_worker_binding_live()` accepts it.

The fixture therefore fails if enter falls back to redirect, while retaining
the existing helper-to-primary handback and lease-epoch assertions.

### Worker-incarnation replacement race

`test_watchdog_signal_suppressed_after_worker_rebind()` enables
`LLAM_LEASE_TEST_BEFORE_WATCHDOG_SIGNAL`, waits for a real overdue watchdog
sample, and verifies that the sampled task has a nonzero `scan_refs` count.
The task holds active-G steady until the sample is pinned. A test-only
`llam_scheduler_test_rebind_primary()` request is then serviced by the owning
shard-0 scheduler context at a task-switch boundary. It performs the normal
signal-stack uninstall, `llam_scheduler_unbind_identity()`,
`llam_scheduler_bind_identity()`, and signal-stack reinstall lifecycle; the
host thread never writes worker or lease matcher fields.

The test verifies a larger live primary incarnation with the same lease epoch,
republished active-G, and the still-held scan reference. It then retires the
task, releases the watchdog gate, and asserts that preempt-request and
preempt-signal counters, plus the task's `preempt_requested` bit, remain
unchanged. All joins and gate waits use the existing three-second bounded
cleanup helpers.

The internal hook is compiled only with `LLAM_ENABLE_TEST_HOOKS`. Its header
contract documents that callers hold no runtime locks, keep the runtime/shard
alive, and keep the shard-0 scheduler running for the bounded call. The
transaction is claimed and executed only by that scheduler's native thread.

## Shutdown coverage waiver

No shutdown-side ACK fixture was added. `test_runtime_shutdown_internal` owns
the process-default singleton teardown, while lease pause gates and the
versioned ACK state belong to explicit test-hook runtime instances. There is no
safe existing fixture boundary to stage a live ACK, replace a worker binding,
and restore the singleton without introducing a second teardown authority or
manually mutating shutdown state. The normal shutdown binary remains covered;
the explicit shutdown/partial-init transfer-state scenario remains a plan
waiver until a safe singleton fixture exists.

## TDD evidence

* RED: adding the new regression first failed the focused compile because the
  test-only scheduler rebinding pathway did not yet exist.
* GREEN: the test-only hook was added with a scheduler-context lifecycle
  transaction; the focused binary then passed normally and in a 20-run serial
  loop. A signal-stack lifecycle correction was required so the new incarnation
  had a matching installed diagnostic stack.

## Verification (Darwin arm64)

Passed:

* `make -B test_legacy_lease_internal`; standalone focused lease test and 20
  consecutive serial runs.
* `test_runtime_shutdown_internal`, `test_runtime_stress`, and
  `test_multi_runtime_core`.
* CMake Debug and Release focused CTest: 4/4 each for lease, shutdown, stress,
  and multi-runtime tests.
* `make test-asan`: supported ASan/UBSan matrix, including the new fixture;
  only the known Darwin stack-switch `__asan_handle_no_return` warnings were
  emitted. The final focused ASan target also passed after the last hook-only
  cleanup.
* `make test-tsan`: supported TSan matrix without a TSan report. The final
  focused TSan target also passed after the last hook-only cleanup.
* `python3 scripts/test_worker_identity_boundary.py`: 20/20 passed.
* `make audit-shared-exports audit-production-test-hooks`: passed; 130 public
  shared symbols and no production test-hook exports.
* `make test_abi_contract` and `make test_abi_compat`.
* `git diff --check` and
  `git diff --exit-code 2cde642 -- include/llam`.

`make test` remains stopped by the known pre-Task-6 Phase 0 execution-
authority scanner: six pre-existing `active_g` findings in `init.c`,
`scheduler.c`, and `runtime_types.h`. No unrelated test failure was reached.

Native Windows/BSD execution was unavailable on this Darwin arm64 host.
