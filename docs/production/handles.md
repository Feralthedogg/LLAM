# Handle boundaries

Explicit runtime handles keep tasks, channels, and I/O inside their owner runtime.
Use this page when more than one runtime shares a process.

## Lifecycle calls

`llam_runtime_create()`, `llam_runtime_run_handle()`, and
`llam_runtime_destroy()` are the canonical embedding-facing lifecycle APIs, and
`llam_runtime_create_compat()` takes the place of the first for plain C tasks.
`llam_runtime_spawn_ex()` should be used to attach root tasks to a specific
runtime handle. Legacy host-thread lifecycle calls remain wrappers for the
process-default runtime, while managed task spawn/stop/shutdown wrappers use
the current task's owner runtime.

## What a handle protects

Explicit runtime handles own their scheduler state, active allocation caches,
blocking helper pool, and platform backend routing. Cold process-wide reusable
storage may exist only for owner-poisoned idle objects; acquisition restamps the
current owner before returning a public handle. Public handles are still backed
by process-wide family-tagged slot tables; each object is owner-tagged so
cross-runtime managed use fails with `EXDEV` instead of silently crossing queues
or wait lists. The slot tables use sealed generation tokens from a slot-stable
affine permutation of the internal epoch space, derived from table secret
material and slot identity. Trivial first-handle guesses, monotonic
next-generation guesses, and old consumed generations do not become valid
handles after object creation or slot reuse. This is an in-process UAF/FFI
hardening layer, not a cryptographic capability boundary against code that can
already read or corrupt LLAM memory.

## Use a process boundary for untrusted code

When the threat model includes untrusted code with possible same-process memory
corruption, use a process boundary. The internal broker-control foundation keeps
capability MAC keys and object authority inside a broker-owned runtime. The
default runtime handles remain the trusted in-process fast path; ordinary
runtime creation does not require broker entropy and must not be described as a
cryptographic security boundary.

The current broker path is intentionally narrower than the public C API. It
covers subject-bound tokens, POSIX Unix-domain and Windows named-pipe control
transports, bounded buffer/channel grants, descriptor/HANDLE grants, private
ring setup over the control transport, broker-owned buffer/channel/descriptor
data-plane operations, predefined task commands, attenuation/revocation,
stale-output zeroing, active-operation pins during destroy, and raw-token
minting rejection. Treat the following as operational invariants:

- Descriptor/HANDLE authority is registered through `SCM_RIGHTS` on POSIX or
  `DuplicateHandle` from the connected named-pipe peer on Windows; raw numeric
  fd/HANDLE values in client messages are not authority.
- Broker transports bind tokens to the issuing subject/session and reject replay
  on another session with `EACCES`.
- Token issuance requires OS entropy for nonce bytes; entropy failure returns
  `EIO` and clears partial output.
- Broker wire-read failures and failed broker-owned reads clear caller output,
  and successful short reads clear the unused suffix.
- Broker-owned fds and ring transport fds are close-on-exec on POSIX.
- Broker destroy rejects new operations, drains already-pinned operations, and
  scrubs MAC keys, revocation state, object ids, and subject sessions.

The [security model](../reference/security/index.md) is the authority for broker
threat boundaries, current coverage, non-goals, and follow-up RPC work.

## Stay inside the owner runtime

Managed task lifecycle wrappers are constrained to the current owner runtime.
If task code receives a foreign runtime handle and calls `llam_runtime_destroy()`,
LLAM ignores it rather than converting that handle into a peer-runtime stop
signal. `llam_runtime_request_stop_handle()` and the handle-scoped constructors
fail with `EXDEV` for a task of a peer runtime. `llam_runtime_spawn_ex()` has no such
check: a task can attach work to a peer as long as any cancellation token it passes
belongs to that peer. Host threads should still perform explicit cross-runtime
orchestration.

Spawn-time cancellation tokens follow the same owner boundary. A task can only
retain a cancellation token owned by its target runtime, and task-group spawns
publish children onto the group owner runtime rather than a caller's unrelated
current/default runtime.

## Remaining hardening

The remaining hardening path is incremental:

- Keep process signal/fault hooks reference-counted and safe across concurrent
  runtime lifecycles.
- Expand platform I/O tests for completion-after-cancel, fd/socket reuse, and
  runtime-destroy ordering on Linux, macOS, and Windows.
- Keep two-runtime tests in CI so task IDs, channels, timers, cancellation
  tokens, caches, and I/O completions stay isolated.

## Concurrent embedding

Host applications should not repeatedly initialize and shut down the legacy
default runtime from concurrent embedding threads. Use explicit runtime handles
for concurrent embedding.
