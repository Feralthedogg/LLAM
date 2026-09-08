# Phase 1A boundary-audit hardening fix 4

Status: complete for the residual lexical source-audit, fixture, production
artifact-hook, and documentation scope.  The implementation commit is
`fix: enforce canonical phase 1a audit forms`.  No runtime production source,
public header, public ABI, P-field, foreign/async path, or wrapper was
changed.

## Test-first evidence

The rereview's remaining bypasses were added as 20 new fixtures before the
checker changes.  The first run was intentionally RED: **12** of the new
assertions passed through the existing checker (the baseline suite was 43/43).
After the strict recognizers and provenance checks were implemented, the full
Phase 1A fixture suite is **63/63 green**.  The Phase 0 worker-identity suite
remains **20/20 green**.

## Boundary changes

* Watchdog validation is a canonical source form for
  `llam_watchdog_check_shard`: direct original and validated owner,
  incarnation, epoch, and native-thread snapshots; the exact positive
  conjunction; and the lease locks held through the signal.  Unbraced or
  conditional failure returns, jumps/labels, conditional snapshots, output
  address escapes, output rebinding, and reachable unlocks fail closed.  The
  small control-flow model tracks unbraced bodies so a conditional `return`
  cannot be mistaken for a dominating failure exit.
* ACK matchers and the explicit sampling-caller set now require direct atomic
  request/generation, ACK, and three tuple fields in one positive conjunction.
  Matcher arguments are tied to exact request/epoch/incarnation field origins
  and are checked for intervening aliases/reassignments.  Publication accepts
  a nonzero ACK only after direct tuple stores on an unconditional path, with
  direct request and worker-incarnation provenance.  Unknown sampling helpers
  and macros are rejected.
* Owner transitions require a direct lease-coordinator origin (including the
  canonical declaration initializer), returning failure guards, exact
  claimant/value derivation, and stable direct values.  Lease pointer
  movement/indexing, pointer/address escapes, unary/compound/macro/conditional
  owner writes, unknown indirect stores/clears, and unrelated designated
  initializers fail closed.  Scheduler unbind clears require the exact direct
  positive owner tuple condition.
* Active-G helpers reject worker address escapes, dereference slot writeback,
  pointer movement, parameter rebinding, and hidden calls while retaining the
  direct atomic helper allowlist.
* Lock auditing rejects generic lock/opaque wrappers, function-pointer aliases,
  unequal dynamic receivers, noncanonical indexed expressions such as `i+0`,
  and cross-shard opaque-lock acquisition without visible `first`/`second`
  shard-ID roles.  Canonical production two-shard ordering remains accepted.

## Production hook policy

The explicit CMake `audit_production_test_hooks` CTest remains registered for
both `$<TARGET_FILE:llam_runtime>` and
`$<TARGET_FILE:llam_runtime_shared>`.  The shared audit rejects uppercase
externally defined `llam_*_test_*` symbols, permits lowercase/local no-op
fallback symbols in unoptimized Debug artifacts, and preserves the sole
`llam_broker_client_self_test_*` exception.  A real temporary externally
defined `llam_scheduler_test_external` object is rejected by the Python audit
regression test.  Make's audit applies its broad `nm -a` check to both static
and shared artifacts; its no-`nm` omission and CMake's explicit CTest skip
(return code 77) are documented as platform limitations, so the policies are
not presented as identical where they are not.

## Verification evidence (Darwin arm64, 2026-09-09)

* `python3 scripts/test_legacy_lease_boundary.py`: **63/63 passed**.
* `python3 scripts/test_worker_identity_boundary.py`: **20/20 passed**.
* A 12-case adversarial matrix covering watchdog CFG/output escapes, ACK
  routing/provenance/publication, owner proof/indirection, active-G aliases,
  and lock wrappers/index expressions rejected **12/12**.
* `python3 scripts/check_legacy_lease_boundary.py --root .`: clean, **187**
  native C/H files.
* `make test`: completed successfully, including runtime, ABI, source-boundary,
  no-owner, export, broker, stress, and example checks.  Focused Make
  `test_legacy_lease_boundary check_legacy_lease_boundary` also passed.
* `make test-asan`: completed successfully.  Darwin emitted only its known
  non-fatal ASan `__asan_handle_no_return` stack-switch warning.
* `make test-tsan`: completed successfully with no reported race failure.
* Fresh CMake Debug configure/build/CTest: **30/30 passed**, including
  `audit_production_test_hooks` and `audit_shared_exports`.
* Fresh CMake Release configure/build/CTest: **30/30 passed**, including the
  same production static/shared artifact audit.
* Make shared-export audit and ABI contract/compatibility binaries passed;
  the shared artifact contains **130** public symbols.
* `git diff --check`: passed.  The diff from `bdb366f` contains only the
  checker, fixtures, and boundary/build documentation/report files; no
  `src/` or `include/` files changed.

## Scope and limitations

The checker is intentionally a lexical canonical allowlist, not a full C
parser, preprocessor, CFG, or whole-program data-flow engine.  Unsupported
macros, wrappers, aliases, pointer movement, noncanonical dynamic indices,
and compiler-specific control-flow semantics are rejected rather than inferred;
future production shapes require an explicit checker/fixture update.  Native
execution and sanitizer evidence here is Darwin arm64 only; native Windows,
BSD, and compiler-specific preprocessing/control-flow behavior were not
executed.  Python-backed CMake tests remain conditional on Python discovery,
and the production-hook audit is conditional on `nm`/`llvm-nm` availability.
Phase 1B P-resource migration, Phase 2 foreign detach/reattach, and Rust,
Python, JVM, .NET, and Go wrappers remain explicitly outside Phase 1A.
