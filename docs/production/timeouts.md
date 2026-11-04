# Timeouts and FD reuse

Timeouts, cancellation, worker rehome, and descriptor reuse race with each other.
Test them together, under descriptor pressure.

## Timeouts, cancellation, and rehome races

Timeout and cancellation paths should always be tested together with dynamic
workers. The stress suite covers join, mutex, condition variable, channel, poll,
I/O cancellation, live accept watches, live poll watches, inflight I/O, and
worker rehome.

Recommended command:

```sh
LLAM_STRESS_ROUNDS=64 \
LLAM_STRESS_DYNAMIC_ROUNDS=8 \
LLAM_STRESS_DYNAMIC_SLEEP_TASKS=4096 \
LLAM_STRESS_DYNAMIC_SLEEP_YIELDS=32 \
LLAM_STRESS_DYNAMIC_LIVE_POLL_WAITERS=1024 \
LLAM_STRESS_PHASE_WATCHDOG_SEC=90 \
./stress
```

If a platform intentionally lacks one backend feature, the phase must print a
clear `skipped=` reason rather than silently reducing coverage.

## File descriptor reuse

LLAM I/O watch tables must not wake a new file/socket that reused the same
numeric fd as an old watch. Linux watch lookup validates `(dev, ino)` identity
where the kernel exposes it. Darwin uses kqueue state ownership and explicit
watch cleanup. Regression coverage should include timeout/close/reopen paths and
must be run under descriptor pressure.

User code should still treat descriptor ownership conventionally: close an fd
only after all LLAM tasks that may poll, read, write, accept, or connect on it
have stopped or have observed cancellation.
