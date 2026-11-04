# Troubleshooting

Start with the smallest focused reproducer. Runtime-owned failures should move
into direct runtime tests instead of depending on the example chat server.

## Spawn or create fails

`llam_runtime_create()` and `llam_runtime_init()` run only task code from the
[managed C toolchain](../guides/managed-c.md). Spawning a plain C task on them fails
with `ENOTSUP`. Build the task code with `llam-clang`, or create the runtime with
`llam_runtime_create_compat()` or `llam_runtime_init_compat()`.

The same two constructors fail with `ENOTSUP` themselves when the build or host has
no qualified native profile; see [Platform support](../start/platforms.md). They fail
with `EINVAL` when `preempt_mode` or `LLAM_PREEMPT_MODE` selects `off` or
`cooperative`.

## Hung or slow shutdown

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

## Descriptor reuse

Use `llam_close()` or `llam_close_handle()` for descriptors used with LLAM I/O.
If raw `close()` is used, the runtime has less diagnostic context for reuse
races.

Run fd-pressure tests when changing watch or close paths.

## CPU-bound tasks

On a compatibility runtime, long C loops must reach LLAM boundaries. Add:

```c
LLAM_PREEMPT_POLL_EVERY(counter++, 1024U);
```

Use `LLAM_PREEMPT_MODE=strict` while hunting missing safepoints.

## Windows I/O differences

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
