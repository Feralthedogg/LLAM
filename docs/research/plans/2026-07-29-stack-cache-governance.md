# Stack Cache Governance Implementation Plan

**Date:** 2026-07-29

**Goal:** Bound every runtime's retained stack mappings in bytes, preserve
runtime ownership, return cold pages while the runtime is live, and expose
auditable trim/security diagnostics without changing the frozen 2.2 option
prefix.

**Design source:**
`docs/research/specs/2026-07-29-stack-cache-governance-design.md`

### Task 1: Prove and correct cross-runtime cache ownership

**Files:**

- Modify: `tests/test_multi_runtime_core.c`
- Modify: `src/core/task/task_stack.c`

- [x] Add a two-runtime regression in which a task running on runtime A spawns
  onto runtime B while A's shard is in TLS. Prewarm A, snapshot both caches,
  and prove B neither pops from nor returns through A's cache.
- [x] Run the focused test and verify RED against the current TLS-first lookup.
- [x] Resolve the task owner first and accept the TLS shard only when
  `g_llam_tls_shard->runtime == task->owner_runtime`; otherwise use the owner's
  home shard/runtime cache.
- [x] Run the focused multi-runtime test and verify GREEN.

### Task 2: Append the public byte-policy and diagnostics contract

**Files:**

- Modify: `include/llam/runtime.h`
- Modify: `src/internal/runtime_resource_plan.h`
- Modify: `src/internal/runtime_types.h`
- Modify: `src/core/lifecycle/resource_plan.c`
- Modify: `src/core/lifecycle/init.c`
- Modify: `tests/test_abi_contract.c`
- Modify: `tests/test_abi_compat.c`
- Modify: `tests/test_runtime_core.c`

- [x] Add current-prefix fields for budget, high/low watermarks, idle age, and
  secure/discard/disabled flags after the existing resource-governance tail.
- [x] Use documented zero defaults: 512 MiB budget, 384 MiB high, 256 MiB low,
  and 30 seconds idle. The explicit disabled flag resolves all three byte
  thresholds to zero.
- [x] Validate page alignment, `low <= high <= budget`, known flags, and checked
  exact-prewarm compatibility before runtime publication.
- [x] Append size-prefixed stats for cached mappings/bytes, committed bytes,
  trim requests, discarded/released bytes, budget rejections,
  secure-return failures, and resident-sample validity/timestamp.
- [x] Add RED/GREEN resolver, prefix, overflow, ABI-layout, and default tests.

### Task 3: Introduce the platform stack-VM boundary

**Files:**

- Add: `src/core/platform/stack_vm.c`
- Modify: `src/internal/runtime_proto_core.h`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`
- Modify: build-manifest tests/inventory only as required by the canonical
  audit
- Modify: `tests/test_runtime_shutdown_internal.c`

- [x] Add focused map/release, discard/reactivate, secure-zero, and optional
  resident-sampling helpers.
- [x] Linux uses `MADV_DONTNEED`; Darwin securely zeros when the selected
  policy cannot rely on discard zeroing; Windows decommits/recommits usable
  pages while retaining reservation and guard protection.
- [x] Add test-only typed failure hooks for discard, reactivate, scrub, and
  resident sampling.
- [ ] Prove the Windows transition code with native CI and MinGW compile gates.

### Task 4: Replace count authority with runtime-wide byte authority

**Files:**

- Modify: `src/internal/runtime_types.h`
- Add: `src/core/task/stack_cache.c`
- Add: `src/core/task/stack_cache_lists.c`
- Modify: `src/core/task/task_stack.c`
- Modify: `tests/test_runtime_core.c`
- Modify: `tests/test_multi_runtime_core.c`

- [x] Give every entry an owner runtime, class, state, committed-byte count,
  and last-return timestamp.
- [x] Add overflow-safe atomic byte reservation before cache publication.
  Class-list counts remain reuse hints; the runtime budget is authoritative
  across all shard and fallback lists.
- [x] Pop list ownership and byte/committed/mapping accounting as one logical
  operation under the owning cache lock.
- [x] Perform secure zero/discard before publication and reactivate after
  detach. On any required state failure, release rather than publish/reuse.
- [x] Add mixed-class and concurrent-shard exact-budget tests.

### Task 5: Implement bounded live trim

**Files:**

- Modify: `include/llam/runtime.h`
- Modify: `src/internal/runtime_proto_core.h`
- Modify: `src/core/task/task_stack.c`
- Modify: `src/core/api/core_api.c`
- Modify: export/installed-contract fixtures
- Modify: `tests/test_runtime_core.c`
- Modify: `tests/test_runtime_shutdown_internal.c`

- [x] Add `llam_runtime_stack_cache_trim_ex(runtime, target_bytes,
  released_bytes)` and `llam_runtime_notify_memory_pressure(runtime)`.
- [x] Serialize trim per runtime. Detach a bounded batch while holding only one
  cache lock, preserve byte authority while detached, then perform VM release
  outside the lock. Remove authority only after success; retain failed releases
  for a later serialized retry.
- [x] Transfer shutdown release failures to a process-owned metadata quarantine,
  expose pending bytes/mappings, and retry a bounded fair batch on later
  runtime initialization.
- [x] Crossing high trims toward low; pressure trims to zero; opportunistic
  idle trim selects only entries older than the configured age.
- [x] Prove manual target, high/low hysteresis, idle age, pressure, concurrent
  pop/push/trim/stats, shutdown, and VM-outside-lock behavior.

### Task 6: Export diagnostics and documentation

**Files:**

- Modify: `src/core/debug/debug.c`
- Modify: `src/core/debug/debug_stats_json.c`
- Modify: `docs/reference/api.md`
- Modify: `docs/reference/options.md`
- Modify: `docs/operations.md`
- Modify: `docs/guides/performance-tuning.md`
- Modify: `scripts/test_research_build_boundary.py`

- [x] Project every authoritative counter to public stats, text, and JSON.
  Resident bytes always carry validity and sample timestamp.
- [x] Document defaults, flag combinations, trim semantics, exact versus
  sampled counters, and cross-runtime ownership.
- [x] Prove stable/research installed declarations, symbols, ABI major,
  SONAME/install name, pkg-config, and CMake imported-target parity.

### Task 7: Stack-cache stage verification gate

- [x] Run focused tests after every RED/GREEN step.
- [x] Run clean stable and research Make and CMake graphs.
- [x] Run ASan/UBSan and TSan including positive controls.
- [x] Run manifest, ratchet structure, supply-chain, installed parity, export,
  whitespace, process-utility, and full Python governance gates.
- [x] Run Linux io_uring integration and long-burst RSS/trim evidence. The
  native segment ran on Linux io_uring; the connected pipeline explicitly
  skipped at its exact-result semantic barrier. A one-million-iteration
  churn/trim/stats burst peaked at 4,988 KiB RSS and ended with zero cached and
  committed bytes.
- [ ] Run the Windows native lifecycle matrix and BSD/macOS compatibility jobs.
- [ ] Record requirement-to-evidence closure and commit only evidenced boxes.
