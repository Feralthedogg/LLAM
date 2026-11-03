# Roadmap

LLAM's near-term roadmap is documentation, stability, and release confidence
rather than broad ABI churn.

## Documentation First

- Split the current README into task-oriented docs.
- Keep install, build, ABI, operations, and security boundaries discoverable
from the first navigation level.
- Add FFI-focused examples for size-aware structs and dynamic ABI checks.
- Add platform-specific verification guides for Linux, macOS, and Windows.

## Runtime Stabilization

- Extend direct API coverage for shutdown races, cancellation ownership, lost
  wakeups, select fanout, blocking helpers, and I/O request ownership.
- Keep promoting low-level failures out of examples into focused runtime tests.
- Broaden platform I/O owner tests for explicit runtime handles.

## Broker Isolation

- Keep broker symbols internal until the runtime RPC surface and descriptor
  authority policy are ready for a public contract.
- Expand broker ring soak coverage and malformed-client tests.
- Preserve the documented boundary: in-process opaque handles harden misuse;
  broker mode is the process-boundary isolation direction.

## Platform And Performance

- Linux: reduce io_uring request allocation, CQE handoff, poll wake, and timer
  overhead.
- macOS: tune kqueue timer batching and helper handoff latency.
- Windows: improve IOCP direct-handoff hit rate and Windows-version batching
  policy.
- BSD: support ended in 3.0.0; keep retired targets rejected by current tooling.
- RISC-V: start with cross-build and QEMU smoke before publishing artifacts.
