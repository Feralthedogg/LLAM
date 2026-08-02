<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
-->

# LCCF Phase 0.5 Representation Decision

## Decision

Select representation B, the 48-byte immutable shared-event sidecar, for the
next test-only Executor experiment.

B normalizes a backend completion once at publication, retains the common
module and payload lifetime through the existing LCCF protocol, and resolves
the active continuation site at materialization. It captures the large and
stable A-to-B normalization gain without retaining an initial resume
descriptor that may no longer match the continuation selected by generated
code.

Keep A as the no-sidecar baseline and fallback. Do not carry C forward as the
default representation. C may remain an experimental control, but its extra
16 bytes and descriptor retention require new evidence before reconsideration.

## Why B

Both persistent representations are correct and both beat A under the
predeclared gates. The choice is decided by the direct B/C contrast:

- B reduces normalization work by 87.50% to 88.89%, depending on route;
- C saves another 8.30% to 18.49% of site lookups;
- C nevertheless has an overall CPU ratio interval of
  `1.018698..1.042092` against B, failing the required `upper <= 0.98` gate;
- C also fails mixed-route wall non-regression and does not conclusively pass
  fused wall or fused/mixed p99 non-regression;
- B uses a 48-byte sidecar while C uses 64 bytes.

The evidence therefore rejects initial-descriptor retention for the selected
generic representation. This is a representation decision, not a rejection
of immutable completion sharing.

## Semantic contract retained

The next experiment must preserve these contracts unchanged:

1. One generation-tagged publication winner owns each immutable completion.
2. Publication is release/acquire; a published event is never rewritten.
3. Module and payload references remain live through every direct or queued
   materialization and callback.
4. Stop, migration, fairness, tracing, pressure, callback-active, and shard
   state are rechecked at the consumption boundary.
5. Direct and queued paths share one consume/callback transaction and one
   failure cleanup path.
6. Every changed continuation site is resolved and validated independently.
7. Invalid sites become immutable `FAIL/EPROTO` events; stale generations do
   not affect a newer generation.
8. Queue saturation uses the intrusive owned fallback and cannot drop work.
9. Storage reuse waits for backend, queue, callback, module, payload, and
   ticket ownership to retire.

LEIR remains the compiler/planner semantic contract. Representation B is an
implementation choice below that boundary and must not leak platform-specific
completion details into LEIR.

## Authorized next experiment

Begin a test-only native-effect-segment prototype with the following boundary:

```text
LEIR semantic segment
        |
        v
AOT/backend-native compiled effect segment
        |
        v
LCCF shared event B + common lifecycle/guard protocol
        |
        v
platform adapter
```

The first backend is Linux `io_uring`. It must compare two independent axes:

1. generic A-versus-B normalization and lifetime sharing;
2. Linux-only linked-operation planning and CQE traffic removal.

Do not combine those numbers. A platform-specific win cannot be reported as a
generic runtime win, and a generic event-sharing win cannot stand in for real
SQE/CQE reduction.

The prototype should keep the interpreter as the differential oracle while a
whole LEIR effect segment is lowered ahead of completion. Validation must
include success, partial completion, cancellation, timeout, linked-operation
failure, stale generation, module unload deferral, queue fallback, and every
continuation-site transition. Measure executed callbacks, SQEs, CQEs, system
calls, process CPU, wall time, and tail latency with separately declared gates.

## Not authorized

This decision does not authorize:

- replacing the production per-completion path;
- changing installed headers or the stable C ABI;
- exposing representation B as a public layout contract;
- removing the interpreter oracle;
- claiming kqueue or IOCP parity from the Linux result;
- changing the project version;
- tagging, packaging, publishing, or releasing version 3.0.0.

Version 3.0.0 remains contingent on real backend evidence, native segment
correctness, lifetime and race validation, portable adapter conformance, and
explicit release authorization after those gates pass.
