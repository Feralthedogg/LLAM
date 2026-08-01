# LEIR AOT CONNECT-WRITE Mechanism Decision

**Date:** 2026-08-01
**Scope:** `SPECIALIZED` Linux/io_uring evidence
**Mechanism verdict:** `CONTINUE`
**3.0.0 release gate:** `BLOCKED`
**Release authorized:** no

## Decision

Continue the generated `CONNECT -> terminal WRITE` direction. The first
controlled screen preserved the portable LEIR result contract, removed one
successful CQE per activation, used no hot-path runtime allocation, and stayed
well below the 5% wall-regression stop boundary in every measured cell.

Do not release 3.0.0. The screen came from one virtualized ARM64 Linux host,
and several concurrency-16 samples exceeded the required 0.70 CPU ratio. The
generated portable fallback, cancellation/fallback matrix, second physical
Linux machine, and release-time license/distribution checks are also still
incomplete.

The compiler-facing ABI fixture is now complete at this research stage. A
standalone C program links only the generated module and public platform
types, while a C++17 consumer compiles against the same header. Neither
consumer includes LLAM runtime internals. The generator, template, checked-in
`.c`/`.h`, and standalone fixture all carry
`LicenseRef-LLAM-Commercial-Reciprocity-1.0`; a regression test rejects stale
output or an Apache identifier in generated output.

## What was measured

The local screen used:

- LinuxKit `6.12.76-linuxkit`, ARM64;
- Ubuntu 24.04 container, GCC 13.3.0, Python 3.12.3, liburing 2.14;
- a privileged container only to expose io_uring through the local Docker
  security boundary;
- loopback TCP and abstract Unix stream sockets;
- concurrency 1 and 16, payloads 64 and 4096 bytes;
- 256 fresh connections per process;
- five fresh-process samples per candidate and cell in ABBA order;
- the portable phase-0 interpreter and the generated Linux AOT module using
  the same listener, payload generator, checksum, worker count, and socket
  lifecycle.

This was a dirty-tree research run. It is useful for the mechanism decision,
not immutable release evidence. Pull-request CI regenerates a clean-source
bundle and records its exact revision, tree digest, binary digest, kernel,
toolchain, and liburing version.

## Results

Ratios are generated-native divided by portable-interpreter wall time; lower
is better. Intervals are deterministic 95% bootstrap intervals over paired
fresh-process ratios.

| Family | Concurrency | Payload | Median wall ratio | 95% interval | Verdict |
|---|---:|---:|---:|---:|---:|
| TCP | 1 | 64 | 0.042 | 0.034–0.045 | `CONTINUE` |
| TCP | 1 | 4096 | 0.040 | 0.023–0.051 | `CONTINUE` |
| TCP | 16 | 64 | 0.470 | 0.464–0.517 | `CONTINUE` |
| TCP | 16 | 4096 | 0.499 | 0.493–0.543 | `CONTINUE` |
| Unix | 1 | 64 | 0.034 | 0.028–0.036 | `CONTINUE` |
| Unix | 1 | 4096 | 0.038 | 0.031–0.042 | `CONTINUE` |
| Unix | 16 | 64 | 0.409 | 0.383–0.689 | `CONTINUE` |
| Unix | 16 | 4096 | 0.425 | 0.409–0.476 | `CONTINUE` |

Every successful native sample reported, per activation:

- one independent queue publication;
- two prepared SQEs;
- one visible terminal CQE;
- one suppressed successful connect CQE;
- one task park and one terminal wake;
- zero hot allocations.

The portable path observed two effect completions per activation. Both paths
produced the same deterministic peer-observed checksum in every pair.

## Why 3.0.0 remains blocked

The current promotion classifier requires all of the following, not merely a
positive mechanism result:

1. at least 1.50x wall improvement;
2. native CPU ratio no greater than 0.70 in every required sample;
3. p99 ratio no greater than 1.10;
4. no short-workload regression;
5. zero correctness, sanitizer, cancellation, fallback, or stress failures;
6. reproduction on at least two recorded Linux machines;
7. a portable generated fallback and an external C/compiler fixture;
8. completed license-transition and distribution checks.

The local aggregate medians were favorable: 11.11x wall speedup, 0.575 CPU
ratio, and 0.295 p99 ratio. The release classifier is deliberately stricter
than those medians. One Unix concurrency-16 sample reached only 1.452x wall
speedup and a 4.60 p99 ratio, the worst CPU sample reached 1.612, and only one
machine was available. It therefore returned `BLOCKED`; the research workflow
additionally asserts that `release_authorized` is always false.

## Next experiment

Keep independent completion ownership and the current semantic barriers.
Investigate concurrency-16 CPU cost without changing the portable baseline:

1. separate module bind/prepare cost from ring submit and completion cost;
2. measure `SUBMIT_ALL`, `COOP_TASKRUN`, and safe single-issuer
   `DEFER_TASKRUN` profiles against same-profile portable references;
3. add connection refusal, cancellation, peer close, short write, and
   unsupported-kernel fallback controls to the evidence bundle;
4. add a generated portable backend, then exercise the completed standalone
   C/C++ ABI fixtures against that backend before making a portable claim;
5. reproduce the frozen matrix on a second physical Linux machine.

The semantic contract and unchanged release criteria are defined in
[LEIR AOT Semantic-Barrier Design](../specs/2026-08-01-leir-aot-semantic-barrier-design.md).
