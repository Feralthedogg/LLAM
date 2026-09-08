# Phase 1A boundary-audit final fix report

Status: complete for the requested scanner, fixture, CMake/Make audit, and
documentation scope.  No runtime production source, public header, or ABI was
changed.  The implementation commit is `fix: close final phase 1a audit gaps`.

## Test-first evidence

The residual watchdog, ACK, owner, active-G, lock, and CMake-registration
fixtures were added before the checker changes.  The initial 41-test run was
RED with 13 expected bypass assertions (including the absent CMake audit).
After the strict allowlist and audit implementation, the suite was expanded
with owner-tuple/clear and symbol-policy precision cases and is GREEN at
43/43.

## Boundary changes

* Watchdog signals are recognized only in
  `llam_watchdog_check_shard`.  The checker requires the direct original and
  validated owner/incarnation/epoch/native-thread snapshot tuples, a positive
  conjunction containing all tuple equalities plus active-G and live-binding
  validation, or the exact production failure-return guard.  Conditional or
  non-dominating snapshots, output/handle rebinding, unknown output mutations,
  negated/OR predicates, and reachable unlocks fail closed.
* ACK matchers require direct atomic loads of the actual ACK/request fields,
  live request bit/generation, and lease/worker tuple equality in one positive
  conjunction.  Matcher calls must carry canonical request/epoch/incarnation
  arguments and route the tuple read; publication accepts a nonzero ACK only
  after direct tuple stores on an unconditional path.  Unknown helpers/macros,
  attacker branches, negations, wrong request generations, and conditional
  tuple stores fail closed.
* Owner writes and the owner-incarnation projection require the direct
  `lease = llam_shard_legacy_lease_locked(shard)` origin (or the audited init
  initializer), the exact named transition claimant/value/operator, visible
  state/worker/binding/sequence guards, and direct tuple publication.  Pointer
  indexing/movement, aliases, compound/unary/conditional/macro writes,
  unknown indirect stores, and unproved clears are rejected.  Only the exact
  init/finalize, redirect/transfer, and scheduler-unbind paths are allowlisted.
* Active-G helpers reject pre/post worker-pointer movement in addition to
  reassignment, unknown calls, aliases, and macro-hidden operations.
* Lock auditing retains only recognized pthread lock/trylock/timedlock/unlock
  operations and direct/simple aliases.  Generic wrappers and function
  pointers with lock receivers are rejected, and dynamic indexed receivers
  with different shard indices cannot be treated as one lock order.  The
  production `first`/`second` shard-ID ordering remains accepted.

## CMake production-hook policy

Added `scripts/audit_production_test_hooks.py` and the
`audit_production_test_hooks` CTest registration.  The audit uses `nm -a` (or
`llvm-nm`) on both `$<TARGET_FILE:llam_runtime>` and
`$<TARGET_FILE:llam_runtime_shared>`.  It rejects uppercase externally defined
symbols matching the broad `llam_*_test_*` policy, while explicitly allowing
lowercase/local fallback symbols emitted by unoptimized Debug artifacts.  The
existing `llam_broker_client_self_test_*` production self-test names remain the
only named exception.  Missing `nm`/`llvm-nm` returns CTest skip code 77; this
platform limitation and the local-symbol policy are documented in the
architecture and build guides.  Make's broad `nm -a` scheduler-prefix audit
and `$(PYTHON)` recipes remain unchanged.

## Verification evidence (Darwin arm64, 2026-09-09)

* `python3 scripts/test_legacy_lease_boundary.py`: 43/43 passed.
* `python3 scripts/test_worker_identity_boundary.py`: 20/20 passed.
* `python3 scripts/check_legacy_lease_boundary.py --root .`: clean, 187
  native C/H files.
* Exact adversarial fixtures for all six residual source-boundary groups are
  rejected, while the canonical active-G/watchdog/ACK/owner/lock forms remain
  clean.
* `PYTHON=/opt/homebrew/bin/python3.14 make test`: completed successfully,
  including runtime, ABI, no-owner, export, and boundary checks.
* `PYTHON=/opt/homebrew/bin/python3.14 make test-hardening`: completed the
  cppcheck (206 files), cargo-audit, ASan/UBSan, TSan, focused boundary, and
  full-fuzz gates; the known Darwin ASan stack-switch warning was the only
  sanitizer diagnostic.
* Fresh CMake Debug configure/build/CTest: 30/30 passed, including
  `audit_production_test_hooks`.
* Fresh CMake Release configure/build/CTest: 30/30 passed, including
  `audit_production_test_hooks`.
* CMake with `CMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE` configures and lists 24
  tests, intentionally omitting Python-backed boundary/export audits.
* `PYTHON=/opt/homebrew/bin/python3.14 make -B test_abi_contract test_abi_compat`,
  both ABI binaries, and `audit_shared_exports`: passed; 130 public shared
  symbols remain.
* `git diff --check`: passed.

## Scope and limitations

The checker is intentionally lexical/conservative rather than a full C parser,
preprocessor, CFG, or data-flow engine.  Its canonical allowlist is explicit;
unsupported macros, wrappers, aliases, pointer movement, and compiler-specific
control-flow semantics are rejected or not inferred.  Native Windows/BSD
runtime and sanitizer execution was unavailable on this Darwin host; CMake
registration/platform branches were inspected, while CMake Python discovery
and `nm` availability remain conditional as documented.  Phase 1A is complete
for the lease/current-G/watchdog/pause boundaries.  P-resource migration is
Phase 1B, foreign detach/reattach is Phase 2, and Rust, Python, JVM, .NET, and
Go wrappers remain explicitly excluded.
