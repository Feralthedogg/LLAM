# Task 3 report — lease-coordinated helper, redirect, and return handoff

Status: complete for the scoped Task 3 transition paths.

The primary opaque-block path now reserves a transfer sequence while the
primary lease is live, marks the task blocked and clears `shard->current` under
`opaque_lock -> shard->lock`, clears the primary lease owner, and grants the
validated helper only after that publication is complete. Helper startup and
runtime-stop checks reject new grants; failed helper startup rolls back the
reserved sequence before redirect fallback.

Helper handback is now an explicit transfer/acknowledgement. The helper stops
fetching work when compensation depth reaches zero, clears its current
publication before completing the handback, publishes the matching sequence
ACK and fresh epoch, and only then drops the active projection. Primary return
waits for that completed boundary. Helper re-blocking and redirect fallback
retain the return worker/incarnation token; ownerless observations never grant
an arbitrary primary, and helper re-blocking never starts a second helper.

The test-hook snapshot now copies the real lease coordinator under the
established lock order. Characterization coverage pauses at transfer request,
owner clear, helper grant, and handback ACK, and checks sequences, epochs,
ownerless claimant validation, helper identity, redirect return tokens, and
active-state-before-ACK ordering. The Make test-hook link now compiles the
transition units with `LLAM_ENABLE_TEST_HOOKS` so those gates are active only
in focused test artifacts. Architecture notes document the updated boundary.

Tests run:

* `make test_legacy_lease_internal` plus five direct reruns: passed.
* `make test_runtime_shutdown_internal`: passed.
* `make test_runtime_stress`: passed, including blocking/preemption cases.
* `make test_runtime_core`: passed.
* Debug and Release CMake focused configure/build/CTest: passed.
* Focused ASan/UBSan and TSan builds/runs: passed. ASan emitted the existing
  Darwin `__asan_handle_no_return` stack-switch warning only.
* `python3 scripts/test_worker_identity_boundary.py`: passed.
* `make audit-production-test-hooks audit-shared-exports`: passed; shared
  export count remains 130 and production artifacts contain no test-hook
  symbols.
* `git diff --check` and public-header identity check: passed.

Expected limitation: `make test` reaches the existing Phase 0 boundary scanner
and fails on the six Task 2 `active_g` references. That scanner update belongs
to Phase 1A Task 6; all runtime tests and audits reached before that check pass.
Native Windows/BSD execution was unavailable on this Darwin host.
