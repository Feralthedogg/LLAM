# LEIR AOT Semantic-Barrier Design

**Date:** 2026-08-01
**Status:** Approved research direction; required before 3.0.0
**Scope:** Compiler/runtime contract and Linux specialization research; no
version bump or release authorization

## 1. Decision

LEIR remains LLAM's language-neutral semantic contract. The runtime will stop
treating a generic per-completion interpreter as the intended compiled path.
Eligible paths will instead be partitioned at semantic barriers and lowered to
backend-native AOT effect segments with direct prepare and resume entrypoints.

The first Linux validation target is:

```text
CONNECT -> terminal WRITE
```

The connect SQE is linked to the write SQE and suppresses its successful CQE.
The final write always retains a CQE. This gives the experiment one observable
completion on successful execution without hiding a result that LEIR needs.

The existing fixed-buffer `RECV -> SEND` pipeline is retained as negative
research evidence, not as a product path. It reduced queue publications,
submit syscalls, and successful CQEs, but its fixed-buffer copies and grouped
retirement barrier produced wall-time regressions across the measured matrix.
The 3.0.0 gate must not be weakened to promote it.

## 2. Why the current experiment stops

The latest Linux evidence shows two separate facts:

1. structural batching works: at width 8 and concurrency 16, queue
   publications reached 0.125 per activation and submit syscalls were about
   0.083 per activation;
2. the candidate still loses: successful measured cells retained one CQE and
   two SQEs per segment, while wall-time ratios ranged from roughly 1.35x to
   more than 12x the baseline.

The principal causes are architectural rather than a missing compiler flag:

- exact receive is not a safe soft-link success predicate because a short
  positive receive is kernel success but not `READ_EXACT` success;
- the fixed candidate copies into and out of page-aligned scratch storage;
- independent invocations share one terminal batch ticket and therefore wait
  for the slowest member before any owner resumes;
- submission was already coalesced by the ring worker, so semantic grouping
  adds a barrier without being required for syscall batching;
- the compact native plan is still walked and reduced at runtime rather than
  being a generated effect-specific module.

These results reject this candidate, not LEIR and not native lowering.

## 3. Alternatives considered

### 3.1 Continue tuning fixed `RECV -> SEND`

Rejected as the primary direction. Buffer registration and fixed files do not
remove the exact-read semantic mismatch. Removing copies would require a
different ownership-facing buffer API, and grouped retirement would still
penalize independent work.

### 3.2 Remove LEIR and expose io_uring directly

Rejected. That would make compiler frontends depend on one operating system,
prevent a stable cross-language semantic contract, and reduce LLAM to another
platform-specific executor.

### 3.3 Generate AOT segments separated by semantic barriers

Selected. It preserves portable semantics, permits direct backend code, and
uses kernel linkage only where its success and dependency rules are equivalent
to the source program.

## 4. Semantic partition contract

The compiler classifies every edge between effect nodes as one of:

```c
typedef enum leir_aot_edge_class {
    LEIR_AOT_EDGE_KERNEL_CHAIN = 0,
    LEIR_AOT_EDGE_COMPLETION_BARRIER = 1,
    LEIR_AOT_EDGE_GRAPH_BREAK = 2,
} leir_aot_edge_class_t;
```

`KERNEL_CHAIN` is legal only when all of the following hold:

- the predecessor has one kernel-visible success predicate;
- that predicate is identical to the LEIR success predicate;
- the successor does not need a hidden predecessor result as an operand;
- error and cancellation propagation are equivalent to LEIR control flow;
- the backend can retain an observable CQE at the segment's retirement fence;
- all referenced resources remain owned until that fence retires.

`COMPLETION_BARRIER` ends the current segment and resumes generated code with
the visible result. It is required for:

- `READ_EXACT` and `WRITE_ALL` loops;
- a nonterminal `READ` or `WRITE` whose short positive result affects a later
  operand or branch;
- data-dependent control flow;
- a resource or buffer ownership transition;
- any backend operation whose kernel and LEIR success predicates differ.

`GRAPH_BREAK` returns control to the portable scheduler. It is required for an
unsupported opcode, unbounded loop, dynamic fan-out, or backend capability
failure.

Partitioning computes maximal safe segments. It must never lengthen a segment
to meet a performance target.

## 5. LEIR connect semantics

Phase 0 gains an experimental `CONNECT` effect with this slot contract:

- `fd_slot`: an unconnected socket;
- `buffer_slot`: immutable bytes containing a complete socket address;
- `length_slot`: address length as `U64`;
- `result_slot`: `I64` containing `0` or a negative error;
- `on_success`: the next node;
- `on_error`: the error path;
- `on_eof`: unused and required to be `LEIR_PHASE0_NODE_NONE`.

The portable engine calls the existing public connect operation and preserves
the same result and cancellation behavior as any backend specialization.

The first native program is deliberately narrow:

```text
CONNECT(fd, address) -> WRITE(fd, immutable_payload) -> RETURN
         | error                         | error
         +-------------> FAIL <----------+
```

`CONNECT` is safe before another operation because its successful result is
exactly zero. The terminal `WRITE` keeps its own CQE, so a short positive write
remains visible and is handled according to ordinary `WRITE`, not
`WRITE_ALL`, semantics.

`CONNECT -> WRITE_ALL`, `CONNECT -> READ_EXACT`, and any path that consumes the
connect result as data remain separated by a completion barrier.

## 6. Generated module contract

The compiler emits C or object code implementing a versioned private ABI. A
module contains immutable metadata and direct functions; it does not contain a
node table for the runtime to interpret.

```c
typedef struct leir_aot_module_v1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t backend_kind;
    uint64_t semantic_digest;
    size_t instance_size;
    size_t instance_alignment;
    size_t slot_count;
    int (*bind)(void *instance, size_t instance_size,
                const leir_phase0_value_t *values,
                size_t value_count);
    int (*prepare)(void *instance,
                   const leir_aot_backend_v1_t *backend);
    int (*resume)(void *instance, uint32_t continuation,
                  int64_t result,
                  leir_aot_resume_result_v1_t *resume_out);
    int (*copy_outputs)(const void *instance,
                        leir_phase0_value_t *values_out,
                        size_t value_count);
    void (*cancel)(void *instance);
} leir_aot_module_v1_t;
```

The backend descriptor exposes a pattern-specific
`prepare_connect_write` entrypoint. Generated code calls it once with typed,
owned operands and continuation identities. It does not hand an opcode array
back to the runtime.

The generated connect-write fixture has:

- one cold `bind` function that validates and captures typed slots;
- one direct `prepare` function that invokes the Linux connect-write primitive,
  which emits exactly two SQEs;
- one generated resume entrypoint for the visible terminal CQE or early
  linked failure;
- no node dispatch loop, opcode switch, dynamic allocation, or result-table
  scan in its activation/completion path;
- an immutable semantic digest tying the module to the LEIR input and lowering
  profile;
- a standalone C consumer and a C++17 compile/link consumer that need public
  platform types but no runtime-internal header or runtime library linkage;
- the project `LicenseRef-LLAM-Commercial-Reciprocity-1.0` identifier emitted
  deterministically into every generated C and header artifact.

Generated C is the first interoperability proof because any frontend that can
emit C or link an object can use it. A later object emitter may replace the C
stage without changing the module ABI.

## 7. Linux lowering

The first specialized segment emits:

```text
SQE 0: IORING_OP_CONNECT | IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS
SQE 1: IORING_OP_SEND, final CQE retained
```

On success, only the final write CQE is observed. On connect failure, the
connect error CQE is observed and the linked tail is treated according to the
kernel's soft-link retirement contract. Every visible token is generation
checked before it can publish a result.

The specialization must fall back to the portable engine if io_uring, the
required opcode, CQE-skip semantics, or the module ABI is unavailable. The
fallback is a correctness path, not a benchmark exclusion.

## 8. Ownership and batching

Each independent LEIR invocation retains its own completion owner, terminal
claim, cancellation generation, and wake decision. The I/O node may drain many
owners into one submission batch, but it must not combine their semantic
retirement.

Grouped terminal tickets are legal only when the frontend program contains an
explicit `join_all`-equivalent barrier. Merely arriving in the same worker
drain is not such a barrier.

This separates two concerns:

- **submission batching:** ring-wide, opportunistic, and invisible to program
  semantics;
- **semantic joining:** program-owned and represented in LEIR.

## 9. Ring profiles

Ring flags are evaluated as separate profiles after the base lowering is
correct:

1. current `IORING_SETUP_SUBMIT_ALL` baseline;
2. `IORING_SETUP_COOP_TASKRUN` plus task-run feature validation;
3. `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN` only after one
   issuer owns both submit and wait operations.

No profile result may be attributed to AOT lowering unless the same profile is
used for its baseline. A profile that changes ownership constraints is a
separate experiment, not an unconditional optimization.

## 10. Measurement contract

The connect-write screen uses controlled `AF_UNIX` and loopback TCP peers and
records at least:

- wall time and CPU time per completed invocation;
- p50 and p99 latency;
- SQEs, visible CQEs, submit calls, submit syscalls, queue publications, parks,
  wakes, cancellations, stale completions, and hot allocations;
- portable interpreter, generated portable module, and generated Linux module
  as separate candidates;
- short-write, connection-refusal, cancellation, peer-close, and fallback
  correctness controls.

The mechanism may continue to broader testing only if it reduces successful
CQEs as designed, has zero hot allocations, preserves every control result,
and does not regress wall time by more than 5% in the initial screen.

The 3.0.0 promotion gate remains stricter:

- at least 1.50x wall-time improvement on the declared target workload;
- candidate CPU ratio no greater than 0.70;
- p99 latency ratio no greater than 1.10;
- short-workload wall ratio at least 0.95 and CPU ratio no greater than 1.05;
- zero correctness, sanitizer, stress, cancellation, or fallback failures;
- results reproduced on at least two Linux machines with recorded kernels,
  CPUs, toolchains, and liburing versions.

Linux evidence is reported as `SPECIALIZED`. It does not establish a portable
runtime win.

## 11. 3.0.0 release gate

Version 3.0.0 remains blocked until all of these exist in the repository and
pass CI:

1. a versioned LEIR semantic contract including explicit barrier rules;
2. a real generated AOT module with no per-completion opcode interpreter;
3. at least one external/compiler-facing C fixture that builds and runs;
4. portable fallback equivalence across supported platforms;
5. Linux connect-write specialization meeting the promotion gate;
6. cancellation, generation, ownership, sanitizer, and stress coverage;
7. reproducible benchmark evidence with portable and specialized verdicts
   separated;
8. completed license-transition and distribution metadata checks.

A partial implementation, a passing microbenchmark, or a Linux-only result is
not release authorization. No version file, tag, package, release workflow, or
release note is changed by this research phase.

## 12. Non-goals

- replacing the public LLAM I/O API in this phase;
- making arbitrary LEIR graphs one io_uring chain;
- promoting the fixed `RECV -> SEND` candidate;
- using multishot receive, provided-buffer rings, zero-copy send, or bundled
  send/receive before the first connect-write mechanism screen;
- claiming general portable performance from Linux-only measurements;
- releasing 3.0.0.

## 13. Primary references

- [io_uring_enter(2): linked requests and CQE skip](https://www.man7.org/linux/man-pages/man2/io_uring_enter.2.html)
- [io_uring_linked_requests(7)](https://man7.org/linux/man-pages/man7/io_uring_linked_requests.7.html)
- [liburing io_uring_setup(2)](https://github.com/axboe/liburing/blob/master/man/io_uring_setup.2)
- [liburing networking guidance](https://github.com/axboe/liburing/wiki/io_uring-and-networking-in-2023)
- [LLVM coroutine lowering](https://llvm.org/docs/Coroutines.html)
- [Monoio runtime architecture](https://github.com/bytedance/monoio)
- [Tokio-uring runtime architecture](https://github.com/tokio-rs/tokio-uring)
