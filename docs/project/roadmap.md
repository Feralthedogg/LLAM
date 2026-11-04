# Roadmap

LLAM's near-term roadmap is stability and release confidence rather than broad ABI
churn. Future core-runtime work preserves the public ABI major 2 contract retained by
3.0.0, including size-aware compatible option prefixes.

## Current guarantees

The current line treats the public C ABI as stable while moving the canonical
embedding boundary to explicit runtime handles. These are part of the maintained
contract rather than future work:

- Focused direct API tests cover lifecycle, public handle stale, reuse, and forgery
  guards, task ownership, cancellation, channel close and select, blocking callbacks,
  condition and mutex deadlines, managed and unmanaged call boundaries, task-local
  isolation, structured task-group ownership, and live I/O wait diagnostics.
- Runtime-owned failures are reduced into focused runtime tests before they rely on
  example-server reproduction.
- The runtime handle API is the canonical embedding boundary. Explicit heap-backed
  runtimes can be created, spawned into, driven, and destroyed independently. Legacy
  host-thread lifecycle wrappers target `llam_runtime_default()`, while managed task
  spawn, stop, and shutdown wrappers use the task's owner runtime. Spawn-time
  cancellation tokens and task-group children stay in their target owner runtime, and
  owner-tagged runtime objects fail cross-owner managed use with `EXDEV`.
- In-process opaque handles are hardened against stale use, wrong-family casts, simple
  forgery, owner mismatch, and active-operation destroy races. They are not a
  capability boundary against arbitrary same-process memory read and write. The
  stronger boundary is the [broker model](../reference/security/index.md).
- Default release builds keep owner diagnostics enabled.
  `LLAM_RUNTIME_DISABLE_OWNER_CHECKS=1` is an explicit source-build profiling knob for
  unsafe singleton-only binaries, not the documented ABI-conformance mode.
- Rare-hang diagnostics are expected to be actionable. `llam_dump_runtime_state()`
  emits lifecycle and stop state, active I/O waiters, block-helper wake state, node
  queues, shard wake and I/O ownership, and task-level wait ownership.
- Release archives stay self-contained, and the chat examples separate best-effort
  throughput from lossless backpressure so benchmark logs are not misread as runtime
  message loss.
- CI stays layered: quick PR gates, platform stress gates, nightly sanitizer, fuzz, and
  benchmark runs, and weekly soak runs.

## Documentation

- Keep install, build, ABI, operations, and security boundaries discoverable from the
  first navigation level.
- Add FFI-focused examples for size-aware structs and dynamic ABI checks.
- Add platform-specific verification guides for Linux, macOS, and Windows.
- Keep documentation focused on the C ABI, platform backends, operational limits, and
  verification commands.

## Runtime stabilization

- Extend direct API coverage for shutdown races, cancellation ownership, lost wakeups,
  select fanout, blocking helpers, and I/O request ownership.
- Keep promoting low-level failures out of examples into focused runtime tests.
- Harden true concurrent multi-runtime execution: broaden platform I/O owner tests,
  keep signal and fault hooks reference-counted across concurrent runtime lifecycles,
  and preserve single-runtime benchmark guardrails.
- Keep improving diagnostic dumps so rare shutdown, cancellation, wake handoff, and I/O
  ownership hangs can be debugged without reproducing them under a debugger.

## Broker isolation

- Keep broker symbols internal until the runtime RPC surface and descriptor authority
  policy are ready for a public contract.
- The implemented foundation covers subject-bound tokens, attenuation and revocation,
  broker-controlled buffer, channel, and descriptor grants, POSIX `SCM_RIGHTS`, Windows
  `DuplicateHandle`, private ring setup over the control transport, bounded batched ring
  serving, predefined task commands, stale-output zeroing, and broker-local locking.
- Remaining work is a broader runtime RPC surface and production policy around which
  operations may expose raw descriptor or HANDLE authority.
- Expand broker ring soak coverage and malformed-client tests.
- Preserve the documented boundary: in-process opaque handles harden misuse; broker mode
  is the process-boundary isolation direction.

## Platform and performance

Performance work stays platform-local and benchmark-gated. Each optimization needs
before and after numbers against LLAM's own baseline, plus Go and Tokio where the
comparison is meaningful.

- Linux: reduce io_uring request allocation, CQE handoff, poll wake, and timer overhead
  until the backend consistently matches the current macOS tuning envelope.
- macOS: keep kqueue direct paths and Mach-aware scheduling stable, and target timer
  batching, helper handoff latency, and workload-specific poll wake profiles.
- Windows: keep IOCP socket and HANDLE paths on the same public API while improving
  direct-handoff hit rate, queue handoff cost, and Windows-version batching policy.
- Scheduler: measure shard lock contention, worker wake storms, task reclaim cost, and
  timer heap locality before replacing primitives.
- Channels: preserve correctness first. Optimize buffered channel paths, select fanout,
  and wake accounting only with regression tests that catch lost wakeups.
- BSD: support ended in 3.0.0; keep retired targets rejected by current tooling.
- RISC-V: start with cross-build and QEMU smoke, and require its own ABI tests and native
  verification before publishing artifacts.

## Ecosystem

The target is a small, embeddable C runtime with reproducible builds, clear examples,
and CI that catches platform regressions early.

- Track benchmark artifacts as CSV and PNG outputs, so performance claims tie to
  recorded runs.
- Keep `scripts/bench_guard.py` as a catastrophic-regression gate. Scheduled
  LLAM/Go/Tokio comparison jobs remain the source of public performance tables.
