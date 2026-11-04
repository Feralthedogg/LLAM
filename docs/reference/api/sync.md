# Sync and channels

Coordinate tasks with mutexes, condition variables, channels, and select. These
primitives park managed tasks instead of blocking worker threads.

## Synchronization

| API | Purpose |
| --- | --- |
| `llam_mutex_create` / `llam_mutex_destroy` | Create and destroy runtime-aware non-recursive mutexes. |
| `llam_mutex_lock` | Lock and park the task when contended. |
| `llam_mutex_lock_until` | Timed lock with an absolute deadline. |
| `llam_mutex_trylock` | Non-parking lock attempt. |
| `llam_mutex_unlock` | Unlock and wake one waiter when present. |
| `llam_cond_create` / `llam_cond_destroy` | Create and destroy runtime-aware condition variables. |
| `llam_cond_wait` | Wait while atomically releasing and reacquiring a LLAM mutex. |
| `llam_cond_wait_until` | Timed condition wait. |
| `llam_cond_signal` | Wake one condition waiter. |
| `llam_cond_broadcast` | Wake all condition waiters. |

## Channels and select

| API | Purpose |
| --- | --- |
| `llam_channel_create` / `llam_channel_destroy` | Create and destroy bounded pointer channels. |
| `llam_channel_send` | Send and park when the channel is full. |
| `llam_channel_try_send` | Non-parking send attempt. |
| `llam_channel_send_until` | Timed send. |
| `llam_channel_recv_result` | Receive with explicit out parameter. Use when `NULL` is a valid payload. |
| `llam_channel_try_recv_result` | Non-parking receive attempt with explicit out parameter. |
| `llam_channel_recv_until_result` | Timed receive with explicit out parameter. |
| `llam_channel_recv` | Convenience receive wrapper. |
| `llam_channel_recv_until` | Convenience timed receive wrapper. |
| `llam_channel_close` | Close the channel and wake blocked senders/receivers. |
| `llam_channel_select` | Select one ready send/receive operation. |
| `LLAM_CHANNEL_SELECT_MAX_OPS` | Maximum operation count accepted by `llam_channel_select`. |
