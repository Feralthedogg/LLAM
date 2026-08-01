# LLAM Four-Track Runtime Research Architecture

Status: accepted for isolated Phase 0 implementation  
Repository anchor: `Feralthedogg/LLAM`, `origin/main` at `a0674b7`, reviewed 2026-08-02  
Scope: research and experimental paths only; no stable ABI changes in Phase 0

## 1. Decision

LLAM should pursue four isolated research tracks:

1. **LCRS — LLAM Coded-Rotation Stealing**
   - Replace all-shard victim discovery with bounded, low-collision candidate palettes.
   - Preserve the current full-scan selector as an exact fallback.
2. **LSWG — LLAM Structural Wait Graph**
   - Build an on-demand AND/OR wait graph only after the current watchdog sees persistent quiescence.
   - Confirm structural stalls with two generation-stable snapshots before classification.
3. **LRPA — LLAM Repetition-based Parallel Amplifier**
   - Compose existing race gadgets into reproducible N-lane concurrent experiments.
   - Provide fault calibration, replay, shrinking, and empirical amplification curves.
4. **LCCF-CFS — Common-Fact Sharing for Causal Completion Fusion**
   - Normalize and validate each winning completion once.
   - Share only stable facts between direct and queued continuation paths; recheck mutable policy guards at consumption time.

These are inspirations from coding theory, parallel repetition, Ramsey/extremal graph constructions, and circuit/formula sharing. They are not mathematical corollaries of those results.

## 2. Isolation boundaries

| Track | Production surface in first active phase | Hot-path effect when disabled |
|---|---|---|
| LCRS | Fiber-plane victim discovery only | Zero |
| LSWG | Controller/watchdog and diagnostics only | Zero, except existing metadata reads |
| LRPA | Tests and experiments only | None; not linked into release targets |
| LCCF-CFS | Experimental Executor plane only | None for existing stackful tasks |

### Non-negotiable boundaries

- LCRS must not alter queue ownership, steal eligibility, pinned-task behavior, merge pauses, or migration accounting.
- LSWG must never hold more than one runtime object lock at a time and must use try-lock or pinned snapshots.
- LRPA must never be a production dependency.
- LCCF-CFS must not memoize mutable scheduling decisions and must not reuse a fact across generations.
- No Phase 0 work changes the installed public ABI.
- Every experimental path has an immediate conservative fallback.

## 3. Dependency graph

```text
LRPA core
  ├── validates LCRS steal/offline races
  ├── validates LSWG capture/wake/cancel races
  └── validates LCCF fact publication and reuse

LCRS ───────── independent fiber-plane optimization
LSWG ───────── independent read-only diagnostic subsystem
LCCF-CFS ───── depends on the existing LCCF standalone model
```

LCRS and LCCF do not share runnable queues in their first implementations. LCCF escape work stays on its own Executor queue until its lifetime and fairness model is proven.

## 4. Common engineering rules

### 4.1 Generation identity

Every snapshot or completion record must include the identity that prevents address ABA:

- task ID plus wait generation;
- public object family, slot, and generation when available;
- I/O operation generation;
- Executor instance generation and backend-ticket generation.

Raw addresses are diagnostic annotations, not identity.

### 4.2 Stable versus mutable data

A value may be shared or cached only if the owning generation and retained references make it immutable.

Examples of stable data:

- normalized backend result;
- winning generation;
- module/site descriptor under a retained module reference;
- object family/slot/generation;
- event kind and payload ownership.

Examples that must be rechecked:

- current home shard;
- online/offline and migration state;
- runtime stop;
- fairness budget;
- tracing/debug mode;
- direct-execution enablement;
- current queue pressure.

### 4.3 Measurement

- Use fresh processes for benchmark samples.
- Alternate baseline/candidate order with ABBA and BAAB blocks.
- Compute process-local ratios before aggregation.
- Retain all raw samples and seeds.
- Do not count diagnostic probe counters as runtime progress.
- A mathematically motivated mechanism must beat a simpler random or recompute baseline or be removed.

## 5. Rollout order

### Phase 0: standalone proof of cost and correctness

- LCRS selector model and topology-property tests.
- LSWG synthetic graph solver and snapshot schema tests.
- LRPA core with one select/cancel gadget and fault calibration.
- LCCF-CFS extension to the existing standalone LCCF model.

No files under installed public headers are changed.

### Phase 1: observe-only runtime integration

- LCRS computes coded and current victims but steals from the current victim.
- LSWG emits advisory reports while the current watchdog remains authoritative.
- LRPA runs existing LLAM race gadgets in PR and nightly profiles.
- LCCF-CFS runs only in the experimental Executor model.

### Phase 2: experimental active mode

- LCRS may actively select victims behind an internal mode.
- LSWG may classify confirmed structural cycles, but external/open waits remain nonfatal.
- LCCF-CFS enters a cross-platform Executor prototype only if LCCF itself passes.
- LRPA becomes a required nightly correctness gate.

### Phase 3: stabilization

- Cross-platform Linux, Darwin/BSD, and Windows validation.
- Size-aware diagnostics or experimental flags may be exposed.
- Stable ABI consideration occurs only after repeated releases with the feature disabled by default.

## 6. Repository-oriented file map

```text
docs/superpowers/specs/
  2026-08-01-llam-coded-rotation-stealing-design.md
  2026-08-01-llam-structural-wait-graph-design.md
  2026-08-01-llam-parallel-race-amplifier-design.md
  2026-08-01-lccf-common-fact-sharing-design.md

experiments/
  lcrs/
  lswg/
  lrpa/
  lccf/

src/engine/scheduler/
  steal_policy.c                 # after LCRS Phase 0

src/engine/watchdog/
  wait_graph_snapshot.c          # after LSWG Phase 0
  wait_graph_solver.c
  wait_graph_report.c

src/executor/                    # only after LCCF prototype authorization
  completion_fact.c
  completion_consume.c
```

## 7. Global rejection rules

A track is rejected or simplified when:

- LCRS does not materially beat deterministic random-k sampling.
- LSWG cannot avoid false positives in open-world external-wake cases.
- LRPA does not reproduce injected faults more reliably than ordinary repetition.
- LCCF-CFS adds a cache line or reference churn without reducing instruction or CPU cost.
- Any feature requires a correctness compromise to meet a performance target.
