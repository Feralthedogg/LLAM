# LSWG Phase 0 Results

Date: 2026-08-02  
Track: LLAM Structural Wait Graph  
Scope: standalone synthetic model only  
Model result: PASS  
Runtime-integration status: HELD

## What was tested

The Phase 0 harness constructs allocation-owned, generation-stable synthetic
wait graphs and analyzes them without linking the LLAM runtime. It exercises:

- mutex, join, condition-variable, channel send/receive, select, timer, I/O,
  blocking-job, cancellation, external-source, and backend-source edges;
- two-task, three-task, and mixed mutex/join closed cycles;
- providerless mutex and I/O chains;
- ready/matchable channels, expired timers, canceled tokens, finished jobs,
  and terminal join targets;
- host-open condition and channel paths, future timers, live I/O, running
  blocking work, uncancelled tokens, and runnable owners;
- incomplete lock, allocation, malformed-edge, and undersized-workspace
  evidence;
- stable-identity behavior across raw-address reuse and multiple runtimes;
- normalized fingerprint changes for task, public-object, I/O, select,
  ownership, and graph-membership changes;
- two-snapshot confirmation for every reportable verdict.

The test executable contains 22 contract cases. Expected verdicts and evidence
members are literal in the fixtures.

## Result summary

| Property | Result | Evidence |
|---|---:|---|
| Closed cycles are found | PASS | Two-task, three-task, join, and mixed SCC fixtures |
| Open-world waits stay nonfatal | PASS | Host signal, timer, I/O, blocking job, cancellation, and runnable-owner fixtures |
| Orphans are distinguished from cycles | PASS | Providerless mutex and I/O fixtures |
| Persistent ready/terminal sources are distinguished | PASS | Lost-wake and overdue-source fixtures |
| Incomplete evidence dominates | PASS | Lock-busy, allocation, inconsistent edge, and workspace cases |
| Stable identity excludes debug addresses | PASS | Raw address and capture sequence do not affect fingerprints |
| Semantic changes invalidate confirmation | PASS | All required generation, state, owner, and membership mutations change the fingerprint |
| Matching OPEN remains nonfatal | PASS | Two matching open snapshots remain unconfirmed OPEN |
| Reportable verdicts require two stable snapshots | PASS | Cycle, orphan, lost-wake, and overdue results confirm only after matching snapshots |
| Solve path allocates after initialization | PASS | Zero model-allocator calls across repeated solves |

## Scale evidence

Four deterministic 100,000-node profiles were exercised:

1. a long chain ending in a runnable task;
2. many independent closed SCCs;
3. a high-fanout select with open alternatives;
4. a mostly open graph containing one orphan.

Each profile was solved five times using the same pre-sized workspace. On an
Apple M4 running Darwin 25.5.0 with Apple Clang 21.0.0, five Release batches
of 20 total solves measured:

```text
151.444 ms
148.326 ms
147.578 ms
152.350 ms
146.339 ms
```

The median batch time was 148.326 ms, or about 7.42 ms per 100,000-node solve.
The conservative Phase 0 batch budget is 5,000 ms. Timing is diagnostic CPU
time, not a cross-platform performance guarantee.

ASan and UBSan completed the same 100,000-node profiles in 310.259 ms for the
20-solve batch with no reported error.

## Verification

- direct strict C11 build with `-Wall -Wextra -Wpedantic -Werror`: PASS;
- focused Make target, including the 100,000-node profile: PASS;
- focused CMake/CTest targets: 2 of 2 PASS;
- full Release CTest suite: 26 of 26 PASS;
- AddressSanitizer and UndefinedBehaviorSanitizer: PASS;
- Apple Clang static analyzer: PASS;
- production `src/` and public ABI changes: none.

## Defect found during hostile review

The first graph validator checked select-edge endpoint semantics only while
iterating existing select nodes. A malformed `SELECT_ALTERNATIVE` whose source
was not a select could therefore escape validation when the graph contained no
select node. A failing regression reproduced the issue. Finalization now
validates every edge shape globally and verifies each select has an alternative
after deterministic edge sorting. The select-presence check is linear in graph
size.

## Limits of this evidence

- There is no runtime snapshot reader, lock acquisition, task pinning, or
  watchdog state-machine integration.
- The model does not measure normal-execution overhead; it is not linked into
  production targets, so this branch adds no production hot-path work.
- Clean and injected LRPA race profiles do not yet exist. Consequently the
  zero-false-positive soak gate required for LSWG Phase 1 is not satisfied.
- Platform CI for this branch must pass after publication before the evidence
  can be treated as portable.

The corresponding advancement decision is recorded in
`2026-08-02-lswg-phase0-decision.md`.
