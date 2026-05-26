# LLAM ChangeLog

## 2.0.0 - 2026-05-25

### Added

* BSD platform support foundation: add FreeBSD, OpenBSD, NetBSD, and DragonFly
  platform macros, share the kqueue readiness/user-wake backend with Darwin
  where portable, keep Mach/ulock paths Darwin-only, wire BSD x86_64/aarch64
  context paths into Make/CMake, add BSD release target detection, and add a
  BSD GitHub Actions VM smoke matrix for core/API/select/I/O-buffer/shared-load
  tests and package-shape checks.

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
  Raw capability issuance and validation now reject object slot zero with
  `EINVAL`, matching the nonzero broker live-object id space and preventing a
  slot-0 token from becoming structurally valid outside live-table checks.
  Direct broker object creation and direct object-token minting now enforce the
  same family-specific rights allowlist as the transport path, so buffer,
  descriptor, channel, and predefined-task tokens cannot carry out-of-family or
  future-reserved rights.
  Direct broker buffer registration also enforces the same 1MiB bounded grant
  limit as the transport path, preventing trusted-helper misuse from reserving
  unexpectedly large broker-local buffers.
  The unsupported Windows `llam_broker_register_fd()` compatibility stub now
  clears token output before returning `ENOTSUP`, matching other broker
  authority-returning failure paths.
  Failed capability attenuation and broker object-revocation helpers now clear
  output tokens even for in-place `token == out_token` calls, so destructive
  narrowing/revocation attempts cannot accidentally keep stale serialized
  authority after an `EACCES`/`EINVAL` failure.
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

* runtime-owned registries, caches, I/O buffers, blocking jobs, and scheduler
  paths are routed through explicit owner runtime state rather than implicit
  singleton state where an object owner is known.

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

* preserve managed task/shard TLS when an embedder creates an explicit runtime
  from inside an existing LLAM task. A successful nested `llam_runtime_create()`
  no longer makes the caller look like an unmanaged host thread for subsequent
  sync, I/O, or current-task queries.

* roll back registered partial runtime initialization failures through the
  normal runtime shutdown path. Failures after public runtime registration, and
  after backend setup such as Winsock startup on Windows, now release backend
  resources before returning the original initialization errno.

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

* bound broker destroy latency for predefined broker tasks. Broker teardown now
  requests cooperative runtime stop before draining task slots, so a client that
  leaves a long `SLEEP_NS_RETURN_U64` command unjoined cannot delay broker
  shutdown until the sleep duration naturally expires.

* prevent byte-stream broker `TASK_JOIN` from becoming a delay primitive for
  long sleeping broker commands. Pending `SLEEP_NS_RETURN_U64` joins now return
  `EAGAIN` promptly instead of driving the broker runtime until the
  client-selected sleep duration completes; otherwise quick joins also return
  `EAGAIN` while any peer sleep command is still pending, because driving the
  broker runtime would drain that sleep too.

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
  session table. Control-transport response failure now also rolls back
  just-created buffer, channel, descriptor, and predefined-task grants, so a
  disconnecting client cannot strand unreachable broker-owned heap, fd/HANDLE,
  channel, or task authority. Predefined-task rollback now claims the spawned
  slot with a CAS, clears client-visible rights, and lets the task detach
  itself from its trampoline before broker-slot reset. This prevents both a
  permanent detached state and a host-thread detach race with task exit/reclaim.
  Client-visible broker task detach now uses the same CAS claim before marking
  a spawned task detached, preventing a racing task completion from leaving an
  active `DETACHED` slot with no task handle to reclaim it.
  Windows named-pipe `CREATE_RING` response failure now closes any peer-process
  duplicated response HANDLE with `DUPLICATE_CLOSE_SOURCE`, preventing an
  unreachable remote mapping HANDLE from remaining in the client process.
  POSIX broker response
  writes use no-SIGPIPE send paths, turning closed-peer writes into ordinary
  `EPIPE`/`ECONNRESET` failures instead of process termination.

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
  helper `exec` calls. POSIX broker socket creation, descriptor/ring fd
  duplication, and `SCM_RIGHTS` receive paths now prefer `SOCK_CLOEXEC`,
  `F_DUPFD_CLOEXEC`, and `MSG_CMSG_CLOEXEC` where available, reducing the
  fork/exec race window before falling back to `fcntl(FD_CLOEXEC)`.
  Windows broker-registered HANDLEs now clear `HANDLE_FLAG_INHERIT` during
  descriptor registration, matching the POSIX close-on-exec boundary for
  child processes created with handle inheritance enabled.
  Failed POSIX broker wire reads now clear request/response output storage
  before returning an error, preventing partial attacker-controlled control
  messages from being reused by buggy callers that ignore the failure return.
  Direct broker-owned buffer reads, channel receives, and descriptor/HANDLE
  reads now clear caller output on failure and zero the unused suffix after
  successful short reads/receives, matching the shared-ring stale-output policy.
  Broker transport failure responses now also clear token, result, and data
  outputs after rolling back just-created authority, so a client that ignores
  `status < 0` cannot reuse stale success fields from a failed response.
  Plain POSIX request helpers, POSIX descriptor-request helpers, and Windows
  HANDLE request helpers now also normalize successfully read failure responses,
  preserving `status`/`error_code` while clearing token/result/data authority
  fields supplied by a faulty or hostile broker peer.
  The same helpers now reject malformed response framing with `EINVAL`, clear
  the full response payload, and close any attached response descriptor before
  returning, so a forged success frame with bad magic/version cannot expose
  token, result, data, fd, or HANDLE authority to a lax caller.
  POSIX response-descriptor reads now also close and suppress any `SCM_RIGHTS`
  fd attached to an error response, so malformed descriptor-bearing failures
  cannot expose fd authority alongside a failed status.
  Broker client request helpers now clear response storage on invalid input or
  request-write failure before a response can be read, covering closed-peer
  control sockets, descriptor-bearing requests, response-descriptor requests,
  Windows HANDLE request helpers, and unsupported Windows fd stubs.
  Broker shared-ring private-name generation now clears the name buffer before
  returning `ENAMETOOLONG`, and local endpoint helper failures now clear fd or
  HANDLE outputs to `-1`/`LLAM_INVALID_HANDLE`. This prevents callers that
  mishandle a failing return code from reusing stale rendezvous names or stale
  descriptor authority.
  Broker transport subject lookup, ring-session registration, ring stats
  collection, and ring submission/completion drain helpers now also clear their
  output storage before returning invalid-input or empty-ring errors, preventing
  stale subject ids, session ids, diagnostic counters, or completion payloads
  from surviving failed control-plane calls. Completion-drain helpers also
  reject impossible element counts with `EOVERFLOW` before size multiplication
  can wrap; the first completion slot is scrubbed as a stale-authority sentinel
  before the fail-closed return. Broker ring serve helpers, ring transport
  creation, task join result output, and shared-memory ring create/open/import
  helpers now follow the same fail-closed output rule, so failed direct broker
  calls cannot leave stale served counts, task results, fd/HANDLE descriptors,
  session ids, or mapping handles in caller-owned storage. Direct broker token
  mint helpers now also clear token output before invalid-broker failures, and
  POSIX broker socket identity capture clears cleanup identity output before
  invalid path failures so stale endpoint identity cannot be reused after a
  failed capture.

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

* degrade transient alternate-signal-stack setup failures on DragonFlyBSD from
  fatal scheduler startup errors into reduced crash-diagnostic coverage. The
  alternate stack is used to make guard-page fault dumps more actionable; an
  `EAGAIN` while a shard is rapidly moved across host pthreads should not make
  otherwise valid scheduler/run-token tests fail.

* keep the Windows security-capability helper build portable by moving helper
  definitions to the translation-unit scope expected by MSVC and CMake builds.

### Tests

* add explicit per-test timeouts to the BSD VM core-test gate so rare hangs
  report the failing executable instead of timing out the entire workflow.

* make the cancellation-token destroy race edge test yield from host pthread
  spin loops and use a smaller BSD smoke iteration count, avoiding false
  NetBSD timeouts while preserving the active-operation/destroy race coverage.

* treat DragonFlyBSD package repository/install outages as infrastructure skips
  while keeping FreeBSD, OpenBSD, and NetBSD package/test failures hard-gated.

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

* add Windows named-pipe `CREATE_RING` response-failure coverage proving a
  closed client pipe cannot strand a peer-local duplicated mapping HANDLE.

* add predefined broker task detach race coverage proving a concurrently
  completing task cannot strand an active detached slot after its public detach
  token is consumed.

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
