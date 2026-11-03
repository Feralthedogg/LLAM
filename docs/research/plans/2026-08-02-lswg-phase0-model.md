# LSWG Phase 0 Structural Solver Implementation Plan

**Goal:** Prove that an on-demand generation-stable AND/OR wait graph can distinguish closed cycles, orphan/lost-wake states, and open-world waits without false fatal verdicts.

**Architecture:** Build an allocation-owned synthetic graph and pure solver under `experiments/lswg/`. Phase 0 contains no runtime snapshot reader: tests construct literal stable identities and edges, run escapability fixpoint plus Tarjan analysis, and apply two-snapshot confirmation. This isolates graph semantics from runtime locking concerns.

**Tech stack:** Portable C11, CMake/Make, no production linkage.

**Licensing:** New sources use `SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0`; modified build files keep Apache-2.0.

**Phase boundary:** No `src/`, public API, watchdog threshold, or normal wait-path change.

---

## Task 1: Define identities, graph schema, and failing behavior tests

**Files:**

- Create: `experiments/lswg/lswg_graph.h`
- Create: `experiments/lswg/test_lswg_solver.c`

Expose fixed-width node identities, node/edge kinds from the accepted design, graph/workspace ownership, verdicts, incomplete reasons, a pure solve call, a stable fingerprint call, and a two-snapshot confirmation call.

Write literal fixtures first for:

- two-task mutex cycle;
- join cycle;
- condition wait with external source;
- select with one future timer alternative;
- owner blocked on live I/O;
- orphan mutex owner;
- ready channel with a parked matching waiter;
- expired timer;
- unstable generation between snapshots.

Each test states which solver mutation it catches. Expected verdicts and minimal member IDs are literal, not produced by graph helpers.

Confirm RED from missing symbols with a direct compile, then commit: `test: define LSWG phase zero contracts`.

## Task 2: Implement validated synthetic graph construction

**Files:**

- Create: `experiments/lswg/lswg_synthetic.c`
- Modify: `experiments/lswg/lswg_graph.h`
- Modify: `experiments/lswg/test_lswg_solver.c`

Implement capacity-bounded node/edge insertion, stable-identity uniqueness, endpoint validation, deterministic adjacency ordering, and explicit external/open source nodes. Duplicate identities, missing endpoints, invalid kinds, overflow, and allocation failure must return errors without a partially usable graph.

Add tests for address reuse with different generations, identical identity at different raw addresses, malformed select alternatives, and deterministic ordering under permuted insertion.

Run GREEN and commit: `research: build validated LSWG graphs`.

## Task 3: Implement AND/OR escapability and structural verdicts

**Files:**

- Create: `experiments/lswg/lswg_solver.c`
- Modify: `experiments/lswg/test_lswg_solver.c`

Add failing cases for every wait kind before implementation. Seed runnable tasks, future timers, live backend work, running blocking jobs, uncancelled tokens, ready channels, and external sources as escapable. Propagate resource providers and OR alternatives to a fixed point. Run Tarjan only on the remaining closed set.

Return deterministic minimal evidence:

- `PROVEN_CYCLE` for a closed SCC;
- `PROVEN_ORPHAN` for a providerless closed chain;
- `MATCHABLE_LOST_WAKE` for stable ready/matched state;
- `OVERDUE_SOURCE` for stable expired/terminal sources;
- `OPEN` when any required alternative escapes;
- `INCOMPLETE` whenever graph or snapshot evidence is insufficient.

Ensure an incomplete bit dominates every proven verdict. Test mixed mutex/join cycles, three-task cycles, host channel/cond wakes, active I/O, finished jobs, canceled tokens, and terminal join targets.

Commit: `research: solve structural wait graphs`.

## Task 4: Add fingerprinting and two-snapshot confirmation

**Files:**

- Modify: `experiments/lswg/lswg_solver.c`
- Modify: `experiments/lswg/lswg_graph.h`
- Modify: `experiments/lswg/test_lswg_solver.c`

Write failing tests where coarse progress, task wait generation, public object generation, I/O generation, select completion, mutex owner, or graph membership changes. Implement a canonical fingerprint over sorted stable identities, semantic fields, and edges; exclude raw addresses and capture sequence.

Confirmation promotes only matching complete snapshots with matching coarse-progress tokens. Matching `OPEN` remains nonfatal. `INCOMPLETE`, allocation failure, and lock-busy simulation can never promote.

Commit: `research: confirm stable LSWG verdicts`.

## Task 5: Prove scale and bounded behavior

**Files:**

- Modify: `experiments/lswg/lswg_synthetic.c`
- Modify: `experiments/lswg/test_lswg_solver.c`

Add a deterministic 100,000-node generator covering long chains, many SCCs, OR fanout, and mostly-open graphs. Pre-size a reusable workspace and assert no allocation occurs during repeated solves. Emit a strict timing line only when invoked with `--scale`; normal tests assert correctness, not a machine-specific duration.

Exercise workspace-too-small and simulated-growth-failure cases. Verify report members remain stable under insertion permutation.

Commit: `test: scale LSWG synthetic analysis`.

## Task 6: Integrate standalone targets

**Files:**

- Modify: `CMakeLists.txt`
- Modify: `Makefile`
- Modify: `.gitignore`

Add only `test_lswg_solver`; do not link it into runtime or install it. Register the focused CTest and a Make target. Keep all production source byte-identical.

Run focused tests, ASan/UBSan, the 100,000-node scale case, and then the full CTest suite. Commit: `build: integrate LSWG research harness`.

## Task 7: Record the Phase 0 decision

**Files:**

- Create: `docs/research/reports/2026-08-02-lswg-phase0-results.md`
- Create: `docs/research/reports/2026-08-02-lswg-phase0-decision.md`

Record every fixture class, stable fingerprint result, allocation behavior, scale timing, sanitizer result, and platform. Phase 1 is authorized only if there are zero false proven verdicts, every closed fixture is confirmed, incomplete always dominates, and the 100,000-node model fits the measured diagnostic budget.

Final verification:

```sh
git diff --check
cmake -S . -B object/lswg-full -DCMAKE_BUILD_TYPE=Release
cmake --build object/lswg-full -j4
ctest --test-dir object/lswg-full --output-on-failure
```
