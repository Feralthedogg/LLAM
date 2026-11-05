---
hide:
  - toc
---

# LLAM docs

Docs for LLAM 3.0.0, public ABI 2.4.

<div class="cards big" markdown>

- [Get started with LLAM](start/quickstart.md)

    Install LLAM, link it from CMake, and run one task.

- [Copy an example](examples/index.md)

    Pick a focused task, channel, I/O, embedding, or blocking-work snippet.

</div>

## Learn the model

<div class="cards" markdown>

- [Execution model](concepts/execution-model.md)

    When LLAM parks, resumes, migrates, or wakes a task.

- [Runtime handles](concepts/runtime-handles.md)

    Owner checks, cross-runtime use, and handle lifetime rules.

- [I/O model](concepts/io-model.md)

    Blocking-looking calls while tasks park on io_uring, kqueue, or IOCP.

</div>

## Find what you need

<div class="cards" markdown>

- [Embed LLAM](guides/embedding.md)

    Create explicit runtimes and keep host-owned lifecycle boundaries clear.

- [Bind from another runtime](reference/abi/index.md)

    Use size-aware structs, fixed-width fields, and ABI metadata safely.

- [Debug a hang](guides/diagnostics.md)

    Capture stats, runtime dumps, wait ownership, and timeout artifacts.

- [Run in production](production/checklist.md)

    File descriptor limits, timeouts, CPU-bound tasks, and observability.

- [Verify a release](production/verification.md)

    Run platform-local tests, stress, soak, sanitizer, and package gates.

- [Check the boundary](reference/security/index.md)

    Separate in-process handle hardening from broker-mode isolation.

- [Follow the roadmap](project/roadmap.md)

    See the path from 3.0.0 to a general-purpose asynchronous execution backend.

</div>
