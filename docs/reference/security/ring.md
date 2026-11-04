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

# Shared ring

The shared ring is the broker's data path. Clients submit operations through
shared memory, and the broker validates each one before it applies it.

## Ring setup

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
  Broker-owned sessions preserve the complete mapping authority, including the
  POSIX shm name when a named mapping is registered internally, so
  `SERVE_RING` poison, explicit session forget, and broker destroy can unlink
  named rendezvous objects rather than only closing the fd/HANDLE. Cleanup is
  keyed by broker-owned authority instead of the active-session marker, so
  partially invalidated sessions still release their mapped resources. Reusing
  an inactive ring-session slot first reclaims any stale broker-owned mapping
  authority before the new session overwrites the slot.

### Serve a ring

- `LLAM_BROKER_WIRE_OP_SERVE_RING` drives submissions from a
  transport-created ring by broker session id. A zero request length keeps the
  original single-request behavior; a nonzero length asks the broker to serve a
  bounded batch and is capped at `LLAM_BROKER_RING_SERVE_BATCH_MAX`. The
  response `result0` reports the number of submissions served. The session id
  is scoped to the control-transport subject; invalid or foreign sessions fail
  before consuming a client submission. Session-id serving claims the session
  busy state while still holding the broker table lock, so response-failure
  cleanup cannot unmap the broker-owned ring between lookup and execution.

### Disconnects

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
  Buffer slot reset also reclaims stale broker-owned heap storage before an
  inactive slot is reused, so partially invalidated buffer grants cannot be
  overwritten by later registrations.
  Task rollback atomically claims a spawned slot and clears client-visible
  rights, then the predefined task detaches itself from inside its trampoline
  before broker-slot reset. This prevents both permanent detached-but-active
  broker slots and host-thread detach races with task exit/reclaim.
  Client-visible predefined task detach uses the same atomic claim: if a task
  completes concurrently, detach observes the completed state and consumes the
  handle from that path instead of overwriting it with a stale detached state.

### Buffer operations

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

## Progress cursors

- Broker-side ring consumers keep submission/completion progress cursors in
  broker-private memory and validate the client-controlled head/tail windows
  before reading or writing slots. Corrupt, inverted, or oversized windows fail
  closed instead of letting a client rewind shared counters and replay stale
  ring entries as new work.
- Broker-published progress cursors must continue to mirror the broker-private
  session cursors. If a shared ring is reset in place or a client-writable
  cursor is corrupted after a session is established, serving fails closed with
  `EINVAL` rather than resynchronizing from shared memory. This prevents a
  stale session from skipping fresh submissions or publishing a completion-tail
  gap that exposes zeroed or stale completion slots as successful responses.
  The broker rechecks those cursors again after any in-flight operation returns
  and before publishing completions; on mismatch it clears any validated output
  window, invalidates the private session, and reclaims broker-owned mappings
  instead of exposing a completion for work that ran under a corrupted cursor
  view.

## Descriptor operations

- `LLAM_BROKER_RING_OP_DESCRIPTOR_READ` and
  `LLAM_BROKER_RING_OP_DESCRIPTOR_WRITE` route descriptor/HANDLE I/O through
  broker-owned slots: POSIX uses fds and Windows uses HANDLEs. The client
  submits a capability token plus shared data offset rather than holding raw
  descriptor authority. The broker duplicates the descriptor/HANDLE under its
  table lock and performs the blocking read/write on the duplicate, preventing
  slot close/reuse races without holding the broker lock during I/O.
  Descriptor cleanup is keyed by broker ownership, not only by the live-slot
  marker: a slot with `close_on_destroy` and a valid fd/HANDLE is reclaimed
  during broker destroy even if an interrupted internal lifecycle has already
  cleared the active marker. The same ownership check runs before an inactive
  descriptor slot is reused, so a later registration cannot overwrite and
  strand an older broker-owned fd/HANDLE.
- `LLAM_BROKER_WIRE_OP_DESCRIPTOR_READ` and
  `LLAM_BROKER_WIRE_OP_DESCRIPTOR_WRITE` expose the same descriptor/HANDLE
  authority through the bounded control transport for smoke/control RPC use.
  They require an existing broker-owned descriptor token; the wire protocol does
  not accept a client-supplied raw fd or HANDLE value as authority.

## Task operations and limits

- Nested broker helper calls carry a frame-scoped effective subject. Subject `0`
  preserves the caller's current subject for internal helpers, conflicting
  nonzero nested subjects fail with `EACCES`, and a subject introduced by an
  inner operation is restored when that operation returns. The subject frame
  stack is bounded; an overflow attempt fails with `EOVERFLOW` without changing
  the current subject or poisoning later broker operations.
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

### Remaining work

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
