# Ownership transition tables

These tables are the review contract for wait ownership, I/O wait ownership,
and Linux native-segment retirement. A transition not listed here must fail
closed or be added here together with a focused race test before production
code accepts it.

## Runtime public handle and owner pins

Explicit runtime handles are encoded slot, generation, and family tokens. Public
objects retain the raw runtime storage through an owner pin so their cleanup
state remains valid after the token and scheduler have been destroyed.

| From | Event | To | Required condition |
|---|---|---|---|
| unregistered storage | runtime registration | live token, owner refs `R` | fresh runtime generation and nonzero handle secret |
| live token, refs `R` | public object publication | live token, refs `R + 1` | owner acquire wins before destroy is claimed |
| live token, refs `R` | destroy claim | claimed live token, refs `R` | active public runtime operations drain |
| claimed live token, refs `R > 0` | finalization | invalid token, retired storage, refs `R` | token removal, scheduler-state reset, secret preservation, and retired publication share one registry critical section |
| claimed live token, refs `0` | finalization | invalid token, freed heap storage | explicit heap runtime only |
| retired storage, refs `R > 1` | public object cleanup | retired storage, refs `R - 1` | decrement under the runtime registry lock |
| retired storage, refs `1` | final public object cleanup | freed heap storage | no later raw owner access is permitted |

The process-default runtime uses stable raw storage rather than a token. Its
owner count survives shutdown. Reinitialization is rejected while any object
from the prior incarnation remains; once that count reaches zero, a fresh
pre-initialization owner epoch may begin.

## Task wait resolver gate

`refs` is the low-bit resolver claim count. `CLOSED` is the publication gate
bit. Closing and draining happens before owner pointers may be cleared or
recycled.

| From | Event | To | Required condition |
|---|---|---|---|
| `OPEN(refs)` | resolver begin | `OPEN(refs + 1)` | count below saturation |
| `OPEN(refs)` | owner close | `CLOSED(refs)` | owner teardown begins |
| `CLOSED(refs)` | resolver end | `CLOSED(refs - 1)` | `refs > 0` |
| `CLOSED(0)` | owner publish | `OPEN(0)` | every owner field is initialized |

Resolver begin while closed is rejected. Resolver end at zero, counter
saturation, and publish from any state other than `CLOSED(0)` fail closed.

## Task wait generation and owner

The owner tuple comprises the active wait node/queue/select state, I/O request
and operation generation, blocking job, join target, parked shard, wait reason,
deadline, and cancellation registration.

| From | Event | To | Ordering requirement |
|---|---|---|---|
| no owner, generation `G` | prepare | no owner, generation `G + 2` | close/drain, advance to invalidate old owner, then reserve fresh generation |
| prepared owner | publish | active owner at current generation | publish all tuple fields before reopening resolver gate |
| active owner | completion, timeout, cancellation, or teardown | no owner, generation `G + 1` | close/drain, advance generation, then clear raw pointers |
| active owner, no deadline | arm deadline | active owner plus deadline | timer insertion succeeds under owner-shard lock |
| active owner plus deadline | disarm or expiry | active owner, no deadline | remove or pop under the same shard lock |
| active owner, unregistered | register cancellation | active owner, registered | token lock validates that cancellation has not already won |
| active owner, registered | unregister or resolve | active owner, unregistered | unlink under token lock before owner recycling |

Generation wrap, a resolver that cannot drain, or partial publication is fatal
because safe owner reuse can no longer be proved.

## I/O request wait modes

Watch modes are distinct owner domains even though abort handling groups them.
Rehome may change the owner shard or node without changing the wait mode.

| From | Event | To |
|---|---|---|
| `NONE` | publish ordinary request | `SUBMIT_QUEUE` |
| `SUBMIT_QUEUE` | backend submits SQE/operation | `INFLIGHT` |
| `SUBMIT_QUEUE` | detach, setup rollback, cancellation, or timeout before submit | `NONE` |
| `INFLIGHT` | terminal completion and request retirement | `NONE` |
| `INFLIGHT` | cancellation or timeout | `INFLIGHT` until backend retirement, then `NONE` |
| `NONE` | publish poll watch | `POLL_WATCH` |
| `NONE` | publish accept watch | `ACCEPT_WATCH` |
| `NONE` | publish receive watch | `RECV_WATCH` |
| any watch mode | completion, close, cancellation, timeout, or setup rollback | `NONE` |

An abort must first claim the task wait generation and request operation
generation. A `SUBMIT_QUEUE` abort detaches from exactly one queue. An
`INFLIGHT` abort queues backend cancellation while retaining request lifetime.
A watch abort removes the waiter under its watch lock. Observing an unrelated
mode or generation is a stale producer and performs no owner mutation.

## Linux native segment

| From | Event | To |
|---|---|---|
| `IDLE` | batch activation | `QUEUED` |
| `QUEUED` | atomic SQ publication | `INFLIGHT` |
| `QUEUED` | activation or publication rollback | `IDLE` |
| `QUEUED` | queued batch cancellation | `RETIRED` |
| `INFLIGHT` | semantic completion before every kernel reference retires | `RETIRING` |
| `INFLIGHT` | final CQE also supplies semantic completion | `RETIRED` |
| `RETIRING` | final CQE or cancel CQE retires remaining references | `RETIRED` |

`RETIRED` is terminal for an activation. Reuse requires a later explicit
reconfiguration that resets the segment to `IDLE` with a fresh generation.

## Linux native batch

| From | Event | To |
|---|---|---|
| `IDLE` | validate and enqueue | `QUEUED` |
| `QUEUED` | every segment is published atomically | `INFLIGHT` |
| `QUEUED` | enqueue/publication rollback | `IDLE` |
| `QUEUED` | cancellation removes queued batch | `RETIRED` |
| `INFLIGHT` | terminal result claimed before all segments retire | `RETIRING` |
| `INFLIGHT` | all segments retire with terminal result | `RETIRED` |
| `RETIRING` | final segment and cancellation reference retire | `RETIRED` |

## Linux native cancellation

| From | Event | To |
|---|---|---|
| `NONE` | inflight cancel request accepted | `QUEUED` |
| `QUEUED` | cancel SQEs published | `SUBMITTED` |
| `QUEUED` | target retires before cancel submission is needed | `RETIRED` |
| `SUBMITTED` | every cancel CQE observed and target retired | `RETIRED` |
| `NONE` | queued batch removed without kernel references | `RETIRED` |

Batch completion may wake the task only after semantic terminal ownership is
claimed once. Storage may be recycled only after both target state and cancel
state are retired.
