# Architecture

This page summarizes the implementation boundaries. The public contract remains
the headers under `include/llam/`.

```mermaid
flowchart TB
    App["C application"] --> API["Public C ABI\ninclude/llam"]
    API --> Core["src/core\nlifecycle, tasks, sync, timers"]
    API --> IO["src/io\nread, write, accept, connect, poll"]
    API --> Platform["include/llam/platform.h\nfd, HANDLE, platform macros"]

    Core --> Scheduler["scheduler shards\nrun queues, timer heaps, wake paths"]
    Core --> Handles["public handle tables\nfamily tags, sealed generations, owner tags"]
    Core --> Broker["broker foundation\ncapability tokens, grants, revocation"]
    Core --> Debug["stats JSON and runtime dumps"]

    Scheduler --> Engine["src/engine\nworkers, watchdog, rehome, scale"]
    Scheduler --> Context["src/asm and context files\nplatform context switch"]
    Scheduler --> Memory["runtime caches\nslabs, stacks, wait nodes, I/O reqs"]

    IO --> Linux["Linux io_uring"]
    IO --> Kqueue["Darwin/BSD kqueue"]
    IO --> Windows["Windows IOCP"]
    IO --> Fallback["blocking helper fallback"]
```

## Scheduler

Each shard owns hot, normal, and inject queues, plus a timer heap and runtime
allocator caches. Task selection favors latency-sensitive work first, then
normal work, injected cross-shard work, and finally stealing.

## Context Switching

Supported platforms use assembly context switches that save the callee-saved
registers required by the platform ABI. Fallback contexts use the portable
context path where assembly support is not available.

## I/O Nodes

I/O nodes own platform backend state:

- Linux: io_uring rings and completion queues
- macOS/BSD: kqueue descriptors and EVFILT_USER wake paths
- Windows: IOCP ports and overlapped operation metadata

Requests carry owner-runtime and owner-shard information so completions wake
the correct task.

## Public Handles

Public handles are process-local misuse hardening. They reject stale, consumed,
wrong-family, and wrong-owner use before object storage is dereferenced. They
are not a sandbox against arbitrary same-process memory read/write.

## Broker Boundary

The broker foundation is the isolation direction for hostile or memory-unsafe
clients. The broker owns runtime state, MAC keys, descriptors/HANDLEs, and ring
sessions outside the untrusted address space. See [Security Model](../security.md).
