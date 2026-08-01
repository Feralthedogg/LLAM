# LEIR Native Pipeline Decision

**Date:** 2026-08-01
**Source revision:** `ca122928ac943d5731b03d66c75c059c2f8bc865`
**Workflow run:** `30519982512`
**Portable verdict:** `REJECT`
**Linux/io_uring verdict:** `REJECT`

## Decision

Stop optimizing the fixed-buffer connected `RECV -> SEND` candidate as the
primary LEIR native-lowering direction. Preserve its implementation and sealed
evidence as a negative experiment, but do not promote it, use it as a 3.0.0
release justification, or generalize the result to other AOT segments.

Continue research with semantic-barrier partitioning and a generated
`CONNECT -> terminal WRITE` segment. That experiment must retain independent
completion ownership and compare portable and Linux-specialized results
separately.

## Evidence scope

The sealed Linux run used:

- Ubuntu 24.04 runner, kernel `6.17.0-1020-azure`, x86-64;
- AMD EPYC 7763, four available logical CPUs, benchmark CPUs `0,1`;
- GCC 13.3.0, Clang 18.1.3, CMake 3.31.6, Python 3.13.14;
- liburing 2.5;
- 9 measured fresh-process pairs per available cell with alternating
  ABBA/BAAB order and 10,000 deterministic bootstrap resamples;
- widths 1/2/4/8, concurrency 1/4/16, and payloads 64/512/4096.

The `fixed_link_skip` candidate had statistically supported wall regressions
in every measured cell. Representative paired estimates were:

| Width | Concurrency | Payload | Wall ratio | CPU ratio | p99 ratio |
|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 64 | 2.707 | 1.006 | 0.992 |
| 2 | 4 | 64 | 12.640 | 2.333 | 1.055 |
| 8 | 16 | 4096 | 1.355 | 1.262 | 1.371 |

Ratios are candidate divided by the public-API baseline; lower is better.
These cells fail the precommitted requirement that no supported wall
regression have a 95% confidence lower bound above 1.00.

## What worked structurally

The experiment did reduce publication and syscall traffic. In the width-8,
concurrency-16, payload-4096 cell, each sample used 8,192 activations and only
1,024 queue publications, or 0.125 publications per activation. Submit
syscalls were roughly 0.114 per activation.

Each two-operation segment still prepared two SQEs and observed one operation
CQE. The linked success CQE was suppressed as designed. Structural success did
not translate into end-to-end performance.

## Why the candidate lost

1. `READ_EXACT` cannot safely treat a short positive receive as linked success.
   The non-fixed `link_skip` candidate was therefore unavailable in all 36
   applicable cells with `exact_result_semantic_barrier`.
2. The fixed candidate copied receive data through registered scratch storage,
   adding work on every activation.
3. Independent segments shared one terminal batch ticket. Submission became
   cheaper, but every owner inherited a slowest-member retirement barrier.
4. Ring-worker submission already coalesced independent work, so semantic
   joining was not required to reduce ring enters.
5. The compact plan still required runtime operation loops and generic result
   reduction; it was not a generated direct AOT resume path.

## Consequences

- The current pipeline remains research-only and cannot authorize a version
  change, tag, package, publication, or release.
- `RECV -> SEND` may be revisited only after a different buffer-ownership
  contract removes copies and exact-result semantics are represented as an
  explicit completion barrier.
- Submission batching and semantic joining are separated. Independent
  invocations keep independent tickets and wakes.
- The next Linux mechanism screen is `CONNECT -> terminal WRITE`, where connect
  success is exactly zero and the final write result remains observable.
- The 3.0.0 promotion thresholds are not relaxed because this experiment lost.

The successor contract is defined in
[LEIR AOT Semantic-Barrier Design](../specs/2026-08-01-leir-aot-semantic-barrier-design.md).
