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

# LLAM allocator algorithm upgrade

Independent research and local integration completed on 2026-10-08. Pro was not used, as explicitly requested. This is a self-reviewed local handoff; it is not merged, externally reviewed or deployed.

## Result and activation path

`llam_task_alloc`'s unmanaged/foreign branch now uses `src/core/memory/task_span.c`; its external return branch updates the same span. Allocation starts at one slot, grows geometrically to an OS-page-density choice of no more than 16 slots, prefers partially occupied spans, and caches only two idle spans per allocation processor. Small ramps stay on the heap; mature spans use a dense 1–4-page mapping, with a tracked heap fallback. Reclaim checks all public pins/scan references under registry exclusion and uses direct unlink links for known allocator-owned entries. Busy spans are quarantined; owner safepoints retry bounded work. Detached resources/pages are destroyed after the allocator mutex is dropped. No new managed alloc/free atomic operation was introduced.

This is a focused upgrade of the host-task pool. The managed task cache, waiter/timer/I/O pools and process-global slot table retain their existing designs. It is not a replacement for general malloc. The new mature span capacity is 12 on this macOS ARM64 host; it is derived from page size and private TCB size, not hard-coded for another OS.

## Evidence and rationale

The adopted concepts are page-local free lists and occupancy ([mimalloc report](https://www.microsoft.com/en-us/research/uploads/prod/2019/06/mimalloc-tr-v1.pdf)) plus packing/bounded empty-space policy ([Hoard](https://people.cs.umass.edu/~emery/pubs/berger-asplos2000.pdf)). Literature, exact sections inspected, alternatives and limitations are in [evidence.md](evidence.md). No upstream allocator code/dependency was imported; this is first-party Apache-2.0 C11 code. We defer sender-side remote batching and physical compaction, not claim them disproved.

An initial heap-only candidate returned storage to libc but did not lower RSS immediately. A mapped candidate then exposed aggregate quadratic registry scanning at large bursts. The final direct unlink mechanism avoids scanning unrelated registry entries for a known span. These exploratory failures and their unfavorable samples remain in the artifacts.

## Project comparison

Baseline is the actual already modified `main` working tree at HEAD `34dcc74a7e3cbb584967f6bf5f7816673ed5169b`, not a clean checkout. Frozen affected headers/archive and a hashed starting-state record bind the comparison. Final task patch SHA256: `10e0b79ac6f9a53f97efc1d3b1a119cdbbd733096b1e2177e8e1f385e247280a`. Earlier buffer/TCB/stack changes and other user edits were preserved.

Five alternating fresh process pairs, same Clang `-O2` runtime builds, balanced profile and 10 workers, measured:

| Fixture | Baseline | Integrated candidate | Scope |
| --- | ---: | ---: | --- |
| 4096 metadata tasks, cached allocator storage | 5,644,768 B | 238,528 B | -95.8%, includes unchanged normal prewarm |
| Same fixture, post-return RSS | 9,011,200 B | 3,325,952 B | -63.1%, macOS process observation |
| 16384 metadata tasks, cached allocator storage | 21,963,232 B | 238,528 B | historical peak no longer sets pool size |
| First single host task, backing storage | 206,608 B | 206,960 B | +352 B including prewarm/bookkeeping |
| Public host spawn 4096, cached allocator storage | 5,644,768 B | 534,880 B | distributed over 10 allocation Ps |
| Public host spawn 4096, allocation time | 4.32 ms | 5.12 ms | adverse cold-burst cost |
| Same public workload, join/reclaim time | 0.50 ms | 0.88 ms | actual page/slot reclamation cost |

The metadata fixture allocates no task stacks; its allocator percentages are not whole-workload RAM savings. The public fixture reaches real runtime spawn/run/consuming-join APIs. An untouched development-size alternative, 8192 tasks with preselected seeded shuffled releases, confirmed the zero-live bound in three fresh pairs. The public confirmation also showed slower cold allocation/join; see [confirmation summary](artifacts/confirmation-summary.json).

Hot-runtime five-pair p50 changes stayed within 3%. A sparse 51-round timer p99 signal triggered longer checking; a separate nine-pair, 511-round timer check reported about +0.5% p50 and +0.2% p99. These are local desktop diagnostics with ordinary background activity, not evidence of universal equivalence or release-quality latency. Exact commands/worker reports/raw runs are retained, including unfavorable cases.

## Acceptance and verification

AC1 PASS locally: ABI/exports, handles, ownership, pinned quarantine, consuming joins, partial init/fault cleanup, mapped fallback and peer-runtime checks. AC2 PASS under the stated zero-live/unpinned conditions: two idle spans at most; maximum 16 slots. AC3 PASS against the proposed common-hot-path investigation thresholds; host-burst reclaim cost is explicit. AC4 PASS: connected real path, normal tests, regression comparisons, documentation, notices, source self-review and reversible task patch.

- `make -j4 CC=clang all test`: PASS, including stress/fuzz and shared/API gates.
- `make -j4 CC=clang test-asan`: PASS; existing custom-stack warnings remain.
- CMake registration plus four affected CTest cases: PASS.
- Ubuntu 24.04 ARM64 Docker/GCC: ten affected suites PASS, including concurrent host-span tests and stress/fuzz.
- Strict MkDocs build and license policy: PASS. The pre-existing tooling-doc link was fixed concurrently outside this task and is excluded from the allocator patch. Historical status/failure logs are archived as gzip; the patch has its required license sidecar.
- Source self-review and reverse-patch check on a disposable copy: PASS. No blinded separate model or Pro review occurred.

Native Windows, BSD, Linux x86_64, TSan and external CI/reviewer approval were not run. The local readiness label does not qualify those platform release gates. Existing context ABI prefix is preserved; private TCB size is 1280 B on this host (previously 1264 B), with the registry link fitting tail padding after span metadata.

## Counterexamples and remaining limits

One survivor per span prevents reclaiming partially live storage. Persistent public pins or registry contention can retain quarantined spans; inactive runtimes retry on activity or teardown. Heap fallback storage may stay resident in libc. Global handle slots retain generation/tombstone capacity to preserve stale-handle safety. OS mapping operations and public global locking have platform-dependent costs. Neither model agreement nor literature benchmark percentages were used as LLAM validation.

## Local handoff and reversal

Changed paths/purposes are in [changes.json](artifacts/changes.json); exact gates/results are in [verification.json](artifacts/verification.json). Review [allocator-upgrade.patch](artifacts/allocator-upgrade.patch). To revert only this task, first run `git apply --reverse --check research/allocator-algorithm-upgrade-2026-10-08/artifacts/allocator-upgrade.patch`; if unchanged relevant hunks match, apply that reverse patch. The check was executed on a disposable copy; actual rollback was not run. Do not reset the repository to HEAD, which would discard unrelated earlier work.

Research status: COMPLETE. H1 supported for the recorded workload and lifetime conditions, not formally verified C or a novel general allocator. Adoption: INTEGRATED. Merge readiness: READY_FOR_MERGE for the tested local scope, with explicit platform limits. No remote actions or Pro messages.
