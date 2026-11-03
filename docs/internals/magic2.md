<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: Apache-2.0
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
See LICENSES/Apache-2.0.txt.
-->

# Internal magic2 Integration

`src/internal/magic2.h` supplies adaptive kernel dispatch, immutable execution
plans, owned bindings, prepared batches, and graphs. LLAM builds one private
implementation in `src/core/api/magic2_api.c`. Internal C callers use
`src/internal/runtime_magic2.h`, which exposes `llam_magic2_dispatch()` for tagged
request operations and typed `llam_magic2_cpu_*()` helpers for direct managed
CPU work. Neither header is installed, and raw `magic2*` functions have
translation-unit-local linkage. The LLAM public headers, ABI version, exports,
and default scheduling policy are unchanged.

## Tagged Dispatch and Results

Inside a managed task, the bridge submits the request through
`llam_call_blocking_result()`. The G parks, and LLAM's existing blocking worker
executes the request. Unmanaged callers execute synchronously. Call without
runtime locks; scheduler context, incomplete managed TLS, runtime-critical,
preemption-disabled, and announced foreign regions are rejected with `EBUSY`.
Native callbacks run without the originating G, P, scheduler TLS, or native
thread affinity. Callbacks and configurations must support that execution
domain, including any required floating-point settings.

The bridge reports three distinct results:

| Result | Meaning |
| --- | --- |
| Return value and `errno` | LLAM submission/completion or cancellation result |
| `*dispatch_status` | The magic2 result; `MAGIC2_ENOTPROCESSED` if it never ran |
| `request.implementation_status` | Kernel result for operations that define it |

An invalid magic2 envelope is a completed dispatch with a magic2 error; it is
not a LLAM submission failure. If cancellation prevents the callback from
starting, dispatch status stays `MAGIC2_ENOTPROCESSED`. If cancellation reaches
an executing request, the existing blocking path waits for its callback to
return before reporting `ECANCELED`. The completed magic2 result, created
handles, and prior buffer writes remain visible. Inspect those results and
release any created handles even when LLAM reports cancellation. The bridge
does not retry a request.

Keep the request, result, referenced descriptors, callback state, and buffers
live until return. A magic2 asynchronous submission has its own handle and
buffer lifetime after dispatch returns; this bridge does not drain the entire
asynchronous operation. Context/graph mutation and destruction still obey
magic2's serialization and ownership contracts. A request waiting indefinitely
inside native code can delay cancellation and shutdown, just like another
blocking-pool callback.

## Direct Managed CPU Plans

`llam_magic2_cpu_plan_create()`, `llam_magic2_cpu_plan_get_info()`,
`llam_magic2_cpu_plan_retain()`, and `llam_magic2_cpu_plan_release()` make the
typed immutable CPU-plan lifecycle available to internal callers. They return
magic2 results directly and retain its owner callbacks and exact-shape rules.
Creation requires `MAGIC2_CPU_SCHEDULE_CALLER` and `worker_limit = 1`; static
and dynamic native-pool schedules are refused with `MAGIC2_EUNSUPPORTED`
before retaining callback state. A caller schedule with another worker limit
fails magic2's validation. As in the supplied API, CPU plans require the
explicit partial-failure contract:

```c
magic2_cpu_plan_spec spec = MAGIC2_CPU_PLAN_SPEC_INIT;
spec.schedule = MAGIC2_CPU_SCHEDULE_CALLER;
spec.worker_limit = 1;
spec.flags = MAGIC2_CPU_PLAN_ALLOW_PARTIAL_FAILURE;
spec.failure_contract = MAGIC2_FAILURE_PARTIAL;
/* Supply the exact shape, grain, tile callback, environment, and scratch. */
```

Call `llam_magic2_cpu_run()` inside an ordinary LLAM task. It executes tiles
directly in that G and slot zero of its validated workspace. The caller's
current P remains the execution authority. The bridge yields between tiles,
including when automatic preemption is disabled, and rechecks CPU features and
floating-point requirements after resumption on a possibly different worker.
The original binding and status preflight checks still validate spans,
alignment, and aliases before ZERO initialization or callback execution.

One activation uses at most one P. Run independent activations in separate
LLAM tasks to use more of the runtime's configured P budget; each activation
needs exclusive workspace and writable regions. This path starts no native
threads, submits no blocking-pool job, and adds no runtime-owned executor or
cleanup hook. Plans can outlive a runtime and be reused by another runtime
when their callback state supports it. An activation retains the plan across
all tile yields, and its live G keeps its runtime alive until it returns.

```c
/* In a managed task, with a live plan and exclusive bindings/workspace. */
magic2_cpu_run_status status = MAGIC2_CPU_RUN_STATUS_INIT;
int run_status = MAGIC2_ENOTPROCESSED;
int rc = llam_magic2_cpu_run(plan, regions, region_count,
                            workspace, workspace_bytes, &status, &run_status);
/* Inspect rc/errno, run_status, and status.implementation_status separately. */
```

`run_status` is required and must be disjoint from every other argument's
storage. The detailed `status` is optional and follows magic2's initialization
and alias rules. Unmanaged, scheduler, foreign, and unsafe managed contexts
fail with `-1/EBUSY` before work begins. Shape or status validation failures
are completed invocations: return value zero and a magic2 error in
`run_status`, with no tile calls or ZERO initialization.

| Event | LLAM result | magic2 result and effects |
| --- | --- | --- |
| Cancel or stop before admission | `-1/ECANCELED` | `ENOTPROCESSED`; output ports and detailed status unchanged |
| All tiles succeed | `0` | `OK`, complete tile count |
| Kernel fails | `0` | `EIMPLEMENTATION`, failed tile and kernel result; earlier writes remain |
| Cancel or stop after admission | `-1/ECANCELED` | `ECANCELLED`, cancelled state; completed tiles and prior writes remain |
| Kernel fails while cancellation is observed | `-1/ECANCELED` | `EIMPLEMENTATION` and the original kernel result take precedence |

Cancellation is observed at tile boundaries. Every started tile returns before
the bridge reports cancellation; subsequent tiles are not started after it is
observed. No tile is retried and no adaptive training runs on this path. Keep
the regions, workspace, result storage, and callback state alive until the
activation returns. If the containing task's join is cancelled, that join is
not completion and does not permit freeing those resources.

Tiles must be bounded CPU work. They must not block, wait, start nested native
pools, retain port/scratch pointers, require native-thread affinity, or leave
runtime or foreign guards unbalanced. A long tile or an initial large ZERO
fill can delay other Gs, cancellation, and shutdown. A host destroy waits for
an executing tile and its G to drain; it does not interrupt arbitrary C code.
Plan owner retain/release callbacks also need to support their calling domain.

## Selective Automatic Policy for Internal Work

`llam_magic2_cpu_work_create()` and `llam_magic2_cpu_auto()` provide a private
execution-policy hook for explicitly described internal buffer transforms or
zero kernels. The descriptor supplies a baseline callback, one to eight
equivalent exact-shape CALLER plans, nonzero operation/semantic identities,
and an estimated cost. An internal producer must establish the buffer,
ownership, bounded-tile, and cancellation contracts. The runtime does not
derive these facts from an arbitrary task function. These functions and their
descriptors remain in the non-installed private header and hidden shared
symbols; no public ABI or runtime option is added.

The default minimum estimated cost is 50 microseconds. Smaller or unknown
work calls the baseline directly after the managed-context/cancellation check,
without retaining plans, reading pressure, or entering the family. This fast
path borrows the descriptor: its producer must keep the descriptor and baseline
state live through return, including callback yields. The benchmark owns its
descriptor until all its tasks drain. Heavier activations additionally retain
the descriptor and its family through completion.

For potentially heavy work, advisory runtime snapshots select three buckets:
quiet, runnable backlog, or I/O/timer pressure. The latter two raise the cost
floor to 2x and 4x respectively, limiting policy overhead while other work is
waiting. The budget is always one existing G/P slot. No native CPU executor or
thread pool is created. Estimates and floors are policy hints supplied by the
internal producer, not universal performance crossover measurements.

One family activation per descriptor may hold a nonblocking reservation.
Concurrent callers receive a pre-execution `EBUSY` admission result and execute
the baseline once; no G waits on the reservation. Its acquire/release ordering
publishes the family evidence between successive owners. Only its owner reads
or writes mutable family selection state, including while that G yields. The
raw native family runner is not called: the bridge reuses its selection and
observation helpers, and executes the selected member with the managed CPU
path. This preserves LLAM tile yields, cancellation, and domain checks.

Successful real activations contribute timing observations. Kernel failure
disables the member in that pressure bucket. Cancelled activations do not train
timing evidence. A failed or cancelled execution never retries another member
or falls back, including after partial writes. If selection finds no eligible
member before execution, the baseline may run once. Result metadata separates
the admission route/result, selected member, bucket, slot budget, and timing
observation from the ordinary LLAM and magic2 execution results. `UINT32_MAX`
marks a bucket that the below-floor path did not sample.

The current consumer is the checked-in internal benchmark descriptor. Existing
owned-buffer allocation is not routed through this hook: its ZERO initialization
does not currently observe task cancellation, so introducing partial cancellable
initialization would change that constructor's contract. No scheduler, waiter,
I/O-completion, allocator-critical, or arbitrary task callback is automatically
wrapped. A real internal producer must satisfy the same descriptor contract and
be measured before using this path. The benchmark currently has one equivalent
family member; it measures policy overhead and admission, not an optimized
kernel search. Regression tests also exercise a two-member family.

## Relationship to G–P–M and Preemption

Magic2's owning plan leases retain kernel environments; they do not grant LLAM
P ownership. The existing versioned execution lease, P ticket, and task run
generation remain authoritative. Magic2 CPU-feature eligibility and checked
buffer shapes do not establish async-preemption-safe instruction ranges.
Linux async capture continues to require LLAM's registered safe PC metadata;
other platforms retain their documented capability and fallback policy.

Adaptive tuning can execute repeatable candidates multiple times. Do not pass
scheduler mutations, I/O, or non-repeatable application effects as adaptive
candidates. Magic2's callback graph operations can execute effects once without
adaptive tuning; the integration regression uses this path. Native kernel
failure does not roll back previous writes. A sealed plan's direct `assume`
entry points require caller-proven spans, ownership, and execution conditions.

The tagged adaptive/profile/graph operations continue to use the blocking
bridge. Typed caller CPU plans use the direct path above. CPU family policy is
available only through the internal work descriptor. Native CPU tickets/executors,
sealed plans, and sealed graphs remain private
to the implementation translation unit. Supporting parallel tiles within one
activation or typed graph execution needs a separate scheduling and draining
contract. The global scheduler still owns task admission and P authority; the
selective descriptor is the only Magic2 execution-policy hook. Rust,
Python, JVM, .NET, and Go wrappers remain outside this integration.

## Direct-path benchmark

The checked-in experiment compares the same bounded tile kernel and the same
LLAM G/P yield and cancellation boundaries with and without the direct Magic2
plan path. It is deliberately outside the default build:

```sh
make -j4 bench_magic2_cpu
./bench_magic2_cpu manual 4096 64 20 1 32
./bench_magic2_cpu magic2 4096 64 20 1 32
./bench_magic2_cpu auto 4096 64 20 1 32
python3 scripts/bench_magic2_cpu.py --samples 9 --warmup 2 \
  --output /tmp/llam-magic2-cpu-comparison.json
```

Arguments are `elements grain iterations tasks inner_rounds`. The JSON result
reports elapsed work time, one-time plan setup, yields, blocking submissions,
automatic route counts, and an exact output checksum. The script checks equal
outputs, yields, and worker budgets for all three modes. Compare fresh paired
runs on an otherwise idle host; include plan setup separately when the plan is
not reused. The experiment is a path-cost comparison, not a claim that Magic2
accelerates an arbitrary LLAM task.

There is no safe transparent selector for arbitrary `llam_task_fn(void *)`
entries: the runtime cannot infer tile boundaries, buffer alias/ownership, a
repeatable kernel, or where cancellation may stop without changing its
semantics. Wrapping every task as one Magic2 tile adds overhead without useful
selection. The `llam_magic2_cpu_*` names are private, non-installed plumbing,
not public API additions. The selective route above accepts only an internal
work descriptor whose kernel and lifetime contract are already known.

## Source and Verification

The current `magic2.h` snapshot comes from the user-supplied attachment received
on 2026-09-21. Its contents match except for one final LF added by the repository
patch workflow. The stored snapshot's SHA-256 is
`b3f8388c6c45fe148ad31390e12e9930b5b448c320471be63bc1edc3a5ea3de5`.
The attachment, which has no final newline, has SHA-256
`2a8258e53959dde127dba5a9679c4a6058765f2f9066110937267d58ae0b3e4c`.
The previous source-checkout import had SHA-256
`515a67527e007d73d751f57ec53eaf83cd64f75c83eae430ffb527e60c7faa85`.
The refresh adds graph context ownership holds, broadens metadata-overlap
validation and batch isolation, documents async-handle contracts, and keeps
CPU-family/profile arithmetic bounded.
Public declarations and the LLAM bridge/auto interfaces are unchanged.
The supplied header has no copyright or SPDX notice. Apart from that final LF,
the integration preserves the supplied bytes and does not assign it a
license. Its origin and redistribution terms need to be established before
submission or distribution under the contribution policy.

The corresponding upstream repository is also vendored at
`third_party/magic2/`, pinned to commit
`b890268b2be97a73977c73a2110b5b5f0af65500`. Its upstream MIT license is
retained in `third_party/magic2/LICENSE`; see `UPSTREAM.md` there for the
provenance record.

The source-boundary checkers verify this exact header path and content hash
before exempting its independent `executor` vocabulary from LLAM authority
rules. A changed snapshot fails closed; a bridge with a missing snapshot also
fails. All LLAM bridge code still receives the ordinary boundary checks.

`test_magic2_internal` links the tagged-dispatch regressions and
`tests/test_magic2_cpu_internal.c` and `tests/test_magic2_auto_internal.c` in
Make, CTest, ASan/UBSan, and TSan builds.
It checks malformed requests, unmanaged dispatch, managed-only CPU admission,
runtime-critical/preemption-disabled refusal, exact spans and results, progress
with one P and automatic preemption disabled, kernel failure without replay,
cancellation before/between/during tiles, and failure concurrent with
cancellation. Direct CPU tests also cover retained plans across yields, six
activations sharing one P without blocking submissions, host destroy draining
an in-flight tile, and plan reuse on a peer runtime after the first is destroyed.
Automatic-policy tests cover thresholds and timer pressure, two-member family
selection, failure quarantine, immediate busy fallback, no replay, cancellation
before/during execution, retained descriptor lifetime, error-preserving cleanup,
and constructor failure cleanup with cleared workspace requirements.
Run the binary after building it:

```sh
make -j4 CC=clang test_magic2_internal
./test_magic2_internal
```

The test verifies LLAM integration behavior, not every algorithm, profile
format, or execution backend in the supplied header. A native pass covers
only the tested platform.
