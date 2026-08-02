# LEIR Linux Native Segment Design

Status: approved internal falsification experiment

Date: 2026-07-27

## Decision

Retain LEIR as an immutable compiler-to-runtime semantic contract, but do not
execute its effects through the Phase 0A per-completion interpreter. Compile a
strictly linear static region once into a Linux `io_uring` segment, submit every
SQE in that segment together, and resume the LLAM task only after the segment
has one terminal outcome.

The first experiment is deliberately Linux-specific. A positive result is
reported as `SPECIALIZED`, never as a portable runtime result. The experiment
does not create a public ABI, change the default runtime policy, bump version
`2.2.0`, or authorize a release.

## Why This Hypothesis Is Different

Phase 0A removed most task publications and context switches but retained one
backend submission and one CQE for every effect. It then interpreted the next
node after every completion. Stable core cells ran at roughly `0.55x` baseline
wall throughput while consuming roughly `1.37x` baseline CPU.

This experiment removes the remaining boundary traffic:

- one compiled segment replaces per-completion node dispatch;
- one backend queue publication represents the whole segment;
- every SQE is made visible in one ring submission;
- `IOSQE_IO_LINK` moves static sequencing into the kernel;
- `IOSQE_CQE_SKIP_SUCCESS` removes successful intermediate CQEs;
- one task park and one terminal wake represent the whole segment.

The hypothesis is falsified if that larger boundary move cannot achieve the
existing category-sized performance target.

## Approaches Considered

### Standalone liburing benchmark

A standalone ring would be quick to build, but it would hide LLAM's task park,
node queue, worker, lifetime, and completion dispatch costs. It could prove
that `io_uring` links work without proving that LLAM can use them
competitively.

### Add links to the Phase 0A interpreter

This would retain the failed generic instance state machine and make it harder
to attribute any result. It would also invite dynamic graph behavior into a
kernel mechanism that only represents static ordering.

### Private native-segment path in the real LLAM Linux backend

This is the selected approach. A small portable compiler recognizes one
eligible LEIR shape. A Linux-only backend object owns the already-lowered
operations, submission tokens, and terminal request. The existing node worker
submits it and the existing task wait protocol parks and wakes the caller.

This approach measures the intended product boundary while keeping the
experiment private and removable.

## Supported Semantic Envelope

The first compiler accepts exactly one linear success path:

- one, two, four, or eight I/O nodes;
- one or more fixed-length `WRITE_ALL` effects, with an optional
  `READ_EXACT` only as the final effect;
- no receive whose success edge names another I/O effect;
- every success edge names the next node or the final `RETURN`;
- every EOF and error edge names a final `FAIL`;
- all fd, buffer, and length slots are known at bind time;
- no result slot controls a later fd, buffer, length, or branch;
- no timeout, cancellation, fork, join, callback, parser, or native escape;
- one final result is published to the waiting task.

At bind time every descriptor must be a connected `SOCK_SEQPACKET` socket.
The instance atomically duplicates each distinct descriptor with
`F_DUPFD_CLOEXEC`, validates the duplicate, and owns it until explicit
instance destruction. Closing or reusing the caller's descriptor therefore
cannot retarget an already-bound segment.

Every message has exactly the declared length. Message-oriented send semantics
are required because successful intermediate CQEs are not observable in skip
mode. A receive is permitted only as the terminal operation, whose CQE remains
visible and can therefore be length-checked. Stream sockets, datagram
truncation, nonterminal receives, and any other operation class that permits
an unobservable short success are rejected.

The backend maps semantic reads to `IORING_OP_RECV` and writes to
`IORING_OP_SEND`. Writes retain `MSG_NOSIGNAL`.

This envelope is intentionally narrow. It tests whether backend-native
compilation can be a performance moat before generalizing the representation.

## Architecture

### Portable LEIR compiler

`experiments/leir/leir_native_plan.h` and
`experiments/leir/leir_native_plan.c` consume the already validated Phase 0
program. Compilation copies only the static operation shape and slot indices
into a compact plan:

```c
typedef enum leir_native_step_kind {
    LEIR_NATIVE_STEP_RECV = 0,
    LEIR_NATIVE_STEP_SEND = 1,
} leir_native_step_kind_t;

typedef struct leir_native_step {
    uint16_t kind;
    uint16_t fd_slot;
    uint16_t buffer_slot;
    uint16_t length_slot;
} leir_native_step_t;

typedef struct leir_native_plan {
    leir_native_step_t steps[8];
    uint16_t step_count;
    uint16_t result_slot;
} leir_native_plan_t;
```

The compiled plan does not retain a generic node table and cannot branch.
Compilation occurs outside timed execution. Binding validates concrete
resources and materializes one backend-native segment without allocation.

### Linux native segment contract

`src/io/linux/runtime_io_segment_linux_internal.h` defines the private
contract between the experiment and the Linux worker:

```c
#define LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS 8U

typedef enum llam_linux_native_op_kind {
    LLAM_LINUX_NATIVE_OP_RECV = 0,
    LLAM_LINUX_NATIVE_OP_SEND = 1,
} llam_linux_native_op_kind_t;

typedef enum llam_linux_native_segment_mode {
    LLAM_LINUX_NATIVE_SEGMENT_LINK = 0,
    LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP = 1,
} llam_linux_native_segment_mode_t;
```

An instance owns:

- one to eight concrete native operations;
- one aligned completion token per operation;
- a pointer to the task's active `llam_io_req_t`;
- activation and operation generation values;
- submitted, completed, and terminal state;
- the first failing operation and normalized error;
- counters for SQEs, submit batches, observed CQEs, suppressed success CQEs,
  task parks, terminal wakes, and hot-path allocations;
- an intrusive next pointer used only while queued to one node.

The type and all entry points remain private. No symbol is added to installed
headers or the shared-library export set.

### Node queue and feature state

Each Linux I/O node receives a private native-segment head and tail protected
by the existing `submit_lock`. Queue admission:

1. validates runtime and node ownership;
2. rejects reuse while a segment is queued or inflight;
3. increments `pending_ops` once for the complete segment;
4. publishes the segment to the queue;
5. wakes the node worker.

Normal ring creation uses `io_uring_queue_init_params` so the runtime records
the kernel feature bitmap. Skip mode requires
`IORING_FEAT_CQE_SKIP`. Link-only mode remains available as a mechanism
control when skip is unavailable.

### SQE preparation

The worker checks `io_uring_sq_space_left()` before acquiring any SQE. If the
ring cannot hold the complete chain, it first submits existing entries and
rechecks. It never consumes a partial chain.

For operation `i < operation_count - 1`:

```text
LINK mode:       IOSQE_IO_LINK
LINK+SKIP mode:  IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS
```

The final operation has neither flag. Every SQE carries the address of its
aligned operation token under a new Linux completion tag.

All linked SQEs are prepared before the worker calls the existing ring submit
helper. Linked requests are therefore contiguous and part of the same submit
call.

### Completion protocol

Link-only mode observes one CQE per SQE. CQEs for a linked chain are ordered,
so the owner records the first non-cancel error, validates successful lengths,
and completes the segment after the final CQE.

Link+skip mode follows the kernel contract for a normal soft-linked chain:

- if every intermediate operation succeeds, those CQEs are omitted and the
  final operation produces the one observed CQE;
- if an intermediate operation fails, that error CQE is visible, the remaining
  soft-linked requests are not executed, and their CQEs are omitted.

The token identifies each visible operation. A visible intermediate failure is
therefore terminal immediately; waiting for a synthetic cancellation tail
would deadlock. The owner retains the first non-cancel error, validates the
segment generation, claims terminal state once, decrements `pending_ops` once,
and completes the segment's task request through the existing LLAM completion
and wake path.

No completion executes LEIR node dispatch. There is no userspace resubmission
between effects.

## Error Handling

Compilation fails with `EINVAL` for branching, cycles, dynamic result
dependencies, unsupported opcodes, invalid terminal edges, or an unsupported
operation count.

Binding fails with:

- `EINVAL` for invalid slots, lengths, buffers, or descriptors;
- `ENOTSOCK` for non-sockets;
- `EPROTOTYPE` for sockets other than `SOCK_SEQPACKET`;
- `ENOTCONN` for unconnected sequence-packet sockets;
- `EOVERFLOW` for lengths that do not fit an SQE;
- `EBUSY` for an active instance.

Submission fails with:

- `ENOTSUP` when the selected mode requires unavailable CQE-skip support;
- `EXDEV` for runtime or node ownership mismatch;
- `EAGAIN` when a complete chain cannot fit after flushing the ring;
- the existing fatal backend error when the ring has entered terminal failure.

An intermediate kernel error reports its operation index and error. Downstream
soft-linked cancellation is not reported as the root failure. A stale token,
generation mismatch, duplicate terminal CQE, counter underflow, or completion
from a foreign runtime fails closed and records a runtime fatal error.

Cancellation is not implemented in this falsification screen. The private
submit API rejects a pre-canceled request, and the benchmark joins every
segment before runtime shutdown. Public stop/cancel does not yet own the native
queue and operation tokens; that is an explicit promotion blocker even if the
performance screen passes.

## Benchmark

The Linux benchmark uses an external process and connected Unix
`SOCK_SEQPACKET` pairs. For each activation the server prepares one distinct
record per operation and sends all records to the peer. The peer receives,
validates, and checksums each record. This isolates linked submission and CQE
traffic without mixing a platform-specific result with request/response
protocol effects.

Three modes use identical payloads and peer behavior:

1. `task` — an ordinary LLAM task calls the existing write path once per
   operation;
2. `link` — one private compiled segment submits the entire chain with
   `IOSQE_IO_LINK`, retaining all CQEs;
3. `link_skip` — the same segment suppresses successful intermediate CQEs.

The matrix covers:

- operation counts 1, 2, 4, and 8;
- concurrency 1, 64, and 512;
- payloads 64, 1024, and 16384 bytes;
- balanced ABBA/BAAB order;
- fresh peer and runtime processes for every paired sample;
- at least 100 ms per measured mode;
- five samples for the screen and nine only after a screen pass.

Each row records:

- wall and server-process CPU time;
- context switches, parks, wakes, and terminal latency;
- logical operations and prepared SQEs;
- ring submit calls and real submit syscalls;
- expected and observed CQEs;
- suppressed success CQEs;
- task resumes avoided;
- hot-path allocation count;
- peer identity, CPU scope, affinity, checksum, and path validity.

## Classification

The precommitted positive result is `SPECIALIZED`, not `CATEGORY`.

Required length-4 and length-8 cells at concurrency 64 and 512 with payloads
64 and 1024 must all achieve:

- wall throughput at least `1.50x` the task baseline;
- CPU ratio at most `0.70x`;
- terminal p99 degradation at most `1.10x`;
- paired-ratio spread at most `1.10x`;
- one backend queue publication and one task park per segment;
- one observed CQE per successful skip segment;
- zero hot-path allocation;
- matching payload and peer checksums.

Length-1 controls must retain at least `0.95x` wall throughput with CPU ratio
at most `1.05x`. Ordinary service-gap p99 degradation must remain at most
`1.10x`.

Use `REJECT` for a stable target miss. Use `INCONCLUSIVE` for missing kernel
features, invalid paths, incorrect results, allocation, insufficient duration,
or unstable paired evidence. Thresholds are not relaxed after observation.

## Test Strategy

Portable unit tests prove:

- eligible linear programs compile into the exact step sequence;
- adjacent sends are accepted and nonterminal receives are rejected;
- branching, cycles, result-dependent paths, unsupported terminals, and
  excessive lengths are rejected;
- binding rejects incorrect slot kinds and lengths before backend submission.

Linux unit tests prepare SQEs into test-owned storage and assert:

- opcode, fd, address, length, send flags, and user data;
- link flags on every non-final operation;
- skip flags only on successful-intermediate candidates;
- no flags on the terminal SQE;
- complete-chain capacity is checked before ring mutation.

Linux integration tests use real `SOCK_SEQPACKET` pairs and prove:

- link-only success observes every CQE and wakes once;
- link+skip success observes exactly one CQE and wakes once;
- an injected peer close identifies the first failed operation;
- invalid socket type and missing feature support fail before queue ownership;
- closing and numerically reusing a bound caller descriptor cannot retarget
  native traffic;
- explicit instance destruction releases every pinned descriptor;
- repeated instance reuse advances generation and rejects stale tokens;
- runtime pending-operation accounting returns to zero.

The branch must pass native tests, CMake portability, ASan/UBSan, TSan, the
existing full test matrix, shared-export audit, and the Linux research
workflow before evidence is classified.

## Security and Compatibility Boundary

The planner trusts no frontend descriptor. Bounds, types, topology, resource
class, lengths, and ownership are validated before native state is published.
User-controlled values never become SQE flags, opcodes, pointers, or
`user_data` tags without validation.

Buffers remain borrowed and owned by the caller for the instance lifetime.
Descriptors are private `CLOEXEC` duplicates owned by the instance and released
by `leir_native_instance_destroy`. The experiment does not expose raw kernel
pointers, does not add a public handle family, and does not weaken close
serialization.

Non-Linux backends receive no behavioral change. They compile private stubs or
omit the Linux-only target. A Linux-only positive result does not imply that
kqueue or IOCP should imitate linked SQEs.

## Source Semantics

The design relies on the documented normal soft-link and CQE-skip contract:

- linked requests execute sequentially and must be submitted together:
  <https://man7.org/linux/man-pages/man7/io_uring_linked_requests.7.html>
- `IOSQE_CQE_SKIP_SUCCESS` omits successful CQEs; on a soft-link failure the
  error CQE is posted while dependent requests are not executed and their CQEs
  are omitted:
  <https://man7.org/linux/man-pages/man2/io_uring_enter.2.html>
- support is advertised by `IORING_FEAT_CQE_SKIP`:
  <https://github.com/axboe/liburing/blob/master/man/io_uring_setup.2>

## Exit Decision

If the screen reaches `SPECIALIZED`, the next design adds whole-chain
cancellation, timeout, teardown, trace source maps, and a generated C-module
example while keeping the API private.

If the screen is a stable reject, retain LEIR only as a semantic integration
format. Do not replace the failed mechanism with another per-completion
executor, and do not version or release this research branch.
