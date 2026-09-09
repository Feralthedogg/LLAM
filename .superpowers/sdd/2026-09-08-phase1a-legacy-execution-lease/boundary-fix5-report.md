# Phase 1A canonical boundary-audit hardening fix 5

## Status

Complete for the residual Phase 1A source-audit and production-hook gaps
identified by `boundary-rereview4-report.md`.  This change is checker,
fixture, build-hook, and documentation only.  No runtime implementation,
public header, exported ABI, P-field, foreign/async path, or language wrapper
was changed.

## Test-first evidence

The residual cases were added as failing fixtures before the corresponding
checker changes.  Against `a281719`, the new ACK, owner, and lock cases passed
through the checker; the failure output was the expected empty diagnostic
list.  After the recognizer hardening, the Phase 1A suite is **77/77** and the
Phase 0 worker-identity suite is **20/20**.

The 12 residual fixture cases cover exact ACK argument provenance, matcher
result recombination, pre-origin temporary aliases, duplicate ACK tuple
publication, owner proof dominance, owner clearing, nested lease rebind,
indirect null slots, watchdog current/output lifetime, and mixed-shard or
pointer-hidden locks.  The lock case contains both report examples (13 C11
source forms total); all were rejected by `scan_source()` and compiled with
`cc -std=c11 -Wall -Wextra -Wpedantic -Werror -fsyntax-only` using minimal
typed fixtures.

## Boundary changes

* ACK sampling now accepts only an unmodified direct atomic load or a stable
  temporary initialized by the exact request-generation,
  ACK-lease-epoch, or ACK-worker-incarnation field.  Parenthesized/cast/
  arithmetic/conditional/attacker expressions, address aliases before the
  load, temporary rebinds, and recombined matcher results fail closed.  Matcher
  bodies require canonical acquire loads, a positive request bit/generation,
  the ACK bit, and all three exact tuple comparisons for both merge and steal.
  Publication requires one unconditional canonical store per tuple field;
  duplicate stores and conditional overwrites after the canonical store are
  rejected.
* Owner proofs require every visible lease assignment to be the direct
  coordinator origin, including the valid declaration initializer.  Failure
  guards return the canonical false value with exact predicate arms; marker
  expressions, conditional proof wrappers, wrong values, nested pointer
  rebind/movement/indexing, unknown calls/macros, indirect/unary/compound
  stores, null slots, and unrelated designated initializers fail closed.
* Watchdog validation protects both snapshots and the active-G `current`
  operand through the signal.  Output/current address aliases from function
  scope, parenthesized lvalues, direct reassignments, conditional/non-exact
  snapshot-failure guards, jumps, and reachable unlocks fail closed.  The
  production two-snapshot/positive-revalidation/lock-held signal path remains
  accepted.
* The Active-G helper audit now recognizes parenthesized worker-address
  escapes as aliases while retaining only direct atomic helper calls.
* Lock auditing requires a matching opaque lock for each receiver in a
  mixed-shard protocol, retains the visible first/second ID-ordered source
  form, rejects noncanonical indices and unknown lock-like wrappers, and
  rejects direct scheduler lock-field pointer escapes.  Legitimate separate
  lock-only sections and direct opaque condition waits remain accepted.
* Make and CMake use the same `scripts/audit_production_test_hooks.py` policy
  over static/shared production artifacts.  It uses configured `$(PYTHON)`,
  `nm -a` with `llvm-nm` fallback, rejects uppercase external
  `llam_*_test_*` definitions, permits lowercase/local no-op fallbacks, and
  keeps `llam_broker_client_self_test_*` as the sole exception.  The existing
  synthetic external-symbol regression remains covered by the Python fixture
  suite; CMake keeps the registered audit and 77 platform skip.

## Verification evidence (Darwin arm64, 2026-09-09)

* `python3 scripts/test_legacy_lease_boundary.py`: **77/77 passed**.
* `python3 scripts/test_worker_identity_boundary.py`: **20/20 passed**.
* `python3 scripts/check_legacy_lease_boundary.py --root .`: clean, **187**
  native C/H files.
* `PYTHON="$(command -v python3)" make test_legacy_lease_boundary
  check_legacy_lease_boundary`: passed; the configured interpreter override
  was honored.
* `make test`: **exit 0**, including runtime, ABI, source-boundary,
  no-owner, broker, stress, example, shared-load, and artifact checks.  The
  Darwin-only Windows IOCP binaries reported their expected skips.
* `make test-hardening`: **exit 0**; cppcheck covered **206** files, cargo
  audit scanned the 15 locked Rust dependencies, sanitizer matrices passed,
  the focused boundary checks passed, and the 2048/512 fuzz matrix passed.
* Fresh CMake Debug configure/build/CTest: **30/30 passed**, including
  `audit_production_test_hooks` and `audit_shared_exports`.
* Fresh CMake Release configure/build/CTest: **30/30 passed**, including the
  same static/shared production-hook audit.
* `make audit-production-test-hooks`: passed for both static/shared artifacts;
  the synthetic external-definition test and `llvm-nm` fallback unit check
  passed.
* `python3 scripts/audit_c_structure.py`: exit 0 with the repository's 27
  pre-existing size warnings; public-header diff against `a281719` is empty.
  `git diff --check a281719 --` passed.

## Canonical allowlist and scope

The checker remains a lexical canonical allowlist, not a C parser,
preprocessor, CFG, or whole-program data-flow engine.  Unsupported macros,
wrappers, aliases, pointer movement, noncanonical indices, and
compiler-specific control-flow forms are rejected rather than inferred.
Phase 1A is complete.  P-resource migration remains Phase 1B; foreign
detach/reattach remains Phase 2; Rust, Python, JVM, .NET, and Go wrappers are
explicitly excluded from this audit.
