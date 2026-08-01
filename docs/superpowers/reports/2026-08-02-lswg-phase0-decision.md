# LSWG Phase 0 Decision

Date: 2026-08-02  
Decision: NARROW  
Semantic model: ACCEPT  
Runtime Phase 1: HOLD

## Decision

Keep the standalone LSWG model as the executable semantic contract. Do not add
a runtime snapshot reader, watchdog verdict authority, fatal mapping, or public
diagnostics API yet.

Phase 0 demonstrates that a bounded on-demand graph can distinguish closed
cycles, providerless chains, persistent lost wakes, overdue sources, and
open-world waits. It also demonstrates deterministic fingerprints, conservative
incomplete handling, allocation-free repeated solves, and acceptable synthetic
scale on the measured host.

It does not demonstrate that a live runtime can capture those facts without
racing wake, cancellation, reclaim, select completion, owner migration, or
shutdown. The accepted plan requires zero false proven verdicts in clean LRPA
and soak profiles before Phase 1. That evidence is not available yet, so model
success does not authorize runtime integration.

## Authorized next work

- Preserve `experiments/lswg/` as the reference solver and fixture format.
- Feed the LSWG capture/wake/cancel race families into the independent LRPA
  Phase 0 harness.
- Use clean LRPA runs to measure false proven verdicts and injected runs to
  verify that stable closed states are still detected.
- Re-evaluate Phase 1 only after platform CI and LRPA evidence are recorded.

## Phase 1 unlock conditions

All of the following are required:

1. zero false `PROVEN_CYCLE`, `PROVEN_ORPHAN`, `MATCHABLE_LOST_WAKE`, or
   `OVERDUE_SOURCE` confirmations in clean LRPA and soak profiles;
2. every injected stable closed fixture is confirmed after two snapshots;
3. wake, cancellation, select completion, reclaim, rehome, shutdown, lock-busy,
   and workspace-growth races become OPEN, PROGRESS_CHANGED, or INCOMPLETE;
4. the 100,000-node model remains within the diagnostic budget on required CI
   platforms;
5. the proposed live capture path holds at most one runtime object lock and
   never blocks waiting for an internal lock;
6. normal execution overhead remains below 0.5 percent when advisory capture
   is integrated behind an internal mode.

## Explicitly not authorized

- no `src/` change from this result;
- no replacement of the current watchdog verdict;
- no fatal error mapping;
- no public ABI or installed header;
- no version bump or 3.0.0 release.

If LRPA cannot keep clean open-world cases free of proven verdicts, LSWG remains
a synthetic diagnostic tool and the runtime integration path is rejected.
