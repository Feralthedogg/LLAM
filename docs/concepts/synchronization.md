# Synchronization

LLAM synchronization primitives park managed tasks instead of blocking OS
worker threads. They are runtime-aware and owner-checked.

## Mutexes And Conditions

`llam_mutex_t` is non-recursive:

- self-lock fails with `EDEADLK`
- non-owner unlock fails with `EPERM`
- cross-runtime managed use fails with `EXDEV`
- destroy fails with `EBUSY` while owned, waited on, or actively used

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

## Select

`llam_channel_select()` waits on multiple channel operations and reports the
selected operation index. Keep operation arrays initialized before each call and
inspect each operation's `result_errno` after selection.

Select correctness is more important than micro-optimization. Regressions
should be reduced into direct runtime tests before relying on example-server
behavior.

## Cancellation Tokens

Cancellation tokens are explicit handles. Waiting tasks and I/O operations
observe cancellation through `ECANCELED`. Destroy fails with `EBUSY` while live
waiters or observers still hold references.
