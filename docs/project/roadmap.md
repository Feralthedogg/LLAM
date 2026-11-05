# Roadmap

LLAM's long-term goal is to become a **general-purpose asynchronous execution
backend** that applications, frameworks, and language runtimes can embed. The path
starts with the task runtime in **3.0.0, public ABI 2.4**, and extends its execution,
I/O, cancellation, and resource contracts to more kinds of hosts.

The milestones below describe planned work. Their order expresses dependencies and
priority; it does not assign release dates or promise features in a particular
version. For what can be used today, follow the
[embedding guide](../guides/embedding.md) and
[platform support matrix](../start/platforms.md).

## Long-term goal

A host should be able to delegate qualified CPU work, asynchronous I/O, timers,
and foreign blocking work to LLAM through one execution backend. The host keeps
its application model, language semantics, and lifecycle authority. LLAM provides
execution progress, operation results, cancellation, and bounded resource use
through documented interfaces.

The intended integration has three parts:

| Part | Responsibility |
| --- | --- |
| Host adapter | Translate the host's submission, completion, cancellation, memory, and thread rules into LLAM's contracts. |
| LLAM core | Schedule admitted work, park waits, manage runtime-owned resources, and expose results and diagnostics through the C ABI. |
| Native backend | Implement the supported operations and execution profiles using the operating system's I/O and native-thread facilities. |

Language adapters should share these contracts and conformance tests. Each adapter
must state which code LLAM can preempt, where foreign code runs, and how its memory
remains valid. The managed C qualification in 3.0.0 does not qualify another
language's stacks, garbage collector, exceptions, or JIT-generated code.

The first integration target is an independently driven LLAM runtime connected to
a host event loop. A host-driven scheduler pump, additional language execution
profiles, and new kernel or device backends require their own designs and evidence.

## Current guarantees

These are delivered foundations in 3.0.0, within their documented platform and
calling-context limits:

| Foundation | Current contract |
| --- | --- |
| Embedding and ABI | Explicit runtime handles, independent lifecycle and ownership checks, caller-sized structures, and ABI major 2 compatibility. |
| Asynchronous execution | Default constructors require compiler/image admission and a qualified native profile. Explicit `*_compat` constructors retain boundary/safepoint execution. |
| Native I/O | Linux io_uring/liburing, macOS kqueue, and Windows IOCP, with documented direct paths and blocking-helper fallbacks. |
| Work composition | Task groups, opt-in linked cancellation, deadlines, task results, partial-transfer helpers, and task-local cleanup. |
| Resource control | Per-runtime worker, blocking-worker, task, and blocking-job limits, resource snapshots, and bounded host drain. |
| Diagnostics and verification | Runtime dumps, direct regression tests, platform CI, stress, sanitizer/fuzz gates, benchmarks, and soak runs. |

The [ABI contract](../reference/abi/index.md),
[threading rules](../reference/abi/threading.md), and
[I/O model](../concepts/io-model.md) remain the source of current guarantees.
Cancellation and drain timeouts preserve the documented ownership of outstanding
work and its memory. Requested preemption quanta are not hard scheduling deadlines.

Standard release archives contain the runtime SDK. The LLVM 23.1.2 managed producer
is built separately; the [managed C guide](../guides/managed-c.md) describes its
requirements. Runtime/backend support and native asynchronous qualification are
separate matrices. In particular, ordinary MSVC and Intel macOS builds do not
establish a qualified asynchronous profile.

## Milestones

The immediate priorities are a dependable execution contract and a reusable host
integration. Broader adapters build on those foundations.

| Stage | Status | Deliverable | Dependency |
| --- | --- | --- | --- |
| 1. Runtime stabilization | Next; extends the delivered core | Repeatable qualification and failure behavior for every claimed profile. | 3.0.0 baseline |
| 2. Host integration | Planned | Nonblocking submission and completion delivery for an external host loop. | Stage 1 contracts |
| 3. Backend operation contracts | Planned | Shared operation and memory-lifetime conformance across native backends and host adapters. | Stage 1; develop alongside stage 2 |
| 4. Resource control and fairness | Planned | Bounded queues and measurable progress under mixed workloads. | Stages 2 and 3 |
| 5. Ecosystem adapters | Planned | Supported host/language integrations using the same core contracts. | Stages 1–3; resource limits from stage 4 |
| 6. Broker isolation | Optional extension of the internal foundation | A versioned process-boundary contract with explicit authority policy. | Stages 1–3 and separate security qualification |

## Runtime stabilization

**Stage 1.** Extend release confidence while preserving ABI major 2, compatible
option prefixes, and the existing handle and lifetime rules.

- Keep reducing shutdown, cancellation, lost-wakeup, select, helper, and I/O ownership
  failures into direct runtime regression tests. Synchronize test prerequisites on
  observable state and elapsed deadlines.
- Broaden concurrent multi-runtime verification, including shared signal/fault-hook
  lifetimes and proof that stopping one runtime leaves its peers operational.
- Verify the matching producer, admitted image, runtime, and relocated installed SDK
  on each claimed managed profile. Native Windows LLVM installation, MSVC asynchronous
  execution, and Intel macOS asynchronous qualification remain separate work items.
- Keep requested/effective policy and refusal reasons observable. Unknown native state
  must be refused before capture; compatibility execution remains an explicit choice.

**Complete when:** each promoted profile passes its native context, ownership,
failure-injection, shutdown, SDK, and performance gates from reproducible inputs.
Unsupported profiles must report a documented refusal and its reason. Dumps must
identify the owner and outstanding work in a failed drain or hang.

## Host integration

**Stage 2.** Let a host keep its existing event loop while LLAM executes work on an
independently driven runtime. The current `llam_runtime_run_handle()` lifecycle is
the starting point; a general host-loop submission/completion interface is planned.

- Define nonblocking submission, admission failure, completion notification, and the
  execution context on which the host consumes each result.
- Specify queue capacity, wakeup coalescing, callback reentrancy, cancellation races,
  and ownership of callback arguments and result storage.
- Start with a host-owned driver thread. Evaluate a bounded host-driven pump only
  after its progress, thread-affinity, and lifecycle rules can be verified.
- Provide a complete C host example that starts multiple runtimes, serves requests,
  handles partial setup failure, and shuts down with outstanding work.

**Complete when:** two independent runtimes can serve one host loop without blocking
that loop on LLAM waits. Tests must cover full queues, completion before registration,
coalesced wakeups, cancellation versus completion, and host shutdown. Accepted work
must have one defined terminal outcome, and teardown must prevent callbacks into
released host state.

## Backend operation contracts

**Stage 3.** Give host adapters a consistent account of operation progress and
memory ownership across readiness, completion, and blocking-helper paths.

- Specify the common lifecycle for submission, partial progress, cancellation,
  completion, and retirement. Distinguish a visible result from the point at which
  all backend access to retained memory has ended.
- Extend capability reporting so an integration can distinguish native support,
  a documented helper path, and an unsupported operation before choosing its policy.
- Keep descriptor/HANDLE ownership, close and reuse, buffer alignment, EOF, error
  translation, and timeout behavior explicit. Preserve platform-specific capabilities
  where they affect the caller's decision.
- Build a shared backend conformance suite before considering a public backend
  extension interface. Kernel-specific queue formats stay behind their adapters.

**Complete when:** every supported backend and helper path passes the same applicable
operation/lifetime cases, plus its native-specific tests. Partial results remain
available on failure, late completions cannot reuse released storage, and cancellation
cannot authorize freeing memory still used by an entered foreign callback.

## Resource control and fairness

**Stage 4.** Build on the limits already present in 3.0.0 so an embedding host can
control a mixture of CPU work, I/O, timers, and foreign calls.

- Account for queued submissions and completions, in-flight I/O, retained buffers,
  prepared execution capacity, and blocking helpers under explicit budgets.
- Propagate backpressure through host adapters. Define overload and retry behavior
  for each bounded queue and helper pool.
- Measure scheduling delay, timer delay, I/O latency, cancellation latency, and drain
  progress alongside throughput. Keep preemption request budgets distinct from
  observed native delivery and execution delay.
- Preserve per-runtime isolation and diagnose which workload or owner prevents
  progress. Tune batching, wakeups, and worker counts using measured contention.

**Complete when:** mixed-load tests demonstrate progress for qualified CPU tasks and
I/O/timer work under the declared limits. Overload remains bounded and recoverable;
timeout retains the resources needed for safe cleanup. Existing correctness and
performance gates remain satisfied under matching build and workload settings.

## Ecosystem

**Stage 5.** Make the same execution backend usable from more hosts through supported
adapters and reproducible SDKs.

- Establish the C host integration first, including C++ hosts using the C ABI and
  explicit foreign-call boundaries. Keep application object models in their hosts.
- Evaluate Rust and Python adapters as initial language-integration candidates.
  Define how each maps submission, completion, cancellation, errors, and retained
  memory to its host runtime before declaring it supported.
- Require each adapter to state its thread-affinity, garbage-collector or ownership,
  exception/unwind, and shutdown rules. Foreign code runs only through boundaries
  its host permits. Native asynchronous preemption of that language's code needs
  separate compiler/runtime cooperation and qualification.
- Keep Go, JVM, .NET, and JIT execution integrations exploratory until those contracts
  and native verification paths exist. Each new preemptible execution profile needs
  matching compiler/runtime evidence in addition to its C ABI adapter.
- Publish versioned compatibility information, installed-SDK examples, and an adapter
  conformance suite. Promote adapters according to real embedding needs and evidence.

**Complete when:** an external language/event-loop consumer can use the backend without
private symbols or a fork of the core. Its tests cover cancellation during foreign
work, retained-memory release, callback context, host shutdown, and ABI negotiation.
Installation must be reproducible using the supported public packages and tooling.

## Broker isolation

**Stage 6, optional.** Extend the existing internal broker foundation for hosts that
need a process boundary. This work can progress independently of language bindings;
ordinary in-process embedding does not require a broker.

The current foundation includes subject-bound tokens, attenuation and revocation,
buffer/channel/descriptor grants, POSIX `SCM_RIGHTS`, Windows `DuplicateHandle`,
private ring setup, bounded batched serving, and predefined task commands.

- Define a versioned runtime RPC surface and production policy for descriptor and
  HANDLE authority before making broker symbols public.
- Carry cancellation, quotas, completion ownership, and client-disconnect cleanup
  through that protocol. Define what remains live after revocation or a client crash.
- Expand malformed-client, authority, crash-recovery, and sustained ring tests.

**Complete when:** the protocol and authority policy are documented and verified
against those failure cases, with resource accounting and peer isolation preserved.
The [security model](../reference/security/index.md) remains explicit: in-process
opaque handles harden misuse, while hostile clients require a process boundary and
appropriate OS isolation.

## Platform and performance

These workstreams apply across the milestones. Runtime/backend support and managed
execution qualification advance independently, with their evidence recorded in the
[platform matrix](../start/platforms.md).

- Linux: reduce io_uring request allocation, CQE handoff, poll wake, and timer overhead
  against a matching Linux baseline.
- macOS: preserve kqueue and Mach context correctness while measuring timer batching,
  helper handoff, and poll wake costs.
- Windows: maintain IOCP socket/HANDLE correctness and improve handoff and batching
  under the separately qualified compiler and OS profiles.
- Scheduler and channels: measure lock contention, wake storms, reclaim costs, timer
  locality, and select fanout before replacing primitives; retain lost-wakeup tests.
- BSD: support ended in 3.0.0; keep retired targets rejected by current tooling.
- RISC-V and additional execution or I/O backends: exploratory. Cross-builds and
  emulation are starting checks; native context, ABI, lifetime, and performance
  evidence are required before support or release artifacts are claimed.

Keep existing throughput and latency gates. Use matching source inputs, build settings,
worker counts, workloads, and host conditions, and retain commands and raw samples.
Lossless workloads require exact delivery; best-effort workloads require accounted-for
drops. Go/Tokio comparisons are useful where the workload and semantics match.

`scripts/bench_guard.py` remains a catastrophic-regression gate. Scheduled comparison
artifacts remain the source of public performance tables. See
[Benchmarks](../production/benchmarks.md) and
[Verification](../production/verification.md) for the measurement and release policy.

## Documentation

- Keep delivered features, planned interfaces, and exploratory profiles visibly
  distinct. Update the API, ABI, support matrix, and examples when a milestone ships.
- Document host integration from installation through failure and final cleanup,
  including size-aware FFI structures and dynamic ABI checks.
- Keep platform-specific verification commands and diagnostics discoverable from the
  main navigation. New adapters need equivalent installed-consumer checks.
- Preserve compatible ABI 2 extensions where possible. Any incompatible contract
  needs an explicit ABI-major decision and migration plan.

Claiming the general-purpose backend role requires at least a native C application
and an independent language/event-loop host to share these public contracts across
the supported platforms, with bounded resource use, defined cancellation and memory
lifetime, and repeatable correctness and performance evidence. Expand the adapter
set as these guarantees remain reproducible.
