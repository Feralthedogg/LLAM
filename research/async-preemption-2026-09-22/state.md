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

# Research state: asynchronous preemption

## Brief and authority

- Goal: design LLAM as a safely asynchronously preemptive C11 N:M runtime, with implementation-ready contracts, platform boundaries, and verification gates.
- delivery_target: RESEARCH. No runtime implementation, commits, pushes, or merges are authorized by this request.
- Scope: managed CPU execution, foreign/native calls, G/M/P ownership, context capture/restore, toolchain certification, scheduling/I/O/cancellation/shutdown, cross-platform rollout, and Magic2 compatibility.
- Non-goals: language wrappers, new Magic2 public APIs, hard real-time guarantees on general-purpose OSes, arbitrary hostile C isolation inside one process.
- Budget: no explicit limit; investigate decision-changing gaps, not unlimited exploration.
- AC1: evidence-backed definition of asynchronous preemption and unavoidable exclusions.
- AC2: current-code gap map and concrete ownership/context/lifetime protocols.
- AC3: platform/compiler/ABI/foreign-code contracts and failure behavior.
- AC4: sequenced implementation plan with correctness, latency, and regression gates.
- AC5: actual Pro collaboration, critical-source checks, and substantive final assessment.

## Environment and preservation

- Repository: `/Users/feral/Desktop/Programming/LLAM`, branch `main`, HEAD `34dcc74a7e3cbb584967f6bf5f7816673ed5169b`.
- Existing integrated working tree is extensively modified and untracked; preserve all of it. This run owns only this research directory.
- Integration baseline material: `.artifacts/worktree-integration.SkDXpn/integration-summary.md` and `.git/integration-backups/2026-09-22-phase1a.x4T83O/` (local only).
- No tests from the integration turn are being represented as executed by this research run.
- codex_model: unknown (exact family not verified from host metadata).
- profile: general.
- adoption_status: NOT_EVALUATED (design only).
- merge_readiness: NOT_APPLICABLE.

## Pro session (private working record)

- Use only the user's specified existing conversation: https://chatgpt.com/g/g-p-69b65124f21c819191677f32cf45e678-feral/c/6a99bf49-4338-83ee-86c8-257a9d6c6cb0
- Browser: Codex in-app browser, tab `1`.
- Verified 2026-09-22: composer `6 Pro`; reasoning control `Pro, 5개 중 5번째`; model family `최신` selected, alternatives `GPT-5.6 Sol` and `GPT-5.5`.
- Recency basis: current official https://developers.openai.com/api/docs/guides/latest-model identifies GPT-6 Astra as current model and documents Pro mode; https://learn.chatgpt.com/docs/models lists Astra and current availability. UI confirms this account's latest-family Pro selection. API model IDs are not inferred from UI labels.
- T-PRO-ASYNC-01 / content identifier `kickoff-v1`: send confirmed at 2026-09-22 08:27 UTC; rendered user message and Pro generation observed. Content: design-only brief, baseline limitations, AC1–AC5, request for Pro-led architecture and literature strands. Response subsequently completed and fully read as recorded below.
- Tracked working diff SHA-256 before research: `baf99dbb9584333e87b1e0c7755e03f6bbfcac5790392f3d23b371b3be174fd8` (`git diff --binary HEAD`; this identifier excludes untracked files).
- Research status: COMPLETE (RESEARCH/design scope only). T-PRO-ASYNC-01 completed (UI: 17m32s). Entire response read through final paragraph. CHANGES: certified managed async + compiler coverage + foreign native-thread islands/bounded compensation; distinguish capture/schedule/migrate/cancel safety; G-owned capsule/M-owned transient arenas; scheduler commit only after quiescence; origin-M first; native restore is a qualification gate, not proved.
- T-PRO-ASYNC-02 / content identifier `evidence-and-protocol-v1`: send confirmed, response completed (UI: 16m5s), entire substantive response read through final architecture assessment. Provided actual path/lifetime observations, retained failure excerpt, E30 compiler result, and primary-source findings. Pro explicitly approves RESEARCH-complete architecture **with** C′/D′, origin-M service, and retirement corrections. These are incorporated in report §§5–9,11–14. No extra approval round required by Pro.
- Final corrections: two immutable capsule banks per G; one unconfirmed restore per M binding; backend witness checked before generic RESTORING rejection; OBSERVED versus all-reader retirement; old doorbell cannot authorize recapture; fresh P ticket for resume; origin slot+incarnation eligibility before claim; generation-bound completion distinct from persistent cancel; old-owner quiescence closes new old-ticket operations; shutdown retains execution/retirement service; real code pin transfers to live frames.
- AC1–AC5 met as design deliverables. R0 failure cause, native restore qualification, toolchain coverage, ports, quantitative latency/throughput remain implementation/measurement-open, not waived.

## Active work

- Pro: lead design alternatives, semantics, research priorities, final assessment.
- Codex: inspect actual local code, primary literature and platform contracts, verify consequential claims, produce design/evidence/report.
- Known prerequisite to verify: integration TSan ownerless shutdown cleanup failure; do not dismiss as harmless or fixed.
- Independent task batch completed: E01–E08 actual code/ABI/log inspection; E10–E29 primary literature/OS/compiler constraints; E30 two compile-only TLS probes PASS (codegen observations only, not runtime tests). T02 transfers the consequential evidence and decision questions.
- This run's files: state.md, evidence.md, final report.md, and compile-only TLS source/IR artifacts. Production and existing test files unchanged.
- Final deliverable: report.md (complete architecture and R0–R6 implementation/verification plan), evidence.md (sources, actual local observations, bounded compiler experiment, limitations), artifacts/tls_codegen_probe.*.
- Preservation: tracked working diff hash unchanged; ten inspected source hashes unchanged; index still unstaged. Final local checks passed: 6 relative artifact links resolve; all 8 owned files have required notices/sidecars and no trailing whitespace. Runtime and documentation-site suites were intentionally not rerun for isolated research records.
- Next action if implementation is requested: R0 baseline regression/root-cause work; independently scoped R1 native backend feasibility may proceed without claiming integrated correctness. No implementation task, background run, commit, push, or deployment was started.
