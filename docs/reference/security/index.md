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

# Security model

LLAM has two different handle models with different threat boundaries.

These pages are intentionally explicit about the boundary: the default public
C handles are in-process misuse hardening, while broker mode is the direction
for isolation from untrusted code. Broker symbols are internal implementation
details in the current tree; they are tested and shipped as a foundation, not as
a stable public C ABI.

## In-process opaque handles

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

## Non-goals

The broker capability model does not replace OS isolation. For untrusted code,
combine broker mode with process sandboxing, seccomp/AppContainer-like policy,
containerization, or language-runtime isolation where appropriate.

The in-process sealed handle model should not be described as protection
against arbitrary same-process memory read/write.

## Verification

Run the focused security suite before changing broker or sealed-handle code:

```sh
make test_security_capability llam_broker
./test_security_capability
./llam_broker --self-test
```

`make test` also exercises `llam_broker --serve-n` with multiple client
self-tests so control-transport subject binding and malformed-session isolation
stay covered by the default gate.
