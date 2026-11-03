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
See LICENSE.
-->

# Phase 2 Foreign Detach and Fail-Closed Relief M

## Status

Phase 2 extends the Phase 1A legacy lease and Phase 1B processor resource
authority. It adds a private, generation-checked M/G/P observation contract for
the existing `llam_enter_blocking()` / `llam_leave_blocking()` compatibility
API. The lease remains the sole execution-owner authority; no public header,
ABI layout, or public statistics schema changes. The internal state dump adds
M/token and detach/deferral diagnostics. Phase 6 language wrappers are out of
scope.

## Goal

Allow one managed G to continue on its bound native M while that M executes
foreign blocking code, after the M safely releases P. Reacquire P only through
the existing lease handback and publish the G only after a fresh authority
check. Reuse the existing opaque helper and redirect path as the relief-M
bridge; do not add a second P-owner authority or a new general worker pool.

This phase remains cooperative. It does not capture an arbitrary C stack, move
a G to another M, or promise to reclaim P from an unannounced busy loop. Unsafe,
unknown, stale, or preemption-disabled states fail closed and record a reason.
Asynchronous context capture and compiler safe-range metadata are later phases.

## Authority model

- **G** is the task/fiber. Its `run_generation` changes whenever it is bound as
  active-G on an M, including direct G-to-G handoffs that bypass the scheduler
  dispatch prologue. It binds a foreign token to one execution of that task
  object.
- **M** is a native worker. Its `execution_state`, current-G publication,
  current-P publication, worker incarnation, and `run_generation` describe one
  bound worker execution. These observations do not independently authorize
  queue access or execution.
- **P** is the processor resource cell. A P-owner ticket records its identity,
  owner M/incarnation, lease projection epoch, P generation, and lease state.
  It is a non-owning snapshot: each operation revalidates it while holding the
  established `opaque_lock -> shard->lock` pair.
- **The legacy lease** remains the only execution-owner authority. The P-owner
  record is composed with it under the same lock order. A foreign token records
  a detached M/G episode but is not an execution lease and cannot override the
  lease or a P ticket.

Each worker publishes one of `IDLE`, `SCHEDULER`, `MANAGED_SAFE`,
`RUNTIME_UNSAFE`, `FOREIGN`, or `STOPPING`, plus nested runtime-critical and
preemption-disable depths. Relief eligibility requires a running G, matching
TLS/M/G identity, `MANAGED_SAFE`, zero depths, an inactive foreign token, and a
current P ticket for the same M/incarnation. Unknown states, a missing or stale
P ticket, an active token, or either nonzero depth defer the transition.

The foreign token is M-local and contains the worker pointer/incarnation and
run generation, task pointer/run generation, processor, lease epoch and P
generation at detach, transfer sequence, mode (`HELPER` or `REDIRECT`), token
generation, and lifecycle state (`INACTIVE`, `ACTIVE`, `RETURNING`). Token
fields are published and validated under the ordered lock pair.

## Outermost foreign enter

1. An outermost enter snapshots a safe managed G/M/P tuple and the current P
   ticket. It records the task and worker run generations, then enters runtime-
   critical and preemption-disabled state before changing scheduler ownership.
2. The existing helper or redirect protocol performs the lease transition.
   On the helper path, the task becomes blocked and active-G is cleared before
   the lease owner is cleared. Redirect fallback publishes the ownerless lease
   and clears current-P first, then clears active-G under the ordered locks;
   through that intermediate publication window the M remains runtime-unsafe
   and preemption-disabled and has no P access.
3. Under the ordered lock pair, the M publishes an `ACTIVE` token and
   `FOREIGN` state, then returns to caller code with opaque depth one. No
   scheduler lock remains held across that caller/foreign code.
4. If the helper has already begun a nested redirect before the primary
   publishes its helper-backed token, publication is allowed only for the exact
   live helper return claimant with no current P, matching worker incarnation,
   and either a matching redirect token or a runtime-critical detach/return in
   progress. This closes the interval between lease publication, active-G
   cleanup, and token publication without authorizing P access.

Nested enters on the same managed task/M only increase its depth; they do not
release P again or create another token. Foreign task creation cannot take the
detached M's P-local allocator fast path: it uses the remote/external route
until a fresh ticket is reacquired. `llam_call_blocking_result()` continues to
use the existing blocking pool.

## Outermost foreign leave

Nested leave validates the active token and decrements only the matching
task-local depth. Outermost leave changes the token to `RETURNING`, disables
preemption, and follows the existing helper/redirect handback sequence. It
does not restore `RUNNING` or active-G merely because the foreign function
returned.

After handback, the final ordered-lock transaction must validate either:

- a fresh ordinary P ticket for the same worker/incarnation; or
- during shutdown only, the exact acknowledged `STOPPING` return: the lease and
  P projection identify the same M/incarnation, the transfer ACK matches, the
  current P is published on that M, and no G is currently published. The
  claimant may either still be retained or have been cleared by a completed
  handback immediately before STOPPING. A helper-backed token may refer to the
  original helper-grant sequence while the acknowledged stop-return has a
  later sequence; the live M/token identity and the exact current ACK bind this
  final unwind.

Only then are the token/depth cleared, the task marked running, and active-G
published. The shutdown case is a restricted unwind so the task can return and
observe cancellation; it is not an ordinary execution ticket or permission for
new queue ownership. A failed handback restores the active token and depth and
does not publish a false G owner.

Calls outside a managed task retain their no-op behavior. A leave from another
unmanaged thread cannot consume a token because its TLS identity does not match.
A stale token or mismatched managed identity fails without changing another
M's depth, P owner, or active-G publication.

## Relief and watchdog behavior

The already-created opaque helper is the only relief M introduced here. Its
activation and return remain serialized by the Phase 1A lease, and it runs
other queued Gs only after P handoff; it never runs the foreign G concurrently.
The existing redirect path remains the bounded fallback when a helper grant is
unavailable. `llam_processor_relief_snapshot()` is an eligibility gate, not an
ownership transfer: the ordered lease transaction still performs the transfer.

Watchdog signal targeting also requires an exact P ticket in addition to the
lease owner/incarnation and safe managed G/M tuple. The ticket is revalidated
with the target immediately before signaling. Unknown M states, runtime-
critical or preemption-disabled states, stale P tickets, and detached foreign
G/M states are deferred with internal counters. An overdue G with no announced
safepoint increments `relief_deferred_unannounced` when a cooperative request
is first issued; it remains on its existing M/P, and the signal never transfers
execution to a second M. The normal build remains cooperative; a busy loop with
no safe boundary is not asynchronously moved.

Internal counters report foreign detaches/reacquisitions, helper-backed relief
activations, unsafe/unknown/stale-ticket deferrals, and unannounced no-safepoint
stall deferrals. They do not extend the public statistics ABI or schema.

## Shutdown, cancellation, and barriers

Merge/offline requests continue to use the generation-bearing lease pause ACK.
No new helper/P grant starts while shutdown is requested. `STOPPING` preserves
the existing claimant and handback ACK until teardown joins the native worker.
An active foreign G can return only through the exact acknowledged shutdown
unwind described above; ownerless or stale claimants cannot manufacture a P
ticket. Runtime stop remains local and cannot stop a peer runtime.

## Verification

- `test_foreign_detach_internal`: current M/G/P ticket, outer and nested token
  transitions, cross-thread no-op, remote foreign spawn allocation, stale-token
  rejection, and exact P reacquisition.
- `test_relief_worker_internal`: eligible safe M/P tuple, explicit deferral
  for preemption-disabled, runtime-critical, unknown-state, stale-ticket, and
  overdue no-safepoint cases; the latter leaves active-G/P on the same M.
- `test_legacy_lease_internal`: helper activation/handback, helper re-blocking,
  ownerless redirect ordering, stop races, and exact shutdown transfer ACKs.
- Run the full Make and fresh Debug CMake/CTest suites, ASan/UBSan, TSan,
  boundary checks, ABI/export/test-hook audits, and strict documentation build.
  Native Windows and BSD execution require their respective hosts and are not
  implied by Darwin verification.
