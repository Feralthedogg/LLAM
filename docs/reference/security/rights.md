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

# Rights and revocation

Give each token only the operations its holder needs, and revoke tokens when
that changes.

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
Epoch `0` is reserved for cleared or uninitialized broker state; token issue
and validation reject it with `EINVAL`.

Broker-owned buffers, descriptors, and byte channels also support
object-specific revocation by rotating the object's generation. Existing tokens
remain cryptographically well-formed, but object authorization fails because the
slot no longer matches the token generation. Broker validation checks both the
token MAC and the current live object slot, so a rotated or destroyed token fails
even when its serialized MAC is still well-formed. The broker can return a
replacement token with a subset of the object's rights in the same operation.
Invalid replacement rights fail before generation rotation, so a rejected request
does not accidentally revoke the object.
