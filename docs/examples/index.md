---
hide:
  - toc
---

# Examples

Use these examples when you want to copy a small shape and adapt it. Each page
focuses on one decision instead of listing every API.

<div class="cards" markdown>

- [Tasks and timers](tasks-and-timers.md)

    Spawn work, join it, sleep, and use interval timers.

- [Channels and select](channels-and-select.md)

    Move values between tasks and wait on multiple channel operations.

- [Runtime I/O](runtime-io.md)

    Read and write without pinning scheduler workers.

- [Embedding](embedding.md)

    Create explicit runtime handles for host-owned lifecycle.

- [Blocking work](blocking-work.md)

    Run foreign blocking code through LLAM compensation paths.

</div>

## How examples are labeled

Examples are labeled by intent:

| Label | Meaning |
| --- | --- |
| Complete program | You can copy the whole block into a file and build around it. |
| Focused snippet | The block belongs inside an existing task, callback, or platform setup. |
| Platform-specific | The example depends on a named OS API such as POSIX `socketpair()` or Windows HANDLEs. |

## Complete programs in the repository

The source tree also ships larger examples:

| Program | Purpose |
| --- | --- |
| `demo` | Small public runtime API walkthrough. |
| `bench` | Runtime microbenchmarks. |
| `stress` | Scheduler, sync, timeout, I/O, and dynamic-worker stress. |
| `server` | Best-effort LLAM-backed TCP chat server. |
| `server_lossless` | Chat server with outbox backpressure. |
| `server_flood` | Native throughput driver for the chat server. |
| `llam_broker` | Internal broker self-test and local transport smoke harness. |

Build them from source with:

```sh
make -j4
./demo
./bench
./server 7777
```

For production validation, use the commands in [Verification](../production/verification.md).

Native C snippets in this section explicitly select `*_compat` runtimes.
The normal runtime constructors require asynchronous admission; use the
[managed C guide](../guides/managed-c.md) to compile qualified task code for that path.
