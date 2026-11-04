# Public handles

Treat public handles as uninterpreted values. LLAM uses them to reject stale,
consumed, and wrong-family access before it touches object storage, and
wrong-owner access before it touches queues or payload.

LLAM public handles are opaque lifetime guards, not cryptographic process
sandbox capabilities.  Callers must treat task, channel, mutex, condvar,
cancellation-token, task-group, and owned-buffer handles as uninterpreted values
returned by LLAM.

The runtime rejects stale, consumed, and wrong-family handles in the slot table,
before dereferencing object storage. It then reads the owner tag from the
proven-live object and rejects a wrong runtime owner before touching wait queues
or payload. Family-tagged handle generations
carry a sealed verifier token produced by a slot-stable affine permutation over
the internal epoch space. The permutation is derived from table secret material
and slot identity, so the first handle in a family and the next handle after
slot reuse are not the trivial `slot + family` or monotonic `generation + 1`
values, and consumed generations do not repeat before epoch exhaustion. This
protects FFI bindings and accidental in-process misuse from turning guessed or
old values into valid objects.

## What handles do not protect

This is not an isolation boundary against malicious code that can read or write
arbitrary memory inside the same process.  If an attacker can disclose runtime
internals, they may also disclose handle table state or secrets; embedders that
need isolation must use process, sandbox, or language-runtime boundaries above
LLAM.

## Broker foundation

The 2.x tree also contains an internal broker capability foundation for a
stronger, process-boundary model. The broker owns runtime state, MAC keys,
registries, descriptors/HANDLEs, and ring sessions outside the untrusted client
address space. Clients hold serialized, subject-bound tokens with explicit
rights and revocation epochs. Current broker coverage includes local POSIX
Unix-domain and Windows named-pipe control transports, bounded buffer/channel
grants, POSIX `SCM_RIGHTS` descriptor grants, Windows `DuplicateHandle` HANDLE
grants, private shared-memory ring setup, token attenuation/revocation,
broker-owned descriptor/channel/buffer data-plane requests, and predefined task
commands. These symbols are internal implementation details, not part of the
stable public ABI; embedders should treat them as the broker-mode foundation
documented in the [security model](../security/index.md).
