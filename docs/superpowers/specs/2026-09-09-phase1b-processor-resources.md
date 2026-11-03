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

# Phase 1B Processor Resource Authority

## Status

Task 1 moved the immutable topology tuple and ordinary scheduler wake pair into
the stable processor cell and introduced the owner record as inert metadata.
Task 2A composes that record transactionally with the Phase 1A lease and
provides a validated, non-owning P ticket. Task 2B moved runnable queues and
timer heaps to P behind operation-boundary access validation. Task 3 moved the
allocator and processor-local stack caches in place and completed logical I/O
route auditing. The legacy lease remains the sole execution-publication
authority; no public ABI or stats surface changed. Task 3 is locally verified;
commit remains gated on the user's DCO sign-off.

## Scope

`llam_processor_t` retains its identity and compatibility fields:

- `runtime` continues to identify its runtime.
- `compat_shard` continues to identify the backing legacy scheduler shard.
- `id` remains the processor's stable shard-compatible index.

The processor cell owns its topology tuple (`cpu_id`, `node_index`, and
`io_node_index`), atomic `online` state, ordinary scheduler `event_fd` and
`event_pending` pair, versioned owner record, runnable queues/timer heap,
allocator, and processor-local stack cache. Runtime-wide stack fallback caches
stay on the runtime. `llam_node_t` continues to own I/O backends; the shard
retains opaque-helper wake state, task registry, public metrics, trace ring,
worker lifecycle, and Phase 1A lease coordinator. Allocation-owner IDs remain
distinct from task home/live placement, active timer-heap ownership, logical
I/O owner P, in-flight owner, and attached backend node.

No public header, exported symbol, statistics schema, or language wrapper
changes in this task.

## Accessor contracts

The private accessors in `runtime_state.h` define these rules:

| Accessor | Contract |
| --- | --- |
| `llam_processor_cpu_id` | Reads the CPU placement written during runtime initialization. The value is immutable until teardown and needs no scheduler lock. |
| `llam_processor_node_index` | Reads the immutable scheduler-locality index; no lock is required. |
| `llam_processor_io_node_index` | Reads the immutable logical I/O route; no lock is required. The `llam_node_t` backend remains node-owned. |
| `llam_processor_online` | Acquire-loads the online flag. Writers publish transitions with release stores through `llam_processor_set_online`. |
| `llam_processor_event_fd` | Reads the descriptor initialized before workers start. It remains stable through online/offline transitions and is closed only after workers are quiescent at teardown. |
| `llam_processor_event_pending` | Returns the stable atomic word used by scheduler wake producers and idle waiters. Callers retain the existing atomic orders and pass this same word to Linux private futex operations. |

Wake coalescing retains its publication ordering: runnable work and its queue
state are published before the wake; producer claims and consumer exchanges
remain acquire/release or acq_rel as required by their paths; failed wake
publication resets the pending bit with release semantics. Moving the word does
not weaken the existing atomic ordering. Node wake state remains independent.

The plain topology fields and descriptor are safe to read without a lock because
runtime initialization precedes worker publication and teardown stops workers
before closing the descriptor. No accessor authorizes a read after runtime
teardown.

## Versioned P-owner projection

`llam_processor_owner_record_t` contains the M/worker pointer, worker
incarnation, a snapshot of the composed lease epoch and state, P generation,
and one of `FREE`, `OWNED`, `TRANSFERRING`, or `STOPPING`. The complete record
is protected by the compatibility shard's existing `shard->lock`, which is
the first P payload lock. No second processor mutex is introduced. Lease
writers hold `opaque_lock` and acquire `shard->lock` in that order before
publishing the lease and matching P projection.

Composition derives the P tuple from the lease; the record cannot claim an M
independently. P generation advances when the projected owner, incarnation,
lease epoch/state, or P state changes. `TRANSFERRING` is written only as an
unobservable intermediate while both locks are held and is rejected by all
ticket accessors. Offline processors retain the matching owner reservation so
no other M can claim them, but ordinary task-execution tickets are rejected
until the processor is online. `STOPPING` retains the last worker/incarnation
for legacy drain bookkeeping; the Task 2A ticket API exposes no ticket in that
state. Final stop clears that pointer after worker joins, before runtime memory
is released.

The ticket binds processor identity, worker pointer, worker incarnation, lease
epoch, lease state, P state, and P generation. Snapshot and validation acquire
`opaque_lock -> shard->lock`, require a live embedded primary/helper binding,
and accept only an online, fully composed `PRIMARY_OWNED` or `HELPER_OWNED`
lease. Initial primary ownership legitimately has lease epoch zero. A ticket
owns no worker, processor, or runtime lifetime; callers must keep that storage
alive and revalidate the ticket at every operation boundary. The lease remains
the only execution-publication authority.

## Lifecycle and retained ownership

Initialization sets every processor descriptor to an invalid sentinel before
partial-init cleanup can run, initializes the topology and atomics, then creates
the scheduler wake descriptor before workers start. Cleanup closes the
processor descriptor after scheduler workers have stopped. Dynamic offlining
changes only `online`; it does not close, duplicate, or replace the descriptor.
Reactivation reuses the same descriptor and pending word.

The scheduler wake pair is used by enqueue, stop/fatal notification, and idle
wait. Opaque-helper wait sequencing and signals remain shard-owned. Each node
retains its own backend descriptor and pending word. Allocator mutexes and
processor stack-cache mutexes are initialized in their final P storage; no
mutex-bearing object is copied or assigned. Partial-init cleanup drains any
processor-local stacks before destroying only an initialized cache mutex, and
allocator teardown occurs after task objects are returned. Runtime fallback
stack caches are drained and destroyed independently.

## Verification boundary

`test_processor_resource_internal` covers processor identity, topology
accessors, primary bind, helper request/clear/grant/rollback/handback, redirect
claimant reacquisition, offline/online and STOPPING-vs-claim races, stale
incarnation/epoch/P-generation tickets, malformed current tuples (including a
matching invalid lease/worker pointer), unbind/finalization lifetime, wake
coalescing/drain, and descriptor lifetime across offline/reactivation and
shutdown. `test_processor_queue_timer_internal` covers owner-only queue mutation
and transactional timer rehome. `test_processor_allocator_io_internal` covers
worker-ring/non-ring route selection, logical-owner and attached-node precedence,
cross-P and same-ID cross-runtime I/O-request frees, task home-P stack-cache
routing, partial-init failure after stack-cache prewarm, and subsequent runtime
recovery.

The Phase 1A boundary audit permits the intended topology/wake, owner-record,
queue/timer, allocator, and stack-cache members on P. It rejects duplicate
processor-owned fields on the shard and copied node backend state, opaque-helper
wake state, task registry, metrics, trace, and lease coordinator fields on P.
The audit remains a conservative lexical check,
not a substitute for compiler or whole-program data-flow analysis.
