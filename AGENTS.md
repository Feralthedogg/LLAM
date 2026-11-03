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

# Working on LLAM

LLAM is a C11 stackful N:M user-thread runtime with a public C ABI. Linux uses
io_uring/liburing, macOS uses kqueue, and native Windows uses IOCP.
Runtime correctness, ABI compatibility, and scheduler latency are central to
changes in this repository.

## Context to use when relevant

Follow the affected code and its tests; use these references for the named
concern. Reading every document is not a prerequisite for a small edit.

| Concern | Reference |
| --- | --- |
| Public API, FFI, struct layouts, errors, handle semantics | [Public headers](include/llam/), [ABI contract](docs/abi.md), [API reference](docs/reference/api.md) |
| Scheduling, lifecycle, synchronization, I/O ownership | [Architecture](docs/internals/architecture.md); implementation in `src/core/`, `src/engine/`, `src/io/`, and private declarations in `src/internal/` |
| Platform setup and build registration | [Build guide](docs/build.md), [Platform support](docs/reference/platform-support.md), `Makefile`, `CMakeLists.txt`, and the affected workflow in `.github/workflows/` |
| Test selection, platform gates, release verification | [Testing strategy](docs/internals/testing-strategy.md), [Verification](docs/operations/verification.md) |
| Public-handle hardening or broker boundaries | [Security model](docs/security.md) |
| Performance measurements and comparisons | [Benchmarks](docs/operations/benchmarks.md) |
| New files, file moves, contributions, licensing | [Contributing](CONTRIBUTING.md), [Licensing](docs/licensing.md) |

`experiments/lcwe/` contains a standalone cost model. Research plans and reports
under `docs/research/` describe experiments, not the implemented public contract.
Keep experimental code outside the production runtime unless integrating it is
part of the requested work.

## Runtime constraints

- Preserve the documented ABI unless an incompatible change is explicitly in
  scope. Within an ABI major, extend public structs only at the tail and retain
  fixed-width fields, caller-size handshakes, compatible-prefix copies, and
  defaults for omitted fields. Keep public API documentation and export checks
  consistent with header changes; internal symbols are not public API.
- Preserve runtime ownership and handle lifetime rules. Resolve and validate
  public handles before dereferencing object storage; retain stale-handle,
  wrong-owner, active-pin, and consuming join/detach behavior, including the
  documented `EINVAL`, `EXDEV`, and `EBUSY` cases. A runtime's stop or shutdown
  must not stop a peer runtime.
- For queue, waiter, cancellation, completion, or reclamation changes, account
  for publication order, lock ownership, and object lifetime. Explain changed
  atomic memory ordering at the relevant code. Preserve wakeup and cleanup
  behavior across timeout, cancel, close, and shutdown races.
- Managed waits park tasks. Route foreign blocking work through LLAM's blocking
  integration instead of blocking scheduler workers. Compatibility constructors
  retain request-based boundary/safepoint preemption. Managed constructors use
  asynchronous capture only after compiler/image admission and native M probes;
  arbitrary unqualified C loops have no asynchronous interruption guarantee.
- Keep backend-specific behavior behind the appropriate platform guards.
  macOS uses kqueue; BSD support ended in 3.0.0. Darwin Mach facilities stay guarded.
  Context changes must keep `src/asm/` layouts and C context definitions aligned
  for the affected architecture.
- Register added or moved production sources and tests in both `Makefile` and
  `CMakeLists.txt` where applicable. Keep fault-injection hooks in test builds.
  Follow nearby C conventions and preserve the default warnings-as-errors build.
- In-process opaque handles defend against misuse and stale access. The broker
  process boundary provides isolation from hostile clients; preserve that
  distinction in implementation and documentation.

## Verification proportional to the change

Build and run the affected test targets first. For example, on macOS:

```sh
make -j4 CC=clang test_runtime_core
./test_runtime_core
```

Linux supports GCC or Clang. Native Windows uses
the CMake/CTest path in the build guide. With a configured CMake build:

```sh
cmake --build build --target test_runtime_core
ctest --test-dir build --output-on-failure -R '^test_runtime_core$'
```

| Change | Relevant starting checks |
| --- | --- |
| Public ABI or exports | `test_abi_contract`, `test_abi_compat`, `test_shared_load`, `make audit-shared-exports` |
| Lifecycle, scheduling, ownership, reclamation | `test_runtime_core`, `test_multi_runtime_core`, `test_runtime_shutdown_internal`, `test_runtime_invariants`; add stress/fuzz for concurrency changes |
| Channels, select, cancellation, synchronization | `test_sync_primitives`, `test_runtime_api_edges`, `test_runtime_select_edges` |
| I/O and buffers | `test_connect_io`, `test_io_buffers`, `test_runtime_io_dump`; native Windows IOCP/HANDLE tests for that backend |
| Broker or capability handling | `test_security_capability`, `llam_broker --self-test` |
| Python tooling or LCWE model | The matching `scripts/test_*.py`; `make test-lcwe-model` for LCWE |
| Documentation or file notices | Check changed paths, links, and notices; `mkdocs build --strict` for documentation-site changes; `make check-license-policy` for license-policy changes |

Make targets named `test_*` build binaries; execute those binaries separately.
For commands with arguments, such as shared-load checks, use the invocation in
`Makefile` or CTest. Reduce runtime bugs found through server stress into direct
runtime regression tests. Lossless server tests require exact delivery;
best-effort flood tests allow only accounted-for drops.

Use `make test` for changes spanning runtime components. For submission, follow
the existing `CONTRIBUTING.md` gate: `make -j4 all test` and
`make check-license-policy`. Select sanitizer, stress, soak, and platform gates
when the affected failure mode or release scope calls for them; a documentation
edit does not need runtime stress. Use `make test-asan` / `make test-tsan` so
sanitizer objects stay separate. Linux TSan is diagnostic in CI, not a substitute
for the ordinary correctness gates. A pass on one OS does not validate another
backend. Report untested platforms and unavailable checks explicitly.

For performance changes, compare affected cases under matching build settings,
worker counts, workloads, and host conditions. Retain commands and raw samples.
Use isolated cases for release-quality comparisons and scheduled artifacts for
public performance tables. Throughput alone does not establish correctness or
latency behavior.

## Scope and completion

- Preserve pre-existing working-tree changes and local measurement artifacts.
  Limit edits to the requested outcome and fixes needed to make it work.
- Proceed with local edits, builds, focused tests, and fixes within the request.
  Finish relevant verification instead of stopping after an initial patch.
  Ask only when a missing decision materially affects scope, compatibility, or
  an action outside the existing authorization.
- Preserve copyright attributions. All current first-party files use the
  Apache-2.0 notice from `CONTRIBUTING.md`; keep third-party notices and terms
  intact. The former mixed-license manifest is no longer a selection rule.
- Report what changed, what was actually verified, and any remaining blocker.
  Do not broaden or repeat passing checks without a new change or unresolved
  concern. Keep this file focused on durable repository constraints; update
  stale guidance instead of accumulating task-specific instructions.

Instruction design reference:
[Rethinking skills and prompts for GPT-6 Astra](https://developers.openai.com/blog/rethinking-skills-and-prompts-for-gpt-6-astra).
