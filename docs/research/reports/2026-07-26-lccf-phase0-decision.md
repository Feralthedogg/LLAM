# LCCF Phase 0 Engineering Decision

## Decision

Stop the current Local Causal Completion Fusion (LCCF) design after Phase 0.

The formal paired-run verdict remains `INCONCLUSIVE`; this document does not
relabel it. The host failed the predeclared stability gate, so the data cannot
support a publishable performance verdict. It does, however, provide enough
dominant negative evidence to reject further implementation of this particular
representation.

The generated report is preserved verbatim in
`2026-07-26-lccf-phase0-results.md`. Its tracked copy is byte-identical to the
raw report in `object/lccf-phase0/lccf_phase0_report.md`.

## Experiment integrity

The complete predeclared matrix ran successfully:

| Check | Result |
|---|---:|
| Matrix cells | 174 / 174 |
| Fresh paired processes | 1,566 / 1,566 |
| Samples per cell | 9 / 9 |
| Minimum-duration failures | 0 |
| Hot-path allocation failures | 0 |
| Checksum or canonical-result mismatches | 0 |

The run used 65,536 instances, a 250 ms minimum per measured mode, fusion
budget 8, chain length 18, and two remote producers. Pair order was balanced
between ABBA and BAAB.

## Why the formal verdict is inconclusive

The predeclared maximum paired-ratio spread was 1.10x. The interactive macOS
host exceeded it in 169 of 174 wall-clock cells and 149 of 174 CPU-time cells.
macOS did not expose an owner-CPU affinity policy to this harness, and the host
changed calibrated work counts while the matrix was running.

Longer focused measurements did not repair the environment:

| Candidate | 1 s wall median | Wall spread | 1 s CPU median | CPU spread |
|---|---:|---:|---:|---:|
| Causal-cell queue | 0.5170x | 1.5011x | 2.0434x | 1.4886x |
| Fused causal cell | 0.5839x | 1.4483x | 1.7126x | 1.2951x |
| Budgeted fused chain | 1.0779x | 1.3171x | 0.9334x | 1.2702x |
| Remote causal cell | 0.7027x | 1.2778x | 1.3286x | 1.2001x |

All four focused cases still failed the 1.10x spread gate. A pinned, otherwise
quiet Linux host is required before assigning a formal `PASS` or `FAIL`.
Repeating the same experiment on the same host would not add useful evidence.

## Dominant directional evidence

The following aggregates cover every fresh process, despite the instability:

| Candidate | Raw rows | Wall min / median / max | CPU min / median / max | Gate reach |
|---|---:|---:|---:|---:|
| Causal-cell queue | 486 | 0.160 / 0.518 / 1.222x | 1.097 / 1.907 / 5.949x | Best of nine reached 0.95x in 2 / 54 cells |
| Fused causal cell | 486 | 0.164 / 0.614 / 1.078x | 1.095 / 1.617 / 5.741x | Best of nine reached 1.25x in 0 / 54 cells |
| Budgeted fused chain | 513 | 0.438 / 1.061 / 1.909x | 0.539 / 0.942 / 1.177x | Median gain is far below the 1.25x / 0.80 CPU target |
| Remote causal cell | 81 | 0.409 / 0.661 / 0.821x | 1.152 / 1.458 / 2.840x | Best of nine reached 0.95x in 0 / 9 cells |

The negative gaps are much larger than the environmental spread:

- A richer causal-cell object makes the simple owner-local path roughly twice
  as expensive in CPU time.
- Removing the ready-queue hop does not repay callback dispatch, state
  indirection, and per-completion generation arbitration.
- Bounded direct chaining is the only promising component, but its complete
  matrix median is only about 6% faster.
- Encoding remote completion as a causal-cell claim is consistently worse than
  the conventional remote waker queue.
- Balanced ABBA and BAAB medians are similar, so pair order is not the primary
  explanation.

The chance that a stable host would turn the queue, fused-cell, or remote-cell
results into the predeclared win is not credible enough to justify production
work. The chain result is too small to be the requested category-defining
runtime technique.

## What LCCF disproved

The optimization boundary cannot remain a per-task completion object. Replacing
a waker with a richer cell merely moves costs between the scheduler, atomics,
and indirect dispatch. LLAM needs to eliminate the representation and
adaptation work itself, not optimize one scheduler hop.

This agrees with the earlier LCWE result: vector execution helped only when hot
state was already resident in a persistent split layout. Generic packing,
scatter, grouping, and pointer-rich language frames consumed the gain.

## Next research hypothesis

Proceed with a **Site-Resident Effect Machine (SREM)** cost model.

SREM moves the compiler/runtime boundary:

- A compiler lowers a homogeneous async region into persistent field planes
  owned by an LLAM execution site; no runtime pack or scatter step exists.
- Completions append compact lane-and-event words, not task pointers, wakers, or
  causal cells.
- A generated effect superblock consumes ready lane masks and issues or retires
  several continuation steps in one specialized dispatch.
- Owner-local transitions require no callback lookup and no per-step generation
  CAS. Generation validation occurs only when an external producer crosses the
  site boundary.
- Address-exposed, divergent, FFI, and cold state remains in scalar sidecars
  with an explicit fallback path.
- C and other languages can use generated descriptors, while compiler backends
  can lower directly to the same runtime contract.

Unlike LCWE, this hypothesis includes the otherwise missing conversion cost in
the baseline and makes it zero by construction for the candidate. Unlike LCCF,
it removes per-transition objects and dispatch rather than replacing them.

The next gate should demand a category-level result: at least 1.50x wall
throughput and at most 0.70x CPU time on two realistic homogeneous pipelines,
with bounded latency and an explicit scalar crossover. Anything less should
remain an optional specialization rather than define LLAM's architecture.
