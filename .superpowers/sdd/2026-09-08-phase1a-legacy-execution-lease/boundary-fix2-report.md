# Phase 1A boundary-audit hardening report (fix 2)

Status: complete for the scanner, fixture, build-audit, and documentation
scope.  No runtime production behavior was changed.  The Phase 0 scanner
(`scripts/check_worker_identity_boundary.py`) and the wrapper exclusion remain
unchanged.

## Fix

Implementation commit: `ae1bf402c4612ee55b6ee7b870c65c1591b2f5b3`
(`fix: close remaining phase 1a audit bypasses`).

The checker now fails closed for the source shapes identified by the re-review:

* Watchdog signals require a direct native-thread snapshot followed by a
  second snapshot, binding/incarnation proof and equality against the original
  handle in a dominating guard.  Reassignment of snapshot outputs and any
  lock unlock/reacquire between the validated snapshot and `pthread_kill`
  invalidate the proof; cleanup returns remain distinguishable from a path
  that reaches the signal.
* ACK matchers require direct atomic ACK, request-generation, and tuple loads
  in one live boolean expression, with the request bit and generation compared
  to the live request.  Tuple sampling is allowlisted to the actual matcher
  callers, publication/reset writers are allowlisted, dead-branch stores are
  rejected, and obvious unknown ACK/PAUSE macros or calls fail closed.
* Owner writes require a direct lease origin, exact `lease->owner = <known
  claimant>` values, live guard/binding/sequence proof, or an exact named reset
  path.  Compound/unary, conditional, macro-hidden, fake-alias, and owner-slot
  writes fail closed.  A declaration initializer using
  `llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);` is
  explicitly accepted.
* Scheduler lock tracking covers lock/trylock/timedlock/unlock, indexed shard
  receivers, aliases derived from `first`/`second`, cross-shard opaque-lock
  ID order, and direct opaque-before-shard order.  Parenthesized, unknown, and
  obvious wrapper/macro acquisitions are rejected.
* Active-G helper surfaces reject hidden calls and worker-parameter rebinding;
  direct worker atomic forms remain allowlisted.  `audit-shared-exports` now
  honors `$(PYTHON)`, while the broad `llam_*_test_*` production-hook policy
  remains covered.

## Regression fixtures

`python3 scripts/test_legacy_lease_boundary.py` contains 36 tests, including
the original positive paths and these new negative/precision fixtures:

* watchdog lock release/reacquire for both scheduler locks, output-target
  reassignment, arbitrary equality operand, and a non-dominating proof branch;
* dead matcher routing, dead tuple publication, unknown macro-hidden ACK
  state, and a matcher omitting live request generation/requested state;
* owner `+=`, `++`, macro-hidden write, fake lease origin, conditional
  attacker value, dead proof markers, unrelated clear, indirect slot writes,
  and the valid declaration initializer;
* reverse second/first opaque-lock order, `a`/`b` aliases derived from
  `first`/`second`, parenthesized calls, lowercase wrappers, and uppercase
  lock macros; and
* active-G macro publication/rebinding plus the Makefile Python override.

The fixtures were first run RED against the pre-fix checker (17 assertions
failed, including the known declaration-initializer false positive), then run
GREEN after the scanner changes.

## Verification (Darwin arm64, 2026-09-09)

* `python3 -m unittest discover -s scripts -p 'test_legacy_lease_boundary.py'`:
  36/36 passed.
* `python3 scripts/test_worker_identity_boundary.py`: 20/20 passed.
* `python3 scripts/check_legacy_lease_boundary.py --root .`: clean, 187 native
  C/H files.
* `PYTHON=/opt/homebrew/bin/python3.14 make test`: passed, including runtime,
  ABI, export, production-hook, no-owner, and boundary checks.
* `PYTHON=/opt/homebrew/bin/python3.14 make test-hardening`: passed, including
  cppcheck (206 files), cargo-audit, ASan/UBSan, TSan, fuzz, and boundary
  checks.  Darwin emitted only the known non-fatal ASan stack-switch warning.
* CMake Debug configure/build + CTest: 29/29 passed.
* CMake Release configure/build + CTest: 29/29 passed.
* `PYTHON=/opt/homebrew/bin/python3.14 make audit-shared-exports
  audit-production-test-hooks`: passed; 130 public shared exports and no
  disallowed production test-hook symbols.
* `git diff --check`: passed.  Public `include/llam` headers and
  `scripts/check_worker_identity_boundary.py` are byte-identical to commit
  `6f075c20`.

## Limitations and boundaries

The checker is intentionally lexical/conservative rather than a complete C
parser, preprocessor, or control-flow/data-flow engine.  It recognizes only
the direct Phase 1A source forms and rejects unsupported aliases, wrappers,
macros, compound expressions, and ambiguous proof paths.  Trylock/timedlock
success is conservatively treated as held.  Compiler-specific control-flow
semantics and native Windows/BSD execution were not exercised on this Darwin
host.  Phase 1A remains complete for lease/current-G/watchdog/pause boundaries;
P-resource migration is Phase 1B, foreign detach/reattach is Phase 2, and Rust,
Python, JVM, .NET, and Go wrappers remain explicitly excluded.
