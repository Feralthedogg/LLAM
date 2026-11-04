# Threading and deadlines

Check where each call may run and how LLAM reads deadline values.

## Threading and ownership

- `llam_runtime_init_ex()`, `llam_runtime_init()`, their `*_compat` forms, and
  `llam_runtime_shutdown()` are not reentrant.
- `llam_runtime_create()`, `llam_runtime_create_managed()`,
  `llam_runtime_init_ex()`, and `llam_runtime_init()` are host calls: a managed
  task gets `EBUSY`. The `*_compat` constructors have no such check.
- `llam_runtime_create()` or `llam_runtime_create_compat()`,
  `llam_runtime_spawn_ex()`, `llam_runtime_run_handle()`, and
  `llam_runtime_destroy()` are the canonical embedding-facing lifecycle calls. They
  operate on independent runtime handles;
  legacy host-thread lifecycle wrappers operate on `llam_runtime_default()`.
  Managed task calls to legacy spawn/stop/shutdown wrappers target the current
  task's owner runtime so explicit-runtime tasks do not accidentally poke the
  default runtime.
- Calling `llam_runtime_shutdown()` from a managed LLAM task or scheduler frame
  does not tear down the owning runtime underneath the active scheduler. It is
  treated as a cooperative stop request; the host thread that owns the run call
  remains responsible for final shutdown.
- Calling `llam_runtime_destroy(runtime)` from a managed task only affects that
  task's owner runtime. A foreign runtime handle is ignored so one scheduler
  cannot accidentally become a stop signal for a peer scheduler.
- A runtime handle must not be destroyed while another host thread is
  concurrently creating tasks or runtime-owned synchronization objects for that
  same handle.
- User pointers passed to tasks, channels, blocking callbacks, or I/O buffers
  remain owned by the caller unless a specific API states otherwise.
- Task, channel, mutex, condition, cancellation, and buffer handles must not be
  copied by value or stack-allocated by callers.
- A cancellation token must outlive all tasks, waits, and I/O operations that
  observe it. Internally retained observers keep a refcount; destroy fails with
  `EBUSY` until those observers are gone.
- Mutex, condition, and channel destroy functions require external
  synchronization: no task may own, wait on, or concurrently access the object
  being destroyed.
- Host-side destroy/release calls for idle public objects may be performed after
  the owner runtime has been destroyed.  Managed tasks still cannot use or
  destroy objects owned by a different live runtime; those calls fail with
  `EXDEV`.
- LLAM may move managed tasks between workers unless the caller supplies a
  documented placement hint. Hints are not ABI guarantees.
- Language runtimes should treat all undocumented exported symbols as private
  implementation details.

## Deadlines and cancellation

- Deadline values are absolute monotonic timestamps in `llam_now_ns()` units.
- All public `*_until(deadline_ns)` APIs treat `deadline_ns == 0` as an
  already-expired absolute deadline, not "disabled". They never wait.
- Expired deadline waits fail with `errno = ETIMEDOUT`, with two exceptions:
  `llam_sleep_until()` returns `0`, because reaching the deadline is its success,
  and `llam_poll_fd_until()` returns `0`.
- Deadline `0` does not stop an operation that needs no wait. An uncontended
  `llam_mutex_lock_until()`, a channel send with buffer space, a channel receive
  with a buffered value, `llam_join_until()` on a completed task, and
  `llam_task_group_join_until()` on a drained group all succeed.
- Stream I/O and timers check the deadline first. `llam_read_until()`,
  `llam_write_until()`, `llam_connect_until()`, and `llam_timer_wait_until()` with
  deadline `0` fail with `ETIMEDOUT` even when the operation is ready, and
  `llam_poll_fd_until()` returns `0` even when the descriptor is ready.
- Cancellation-aware operations fail with `errno = ECANCELED` where the public
  header documents cancellation token participation.
- Immediate fast paths are allowed to complete before observing cancellation if
  the operation had already become ready.
- LLAM preserves syscall-style `errno` values for I/O operations and does not
  translate them into backend-specific domains.
