# Task 6 report — integration audits, documentation, and whole-branch verification

Status: complete for the Phase 1A integration boundary.

Integration commits:

* `ea7fb79` — initial Phase 1A boundary/build integration.
* `018d920c8b393fbf19b04638162f084a367d4837` —
  `fix: harden phase 1a boundary audit`.

The follow-up hardening report is [boundary-fix-report.md](boundary-fix-report.md).

## Boundary audit

The Phase 1A checker composes the unchanged Phase 0 worker-identity scanner
with token-precise checks for the exact atomic `active_g` field/init/helper
surfaces, versioned pause ACK tuple comparisons and publication order,
lease-owner claimant/binding proof, watchdog snapshot/native-thread binding and
lock lifetime, and all scheduler lock acquisition forms.  It rejects direct
shard `current`/`preempt_thread` authority, copied processor resources,
`owns_p`, owner aliases/slots, and unversioned ACK decisions.  The scanner
walks only native C/H runtime sources; Rust, Python, JVM, .NET, and Go wrappers
remain explicitly excluded.

The fixture suite now has 19 Phase 1A tests, including every reproduced false
negative from the independent review: malformed active-G type/object/atomic
surfaces, arbitrary or prematurely signalled watchdog targets, tautological
or misbound ACK/watchdog equality, dead-branch and aliased ACKs, indirect
owner writes, try/timed/aliased/reverse locks, a valid two-shard merge path,
and the scheduler test-hook export regression.  The standalone Phase 0 suite
remains 20/20.

## Build and registration

Make invokes both Phase 1A boundary checks from `test` and its ASan/UBSan and
TSan recipes using `$(PYTHON)` consistently.  The production hook audit uses
the broad `llam_*_test_*` policy with the known broker client self-test symbol
family excluded, and the scheduler-hook fixture remains covered.  CMake/CTest
retains the Python-conditional registration, now documented inline, and keeps
the Phase 0 boundary test alongside the Phase 1A tests.

The architecture page records Phase 1A as completed and points future work to
the Phase 1B P-resource field migration and Phase 2 foreign-detach boundary.
Public headers and the exported ABI remain unchanged.

## Verification evidence (Darwin arm64, 2026-09-09)

* `make test`: passed, including normal runtime, no-owner, boundary, export,
  and production-hook checks.
* `make test-asan` and `make test-tsan`: passed, including the focused legacy
  lease binaries and boundary checks.  ASan emitted only the known Darwin
  stack-switch warning.
* CMake Debug configure/build passed; full CTest passed 29/29.  A final
  post-edit CTest boundary/export subset passed 4/4.
* `make analyze-cppcheck`: passed for 206 files.
* `make audit-production-test-hooks audit-shared-exports`: passed with 130
  public shared exports.
* `python3 scripts/test_worker_identity_boundary.py`: 20/20 passed.
* `python3 scripts/test_legacy_lease_boundary.py`: 19/19 passed.
* `python3 scripts/check_legacy_lease_boundary.py --root .`: 187 source/header
  files clean.
* `PYTHON=python3.14 make test_legacy_lease_boundary
  check_legacy_lease_boundary`: passed.
* `git diff --check` passed; public headers are byte-identical to the pre-fix
  `d47c87dbaf3a06477133ef25388675f374a0598d` tree.

Native Windows/BSD execution was unavailable on this Darwin arm64 host.  The
review's historical active-helper leave failure did not reproduce in the
fresh full Make or CMake runs after rebuilding the existing runtime tree; this
scanner-only fix did not modify runtime production code.  A separate repeated
characterization still observed unrelated existing sigaltstack-EINVAL test
intermittency, which remains follow-up work rather than a scanner regression.
