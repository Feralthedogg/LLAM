# Synchronization

LLAM synchronization primitives park managed tasks instead of blocking OS
worker threads. They are runtime-aware and owner-checked.

## Mutexes and conditions

`llam_mutex_t` is non-recursive:

- self-lock fails with `EDEADLK`
- non-owner unlock fails with `EPERM`
- cross-runtime managed use fails with `EXDEV`
- destroy fails with `EBUSY` while owned, waited on, or actively used
- lock, try-lock, and unlock fail with `ENOTSUP` outside a managed task

Condition waits must use the usual predicate loop:

```c
while (!ready) {
    if (llam_cond_wait(cond, mutex) != 0) {
        break;
    }
}
```

Signal and broadcast may be called from managed or unmanaged host threads while
the owner runtime is live.

A mutex is an atomic owner fast path plus a `llam_wait_queue_t` for contention. A
condition variable is a FIFO waiter queue. Condition destroy returns `EBUSY` for
queued waiters, for signal- or broadcast-selected waiters that have not returned,
and for in-flight public operations.

## Channels

Channels transfer pointer values through a bounded buffer. Capacity must be at
least `1`.

```c
llam_channel_t *ch = llam_channel_create(16);
llam_channel_send(ch, item);
```

Use result-style receive APIs when `NULL` is a valid payload:

```c
void *value = NULL;
int rc = llam_channel_recv_result(ch, &value);
```

Closing a channel is idempotent. Sends fail with `EPIPE`, while buffered values
remain drainable.

Create a channel for a chosen runtime with `llam_runtime_channel_create()`. A channel
is a bounded pointer-valued ring buffer with separate send and receive wait queues.
Destroy returns `EBUSY` for buffered values, parked or close-woken waiters, and
in-flight public operations.

Nonblocking try-send and try-receive are also valid from host threads after runtime
initialization, and try-receive can drain buffered values after shutdown, so
embedders can clean up without entering a managed task.

## Select

`llam_channel_select()` waits on multiple channel operations and reports the
selected operation index. Keep operation arrays initialized before each call and
inspect each operation's `result_errno` after selection.

Select correctness is more important than micro-optimization. Regressions
should be reduced into direct runtime tests before relying on example-server
behavior.

## Cancellation tokens

Cancellation tokens are explicit handles. Waiting tasks and I/O operations
observe cancellation through `ECANCELED`. Destroy fails with `EBUSY` while live
waiters or observers still hold references.
