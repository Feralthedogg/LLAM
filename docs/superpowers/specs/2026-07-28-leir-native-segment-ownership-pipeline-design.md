# LEIR Native Segment Ownership, Pipeline, and Batch Design

**Date:** 2026-07-28  
**Status:** Approved for implementation by delegated maintainer judgment  
**Scope:** Experimental LEIR/Linux backend only; no public LLAM API or version
promise

## 1. Objective

Keep LEIR as the compiler/planner semantic contract while lowering eligible
effect paths to backend-native io_uring segments. This phase must answer a
harder question than the existing SEND-only experiment:

> Can LLAM safely own, cancel, retire, and batch useful RECV → SEND effect
> segments so that reduced publication, park/wake, file lookup, buffer mapping,
> and CQE traffic produce a measurable end-to-end gain?

The work is ordered by dependency:

1. complete cancellation and kernel-ownership retirement;
2. admit connected RECV → SEND paths;
3. add a real fixed-buffer receive and fixed-file path;
4. publish multiple segments through one batch ticket;
5. measure platform-specific gains separately from portable LEIR gains.

No release is justified merely because the code works. A version bump and
release require the correctness gates, repository CI, and the performance gates
in this document to pass.

## 2. Current gap

The existing native segment has one token per operation and wakes a task when
the reducer sees a semantic terminal CQE. That is insufficient for reuse:

- an earlier linked failure may be semantically terminal while later target
  CQEs can still reference operation tokens;
- an async-cancel CQE and the target request CQE are distinct and can arrive in
  either order;
- generic request cancellation targets request user-data, while native SQEs
  use per-operation token user-data;
- a returned instance currently resets its token storage immediately;
- one request, queue publication, pending owner, park, and wake are paid per
  segment even when the I/O worker submits several segments in one ring enter.

The Linux cancellation contract explicitly requires the application to handle
both the cancel request and the target request completion, without assuming
their ordering. See
[io_uring_cancelation(7)](https://man7.org/linux/man-pages/man7/io_uring_cancelation.7.html).

## 3. Requirements

### 3.1 Correctness

- A task, instance, fd duplicate, fixed-file slot, fixed-buffer slot, operation
  token, and batch ticket must not be reused while the kernel can still
  reference them.
- Cancellation must work while a batch is queued, detached for submission,
  partially submitted, in flight, completing, or racing with natural success.
- Every published batch owns exactly one `pending_ops` unit and releases it
  exactly once.
- Every batch parks and wakes its task at most once.
- A final non-skipped operation CQE is the target-retirement fence for each
  linked segment.
- If cancellation SQEs were submitted, the batch is reusable only after every
  cancel SQE has produced its own CQE as well.
- Runtime shutdown may use ring teardown as the ultimate kernel-retirement
  fence, but normal instance reuse may not.
- The portable interpreter and existing public runtime I/O semantics remain
  unchanged.

### 3.2 Workload

- The native planner must accept a bounded acyclic success chain containing
  RECV followed by SEND.
- EOF and error branches remain explicit terminal LEIR failures.
- The initial workload remains connected `AF_UNIX/SOCK_SEQPACKET`, which makes
  one message equal to one exact logical transfer in the controlled
  experiment.
- The fixed-resource candidate uses a registered buffer for RECV and a fixed
  file for both RECV and SEND.
- SEND continues to use `IORING_OP_SEND` with `MSG_NOSIGNAL`. The first phase
  does not use `WRITE_FIXED`, because its interface cannot carry
  `MSG_NOSIGNAL`, and does not use `SEND_ZC`, because its notification CQE adds
  a second lifetime protocol that would confound the CQE-reduction experiment.

Registered buffers only avoid mapping through fixed-buffer operations such as
`READ_FIXED`; registration alone is not counted as success. See
[io_uring_registered_buffers(7)](https://man7.org/linux/man-pages/man7/io_uring_registered_buffers.7.html)
and
[io_uring_prep_read_fixed(3)](https://man7.org/linux/man-pages/man3/io_uring_prep_read_fixed.3.html).
Fixed-file SQEs use a registered table index with `IOSQE_FIXED_FILE`, as
specified by
[io_uring_register(2)](https://www.man7.org/linux/man-pages/man2/io_uring_register.2.html).

### 3.3 Boundedness

- At most 8 operations per segment.
- At most 8 segments per batch ticket.
- At most 64 operation SQEs and 64 cancel SQEs per batch.
- No allocation in the activation, submission, completion, or cancellation
  hot path.
- Cold instance binding/attachment may allocate fixed scratch storage and
  duplicate descriptors.

## 4. Considered architectures

### 4.1 Dedicated ring per instance or compiled module

This makes ring-wide cancellation and resource registration simple. It is
rejected because it multiplies kernel rings and workers, prevents natural
coalescing across languages/modules, and moves LLAM away from a shared
concurrency-runtime backend.

### 4.2 Runtime-wide immutable benchmark tables

This pre-registers every benchmark fd and buffer before execution. It is easy
to measure but rejected as the product architecture because arbitrary embedded
language runtimes cannot know all resources at LLAM initialization, and table
ownership becomes benchmark-specific.

### 4.3 Shared ring with bounded batch tickets and sparse resource arenas

This is selected. Each node owns sparse fixed-file and fixed-buffer tables.
Instances attach cold resources to a node, while each activation publishes a
bounded batch ticket. Segment tokens remain unique, and semantic completion is
separate from kernel retirement.

This preserves LLAM's shared backend, supports unrelated frontends, and gives a
direct path from compiler batching decisions to runtime cost amortization.

## 5. Ownership model

### 5.1 Two terminal concepts

Each segment tracks:

- **semantic terminal:** the logical LEIR result is known;
- **target retired:** the final, deliberately non-skipped operation token has
  produced a CQE;
- **cancel retired:** every submitted async-cancel token has produced a CQE;
- **reusable:** target retired and cancel retired.

The task is completed only when every segment in its batch is reusable. This is
more conservative than waking at semantic terminal, but makes the initial
contract auditable and prevents a resumed task from destroying or rebinding
storage still reachable from the ring.

### 5.2 Segment state

The state machine is:

```text
IDLE -> QUEUED -> INFLIGHT -> RETIRING -> RETIRED -> IDLE
          |                       ^
          +---- queued cancel ----+
```

- `QUEUED` cancellation removes all segments belonging to the ticket under the
  node submit lock, marks them retired locally, releases the ticket's one
  pending unit, and lets the generic abort path reinject the task.
- `INFLIGHT` cancellation appends the ticket to an allocation-free native
  cancel queue.
- The worker always prepares operation SQEs before cancellation SQEs for the
  same ticket.
- The final operation never sets `IOSQE_CQE_SKIP_SUCCESS`; its CQE is the
  per-segment target-retirement fence on success, error, linked cancellation,
  and short/partial ring submission retry.

### 5.3 Cancellation tokens

Every operation keeps its existing aligned token. Every possible cancel SQE
gets a second aligned token embedded in the segment. Both carry:

- owner segment;
- activation generation;
- operation index.

Operation tokens use `LLAM_IO_UDATA_NATIVE_SEGMENT`; cancel tokens use a new
`LLAM_IO_UDATA_NATIVE_CANCEL` tag. The cancel SQE targets the encoded operation
token and reports through its cancel token. Generation checks reject stale or
duplicated CQEs.

A ticket records cancel SQEs prepared and cancel CQEs observed. `0`,
`-ENOENT`, and `-EALREADY` are control outcomes, not target-retirement evidence.
Only the final operation token retires target ownership.

### 5.4 Request integration

The embedded `llam_io_req_t` points to its active native batch ticket while the
wait is published. Generic wait cancellation dispatches by that pointer:

- native queued detach for `SUBMIT_QUEUE`;
- native cancel-ticket publication for `INFLIGHT`;
- existing request cancellation for ordinary I/O.

The pointer is cleared only after queued detach or full batch retirement.

## 6. Batch ticket

`llam_linux_native_batch_t` is caller-owned storage that remains live on the
parked task stack. It contains:

- one request owner;
- 1–8 segment pointers;
- node/runtime identity;
- one intrusive submission identity and one cancel-queue link;
- retired-segment and cancel counters;
- first semantic error by segment order;
- one-shot terminal claim.

`llam_linux_native_batch_enqueue()` validates all segments before mutating any
state, then appends the complete segment list under one lock, increments
`pending_ops` once, publishes one queue unit, and kicks once.

`leir_native_instance_run()` becomes a width-one wrapper around:

```c
int leir_native_batch_run(
    leir_native_instance_t *const *instances,
    leir_phase0_value_t *const *values_out,
    const size_t *value_counts,
    leir_native_metrics_t *metrics_out,
    size_t instance_count,
    leir_native_batch_metrics_t *batch_metrics_out);
```

All instances must be idle, bound, owned by the current runtime, and attachable
to the current I/O node. Acquisition is all-or-nothing and rolls back in reverse
order before publication.

The width-one and width-N paths use the same backend code, so batching is not a
benchmark-only shortcut.

## 7. RECV -> SEND lowering

The native planner no longer requires RECV success to return immediately.
Instead it accepts the same bounded success-chain grammar for RECV and SEND:

- success names the next effect or the final return;
- EOF and error name terminal failure nodes;
- result slots cannot be consumed later as fd, buffer, or length inputs;
- operation count remains 1, 2, 4, or 8.

For the first connected pipeline, RECV and SEND share one mutable buffer slot.
The linked SEND observes bytes written by the preceding RECV. The controlled
SEQPACKET peer sends exactly the compiled length; short transfers remain
`EMSGSIZE`.

## 8. Fixed resources

### 8.1 Node arena

When a Linux ring starts, LLAM attempts to create bounded sparse tables:

- 64 fixed-file slots;
- 64 fixed-buffer slots.

Failure disables only the fixed candidate. Normal native segments and all
portable paths remain available. Slot bitmaps are protected by a dedicated
node resource mutex.

### 8.2 Instance attachment

Binding duplicates distinct fds with `F_DUPFD_CLOEXEC` as before. A fixed-mode
instance also allocates anonymous scratch storage for each distinct LEIR buffer
slot.

On its first activation, the instance:

1. claims enough file and buffer slots on the current node;
2. updates sparse file slots with pinned duplicates;
3. updates sparse buffer slots with instance-owned scratch mappings;
4. rewrites native op descriptors to table indexes and scratch addresses.

The attachment persists across activations on that node. A different node
returns `EXDEV`; the compiler/runtime may choose a different instance or the
non-fixed candidate.

Before an activation, external input bytes needed by a SEND-before-RECV path
are copied into scratch. After retirement, buffers written by RECV are copied
back. These copies are part of measured candidate cost.

RECV is encoded with `io_uring_prep_read_fixed(..., offset = -1, buf_index)`.
SEND is encoded with `io_uring_prep_send(..., MSG_NOSIGNAL)` against the same
registered scratch address. Both SQEs use the fixed-file table index and
`IOSQE_FIXED_FILE`.

Detach clears sparse slots only after the instance is idle and its last ticket
is retired. Ring teardown is the fallback fence during runtime shutdown.

## 9. Submission and completion ordering

One worker pass performs:

1. ordinary backend controls;
2. native batch submissions;
3. native batch cancellations;
4. ordinary I/O requests;
5. one ring submit for all prepared SQEs where capacity permits.

Preparing operations before their cancels prevents a cancel from returning
`ENOENT` and then allowing a not-yet-visible target to be submitted afterward.
Short ring submits preserve the existing retry obligation; a ticket cannot
retire until its final target CQEs arrive.

Completion handling is allocation-free:

- operation CQE -> reduce semantic result, note final-target retirement;
- cancel CQE -> validate generation and note cancel retirement;
- reusable segment -> increment ticket retired count;
- final reusable segment -> claim ticket once and complete its one request.

## 10. Metrics and cost model

New counters distinguish logical work from ownership traffic:

- batch activations and segments;
- batch queue publications;
- operation SQEs and cancel SQEs;
- operation CQEs and cancel CQEs;
- target-retirement waits;
- fixed-file and fixed-buffer attachments;
- task parks and terminal wakes;
- ring submit calls/syscalls and entries;
- hot allocations.

For batch width `B`, the expected structural result is:

```text
queue publications / segments <= 1 / B
task parks        / segments <= 1 / B
terminal wakes    / segments <= 1 / B
```

CQE suppression is evaluated independently because fixed-buffer and
cancellation protocols may change CQE counts without changing task costs.

## 11. Validation

### 11.1 Deterministic tests

- planner accepts RECV → SEND and rejects cycles, alias misuse, invalid branch
  targets, and unsupported counts;
- queued cancellation removes an entire ticket atomically;
- in-flight cancellation covers cancel-before-target, target-before-cancel,
  partial cancel matches, linked failure, and duplicate/stale CQEs;
- semantic terminal never implies reuse before final-target and cancel
  retirement;
- batch width 1 and 2/4/8 use one pending owner, publication, park, and wake;
- fixed SQEs encode the correct file and buffer indexes;
- slot exhaustion and partial attachment rollback leak no fd, slot, mapping, or
  pending owner;
- destroy/rebind rejects live ownership.

### 11.2 Dynamic tests

- ASan/UBSan and TSan test binaries;
- cancellation stress with randomized cancel points;
- fd-number reuse after binding;
- peer close/error paths;
- runtime stop and ring teardown;
- no allocation in activation, submission, completion, or cancellation paths.

### 11.3 Linux evidence matrix

The platform-specific matrix compares:

1. public LLAM RECV + SEND baseline;
2. native width-one link;
3. native width-one link + CQE skip;
4. fixed RECV/fixed-fd link + CQE skip;
5. fixed RECV/fixed-fd batched link + CQE skip at widths 2/4/8.

At minimum it spans payload 64/512/4096 bytes and concurrency 1/4/16. Each cell
uses balanced order, warmup, repeated samples, raw artifact retention, and
confidence intervals. Portable LEIR/interpreter evidence remains a separate
report and cannot be used to claim a Linux backend win.

## 12. Decision gates

### Correctness gate

- all repository and sanitizer CI passes;
- zero ownership invariant failures across cancellation stress;
- zero pending owners, active I/O waiters, fd leaks, or fixed-slot leaks;
- exact peer payload/checksum agreement;
- structural batching counters match the model.

### Specialized-performance gate

A Linux candidate is `SPECIALIZED` only if:

- at least two non-trivial matrix regions show a lower 95% confidence bound of
  at least 5% wall-time improvement over the public baseline;
- median process CPU does not regress by more than 3%;
- terminal p99 does not regress by more than 10%;
- fixed mode beats the corresponding non-fixed native mode in at least one
  region without losing the correctness gate;
- width-4 or width-8 batching beats width-one native mode in at least one
  region and its publication/park/wake ratios meet the structural model.

Otherwise the result is `INCONCLUSIVE` or `REJECT`; the implementation may
remain experimental, but it does not justify a release.

### Release gate

Only after the correctness gate, full branch CI, and specialized-performance
gate pass may the project version be bumped and a release created. The release
notes must separate:

- portable LEIR/compiler progress;
- Linux/io_uring-specific performance evidence;
- unsupported kernels/resource limits and fallbacks.

## 13. Non-goals

- public stabilization of the native instance or batch API;
- TCP stream exact-read fusion;
- SEND_ZC notification ownership;
- multishot receive or provided-buffer-ring integration;
- cross-node migration of an attached fixed instance;
- claiming a general LLAM speedup from Linux-only evidence.

