# Phase 1A legacy-execution-lease boundary fix 7

Decision: **CLOSED**

This fix closes the ordinary C parenthesized/cast/comma expression gaps
identified by `boundary-rereview6-report.md`.  The checker remains a lexical,
operation-closed allowlist: it rejects unsupported source forms instead of
trying to infer their data flow.  No runtime implementation or public ABI
change is included.

## Scope

Changed files are the Phase 1A checker, its fixture suite, and the build and
architecture documentation.  The native runtime sources, public headers,
P-field/foreign/async paths, wrappers, Make production-hook policy, and CMake
production-hook policy are unchanged.  Phase 1B P-resource migration, Phase 2
foreign detach/reattach, and Rust, Python, JVM, .NET, and Go wrappers remain
outside this audit.

## Closed residuals

- Active-G helper parameters are expression-stable: parenthesized/cast/comma
  assignments, pointer movement, bare address escapes, and slot writeback are
  rejected across publish, load, and clear.  The exact one-operation
  publish/load/clear forms remain accepted.
- ACK sampling freezes direct shard/runtime receivers and request,
  epoch, and worker-incarnation temporaries through the exact matcher call.
  Matchers require the exact owner helper, receiver, tuple arguments, and
  positive versioned predicates.  Publication is checked through the closing
  brace: tuple stores are unconditional field-derived releases, zero reset
  stores are allowed only before the publication edge, and no tuple or ACK
  store may follow the sole nonzero ACK store.
- Owner proofs freeze shard, sequence, incarnation, claimant, and worker
  values after their guards.  Parenthesized/cast/comma owner lvalues and
  indirect slot unary/compound/null writes are rejected regardless of slot
  provenance; direct declaration initializers and actual transitions remain
  valid.
- Watchdog output/current stability now catches grouped/cast/comma movement
  before, between, and after snapshots, as well as address aliases.  Failure
  snapshot exits require the braced direct-return form; nested or unbraced
  conditional returns do not establish a terminating proof.  Existing exact
  tuple/epoch/native/active-G/live-binding and lock-held requirements remain.
- Lock auditing rejects parenthesized/cast/comma receiver, index, and
  first/second role rebinding after acquisition.  Unknown wrappers,
  function-pointer calls, pointer-hidden locks, and noncanonical indices stay
  fail-closed while native first/second ordering remains clean.

## Fixture and validation evidence

- `python3 scripts/test_legacy_lease_boundary.py`: **124/124 passed**;
  this exceeds the 110-fixture Phase 1A requirement and includes the new
  grouped/cast/comma residual witnesses.
- `python3 scripts/test_worker_identity_boundary.py`: **20/20 passed**.
- `python3 scripts/check_legacy_lease_boundary.py --root .`: clean over
  **187 native C/H files**.
- New typed adversarial witnesses for Active-G, ACK sampling/publication,
  owner transitions, watchdog snapshots/CFG, and lock aliases were compiled
  with `cc -std=c11 -Wall -Wextra -Wpedantic -Werror -fsyntax-only`; all
  compiled cleanly and were rejected by the checker.
- `make -j4 test`: exit 0, including ABI contract/compatibility, runtime,
  source, no-owner, shared-export, production-hook, and broker checks.
- `make -j1 test-hardening`: exit 0.  cppcheck covered 206 files, cargo audit
  scanned 15 locked dependencies, ASan/UBSan and TSan binaries ran, and the
  2048/512 fuzz run completed.  The only sanitizer diagnostic was the known
  Darwin stack-switch `__asan_handle_no_return` warning.  An earlier parallel
  hardening run hit the unrelated timing-sensitive process-wrapper test;
  `make test-process-utils` passed independently and the sequential full
  hardening run passed.
- Fresh CMake Debug configure/build/CTest: **30/30 passed**, including
  `audit_production_test_hooks` and `audit_shared_exports`.
- Fresh CMake Release configure/build/CTest: **30/30 passed**, including
  `audit_production_test_hooks` and `audit_shared_exports`.
- CMake with `CMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE`: configured and listed
  the documented **24 non-Python tests**.
- `make audit-production-test-hooks audit-shared-exports`: passed with the
  same Python-driven production policy and **130 public shared symbols**.
- `python3 scripts/audit_c_structure.py`: exit 0 with the existing 27 size
  warnings.  `git diff --check` passed.

## Documentation and platform limits

`docs/internals/architecture.md` and `docs/build.md` now state the lexical
canonical allowlist, grouped/cast/comma rejection, pre-ACK zero-reset
exception, braced watchdog failure-return requirement, and Phase 1B/Phase 2
and wrapper exclusions.  Make and CMake continue to invoke the same
`scripts/audit_production_test_hooks.py` policy, with `nm`/`llvm-nm` fallback
and the documented local-symbol exception.

Validation ran on Darwin arm64 with Apple clang 21, CMake/CTest 4.3.2, and
Python 3.14.3.  Native Windows/BSD/Linux runtime behavior and
compiler-specific preprocessing/control-flow semantics remain CI
responsibilities.
