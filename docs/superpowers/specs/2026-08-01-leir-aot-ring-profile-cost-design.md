# LEIR AOT io_uring Profile and Cost-Attribution Design

**Date:** 2026-08-01
**Status:** Approved research continuation
**Scope:** Linux-specialized measurement only; no version bump, package, tag,
or release authorization

## 1. Decision

Continue the successful generated `CONNECT -> WRITE` mechanism by measuring
where its remaining concurrency-16 CPU cost is spent and whether supported
io_uring task-work profiles change that cost.

The experiment adds three strict ring profiles:

1. `submit_all`:
   `IORING_SETUP_SUBMIT_ALL`;
2. `coop_taskrun`:
   `IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN`;
3. `defer_taskrun`:
   `IORING_SETUP_SUBMIT_ALL | IORING_SETUP_SINGLE_ISSUER |
   IORING_SETUP_DEFER_TASKRUN`.

Every generated-native sample is compared only with a portable-interpreter
sample created under the same requested profile. Unsupported profiles are
recorded as unavailable and never silently downgraded. A profile with a
correctness failure or a greater-than-5% mechanism regression is rejected
without changing the verdict of another profile.

This experiment remains Linux-specialized. It does not establish a portable
runtime win and cannot authorize LLAM 3.0.0.

## 2. Why this is the next experiment

The first AOT screen already proved the central mechanism:

- two SQEs are prepared for each activation;
- the successful connect CQE is suppressed;
- one terminal CQE and one terminal wake remain;
- no hot-path allocation occurs;
- portable and generated-native results have identical peer-observed output.

The remaining uncertainty is narrower. Some concurrency-16 samples exceeded
the CPU and p99 promotion limits even though aggregate wall time was strongly
positive. Extending the compiler ABI or accepting more effect shapes before
attributing that cost would mix frontend, generated-module, ring, and
completion behavior.

This phase therefore isolates the cost first. The result chooses the next
branch:

- if module bind/prepare/resume dominates, improve the generated ABI and move
  directly to the generated portable backend;
- if queue/submit/completion dominates and a safe profile wins, retain that
  profile as Linux-only evidence;
- if neither changes the conclusion, stop tuning ring flags and build the
  portable compiled-continuation backend next.

## 3. Alternatives considered

### 3.1 Build the generated portable backend immediately

This most directly advances LLAM as a language-runtime backend, but it would
change the AOT ABI before the remaining cost is attributed. Defer it by one
bounded experiment, not indefinitely.

### 3.2 Generalize to arbitrary kernel-linked chains

Longer static chains could suppress more successful CQEs. They also expand
the error, cancellation, and resource-lifetime matrix before the two-effect
baseline is fully understood. Do not generalize in this phase.

### 3.3 Attribute cost and compare strict ring profiles

Selected. It answers the current evidence gap with the smallest semantic
change. The portable LEIR program, generated module, completion ownership,
and CQE-suppression contract remain unchanged.

## 4. Strict profile selection

Profile selection exists only in research-enabled Linux builds. The benchmark
passes one exact profile through a research-only process setting before
runtime initialization. Normal builds and research builds without that
setting keep the existing setup and fallback behavior.

The selector is a small, independently tested Linux research component. It
maps the exact profile spelling to setup flags and reports:

- `EINVAL` for an unknown or malformed profile;
- `ENOTSUP` when the build headers do not expose every required flag;
- `EINVAL` when an explicit profile is combined with SQPOLL, because that
  would confound this experiment;
- a strict flag set and canonical name on success.

When an explicit profile is selected, ring creation is exact. Kernel
`EINVAL` or `EOPNOTSUPP` becomes profile unavailability; the runtime does not
retry with fewer flags. Other setup failures retain their original error.

`defer_taskrun` additionally requires the ring creator to be the dedicated
submit-and-wait worker in LLAM's current non-disabled-ring setup. Current Linux
kernels bind a `SINGLE_ISSUER` ring to the task calling `io_uring_setup()` when
the ring is created without `IORING_SETUP_R_DISABLED`; a different task then
receives `EEXIST` from `io_uring_enter()`. LLAM currently creates rings during
runtime initialization and submits from a later I/O worker, so this profile is
reported as `ENOTSUP` before ring creation instead of entering a non-progress
state. `submit_all` and `coop_taskrun` remain compatible with this topology.

Evaluating `defer_taskrun` requires a separate worker-owned ring-construction
experiment with a startup handshake, probe and resource registration on the
worker, and explicit initialization-failure propagation. That architectural
change is outside this bounded flag-and-cost comparison.

## 5. Cost attribution

Each benchmark sample advances to schema version 2 and includes:

- `ring_profile`: the exact strict profile;
- `bind_ns`: aggregate time spent binding activation values;
- `execute_ns`: aggregate time inside the portable run or generated ticket
  run, excluding socket construction and payload generation;
- `aot_prepare_ns`: generated module preparation time;
- `aot_ring_ns`: time from native-segment issue through terminal completion
  and task resumption;
- `aot_resume_ns`: generated resume plus output-copy time.

The three `aot_*` fields are zero for the portable interpreter and positive
aggregate diagnostics for generated-native samples. For generated-native
samples their sum must not exceed `execute_ns`. These clocks are diagnostic:
the existing process wall, CPU, and p99 fields remain the performance
criteria.

The instrumentation deliberately keeps ring work combined as `aot_ring_ns`.
Splitting queue wait, the shared submit syscall, kernel execution, CQ drain,
and scheduler wake would require assigning one shared batch timestamp to many
independent segment owners. That is a separate tracing experiment and is not
needed to distinguish generated-module overhead from runtime/kernel overhead.

## 6. Profile-isolated classification

The screen matrix adds `ring_profile` to cell identity and pairing. A portable
sample and generated-native sample can be paired only when profile, family,
concurrency, payload, and activation count all match.

Each profile receives one of:

- `CONTINUE`: every required cell preserves counters and correctness, and the
  native wall-time confidence interval does not cross the 5% stop boundary;
- `REJECT`: a completed cell violates correctness, structural counters, hot
  allocation, checksum, or the wall boundary;
- `INCOMPLETE`: samples are missing or confidence straddles the boundary;
- `UNAVAILABLE`: every attempted sample reports the same supported skip class
  before producing a result.

The control profile is `submit_all`. Its existing mechanism criteria remain
authoritative. Optional profile rejection or unavailability does not rewrite
the control result. Evidence records a per-profile verdict and a recommended
profile; recommendation requires `CONTINUE` and is ranked by same-profile
native CPU ratio, then p99 ratio, then wall ratio.

The release gate remains separate and always sets `release_authorized` to
false. A profile result from one machine cannot make the gate ready.

## 7. Components and boundaries

### Linux research selector

`src/io/linux/research_ring_profile.c` and its private header own spelling,
compile-time capability checks, setup-flag composition, and strict setup error
normalization. They contain no benchmark policy.

### Ring initialization

`src/io/engine/io_engine.c` reads the research request and performs exact ring
creation before the existing SQPOLL/default branches. With no explicit
request, its behavior is byte-for-byte equivalent at the API level to the
current runtime.

### Generated ticket metrics

`experiments/leir/leir_aot_linux.c` owns generated-module subphase timing.
The benchmark owns outer bind and execute timing. No timer or profile field is
added to the public LLAM API or to production runtime layouts.

### Evidence driver

`scripts/bench_leir_aot_connect.py` owns profile matrices, pairing,
classification, metadata, CSV/JSON projections, and the human-readable
summary. It must never infer that a missing optional profile is a portable
failure.

## 8. Error and fallback behavior

- Unknown profile: benchmark usage failure, exit 2.
- Profile absent from build headers: benchmark skip, exit 77.
- Profile incompatible with the current ring creator/submitter topology:
  benchmark skip, exit 77.
- Profile rejected by kernel: benchmark skip, exit 77 with the exact profile
  in the diagnostic.
- Runtime correctness or ownership failure after initialization: failed
  sample, never a skip.
- A profile that is unavailable for only one candidate or changes
  availability during a cell: `INCOMPLETE`, not `UNAVAILABLE`.
- No explicit profile: existing runtime fallback remains unchanged.

This distinction prevents a real native failure from being mislabeled as a
kernel capability gap.

## 9. Testing and evidence

The test sequence is:

1. selector table tests for exact flags, names, unsupported headers, malformed
   values, and SQPOLL rejection;
2. Python parser and classifier tests for schema 2, profile identity, cost
   invariants, profile isolation, and recommendation ranking;
3. Linux portable/native smoke runs for every supported profile;
4. the frozen TCP/Unix, concurrency 1/16, payload 64/4096 matrix with paired
   same-profile samples;
5. sanitizer and existing ownership/cancellation regressions;
6. stable-build, license, manifest, structure, and packaging-boundary audits.

Evidence records the source revision, tree digest, binary digest, kernel,
CPU, compiler, liburing version, requested profiles, per-profile capability,
and all raw samples. Local results remain research evidence until reproduced
from clean revisions on separate recorded Linux machines.

## 10. Licensing and distribution

Every new code-bearing file uses
`LicenseRef-LLAM-Commercial-Reciprocity-1.0`. Existing Apache-licensed files
retain their notices. The research selector and AOT targets are compiled only
when research mode is enabled on Linux. Default packages must contain no new
research symbol or object.

## 11. Stop conditions

Stop or narrow the experiment if any of the following occurs:

- a profile requires submit or wait calls from more than one task per ring;
- exact setup cannot be distinguished from a silent fallback;
- same-profile pairing cannot be proven in raw evidence;
- cost instrumentation changes structural counters or causes more than a 5%
  regression in the control screen;
- cancellation, generation, ownership, sanitizer, or stable-build tests fail;
- a change would require a public API or ABI promise in this research phase.

## 12. Non-goals

- selecting a production-default io_uring profile;
- claiming portable AOT performance;
- adding longer effect chains or new LEIR opcodes;
- changing semantic barriers or completion ownership;
- adding SQPOLL to the profile matrix;
- completing the generated portable backend in this phase;
- changing a version, tag, package, or release workflow;
- releasing LLAM 3.0.0.

## 13. Primary references

- [io_uring setup flags](https://www.man7.org/linux/man-pages/man7/io_uring_setup_flags.7.html)
- [io_uring_setup(2)](https://man7.org/linux/man-pages/man2/io_uring_setup.2.html)
- [io_uring_submit_and_wait(3)](https://man7.org/linux/man-pages/man3/io_uring_submit_and_wait.3.html)
- [Linux io_uring setup and issuer enforcement](https://github.com/torvalds/linux/blob/master/io_uring/io_uring.c)
- [liburing networking guidance](https://github.com/axboe/liburing/wiki/io_uring-and-networking-in-2023)
