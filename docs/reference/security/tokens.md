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

# Broker tokens and keys

Broker mode moves authority out of the untrusted process. The client holds
serialized tokens; the broker holds the keys and validates every token before
it acts. Broker symbols are internal in the current tree, not a stable public C
ABI.

## Trust boundary

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

## Tokens and keys

The current 3.0.0 implementation provides the following internal token and
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

## Validation

- Broker validation binds every token to the live broker runtime id after MAC
  validation. A structurally valid token for a different runtime fails closed
  even if internal test code signs it with the same key material.
- Raw and client-visible validation require a nonzero requested-rights mask.
  Zero-right validation fails with `EINVAL`, so broker transports and direct
  helpers cannot be used as a rightless live-token oracle.
- Raw capability issuance and validation reject slot zero with `EINVAL`. Broker
  object slots are nonzero, so slot zero cannot become a structurally valid
  authority outside live-object table checks.

## Minting and grants

- Direct broker object creation and direct object-token minting use the same
  family-specific rights allowlist as the transport path. Buffer, descriptor,
  channel, and predefined-task tokens cannot carry rights from another family or
  future-reserved bits.
- Client-writable broker grant requests must also carry an explicit nonzero
  rights mask. Zero-right `CREATE_BUFFER`, `CREATE_CHANNEL`, and
  `REGISTER_DESCRIPTOR` requests fail with `EACCES` instead of defaulting to the
  maximum rights allowed for that object family.
- Direct broker buffer registration uses the same bounded grant limit as the
  transport path. Buffer grants larger than `LLAM_BROKER_BUFFER_MAX_BYTES` fail
  with `EINVAL` before allocation.
- Unsupported platform compatibility stubs that would otherwise return broker
  authority, such as Windows `llam_broker_register_fd()`, clear token outputs
  before reporting `ENOTSUP`.

## Failure and revocation

- Failed attenuation and object-revocation calls clear output token storage,
  including in-place `token == out_token` calls. Treat a nonzero return as
  owning no fresh serialized authority.
- Object-specific revocation is atomic with respect to replacement-token
  issuance. If nonce entropy is unavailable, revocation fails with `EIO` and
  leaves the old object generation valid.

## Test coverage

- `test_security_capability` verifies rights checks, tamper rejection,
  broker-owned attenuation, wrong-key rejection, foreign-runtime rejection,
  family-rights allowlist rejection, zero-slot issuance rejection,
  zero-right validation rejection,
  explicit-rights grant rejection,
  malformed control-dispatch fail-closed output scrubbing,
  malformed shared-ring fail-closed completion/output scrubbing,
  failed-output token clearing,
  secure-entropy fail-closed key creation, atomic object-specific revocation,
  POSIX shared-memory ring mapping,
  broker-side ring capability validation/attenuation/revocation,
  buffer-grant bounds, control-transport buffer/channel data-plane requests,
  and the POSIX socketpair request path.
