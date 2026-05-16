# LLAM ChangeLog

## 1.0.2 - 2026-05-16

This entry summarizes changes staged after the 1.0.1 release. Generated local
test binaries are intentionally omitted.

### Core runtime

* wait/wake: close the early-completion lost-wakeup window for mutex, condvar,
  channel send/recv, channel select, and I/O waits by using explicit
  armed/completed wait-node states instead of relying on task state alone.

* channel-select: add an inline-vs-queued completion state machine so a select
  operation that completes before the waiter parks is consumed inline, while a
  completion that races after park is reinjected exactly once.

* deadlines: defensively disarm wait deadlines after early wake on mutex,
  condvar, channel, select, and I/O paths. This prevents stale timers from
  firing after a wait has already completed.

* shutdown: add a runtime-wide parked-waiter cancellation pass so cooperative
  stop can wake tasks parked on channels, mutexes, joins, sleeps, I/O, or
  blocking callbacks instead of waiting for unrelated producers.

* join: make public task handles single-owner for join/detach. Concurrent joins
  now fail with `EBUSY`; timed-out joins release the claim and leave the handle
  joinable.

* task-group: preserve child ownership across partial `join_until()` failure and
  reject concurrent spawn/join/destroy races with `EBUSY`.

* blocking-calls: clarify and enforce cancellation semantics. A queued callback
  can be cancelled before execution; a running callback reports cancellation
  only after the callback returns, preserving caller-owned argument lifetime.

* scheduler: bound direct-yield FIFO preference so two yielding tasks cannot
  starve fresh owner-deque work indefinitely.

* spawn: release a retained cancellation-token reference if stack allocation
  fails before the task is published.

* task-local: make an unset value for an active key return `NULL` with
  `errno == 0`, distinguishing it from invalid key or unmanaged-task failures.

* reclaim: protect listed parked tasks with scan references while shutdown
  diagnostics/cancellation scans run outside shard-list locks.

* portability: remove data races from cached page-size discovery, Darwin
  timebase initialization, and optional Darwin `ulock` symbol resolution.

### I/O runtime

* shutdown: make Darwin, Linux, and Windows I/O workers exit on
  `shutdown_requested` rather than ordinary stop requests, avoiding premature
  worker termination while pending operations remain owned by the runtime.

* Darwin: fix a receive-completion failure path that could free a copied buffer
  without clearing the pointer.

* I/O waits: disarm deadline timers after fast I/O completion wins the race with
  wait setup.

### Diagnostics and observability

* runtime-dump: expand `llam_dump_runtime_state()` output with lifecycle state,
  stop/shutdown state, active I/O waiter counts, node queue depths, shard wake
  and I/O ownership, and per-task wait ownership fields.

* runtime-dump: report task wait owners for I/O requests, select waits, wait
  nodes, blocking jobs, join targets, and timers.

* stats: split JSON statistics writing into a dedicated implementation file and
  add regression coverage for duplicated or missing fields.

* stress: add POSIX signal-driven runtime dumps for the `stress` executable via
  `LLAM_RUNTIME_DUMP_ON_SIGNAL`.

* tooling: add `scripts/run_with_timeout.py` to stream long-running logs, request
  a runtime dump on timeout, interrupt gracefully, and then hard-kill only after
  a grace period.

### Public API and documentation

* document the runtime handle API as the 1.x embedding boundary while keeping
  the implementation tied to one process-global runtime.

* document that non-default runtime handles fail with `EINVAL` and second live
  runtime creation fails with `EBUSY`.

* document task join/detach ownership, task-group `EBUSY` cases, and timeout
  ownership preservation.

* update operations guidance for runtime dumps, CI timeout artifacts, expected
  vs unexpected server edge errors, and server stop-time dump artifacts.

### Example server

* replace the chat outbox `pthread_mutex_t` with `llam_mutex_t` so the example
  does not block an OS worker in the common short critical sections.

* add explicit best-effort and lossless server modes to the flood harness. The
  `--server-best-effort` mode validates bounded-drop throughput; the
  `--server-lossless` mode validates producer backpressure.

* defer socket close until final client release and use enqueue references for
  broadcast snapshots, avoiding stale writes to a reused file descriptor.

* improve shutdown behavior by publishing close state atomically, closing wake
  channels, and avoiding new broadcasts during stop.

* report server-side delivery accounting including attempted deliveries,
  enqueued deliveries, outbox-full drops, outbox-closed drops, missing delivery
  counts, and accounting gaps.

* split edge-stress client errors into expected churn/cleanup failures and
  unexpected failures. `unexpected_client_errors` is the gating signal.

### Stress and test coverage

* add focused public API edge tests for lifecycle, task ownership,
  cancellation, channel close/drain, blocking callbacks, cond/mutex deadlines,
  and managed/unmanaged call boundaries.

* add focused channel-select race tests for send, close, and cancellation
  lost-wakeup coverage.

* add task-local and task-group ownership tests, including task-local
  isolation, group destroy-with-live-children, cancellation, and partial
  `join_until()` behavior.

* add unmanaged OS-thread join tests so the public boundary is covered without
  relying on scheduler progress from the joining thread.

* add live-I/O dump tests on POSIX and Windows IOCP to prove parked I/O
  ownership appears in diagnostic dumps.

* add seeded runtime invariant tests for channel close/drain, select timeout
  and close, cancellation wakeups across sleep/channel/condvar, task-group
  timeout recovery, and runtime statistics sanity.

* make dynamic-worker stress diagnostics more actionable by printing live-task,
  active-I/O, effective-live, and block-pending counts when downscale assertions
  miss.

### CI

* include the new focused tests in Make and CMake test targets.

* run seeded runtime invariant checks in the Stress workflow on Linux, macOS,
  and Windows.

* run deeper seeded invariant repeats in Nightly Deep CI and include the checks
  in ASan/UBSan and experimental TSan lanes.

* stream long-running stress output through `scripts/run_with_timeout.py` so CI
  timeout failures retain partial logs and runtime dumps.

* keep Windows 10/11 forced-policy stress covering the new invariant test in
  addition to runtime stress, fuzz, IOCP, and policy tests.

### Known behavior

* high-rate chat-server flood in best-effort mode can drop deliveries when a
  client outbox fills. This is expected policy, not a lossless guarantee; use
  lossless mode when the test requires backpressure and exact delivery.

* the runtime remains one active process runtime in the 1.x ABI. The handle API
  is the documented embedding boundary, not concurrent multi-runtime isolation.
