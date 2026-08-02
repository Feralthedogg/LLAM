# LEIR Portable Compiled Executor Design

**Date:** 2026-08-02
**Status:** Approved research direction; required evidence for 3.0.0
**Scope:** Test-only portable execution, shared completion representation, and
Linux specialization comparison; no production promotion or release
authorization

## 1. Decision

LEIR remains the compiler and planner semantic contract. Eligible LEIR effect
segments are emitted as generated modules with direct bind, prepare, and resume
entrypoints. The normal compiled path must not walk an opcode table after each
completion.

The next executor has two implementations behind the same generated-module
contract:

1. a portable compiled Executor that calls LLAM's portable effect API directly;
2. the existing Linux io_uring adapter that lowers the same segment to linked
   SQEs and suppresses the successful intermediate CQE.

Both implementations publish the selected LCCF representation B: one immutable
48-byte normalized completion event. Continuation-site lookup and generated
module resume happen only when the terminal event is consumed.

The interpreter remains available as a semantic oracle and as the explicit
fallback for `GRAPH_BREAK`. It is not the intended per-completion backend for an
eligible compiled segment.

## 2. First bounded program

The first cross-backend generated program remains:

```text
CONNECT -> terminal WRITE
```

Its semantic rules are unchanged:

- connect failure resumes the connect error continuation;
- connect success advances to write without exposing a source-language value;
- write returns the partial byte count accepted by LEIR `WRITE`;
- a negative write result resumes the write error path;
- zero for a non-empty payload is an I/O failure;
- a result larger than the requested payload is a protocol failure;
- cancellation is terminal and cannot be converted into success;
- output slots are copied only after a successful generated resume.

This path is deliberately small enough to prove the contracts before adding
more effect shapes.

## 3. Goals

The experiment must establish all of the following:

- one generated module can bind to portable and specialized backends;
- the portable path has no opcode loop and no per-completion interpreter;
- representation B normalizes platform results once at terminal publication;
- the consumer resolves the continuation site at materialization time;
- generation, cancellation, module lifetime, and single-consumer rules hold;
- portable and Linux results are semantically identical to the interpreter;
- Linux CQE and park/wake reductions are measured separately from the portable
  compiled-path result;
- all research targets stay behind `LLAM_BUILD_RESEARCH=1` and are absent from
  packages.

## 4. Non-goals

This phase does not:

- change the public LLAM ABI;
- expose LEIR or the generated-module ABI as a stable SDK promise;
- replace all interpreter paths;
- claim that Linux-specific gains are portable;
- promote a new default runtime path;
- change the project version, create a tag, package 3.0.0, or publish a release;
- merge the held relicense branch.

## 5. Contract layers

The implementation has four layers, in dependency order:

```text
LEIR semantic segment
    -> generated backend-agnostic module
    -> portable or Linux effect adapter
    -> immutable 48-byte completion event
    -> common terminal consume and generated resume
```

### 5.1 LEIR planner

`leir_aot_plan_compile` remains authoritative for partitioning. Its edge
classes retain their current meanings:

- `KERNEL_CHAIN`: a backend may submit adjacent effects as one dependency
  chain because backend success is equivalent to LEIR success;
- `COMPLETION_BARRIER`: the next effect needs a source-visible result or a
  semantic check before submission;
- `GRAPH_BREAK`: generated execution stops and transfers to the interpreter.

The portable Executor may execute a `KERNEL_CHAIN` sequentially. The edge class
permits backend-native chaining; it does not require a kernel facility.

### 5.2 Generated module

The current prepare callback already carries backend-neutral values: descriptor,
address, payload, generation, and continuation identifiers. The research ABI is
corrected so the module advertises backend independence while each bound backend
advertises its concrete kind.

The module owns:

- copied stable inputs needed after bind;
- generation advancement;
- continuation-specific result validation;
- output slot mutation;
- cancellation state;
- a stable semantic digest.

The backend owns:

- effect issuance;
- platform result capture;
- terminal event publication;
- parking and waking;
- queueing and completion-source details.

No generated function may inspect an io_uring CQE, kqueue event, IOCP packet, or
platform errno convention directly.

### 5.3 Completion event B

The event layout is fixed for this experiment:

```c
typedef struct lccf_event_core {
    uint64_t generation;
    uint64_t stable_flags;
    int64_t result;
    uint64_t payload_word;
    uint32_t captured_home_shard;
    uint32_t source_node;
    int32_t error_code;
    uint8_t event_kind;
    uint8_t source_kind;
    uint16_t reserved;
} lccf_event_core_t;
```

The structure is exactly 48 bytes. Publication performs platform normalization
once and then treats the bytes as immutable. Materialization validates the
generation and resolves the requested continuation site. The event never owns
a raw platform completion pointer.

The selected LCCF implementation is reused as a research dependency. LEIR does
not create a second, subtly different event format.

### 5.4 Common terminal consumer

The terminal consumer performs this order:

1. acquire the published event;
2. validate generation and the single-consumer claim;
3. recheck cancellation and module availability;
4. materialize the continuation-specific fact;
5. invoke the generated resume function;
6. copy outputs when resume succeeds;
7. release backend, payload, and module ownership;
8. mark the ticket reusable or terminal.

Queue fallback uses the same materialization and resume functions. It may change
where consumption happens, but not the meaning of the event.

## 6. Portable compiled Executor

The portable Executor is a test-only ticket with caller-provided storage. It
uses the generated module directly and has no allocation after initialization.

For the first segment it executes:

```text
module.prepare(portable backend)
    -> llam_connect(fd, address)
    -> on success, llam_write(fd, payload)
    -> publish one terminal B event
    -> common consume
    -> module.resume(continuation, result)
    -> module.copy_outputs(...)
```

`llam_connect` and `llam_write` may each park the task. That cost is part of the
portable control measurement. The compiled Executor still avoids interpreter
dispatch between them: native C control flow selects the next operation.

Required ticket states are:

```text
INITIALIZED -> BINDING -> BOUND -> RUNNING -> PUBLISHED
            -> CONSUMING -> CONSUMED -> BINDING
            -> DESTROYED
```

Cancellation from `BOUND` calls the module cancel entrypoint. Cancellation
during `RUNNING` is observed before publication and again before consumption.
Destroy returns `EBUSY` while binding, running, published, or consuming.

## 7. Linux io_uring specialization

The existing Linux adapter remains the specialized reference implementation.
For a successful connect-write segment it submits:

```text
CONNECT: IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS
WRITE:   terminal CQE retained
```

The connect failure CQE is observable and selects the connect error
continuation. Successful connect does not produce CQE traffic. The write CQE is
the terminal retirement fence and selects the write result continuation.

The Linux adapter must publish the same B event and use the same terminal
consumer as the portable Executor. Platform-specific reducer logic may choose
the continuation and normalized result, but it may not directly call generated
resume.

Independent invocations keep independent ownership and terminal wakes. Ring
submission may coalesce tickets; retirement may not join them.

## 8. Fallback and oracle rules

The interpreter runs in three cases only:

- the planner emits `GRAPH_BREAK`;
- no compiled module matches the semantic digest;
- a backend reports unsupported capability before it takes effect ownership.

After a backend accepts ownership, errors are terminal results and cannot be
silently replayed through the interpreter. This prevents duplicate connect or
write side effects.

Every correctness fixture runs the same initial slots through:

1. the interpreter oracle;
2. the portable compiled Executor;
3. the Linux specialized Executor where available.

The comparison covers action, error, all output slots, observable peer bytes,
and ownership counters.

## 9. Required correctness matrix

The test matrix includes:

- success and partial write;
- refused connect;
- write failure after successful connect;
- cancellation before run and during an effect;
- timeout/cancel result normalization;
- stale generation and duplicate terminal delivery;
- invalid continuation and malformed platform result;
- queue fallback and direct consumption;
- module-unload deferral while a ticket holds a module reference;
- destroy and rebind races;
- two tickets completing out of order;
- every connect and write continuation transition;
- no allocation after bind for generated/module/event state.

Sanitizer, Windows, macOS, BSD, Linux x86_64, and Linux aarch64 builds must keep
the test-only boundary intact. Linux-only tests may skip only for documented
kernel or sandbox capability errors.

## 10. Evidence axes

Two verdicts are computed independently.

### 10.1 Portable compiled-path verdict

Compare the interpreter with the portable compiled Executor using the same
portable LLAM effects. Report:

- wall time and CPU time;
- p50 and p99 activation latency;
- interpreter dispatches per activation;
- normalizations and site lookups per activation;
- parks, wakes, and hot allocations;
- correctness and cancellation failures.

This verdict answers whether compilation and representation B improve the
generic runtime path. Linux SQE/CQE counts are excluded from this verdict.

### 10.2 Linux specialization verdict

Compare portable compiled B with Linux linked B. Report:

- SQEs, observed CQEs, and suppressed successful CQEs;
- queue publications and submit syscalls;
- parks and terminal wakes;
- wall, CPU, p50, and p99 ratios;
- correctness and fallback coverage.

This verdict answers whether io_uring linkage adds a platform-specific win. It
cannot rewrite a negative or inconclusive portable verdict.

## 11. Staged integration gates

The work advances in stages:

1. `MODEL`: generated module, B event, and fake backend tests only;
2. `PORTABLE`: real LLAM connect-write execution and oracle parity;
3. `LINUX`: linked SQE/CQE evidence with the same terminal consumer;
4. `INTERNAL`: an opt-in research selector may choose compiled execution;
5. `PRODUCTION_CANDIDATE`: considered only after all evidence gates pass.

Advancing a stage requires the preceding stage's correctness receipts. A stage
may be rejected without invalidating LEIR or the other backend.

Production consideration additionally requires:

- two independent Linux machines for Linux claims;
- complete cross-platform build and sanitizer coverage;
- no public ABI change;
- no short-workload regression;
- portable evidence reported separately and not worse than its gate;
- Linux evidence meeting its own structural and performance gate;
- explicit owner approval after the current 3.0.0 hold is lifted.

## 12. Alternatives rejected

### 12.1 Copy representation B into LEIR

Rejected. Two implementations would drift in malformed-result handling,
generation checks, and continuation lookup.

### 12.2 Keep the module Linux-only and emulate Linux in the portable path

Rejected. A backend identifier must describe the implementation actually
executing effects. Lying about it would make capability checks and evidence
unreliable.

### 12.3 Resume generated code directly from every platform completion

Rejected. It duplicates lifecycle, cancellation, queue fallback, and module
lifetime checks across backends and recreates the policy over-design the common
event is meant to remove.

### 12.4 Promote the current Linux path immediately

Rejected. The Linux mechanism has encouraging structural evidence, but the
portable compiled control and common B consumer have not yet completed their
cross-platform proof.

## 13. Success condition for this phase

This phase succeeds when one generated connect-write module passes oracle
parity through both adapters, representation B is shared rather than copied,
the portable path records zero per-completion interpreter dispatches, Linux
success records two SQEs and one observed CQE, and the two evidence verdicts are
published separately.

Success authorizes only the next research stage. It does not authorize a
version change or release.
