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

# Broker transport

The broker accepts local control sessions over POSIX Unix-domain sockets and
Windows named pipes. This page covers how sessions start, how descriptors and
HANDLEs cross the boundary, and how the broker shuts down.

## Control sessions

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
- Windows terminal responses allow the peer to read and close within the
  250 ms absolute frame deadline before the pipe instance is disconnected,
  because a disconnect discards unread replies. The wait is bounded even if the
  peer stops reading; session authority is already revoked before this final
  transport wait.
- The local control transport can create bounded broker-owned buffer and byte
  channel grants, then exercise fixed-size buffer read/write, byte-channel
  send/receive/close, and predefined task spawn/join/detach requests through
  those grants. Clients choose only size/capacity/task kind and a subset of
  family-specific rights; `ADMIN`, oversized buffers, over-capacity channels,
  invalid task commands, and payloads larger than `LLAM_BROKER_WIRE_DATA_BYTES`
  fail closed instead of minting authority over arbitrary broker objects or
  copying unbounded data through the control pipe.

## Descriptors and identity

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
  authority by accident. Where the platform exposes atomic close-on-exec
  primitives, broker sockets use `SOCK_CLOEXEC`, accepted control sockets use
  `accept4(SOCK_CLOEXEC)` on Linux, POSIX shm-backed ring fds use `O_CLOEXEC`,
  descriptor/ring duplicates use `F_DUPFD_CLOEXEC`, and received ancillary fds
  use `MSG_CMSG_CLOEXEC` before falling back to post-creation
  `fcntl(FD_CLOEXEC)`.
- Windows broker-registered HANDLEs clear `HANDLE_FLAG_INHERIT` during
  descriptor registration. This mirrors the POSIX close-on-exec rule and keeps
  broker descriptor authority out of child processes created with handle
  inheritance enabled.

### Clear output on failure

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

### Client identity

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
  one-request serving helpers. Subject allocation also rejects a duplicate live
  subject inside the broker session table; even a pathological entropy/mixing
  collision fails closed instead of making two sessions mutually replayable.

## Locks and teardown

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
