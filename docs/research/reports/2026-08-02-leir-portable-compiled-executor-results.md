# LEIR Portable Compiled Executor Results

**Date:** 2026-08-02
**Scope:** portable compiled semantics and separate Linux/io_uring specialization
**Portable correctness:** `PASS`
**Portable performance:** `INCONCLUSIVE`
**3.0.0 release gate:** `BLOCKED`
**Release authorized:** no

## Provenance

The sealed local run measured the clean source revision
`4ebf252549566415ac6911862fe3b9bd074c7c84` with source-tree digest
`3adb5baaf4a65ca2b627e71054d0dc49204d991773638ca540aa880c8e0ff107`.
The benchmark binary digest was
`89819500d59731bb7d73e135265684903820dc8f66e57cdc04d0db8b6a6696a2`.

The environment was:

- LinuxKit `6.12.76-linuxkit` on ARM64;
- Ubuntu toolchain container `llam-leir-linux:2cd`;
- GCC 13.3.0, Python 3.12.3, and liburing 2.14;
- privileged Docker execution to expose local io_uring support;
- one virtualized host, not a second independent machine.

The ignored local evidence bundle is
`object/leir-portable-compiled-executor/`. It contains `attempts.json`,
`raw.csv`, `summary.csv`, `metadata.json`, `verdict.json`, and `summary.md`.
The read-only `--audit-only` replay regenerated every projection byte for byte
and verified the complete metadata-declared worklist.

## Matrix

The frozen matrix used TCP and abstract Unix stream sockets, concurrency 1 and
16, payloads 64 and 4096 bytes, 256 activations, and five paired samples per
candidate. Candidate order followed the declared ABBA/BAAB schedule with a
shared deterministic seed for each pair.

Two evidence axes were kept independent:

1. `portable_control`: phase-0 interpreter oracle versus portable compiled B;
2. each Linux profile: portable compiled B versus Linux linked B.

This produced 320 scheduled attempts. `submit_all` and `coop_taskrun` produced
240 valid result rows. Both candidates in all eight `defer_taskrun` cells
returned the explicit unsupported-platform skip, accounting for the remaining
80 attempts. No fallback profile or synthetic sample replaced those rows.

## Correctness and structural receipts

All 240 emitted rows reported `correctness=1`. Every paired result checksum
and peer-observed payload checksum matched. Across those rows the screen
completed 61,440 activations and 122,880 logical operations.

The 40 oracle rows recorded 30,720 interpreter dispatches, exactly three per
activation. All 200 compiled rows recorded:

- zero interpreter dispatches;
- 51,200 normalizations and 51,200 site lookups, exactly one of each per
  activation;
- one park and one wake per activation in this run;
- zero hot-path allocations.

The 80 successful Linux rows recorded:

- 40,960 prepared SQEs, exactly two per activation;
- 20,480 observed CQEs, exactly one per activation;
- 20,480 suppressed successful CQEs, exactly one per activation;
- 20,480 queue publications, exactly one per activation;
- 11,673 submit syscalls in total, with 19 to 256 per 256-activation process.

The linked Linux segment therefore removed one successful CQE per activation
without changing the portable result contract. The experiment did not emit a
same-schema submit-syscall counter for the portable candidate, so it does not
claim comparative syscall elimination.

## Performance evidence

Ratios below are candidate divided by baseline; lower is better. Each value is
the median of the eight cell-level paired medians for that axis or profile.

| Axis | Correctness | Performance | Wall | CPU | p50 | p99 |
|---|---:|---:|---:|---:|---:|---:|
| portable compiled / interpreter oracle | `PASS` | `INCONCLUSIVE` | 0.224 | 0.647 | 0.248 | 0.283 |
| Linux `submit_all` / portable compiled | `PASS` | `INCONCLUSIVE` | 0.945 | 0.851 | 0.990 | 0.955 |
| Linux `coop_taskrun` / portable compiled | `PASS` | `INCONCLUSIVE` | 0.967 | 0.911 | 0.980 | 1.008 |
| Linux `defer_taskrun` / portable compiled | `INCONCLUSIVE` | `INCONCLUSIVE` | unavailable | unavailable | unavailable | unavailable |

All eight portable cells passed their semantic gates, but every cell exceeded
the predeclared 20% maximum spread in at least one performance ratio. Seven of
eight `coop_taskrun` cells and all eight `submit_all` cells were likewise
inconclusive because of ratio spread; one Unix/concurrency-1/64-byte
`coop_taskrun` cell passed its performance gate. The classifier therefore
retained `INCONCLUSIVE` instead of converting favorable medians into a
performance claim.

## Limitations

- The portable comparison ran on a Linux LLAM backend. It validates the
  backend-agnostic compiled adapter contract, not macOS or Windows performance.
- The Linux specialization came from one virtualized ARM64 host.
- `defer_taskrun` is unavailable with the current setup-task/dedicated-I/O-
  worker ring topology and was not silently downgraded.
- Loopback `CONNECT -> WRITE` is the first effect segment only; it does not
  cover reads, timers, files, DNS, cancellation races under load, or
  backpressure-heavy applications.
- Performance dispersion remains above the declared threshold. Longer frozen
  windows and independent physical hosts are required before promotion.

These results authorize continued research only. They do not authorize
production promotion, a version change, tagging, packaging, publication, or a
3.0.0 release.
