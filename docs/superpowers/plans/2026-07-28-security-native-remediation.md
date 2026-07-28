# Native Segment and Security Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:executing-plans` to implement this plan task-by-task. Every
> behavior change follows RED → GREEN → focused regression → commit.

**Goal:** Close the correctness blockers in the compiled LEIR native-segment
path and remediate every validated Codex Security finding without changing
LEIR semantics or the supported public ABI.

**Architecture:** LEIR remains the compiler/planner meaning contract. The
Linux backend owns kernel submission and retirement with generation-tagged
tickets; the experimental adapter owns an explicit terminal lifecycle.
Runtime cleanup paths retain typed ownership of results until either delivery
or discard. Broker requests use stable descriptor identity, per-subject
capacity reservations, and one aggregate deadline. CI and release inputs are
immutable and least-privileged.

**Tech Stack:** C11 atomics, pthreads, io_uring, Windows IOCP, CMake/Make,
Python `unittest`, GitHub Actions, AddressSanitizer, UndefinedBehaviorSanitizer,
ThreadSanitizer where supported.

## Non-negotiable contracts

- Do not change LEIR program semantics or expose the private experimental API.
- Do not reuse a segment, batch, watch, descriptor, or native instance until
  its final owner has retired.
- Never infer kernel retirement for SQEs that were not submitted.
- Positive short reads and EOF are semantic outcomes, not malformed CQEs.
- Cancellation transfers a produced resource to a typed discard path.
- One broker batch owns one absolute deadline; entry count cannot multiply it.
- Preserve capacity for at least one authenticated peer/recovery subject.
- Default GitHub token permissions are read-only; only publication receives
  `contents: write`.
- Do not bump or release unless correctness, all CI, and the specialized Linux
  performance gate independently pass.

---

### Task 1: Make native CQE reduction total for short reads and partial submit

**Files:**
- Modify: `src/io/linux/watch/linux_segment.c`
- Modify: `src/io/linux/watch/prelude.c`
- Modify: `src/io/linux/runtime_io_segment_linux_internal.h`
- Modify: `src/io/engine/io_engine.c`
- Modify: `src/internal/runtime_types.h`
- Modify: `experiments/leir/test_leir_native_linux.c`

- [x] Add reducer tests where a non-tail `READ_FIXED` returns `0` and a
  positive value smaller than the requested length. Assert semantic EOF/error
  selection, target retirement, no fatal runtime state, and reusable batch.
- [x] Add an injected `io_uring_enter` short-submit test for a linked batch.
  Assert only the submitted prefix is considered in flight, the unsubmitted
  suffix remains owned by the caller, and token storage is not released.
- [x] Run `make -j4 test_leir_native_linux && ./test_leir_native_linux` and
  record both intended failures.
- [x] Replace the sign-only non-tail CQE guard with opcode-aware expected-result
  classification. A successful non-tail CQE is impossible only when the
  operation's semantic contract says it should have been skipped; EOF/short
  reads transition to a real semantic failure and wait for kernel-proven
  retirement.
- [x] Request `IORING_SETUP_SUBMIT_ALL` and record whether the kernel accepted
  it. Reject multi-operation native chains when that setup guarantee is absent.
  Guard omitted-tail retirement with the same recorded guarantee so even an
  injected fragmented submit cannot release caller-owned token storage.
- [x] Run the focused test natively and in the privileged Linux 6.x io_uring
  container.
- [x] Commit as `fix: make native segment retirement submission-aware`.

### Task 2: Close native adapter copy and lifecycle races

**Files:**
- Modify: `experiments/leir/leir_native_segment.c`
- Modify: `experiments/leir/leir_native_segment.h`
- Modify: `experiments/leir/test_leir_native_segment.c`

- [x] Add tests proving a failed/short fixed receive never copies uninitialized
  or stale scratch bytes into the caller buffer.
- [x] Add deterministic bind-vs-run and bind-vs-destroy barriers. Assert one
  operation owns the instance, a loser gets `EBUSY`/`EINVAL`, and destroy leaves
  a permanent terminal state.
- [x] Run `make -j4 test_leir_native_segment && ./test_leir_native_segment` and
  observe the stale-copy and lifecycle failures.
- [x] Claim `BINDING` before reading `program`, slots, or values; validate under
  that claim; roll back only to live `IDLE`.
- [x] Add `DESTROYED` as a one-way state. Destroy claims live `IDLE`, clears
  resources, then publishes `DESTROYED`; no delayed operation can win an ABA.
- [x] Snapshot each caller buffer before activation, copy output only when a
  receive in that activation completed, and treat an unset error index as no
  successful prefix. A short receive can preserve caller bytes but cannot
  disclose scratch bytes from an earlier activation.
- [x] Run native segment tests, ASan/UBSan, and the deterministic race loops.
- [x] Commit as `fix: seal native instance ownership transitions`.

### Task 3: Dispose blocking results suppressed by cancellation

**Files:**
- Modify: `Makefile`
- Modify: `src/io/runtime_io_api_internal.h`
- Modify: `src/io/api/blocking_ops.c`
- Modify: `src/io/api/blocking_wrappers.c`
- Modify: `src/io/api/public.c`
- Modify: `tests/test_runtime_shutdown_internal.c`

- [x] Add deterministic cancellation tests in which `getaddrinfo`, managed
  `open`, and blocking `accept` first produce a valid result and cancellation
  wins before delivery. Count resolver frees and verify descriptor/handle
  closure through reuse-safe probes.
- [x] Run only the new tests and observe the leaked resolver allocation and
  live descriptors.
- [x] On every error return from `llam_call_blocking_result` or
  `llam_call_blocking_io`, inspect the typed output. Call `freeaddrinfo`, close
  a produced managed-open handle, or close a produced accepted socket before
  recycling request storage.
- [x] Keep ordinary worker failure and no-result cancellation unchanged.
- [x] Run API edge, core, shutdown, Linux ASan/UBSan, and Windows cross-build
  tests.
- [x] Commit as `fix: discard canceled blocking results`.

### Task 4: Unpublish watch waiters before reclaiming watches

**Files:**
- Modify: `Makefile`
- Modify: `src/internal/runtime_proto_io.h`
- Modify: `src/io/watch/close.c`
- Modify: `tests/test_runtime_shutdown_internal.c`

- [x] Add deterministic poll-, accept-, and recv-watch tests that pause close
  after detaching a waiter and before completion. Assert cancellation cannot
  re-enter through an old wait mode or raw watch pointer.
- [x] While holding `watch_lock`, transfer each detached waiter away from the
  watch: clear the request's watch pointer/owner metadata and publish
  `LLAM_IO_WAIT_MODE_NONE` before any path can free the watch.
- [x] Prove the detached completion list carries requests rather than watch
  objects, so no additional watch lifetime reference is required.
- [x] Make cancellation treat an already-unpublished waiter as a completed
  race, without dereferencing the old watch.
- [x] Run the race contract under ASan/UBSan and TSan, run host/Linux
  watch/API regressions, and cross-build production/test-hook Windows targets.
- [x] Commit as `fix: retire watch waiters before reclamation`.

### Task 5: Bind broker I/O to stable descriptor identities

**Files:**
- Modify: `src/core/broker/broker_descriptor.c`
- Modify: `src/internal/runtime_broker.h`
- Modify: `tests/test_security_capability.c`

- [x] Add read and write tests that register a borrowed fd, start an operation,
  close/reuse the original numeric fd for a different object, and assert that
  the broker cannot read from or write to the replacement object.
- [x] Duplicate borrowed descriptors into broker-owned CLOEXEC storage at
  registration and use only the duplicate for asynchronous operations.
- [x] Close the duplicate exactly once on unregister, rollback, broker
  shutdown, and all partial-registration failures.
- [x] Run capability tests plus ASan leak detection.
- [x] Commit as `fix: stabilize borrowed broker descriptors`.

### Task 6: Make scheduler ownership publication transactional

**Files:**
- Modify: `src/core/sched/queue_base.c`
- Modify: `src/internal/runtime_proto_io.h`
- Modify: `src/io/watch/watch_queue.c`
- Modify: `tests/test_runtime_shutdown_internal.c`

- [x] Add a deterministic in-flight rehome test that pauses immediately after
  owner publication. Simulate completion there and assert the target counter
  was credited before publication, both counters drain to zero, and CAS-lost
  provisional credit rolls back exactly once.
- [x] Add target-counter saturation coverage that proves rehome fails before
  target ownership becomes visible.
- [x] Add a dynamic-merge admission test with a join waiter on the executing
  source. Assert request-before-ack keeps the wake on the source and
  request-plus-ack permits migration.
- [x] Run the focused tests and observe the uncredited owner publication,
  deferred underflow fatal, and pre-ack wake reroute.
- [x] Acquire target in-flight credit before publishing target ownership; roll
  back only the provisional target credit when the owner CAS loses, then debit
  the source after successful publication.
- [x] Keep a requested-but-unacknowledged shard accepting its wake traffic
  until the scheduler publishes the merge-safe context-save acknowledgement.
- [x] Run shutdown, core, stress, invariant, production-hook audit, host
  ASan/UBSan and TSan, privileged Linux ASan/UBSan, and Windows cross-build
  suites.
- [x] Commit as `fix: serialize scheduler ownership publication`.

### Task 7: Enforce broker fairness and one batch deadline

**Files:**
- Modify: `src/internal/runtime_broker.h`
- Modify: `src/core/broker/broker_buffer.c`
- Modify: `src/core/broker/broker_channel.c`
- Modify: `src/core/broker/broker_descriptor.c`
- Modify: `src/core/broker/ring/broker_ring.c`
- Modify: `src/core/broker/ring/broker_ring_dispatch.c`
- Modify: `src/core/broker/ring/broker_ring_ops.c`
- Modify: `src/core/broker/transport/broker_transport_ops.c`
- Modify: `src/core/broker/transport/broker_transport_ring.c`
- Modify: `tests/test_security_capability.c`

- [x] Add per-subject exhaustion tests for buffers, channels, descriptors, and
  ring sessions. One subject must hit `EDQUOT` while a different authenticated
  subject can still allocate the reserved capacity.
- [x] Add batched read/write tests with multiple blocking entries and a small
  timeout. Assert elapsed time is bounded by one timeout plus scheduler jitter,
  not entry count times timeout.
- [x] Run the tests and observe current monopolization and multiplied waits.
- [x] Track live counts by authenticated subject and resource kind. Enforce a
  bounded per-subject quota while preserving one peer/recovery reserve; release
  counts on all normal, rollback, disconnect, and shutdown paths.
- [x] Convert the request timeout once to a monotonic absolute deadline and
  pass only remaining time to each readiness wait; preserve the platform
  timeout contract (`EAGAIN` on POSIX, `ETIMEDOUT` on Windows) when no budget
  remains.
- [x] Run capability, ring, transport, timing, and stress tests.
- [x] Commit as `fix: bound broker subjects and batch deadlines`.

### Task 8: Generation-bind Windows socket IOCP association

**Files:**
- Modify: `src/io/windows/watch/socket.c`
- Modify: `src/io/windows/watch/windows_submit.c`
- Modify: `src/io/windows/watch/windows_completion.c`
- Modify: `src/io/windows/watch/windows_control.c`
- Modify: `src/io/windows/watch/pool.c`
- Modify: `src/io/windows/watch/windows_watch.c`
- Modify: `src/io/windows/runtime_io_watch_windows_internal.h`
- Modify: `src/internal/runtime_types.h`
- Modify: `src/core/lifecycle/init.c`
- Modify: `src/core/lifecycle/shutdown.c`
- Modify: `src/io/engine/io_engine.c`
- Modify: `tests/test_windows_iocp_io.c`

- [x] Add a Windows test that closes an associated socket, forces numeric
  `SOCKET` reuse, then submits through the new watch. Assert the new generation
  is associated with the node completion key and a replacement already bound
  to a foreign IOCP is rejected.
- [x] Store association in a generation-owned watch object, not a process-wide
  numeric-socket cache. Put generation in every overlapped request and verify
  it before completion delivery.
- [x] Retain a broker-owned socket/HANDLE authority for each generation. Use
  `CompareObjectHandles` to distinguish the same live kernel object from
  numeric reuse, associate only a genuinely new object, and issue/cancel/
  finalize I/O through the retained authority so reuse cannot retarget an
  operation.
- [x] Retire stale generation storage only after every overlapped operation
  releases its pin; synchronize the operation pool for fail-closed foreign
  completion handling.
- [x] Record the RED result: the stale numeric hit produced no packet on the
  runtime IOCP; naively calling `CreateIoCompletionPort` again on an unchanged
  live socket then broke ordinary `POLLOUT` with `EINVAL`.
- [x] Cross-compile the IOCP, HANDLE, and API-edge targets with MinGW and run
  them under Wine; repeat the socket reuse/round-trip test five times.
- [ ] Require the native Windows CI tests after push.
- [x] Commit as `fix: generation-bind Windows IOCP sockets`.

### Task 9: Make CI and release inputs immutable and least-privileged

**Files:**
- Modify: `.github/workflows/benchmarks.yml`
- Modify: `.github/workflows/bsd.yml`
- Modify: `.github/workflows/docs.yml`
- Modify: `.github/workflows/leir-research.yml`
- Modify: `.github/workflows/leir-native-research.yml`
- Modify: `.github/workflows/linux.yml`
- Modify: `.github/workflows/macos.yml`
- Modify: `.github/workflows/nightly.yml`
- Modify: `.github/workflows/release.yml`
- Modify: `.github/workflows/soak.yml`
- Modify: `.github/workflows/srem-research.yml`
- Modify: `.github/workflows/stress.yml`
- Modify: `Makefile`
- Modify: `docs/requirements.txt`
- Add: `scripts/check_ci_supply_chain.py`
- Add: `scripts/test_check_ci_supply_chain.py`

- [x] Add a repository policy test that rejects external `uses:` values not
  pinned to a 40-hex commit, broad workflow-level write permissions, plain
  HTTP package repositories, and unhashed docs requirements.
- [x] Run the policy test and observe all current violations.
- [x] Resolve each existing action tag to its reviewed upstream commit and pin
  it with a trailing version comment.
- [x] Pin docs dependencies to exact versions and hashes; install with
  `pip --require-hashes`.
- [x] Remove the DragonFly HTTP fallback. Require authenticated HTTPS plus
  trusted signed repository metadata; fail closed instead of publishing a
  platform archive when verification is unavailable.
- [x] Set release workflow default permissions to `contents: read`; give only
  the final publisher job `contents: write`; disable credential persistence in
  builder checkouts.
- [x] Run the policy unit test, parse every workflow, build docs from the locked
  requirements, and dry-run platform packaging where available.
- [x] Commit as `security: pin CI and isolate release authority`.

### Task 10: Verify, record fix outcomes, publish the branch

**Files:**
- Add outside repository:
  `/private/var/folders/vx/23xrg8c53d54db_ypgjnhw8r0000gn/T/codex-security-scans-7oG684/leir-native-segment/ba596579d9777348981021553bdbcf4596ed8a0f_20260727T215134Z_4xvuww_u/artifacts/fix_report.md`

- [ ] Run formatting/static checks and every focused test introduced above.
- [ ] Run the full Make and CMake/CTest suites on the host.
- [ ] Run ASan/UBSan and supported race/stress suites.
- [ ] Run privileged Linux io_uring tests and the connected RECV → SEND
  benchmark matrix with portable and Linux-specific gates reported separately.
- [x] Reproduce the legacy native screen timeout and show that its single-CPU
  affinity serialized the runtime with its peer (`120s` runner timeout;
  direct unpinned control completed).
- [x] Remove the obsolete whole-matrix screen from release evidence and pin
  the connected pipeline to two available CPUs, failing closed when the runner
  cannot provide both.
- [ ] Re-scan the branch with a normal Codex Security scan or perform a
  security diff scan against the sealed baseline.
- [ ] Write one outcome per original rule ID to `fix_report.md`, including
  root cause, changed files, RED/GREEN commands, and any platform validation
  completed by CI.
- [ ] Review the diff, remove generated binaries, commit any evidence-only
  changes, push `codex/leir-native-segment`, and wait for every required GitHub
  check.
- [ ] Fix CI failures within scope and repeat until green.
- [ ] Run the specialized performance decision gate. If it fails or regresses,
  keep this as an unreleased research/security branch. Only if correctness,
  full CI, and performance all pass, bump the version, tag, push, and verify
  the release workflow and assets.
