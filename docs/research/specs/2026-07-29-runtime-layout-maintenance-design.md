# Runtime Layout and Maintenance Design

**Date:** 2026-07-29

**Status:** Approved as part of the runtime-backend productization program.

## Goal

Reduce unconditional task/shard memory without introducing a new lifetime
graph, then make existing ownership state machines and diagnostics enforceable
and maintainable.

## Measured baseline

At the starting revision on Darwin and Linux arm64:

| Type | Size |
|---|---:|
| `llam_task_t` | 1,584 B |
| `llam_shard_t` | 56,320 B |
| `llam_cldeque_t` | 32,896 B |
| `llam_wait_node_t` | 80 B |
| `llam_timer_node_t` | 80 B |
| `llam_io_req_t` | 264/272 B |
| shard trace events | 20,480 B |

Wait/select nodes plus an unused timer consume 400 bytes per task. The
experimental deque and optional trace ring account for more than 94% of each
shard.

## Safe task shrink

Remove the unused embedded timer node; real timers already come from the shard
timer pool. `active_timer` and all publication metadata remain embedded.

Allocate ordinary wait and select nodes from the existing shard wait-node
pool. Allocation finishes before queue insertion, cancellation registration,
deadline arming, or active-owner publication. On partial select failure, all
acquired nodes are returned. Final release remains the waiter’s responsibility
after tracking is cleared and resolver claims drain. Remote return uses the
existing owner-shard list.

Retain the embedded I/O request for this program. Its address currently
participates in generation, backend-event, scan-ref, cancellation, and LEIR
zero-hot-allocation protocols. Removing it requires a separate storage-kind
design with a reusable bind-time request for LEIR.

Task size gates after timer/wait/select removal are:

- POSIX `llam_task_t <= 1120`;
- Windows `llam_task_t <= 1280`.

Cross-thread lifetime anchors—active pointers, generation and resolver state,
cancellation links, scan refs, completion/reclaim state—remain in the hot
task.

## Optional shard storage and alignment

Add overflow-safe aligned zero-allocation/free helpers using
`posix_memalign/free` on POSIX and `_aligned_malloc/_aligned_free` on Windows.
Use them for all over-aligned runtime, node, shard, deque, and trace arrays with
correct platform pairing.

When lock-free normal queues are disabled, no Chase–Lev storage exists and all
accesses use the locked FIFO. When enabled, one contiguous aligned runtime
array supplies exactly one immutable pointer per active shard. Requested
allocation failure aborts initialization with `ENOMEM`.

Tracing likewise owns one stable runtime array only when enabled. Producers
perform a pointer null check; fault-handler reads remain allocation-free and
lock-free. Storage is released only after producers stop and process handlers
are restored.

Final base-shard budgets are POSIX 4096 bytes and Windows 8192 bytes.

## LEIR Linux cold state

The common experimental native instance retains only portable plan/slot/mode
state and a nullable Linux bind-state pointer. First bind validates inputs,
builds a candidate Linux state off to the side, and commits by pointer swap
only while the instance is in `BINDING`.

The core Linux state owns the native segment, pinned FDs, and operation
indices. Fixed-buffer state is allocated only for fixed modes and owns buffers,
indices, lease, and runtime identity. Failed bind/rebind leaves the prior
usable state intact. Segment and lease addresses remain stable for their full
kernel/list lifetime. Destroy detaches a lease before freeing and retains state
if detach fails.

Budgets are common instance 384 bytes, Linux core 896 bytes, and fixed state
512 bytes.

## Ownership transition tables and file boundaries

Before moving code, checked-in tables define:

- wait resolver gate, generation, owner publication/clear, deadline/cancel
  registration, and I/O abort;
- I/O wait modes `NONE`, `SUBMIT_QUEUE`, `INFLIGHT`, and watch variants;
- Linux segment, batch, and cancel transitions through queued, inflight,
  retiring, retired, and idle.

Then split only whole ownership domains:

- `wait_owner.c`, `wait_deadline.c`, `wait_io_abort.c`;
- `task_bootstrap.c`, `issue_wait.c`, `issue_watch.c`;
- `linux_segment_reducer.c`, `queue.c`, `submit.c`, `complete.c`.

Lock-coherent mutations and fail-closed abort branches stay together.

## Direct handoff and autotune truthfulness

A private superset handoff rejection enum and pure common policy guard replace
duplicated policy checks. Wake, yield, join, and reinjection retain their own
context checks, queue commits, race classification, fallbacks, and metric
recorders. A table-driven matrix locks timer policies and reason mappings
before call-site conversion.

Autotune reports separate recognized, observable, controllable,
active-observation, and active-control masks. Current observability is handoff
and idle; current control is handoff only. Legacy fields remain with documented
configured/recognized meaning. `min_hold_ns` is enforced for handoff changes
or explicitly marked reserved.

## Runtime public handles

Handle conversion is an isolated final stage. Public explicit-runtime pointers
become encoded tokens in the existing slot/generation/family table while raw
runtime pointers remain internal to tasks, requests, owner stamps, and live
iteration.

Public boundaries resolve and pin tokens. Destroy drains active public
operations, releases the slot generation, and frees full runtime state.
Default-runtime behavior remains explicit. Tests cover deterministic slot
reuse, stale/wrong-family tokens, generation retirement, destroy races, and
raw internal owner-runtime pins before the full retired-state list is removed.

## Structure and fork maintenance

The structured size ratchet from the build-governance design covers production,
tests, and experiments. File-size budgets are lowered after each safe split.

Fork support is documentation and a supported child-exec integration test, not
partial mutex reset. Post-init child use of inherited LLAM state remains
unsupported.

## Verification sequence

1. Alignment helpers and layout guardrails.
2. Embedded timer removal.
3. Pooled wait/select nodes with allocation/race tests.
4. Conditional deque and trace storage.
5. Lazy LEIR Linux state.
6. Transition-table tests, handoff guard, and safe file splits.
7. Autotune domain correction and structure ratchet.
8. Isolated generational runtime-handle conversion.

Each stage runs focused RED/GREEN tests, the owning shutdown/security suites,
ASan, TSan, platform builds, and size budgets.
