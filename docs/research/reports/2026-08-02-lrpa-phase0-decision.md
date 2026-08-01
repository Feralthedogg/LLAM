# LRPA Phase 0 Decision

Date: 2026-08-02

Decision: NARROW

Experimental harness: ACCEPT

Production/runtime integration: HOLD

## Decision

Keep the N-lane coordinator, semantic oracle, strict result protocol, replay,
and real-manifest shrinker as an isolated experiment and test facility. Do not
link LRPA into the runtime, expose it through the public ABI, or interpret its
calibration curve as a production reliability claim.

The Phase 0 evidence is strong enough to retain the amplifier rather than fall
back to ordinary seed repetition alone. Under the same lane-execution budget,
the 8-lane profile detected 8 skipped-CAS failures versus 1 in the 1-lane
profile, and 8 stale-generation failures versus 0. All 39 observed failures
replayed with stable signatures, all ten sampled manifests shrank while
preserving their signatures, and the measured host completed more than one
million clean executions without a false oracle verdict or cleanup failure.

The result remains NARROW because both faults and the first gadget are
synthetic. In particular, fault eligibility deliberately depends on lane count,
and every sampled failure remained after all perturbation steps were removed.
The experiment therefore validates composition, orchestration, replay,
shrinking, and accounting. It does not yet validate production race
amplification or the perturbation policy.

## Authorized next work

- Keep `experiments/lrpa/` and its bounded CMake/Make tests separate from all
  runtime library targets.
- Add one adapter at a time for real runtime-owned state, beginning with the
  LSWG capture/wake/cancel race family, then I/O cancel/complete/stop.
- Require each adapter to use the existing runtime implementation and a
  state-based oracle; model-only copies do not unlock integration.
- Run at least one million clean executions per required platform in nightly
  CI, retaining every timeout, crash, and schema-invalid sample.
- Measure whether perturbations improve detection after holding lane count,
  process count, and total operations constant. Remove perturbation actions
  that show no marginal value.
- Preserve ordinary 1-lane repetition as the control in every campaign and
  continue reporting correlated empirical rates without an independence
  claim.

## Runtime-integration unlock conditions

All of the following are required:

1. zero false semantic failures over at least one million clean lane executions
   on every required platform;
2. at least one production-backed gadget demonstrates a material equal-cost
   detection improvement over 1-lane repetition;
3. deterministic/core failure replay remains at or above 99 percent;
4. at least 90 percent of sampled production-backed calibration failures shrink
   while preserving the same semantic signature;
5. timeout, setup-failure, and abort profiles leave no thread, task, wait node,
   backend reference, or child process alive;
6. ordinary runtime binaries contain no calibration fault path or LRPA object;
7. platform CI, sanitizers, and the full LLAM suite pass.

## Explicitly not authorized

- no production `src/` change from this result;
- no installed header, runtime flag, or public symbol;
- no claim that the observed `lane_count^2` calibration curve transfers to
  real races;
- no replacement of existing fuzz, sanitizer, or soak testing;
- no version bump and no 3.0.0 release.

If a production-backed gadget shows no material improvement over equal-cost
ordinary repetition, remove its amplifier-specific schedule and retain only
the oracle, manifest, replay, and shrink components.
