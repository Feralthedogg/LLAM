# Examples

Use these examples when you want to copy a small shape and adapt it. Each page
focuses on one decision instead of listing every API.

Examples are labeled by intent:

| Label | Meaning |
| --- | --- |
| Complete program | You can copy the whole block into a file and build around it. |
| Focused snippet | The block belongs inside an existing task, callback, or platform setup. |
| Platform-specific | The example depends on a named OS API such as POSIX `socketpair()` or Windows HANDLEs. |

<div class="llam-card-grid llam-card-grid-wide">
  <a class="llam-card" href="tasks-and-timers/">
    <h3>Tasks and timers</h3>
    <p>Spawn work, join it, sleep, and use interval timers.</p>
  </a>
  <a class="llam-card" href="channels-and-select/">
    <h3>Channels and select</h3>
    <p>Move values between tasks and wait on multiple channel operations.</p>
  </a>
  <a class="llam-card" href="runtime-io/">
    <h3>Runtime I/O</h3>
    <p>Read and write without pinning scheduler workers.</p>
  </a>
  <a class="llam-card" href="embedding/">
    <h3>Embedding</h3>
    <p>Create explicit runtime handles for host-owned lifecycle.</p>
  </a>
  <a class="llam-card" href="blocking-work/">
    <h3>Blocking work</h3>
    <p>Run foreign blocking code through LLAM compensation paths.</p>
  </a>
</div>

## Complete Programs In The Repository

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

For production validation, use the commands in [Verification](../operations/verification.md).
