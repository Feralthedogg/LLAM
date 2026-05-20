# LLAM ChangeLog

## 1.1.0 - 2026-05-20

This entry summarizes the source, test, CI, packaging, and documentation changes
staged after the 1.0.1 release. Generated local test binaries and analyzer
artifacts are intentionally omitted.

### Core runtime

* lifecycle: serialize runtime construction and shutdown through the lifecycle
  lock so concurrent init/create/shutdown calls cannot observe or tear down a
  partially constructed singleton.

* lifecycle: treat `llam_runtime_shutdown()` from a managed task or scheduler
  frame as a cooperative stop request. Final teardown remains the host thread's
  responsibility after `llam_run()` returns.

* wait/wake: close the early-completion lost-wakeup window for mutex, condvar,
  channel send/recv, channel select, and I/O waits by using explicit
  armed/completed wait-node states instead of relying on task state alone.

* channel-select: add an inline-vs-queued completion state machine so a select
  operation that completes before the waiter parks is consumed inline, while a
  completion that races after park is reinjected exactly once.

* channels: collapse blocking send and timed send through one internal
  implementation, removing duplicated waiter/wakeup logic from the send paths.

* channels: make nonblocking try-send/try-receive use `EAGAIN` for would-block
  cases. Timed APIs continue to use `ETIMEDOUT` for expired deadlines.

* deadlines: defensively disarm wait deadlines after early wake on mutex,
  condvar, channel, select, and I/O paths. This prevents stale timers from
  firing after a wait has already completed.

* shutdown: add a runtime-wide parked-waiter cancellation pass so cooperative
  stop can wake tasks parked on channels, mutexes, joins, sleeps, I/O, or
  blocking callbacks instead of waiting for unrelated producers.

* cancellation: add a live-token registry and active-operation accounting so
  `cancel`, `is_cancelled`, retain, and destroy races return `EINVAL` or
  `EBUSY` instead of dereferencing reclaimed token storage.

* cancellation: mark detached waiter chains unregistered while holding the token
  lock and hold short scan references while cancellation walks the chain.

* join: make public task handles single-owner for join/detach. Concurrent joins
  now fail with `EBUSY`; timed-out joins release the claim and leave the handle
  joinable.

* task-group: add a live-handle registry, preserve child ownership across
  partial `join_until()` failure, and reject concurrent spawn/cancel/join/destroy
  races with `EBUSY` or stale-handle `EINVAL`.

* task-group: keep explicit caller-owned cancellation tokens outside group
  cancellation; only children using the group-provided token are cancelled by
  `llam_task_group_cancel()`.

* blocking-calls: clarify and enforce cancellation semantics. A queued callback
  can be cancelled before execution; a running callback reports cancellation
  only after the callback returns, preserving caller-owned argument lifetime.

* scheduler: bound direct-yield FIFO preference so two yielding tasks cannot
  starve fresh owner-deque work indefinitely.

* preemption: add request-based automatic preemption policy for CPU-heavy tasks.
  The runtime can request preemption from watchdog/policy state, while the
  actual context switch remains bounded to safepoints, yields, waits, and I/O
  calls.

* preemption: add public hot-loop helpers `LLAM_PREEMPT_POLL()` and
  `LLAM_PREEMPT_POLL_EVERY(counter, interval)`. The counted helper evaluates
  macro arguments once and treats `0`/`1` intervals as poll-every-call.

* spawn: release a retained cancellation-token reference if stack allocation
  fails before the task is published.

* task-local: make an unset value for an active key return `NULL` with
  `errno == 0`, distinguishing it from invalid key or unmanaged-task failures.

* task-local: prevent deleted key IDs from being reused while live tasks still
  hold entries for the old key.

* reclaim: protect listed parked tasks with scan references while shutdown
  diagnostics/cancellation scans run outside shard-list locks.

* portability: remove data races from cached page-size discovery, Darwin
  timebase initialization, and optional Darwin `ulock` symbol resolution.

### I/O runtime

* shutdown: make Darwin, Linux, and Windows I/O workers exit on
  `shutdown_requested` rather than ordinary stop requests, avoiding premature
  worker termination while pending operations remain owned by the runtime.

* I/O ownership: when worker threads detach submit queues, mark requests
  in-flight before releasing the submit lock so timeout/cancellation paths issue
  backend cancellation instead of silently missing ownership transfer.

* I/O waits: disarm deadline timers after fast I/O completion wins the race with
  wait setup.

* I/O waits: make Darwin and Linux poll/accept/recv watcher removal helpers
  tolerate `NULL` watches after a racing completion has already detached the
  request. Windows unsupported-watch stubs now document the same no-op contract.

* I/O fallback: blocking-worker read, write, accept, connect, poll, and handle
  poll callbacks now observe both task cancellation and runtime stop, preventing
  running fallback jobs from keeping shutdown alive indefinitely.

* I/O fallback: restore temporary descriptor flags without clobbering the
  operation's `errno`.

* Windows IOCP: wire the Windows 11 skip-completion policy into associated
  socket/HANDLE handles. Immediate overlapped successes on handles where
  `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` is accepted are completed inline through
  the same finalizer used by queued IOCP packets.

* Windows IOCP: expose skip-completion handle counts, failures, and inline
  completions in runtime dumps so direct-success hit rate can be diagnosed
  without a profiler.

* owned buffers: zero-byte `llam_read_owned()` and `llam_recv_owned()` now
  complete as no-op success with `out == NULL`, matching the public API
  contract and avoiding descriptor inspection.

* owned buffers: allocate public owned-buffer wrappers from detached storage so
  callers may release a successful buffer after `llam_runtime_shutdown()`.

* owned buffers: preserve `EBADF` for invalid-descriptor `recv_owned()` rather
  than normalizing all failed socket probes to `ENOTSOCK`.

* poll: normalize the public invalid-fd sentinel to `POLLNVAL` for single-fd
  `llam_poll_fd()` and `llam_platform_poll_now()` instead of inheriting native
  poll-array "ignore negative fd" behavior.

* poll: translate Darwin kqueue registration `EBADF`/`EINVAL` into `POLLNVAL`
  revents and return one ready descriptor, matching poll-compatible semantics.

* writev: validate aggregate fallback byte counts before writing any slice, so
  invalid tail slices cannot be hidden behind partial success. Unmanaged and
  direct native writev paths still preserve platform behavior for descriptors
  such as `/dev/null`.

* Darwin/Linux: release copied receive-ready payloads when no caller accepts
  ownership from a ready node.

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

* document managed-task shutdown behavior, task-group cancellation ownership,
  token destroy/cancel race results, channel try-send/try-recv host-thread
  boundaries, and owned-buffer lifetime after shutdown.

* document the runtime handle API as the 1.x embedding boundary while keeping
  the implementation tied to one process-global runtime.

* document that non-default runtime handles fail with `EINVAL` and second live
  runtime creation fails with `EBUSY`.

* document task join/detach ownership, task-group `EBUSY` cases, and timeout
  ownership preservation.

* update operations guidance for runtime dumps, CI timeout artifacts, expected
  vs unexpected server edge errors, and server stop-time dump artifacts.

* document automatic preemption modes, safepoint polling macros, and the
  environment knobs that control preemption quantum and cheap-safepoint clock
  sampling.

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

* add and wire `test_runtime_shutdown_internal` for shutdown internals and
  partial-init cleanup coverage.

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

* add direct I/O edge tests for zero-size owned reads, invalid-fd poll,
  invalid-descriptor errno preservation, POSIX handle aliasing, writev aggregate
  validation, and owned-buffer release after runtime shutdown.

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

* add Makefile object/link build signatures so sanitizer or custom-`OBJDIR`
  builds cannot silently contaminate normal executable targets.

* make Windows-host Makefile targets delegate to the native CMake backend instead
  of failing with `windows-unsupported`, and route `make verify-windows` through
  `scripts/verify_windows.ps1 -Native`.

* make the Windows extension-function cache type-safe for `AcceptEx` and
  `ConnectEx`, which lets MinGW `-Wpedantic -Werror` compile the Windows IOCP
  files cleanly.

* add package smoke checks that compile bundled `demo` and `bench` examples from
  installed release archives and verify required example support files are
  present.

* run seeded runtime invariant checks in the Stress workflow on Linux, macOS,
  and Windows.

* repeat `test_runtime_api_edges` in Stress, Nightly, and Soak jobs so fault
  injection and completion-order races are exercised more than once per run.

* run deeper seeded invariant repeats in Nightly Deep CI and include the checks
  in ASan/UBSan and experimental TSan lanes.

* stream long-running stress output through `scripts/run_with_timeout.py` so CI
  timeout failures retain partial logs and runtime dumps.

* keep Windows 10/11 forced-policy stress covering the new invariant test in
  addition to runtime stress, fuzz, IOCP, and policy tests.

* repeat native Windows IOCP I/O tests under forced Windows 10 and Windows 11
  policy branches in Stress and Nightly workflows.

### Known behavior

* high-rate chat-server flood in best-effort mode can drop deliveries when a
  client outbox fills. This is expected policy, not a lossless guarantee; use
  lossless mode when the test requires backpressure and exact delivery.

* the runtime remains one active process runtime in the 1.x ABI. The handle API
  is the documented embedding boundary, not concurrent multi-runtime isolation.
