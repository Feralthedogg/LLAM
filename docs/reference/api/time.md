# Time and cancellation

Read the runtime clock, sleep until a deadline, wait on timers and signals, and
cancel waits.

## Time, timers, and signals

| API | Purpose |
| --- | --- |
| `llam_now_ns` | Monotonic nanosecond clock used by deadlines. |
| `llam_sleep_ns` | Relative sleep for the current managed task. |
| `llam_sleep_until` | Sleep until an absolute deadline. |
| `llam_timer_create_ex` | Create a waitable interval timer from `llam_timer_opts_t`. |
| `llam_timer_create` | Create a timer with only an interval. |
| `llam_timer_wait` | Wait for timer ticks. |
| `llam_timer_wait_until` | Wait for timer ticks until a deadline. |
| `llam_timer_reset` | Reset first deadline and interval. |
| `llam_timer_cancel` | Cancel pending timer waits. |
| `llam_timer_destroy` | Destroy a timer. |
| `LLAM_TIMER_OPTS_CURRENT_SIZE` | Size token for `llam_timer_create_ex`. |
| `llam_signal_set_create_ex` | Create a signal wait set. Linux-only; other platforms return `ENOTSUP`. |
| `llam_signal_wait` | Wait for a signal event. |
| `llam_signal_wait_until` | Wait for a signal event until a deadline. |
| `llam_signal_set_destroy` | Destroy a signal wait set. |
| `LLAM_SIGNAL_OPTS_CURRENT_SIZE` | Size token for `llam_signal_set_create_ex`. |

## Cancellation

| API | Purpose |
| --- | --- |
| `llam_cancel_token_create` | Create a cancellation token. |
| `llam_cancel_token_cancel` | Request cancellation for current and future observers. |
| `llam_cancel_token_is_cancelled` | Query cancellation state. |
| `llam_cancel_token_destroy` | Destroy a token when it has no live waiters or observers. |
