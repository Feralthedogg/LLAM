# macOS Performance Track

This document records the macOS-specific performance boundary for LLAM 1.0.0.

## Current Native Paths

| Area | macOS implementation | Status |
| --- | --- | --- |
| Context switch | arm64 and x86_64 assembly paths under `src/asm/darwin/` | Native |
| I/O readiness | kqueue watch/completion backend | Native |
| Direct I/O fast path | nonblocking read/write/connect/poll attempts before backend submission | Enabled |
| Task allocation | task metadata and stack cache reuse | Enabled |
| Safepoints | cheap hot-path checks with heavier sampling outside the common path | Enabled |
| Release packaging | macOS arm64 and x86_64 release archives | Enabled |

## Benchmarks

Use the same matrix for local and CI measurements:

```bash
python3 scripts/bench_runtime_compare.py --runtime all --rounds 31 --warmup 5
```

For fast regression checks:

```bash
python3 scripts/bench_runtime_compare.py --runtime all --rounds 9 --warmup 1 --out-dir object/bench_compare
```

The `Runtime Benchmarks` GitHub workflow runs LLAM, Goroutine, and Tokio on
macOS arm64, macOS x86_64, and Linux x86_64, then uploads CSV and graph
artifacts.

Windows performance numbers are intentionally absent in 1.0.0 because the IOCP
socket backend still needs longer Windows 10/11 stress and benchmark acceptance.
See `docs/windows-roadmap.md`.

## Remaining High-Cost Areas

| Case | Likely remaining cost | Next structural change |
| --- | --- | --- |
| `opaque_block` | helper thread handoff and pthread condition wake cost | Replace helper waits with a Mach semaphore or Darwin `__ulock_wait`/`__ulock_wake` path behind a portability boundary. |
| `sleep_fanout` | timer insertion and wake batching under high fanout | Replace per-wakeup timer handling with a shard-local timer wheel or batched min-heap drain. |
| `poll_wake` | kqueue wake/reinject overhead after direct fast path misses | Keep direct path, then profile workload-specific wake storms before changing structure. |
| `spawn_join` | remaining task metadata initialization and join bookkeeping | Add a more aggressive single-join fast path only if ABI-visible semantics stay unchanged. |

These items require larger scheduler/blocking subsystem changes. They are not
ABI-breaking, but they should be benchmark-gated because small code-level tweaks
are no longer expected to move macOS numbers significantly.

## Acceptance Gate

Before claiming a macOS runtime optimization:

- Run `CC=clang make clean all test`.
- Run `CC=clang make verify-darwin`.
- Run LLAM-only focused benches for the touched case.
- Run `scripts/bench_runtime_compare.py --runtime all` when comparing against Go or Tokio.
- Keep `sleep_fanout`, `poll_wake`, and `opaque_block` free of regressions in both p50 and p99 where possible.
