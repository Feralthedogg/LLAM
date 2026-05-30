# LLAM ChangeLog

## 2.0.1 - Unreleased

### Added

* BSD platform support foundation for FreeBSD, OpenBSD, NetBSD, and
  DragonFlyBSD, including platform macros, kqueue readiness/user-wake reuse
  where portable, BSD package target detection, CMake/Make wiring, and a BSD
  VM smoke matrix for core/API/select/I/O-buffer/shared-load coverage.

* isolated benchmark case execution in the release benchmark workflow and
  release-gate documentation. Public LLAM/Go/Tokio comparisons now avoid
  cross-case worker, timer, cache, and CPU-frequency state carrying into
  latency-sensitive cases such as `spawn_join`.

* additional multi-runtime, blocking-pool, public-handle, owned-buffer, broker,
  Windows IOCP, BSD kqueue, and server-stress regression coverage for the
  post-2.0.0 hardening queue.

* a direct runtime soak gate for repeated LLAM core fuzz, multi-runtime
  ownership/isolation, runtime stress, shutdown, and owned-buffer coverage
  without depending on the example chat server policy.

### Changed

* server composite stress policy now separates throughput guardrails from
  long-running soak stability checks. Standard Stress keeps absolute
  delivery-MPS thresholds, while `--soak-hour` gates on traffic, stats,
  accounting, shutdown, resource limits, and unexpected edge errors instead of
  hosted-runner-sensitive MPS numbers.

* release and package smoke checks now cover the expanded BSD targets, stricter
  install/package path safety cases, shared-library metadata, and isolated
  benchmark commands used for release-quality comparisons.

* runtime owner, public-slot, channel, select, kqueue, io_uring, Windows IOCP,
  broker-ring, and broker-transport paths received targeted hot-path and
  teardown stabilization without changing the public C ABI.

### Fixed

* preserve managed task/shard TLS when an embedder creates an explicit runtime
  from inside an existing LLAM task. Successful nested runtime creation no
  longer makes the caller look unmanaged for later sync, I/O, or current-task
  queries.

* roll back registered partial runtime initialization failures through the
  normal runtime shutdown path so backend resources, including Windows Winsock
  setup, are released before returning the original initialization errno.

* clear channel receive outputs on failure and document the output-clearing
  contract, preventing stale caller storage from surviving failed receive paths.

* harden owned-buffer handle publication, public handle shift bounds, adjacent
  public-handle generation guesses, unmanaged join/destroy races, multi-runtime
  blocking-pool isolation, signal/floating-point global lifetime, provided
  buffer detachment before data access, and concurrent run/watchdog contracts.

* stabilize Linux io_uring interrupted waits, Darwin/BSD kqueue transient
  `EAGAIN` and cleanup errors, BSD poll validation, DragonFly alternate signal
  stack `EAGAIN`, Windows IOCP association metadata/locking, Windows IOCP smoke
  completion, and UDP IOCP poll opt-in behavior.

* harden broker capability and transport paths after 2.0.0: reject slot-zero
  tokens, enforce family-specific rights allowlists, bound direct buffer
  grants, clear output authority on failed attenuation/revocation/transport
  calls, preserve revoke atomicity on entropy failure, serialize concurrent
  broker destroy, pin dispatcher/ring operations during teardown, close POSIX
  broker fds with close-on-exec protections, and clear inherited Windows HANDLE
  authority.

* reject overflowing absolute ranges, ranges beyond
  `LLAM_BROKER_BUFFER_MAX_BYTES`, and non-buffer authority bits when minting
  internal broker buffer grants. Buffer-grant validation already rejected
  relative overflow; creation now fails closed before storing impossible grant
  ranges or unknown rights for future data-plane users.

* reject zero revocation epochs when issuing or validating broker capability
  tokens. Epoch zero is reserved for cleared/uninitialized broker state, so raw
  internal token helpers can no longer mint authority that validates against a
  destroyed or never-initialized epoch sentinel.

* reject zero-right raw capability validation. Direct capability helpers now
  match broker-visible validation by requiring callers to prove a concrete
  authority bit instead of using validation as a rightless live-token oracle.

* reject zero-right client grant requests on the broker control transport.
  `CREATE_BUFFER`, `CREATE_CHANNEL`, and `REGISTER_DESCRIPTOR` now require an
  explicit nonzero rights mask instead of treating an omitted rights field as
  the maximum rights allowed for that object family.

* prevent broker teardown and transport races from stranding shared-memory ring
  sessions, duplicated response fds/HANDLEs, predefined task grants, detached
  task slots, broker byte channels, descriptor/HANDLE grants, or stale doorbell
  waits after response failures, disconnects, or concurrent destroy.

* keep Windows named-pipe broker servers alive when a malformed client connects
  and closes before `ConnectNamedPipe()` completes. The broken session now
  consumes only its own accept budget instead of failing the long-running local
  broker loop.

* consume broker ring fd/HANDLE ownership on failed imported-ring mapping.
  Malformed or non-ring authorities passed with `take_ownership=true` no longer
  leak POSIX descriptors or Windows kernel HANDLEs after validation or mapping
  failure.

* make `llam_runtime_collect_stats_ex_rt()` safe against default-runtime
  init/destroy races by pinning the runtime before reading runtime-owned state.
  Pre-init default stats still return an empty size-aware snapshot without
  publishing a spurious error.

* make `llam_dump_runtime_state()` use the same registry-pin/lifecycle gate as
  stats collection, so human diagnostics cannot inspect default-runtime storage
  while another host thread is still constructing or tearing it down.

* make unmanaged `llam_sleep_*()` calls validate the default runtime through a
  public-op registry pin instead of reading default-runtime storage directly
  during host-thread init/shutdown races.

* clear `llam_runtime_collect_stats_ex_handle(NULL, ...)` output snapshots before
  returning `EINVAL`, so FFI callers cannot accidentally reuse stale worker or
  context-switch counters from an earlier successful collection.

* clear every `llam_channel_select()` operation's `result_errno` before managed
  context and per-operation validation. Reused FFI operation arrays no longer
  retain stale close/cancel/timeout status when select fails before choosing an
  operation.

* fix task-local key allocation after capacity growth. Unissued future key slots
  are no longer treated as reusable deleted keys, preventing duplicate key
  issuance, preserving the full 65,535-key budget, and ensuring real exhaustion
  reports `ENOSPC`.

* make internal public-slot handle encode/decode helpers fail closed for
  zero-width generation fields and correctly round-trip narrow generation shifts.
  This keeps future opaque-handle families from accepting non-canonical
  slot/generation bit layouts outside the current 64-bit shift-32 path.

* make the direct runtime soak runner preserve timeout diagnostics even on
  Python versions that expose captured timeout output as bytes, and fail early
  when a configured test path is missing, non-file, or non-executable.

* make direct runtime soak timeouts terminate the whole POSIX child process
  group, preventing orphaned helper processes from polluting later soak cycles
  or CI jobs after a hung test.

* add shared CI process helpers and apply process-group timeout cleanup to
  benchmark guard, benchmark comparison, and benchmark matrix runners, so
  timed-out wrapper commands cannot leave workload grandchildren behind.

* apply the same process-group timeout cleanup to Linux and macOS verification
  scripts, replacing raw `subprocess.run(timeout=...)` calls that could leave
  helper descendants alive after a timed-out verify command.

* extend the shared CI process helper to use Windows `taskkill /T /F` on
  timeouts and normalize path-like command arguments before spawning, matching
  the descendant cleanup guarantees already used on POSIX process groups.

* add a process-helper regression test and include it in `make test-hardening`,
  permanently checking that timeout cleanup removes descendant processes rather
  than only terminating the direct wrapper command.
* bound BSD VM startup in CI/release workflows and keep DragonFly BSD
  infrastructure stalls from blocking the required BSD gate indefinitely.
* make `scripts/run_with_timeout.py` use process-tree cleanup on Windows too,
  and extend the process-helper regression test to cover streamed timeout
  wrappers, not only captured subprocess helpers.
* send `run_with_timeout.py --dump-on-timeout` signals to the POSIX child
  process group so wrapper scripts cannot hide the actual runtime process from
  rare-hang dump collection.
* ensure timeout cleanup still kills descendant processes when a POSIX dump
  signal terminates the direct wrapper before the final interrupt/kill phase.
* start server stress harness children in isolated POSIX process groups and
  interrupt/kill the whole process tree during cleanup. Wrapper-style test
  servers can no longer leave real server descendants running after startup or
  connection failures.
* bound `stress_server_composite.py` child phase commands with process-tree
  timeout cleanup and explicit timeout diagnostics, preventing hung flood
  wrappers from stalling the composite suite until the outer CI job timeout.
* drop queued Linux io_uring, Darwin kqueue, and Windows IOCP cancel-control
  records when a request completes naturally before the queued cancel control is
  processed. This prevents late backend control handling from dereferencing
  request storage or canceling a later request that reuses the same embedded
  request address.
* use the declared Windows `RtlGenRandom()` entry point for public-slot sealing
  entropy instead of casting a dynamically resolved `FARPROC`, fixing MinGW
  `-Werror=cast-function-type` builds while keeping the secure entropy path
  fail-closed.

### Tests

* latest `dev` CI gates cover Linux sanitizer/security checks, macOS builds,
  BSD VM smoke coverage, Windows stress/build coverage through the Stress
  matrix, and package-shape checks.

* Weekly Soak now runs a direct runtime soak before the hour-long server
  composite, giving long-running coverage for core scheduler, multi-runtime,
  shutdown, and owned-buffer paths independently from sample-server behavior.

* added and stabilized edge-case coverage for broker replay guards, ring
  doorbell/flood paths, predefined task detach races, Windows named-pipe
  response-failure cleanup, BSD timeouts/package bootstrap, stress diagnostics,
  and hosted-runner performance guardrails.

* added broker cleanup coverage for partially initialized POSIX ring mappings
  whose fixed-size names are not NUL-terminated before unmap/reset.

* added broker buffer-grant regression coverage for absolute range overflow,
  maximum-buffer bound overflow, non-buffer rights, and relative range overflow
  at the internal grant boundary.

* added broker capability regression coverage for zero-epoch token issuance and
  validation rejection, including stale-output clearing on failed issue.

* added raw capability regression coverage for zero-right validation rejection,
  including subject-bound validation.

* added broker transport regression coverage proving zero-right grant requests
  fail closed for buffer, channel, and descriptor/HANDLE authority.

* added deterministic malformed broker control-dispatch coverage proving
  failed request validation cannot publish stale token, result, descriptor, or
  data authority fields.

* added malformed broker shared-ring submission coverage proving failed fast-path
  completions publish no stale result fields and clear validated output windows.

* made the Windows named-pipe malformed-session regression tolerate transient
  `EPIPE`/busy/not-found states while a just-broken pipe instance is retired,
  while still failing protocol and authority errors immediately.

* added broker ring import regression coverage proving failed owned imports
  close the transferred fd/HANDLE authority and leave the caller mapping output
  reset.

* made the Windows cross-process broker ring session-replay guard wait for an
  explicit child readiness event before rewinding public cursors, avoiding a
  hosted Windows 2022 scheduling race where the process wait timeout could be
  charged before the replay check began.

* keep Linux GCC ThreadSanitizer coverage enabled by probing support for
  `-Wno-error=tsan`; GCC's `atomic_thread_fence` TSan warning no longer breaks
  the sanitizer build while Clang-based hosts keep their previous flags.
* add process-helper regression coverage for `stress_server.py` and
  `stress_server_composite.py` wrapper-server cleanup paths.
* add process-helper regression coverage for hung composite flood wrappers,
  proving timeout diagnostics are emitted and flood descendants are reaped.

* add pre-init stats/JSON snapshot assertions plus host-init race coverage for
  `llam_dump_runtime_state()`, `llam_runtime_write_stats_json()`, and unmanaged
  `llam_sleep_*()` alongside the existing stats race guard.

* add regression coverage for select-operation errno clearing, NULL runtime
  stats handle output clearing, and task-local key exhaustion clearing
  `out_key` to `LLAM_TASK_LOCAL_INVALID_KEY`.

* add public-slot shift-bound regression coverage for zero-width and narrow
  slot/generation handle layouts.

* add internal shutdown regression coverage for the completion-before-cancel
  backend race on Linux, Darwin, and Windows, proving stale cancel-control
  records are removed before request ownership can return to user code.

### Performance

* recovered release-quality `spawn_join` measurements by running public
  comparisons with `--isolate-cases`; isolated LLAM-only median smoke currently
  shows `spawn_join` around 4M ops/s on the local macOS aarch64 host.

## 2.0.0 - 2026-05-25

### Added

* explicit multi-runtime lifecycle surface: `llam_runtime_create()`,
  `llam_runtime_spawn_ex()`, `llam_runtime_run_handle()`, and
  `llam_runtime_destroy()` are the canonical embedding boundary. Legacy
  singleton APIs remain source-compatible wrappers around the process-default
  runtime.

* multi-runtime regression coverage: add `test_multi_runtime_core` and connect
  it to Make, CMake, CI, Docker verification, and sanitizer target suites.

* internal broker capability foundation: add broker-local capability keys,
  attenuable rights tokens, revocation epochs, tamper-resistant MAC validation,
  broker-owned control/ring attenuation, object-specific generation revocation,
  `test_security_capability`, POSIX Unix-domain and Windows named-pipe control
  transports, and `llam_broker --self-test` / `--serve-once` /
  `--serve-n` / `--serve` / `--client-self-test` hooks as the first gate for
  future out-of-process secure transport work. The untrusted control transport
  rejects raw arbitrary token minting with `EACCES`; capability issuance remains
  tied to trusted broker object creation, bounded buffer/channel grant requests,
  or broker-side attenuation/revocation.
  Tokens minted through the local control transport are additionally bound to a
  per-session broker subject id, so replaying the serialized token on another
  broker control connection fails with `EACCES`.
  Shared-memory ring serving now has the same subject-binding hook: a ring
  served under one subject rejects subject-0 or different-subject serving before
  consuming pending submissions.
  Broker capability MAC keys are now created only by broker setup. Ordinary
  in-process runtime creation no longer depends on broker capability entropy or
  carries broker signing authority, preserving the documented separation between
  UAF/FFI hardening handles and out-of-process capability boundaries.
  Broker capability MAC key creation now requires secure OS entropy and fails
  closed instead of falling back to deterministic address/time/pid material;
  token issuance now also requires entropy for MAC-covered nonces and clears
  partial output on failure; transport subject creation follows the same
  fail-closed rule so cross-session replay binding never degrades to
  deterministic fd/path state.
  Client-visible broker validation now rejects zero-right validation with
  `EINVAL`, closing a rightless live-token oracle while keeping internal
  attenuation validation broker-only.
  Failed capability attenuation and broker object-revocation helpers now clear
  non-aliased output tokens, so callers cannot accidentally reuse stale
  serialized authority after an `EACCES`/`EINVAL` failure.
  Object-specific revocation now issues the replacement token before committing
  the live slot generation, so entropy failure reports `EIO` without invalidating
  the caller's existing token.
  `test_security_capability` includes entropy-failure and zero-right validation
  regression guards, failed-output token clearing checks, and revoke atomicity
  coverage.

* broker data-plane foundation: add POSIX `shm_open`/`mmap` and Windows
  named-file-mapping submission/completion rings plus explicit buffer-grant
  validation so a future broker worker can move high-volume requests without
  sharing runtime keys or registries with the client process. The ring path can
  process broker-side capability validation/attenuation/revocation requests and
  broker-owned buffer read/write copies through a bounded shared data window.
  The local control transport now also exercises fixed-size broker-owned buffer
  read/write, byte-channel send/receive/close, POSIX `SCM_RIGHTS` descriptor
  grants, Windows `DuplicateHandle` named-pipe HANDLE grants,
  descriptor/HANDLE read/write via broker-owned tokens, and predefined
  task spawn/join/detach requests, keeping smoke tests independent from
  shared-memory ring setup while preserving bounded payloads and avoiding raw
  client descriptors or function pointers.
  POSIX fd and Windows
  HANDLE read/write can route through broker-owned descriptor slots instead of
  exposing raw descriptor authority to the client side, with descriptor/HANDLE
  duplication performed under the broker table lock before blocking I/O runs.
  Broker-owned byte channels can now route send, receive, and close requests
  through the same capability-checked ring path. Predefined broker-owned task
  commands can now spawn on the broker runtime and be joined or detached through
  ring-issued capabilities without accepting raw client function pointers; the
  initial command set covers scalar return, increment, popcount, and cooperative
  sleep/return work.

* sanitizer entry points: add `make test-asan` for public handle/runtime/I/O
  edge coverage and `make test-tsan` as a shorter runtime core, shutdown, and
  multi-runtime race gate.

* `llam_close()` as the fd/socket close boundary for descriptors used with LLAM
  I/O. It validates `LLAM_INVALID_FD` as `EBADF`, invalidates runtime-local
  descriptor state, and then delegates to the platform close primitive.

* positional file I/O APIs for storage workloads: `llam_pread()`,
  `llam_pwrite()`, `llam_preadv()`, and `llam_pwritev()` preserve the current
  file offset while using scheduler-safe backends where available.

* generic HANDLE positional I/O APIs: `llam_pread_handle()`,
  `llam_pwrite_handle()`, `llam_preadv_handle()`, `llam_pwritev_handle()`, and
  `llam_close_handle()` provide the Windows file I/O path without treating file
  HANDLEs as Winsock sockets.

* aligned owned-buffer allocation for direct-I/O callers:
  `llam_io_buffer_opts_init()`, `llam_io_buffer_alloc_ex()`,
  `llam_io_buffer_alloc()`, `llam_io_buffer_alloc_aligned()`,
  `llam_io_buffer_alignment()`, `llam_pread_owned_aligned()`, and
  `llam_pread_handle_owned_aligned()`.

* `llam_mut_iovec_t`, `llam_io_buffer_opts_t`,
  `LLAM_IO_BUFFER_OPTS_CURRENT_SIZE`, and `LLAM_IO_BUFFER_F_ZERO_FILL` as the
  ABI-stable layout and initialization surface for mutable scatter/gather reads
  and aligned DB/storage buffers.

### Changed

* ABI metadata and shared-library soname/install-name move to ABI major `2`
  with version `2.0.0`.

* `llam_spawn_opts_t` now names the former post-`flags` ABI padding as
  `reserved0`. The field is zeroed by `llam_spawn_opts_init()`, ignored by the
  current runtime, and keeps `deadline_ns` and later field offsets stable.

* Linux positional fd I/O submits explicit-offset read/write requests through
  the io_uring backend. POSIX fallback paths use `pread`/`pwrite` directly from
  unmanaged threads and the blocking-helper path from managed tasks when no true
  async file backend is available.

* Windows positional file I/O is HANDLE-first: fd/socket positional APIs return
  `ENOTSUP`, while HANDLE APIs use overlapped `ReadFile`/`WriteFile` with the
  requested offset.

* runtime-owned caches, I/O buffers, blocking jobs, and scheduler paths are
  routed through explicit owner runtime state rather than implicit singleton
  state where an object owner is known. Public handle slot tables remain
  process-wide family registries with sealed generations and owner-runtime
  checks.

* channel lifecycle, public slot helpers, owned I/O buffer accessors, and
  lifecycle locking were split out of oversized translation units without
  changing public behavior.

* README architecture diagrams now show the 2.x runtime-handle boundary,
  runtime-owned scheduler/cache/I/O state, public-handle hardening flow, and
  optional broker process boundary in more detail.

### Fixed

* harden public opaque handles with family-tagged generations so a forged or
  mistyped FFI handle from one object family cannot be accepted by another
  registry that happens to hold the same slot and generation.

* harden family-tagged public handle generations with sealed verifier tokens
  derived from runtime/table secret material, slot id, internal epoch, and a
  per-slot nonce. Trivially guessed first handles and monotonic next-generation
  guesses after slot reuse are rejected. This remains an in-process UAF/FFI
  hardening layer, not a cryptographic sandbox boundary.

* reject `llam_runtime_run_handle(NULL)` with `EINVAL` instead of allowing the
  explicit runtime handle path to dereference a null handle.

* split internal raw owned-buffer cleanup from the public encoded-handle release
  path so public buffer accessors cannot consume a guessed raw wrapper address.

* make internal raw owned-buffer cleanup prefer live wrapper membership before
  public-handle decoding, preventing a large public slot table from hiding raw
  cleanup behind a decodable but unrelated slot number.

* harden broker shared-memory submission/completion rings with broker-private
  progress cursors and corrupt client-controlled head/tail window checks.
  Impossible or rewound windows now fail closed with `EINVAL`/`EAGAIN` instead
  of allowing stale ring slots to be interpreted as fresh broker requests.
  Ring layout version 2 separates the four shared producer/consumer cursors
  onto independent cache lines and adds a batched completion drain primitive, so
  high-rate broker transports avoid avoidable cursor false sharing and can
  publish the client drain cursor once per response batch. Broker serving now
  reserves and executes bounded multi-entry request batches, then publishes
  `submit_head` and `complete_tail` once per served batch instead of once per
  request; single-request serving remains a wrapper over the same path. Broker rings now
  expose diagnostic-only counters for empty/full `EAGAIN` pressure, batch drain
  size, cursor-publication estimates, and broker serve latency, plus an optional
  doorbell wait helper backed by Linux eventfd, Darwin kqueue user events, and
  Windows events so transports can sleep on empty/full transitions instead of
  tight-polling. Security regression coverage now includes doorbell-driven flood
  coverage, a POSIX process-shared ring flood, a Windows unnamed-file-mapping
  ring flood, and a Windows child-process named-mapping flood so cursor ordering
  and stats are exercised across the platform mapping backends. POSIX coverage
  also runs forked/PID-separated broker processes through the real broker-ring
  session path and then rewinds client-visible cursors to prove broker-private
  cursors reject stale replay after teardown coordination on both POSIX and
  Windows mappings. POSIX and Windows coverage also include process-separated
  broker teardown guards where the broker exits after serving only a prefix of
  a larger submitted window, proving clients can drain the completed prefix and
  unmap without waiting forever on abandoned slots. Stress CI now repeats
  `test_security_capability` as a dedicated broker-ring gate on Linux, macOS,
  Windows 2022, and Windows 2025 with a higher broker-ring flood count instead
  of relying only on the broad CTest pass to surface IPC regressions.

* extend the `LLAM_BROKER_WIRE_OP_SERVE_RING` control-plane RPC so transports
  can request a bounded broker-ring serve batch. A zero request length preserves
  the old single-request behavior, nonzero lengths are capped to the broker's
  bounded stack batch, and `result0` reports the number of served submissions.

* bind broker capability validation to the live broker runtime id after MAC
  validation. This makes an internally fabricated or accidentally cross-issued
  foreign-runtime token fail closed even if it has a valid signature under the
  current test key material.
* make broker capability validation live-object aware. Structurally valid tokens
  now fail once object-specific revocation rotates the buffer, descriptor,
  channel, or task slot generation, and transport/ring validation paths share the
  same stale-token rejection.

* serialize public broker validation and attenuation with object-specific
  revocation under the broker lock, eliminating a TSan-reproduced race between
  live-slot authorization reads and generation rotation.

* harden POSIX broker Unix-domain listen paths: pre-existing filesystem entries
  are no longer unlinked as stale endpoints, newly bound broker sockets are
  chmod'd to owner-only `0600` instead of inheriting a permissive umask, and
  serve-once cleanup unlinks only the exact socket inode it created.

* guard broker object tables and ring-session cursors with broker-local locking.
  Serving the same shared-memory ring concurrently now fails with `EBUSY`, and
  broker descriptor/HANDLE I/O duplicates authority before the blocking call to
  avoid close/reuse races while keeping unrelated broker operations unblocked.

* guard broker destroy with active-operation pins. Once destroy starts, new
  broker operations fail closed, while accepted ring/control/data-plane
  operations finish before broker-owned tables, descriptors, MAC keys, and
  runtime state are cleared. Same-thread helper calls made by the accepted
  operation keep using its existing broker authority instead of failing as new
  external work.

* scrub broker authority residue during teardown: capability MAC keys,
  revocation epoch, object id counters, transport subject sessions, and ring
  subject sessions are cleared after accepted broker operations drain.
  Capability validation now wipes temporary MAC comparison material after use.

* fix broker subject scoping for nested operations. A subject introduced by an
  inner transport or ring operation is now restored when that operation returns,
  so it cannot leak into an outer bearer operation and accidentally authorize
  later validation, attenuation, revocation, or issuance under the wrong
  session.

* keep broker local-control transport subjects stable across repeated
  `serve_one` calls on the same connection and fail closed with `ENOSPC` when a
  new control connection cannot reserve a subject slot. This prevents
  one-request serving helpers from minting tokens that cannot be validated on
  the next request. POSIX control sessions are keyed by socket object identity
  rather than fd number alone, so descriptor-number reuse does not inherit the
  old subject. New subject reservation is now also rejected after broker destroy
  starts, preventing shutdown from racing with a fresh external audience.

* add sequential multi-session broker serving. `llam_broker --serve-n` and
  `--serve` keep broker-owned authority state in one process while assigning a
  fresh session subject to each accepted local control connection; a client
  STOP closes only that session.

* isolate malformed local-control broker sessions. A short write, early close,
  or broken pipe now fails only that accepted client session, while
  long-running `--serve` / `--serve-n` brokers continue accepting later
  sessions with fresh subject ids.

* harden Windows broker named-pipe creation with an explicit current-user,
  LocalSystem, and Administrators DACL plus remote-client rejection. Pipe
  creation now fails closed if the security descriptor cannot be built instead
  of relying on the process default DACL.

* harden Windows broker ring named-file-mapping creation with the same explicit
  current-user, LocalSystem, and Administrators DACL. The security capability
  test now inspects the mapping DACL so the ring boundary cannot silently fall
  back to an ambient process default descriptor.

* add high-entropy private broker ring mapping names and switch shared-memory
  mapping tests away from predictable pid-derived names. Named mappings remain
  bearer rendezvous authority, but broker setup no longer defaults to names an
  opportunistic same-UID process can guess from the test process id.
  POSIX broker ring setup can now create an immediately unlinked shm object and
  map it by fd, giving the broker a path to deliver ring authority with
  `SCM_RIGHTS` instead of exposing any reusable shared-memory name.
  Windows broker ring setup can now create an unnamed file mapping and map a
  duplicated HANDLE, giving the broker the same no-reusable-name authority path
  with `DuplicateHandle`.
  The local control transport now exposes this as
  `LLAM_BROKER_WIRE_OP_CREATE_RING`: POSIX replies attach the immediately
  unlinked ring fd with `SCM_RIGHTS`, Windows replies duplicate the unnamed
  mapping HANDLE into the connected pipe peer, and the broker retains its own
  mapping in ring-session state for teardown without publishing a reusable
  shm or object-manager name. The matching `LLAM_BROKER_WIRE_OP_SERVE_RING`
  operation serves one submission from that broker-owned session by
  subject-scoped session id, rejecting invalid or foreign sessions before any
  client-visible ring entry is consumed. Session-id serving now claims the
  session busy state while still holding the broker table lock, closing the
  lookup-to-execution window where cleanup could otherwise unmap the
  broker-owned ring. If response fd/HANDLE delivery fails after session
  creation, the broker now tears down the just-created ring session so a
  disconnecting client cannot strand private mappings or exhaust the ring
  session table. POSIX broker response writes use no-SIGPIPE send paths,
  turning closed-peer writes into ordinary `EPIPE`/`ECONNRESET` failures
  instead of process termination.

* reject global `LLAM_BROKER_WIRE_OP_REVOKE_ALL` on the untrusted local control
  transport with `EACCES`. Global epoch revocation remains available only as a
  trusted in-process broker-management API; client-visible revocation must use
  object-specific destroy authority.

* close descriptor grants received with malformed broker wire headers or any
  non-`REGISTER_DESCRIPTOR` operation. A client can no longer combine an invalid
  request header or unrelated control op with an attached fd/HANDLE to leak a
  broker-side duplicated descriptor and exhaust broker resources. The transport
  wrapper now treats the dispatcher as the single owner of accepted descriptor
  grants, avoiding fd/HANDLE double-close hazards on malformed requests.
  POSIX `SCM_RIGHTS` requests that attach multiple fds are now rejected after
  closing every received fd, preventing over-authorized descriptor arrays from
  leaking the later ancillary fds into the broker process.
  POSIX broker-control sockets, accepted sessions, received descriptor grants,
  broker-owned descriptor slots, private ring mapping fds, response fd
  duplicates, and ring mapping duplicates are now marked close-on-exec so
  broker capability transport or data-plane authority is not inherited by later
  helper `exec` calls.
  Failed POSIX broker wire reads now clear request/response output storage
  before returning an error, preventing partial attacker-controlled control
  messages from being reused by buggy callers that ignore the failure return.
  Direct broker-owned buffer reads, channel receives, and descriptor/HANDLE
  reads now clear caller output on failure and zero the unused suffix after
  successful short reads/receives, matching the shared-ring stale-output policy.

* reject oversized broker ring task command ids before narrowing to the
  predefined 32-bit command enum. This keeps high-bit shared-memory submissions
  from truncating into an allowed broker-owned task command.

* clear validated shared-memory ring output windows on failed output-producing
  broker operations and clear the unused suffix after successful short
  descriptor/HANDLE reads or channel receives. Failed attenuation, revocation,
  buffer read, descriptor/HANDLE read, channel receive, or predefined task spawn
  no longer leaves stale token or data bytes that could be mistaken for fresh
  broker output by a buggy client that ignores the failure completion; short
  success paths now also protect clients that mishandle the returned byte count.

* clear token outputs before validation in raw capability issuance and
  broker-owned buffer/channel/descriptor/task creation paths. Invalid input or
  early creation failure can no longer leave a stale previous token in caller
  storage that might be confused with newly issued authority.

* clarify the managed-sleep TLS boundary, dynamic-watchdog diagnostic printf
  type, and ABI/sentinel tests so static analysis no longer masks real findings
  behind tool-noise warnings.

* make live runtime dumps avoid unsynchronized raw wait-owner pointer reads.
  Task-level `wait_owner` is now derived from atomic wait reason plus atomic
  I/O/blocking-job owners, eliminating a TSan-reproduced race between
  diagnostics and wait-tracking cleanup.

* explicit runtime creation no longer reports singleton-style `EBUSY` when two
  heap-backed runtime handles are created concurrently; construction remains
  serialized internally while both handles can initialize successfully.

* allocator free paths now fail closed when an object's owner runtime is absent
  instead of falling back to the process-default runtime cache.

* `LLAM_FD_IS_INVALID(x)` and `LLAM_HANDLE_IS_INVALID(x)` now wrap public static
  inline helpers, preserving the macro names while evaluating `x` exactly once.

* document the `NULL` ambiguity of `llam_call_blocking()` and
  `llam_channel_recv()` more explicitly; production and FFI callers should use
  the `_result` forms when `NULL` is a valid payload or callback result.

* move the broker channel slot table out of `llam_broker_t` inline storage and
  into broker-owned heap storage. This keeps stack-allocated broker control
  objects below conservative Windows thread-stack limits while preserving
  broker ownership and channel table cleanup semantics.

### Tests

* add direct public-slot family collision and max-epoch regression coverage, plus
  multi-runtime sync handle confusion and null runtime handle checks.

* add broker ring counter-corruption coverage for stale replay, inverted
  windows, and oversized submission/completion windows.

* add broker ring failed-output coverage proving failed output-producing
  operations clear the validated shared-memory output window before publishing
  a failure completion.

* add a broker ring batch performance gate that records requests/s, p50/p99
  request latency, cursor publications per request, and broker completion-tail
  publish count so future shared-ring IPC regressions are caught by tests rather
  than inferred from ad-hoc benchmarks.

* cover POSIX and Windows broker control transports serving multiple shared-ring
  submissions per `SERVE_RING` request and assert that broker cursor publication
  counters advance once for the batch.

* add broker concurrent channel state coverage under `test_security_capability`
  and include the security-capability test in ASan/UBSan and TSan suites.

* add a broker destroy-vs-active-ring-I/O regression test proving that a
  concurrent destroy waits for the pinned operation before clearing
  broker-owned state.

* add a broker nested-operation regression test proving that an already
  accepted operation can still call internal broker helpers after destroy has
  started, while new external operations remain rejected.

* add disconnected-`CREATE_RING` coverage proving a client that closes before
  receiving the private ring fd cannot leave an unreachable broker-owned ring
  session behind.

* add ABI layout and single-evaluation predicate checks for `reserved0`,
  `LLAM_FD_IS_INVALID()`, and `LLAM_HANDLE_IS_INVALID()`.

* add close-boundary coverage for invalid descriptors and unmanaged platform fd
  closure, and export-check `llam_close` from the shared-library smoke test.

* add ABI layout coverage for `llam_mut_iovec_t`, `llam_io_buffer_opts_t`, the
  aligned-buffer size macro, and default option initialization.

* add positional I/O coverage for fd and POSIX-HANDLE aliases, vector reads and
  writes, EOF handling, file-offset preservation, aligned buffer allocation,
  invalid alignment rejection, and aligned owned positional reads.

* add owned-buffer raw-release collision coverage so internal setup/error
  cleanup remains reliable after the public buffer registry has grown.

* add `make analyze-cppcheck` and `make audit-deps` security gates and wire them
  into Linux CI.

* extend Windows stress, nightly, release, and native verification coverage to
  include multi-runtime, public API edge, shutdown, and owned-buffer tests.

* extend Linux/macOS packaging and shared-library checks to the `2.0.0` ABI
  names.

* keep Windows release packaging and installer defaults aligned with the new
  `v2.0.0` release tag.

* add a broker control-object size guard to `test_security_capability` so large
  data-plane tables stay heap-owned instead of accidentally making
  stack-allocated broker values unsafe on Windows.

## 1.2.0 - 2026-05-21

### Core runtime

* multi-runtime groundwork: tag tasks, wait nodes, timers, I/O requests,
  runtime-owned buffers, channels, mutexes, condvars, cancellation tokens, and
  task groups with the runtime owner that created them.

* diagnostics: reject cross-owner use of runtime-aware public handles with
  `EXDEV`. LLAM 1.x still permits only one live process runtime, but this gives
  the 1.2.x line deterministic misuse detection before the 2.x isolation work
  removes singleton/TLS ownership assumptions.

* handle API: route run, cooperative stop, shutdown, stats collection, and stats
  JSON through runtime-owned internal entry points. The public handle remains an
  alias for the singleton in 1.x, but the implementation no longer hard-codes
  those paths directly to global-only wrappers.

* owner checks: mark cross-owner and invalid-handle branches as unlikely in hot
  paths. Source builds may explicitly define
  `LLAM_RUNTIME_DISABLE_OWNER_CHECKS=1` for unsafe singleton-only profiling, but
  default builds keep the public `EXDEV` diagnostics enabled.

* wait cancellation: snapshot join and wait-queue ownership before taking
  cancellation locks, preventing runtime-stop cancellation from unlocking a
  cleared mutex when it races with a normal producer wake.

### Tests and documentation

* add direct owner-mismatch coverage for sync primitives, channels, select,
  cancellation tokens, task groups, join, and detach.

* tighten benchmark regression guardrails to cover scheduler, channel, select,
  I/O echo, and poll-wake throughput with conservative hard-fail thresholds.

* make the LLAM/Go/Tokio comparison script process-sample aware and report
  median rows by default, reducing false regressions from one noisy benchmark
  process.

* document runtime-object ownership and the new `EXDEV` errno contract in the
  public API and ABI guide.

## 1.1.0 - 2026-05-20

This entry summarizes the source, test, CI, packaging, and documentation changes
staged after the 1.0.1 release. Generated local test binaries and analyzer
artifacts are intentionally omitted.

### Core runtime

* lifecycle: serialize runtime construction and shutdown through the lifecycle
  lock so concurrent init/create/shutdown calls cannot observe or tear down a
  partially constructed singleton.

* lifecycle: treat `llam_runtime_shutdown()` from a managed task or scheduler
  frame as a cooperative stop request. Final teardown remains the host thread's
  responsibility after `llam_run()` returns.

* wait/wake: close the early-completion lost-wakeup window for mutex, condvar,
  channel send/recv, channel select, and I/O waits by using explicit
  armed/completed wait-node states instead of relying on task state alone.

* channel-select: add an inline-vs-queued completion state machine so a select
  operation that completes before the waiter parks is consumed inline, while a
  completion that races after park is reinjected exactly once.

* channels: collapse blocking send and timed send through one internal
  implementation, removing duplicated waiter/wakeup logic from the send paths.

* channels: make nonblocking try-send/try-receive use `EAGAIN` for would-block
  cases. Timed APIs continue to use `ETIMEDOUT` for expired deadlines.

* deadlines: defensively disarm wait deadlines after early wake on mutex,
  condvar, channel, select, and I/O paths. This prevents stale timers from
  firing after a wait has already completed.

* shutdown: add a runtime-wide parked-waiter cancellation pass so cooperative
  stop can wake tasks parked on channels, mutexes, joins, sleeps, I/O, or
  blocking callbacks instead of waiting for unrelated producers.

* cancellation: add a live-token registry and active-operation accounting so
  `cancel`, `is_cancelled`, retain, and destroy races return `EINVAL` or
  `EBUSY` instead of dereferencing reclaimed token storage.

* cancellation: mark detached waiter chains unregistered while holding the token
  lock and hold short scan references while cancellation walks the chain.

* join: make public task handles single-owner for join/detach. Concurrent joins
  now fail with `EBUSY`; timed-out joins release the claim and leave the handle
  joinable.

* task-group: add a live-handle registry, preserve child ownership across
  partial `join_until()` failure, and reject concurrent spawn/cancel/join/destroy
  races with `EBUSY` or stale-handle `EINVAL`.

* task-group: keep explicit caller-owned cancellation tokens outside group
  cancellation; only children using the group-provided token are cancelled by
  `llam_task_group_cancel()`.

* blocking-calls: clarify and enforce cancellation semantics. A queued callback
  can be cancelled before execution; a running callback reports cancellation
  only after the callback returns, preserving caller-owned argument lifetime.

* scheduler: bound direct-yield FIFO preference so two yielding tasks cannot
  starve fresh owner-deque work indefinitely.

* preemption: add request-based automatic preemption policy for CPU-heavy tasks.
  The runtime can request preemption from watchdog/policy state, while the
  actual context switch remains bounded to safepoints, yields, waits, and I/O
  calls.

* preemption: add public hot-loop helpers `LLAM_PREEMPT_POLL()` and
  `LLAM_PREEMPT_POLL_EVERY(counter, interval)`. The counted helper evaluates
  macro arguments once and treats `0`/`1` intervals as poll-every-call.

* spawn: release a retained cancellation-token reference if stack allocation
  fails before the task is published.

* task-local: make an unset value for an active key return `NULL` with
  `errno == 0`, distinguishing it from invalid key or unmanaged-task failures.

* task-local: prevent deleted key IDs from being reused while live tasks still
  hold entries for the old key.

* reclaim: protect listed parked tasks with scan references while shutdown
  diagnostics/cancellation scans run outside shard-list locks.

* portability: remove data races from cached page-size discovery, Darwin
  timebase initialization, and optional Darwin `ulock` symbol resolution.

### I/O runtime

* shutdown: make Darwin, Linux, and Windows I/O workers exit on
  `shutdown_requested` rather than ordinary stop requests, avoiding premature
  worker termination while pending operations remain owned by the runtime.

* I/O ownership: when worker threads detach submit queues, mark requests
  in-flight before releasing the submit lock so timeout/cancellation paths issue
  backend cancellation instead of silently missing ownership transfer.

* I/O waits: disarm deadline timers after fast I/O completion wins the race with
  wait setup.

* I/O waits: make Darwin and Linux poll/accept/recv watcher removal helpers
  tolerate `NULL` watches after a racing completion has already detached the
  request. Windows unsupported-watch stubs now document the same no-op contract.

* I/O fallback: blocking-worker read, write, accept, connect, poll, and handle
  poll callbacks now observe both task cancellation and runtime stop, preventing
  running fallback jobs from keeping shutdown alive indefinitely.

* I/O fallback: restore temporary descriptor flags without clobbering the
  operation's `errno`.

* Windows IOCP: wire the Windows 11 skip-completion policy into associated
  socket/HANDLE handles. Immediate overlapped successes on handles where
  `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` is accepted are completed inline through
  the same finalizer used by queued IOCP packets.

* Windows IOCP: expose skip-completion handle counts, failures, and inline
  completions in runtime dumps so direct-success hit rate can be diagnosed
  without a profiler.

* owned buffers: zero-byte `llam_read_owned()` and `llam_recv_owned()` now
  complete as no-op success with `out == NULL`, matching the public API
  contract and avoiding descriptor inspection.

* owned buffers: allocate public owned-buffer wrappers from detached storage so
  callers may release a successful buffer after `llam_runtime_shutdown()`.

* owned buffers: preserve `EBADF` for invalid-descriptor `recv_owned()` rather
  than normalizing all failed socket probes to `ENOTSOCK`.

* poll: normalize the public invalid-fd sentinel to `POLLNVAL` for single-fd
  `llam_poll_fd()` and `llam_platform_poll_now()` instead of inheriting native
  poll-array "ignore negative fd" behavior.

* poll: translate Darwin kqueue registration `EBADF`/`EINVAL` into `POLLNVAL`
  revents and return one ready descriptor, matching poll-compatible semantics.

* writev: validate aggregate fallback byte counts before writing any slice, so
  invalid tail slices cannot be hidden behind partial success. Unmanaged and
  direct native writev paths still preserve platform behavior for descriptors
  such as `/dev/null`.

* Darwin/Linux: release copied receive-ready payloads when no caller accepts
  ownership from a ready node.

### Diagnostics and observability

* runtime-dump: expand `llam_dump_runtime_state()` output with lifecycle state,
  stop/shutdown state, active I/O waiter counts, node queue depths, shard wake
  and I/O ownership, and per-task wait ownership fields.

* runtime-dump: report task wait owners for I/O requests, select waits, wait
  nodes, blocking jobs, join targets, and timers.

* stats: split JSON statistics writing into a dedicated implementation file and
  add regression coverage for duplicated or missing fields.

* stress: add POSIX signal-driven runtime dumps for the `stress` executable via
  `LLAM_RUNTIME_DUMP_ON_SIGNAL`.

* tooling: add `scripts/run_with_timeout.py` to stream long-running logs, request
  a runtime dump on timeout, interrupt gracefully, and then hard-kill only after
  a grace period.

### Public API and documentation

* document managed-task shutdown behavior, task-group cancellation ownership,
  token destroy/cancel race results, channel try-send/try-recv host-thread
  boundaries, and owned-buffer lifetime after shutdown.

* document the runtime handle API as the 1.x embedding boundary while keeping
  the implementation tied to one process-global runtime.

* document that non-default runtime handles fail with `EINVAL` and second live
  runtime creation fails with `EBUSY`.

* document task join/detach ownership, task-group `EBUSY` cases, and timeout
  ownership preservation.

* update operations guidance for runtime dumps, CI timeout artifacts, expected
  vs unexpected server edge errors, and server stop-time dump artifacts.

* document automatic preemption modes, safepoint polling macros, and the
  environment knobs that control preemption quantum and cheap-safepoint clock
  sampling.

### Example server

* replace the chat outbox `pthread_mutex_t` with `llam_mutex_t` so the example
  does not block an OS worker in the common short critical sections.

* add explicit best-effort and lossless server modes to the flood harness. The
  `--server-best-effort` mode validates bounded-drop throughput; the
  `--server-lossless` mode validates producer backpressure.

* defer socket close until final client release and use enqueue references for
  broadcast snapshots, avoiding stale writes to a reused file descriptor.

* improve shutdown behavior by publishing close state atomically, closing wake
  channels, and avoiding new broadcasts during stop.

* report server-side delivery accounting including attempted deliveries,
  enqueued deliveries, outbox-full drops, outbox-closed drops, missing delivery
  counts, and accounting gaps.

* split edge-stress client errors into expected churn/cleanup failures and
  unexpected failures. `unexpected_client_errors` is the gating signal.

### Stress and test coverage

* add and wire `test_runtime_shutdown_internal` for shutdown internals and
  partial-init cleanup coverage.

* add focused public API edge tests for lifecycle, task ownership,
  cancellation, channel close/drain, blocking callbacks, cond/mutex deadlines,
  and managed/unmanaged call boundaries.

* add focused channel-select race tests for send, close, and cancellation
  lost-wakeup coverage.

* add task-local and task-group ownership tests, including task-local
  isolation, group destroy-with-live-children, cancellation, and partial
  `join_until()` behavior.

* add unmanaged OS-thread join tests so the public boundary is covered without
  relying on scheduler progress from the joining thread.

* add direct I/O edge tests for zero-size owned reads, invalid-fd poll,
  invalid-descriptor errno preservation, POSIX handle aliasing, writev aggregate
  validation, and owned-buffer release after runtime shutdown.

* add live-I/O dump tests on POSIX and Windows IOCP to prove parked I/O
  ownership appears in diagnostic dumps.

* add seeded runtime invariant tests for channel close/drain, select timeout
  and close, cancellation wakeups across sleep/channel/condvar, task-group
  timeout recovery, and runtime statistics sanity.

* make dynamic-worker stress diagnostics more actionable by printing live-task,
  active-I/O, effective-live, and block-pending counts when downscale assertions
  miss.

### CI

* include the new focused tests in Make and CMake test targets.

* add Makefile object/link build signatures so sanitizer or custom-`OBJDIR`
  builds cannot silently contaminate normal executable targets.

* make Windows-host Makefile targets delegate to the native CMake backend instead
  of failing with `windows-unsupported`, and route `make verify-windows` through
  `scripts/verify_windows.ps1 -Native`.

* make the Windows extension-function cache type-safe for `AcceptEx` and
  `ConnectEx`, which lets MinGW `-Wpedantic -Werror` compile the Windows IOCP
  files cleanly.

* add package smoke checks that compile bundled `demo` and `bench` examples from
  installed release archives and verify required example support files are
  present.

* run seeded runtime invariant checks in the Stress workflow on Linux, macOS,
  and Windows.

* repeat `test_runtime_api_edges` in Stress, Nightly, and Soak jobs so fault
  injection and completion-order races are exercised more than once per run.

* run deeper seeded invariant repeats in Nightly Deep CI and include the checks
  in ASan/UBSan and experimental TSan lanes.

* stream long-running stress output through `scripts/run_with_timeout.py` so CI
  timeout failures retain partial logs and runtime dumps.

* keep Windows 10/11 forced-policy stress covering the new invariant test in
  addition to runtime stress, fuzz, IOCP, and policy tests.

* repeat native Windows IOCP I/O tests under forced Windows 10 and Windows 11
  policy branches in Stress and Nightly workflows.

### Known behavior

* high-rate chat-server flood in best-effort mode can drop deliveries when a
  client outbox fills. This is expected policy, not a lossless guarantee; use
  lossless mode when the test requires backpressure and exact delivery.

* the runtime remains one active process runtime in the 1.x ABI. The handle API
  is the documented embedding boundary, not concurrent multi-runtime isolation.
