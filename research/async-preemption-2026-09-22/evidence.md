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

# Evidence and claim ledger

Research date: 2026-09-22. This is design research, not an implementation verification report.
Original material was inspected by Codex. Model agreement is not evidence.
Read depth below is deliberately narrower than a claim of exhaustive literature review.

## Current repository: observed facts

Base: `main`, `34dcc74a7e3cbb584967f6bf5f7816673ed5169b` plus extensive existing uncommitted work.
Tracked diff SHA-256: `baf99dbb9584333e87b1e0c7755e03f6bbfcac5790392f3d23b371b3be174fd8`.
Relevant actual-file hashes, including untracked sources:

| Path | SHA-256 |
| --- | --- |
| `src/core/context/async_context_linux_x86_64.c` | `8e0bca48bd95c4d9fba79c68c162e1af79a479273b2a0250790c280770bc3541` |
| `src/internal/async_preempt_abi.h` | `7a8f038f44af5d7556eeb6ddb81845c9c453788eaf8d880cbc2296857ca820db` |
| `src/asm/linux/x86_64/async_preempt_trampoline.S` | `653d0321eb7c7ebbe765eb14ba4851858db372b166eb9cbcf43eefda91c83816` |
| `tools/preempt/llam_preempt_pass.cpp` | `ab276528ee8ad8efbd3b3c796fd13d1824f1cf6b702dd9bb4d418fdadfa27617` |
| `src/core/preempt/capabilities.c` | `dee3078d8bfc3ba192e8d53e3a829e55457cd67af7c2eb89a4c861175adb864e` |
| `src/core/sched/scheduler.c` | `d68dd96a1ffb334c93a22a5cff9ce15c60a79bd22b9efc3fc56d053054dd1ba2` |
| `src/core/api/blocking_api.c` | `8767f6bc4bfcea16645e4a81e9b92daab73ce1c77db161150928c0d5ae407b0f` |
| `src/internal/runtime_types.h` | `688d02bf4bf1a5c155caaaf2ce701fb1d2a360fa5c0086868bf7c653c5c64a7d` |
| `src/core/base/errno.c` | `170115724a04936d7f3eb965424f5efc325373a31e3b7362994950ff8a585fc3` |
| `src/core/task/yield_join_sleep.c` | `94777008cd104158ecb74cb238b3a452b443bb205a429cd69e3569cb1ab91726` |

These hashes identify the inspected components, not a claim that the entire dirty repository is represented by the HEAD commit.

| ID | Original local location and inspection | Observation | Limit |
| --- | --- | --- | --- |
| E01 | `src/core/context/async_context_linux_x86_64.c:374–514,924–1026,1319–1361`; `src/internal/async_preempt_abi.h`; full 68-line trampoline | Exact request tuple checked in handler; immutable safe ranges; one process runtime/capture configuration; primary only; deterministic one-P restriction; capture stored on worker; singleton resume PC; normal `llam_yield()` bridge. | Source inspection, not native async execution. Removing restrictions alone is not a design. |
| E02 | `tools/preempt/llam_preempt_pass.cpp:112–161,220–276`; `docs/tooling/preempt-metadata.md` | Async verifier rejects memory loads/stores, calls/asm, alloca, atomic mutations/EH. Cooperative mode inserts calls at selected natural-loop latches. Metadata is not automatically registered. | Cannot certify arbitrary loops, transitive call graphs, late machine transformations, or TLS migration from this pass. |
| E03 | `src/internal/runtime_types.h:1142–1238,1320–1401`; `scheduler.c:901–962`; Phase 2 spec | Stable P resources, M active G/current P and generations; authoritative legacy lease and non-owning P ticket. Generation equality does not pin memory. | Worker state called MANAGED_SAFE is not by itself proof that arbitrary application PC is safe. |
| E04 | `src/core/task/yield_join_sleep.c:359–424`; `src/core/base/errno.c:36–102` | Normal yield sets RUNNABLE/enqueues and clears active G before final errno/context save. | This order requires a documented exclusion proof for every consumer. It is a design audit target, not a newly reproduced root cause. |
| E05 | `.artifacts/worktree-integration.SkDXpn/tsan.log:305–306`; integration-summary.md | Existing TSan integration run reports ownerless shutdown cleanup deadline failure followed by segfault. | Read retained log, did not rerun; no proven root cause or harmless-flake classification. Prior ordinary/ASan passes do not close this gate. |
| E06 | `include/llam/runtime.h:203–216`; `docs/abi.md:310–333,396–405,606–612`; `capabilities.c:134–149` | Existing STRICT is diagnostic, not async-required. PINNED is a preference, not a native-thread-affinity guarantee. Stop is cooperative; errno preserved at LLAM switch boundaries. Default effective policy remains cooperative unless experimental setup is active. | No permission to silently reinterpret ABI-compatible numeric fields as stronger contracts. |
| E07 | `docs/internals/magic2.md:25–52,103–190,236–283` | Separate LLAM vs kernel status, drain in-flight native work, caller/tile paths, no replay after partial writes; buffer shapes are not async-safe PC certificates. | Design must not infer native TLS safety, preemptibility, or speedup from Magic2 plan selection. |
| E08 | `src/internal/runtime_types.h:143`; `src/engine/watchdog/watchdog_probe.c:239–315`; `docs/operations/benchmarks.md` CPU-spike section | Existing watchdog interval is 1 ms; watchdog uses last-safepoint age, pressure and tuple checks. Existing CPU-spike harness saturates all reported workers and measures timer/socket/control latency. | Reuse the harness, but add proven async-path activation and no-rescue checks; a requested quantum is not a measured scheduling bound. |

## Literature coverage and primary sources

Search routes: original author/lab pages, USENIX proceedings, Go source, OS vendors/kernel documentation, arXiv current-year preprints. Queries included `Scheduler Activations Anderson Bershad Lazowska Levy 1992 pdf`, `Shinjuku preemptive user level threading NSDI 2019`, `Concord preemption compiler paper`, `site.usenix.org preemptive user level threading 2024 2025 2026`, `Go asynchronous preemption preempt.go unsafe points`, POSIX signal atomics, Windows suspend synchronization, Linux extended register signal frames. Citation trails from Aspen led to Concord and Caladan. Search snippets alone are not treated as inspected methods.

| ID | Source / version / read depth | Relevant observation and application limit |
| --- | --- | --- |
| E10 | Anderson et al., [Scheduler Activations](https://www.cs.washington.edu/homes/tom/pubs/sched_act.pdf), TOCS 1992 (SOSP 1991 predecessor); original PDF, §§3.3 and 4.3 inspected | Preempted critical-section holders create deadlock/performance hazards; their solution uses kernel upcalls and controlled continuation. Establishes the ownership/critical-section problem, not a drop-in portable LLAM backend. One paper/version lineage. |
| E11 | [Go runtime preempt.go](https://go.dev/src/runtime/preempt.go), live source read 2026-09-22, introduction and `isAsyncSafePoint` | Go validates runtime state, PC metadata and stack room before injecting a continuation. Unknown/non-Go and unsafe runtime PCs are excluded. Mature industrial feasibility evidence, not evidence that arbitrary C is safe or that LLAM satisfies the same contract. |
| E12 | [Go runtime os_windows.go](https://go.dev/src/runtime/os_windows.go), live source read 2026-09-22, `preemptM` and suspended-thread lock rules | Duplicates a live thread handle, serializes suspension, obtains context before inspecting state, avoids new locks while target is suspended, checks safe PC, and resumes. Shows constrained Windows implementation is possible; copying the API calls without Go's invariants is insufficient. |
| E13 | Kaffes et al., [Shinjuku](https://www.usenix.org/system/files/nsdi19-kaffes.pdf), NSDI 2019, §§3.2–3.3 inspected | Dune/VT-x/IPI and deliberate context-state choices enable fine-grained preemption in its environment. Its signal-mask and runtime assumptions must not be imported as portable guarantees; published latency numbers are not LLAM measurements. |
| E14 | [Concord: Achieving Microsecond-Scale Tail Latency Efficiently with Approximate Optimal Scheduling](https://marioskogias.github.io/docs/concord.pdf), SOSP 2023, §3.1 including safety discussion inspected | Compiler checks coordinate with dispatcher. External calls are not instrumented and application lock regions need explicit suppression; long regions remain a latency limitation. Useful fallback/toolchain alternative, not truly instruction-interrupt capture or universal C compatibility. |
| E15 | Guo et al., [Benefits and Limitations of User Interrupts](https://www.usenix.org/system/files/nsdi25-guo.pdf), NSDI 2025, §§1–2, safety/context discussion, Aspen-KB/Go design, setup in §4 inspected | Low interrupt cost alone does not remove TLS, lock, scheduling, or I/O bottlenecks. Same-runtime mechanism comparisons challenge universal wins for either polling or interrupts. Aspen-Go's Go 1.21 baseline is not today's Go. Evaluation used specific Xeon hardware and a custom Intel kernel; no LLAM performance extrapolation. |
| E16 | Roca/Beltran, [Rethinking Thread Scheduling under Oversubscription](https://arxiv.org/html/2601.20435v1), 2026-01-28 preprint v1, §§2–4.4 and performance confound discussion inspected | Cooperative, same-thread task binding plus modified glibc is an alternative for oversubscription. Unintercepted waits/busy barriers can stall; authors adapt barriers. Not a universal involuntary-preemption solution, and reported gains include thread-cache effects. Publication/independent reproduction not established here. |
| E17 | [PeeR](https://www.usenix.org/conference/osdi26/presentation/carin), OSDI 2026, venue abstract plus [author artifact README](https://github.com/hipersys-team/PeeR) inspected | Helper-boundary budget checks rely on eBPF verifier properties and kernel continuations. Relevant recent analogy for compiler-certified boundaries, but not evidence for arbitrary C or a direct dependency candidate. Full paper not required for this excluded-domain observation; no performance claim adopted. |
| E18 | Fried et al., [Caladan](https://www.usenix.org/system/files/osdi20-fried.pdf), OSDI 2020, original PDF introduction and §8 inspected | Core-allocation/interference architecture is useful, but depends on runtime/kernel support and exposed concurrency. Not a library-only portable replacement. No published speedup adopted as an LLAM result. |

## OS/ABI evidence

| ID | Original source / read depth | Finding and consequence |
| --- | --- | --- |
| E20 | Microsoft, [Suspending Thread Execution](https://learn.microsoft.com/en-us/windows/win32/procthread/suspending-thread-execution), full article | SuspendThread is debugger-oriented, not an ordinary synchronization primitive. Must pair with the restricted protocol in E12; does not by itself make Windows async capture impossible. |
| E21 | Linux, [SVE ABI](https://docs.kernel.org/arch/arm64/sve.html), §§1,3–6 inspected | Signal data is variable-sized; FPSIMD and SVE records interact, and sigreturn requires matching vector length. Backend must preserve full enabled state and enforce worker capability compatibility. |
| E22 | Linux, [SME ABI](https://docs.kernel.org/arch/arm64/sme.html), §§1–5 inspected | Streaming mode, ZA, optional ZT state and TPIDR2 are distinct per-thread state; signal records and vector-length conditions matter. Saving NEON alone is not a complete context contract. Detailed backend remains unvalidated. |
| E23 | POSIX.1-2024, [Signal concepts](https://pubs.opengroup.org/onlinepubs/9799919799/functions/V2_chap02.html), official search passages; full fetch returned 403 | Lock-free atomics are in the async-signal-safe rules. Access restriction retained: no claim of full-page verification. Handler audit must include generated calls/TLS access, not just source-level absence of malloc. |
| E24 | Apple, [archived sigaction manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/sigaction.2.html), signal safety and handler prototype inspected; local SDK `usr/include/mach/arm/_structs.h:107–157` and `arm/_mcontext.h:61–87` | SA_SIGINFO context availability alone does not establish a modern macOS arm64 restore/PAC/SME contract. SDK explicitly makes arm64e PC/LR/SP/FP opaque/accessor-based. Modern native qualification is required; the historical manual cannot certify production support. |
| E25 | Linux, [XSTATE](https://docs.kernel.org/arch/x86/xstate.html), complete short page | AMX permissions and dynamic XSAVE signal-frame size affect alternate-stack capacity; absent dynamic components need correct initial-state semantics. A fixed SSE-sized buffer is insufficient. |
| E26 | Linux, [CET shadow stack](https://docs.kernel.org/arch/x86/shstk.html), enabling and signal sections | Live per-thread CET state matters, not just an ELF build note. Kernel restores a shadow-stack token on sigreturn; ordinary GPR restore is not equivalent. Unsupported protections must be rejected, never silently disabled. |
| E27 | [signal-safety(7)](https://man7.org/linux/man-pages/man7/signal-safety.7.html), Linux man-pages 6.19 (2026-06-05), safety explanation and errno/longjmp notes | Handler reentrancy is a distinct constraint; errno must survive handler execution. Returning via a trampoline does not establish safety if interrupted code still owns non-reentrant native state. |
| E28 | LLVM [D125291](https://reviews.llvm.org/D125291), historical 2022 compiler design discussion, summary and thread-identification concerns inspected | TLS address semantics and optimizer transformations require an explicit compiler contract. This review is historical motivation, not proof that an old compiler issue remains unfixed. |
| E29 | Microsoft [GetThreadContext](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadcontext) and [QueueUserAPC](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-queueuserapc), API remarks inspected | Context requires a suspended target; ordinary APC delivery depends on an alertable state and cannot be the sole no-poll busy-loop preemption mechanism. Special APCs are a different facility and not certified by this design. |
| E31 | Ousterhout et al., [Shenango](https://www.usenix.org/system/files/nsdi19-ousterhout.pdf), NSDI 2019, p.368 preemption/TLS discussion checked after Pro pointed to it | Runtime critical regions defer preemption; stale TLS addresses across kernel-thread changes are explicitly identified. This historical system used special TLS mechanisms and application exclusions. Same lineage as Caladan, not independent replication. |
| E32 | [sigreturn(2)](https://man7.org/linux/man-pages/man2/sigreturn.2.html), Linux manual, description/notes inspected | Native return restores mask and machine state, but is not a POSIX interface or a portable application API. A synthetic full-restore backend remains an architecture-specific qualification candidate; the manual warns against direct application calls. |
| E33 | WG14 [C issue 0461](https://www.open-std.org/jtc1/sc22/wg14/issues/c11c17/issue0461.html), 2014 issue and disposition inspected | C11/C17 signal-object rules are restrictive; suggested relaxation was not simply adopted as a defect fix. Must document actual POSIX/compiler/OS extensions rather than calling the entire implementation strictly ISO C11-portable. |

## Executed neutral compiler check (E30)

Question: does an ordinary C call force native TLS address recomputation afterward?
Draft exposure: no Pro design response had arrived. Codex already knew the TLS concern;
this is a direct compile-only check, not a blind independent replication.

Input: [tls_codegen_probe.c](artifacts/tls_codegen_probe.c), 16 lines, no includes,
external opaque call between two implicit reads of the same `_Thread_local` variable.

Executed with Apple clang 21.0.0 (`clang-2100.1.1.101`), host arm64 Darwin 25.5.0:

```sh
clang -std=c11 -O2 -Wall -Wextra -Werror -S -emit-llvm research/async-preemption-2026-09-22/artifacts/tls_codegen_probe.c -o research/async-preemption-2026-09-22/artifacts/tls_codegen_probe.ll
clang -target x86_64-unknown-linux-gnu -std=c11 -O2 -Wall -Wextra -Werror -S -emit-llvm research/async-preemption-2026-09-22/artifacts/tls_codegen_probe.c -o research/async-preemption-2026-09-22/artifacts/tls_codegen_probe_linux_x86_64.ll
```

Both commands exited 0. Both generated functions compute one
`llvm.threadlocal.address.p0` pointer and reuse it for loads before and after the
external call. Generated IR is retained beside the source.
This establishes an actual optimizer behavior on these two code-generation targets;
no runtime migration was implemented or executed and no whole-program miscompilation
is claimed. It disproves the assumption that an ordinary opaque C function call
alone forces native TLS rebinding. Managed async migration therefore needs its own
compiler/TLS contract or must exclude this code from migration.

No upstream implementation was downloaded and executed or incorporated. Attribution is for design reasoning, not a claim of code-reuse permission. Recent search is targeted, not exhaustive; kernel/hardware-specific work and eBPF results are not pooled as independent evidence for portable C preemption.

## Critical claims and appraisal

| Claim | AC | Status / strongest warranted conclusion | Evidence, counterevidence, missing gate |
| --- | --- | --- | --- |
| C1 | AC1 | supported: safe asynchronous scheduling requires a declared execution domain; unconditional safe migration/cancellation of arbitrary foreign C is not established. | E10–E16, E20. Known lock/TLS counterexamples invalidate the unconditional interpretation. This is not a theorem that every stronger integrated system is impossible. |
| C2 | AC2 | supported: existing LLAM async implementation is a narrow experiment, not production multi-P/multi-runtime support. | E01–E03, hashes above. Native ports and all enabled extended state not tested here. |
| C3 | AC2/4 | supported: save-complete/quiescent publication must precede other-worker resume and reclamation. Current paths need an explicit exclusion proof or redesign. | E03–E05; deterministic static interleaving analysis, draft exposure: no Pro draft yet. E05 is not assigned a root cause. |
| C4 | AC3 | supported: capability discovery, policy, admission, and strength of guarantee must be distinct; STRICT/PINNED cannot silently acquire new meanings. | E06, E11–E12, E20–E24. New optional tail fields/API would require ABI proposal and tests, not changes this turn. |
| C5 | AC4 | unverified: chosen design will improve LLAM p99/p99.9 latency within acceptable throughput/memory cost. | E13–E16 inform evaluation only. No candidate implementation or matched benchmark exists in this research. |
| C6 | AC5 | supported: actual Pro final architecture assessment completed and corrections incorporated. | T-PRO-ASYNC-01/02 entire responses read. Design approval only, not independent runtime validation. |

## Pro review and exposure record

T-PRO-ASYNC-01 finished; full response including final paragraph read. It recommends
certified managed async + compiler coverage + native exclusive/compensation domains,
G-owned persistent capsule versus M transient storage, quiescent scheduler publication,
origin-M first, and explicit backend gates. It requests source/lifetime mapping and the
retained shutdown failure evidence. T-PRO-ASYNC-02 transmitted these observations and
the proposed detailed protocol, including E30's actual IR result, for substantive
architecture assessment. T02 completed (UI 16m5s); full substantive response read.
Pro judged the corrected architecture RESEARCH-complete, separating unresolved runtime
implementation and native qualification from completion of this design request.

Corrections incorporated in report.md: scheduler-to-queue reference transfer without a gap;
quiescence also prevents new old-ticket operations; wait-generation completion distinct
from persistent cancellation; explicit restore-attempt record and backend reentry witness;
witness check before generic RESTORING rejection; OBSERVED does not authorize freeing live
readers; two capsule banks per G and one unconfirmed restore per M binding; old doorbells
may witness but not authorize recapture; origin-M slot/incarnation eligibility and continued
M service; drain/retirement remains alive after admission close; real module pin survives as
running-frame references. These are accepted design obligations, not executed proofs.

Targeted checks after reading Pro's draft (exposure: seen): Shenango's TLS paragraph
was verified in the original PDF (E31); Linux signal-return scope/limitations were
checked directly (E32); WG14 issue disposition was checked, avoiding treating a
suggested standards change as accepted (E33). These are direct source checks, not
statistically independent model judgments.

## Final acceptance and local integrity

- AC1–AC4: defined in report.md with execution domains, transition/retirement contracts,
  backend failure gates, ABI semantics, file mapping, and regression/evaluation plan.
- AC5: actual latest-family `6 Pro` collaboration, two complete assessments, direct source
  checks, code observations and E30 compile-only result. Models' agreement is not counted
  as empirical evidence for C5.
- Final research status: COMPLETE. C5 latency benefit remains unverified; R0/native ports
  and implementation correctness remain open. No project source implementation occurred.
- Tracked-diff SHA and ten inspected source hashes remained identical to the starting
  identifiers; index remained unchanged. Local Markdown artifact-link validation passed
  (6 links). `git diff --no-index --check /dev/null <owned-md>` emitted no whitespace
  errors (exit 1 means a new file differs from /dev/null, not a test failure).
- Documentation-site build and runtime suites were not rerun: only isolated research
  records and compile-only evidence were added, not MkDocs configuration or runtime code.
- Final owned-file check: PASS, 8 research files, required notices/generated-IR sidecars
  and trailing-whitespace checks. Tracked working-diff hash still matches the baseline;
  no staged changes were introduced.

Appraisal: original code establishes what is implemented, not race freedom. OS documentation establishes API constraints, not our complete backend. Research papers establish results only for their studied workloads and hardware. The same authors/artifact versions are shared origins, not extra replications. Design proofs must state assumptions; no new measurements or formal verification are claimed at this point.
