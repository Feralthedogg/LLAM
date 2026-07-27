# LEIR Phase 0A Engineering Decision

Status: **STOP the generic userspace-advance engine; retain LEIR only as a
compiler/planner contract and test backend-native compiled segments
independently**

Date: 2026-07-27

## Decision

Do not advance the Phase 0A userspace LEIR interpreter to a full gate,
production prototype, public ABI, version bump, or release.

The formal screening verdict remains `INCONCLUSIVE`. The complete 225-cell
screen produced 43 wall-spread and 24 CPU-spread failures above the
predeclared `1.10x` ceiling, so this document does not relabel the classifier
result as `ADVANCE_REJECT`.

The engineering decision is nevertheless to stop this implementation
direction. Twenty-seven of the 28 continuation-gate and short-control cells
were stable, every short control exceeded its CPU guard, and an analysis that
independently selects the best wall and best CPU sample from every required
core cell still leaves both workloads far below even the continuation target.
Another run or the nine-sample full gate cannot plausibly turn this
per-completion interpreter into a category-level result.

The reusable architectural result is narrower:

- a typed effect region can advance across real pending I/O without an
  intermediate language-task publication;
- cancellation, generation, lifetime, partial-I/O, and terminal ownership can
  be made correct without hot-path allocation;
- removing task resumption alone is not the winning boundary because it does
  not remove backend submissions or completions, and the userspace
  graph-advance cost is larger than the scheduling work it replaces.

LEIR may remain an immutable compiler-to-planner description, but it should
not be interpreted node by node on the completion path. The next experiment,
if pursued, must compile a static effect segment into a backend-native plan
that removes submissions and completion traffic as well as task resumes.

## Evidence Provenance

- Branch head:
  `bf350ad506f0b84f4c99158a40d7eb514da839df`
- Authoritative checked-out source:
  `9ea8008e706f9ed5e10d18cd6b9cb78b4c542fe9`
- The authoritative source is GitHub's synthetic pull-request merge with
  parents:
  - base `5c8eb2fd4bb2af877ea3d57db11ea92d594a08b9`
  - branch head `bf350ad506f0b84f4c99158a40d7eb514da839df`
- GitHub Actions run:
  [LEIR Phase 0A Research #30242278950](https://github.com/Feralthedogg/LLAM/actions/runs/30242278950)
- Artifact: `leir-phase0a-screen-linux-x86_64`
- Runner: Ubuntu 24.04, Linux `6.17.0-1020-azure`, AMD EPYC 7763,
  four vCPUs
- Server affinity: CPU 0 from an allowed CPU set of 0-3
- Compilers: GCC 13.3.0 and Clang 18.1.3
- Matrix: 225 cells, five fresh processes per cell, 1,125 raw rows
- Pair order: 675 ABBA rows and 450 BAAB rows
- Measurement: 16 balanced blocks and at least 100 ms per mode
- Generated report:
  `docs/superpowers/reports/2026-07-27-leir-phase0a-results.md`
- Generated report SHA-256:
  `d9d084a83457204c5604dbfee6ff11ca64c8a304acb07b215d539198ffb38a08`

The raw sample CSV, summary CSV, metadata, source identity, runner
environment, affinity record, native logs, CMake logs, sanitizer logs, and
byte-identical raw/tracked reports remain in the workflow artifact.

## Experiment Integrity

The full declared screen completed:

| Check | Result |
|---|---:|
| Matrix cells | 225 / 225 |
| Fresh paired processes | 1,125 / 1,125 |
| Samples per cell | 5 / 5 |
| ABBA / BAAB rows | 675 / 450 |
| Minimum-duration failures | 0 |
| Pending-path failures | 0 |
| Checksum mismatches | 0 |
| Hot-path allocations | 0 |
| Mechanism-accounting failures | 0 |
| Non-server CPU scopes | 0 |

The CMake portability gate, ASan/UBSan gate, TSan gate, native tests, and
artifact report comparison all passed on the same synthetic merge source.

The formal classifier output is:

```text
phase=screen
verdict=INCONCLUSIVE
reason_count=67
wall_spread_failures=43
cpu_spread_failures=24
```

The largest wall spread was `1.444603x` at
`socket_relay, nodes=8, concurrency=1, payload=1024, budget=1`. The largest
CPU spread was `1.192464x` at
`socket_relay, nodes=8, concurrency=512, payload=64, budget=8`.

Only the latter cell was unstable among the 28 continuation-gate and
short-control cells. The other 27 passed both spread ceilings, so the
dominant negative result is not an artifact of broadly unstable gate data.

## Core Gate Evidence

The required screening cells use nodes 4 and 8, concurrency 64 and 512,
payloads 64 and 1024, and inline budget 8.

| Workload | Median wall floor | Median CPU ceiling | Candidate-favorable wall floor | Candidate-favorable CPU ceiling |
|---|---:|---:|---:|---:|
| `socket_relay` | 0.546414x | 1.363331x | 0.548492x | 1.319628x |
| `framed_rpc` | 0.555947x | 1.374077x | 0.564831x | 1.274833x |
| Phase 0A continuation target | >= 1.250000x | <= 0.850000x | >= 1.250000x | <= 0.850000x |
| Final category target | >= 1.500000x | <= 0.700000x | >= 1.500000x | <= 0.700000x |

The candidate-favorable bounds deliberately bias toward LEIR: within every
required cell they select the highest wall speedup and the lowest CPU ratio
from separate fresh processes, then take the required worst cell. Even this
non-publishable best-case construction achieves only about `0.55x` wall
throughput and consumes at least `1.27x` to `1.32x` CPU in the limiting cells.

This is not a marginal miss. The favorable wall floors are below the stable
reject boundary of `1.10x`, while the favorable CPU ceilings remain above
`0.95x`. Measurement variance cannot bridge the gap to either continuation
or category targets.

## Short Controls And Latency

The gate-relevant length-1 and graph-break controls also reject the current
representation:

| Check | Result |
|---|---:|
| Short-control cells | 12 |
| Wall below 0.95x | 6 / 12 |
| CPU above 1.05x | 12 / 12 |
| Cells passing both guards | 0 / 12 |
| Relevant cells with a service or terminal p99 ratio above 1.10x | 6 / 28 |

The graph-break result matters strategically. When native computation forces
an immediate escape, the candidate pays binding, instance, and completion
dispatch overhead without enough eliminated work to amortize it.

## Mechanism Decomposition

The intended mechanism executed:

- candidate task-I/O publications fell to `0.125x`-`0.250x` of baseline in
  the core cells;
- median context-switch ratios were `0.234567x` for `socket_relay` and
  `0.238309x` for `framed_rpc`;
- 7,779,328 intermediate task resumes were avoided across the 80 raw
  gate-driving samples;
- every candidate backend submission matched one effect completion;
- direct completions and fairness resubmissions were zero in the gate cells,
  proving the measured path used real pending backend I/O;
- no hot-path allocation or heap request was observed.

LEIR therefore removed roughly three quarters of the task scheduling traffic
and still lost substantially in wall time and CPU. The remaining kernel
submission/completion count did not change. Typed-slot access, node dispatch,
request re-preparation, sink dispatch, and instance state transitions cost
more than restoring the stackful task and issuing its next ordinary operation.

The failed component is not correctness or task-publication elimination. It
is interpreting a general effect graph after every completion while retaining
one backend operation and one completion per effect.

## Consequences

- Do not run the nine-sample Phase 0A gate.
- Do not expose the experimental descriptor or instance model as a C ABI.
- Do not enable the completion-sink path as a production policy.
- Keep the experiment and authoritative negative evidence so this
  per-completion interpreter is not reintroduced under another name.
- Preserve the independently valuable runtime fixes discovered by this work:
  socket receive classification across async reads and the early-completion
  park race. They should be carried through a focused production change, not
  released as part of this research branch.
- Keep version `2.2.0` unchanged. Passing CI proves the experiment is correct
  and reproducible; it does not establish production readiness.

## Next Research Direction

Retain the user's intended product position—LLAM as a language-neutral runtime
backend—but move the optimization boundary from **interpreted effects** to
**compiled effect segments**:

1. A frontend or C builder emits the same typed, immutable effect-region
   description.
2. Program creation validates it once and lowers each static segment into a
   backend-native plan; execution does not walk a generic node table.
3. Linux first tests linked `io_uring` SQEs and safe CQE suppression so the
   candidate removes submit syscalls and completions, not only task resumes.
4. Dynamic result dependencies and graph breaks call generated native
   transition code or return to the ordinary LLAM task path.
5. Windows and kqueue work begins only after a Linux segment demonstrates a
   category-sized win; their executors must be backend-specific rather than a
   portable per-completion interpreter.
6. A Linux-only win is labeled `SPECIALIZED`. It does not define LLAM's
   cross-platform architecture or authorize a public ABI.

This is not Phase 0B advancement under the failed Phase 0A continuation gate.
It is a separate, narrower falsification experiment aimed at the missing
mechanism: eliminating backend operations and CQE traffic through ahead-of-time
segment lowering.

The next screen should require the existing final targets without relaxation:
at least `1.50x` wall throughput, at most `0.70x` CPU, short controls within
`0.95x`/`1.05x`, p99 degradation at most 10%, and no hot-path allocation. If a
backend-native static segment cannot clear that bar, LEIR should remain only a
semantic integration format and LLAM should compete on its stable C ABI,
portable scheduling, cancellation, observability, and adapter ecosystem
rather than claiming effect-graph execution as its performance moat.
