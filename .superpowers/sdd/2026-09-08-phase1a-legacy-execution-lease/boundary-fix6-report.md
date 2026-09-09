# Phase 1A legacy-execution-lease boundary fix 6

Decision: **CLOSED**

This fix closes the residual operation-shape and provenance bypasses identified
by `boundary-rereview5-report.md`.  The source audit remains deliberately
lexical and operation-closed: unsupported C forms are rejected rather than
inferred.  A future canonical operation must add both a fixture and an
explicit checker rule before it can be accepted.

## Scope

The change is limited to `scripts/check_legacy_lease_boundary.py`, its Phase 1A
fixture suite, and this report.  No runtime production source, public header,
ABI/P-field, foreign/async path, or language wrapper was changed.  Phase 1B
P-resource migration and Phase 2 foreign detach/reattach remain out of scope;
Rust, Python, JVM, .NET, and Go wrappers remain out of scope as well.

## Closed residuals

- ACK matchers now require exactly one positive
  `llam_shard_pause_owner_matches_locked(shard, epoch, incarnation)`
  conjunction, with the exact merge/steal request-bit, request-generation,
  ACK-bit, and three tuple comparisons.  Matcher parameters must remain stable
  and the body is expression-only.
- ACK sampling freezes the direct shard/runtime receiver and exact tuple-load
  origins through the matcher decision.  Publication freezes shard/TLS-worker
  provenance, rejects address aliases, requires release stores, and permits
  only one direct tuple publication per field before a nonzero ACK.
- Owner transitions and clears reject parenthesized/cast/comma lvalues,
  indirect slots, indexed/dereferenced writes, designated initializers,
  unknown/function-pointer calls, conditional proofs, lease-pointer movement,
  and parameter rebinding/address escapes.  The actual direct declaration
  initializer remains accepted.
- Watchdog checks protect all original/validated snapshot outputs and
  `current` from rebinding/address escapes, require the exact two snapshots and
  positive/failure guards, and reject unknown calls, labels/gotos, and lock
  reacquisition on the signal path.
- Active-G helpers are operation-closed: publish is one release store of
  `task`, load is one acquire load returned directly, and clear is one release
  NULL store plus the existing release timestamp clear.
- Lock auditing resolves aliases at source position, rejects post-acquisition
  alias rebinding, pointer/function-pointer/unknown wrappers, complex
  receivers, noncanonical indices, and cross-shard orders without visible
  `first`/`second` provenance.  Direct pthread lock/try/timed/unlock forms and
  the native first/second order remain accepted.

## Fixture and validation evidence

- `python3 -m unittest discover -s scripts -p 'test_legacy_lease_boundary.py'`:
  **96/96 passed** (77 prior fixtures plus residual regression coverage).
- `python3 -m unittest discover -s scripts -p 'test_worker_identity_boundary.py'`:
  **20/20 passed**.
- `python3 scripts/check_legacy_lease_boundary.py --root .`: clean over
  **187 native C/H files**.
- Typed residual witnesses (owner slot, parenthesized owner, and lock-alias
  rebinding) compiled with
  `cc -std=c11 -Wall -Wextra -Wpedantic -Werror -fsyntax-only`: **3/3 status 0**.
- `PYTHON="$(command -v python3)" make test_legacy_lease_boundary
  check_legacy_lease_boundary`: passed with the configured `$(PYTHON)`.
- Fresh CMake Debug and Release configure/build/CTest: **30/30 passed** in
  each configuration, including the production-hook and shared-export audits.
- CMake with `CMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE`: configured and listed
  the expected **24 non-Python tests**.
- `make test`: passed, including ABI contract/compatibility, runtime tests,
  source audits, production-hook audit, shared-export audit, and no-owner
  checks.
- `make test-hardening`: cppcheck covered **206 files**, cargo audit scanned
  **15 locked dependencies**, and ASan/UBSan, TSan, and 2048/512 fuzz paths
  completed successfully.  The known Darwin stack-switch
  `__asan_handle_no_return` warning was the only sanitizer diagnostic.
- Shared-export audit reports **130 public symbols**.  Make and CMake invoke
  the same `scripts/audit_production_test_hooks.py` policy over static/shared
  production artifacts; `nm` falls back to `llvm-nm`, lowercase local fallback
  symbols are allowed, uppercase external test hooks are rejected, and the
  broker self-test exception is preserved.
- `git diff --check` passed; public `include/llam` headers remain byte-identical
  to the d3 baseline.

## Platform limits

Validation ran on Darwin arm64.  Native Windows/BSD runtime behavior,
compiler-specific preprocessing/control-flow behavior, and native Linux
execution remain CI responsibilities.
