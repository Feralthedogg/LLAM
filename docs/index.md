---
template: home.html
---

# LLAM

LLAM, the Low Level Asynchronous Machine, lets you write blocking C and run it
concurrently. It gives every task its own stack and parks waits on native I/O backends, so straightforward C code can scale across
Linux, macOS, and Windows.

## Install

=== "Linux, macOS"

    ```sh
    curl -fsSL https://github.com/Feralthedogg/LLAM/releases/download/v3.0.0/install.sh |
      sh -s -- --version 3.0.0 --prefix "$HOME/.local"
    ```

=== "Windows"

    ```powershell
    Invoke-WebRequest "https://github.com/Feralthedogg/LLAM/releases/download/v3.0.0/install.ps1" -OutFile install.ps1
    .\install.ps1 -Version 3.0.0 -Prefix "$env:LOCALAPPDATA\LLAM"
    ```

These docs target 3.0.0. A [source build](start/build.md) is also available.
See [SDK version matching](start/quickstart.md#match-the-sdk-version).

## Choose what you need

<div class="cards" markdown>

- [Start a new app](start/quickstart.md)

    Install LLAM, link through CMake, and run the smallest working program.

- [Embed in a host](guides/embedding.md)

    Create explicit runtimes and keep host-owned lifecycle boundaries clear.

- [Bind from another language](reference/abi/index.md)

    Use size-aware structs, fixed-width fields, and ABI metadata safely.

- [Run in production](production/checklist.md)

    File descriptor limits, timeouts, CPU-bound tasks, and observability.

- [Tour the examples](examples/index.md)

    Pick a focused task, channel, I/O, embedding, or blocking-work snippet.

</div>

## Supported platforms

| Platform | Status | Backend |
| --- | --- | --- |
| Linux x86_64, aarch64 | Primary | io_uring/liburing |
| macOS arm64, x86_64 | Supported | kqueue |
| Windows 10, 11 x86_64 | Supported | IOCP |

See [Platform support](start/platforms.md) for verification commands and backend
notes.
