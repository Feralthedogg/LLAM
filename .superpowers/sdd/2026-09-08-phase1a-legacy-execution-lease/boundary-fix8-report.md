# Phase 1A legacy-execution-lease boundary fix 8

Decision: **CLOSED**

Fix 8 closes the three direct canonical-audit mismatches identified by the
final whole-branch review.  The checker remains a conservative lexical,
operation-closed allowlist.  No runtime production source, public header,
exported ABI, P-field/resource path, foreign/async path, or language-wrapper
source changed.

## Closed residuals

- ACK sampler matcher results now preserve polarity: a direct matcher return
  is the success path, a direct negation is accepted only for the canonical
  failure branch, and failure branches cannot return success.  Boolean result
  aliases are rejected, including address escape and overwrite through an
  alias, and the routed result is frozen through its decision.
- Owner transition/clear proofs now freeze coordinator and worker member state
  plus shard/worker/tuple identity values from the final failure guard through
  the owner/owner-incarnation publication pair.  The actual handback ACK
  assignment and canonical declaration initializers remain valid; transition
  writes after publication remain outside this pre-publication window.
- Watchdog proofs now freeze the shard receiver before its first lock and
  reject owner/validated-owner member mutation or address escape after the
  tuple snapshots.  The production two-snapshot, active-G, positive
  revalidation, and lock-held signal path remains clean.

## Fixtures and verification

The new Phase 1A tests include warning-clean typed C negative fixtures for
matcher polarity/result aliases, owner proof member/identity mutation, and
watchdog receiver/owner provenance.  Each typed fixture is checked with:

```text
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -fsyntax-only
```

Observed results from the fix-8 worktree:

- TDD red stage: the three new fixture groups initially failed on `abfdbfc`
  because the probes scanned clean.
- `python3 scripts/test_legacy_lease_boundary.py`: **127/127 passed**.
- `python3 scripts/test_worker_identity_boundary.py`: **20/20 passed**.
- `python3 scripts/check_legacy_lease_boundary.py --root .`: clean over
  **187** native C/H files.
- `make -j1 test_legacy_lease_boundary check_legacy_lease_boundary`: passed.
- Existing CMake Debug build plus targeted CTest for
  `test_worker_identity_boundary`, `test_legacy_lease_boundary`, and
  `check_legacy_lease_boundary`: **3/3 passed**.
- `git diff --check`: passed.

The two pre-existing untracked focused binaries in the worktree
(`test_legacy_lease_internal` and `test_worker_identity_internal`) were not
modified or removed.
