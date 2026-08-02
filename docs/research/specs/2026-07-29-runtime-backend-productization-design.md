# Runtime Backend Productization Design

**Date:** 2026-07-29

**Status:** Approved for autonomous execution by the request to implement the
complete review, with implementation decisions delegated to the maintainer.

## Purpose

LLAM is to remain a general-purpose concurrency runtime and language-runtime
backend. LEIR remains the portable semantic contract between compiler/planner
frontends and LLAM backends. Experimental backend work must not weaken the
stable runtime, public ABI, host process, or release boundary.

This design turns the full P0/P1/P2 review into a staged productization
program. It does not replace LLAM's ownership model or rewrite the scheduler.
It makes resource use explicit, gives embedders stable context and driving
contracts, bounds retained memory, isolates research code, teaches sanitizers
about fibers, and reduces representation and maintenance debt without
weakening existing correctness invariants.

## Evidence at the starting revision

The starting revision is
`32916b8e69c54f9c527c5c5f0eca475db5ff2e50`.

- The experimental Chase-Lev delayed-thief slot reuse race is fixed and has a
  deterministic wraparound regression.
- LEIR native segments reduce successful CQE traffic as designed, but the
  precommitted performance verdict is `REJECT` on both measured
  architectures.
- `LLAM_BUILD_RESEARCH` does not exist. Default CMake builds research targets,
  and Linux production libraries include native-segment implementation.
- Runtime worker, blocking-pool, affinity, and prewarm controls are not public.
- Task prewarm is per shard while stack prewarm is runtime-total. Timer
  prewarm is also per shard.
- Stack-cache retention is bounded by per-class/per-shard counts rather than a
  runtime byte budget. At 64 release-fast shards, the theoretical retained
  mapping total is several GiB.
- A cross-runtime stack-cache ownership defect exists: an A task spawning on B
  can select A's TLS shard cache while allocating B's task stack.
- `llam_task` and `llam_shard` retain cold and experimental storage
  unconditionally.
- ASan and TSan fiber transition APIs are not used.
- A non-guard SIGSEGV replaces the host handler with `SIG_DFL` instead of
  chaining to the saved handler.
- Destroyed explicit runtime handles retain the full runtime object forever to
  avoid raw-pointer ABA.
- Autotune reports several supported domains while only handoff has an
  actuator.
- Structure auditing excludes tests and experiments and does not reliably
  promote explicit size-budget violations in strict mode.
- The current cancellation-result test has a nondeterministic accept ordering:
  cancellation can be observed after a transient `EAGAIN` before a successful
  result exists, so the test does not always reach the disposal boundary it
  claims to test.

## Alternatives considered

### A. Patch only the externally visible controls

Append worker and memory options, update documentation, and leave internal
layout and build boundaries unchanged.

This is rejected. It would expose control knobs while research code still
ships by default, retained memory remains unaccounted, host TLS/signal
contracts remain ambiguous, and the largest lifetime protocols remain outside
structural enforcement.

### B. Replace the scheduler and runtime representation in one rewrite

Introduce a new scheduler, new handle table, cold-state graph, and external
driver at once.

This is rejected. LLAM's strongest property is its existing ownership and
generation protocol. A one-shot rewrite would multiply the state space and
destroy the ability to prove which change caused a regression.

### C. Staged contract-first productization

First isolate build/release research, then add a pure resource plan, then
memory governance, embedding contracts, host/sanitizer integration, layout
changes, and finally state-machine refactoring and generational runtime
handles. Each stage has deterministic tests, compatibility gates, and its own
review.

This is the selected approach.

## Program invariants

The following requirements bind every subproject:

1. Existing cancellation, generation, active-operation, wait-owner, I/O
   retirement, and cross-runtime `EXDEV` invariants remain intact.
2. No public API reads beyond a caller-supplied size-aware prefix. Legacy
   convenience wrappers continue to consume only the frozen 2.2 prefix.
3. Research-off is the default for Make, CMake, packages, and standard CI.
4. A research-enabled build cannot be packaged or released.
5. Research-on and research-off builds expose identical installed headers,
   ABI major, SONAME, pkg-config/CMake interfaces, and public dynamic symbols.
6. Exact public resource requests either resolve completely or fail before
   runtime threads and partial resource graphs are published.
7. Runtime-owned mappings, threads, and handles have one explicit owner and
   are accounted exactly once on every success, failure, cancellation, and
   shutdown path.
8. Test-only hooks remain absent from production static/shared archives.
9. Default behavior remains compatible unless the old behavior is the
   unbounded or ambiguous condition being corrected. Such changes are
   documented and covered by prefix/legacy tests.
10. LEIR native segments remain private research until a frozen, audited
    performance gate returns the promotion verdict required by the release
    workflow.
11. No version bump, tag, or release occurs while the native performance gate
    remains `REJECT`.
12. Darwin, Linux, Windows, and supported BSD builds remain first-class.

## Subprojects and dependency order

### 1. Research and build governance

- Add a private `LLAM_BUILD_RESEARCH` switch, default off.
- Exclude native-segment implementation and all research executables/tests
  from default builds.
- Reject packaging when research is enabled.
- Add canonical version/source/target manifests and Make/CMake parity audits.
- Replace mutable benchmark output with exclusive, hash-bound evidence
  bundles and a machine-readable verdict.
- Separate nonblocking screening from a release/manual promotion gate.
- Extend structure auditing to tests/experiments with a growth ratchet.

This stage establishes the product boundary needed by every later release
claim.

### 2. Runtime resource governance

- Resolve worker min/initial/max, blocking min/max, selected CPUs, affinity
  policy, and runtime-total prewarm targets in one pure resource plan.
- Start blocking workers lazily between the configured bounds.
- Capture and restore host-driver affinity around each run.
- Report actual scheduler, blocking, I/O, controller, and opaque-helper thread
  counts separately from configured shard capacity.
- Treat task, stack, and timer prewarm as explicit runtime totals.
- Validate 1/8/64-worker plans and aggregate overflow/budget limits before
  allocation.

### 3. Stack-cache memory governance

- Fix cross-runtime cache ownership before introducing accounting.
- Enforce a runtime-total byte budget with high/low watermarks.
- Add live manual, idle, and memory-pressure trim paths.
- Add stack-specific map/release/discard/reactivate/scrub platform helpers.
- Expose exact cached/committed accounting and validity-qualified residency
  diagnostics.
- Support secure scrub/discard-on-return policy.

### 4. Language-runtime embedding contract

- Add O(1) per-task user context and a small fixed set of inline context slots.
- Add opt-in suspend/resume callbacks around every scheduler/task and direct
  task/task switch.
- Make `PINNED` a documented hard non-migration contract and prove every
  steal/rehome path respects it.
- Add an external host-loop contract for readiness handle, next deadline,
  bounded drive, and explicit wake.
- State the migration/TLS rules and callback reentrancy restrictions.

### 5. Sanitizer and host-process boundaries

- Integrate ASan and TSan fiber lifecycle/switch annotations.
- Chain non-guard faults to the saved host action and provide explicit
  signal-install opt-outs.
- Add a hardened build profile with stack clash/protector, FORTIFY, and
  supported RELRO/NOW flags.
- Document fork-after-init as exec-or-exit only and prove the supported parent
  plus child-exec path.

### 6. Runtime layout and maintenance

- Remove the unused embedded timer node and source wait/select payloads from
  the existing shard pools. Retain the embedded I/O request until a separate
  storage-ownership design preserves LEIR's zero-hot-allocation contract.
- Split Linux LEIR native bind-time state behind a lazily allocated pointer
  without moving active runtime publication anchors out of `llam_task`.
- Allocate the experimental Chase-Lev deque only when enabled.
- Allocate the trace ring only when tracing is enabled.
- Add architecture-specific `llam_task` and `llam_shard` size budgets.
- Introduce a common pure direct-handoff policy guard and superset rejection
  enum while retaining operation-specific commit and metric paths.
- Split wait ownership, I/O issue, and Linux native segment files only along
  documented ownership-transition boundaries.
- Separate autotune recognized, observable, active-observation,
  controllable, and active-control domains.
- Convert explicit runtime public handles to the repository's established
  slot/generation token representation so full runtime state can be freed.

## Verification architecture

Every behavioral change follows RED/GREEN:

- a test names the incorrect production transition or resource outcome;
- it is run and observed failing for that reason;
- the smallest complete implementation is added;
- the focused test, owning suite, sanitizers, build audits, and relevant
  cross-platform targets run before the task advances.

The branch completion gate requires:

- clean default Make and CMake builds with research absent;
- research-enabled Make and CMake builds with full LEIR tests;
- public ABI and package parity between research modes;
- complete Make, CTest, ASan/UBSan, TSan, structure, dependency, and export
  audits;
- MinGW/Wine validation plus native Windows CI;
- privileged Linux native-segment unit/evidence validation;
- independent task reviews and one whole-branch review;
- a requirement-by-requirement completion matrix backed by source, test, or
  artifact evidence.

## Delivery and release policy

Changes land as reviewable commits on `leir-native-segment` and keep PR #4
draft until all productization work is complete. The current research verdict
does not authorize LEIR promotion. CI success proves correctness and
portability; it does not override a `REJECT` performance gate. Consequently,
the completed branch may be pushed and reviewed, but it must not be versioned,
tagged, or released until a later immutable performance bundle satisfies the
promotion gate.
