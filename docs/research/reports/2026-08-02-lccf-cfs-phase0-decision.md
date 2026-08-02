<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
-->

# LCCF-CFS Phase 0 Engineering Decision

## Decision

Do not advance the persistent Common-Fact sidecar into the Executor yet.

Phase 0 is `INCONCLUSIVE`, not rejected. Correctness and lifetime evidence is
strong, and the heterogeneous eight-site model still shows a favorable median
signal. However, 71 of 72 cells in the controlled repeat exceed the declared
ratio-spread limit. The result cannot authorize production work until the
measurement is stable.

The earlier provisional `PASS` and split-64+64 selection are withdrawn. That
result allowed one resolved callback address to stand in for multiple logical
sites and therefore overstated the transferable work reduction.

## Contracts worth keeping

The following results are independent of the performance verdict and should
remain the semantic basis for later Executor work:

1. `ARMED(g) -> FACT_BUILDING(g)` has one acquire-release winner.
2. Ticket ownership is indexed and generation-tagged; an old ticket cannot
   retire a new generation's backend reference.
3. Fact publication is release/acquire and the published record is immutable.
4. Lifecycle close and in-flight admission share one atomic gate.
5. Finishing publishes `TERMINAL(g)` first; only an explicit, strictly newer
   rearm may expose `ARMED(g+1)`.
6. Direct and queued paths use one consume/callback transaction and one abort
   cleanup path.
7. Mutable stop, migration, fairness, tracing, pressure, callback-active, and
   shard state are rechecked at consumption.
8. The initial resume descriptor may be retained, but every changed
   continuation site must resolve and validate its own function pointer.
9. Queue saturation falls back to an intrusive owned list and never drops a
   cell.
10. Callback, queue, backend, external, payload, module, and ticket ownership
    must all retire before storage reuse.

## Next experiment

Run a narrower Phase 0.5 representation comparison before touching `src/`:

```text
A. canonical event normalization helper only
B. immutable event + retained module lifetime
C. full fact with retained initial resume descriptor
```

Use heterogeneous sites in every performance cell. Measure each option with
long-lived in-process paired windows, randomized ABBA blocks, a minimum window
duration, and a predeclared confidence or spread rule. Keep raw samples and
continue to classify correctness before performance.

This comparison answers the remaining architectural question: whether the
roughly 23% median eight-site model signal requires a persistent 64-byte fact,
or whether a smaller canonical-event helper captures most of it with less
lifetime machinery.

## Executor boundary

Only after Phase 0.5 produces stable evidence may a test-only Executor
prototype begin. If it does, start with Linux `io_uring` and report two numbers
separately:

- generic normalization/lifetime sharing;
- Linux-only linked-operation and CQE traffic reduction.

The prototype must remain internal, allocation-free after instance creation,
and outside the stable ABI. kqueue and IOCP adapter conformance follow only
after the generic contract is stable.

## Release status

No release or version change is authorized. Version 3.0.0 remains held until
real backend, race, lifetime, and workload evidence passes its declared gates.
