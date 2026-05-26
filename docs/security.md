<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: Apache-2.0
-->

# LLAM Security Model

LLAM has two different handle models with different threat boundaries.

This document is intentionally explicit about the boundary: the default public
C handles are in-process misuse hardening, while broker mode is the direction
for isolation from untrusted code. Broker symbols are internal implementation
details in the current tree; they are tested and shipped as a foundation, not as
a stable public C ABI.

## In-Process Opaque Handles

The default C API returns opaque task, channel, mutex, condvar, cancellation,
task-group, runtime, and owned-buffer handles. These handles are hardened
against accidental misuse and common FFI bugs:

- stale or consumed handle reuse
- wrong object-family casts
- simple forged first handles
- monotonic next-generation guesses after slot reuse
- cross-runtime managed use through owner checks

This is an in-process safety layer. It is not a cryptographic capability
system. If code in the same process can arbitrarily read or write memory, it can
disclose or corrupt LLAM runtime state, including handle tables and secrets.

## Broker Capability Boundary

The capability boundary for hostile or memory-unsafe plugins must be outside
the untrusted address space. LLAM's broker-mode control plane follows this
model:

```text
untrusted client process
  owns serialized tokens only
  cannot read broker keys or registry memory

trusted LLAM broker process
  owns runtime, registries, descriptors, I/O backend, and MAC keys
  validates every token before applying an operation
```

The current 2.0.0 implementation provides the following internal token and
broker-control foundation:

- `llam_capability_token_t` carries runtime id, object family, slot,
  generation, rights, revocation epoch, broker-session subject id, nonce, and
  MAC.
- `llam_capability_key_t` is broker-local authority. It must not be mapped into
  an untrusted client address space. Broker key initialization requires secure
  OS entropy and fails closed with `EIO` if it is unavailable; LLAM does not use
  address/time/pid fallback material for out-of-process capability MAC keys.
- `llam_broker_t` owns a runtime, broker key, and revocation epoch.
- Broker MAC keys are created by broker initialization, not ordinary runtime
  creation. In-process runtime handles therefore remain a fast misuse-hardening
  API and do not depend on broker capability entropy unless the embedder opts
  into broker mode.
- Broker validation binds every token to the live broker runtime id after MAC
  validation. A structurally valid token for a different runtime fails closed
  even if internal test code signs it with the same key material.
- Client-visible validation requires a nonzero requested-rights mask. Zero-right
  validation fails with `EINVAL`, so broker transports cannot be used as a
  rightless live-token oracle.
- Raw capability issuance and validation reject slot zero with `EINVAL`. Broker
  object slots are nonzero, so slot zero cannot become a structurally valid
  authority outside live-object table checks.
- Direct broker object creation and direct object-token minting use the same
  family-specific rights allowlist as the transport path. Buffer, descriptor,
  channel, and predefined-task tokens cannot carry rights from another family or
  future-reserved bits.
- Direct broker buffer registration uses the same bounded grant limit as the
  transport path. Buffer grants larger than `LLAM_BROKER_BUFFER_MAX_BYTES` fail
  with `EINVAL` before allocation.
- Unsupported platform compatibility stubs that would otherwise return broker
  authority, such as Windows `llam_broker_register_fd()`, clear token outputs
  before reporting `ENOTSUP`.
- Failed attenuation and object-revocation calls clear output token storage,
  including in-place `token == out_token` calls. Treat a nonzero return as
  owning no fresh serialized authority.
- Object-specific revocation is atomic with respect to replacement-token
  issuance. If nonce entropy is unavailable, revocation fails with `EIO` and
  leaves the old object generation valid.
- `test_security_capability` verifies rights checks, tamper rejection,
  broker-owned attenuation, wrong-key rejection, foreign-runtime rejection,
  family-rights allowlist rejection, zero-slot issuance rejection,
  zero-right validation rejection,
  failed-output token clearing,
  secure-entropy fail-closed key creation, atomic object-specific revocation,
  POSIX shared-memory ring mapping,
  broker-side ring capability validation/attenuation/revocation,
  buffer-grant bounds, control-transport buffer/channel data-plane requests,
  and the POSIX socketpair request path.
- `llam_broker --self-test` validates the broker-control path before a
  transport layer is attached.
- `llam_broker --serve <path>`, `llam_broker --serve-n <path> <count>`,
  `llam_broker --serve-once <path>`, and
  `llam_broker --client-self-test <path>` exercise the local control transport:
  Unix-domain sockets on POSIX and named pipes on Windows.
- POSIX broker listen paths do not remove pre-existing filesystem entries:
  regular files, directories, symlinks, and stale socket nodes fail with
  `EEXIST` and are left intact. Newly bound broker sockets are chmod'd to
  owner-only `0600` using a no-symlink-following permission change when the
  platform exposes one. Platforms without that primitive fail closed rather
  than applying `chmod()` through a potentially swapped path. Cleanup records
  the filesystem socket identity immediately after bind and unlinks only that
  same path identity; this avoids relying on non-portable AF_UNIX fd inode
  comparisons.
- Windows broker named pipes are created with an explicit DACL for the current
  user, LocalSystem, and Administrators, plus `PIPE_REJECT_REMOTE_CLIENTS`.
  The broker fails closed if it cannot build that security descriptor instead
  of falling back to the process default DACL.
- Long-running broker serve mode accepts independent local control sessions
  sequentially. Each connection gets its own broker subject id; a client STOP
  closes only that session and does not reuse token audience state for the next
  connection. Malformed control clients fail only their accepted session; they
  cannot terminate a long-running broker process that is serving later
  sessions.
- The local control transport can create bounded broker-owned buffer and byte
  channel grants, then exercise fixed-size buffer read/write, byte-channel
  send/receive/close, and predefined task spawn/join/detach requests through
  those grants. Clients choose only size/capacity/task kind and a subset of
  family-specific rights; `ADMIN`, oversized buffers, over-capacity channels,
  invalid task commands, and payloads larger than `LLAM_BROKER_WIRE_DATA_BYTES`
  fail closed instead of minting authority over arbitrary broker objects or
  copying unbounded data through the control pipe.
- `llam_broker_ring_create_shm()`, `llam_broker_ring_open_shm()`, and
  `llam_broker_ring_unmap()` provide the internal shared-memory mapping layer
  for broker submission/completion rings. POSIX uses `shm_open`/`mmap`; Windows
  uses named file mappings. POSIX mappings are created owner-only, and Windows
  mappings are created with the same current-user, LocalSystem, and
  Administrators DACL policy as the control pipe.
- `llam_broker_ring_create_private_shm()` generates a high-entropy mapping name
  and should be preferred over caller-supplied, predictable pid/path names for
  broker setup and tests. A named mapping is still bearer rendezvous authority:
  the name should be delivered only over a subject-bound control session, and a
  fd/HANDLE-passing setup path should avoid disclosing reusable names altogether.
- On POSIX, `llam_broker_ring_create_private_fd()` creates the mapping through
  `shm_open`, immediately unlinks the rendezvous name, and keeps only the fd as
  authority. `llam_broker_ring_map_fd()` can map a passed fd, so a broker can
  deliver ring authority with `SCM_RIGHTS` instead of publishing a reusable
  shared-memory name.
- On Windows, `llam_broker_ring_create_private_handle()` creates an unnamed file
  mapping and keeps only the mapping HANDLE as authority. `llam_broker_ring_map_handle()`
  can map a duplicated HANDLE, so a broker can deliver ring authority with
  `DuplicateHandle` over the subject-bound control pipe instead of publishing a
  reusable object-manager name.
- The local control transport exposes this setup as
  `LLAM_BROKER_WIRE_OP_CREATE_RING`. POSIX responses attach the private ring fd
  with `SCM_RIGHTS`; Windows responses duplicate the unnamed mapping HANDLE into
  the connected named-pipe peer and return that peer-local HANDLE value. The
  broker retains its own mapping in broker-owned ring-session state, so
  shutdown can unmap it without trusting client-visible ring memory.
- `LLAM_BROKER_WIRE_OP_SERVE_RING` drives submissions from a
  transport-created ring by broker session id. A zero request length keeps the
  original single-request behavior; a nonzero length asks the broker to serve a
  bounded batch and is capped at `LLAM_BROKER_RING_SERVE_BATCH_MAX`. The
  response `result0` reports the number of submissions served. The session id
  is scoped to the control-transport subject; invalid or foreign sessions fail
  before consuming a client submission. Session-id serving claims the session
  busy state while still holding the broker table lock, so response-failure
  cleanup cannot unmap the broker-owned ring between lookup and execution.
- If a client disconnects after `CREATE_RING` allocates broker-owned ring state
  but before the fd/HANDLE response can be delivered, the broker reclaims that
  session immediately. POSIX transport writes use no-SIGPIPE send paths so this
  is reported as a local transport failure instead of terminating the broker.
  On Windows, the response HANDLE is duplicated into the named-pipe peer before
  the response bytes are written; if that write fails, the broker closes the
  peer-process duplicate with `DUPLICATE_CLOSE_SOURCE` before rolling back the
  ring session so an unreachable peer-local HANDLE cannot remain as authority.
- If a control connection drops after the broker creates a buffer, channel,
  descriptor, predefined task, or ring grant but before the response can be
  delivered, the transport rolls back the just-created broker-owned authority.
  A disconnecting peer therefore cannot strand unreachable fd/HANDLE slots,
  heap buffers, channel slots, task slots, or ring sessions as a resource leak.
  Task rollback atomically claims a spawned slot and clears client-visible
  rights, then the predefined task detaches itself from inside its trampoline
  before broker-slot reset. This prevents both permanent detached-but-active
  broker slots and host-thread detach races with task exit/reclaim.
  Client-visible predefined task detach uses the same atomic claim: if a task
  completes concurrently, detach observes the completed state and consumes the
  handle from that path instead of overwriting it with a stale detached state.
- `LLAM_BROKER_RING_OP_BUFFER_READ` and
  `LLAM_BROKER_RING_OP_BUFFER_WRITE` copy through a bounded shared data window
  only after broker-side capability validation and broker-owned buffer bounds
  checks succeed.
- Output-producing shared-memory ring operations clear their already-validated
  output window before publishing a failure completion, and successful short
  descriptor/channel reads clear the unused suffix of the requested output
  window. This does not turn the shared mapping into trusted storage, but it
  prevents stale successful output bytes or tokens from being confused with
  fresh broker output if a client bug ignores the completion status or result
  length.
- Broker-side ring consumers keep submission/completion progress cursors in
  broker-private memory and validate the client-controlled head/tail windows
  before reading or writing slots. Corrupt, inverted, or oversized windows fail
  closed instead of letting a client rewind shared counters and replay stale
  ring entries as new work.
- Broker object tables and ring-session state are guarded by broker-local
  locking. Serving the same shared-memory ring concurrently fails with `EBUSY`
  instead of racing broker-private cursors.
- Public broker validation and attenuation also take the broker lock before
  consulting live object slots, so object-specific revocation cannot race a
  concurrent validation reader.
- Broker destroy is two-phase for accepted broker operations: new operations are
  rejected once destroy starts, while already-pinned broker operations must
  drain before broker-owned tables, MAC keys, descriptors, rings, and runtime
  state are cleared. Same-thread helper calls made by an already accepted
  operation remain valid during this drain phase. This prevents a ring server
  from publishing a completion into broker-private state after a concurrent
  destroy has invalidated it.
- Broker destroy requests cooperative runtime stop before draining predefined
  broker task slots. A client that leaves a long `SLEEP_NS_RETURN_U64` command
  unjoined therefore cannot keep shutdown blocked until that sleep naturally
  expires; the task is cancelled while its broker-owned slot storage is still
  valid for the trampoline.
- Byte-stream control transports also avoid completion-driving joins whenever a
  long sleep command is pending in the broker runtime. `LLAM_BROKER_WIRE_OP_TASK_JOIN`
  returns `EAGAIN` for an uncompleted `SLEEP_NS_RETURN_U64` task and also for
  otherwise quick joins that would have to drain a peer session's pending sleep.
  This prevents one client from pinning the broker serve thread by hiding a
  client-selected delay behind another session's short control request.
- Broker teardown also scrubs broker-local authority state after the drain:
  capability MAC keys, revocation epoch, object id counters, transport subject
  sessions, and ring subject sessions are cleared before the caller-owned broker
  struct is left inactive. Capability validation wipes temporary MAC material
  after comparison, reducing key-derived residue in the trusted broker process.
- `LLAM_BROKER_RING_OP_DESCRIPTOR_READ` and
  `LLAM_BROKER_RING_OP_DESCRIPTOR_WRITE` route descriptor/HANDLE I/O through
  broker-owned slots: POSIX uses fds and Windows uses HANDLEs. The client
  submits a capability token plus shared data offset rather than holding raw
  descriptor authority. The broker duplicates the descriptor/HANDLE under its
  table lock and performs the blocking read/write on the duplicate, preventing
  slot close/reuse races without holding the broker lock during I/O.
- `LLAM_BROKER_WIRE_OP_DESCRIPTOR_READ` and
  `LLAM_BROKER_WIRE_OP_DESCRIPTOR_WRITE` expose the same descriptor/HANDLE
  authority through the bounded control transport for smoke/control RPC use.
  They require an existing broker-owned descriptor token; the wire protocol does
  not accept a client-supplied raw fd or HANDLE value as authority.
- POSIX control sockets grant descriptors with `SCM_RIGHTS` through
  `LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR`. The broker registers only the fd
  received from the kernel ancillary data path, rejects over-authorized grants,
  closes every fd received in a multi-fd ancillary array, rejects register
  requests that contain no passed descriptor, and closes a received descriptor
  when the surrounding wire header is invalid or the request operation is not
  `REGISTER_DESCRIPTOR`.
- POSIX broker authority fds are made close-on-exec at every broker boundary:
  listener, client, accepted control sockets, received `SCM_RIGHTS` fds,
  broker-owned descriptor slots, private ring mapping fds, duplicated ring
  response fds, and mapped ring fd duplicates. This prevents later helper
  `exec` calls from inheriting broker capability transport or data-plane
  authority by accident.
- Failed POSIX broker wire reads clear the caller's request/response structure
  before returning an error. A short write, early close, truncated ancillary
  message, or malformed descriptor grant therefore cannot leave attacker-chosen
  partial fields in output storage for buggy callers to reuse.
- Broker-owned buffer reads, channel receives, and descriptor/HANDLE reads clear
  caller output on failure. Successful short receives/reads also clear the
  unused suffix of the requested output window, matching the shared-ring stale
  output policy for direct broker helper calls.
- Direct broker helpers that return authority or status outputs clear those
  outputs before validation. Failed ring serving, transport ring creation,
  direct token minting, task join, and shared-memory ring create/open/import
  calls therefore leave no stale served count, minted token, task result,
  fd/HANDLE, session id, or mapping handle for an FFI caller to accidentally
  reuse after observing a nonzero return.
- Broker transport failure responses clear client-visible token, result, and
  data output fields after any response-failure rollback and again when client
  request helpers receive a semantically failed response. The response still
  carries framing metadata such as magic, version, runtime id, revocation epoch,
  failed status, and error code, but it cannot leak stale success authority to
  clients that mishandle `status < 0`.
- POSIX response-descriptor helpers treat an fd attached to a failed response as
  malformed authority. The helper closes the received `SCM_RIGHTS` fd, clears
  the descriptor output to `-1`, and scrubs token/result/data fields while
  preserving the failed response status and error code.
- Broker client request helpers clear response storage on invalid arguments,
  request-write failure, malformed response framing, and authority-bearing
  fields in successfully read failure responses. Malformed success responses
  are rejected with `EINVAL` and fully scrubbed, and response-descriptor helpers
  also close any attached `SCM_RIGHTS` fd before returning. This includes plain
  POSIX requests, descriptor-bearing POSIX requests, response-descriptor
  requests, Windows HANDLE request helpers, and unsupported Windows fd stubs.
- Broker shared-ring private-name generation and local endpoint helper failures
  scrub their output parameters before returning an error. Short name buffers
  do not retain truncated high-entropy rendezvous names, and failed listen,
  connect, accept, or unsupported endpoint helpers do not leave stale fd/HANDLE
  authority in caller-provided output storage.
- POSIX broker socket identity capture clears its output before validation.
  A failed capture therefore cannot leave stale cleanup identity that a caller
  might accidentally reuse for chmod/unlink guards.
- Broker transport subject lookup, ring-session registration, ring stats
  collection, and ring submission/completion drain helpers zero their output
  storage on invalid-input or empty-ring failures. Subject ids, session ids,
  diagnostic counters, and completion payloads therefore do not survive across
  failed control-plane calls. Completion-drain helpers reject element counts
  whose byte size would overflow with `EOVERFLOW` before multiplying; because
  such a count cannot be trusted as the real output length, only the first
  completion slot is scrubbed as a stale-authority sentinel.
- Windows named-pipe control sessions grant HANDLEs through the same register
  operation only after the broker asks the kernel for the connected peer
  process id and duplicates that peer's HANDLE with `DuplicateHandle`. The wire
  HANDLE value only selects a handle in the authenticated peer process; it is
  not broker-side authority by itself.
- Tokens minted through the local control transport are bound to a per-session
  broker subject id that is included in the token MAC. Replaying a serialized
  token on a different broker control session fails with `EACCES`. The broker
  keeps that subject stable across repeated `serve_one` calls on the same local
  control connection and forgets it when the connection stops or fails. If the
  broker cannot reserve a subject slot for a new connection, serving fails with
  `ENOSPC` instead of issuing a one-shot token that would be unverifiable on the
  next request. New subject reservation also fails once broker destroy has
  started, so shutdown cannot mint a fresh audience while new external work is
  supposed to be closed. POSIX sessions key the subject table by the socket
  object identity rather than the fd number alone, so a later connection that
  reuses the same fd does not inherit the old subject. Windows named-pipe
  sessions forget subjects on STOP/error and require that close boundary for
  one-request serving helpers.
- Nested broker helper calls carry a frame-scoped effective subject. Subject `0`
  preserves the caller's current subject for internal helpers, conflicting
  nonzero nested subjects fail with `EACCES`, and a subject introduced by an
  inner operation is restored when that operation returns.
- Shared-memory ring serving also has a subject-bound entry point. Once a broker
  ring is served with a subject id, later attempts to serve the same ring through
  subject `0` or another subject fail with `EACCES` before consuming a pending
  submission. Direct in-process broker tests may still use subject id `0`
  because they are not a hostile address-space boundary.
- `LLAM_BROKER_RING_OP_TASK_SPAWN`, `LLAM_BROKER_RING_OP_TASK_JOIN`, and
  `LLAM_BROKER_RING_OP_TASK_DETACH` route predefined broker-owned task
  commands through the same capability path. Clients do not submit raw function
  pointers; the broker selects the command implementation and returns only
  join/detach authority. The ring path rejects task command ids outside the
  32-bit predefined enum range before casting, so high-bit values cannot
  truncate into a valid broker command.
- Pending task joins on both transports are non-authoritative progress hints.
  Ring joins publish `EAGAIN` until the task has completed; the byte-stream
  transport may drive short predefined commands for request/response smoke paths,
  but long sleep commands remain retry-only to avoid control-plane starvation.
- Global revocation remains a trusted in-process broker-management API. The
  local control transport rejects `LLAM_BROKER_WIRE_OP_REVOKE_ALL` with
  `EACCES`; client-visible revocation must present object-specific destroy
  authority through attenuation/revoke operations.

The high-throughput data plane is still not a complete runtime RPC surface. The
current tree has bounded control-transport buffer/channel/descriptor/task
requests for small messages, the ring layout, POSIX/Windows shared-memory mapping,
broker-side capability validation, attenuation, and object generation rotation
over the ring, explicit buffer-grant validation, broker-owned buffer read/write
copies through the shared data window, broker-owned byte-channel
send/receive/close routing, broker-owned descriptor/HANDLE read/write routing
over both control transport and rings, and a small predefined task-command set
covering scalar return, increment, popcount, and cooperative sleep/return work.
The remaining work is to turn this
into a broader runtime RPC surface while keeping raw descriptor/HANDLE
authority inside the broker unless an explicitly unsafe fast path is selected.

## Rights

Capability rights are attenuable. A holder can be given only the operations it
needs:

```text
SEND
RECV
JOIN
DETACH
CLOSE
DESTROY
READ
WRITE
ADMIN
```

Rights can be reduced by asking the broker to MAC a new token with a subset of
the original rights. The client never needs access to the broker key for
least-privilege delegation, and attempts to request rights outside the source
token fail with `EACCES`.

Raw token minting is intentionally not exposed on the untrusted control
transport. Clients may create broker-owned buffers/channels through bounded
grant operations, validate, attenuate, revoke, or use tokens that were issued
by trusted broker object-creation paths, but a direct transport request to mint
an arbitrary `(family, slot, generation, rights)` token fails closed with
`EACCES`.

All token-producing helpers clear their output token before validating inputs.
Invalid rights, invalid generations, allocation failure, entropy failure, or
object creation failure therefore cannot leave a stale previous token in caller
storage that might be mistaken for a newly issued authority.

## Revocation

The broker tracks a global revocation epoch. Tokens include the epoch at
issuance time. When the broker advances the epoch, older tokens fail validation.

Broker-owned buffers, descriptors, and byte channels also support
object-specific revocation by rotating the object's generation. Existing tokens
remain cryptographically well-formed, but object authorization fails because the
slot no longer matches the token generation. Broker validation checks both the
token MAC and the current live object slot, so a rotated or destroyed token fails
even when its serialized MAC is still well-formed. The broker can return a
replacement token with a subset of the object's rights in the same operation.
Invalid replacement rights fail before generation rotation, so a rejected request
does not accidentally revoke the object.

## Non-Goals

The broker capability model does not replace OS isolation. For untrusted code,
combine broker mode with process sandboxing, seccomp/AppContainer-like policy,
containerization, or language-runtime isolation where appropriate.

The in-process sealed handle model should not be described as protection
against arbitrary same-process memory read/write.

## Verification

Run the focused security suite before changing broker or sealed-handle code:

```bash
make test_security_capability llam_broker
./test_security_capability
./llam_broker --self-test
```

`make test` also exercises `llam_broker --serve-n` with multiple client
self-tests so control-transport subject binding and malformed-session isolation
stay covered by the default gate.
