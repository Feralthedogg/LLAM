# Troubleshooting

Start with the smallest focused reproducer. Runtime-owned failures should move
into direct runtime tests instead of depending on the example chat server.

## Hung Or Slow Shutdown

Capture a runtime dump:

```c
llam_dump_runtime_state(fd);
```

Inspect task fields:

- `wait_owner=`
- `cancel_registered=`
- `deadline_ns=`
- `io_req=`
- `block_job=`

These identify whether the wait is owned by cancellation, timeout, I/O,
blocking work, or a lost wake handoff.

## Descriptor Reuse

Use `llam_close()` or `llam_close_handle()` for descriptors used with LLAM I/O.
If raw `close()` is used, the runtime has less diagnostic context for reuse
races.

Run fd-pressure tests when changing watch or close paths.

## CPU-Bound Tasks

Long C loops must reach LLAM boundaries. Add:

```c
LLAM_PREEMPT_POLL_EVERY(counter++, 1024U);
```

Use `LLAM_PREEMPT_MODE=strict` while hunting missing safepoints.

## Windows I/O Differences

Windows fd/socket positional APIs return `ENOTSUP`. Use HANDLE variants for
file I/O:

```c
llam_pread_handle(handle, buf, len, offset);
```

TCP `POLLIN` on Windows remains conservative by default. Enable
`LLAM_WINDOWS_IOCP_TCP_POLLIN=1` only for controlled smoke or benchmark runs.

## Sanitizers

ASan/UBSan are hard gates. Linux TSan is diagnostic because stackful fiber
runtimes can interact differently with sanitizer runtimes across hosted
environments. Treat TSan output as triage input, not an automatic release
blocker unless reproduced by a focused test.
