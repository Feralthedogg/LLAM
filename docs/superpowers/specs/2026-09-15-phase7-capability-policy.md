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
See LICENSES/Apache-2.0.txt.
-->

# Phase 7 capability and fallback policy

## Scope

This phase adds private runtime capability/fallback state around the existing
cooperative preemption modes and the opt-in Linux x86-64 context experiment.
It does not add public C structs, symbols, stats fields, or language bindings.
Compiler-emitted `.llam.preempt` records are not automatically loaded or
registered; callers must use the existing experimental safe-range registration
path before enabling async capture.

## Policy

| Requested mode | Effective policy | Async range behavior |
| --- | --- | --- |
| `OFF` | off | No automatic requests or capture |
| `COOPERATIVE` | cooperative | Safepoint behavior only; async is refused |
| `AUTO` / `STRICT`, backend unavailable or disabled | cooperative | Requests fall back to LLAM boundaries; unsupported enable returns `ENOTSUP` |
| `AUTO` / `STRICT`, backend active with ranges | hybrid | Registered safe PCs may capture; unknown/unregistered PCs defer to a later safepoint |

The capability record retains compiled-backend bits (including the version-1
metadata parser), async backend state,
registered-range count, and a typed failure reason. Default `AUTO` does not
implicitly enable signal handlers, choose a signal, register code, or change the
runtime's cooperative default.

## Observability and ABI

The internal text dump reports effective policy, compiled backends, async
state, fallback reason, registered safe ranges, async requests/captures/resumes,
deferred unsafe/stale requests, signal collisions, copied context bytes, and
maximum request-to-capture acknowledgment latency. It also retains the existing
foreign detach/reacquire and relief-worker counters. Public statistics and the
JSON stats schema are unchanged. An ABI-minor capability query can be considered
later if consumers require a stable programmatic capability interface.
