# Scheduler

How shards, processors, and workers run tasks: the run queues, time slices, timer
accounting, and dynamic offlining.

## Run queues

LLAM is an N:M stackful scheduler. Each task is a `void (*)(void *)` function
with its own stack. OS worker threads execute managed tasks through a shard's
single P owner. The processor cell owns runnable and timer-heap storage, its
allocator, and per-P stack caches; `shard->lock` remains the first migration's
payload lock. Diagnostic task lists, metrics, trace state, and worker contexts
remain on the shard. The processor-owned runnable lanes are:

- `hot_q` for latency-class work and completions, capacity 1024.
- `norm_q` for ordinary runnable work, capacity 4096.
- `inject_q` for cross-shard or host-injected work, capacity 1024.
- a timer heap for sleeps and timed waits.
- per-P allocation caches and remote-free queues for tasks, wait nodes, timer
  nodes, I/O requests, and I/O buffers.
- a per-P stack cache, with a separate runtime-wide stack fallback cache.
- a scheduler fiber context used to switch into managed tasks.

`norm_q` has two implementations, selected at init time: a mutex-guarded FIFO queue,
or a Chase-Lev lock-free deque (`llam_cldeque_t`) for work stealing. The lock-free
deque is a bounded circular buffer of 4096 task pointers with separated `top` (steal
end) and `bottom` (push and pop end) atomics, each on its own cache line.

## Task and buffer allocation

Each processor's allocator (`llam_allocator_t`) keeps free lists for five object
types:

| Object | Per slab | Purpose |
| --- | --- | --- |
| `llam_task_t` | 16 | Task metadata with its embedded wait node, I/O request, and timer node. |
| `llam_wait_node_t` | 64 | Mutex, condition, and channel waiter nodes. |
| `llam_timer_node_t` | 64 | Timer heap entries. |
| `llam_io_req_t` | 64 | I/O operation descriptors. |
| `llam_io_buffer_t` | 16, or 512 in huge-allocator mode | Owned I/O buffers. |

When an object is freed on a different processor than it was allocated on, the free
goes through a lock-free MPSC list (`task_remote_free`, `wait_remote_free`, and so
on) and drains into the local free list during `llam_allocator_quiescent()` at safe
points. Each remote-free queue sits on its own cache line to avoid false sharing.

Balanced and debug-safe profiles prewarm one 16-object task slab per processor;
release-fast and io-latency retain a 128-object reserve. `LLAM_TASK_CACHE_PREWARM`
overrides these defaults. Owned I/O buffers allocate only the inline payload
needed by small requests, rounded to 16 bytes; large or explicitly aligned
payloads use a compact header and separate storage. Public buffers remain
detached from processor slabs so their handles can outlive the runtime.

Unmanaged host spawns use a separate adaptive task-span pool under the owning
processor's allocator lock. Spans start at one task, grow geometrically, and
stop at a page-density capacity of at most 16 tasks. Small ramps use the system
heap; mature spans use a dense page mapping, with a tracked heap fallback if
mapping fails. Allocation prefers partially occupied spans and keeps at most
two completely empty reusable spans per processor. Old empty spans are retired
as a batch under the task registry lock, then their resources and backing pages
are released after the allocator lock is dropped. Active public pins, scan refs
or registry contention quarantine the entire span for later retry; quarantined
storage is never reused. A shared pending hint lets owner safepoints finish one
retirement at a time without adding an atomic to the managed alloc/free path.
Inactive runtimes finish remaining retirement on later activity or teardown.
Partially live spans and the process-global handle table can still retain
memory. Heap fallback storage returns to libc and may remain in its own caches.

## Select wait nodes

Multi-channel select keeps its first four wait nodes in the suspended select
call's fiber stack frame; wider selects borrow additional processor wait nodes.
Tasks do not reserve select nodes or initialize them at spawn. The ready-only
path does not initialize scratch waiters. Queue cleanup, cancellation-resolver
drain, and timer-reference drain finish before the frame returns, so scratch
waiters share the lifetime of the existing stack-backed select state.

## Stack caches

| Stack class | Size | Processor cache limit | Runtime-wide limit |
| --- | --- | --- | --- |
| Default | 64 KiB | 256 (512 in release-fast) | 4096 |
| Large | 256 KiB | 64 | 512 |
| Huge | 1 MiB | 16 | 128 |

On POSIX, stack mappings use `mmap(MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK)` with a
guard page (`mprotect(PROT_NONE)`) at the bottom. Returned stacks are cached for
reuse, and `llam_runtime_prewarm_stack_cache` prewarms the cache at init.

Processor stack caches keep warm payloads up to 16 MiB for default stacks and
8 MiB each for large and huge stacks in balanced and debug-safe profiles.
Release-fast and io-latency retain their larger warm limits. The runtime-wide
fallback keeps a shared warm reserve of 32 MiB for
default stacks and 8 MiB each for large and huge stacks, and consumes warm
entries before cold entries. Further overflow receives platform-specific
discard advice to release or mark payload pages reclaimable. Guard pages and
virtual mappings remain available for reuse, and Darwin reuse accounting is
restored before context initialization. Advice is opportunistic; a failed hint
does not invalidate a cached stack. Existing mapping-count limits still bound
the fallback cache.

## Timers and remote work

The processor cell also owns timer count and callback accounting. Timer-node
`owner_shard` remains its allocator/cache return destination; a separate task
field identifies the active timer-heap owner so cancellation can retry safely
across rehome. The I/O node continues to own its platform backend and node wake
state.

Remote spawns, cross-shard wakes, and timer completions publish through the
processor inject queue (or runtime overflow). Only the current M's
generation-checked P queue access can mutate the local hot/normal lane or the
lock-free normal-deque bottom. STOPPING drain and opaque redirect release use
distinct private access modes; the lease remains the execution authority.
Dynamic offline waits for timer callbacks to quiesce, reserves target capacity,
validates wait generations and pins wait ownership, then moves timer nodes under
the ordered source/target locks. A failed preflight or reserve keeps the source
heap and task ownership unchanged.

## Scheduler loop

The compatibility scheduler loop (`llam_scheduler_loop`) runs on its worker threads.
Registered managed execution uses `llam_managed_runtime_run()` and fresh P claims
around dispatch and native restoration. The compatibility loop is:

```text
loop:
    1. Check runtime drain (live_tasks == 0, stop)
    2. Handle merge-pause requests from the watchdog
    3. Handle dynamic-worker offline state
    4. Drain the inject queue (up to 32 tasks per pass)
    5. Fire expired timers
    6. Dequeue from hot_q, then norm_q
    7. Attempt a work steal from a random sibling shard
    8. If no task was found, wait idle on the platform wake handle
    9. Context switch to the selected task
   10. On return, record metrics, check safepoints, repeat
```

The hot queue is reserved for latency-class tasks and I/O completions. The inject
queue receives cross-shard work and is drained with a budget cap to prevent
starvation.

[Context switching](context.md) uses the platform context path. Managed
constructors default to required asynchronous preemption: admitted compiler
images and native M probes must qualify before user code runs. Captured
continuations return through scheduler dispatch and a fresh registered lease.
Explicit compatibility constructors retain request-based preemption at LLAM
boundaries such as yields, waits, I/O and safepoints.

Cooperative legacy tasks can hand off directly to local work when joining or
parking a select. A yield with no runnable peer can retain the current task for
at most 64 consecutive local turns. Timers, remote injections, stop requests,
preemption requests, controller pauses and pressure force the scheduler path;
the configured handoff budget can shorten that bound. Asynchronous and
registered continuations always pass through scheduler dispatch.
