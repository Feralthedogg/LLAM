<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
-->

# LCCF-CFS Phase 0 Engineering Decision

## Decision

Advance Common-Fact Sharing to a narrow, internal Executor prototype.

The Phase 0 verdict is `PASS`: all 72 independent model cells passed, shared
paths normalized and resolved exactly once per generation, no canonical output
changed, and all lifetime references balanced. The smallest observed wall
speedup was 1.779x and the largest median CPU ratio was 0.562 across all cells.

This does not reverse the earlier LCCF decision. The rejected design replaced a
conventional waker with a richer per-completion scheduling object and failed to
repay that representation cost. CFS keeps the ordinary scheduling boundary and
removes duplicated backend adaptation, site lookup, and pin work. It is a
narrower hypothesis with different evidence.

## Selected prototype shape

Use the split 64 + 64 layout:

```text
64-byte hot causal cell
64-byte immutable fact/event sidecar already owned by the Executor instance
```

The three layouts were performance-equivalent in the model. Split 64 + 64 is
selected because it does not enlarge the hot cell and requires no allocation
after instance creation.

The prototype must preserve these contracts:

1. `ARMED(g) -> FACT_BUILDING(g)` has one acquire-release winner.
2. Only the winner normalizes the backend result, resolves the site, and pins
   module/payload lifetime.
3. Ordinary fact writes become visible only through release publication to
   `FACT_READY(g)`.
4. Direct and queued paths call one common fact consumer.
5. Migration, stop, policy, fairness, tracing, pressure, callback-active, and
   shard state are fresh guards, never cached fact truth.
6. Losing tickets retire only references for their exact generation.
7. Queue, callback, backend, external, payload, and module references must all
   retire before the sidecar can be reused.
8. Queue forwarding must defer before targeting an offline destination.

## Phase 1 boundary

The next change may add a test-only/internal Executor prototype, but must not:

- expose the fact record in a public header or stable ABI;
- allocate a fact or queue node on completion;
- replace the existing production path by default;
- change version metadata or publish 3.0.0;
- combine platform-specific CQE savings with generic CFS gains in one number.

Start with Linux `io_uring`, because it gives the clearest opportunity to
measure raw CQE decode removal and connected-operation traffic separately. Add
kqueue and IOCP adapter conformance only after the common contract is stable.

## Required Phase 1 evidence

The Executor prototype advances only if it preserves:

- literal event, errno, payload, command, and final-frame equality;
- exactly one normalization and site lookup per winning generation;
- zero hot allocation and balanced module/payload references;
- direct-path throughput of at least 98% of recompute;
- mixed CPU or instruction improvement of at least 5%;
- no more than 5% p99 regression;
- LRPA race coverage for publication, double consume, migration, stop,
  unregister, callback failure, queue overflow, and generation reuse.

Report generic CFS results independently from Linux-only linked-SQE or CQE
traffic reductions. If the real Executor makes normalization/site work too
small to produce measurable benefit, remove the persistent sidecar and retain
only the canonical platform normalization helper.

## Release status

No release is authorized. Version 3.0.0 remains held until the internal
Executor prototype passes real backend, race, lifetime, and workload evidence.
