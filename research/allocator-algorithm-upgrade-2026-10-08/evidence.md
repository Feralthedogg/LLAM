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

# Evidence

Search cutoff: 2026-10-08. Public primary sources only; Pro disabled by explicit user instruction.

## Search map and original material

Queries covered page-local free lists/empty-page reclamation; producer-consumer batching; Hoard fullness thresholds; virtual spans; compaction; and 2025–2026 allocator/reclamation updates. Routes: public web search, Microsoft original reports/publication records, original university PDFs, proceedings records, arXiv discovery, and official jemalloc/mimalloc documentation. Search results on Reddit/Wikipedia/marketing mirrors were excluded as evidence. This is a targeted engineering review, not systematic or exhaustive. Newer safe-reclamation and MicroVM preprint leads are adjacent settings and not necessary to replace LLAM's existing pin protocol.

- **E-P1:** Daan Leijen, Benjamin Zorn, Leonardo de Moura, *Mimalloc: Free List Sharding in Action*, MSR-TR-2019-18, June 2019, technical report v1. [Original](https://www.microsoft.com/en-us/research/uploads/prod/2019/06/mimalloc-tr-v1.pdf). Original inspected: sections 2.1–2.4 and 3.3–3.4, PDF pages 3–8, extracted locally after web section retrieval failed. Page-local lists and occupancy identify empty pages; bounded maintenance and separating full pages avoid exhaustive scans. Transfer: LLAM has fixed-type objects and stable P ownership, so a host-task span can track occupied slots. No claim that its general-malloc benchmark gains transfer. No code imported. Publication record checked; no notice found there, not a universal retraction check.
- **E-P2:** Paul Liétar, Theodore Butler, Sylvan Clebsch, Sophia Drossopoulou, Juliana Franco, Matthew J. Parkinson, Alex Shamis, Christoph M. Wintersteiger, David Chisnall, *snmalloc: A Message Passing Allocator*, ISMM 2019, DOI 10.1145/3315573.3329980. [Original PDF read](https://www.microsoft.com/en-us/research/uploads/prod/2020/04/snmalloc.pdf), sections 1–2, particularly batching and slab metadata. Microsoft plus university authors; overlaps the industrial research strand, not independent confirmation of P1. Its batching informs an alternative; LLAM already drains remote lists and adding sender caches would introduce runtime-lifetime obligations. Defer that alternative. No code imported; correction metadata not exhaustively checked.
- **E-P3:** Emery D. Berger, Kathryn S. McKinley, Robert D. Blumofe, Paul R. Wilson, *Hoard: A Scalable Memory Allocator for Multithreaded Applications*, ASPLOS 2000. [Original](https://people.cs.umass.edu/~emery/pubs/berger-asplos2000.pdf), sections 2–4 inspected. University work; per-heap fullness policy bounds retained space and favors occupied superblocks. Importantly the original implementation transfers empty superblocks to a global heap, not necessarily the OS. Transfer only packing/bounding ideas: LLAM must retain runtime ownership and cannot globally mix live objects across runtimes. Our two-empty-span policy is a new LLAM parameter, not Hoard's original bound. No notice found in the checked original/DBLP record; no universal absence claimed.
- **E-P4:** Martin Aigner, Christoph M. Kirsch, Michael Lippautz, Ana Sokolova, *Fast, Multicore-Scalable, Low-Fragmentation Memory Allocation through Large Virtual Memory and Global Data Structures*, OOPSLA 2015, DOI 10.1145/2814270.2814294. [Original](https://ckirsch.github.io/publications/conferences/OOPSLA15-Scalloc.pdf), sections 2–3/span-pool threshold inspected. Independent University of Salzburg work. Virtual spans and purge thresholds offer a competing reclaim route; aggressive purge can cost performance. Defer large virtual reservations for this fixed-size, cross-platform pool. Benchmark and virtual-address assumptions differ from LLAM; not reproduced. Update notices not checked beyond source/venue records.
- **E-P5:** Bobby Powers, David Tench, Emery D. Berger, Andrew McGregor, *Mesh: Compacting Memory Management for C/C++ Applications*, PLDI 2019, DOI 10.1145/3314221.3314582. [Original](https://people.cs.umass.edu/~mcgregor/papers/19-pldi.pdf), introduction and meshing mechanism inspected. University work related to Hoard's author lineage, not a separate replication. It preserves object addresses using physical-page remapping and randomized placement. Relevant counterexample: one survivor at the same offset on every span prevents trivial compaction. LLAM's partial spans can remain resident; full physical compaction is deferred because of remapping, synchronization and platform requirements. No benchmark reproduced; update notices not exhaustively checked.
- **E-P6:** Daan Leijen, [Microsoft's May 13, 2026 mimalloc article](https://www.microsoft.com/en-us/research/blog/mimalloc-a-high-performance-scalable-memory-allocator-for-the-modern-era/). Original fast/free-path and page-map discussion inspected. This is a current company technical article, not a new peer-reviewed paper or independent replication of P1. It describes occupancy and page lookup; its production performance claims are not evidence for LLAM. The article says a further technical report is forthcoming, so no unseen report is claimed.
- **E-P7:** [Official jemalloc manual](https://jemalloc.net/jemalloc.3.html), dirty/muzzy decay and cache sections inspected. Implementation documentation, not a paper. It establishes purge-policy alternatives and reinforces that releasing allocator ownership need not immediately lower OS RSS. We do not substitute jemalloc or import its code.

## Claims, predictions and transfer contract

**C1 / supported by current source:** the original host-task free list and one-object chunks persist until shutdown. Stage-2 baseline retained 4,096 host TCBs; fresh confirmation will bind this to the current source snapshots.

**H1 v1 / prototype:** adaptive 1/2/4/8/16-item host spans, span-local LIFO lists, partial-first allocation, two empty spans, and quarantined busy spans. Prediction: zero correctly unpinned live tasks retain at most 32 span slots per P, independent of historical burst size. A simulation (E-M1, `artifacts/span-model.json`) passed burst boundaries and 50,000 seeded mixed operations. It does not validate C concurrency or public handles. Counterexample: one survivor per span retained 4,111 reserved slots for 260 live cells; partially-live spans cannot be freed.

**H2 / deferred:** producer-side batched remote returns could reduce atomics, but current owner draining already batches returns and sender caches must survive peer runtime shutdown safely. No prototype or performance claim.

**H3 / not selected:** freeing standalone host TCBs individually bounds memory but retains per-object allocation/bookkeeping traffic and would require frequent registry removal. Occupancy spans provide a testable alternative.

**I1:** actual path is `llam_task_alloc` foreign branch -> external task pool, and `llam_task_allocator_free` external branch. The normal managed free list and remote CAS lists retain their existing policy. Prototype code is connected in `src/core/memory/task_span.c`, registered in Make/CMake. No third-party code or dependencies imported; implementation is first-party Apache-2.0. A bulk try-unregister check must reject active pins/scan refs without spinning on a worker. Occupancy and lists share the existing allocator mutex; detached storage is destroyed after unlock. Acceptance and proposed regression defaults remain in state.md.

**Neutral self-check / draft exposure seen:** does the implemented transition free a span while a public or scanner reference can still discover it? Verify under registry exclusion plus allocator occupancy, with a held-pin race, stale-handle checks and runtime isolation. No claim of a blinded separate reviewer or Pro review.

Current decision: prototype/integration candidate; no ADOPT or INTEGRATED claim until real-path tests and comparison pass.

## E-M2 / H1 v1 observation and revision

The connected C candidate passed six targeted suites. On macOS, allocator storage after the 4096-object return dropped from about 5.64 MB to 0.25 MB including normal prewarm; however RSS/physical footprint stayed at their live phase because the system heap retained freed blocks. This supports allocator reuse, not immediate OS reclamation. H1 v2 keeps the two-span/16-slot ceilings and uses a dense 1–4-page mapping only for the mature span capacity; 1/2/4/8 ramps remain heap-backed where applicable. Choose the capacity with the best payload/page ratio, so sparse padding cannot silently dominate. Mapping failure retains a tested heap fallback. New page-backed results require fresh confirmation and memory-accounting of mapped bytes, not malloc_size on a mapping.

## H1 v3 / failed removal-cost premise

E-M3 v2 fresh paired measurements found that mapping-based reclaim reduced OS memory after a burst, but a 16384-metadata-object release took about 181 ms versus 0.6 ms with historical caching. Full registry walking for every retired span had quadratic aggregate cost, so that version is not the final adoption candidate. Add a registry predecessor link in existing TCB tail padding (size must be measured), retain membership validation for untrusted lookup paths, and unlink allocator-owned registered slots in O(span slots) only under the same registry mutex. Required handle/ABA/pin suites and final confirmation are invalidated by this change and will be rerun. Acceptance thresholds are unchanged.

## E-M4 / final-path adoption evidence before full gates

H1 v3 connected tests pass held-pin quarantine, sparse survivors, page/heap construction failures, mapped fallback, 4 host threads, owner-safepoint cleanup, consuming joins, ABI edges and runtime isolation. Five alternating fresh pairs on the final source reduced cached allocator bytes at 4096 metadata objects from 5,644,768 to 238,528; RSS from 9,011,200 to 3,325,952. At 16384 objects the cache stayed 238,528 bytes. First-spawn backing rose only 352 bytes in this fixture. Raw release now took about 0.89 ms at 4096 and 3.67 ms at 16384, vs 0.05/0.61 ms for the baseline that retains everything. Public 4096-host allocation median rose 4.32 to 5.12 ms; join/reclaim median rose 0.50 to 0.88 ms. The trade-off is actual reclaim work, not universal speedup.

Five-pair hot-runtime p50 changes were within 3%; a 51-round timer p99 signal above 10% triggered longer checking. Nine fresh 511-round timer pairs measured +0.44% p50 and +0.22% p99. Confirmation used unused 8192-object cases with a preselected seeded shuffled release order; final output is in confirmation-span.json. Timer checking briefly overlapped that memory run's start, so it is a diagnostic rather than a controlled release-latency guarantee. All unfavorable samples are retained. No threshold was relaxed.

Sparse survivors still prevent span reclamation. The bound covers reusable empty spans; quarantined reader/registry-busy spans add retained storage until owner activity, safepoints, or teardown can retire them. Managed allocation/free atomics were not changed; the shared existing pending hint gates cold maintenance.

## Artifact-policy repair

The first license-policy run rejected the newly saved historical Git-status text because it names deleted obsolete license files. Those names are historical data, not current licensing guidance. Preserve the exact bytes in preexisting-status.txt.gz with a SHA256 record, remove only the task-owned plain status copy, and rerun the unchanged policy checker. No source notice or policy assertion was weakened.
