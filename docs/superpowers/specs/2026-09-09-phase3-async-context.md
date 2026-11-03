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

# Phase 3: Linux x86-64 Experimental Async Preemption Base

## Status and scope

This phase adds an internal, opt-in experiment behind
`LLAM_EXPERIMENTAL_ASYNC_PREEMPT`. It does not change public headers, public
statistics, exported symbols, or the default cooperative preemption policy.
Normal builds keep the existing one-store `SIGUSR1` handler. Non-Linux and
non-x86-64 builds report the focused test as skipped; they do not gain an
asynchronous backend.

The feature supports prepared primary workers within one enabled runtime per
process, an unblocked reserved real-time signal on each worker, and manually
registered, immutable code intervals. `deterministic=1` still selects one P;
nondeterministic configurations can use multiple primary M/P pairs. It refuses startup
when it cannot reserve an unused signal or provide the required context state.
The active handler action includes `SA_SIGINFO | SA_ONSTACK | SA_RESTART`; the
normal cooperative signal remains independent. Enablement must precede task
creation and execution. This is a base implementation, not a promise that
arbitrary C code or every supported LLAM platform is asynchronously preemptible.

The multi-worker extension binds **every G at first dispatch**, not at first
capture. Its native M/incarnation/run service pin remains through cooperative
waits and callback offload until off-stack terminal cleanup. NEW Gs may use
ordinary placement/stealing; started Gs use the origin M's intrusive ready queue,
protected by the associated shard lock. This is separate from public `PINNED`
semantics. Cross-M async migration is not supported.

Enable prepares every configured primary slot without partial publication. Each
actual native binding then installs its signal stack and probes its signal ABI.
An all-worker startup barrier prevents any user dispatch until all probes pass.
Thread/probe failure or a pre-dispatch close releases the barrier and joins the
prepared threads without waiting for NEW Gs to complete. Repeated runs use new
run/binding generations. Capability is `prepared` before native qualification.

## Request and capture protocol

The watchdog first validates the ordinary processor-owner ticket while holding
the existing lease lock pair. It publishes one request generation with the
target task, task/M run generations, worker incarnation, P generation, and
lease-projection epoch before sending the reserved signal. The signal handler
uses only lock-free atomics, bounded range lookup, register assignments, and
preallocated copies. It takes no locks, allocates no memory, and calls no
general-purpose runtime helper.

The handler accepts only the exact live task/M/P tuple and a PC inside one of
the registered intervals. The current P generation and lease epoch have
lock-free, signal-only worker mirrors; they are not execution authorities and
do not replace the lease ticket. A stale tuple is discarded. An unregistered
PC or an unsafe worker state leaves the ordinary cooperative preemption request
pending. Such requests are retried at a bounded interval while the same
generation remains eligible.

On acceptance, the handler copies the machine context, signal mask, alternate
stack descriptor, FP/XSTATE image and interrupted `errno` into a **G-owned
capsule**. Each task has two independently allocated banks, prepared and touched
before task publication. M-owned capture scratch is never the only copy of a
suspended G's continuation.

The first kernel signal return enters a separate, guarded M landing stack. The
assembly bridge establishes the C ABI, clears DF and restores scheduler FP state
before calling C. It saves only disposable M landing state and returns to the
ordinary scheduler stack; it does not call `llam_yield()` or overwrite `G.ctx`.
The scheduler clears active-G, completes the capsule's `READY` publication and
then publishes the runnable G. The run gate and the live, uncompleted G retain
runtime/task storage throughout this off-stack publication gap.

Dispatch distinguishes a cooperative context from a ready capsule. An async
capsule is eligible only on its original M and binding incarnation. Its unique
resume claim builds a fresh Linux `rt_sigreturn` frame on M-owned storage, with
a separate restore XSTATE image. The kernel restores the interrupted PC/SP,
registers, flags, signal mask and supported extended state together. There is no
global resume PC and no manual last-register jump. An ambiguous post-claim
restore failure is fatal, never retried as if the capsule remained `READY`.

The old bank remains `RESTORING` until real user reentry is witnessed, either by
a signal interrupting the exact G/M dispatch on its G stack in a certified PC
range, or by return to the scheduler from that restore. Observation records the
exact restore sequence, not just a boolean epoch. A stale signal can witness
reentry without authorizing a new capture. A new capture uses the other bank;
it cannot overwrite the outstanding restore. Only the scheduler retires the
observed bank, with the reserved signal blocked, withdrawing its pending pointer
before publishing `FREE`. Capsule readers remain local to the origin M; this
extension adds no external capsule-payload readers.

The Linux signal-frame parser validates the legacy/XSTATE magic, size bounds,
feature masks, trailing magic, and standard (not compacted) XSAVE layout before
restoring. PKRU is rejected because the current restore sequence cannot safely
apply page-access rights as its final operation. CET shadow-stack state and
builds advertising CET are unsupported and fail closed. The format checks
follow the [Linux x86 signal-context UAPI](https://github.com/torvalds/linux/blob/master/arch/x86/include/uapi/asm/sigcontext.h); the shadow-stack probe uses Linux's documented
`ARCH_SHSTK_STATUS` interface ([kernel CET documentation](https://docs.kernel.org/next/x86/shstk.html)).

## Concurrency and lifecycle constraints

- Only one runtime may reserve the experimental backend per process, but many
  Gs can remain suspended concurrently: their payloads live in separate G-owned
  banks. M storage is transient landing/restore storage.
- Opaque helpers cannot be created or dispatch Gs in an activated runtime.
  Managed `llam_enter_blocking()` still returns `ENOTSUP` before changing depth,
  token or lease state. `llam_call_blocking_result()` is the supported foreign
  path: callbacks run on the existing native blocking pool and their parked G
  resumes on its original M. Native callbacks do not inherit the submitter's TLS
  or lock ownership; nested unmanaged callback calls remain synchronous-inline.
- The worker alternate signal stack and a separate guarded trampoline stack
  must both be live. The worker's reserved signal must not be blocked. Startup
  fails if those conditions are not true on the actual scheduler thread. A
  synchronous probe on that thread also validates the delivered signal frame
  before dispatching any G. Missing user CS/SS or unsupported XSTATE produces
  `signal-frame-unsupported`; the implementation never invents selectors.
- Activated experimental runtimes disable direct yield, join and wake/channel
  handoffs. Every dispatch and restore retirement goes through the scheduler.
  Ordinary builds and nonactivated runtimes retain their existing fast paths.
- Task completion is not permission to discard a pending restore. Scheduler
  return retires it before stack release and `reclaim_ready`. On worker exit,
  native unbinding first quiesces senders under the existing lease locks; the
  reserved signal is then blocked/drained before TLS and alternate-stack release.
- Unknown PCs, stale run or ownership generations, nested/duplicate requests,
  an unsupported signal-frame layout, or disabled async state defer
  or decline capture. The original cooperative request is retained unless a
  successful capture consumes it.
- The manual range API is private. Registration is frozen at enable time and
  overlapping ranges are rejected. A range author is responsible for proving
  the code is safe to interrupt and yield from; this is not a general-purpose
  mechanism for preempting arbitrary C, foreign code, lock holders, system
  calls, or code with external side effects that require instruction-level
  atomicity.
- The reserved signal is process-wide. Applications must not replace its
  action or block it on the scheduler thread while the experiment is enabled.
  Shutdown restores the prior action only if LLAM still owns that action.

## Admission and retirement budgets

These private experimental variables are read and frozen at enable time:

| Variable | Default | Meaning |
| --- | --- | --- |
| `LLAM_ASYNC_CAPSULE_BUDGET_BYTES` | 67108864 (64 MiB) | Logical reserved bytes for both G capsule banks at full prepared capacity plus their fixed storage header. |
| `LLAM_ASYNC_BLOCK_JOB_LIMIT` | 1024 | Unreturned blocking admission tickets, including preparation and terminal cleanup, not just queued/running callbacks. |

Values must be nonzero decimal integers within the corresponding size/counter
range. Empty, malformed, zero and overflowing values fail enable with `EINVAL`
and no partial activation. These defaults are engineering limits, not SLOs or an
RSS cap: G stacks, M arenas, allocator retention and user callback allocations
are separate. Configuration does not grow the fixed blocking pool.

G admission reserves its full charge before allocating banks; quota/allocation
failure returns spawn failure with `ENOMEM`. The reservation is returned only
after reader/restore retirement and bank storage release. Activated runtimes
track even never-started Gs in their lifetime lists so destroy cannot bypass
that finalizer. A completed public handle does not keep retired capsule bytes
charged.

Pool submission reserves a ticket before job/wait-node allocation or acquiring
new callback references. Saturation returns `EAGAIN`, leaves a valid `out` NULL,
does not park, and never runs the rejected callback. Preparation failures return
the ticket; after publication the job's unique finalizer owns it. Running cancel
retains the ticket and callback arguments until actual return; queued cancelled
jobs retain it until tombstone drain and final reference cleanup. Therefore
`block_pending` may be below the limit while admission still returns `EAGAIN`.
Stop/cancel are rechecked before queue publication. Ordinary-mode accounting
and the public ABI remain unchanged.

## Stop is not the end of execution service

Closing admission rejects new Gs/offload jobs but does not discard accepted
continuations. During an active async run the controller switches to a
service-only drain branch: no autotuning, scaling or rehome. A separate private
snapshot validates the same prepared primary, run/G identity and current OWNED
or STOPPING drain projection. The ordinary execute-ticket and watchdog snapshots
still reject shutdown/STOPPING; their contract was not broadened.

Drain requests retain exact execution identity while carrying current service
authorization. A stale normal request is replaced through the existing mailbox
CAS protocol, not overwritten while CAPTURING. The handler retains all PC/SP,
unsafe-depth and CPU-state checks. Drain service uses the dispatch quantum
without the ordinary pressure suppression. Origin-ready dispatch and internal
timer/cancel/offload wakes remain available. Valid OWNED-to-STOPPING transition
does not invalidate an already accepted restore or its independent witness.

Each worker withdraws its target under the sender locks, drains the reserved
signal and leaves; run completion is published before controller teardown/join.
Never-run/startup-aborted Gs do not keep the controller waiting. This provides
execution opportunities to perform normal cleanup, not forced termination of
an infinite unsafe loop or a foreign callback that never returns.

Async capture does not inject cancellation into arbitrary machine code, migrate
a captured task to another M, make foreign libraries reentrant, or supply hard
real-time guarantees. Stop remains cooperative for an unregistered busy loop.
The broader design in `research/async-preemption-2026-09-22/` is a roadmap, not
an implemented contract.

The prerequisite foreign-detach correction applies to ordinary builds too:
`llam_yield()` cannot enqueue a foreign G after it relinquishes P. A foreign
join waits on its existing native M, retaining timeout/cancellation and join
claim rules, and waits for actual off-stack reclamation before consuming the
handle. This also covers a stop-time failed leave that retains its foreign token.

## Verification

`tests/test_async_preempt_linux_x86_64.c` covers:

- A no-poll assembly leaf yielding CPU to a companion through actual signal
  capture, restoring GPR/XMM state and completing without host rescue.
- An unregistered leaf that is never captured; only host rescue releases it.
- Three runs on the same release-profile runtime, each holding six no-poll Gs
  suspended concurrently and requiring at least 24 captures. Oracles check
  flags, red-zone data, supported SSE/AVX state, FP control, `errno` and captured
  signal-mask restoration. A managed join must dispatch a ready capsule without
  a direct cooperative-context handoff. Immediate task exit leaves no pending
  restore behind.
- Seven one-shot preparation-allocation failures, clean retry and never-started
  task cleanup. These hooks do not exist in production artifacts.
- A late reserved signal that must witness an outstanding restore without
  authorizing another capture; and a stop injected between active-G withdrawal
  and READY publication. Both require complete retirement and host mask/actual
  alternate-stack restoration, without timeout rescue.
- Independent missing-selector refusal checks. An actual unsupported signal
  ABI must fail before any G starts; only that precise refusal or unsupported
  CPU context is reported as skip 77. Timeouts, crashes and state mismatches fail.

`test_foreign_detach_internal` also checks native foreign join timeout,
cancellation, self-join rejection, retry after claim release, and errno/binding
preservation. `test_legacy_lease_internal` covers ownerless stop/cleanup and
supports exact-case diagnostic selection with `LLAM_LEGACY_LEASE_TEST`.

`test_async_preempt_multi_linux_x86_64` covers actual 2/4-primary captures with
overlapping suspended Gs and native-origin preservation through yield, timer and
offload. It also exercises nth-worker allocation/create/probe rollback, concurrent
capsule reservations, repeated-run quota reuse, tiny blocking limits, queued and
running cancellation, timeout claim preservation, recursive native callbacks,
preparation/stop races and config refusal. Delayed normal descriptors are
replaced by real drain requests in plain-stop and concurrent-destroy workloads;
only admitted same-M controllers release the busy leaves. Startup close must
execute no user G. Test-only hooks are absent from production artifacts.

This phase does not claim native x86 hardware coverage from the current Darwin
arm64 host. Positive Linux x86-64 tests use explicit QEMU user emulation with
Nehalem (FXSAVE) and Haswell (AVX/XSAVE) CPU models inside Docker. The default
amd64 translator on this host supplies an incomplete signal frame and is
rejected; its skip is not positive async coverage. Native x86 signal-return,
cross-M migration, CET/PKRU, other OS/architecture backends and production
latency qualification remain unverified or unsupported. Keep this feature
experimental and opt-in.
