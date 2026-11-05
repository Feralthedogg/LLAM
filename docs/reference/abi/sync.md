# Synchronization semantics

The exact behavior of mutexes, condition variables, channels, and cancellation
tokens.

LLAM mutexes, condition variables, channels, and cancellation tokens are
runtime-aware. When called from a managed task, blocking waits park the task
instead of blocking the scheduler worker.

## Outside the runtime

Outside-runtime behavior is fixed as follows:

- `llam_yield()` outside a managed task is a no-op.
- `llam_task_safepoint()` outside a managed task is a no-op.
- `llam_sleep_ns()` and `llam_sleep_until()` block the calling OS thread after
  runtime initialization; before initialization they fail with `EINVAL`.
- `llam_call_blocking_result()` runs the callback synchronously outside a
  managed task.
- `llam_enter_blocking()` and `llam_leave_blocking()` outside a managed task
  are no-ops and return `0`.
- Runtime mutex, condition, and channel blocking operations require a managed
  task context and fail with `ENOTSUP` outside one. So do
  `llam_mutex_trylock()`, `llam_mutex_unlock()`, and `llam_channel_select()`,
  even with a deadline of `0`, although those calls do not park.
- `llam_timer_wait()` and `llam_timer_wait_until()` block the calling OS thread
  outside a managed task.
- Nonblocking channel operations (`llam_channel_try_send()` and
  `llam_channel_try_recv_result()`) may be called from unmanaged OS threads
  after runtime initialization because they never park. This is the supported
  host-thread cleanup path for draining buffered channel values before destroy.
  `llam_channel_try_recv_result()` can also drain already-buffered values after
  runtime shutdown; it never wakes parked senders without a live runtime. While
  the runtime is live, a nonblocking operation that would park fails with
  `errno = EAGAIN`, not `ETIMEDOUT`.
- Runtime I/O helpers document their own direct syscall fallback in
  [I/O semantics](io.md).

Language runtimes should perform blocking coordination from LLAM tasks whenever
possible.

## After the owner runtime is gone

A host thread can still touch an object after its owner runtime has stopped
existing. The result depends on which kind of runtime owned it: the default runtime
after `llam_runtime_shutdown()`, or an explicit runtime after
`llam_runtime_destroy()`.

| Operation | Default runtime | Explicit runtime |
| --- | --- | --- |
| `llam_channel_try_recv_result()`, value buffered | Returns the value | Returns the value |
| `llam_channel_try_recv_result()`, buffer empty | `ENOTSUP`, or `EPIPE` if closed | `ENOTSUP`, or `EPIPE` if closed |
| `llam_channel_try_send()` | `ENOTSUP` | `EXDEV` |
| `llam_cond_signal()`, `llam_cond_broadcast()` | `ENOTSUP` | `EXDEV` |
| `llam_cancel_token_cancel()`, `llam_cancel_token_is_cancelled()` | Succeeds | `EXDEV` |
| `llam_task_group_cancel()`, `llam_task_group_join()` on an idle group | Succeeds | `EXDEV` |
| `llam_timer_wait()`, `llam_timer_reset()`, `llam_timer_cancel()` | `EINVAL` | `EXDEV` |
| `llam_channel_close()` | Succeeds | Succeeds |
| Destroying an idle mutex, condition, channel, token, timer, or group | Succeeds | Succeeds |

## Mutexes

LLAM mutexes are non-recursive. `llam_mutex_lock()` and
`llam_mutex_lock_until()` return `-1/EDEADLK` when the current managed task
already owns the mutex. Mutex try-lock returns `-1/EBUSY` when the mutex is
already locked. Mutex unlock returns `-1/EPERM` when the current managed task
does not own the mutex.
Managed cross-runtime mutex use or destroy attempts return `-1/EXDEV`; active
owners, waiters, or public-operation pins make destroy return `-1/EBUSY`.

## Condition variables

Condition waits follow condition-variable style rules. The caller must own the
associated mutex on entry. LLAM atomically releases it while waiting and
reacquires it before returning, including signal, broadcast, timeout,
cancellation, runtime stop, and spurious wake paths. Callers must wait in a
predicate loop because wakeups are not a predicate guarantee. Invalid-argument or
wrong-owner failures return before releasing the mutex.

The reacquire has no deadline and cannot be cancelled. After `ETIMEDOUT` or
`ECANCELED`, the call still waits for the mutex's current owner to unlock it, so it
can return later than its deadline, and a stopping runtime cannot drain until that
owner unlocks. A task that exits while holding the mutex leaves the waiter parked
forever, so the run call does not return; `llam_runtime_drain_until()` still gives the
host a bounded wait, and a [runtime dump](../../guides/diagnostics.md#runtime-dumps)
shows the parked task.

`llam_cond_signal()` and `llam_cond_broadcast()` may be called with or without
the associated mutex held, and may be called outside a managed LLAM task while
the owner runtime is live. Invalid condition handles fail with `-1/EINVAL`,
active teardown/corruption sentinels fail with `-1/EBUSY`, cross-runtime managed
use fails with `-1/EXDEV`, and unmanaged calls after the owner runtime can no
longer service waiter wakeups fail with `-1/ENOTSUP` for the default runtime or
`-1/EXDEV` for a destroyed explicit runtime.
Condition destroy follows the same owner contract: managed cross-runtime
destroy returns `-1/EXDEV`, and live waiters, selected-but-not-returned waiters,
or active public-operation pins return `-1/EBUSY`.

## Channels

Channel receive APIs come in two forms:

```c
void *llam_channel_recv(llam_channel_t *channel);
int llam_channel_recv_result(llam_channel_t *channel, void **out);
```

The pointer-return convenience APIs are compact but ambiguous when `NULL` is a
valid payload. FFI bindings and code that sends `NULL` should use
`llam_channel_recv_result()` or `llam_channel_recv_until_result()`, where `0`
means a value was received and `*out` may legitimately be `NULL`.

Channel close rules are fixed:

- `llam_channel_close()` is idempotent.
- Sends after close fail with `EPIPE`.
- Buffered values sent before close remain drainable.
- Receives fail with `EPIPE` only after the buffer is empty and no sender can
  satisfy the receive.
- `NULL` payloads are valid; use `_result` receive APIs when they matter.
- Timed channel operations treat deadline `0` as an already-expired absolute
  deadline. They never wait, but a send with buffer space or a receive with a
  buffered value still succeeds.
- `llam_channel_select()` with deadline `0` makes one nonblocking scan and fails
  with `ETIMEDOUT` when no operation is ready.
