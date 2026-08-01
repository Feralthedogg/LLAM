# LEIR AOT Ring-Profile and Cost-Attribution Decision

**Date:** 2026-08-01

**Scope:** Linux/io_uring-specialized research

**Decision:** `NARROW`

**3.0.0 release gate:** `BLOCKED`

**Release authorized:** no

## Decision

Keep the generated `CONNECT -> WRITE` effect segment and its LEIR semantic
contract. Stop treating further io_uring setup-flag tuning as the next primary
compiler/runtime milestone.

`submit_all` and `coop_taskrun` both preserved correctness and remained below
the mechanism stop boundary in every measured cell. However, two clean full
runs on the same machine selected different recommended profiles. That result
is useful Linux evidence, but it is not stable enough to choose a production
default or justify a new ring lifecycle.

The next primary experiment is a generated portable compiled-continuation
backend using the same LEIR meanings and generated-module ABI shape. The Linux
io_uring backend remains the specialized reference implementation. This moves
LLAM toward a reusable runtime backend for C and other language compilers
without turning a Linux flag result into a portable claim.

## Source and environments

Both accepted evidence bundles used the clean source revision
`00bb0d00b72e094a83dc57af4c1abbfab207db32` with source-tree digest
`2c13a3a26ccbfbea8088b051fa1628b1fb1cf5999c20f54d81e6cfc1f56fba3d`.
The benchmark binary SHA-256 was
`7af4c3ba48660fba817574dc59f3a86184e44f040c92017cdce71f5c317ae64f`.

The measured environment was:

- LinuxKit `6.12.76-linuxkit`, AArch64;
- Ubuntu 24.04 container, GCC 13.3.0;
- Python 3.12.3 and liburing 2.14;
- privileged local container so io_uring was available;
- loopback TCP and abstract Unix stream sockets;
- concurrency 1 and 16, payloads 64 and 4096 bytes;
- 256 activations and five fresh-process samples per candidate and cell;
- same-profile portable/native ABBA pairing.

The local-only create-once bundles were
`local-arm64-00bb0d0` and `local-arm64-repeat-00bb0d0`. Both recorded
`source_tree_dirty=false`. An earlier dry run whose container mount could not
resolve the linked-worktree Git metadata was excluded from this decision.

The supplied second Linux host could not be reached from the execution
environment: SSH failed before authentication with `Network is unreachable`.
Consequently this is not a two-machine reproduction and cannot satisfy a
release gate.

The companion macOS verification used macOS 26.5.1 on ARM64, Apple Clang
21.0.0, CMake 4.3.2, and Python 3.12.7.

## Profile results

Ratios are generated-native divided by the same-profile portable interpreter;
lower is better. Each available profile contributed 40 paired samples per
bundle. All eight matrix cells continued in both runs.

| Bundle | Profile | Capability | Verdict | CPU ratio | p99 ratio | Wall ratio |
|---|---|---|---|---:|---:|---:|
| first clean run | `submit_all` | available | `CONTINUE` | 0.520 | 0.279 | 0.192 |
| first clean run | `coop_taskrun` | available | `CONTINUE` | 0.404 | 0.219 | 0.155 |
| first clean run | `defer_taskrun` | unavailable | `UNAVAILABLE` | n/a | n/a | n/a |
| clean repeat | `submit_all` | available | `CONTINUE` | 0.474 | 0.189 | 0.172 |
| clean repeat | `coop_taskrun` | available | `CONTINUE` | 0.579 | 0.438 | 0.186 |
| clean repeat | `defer_taskrun` | unavailable | `UNAVAILABLE` | n/a | n/a | n/a |

The first clean run recommended `coop_taskrun`; the repeat recommended
`submit_all`. Across the two runs, the highest per-cell upper 95% bootstrap
wall-ratio bound was 0.617 for `submit_all` and 0.631 for `coop_taskrun`, both
well below the 1.05 mechanism stop boundary. The mechanism is therefore valid,
but the optional profile ranking is not repeat-stable.

`defer_taskrun` is unavailable by construction in the current topology.
Current kernels bind a non-disabled `SINGLE_ISSUER` ring to the task that calls
`io_uring_setup()`. LLAM creates rings on the runtime initialization task and
submits from its I/O worker, so the research selector returns `ENOTSUP` before
ring creation. This prevents the observed `io_uring_enter(...)=EEXIST`
non-progress state. Supporting the profile would require worker-owned ring
construction and an explicit startup handshake.

## Cost attribution

Median native subphase shares across both accepted bundles were:

| Profile | Prepare / execute | Ring / execute | Resume / execute |
|---|---:|---:|---:|
| `submit_all` | 0.216% | 99.448% | 0.042% |
| `coop_taskrun` | 0.071% | 99.592% | 0.046% |

The generated module's prepare and resume work is already a small fraction of
the measured ticket. Almost all time attributed inside `execute_ns` is in the
native issue, completion, park, and resumption interval. Optimizing generated
slot preparation before changing the broader backend shape is therefore low
leverage.

This timing does not prove that kernel execution alone consumes 99% of CPU.
`aot_ring_ns` intentionally includes queueing, submission, kernel work,
completion drain, scheduling, and task resumption. Separating those consumers
requires a later tracing experiment.

## Structural and safety results

All 160 native rows across the two accepted bundles satisfied the complete
structural contract:

- one queue publication per activation;
- two prepared SQEs per activation;
- one visible terminal CQE per activation;
- one suppressed successful connect CQE per activation;
- one task park and one terminal wake per activation;
- zero hot allocations;
- matching portable/native peer checksum and successful correctness flag.

Focused Linux GCC and Clang/CMake contracts passed. The AOT plan, generated
module, standalone C consumer, integration, Linux unit, ownership, strict ring
profile, benchmark parser, and smoke benchmark tests all passed. ASan and
UBSan passed the same seven AOT contract targets. On macOS, the full stable
`check` and full research test set passed after guarding the Linux-only timing
helper and correcting the Make unittest entry point.

Repository license, C-structure ratchet, build-manifest, shared-export,
hardening, production test-hook, package-boundary, and installed-contract
audits passed. Research executables are now ignored so a build cannot pollute
source provenance or the source-tree digest.

## Why the decision is `NARROW`

The control mechanism is healthy, so `STOP` would discard useful work. A
stable optional-profile win was not reproduced, so `CONTINUE` would overstate
the setup-flag result. `NARROW` preserves the Linux implementation and cost
evidence while ending this flag comparison as the primary line of work.

No runtime default, public option, ABI promise, package content, version, tag,
or release behavior changes as a result of this experiment.

## Next compiler/runtime experiment

Build a backend-neutral generated portable continuation for the same frozen
`CONNECT -> WRITE` LEIR program:

1. keep LEIR as the compiler/planner semantic contract;
2. compile static control flow and typed continuations ahead of time;
3. issue effects through a portable LLAM backend interface without a
   per-completion opcode interpreter;
4. preserve independent completion ownership, cancellation, one terminal
   wake, and zero hot-path allocation;
5. make the generated module callable from the standalone C fixture so other
   language compilers can target the same runtime contract;
6. compare generated portable execution with the portable interpreter before
   adding another Linux effect shape.

Keep `submit_all` as the Linux control profile. Retain `coop_taskrun` only as
an optional research comparison until a second recorded Linux machine shows a
repeat-stable win. Revisit worker-owned ring construction and
`defer_taskrun` only as a separate lifecycle experiment after the portable
compiled backend establishes the general runtime-backend position.
