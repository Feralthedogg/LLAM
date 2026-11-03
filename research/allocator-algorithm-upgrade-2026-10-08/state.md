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

# Allocator algorithm upgrade

- Date: 2026-10-08, Asia/Seoul.
- Codex model: unknown; profile: general; model verification unavailable.
- Collaboration: Codex alone. User explicitly overrides Pro/browser collaboration; no Pro model, conversation or messages used.
- Delivery target: INTEGRATE in `/Users/feral/Desktop/Programming/LLAM`. Preserve the already modified working tree and earlier allocator work.
- Modes: literature synthesis, empirical method transfer, project integration. No explicit token, time or financial budget was supplied.
- Current hypothesis: adaptive owner-local spans with partial-first allocation and a bounded empty-span cache can remove historical-peak retention in the unmanaged host-task pool. This is a fixed-type LLAM adaptation, not a reimplementation of general malloc.
- Alternatives: sender-batched remote frees; replacing the entire runtime with a general allocator; immediate standalone-object trimming. Evaluate assumptions before adoption.

## Acceptance contract (proposed engineering defaults)

AC1: Preserve public ABI, handle validation, consuming joins/detaches, active pins, runtime isolation, and fault cleanup. Required focused and repository correctness gates must pass.
AC2: At zero live external tasks, correctly unpinned allocator storage must have a size-independent bound of two empty spans per processor, with each span at most 16 tasks. Busy/pinned spans must remain alive and quarantined.
AC3: Do not add malloc/free or new atomics to the managed allocation/free hot path. Compare external burst, serial reuse, sparse survivors, and concurrent host spawns, plus ordinary runtime workloads. Proposed regression guardrail: investigate consistent >5% p50 or >10% p99 paired regressions in existing hot cases; do not silently relax these defaults.
AC4: Preserve local evidence, exact workload/build conditions, unfavorable cases, final diff and a reproducible handoff. Test final source, including normal build, ASan/UBSan, stress/fuzz, license and docs. Native Windows/BSD unavailable checks must remain explicit.

Baseline: see artifacts/starting-state.json; HEAD alone excludes extensive existing edits. All source snapshots and the baseline archive are local.

Research status: COMPLETE. Adoption status: INTEGRATED. Merge readiness: READY_FOR_MERGE for the tested local scope; other native platforms remain unqualified.
Next: human review of report.md and the owned task patch. Final source matches artifacts/changes.json and the patch SHA256 in artifacts/verification.json. Required local gates passed; source self-review and disposable reverse-check passed. Keep sparse/pinned/fallback and host-burst limits in any future evaluation. No Pro used. Review is self-review; no separate blinded model is used.
