# LLAM 3.0.0 License Transition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish `v3.0.0` as the first LLAM Commercial Reciprocity
License 1.0 release, add the required operational policy, enforce it
automatically, and publish verified artifacts without changing ABI major 2.

**Architecture:** The repository root `LICENSE` is the authoritative license
for each new snapshot. Concise SPDX headers identify LLAM-authored files, a
policy checker enforces the transition contract in normal CI, and release
packaging continues to copy the authoritative license into every archive.

**Tech stack:** C11, Make, CMake/CTest, Python 3, POSIX shell, PowerShell,
GitHub Actions, GitHub repository security settings.

---

### Task 1: Record the transition contract

**Files:**
- Create: `docs/research/specs/2026-08-01-license-transition-design.md`
- Create: `docs/research/plans/2026-08-01-license-transition.md`

- [x] Confirm the baseline `make -j4 all test CC=clang` run exits successfully.
- [x] Record the version boundary, unchanged ABI, policy files, automated
      checks, and release acceptance criteria.
- [x] Verify both documents contain no unresolved placeholders.

### Task 2: Install the authoritative license and notices

**Files:**
- Replace: `LICENSE`
- Modify: every tracked LLAM-authored file carrying an Apache identifier or
  boilerplate
- Create: `docs/licensing.md`

- [x] Add a failing policy fixture/test for the exact section 1.4 text and
      prohibited stale Apache notices.
- [x] Replace `LICENSE` with the supplied text and its express application
      notice.
- [x] Convert existing LLAM-authored notices to
      `LicenseRef-LLAM-Commercial-Reciprocity-1.0`.
- [x] Document that `v2.2.1` and earlier remain Apache 2.0 while `v3.0.0`
      and later snapshots use the license shipped with each immutable version.
- [x] Run the policy test and confirm it passes.

### Task 3: Add reporting and contribution operations

**Files:**
- Create: `.github/SECURITY.md`
- Create: `.github/ISSUE_TEMPLATE/defect-report.yml`
- Create: `.github/ISSUE_TEMPLATE/config.yml`
- Create: `CONTRIBUTING.md`

- [x] Add supported-version and private-reporting guidance with the required
      seven-day security and forty-five-day general-defect reporting windows.
- [x] Add a structured non-security defect form with version, platform,
      backend, modification, reproduction, evidence, and impact fields.
- [x] Add the exact contribution-license clause and DCO sign-off workflow.
- [x] Validate all YAML files with a parser.
- [x] Enable GitHub Private Vulnerability Reporting and confirm the repository
      reports `enabled: true`.

### Task 4: Enforce the repository policy

**Files:**
- Create: `scripts/check_license_policy.py`
- Create: `scripts/test_license_policy.py`
- Modify: `Makefile`
- Modify: `.github/workflows/linux.yml`

- [x] Write tests that demonstrate failures for altered section 1.4, stale
      Apache notices, missing operational files, missing README disclosure, and
      release packaging that omits `LICENSE`.
- [x] Implement the checker using tracked-file input so ignored build output
      and historical Git objects are not scanned.
- [x] Add `check-license-policy` to `make check` and the Linux CI gate.
- [x] Run the unit tests and the checker against the working tree.

### Task 5: Create the 3.0.0 product boundary

**Files:**
- Modify: `CHANGELOG.md`
- Modify: `CMakeLists.txt`
- Modify: `Makefile`
- Modify: `README.md`
- Modify: `docs/abi.md`
- Modify: `docs/getting-started.md`
- Modify: `docs/security.md`
- Modify: `include/llam/runtime.h`
- Modify: `src/core/base/abi.c`
- Modify: `tests/test_shared_load.c`
- Modify: release/install scripts and CI workflow version constants

- [x] Add a `3.0.0` changelog entry describing the license boundary and
      operational files.
- [x] Change product/library version literals from `2.2.1` to `3.0.0`.
- [x] Keep `LLAM_ABI_MAJOR`, SONAME/install-name, and ABI documentation at 2.
- [x] Update README badge, install examples, and license section.
- [x] Confirm historical changelog entries and the licensing record still
      identify prior Apache releases accurately.

### Task 6: Verify builds, policy, and packages

**Files:**
- Verify only

- [x] Run `python3 scripts/test_license_policy.py`.
- [x] Run `python3 scripts/check_license_policy.py`.
- [x] Run `make clean && make -j4 all test CC=clang`.
- [x] Configure and build with CMake, then run CTest.
- [x] Parse workflow and issue-form YAML.
- [x] Parse the PowerShell packaging script in PowerShell 7.
- [x] Build a local release archive and verify `LICENSE`, `VERSION=3.0.0`,
      `LIBRARY_VERSION=3.0.0`, and ABI-major-2 library names.
- [x] Review the complete diff and confirm no user-owned checkout changes or
      unrelated files are included.

### Task 7: Publish and release

**Files:**
- Git and hosted repository state

- [ ] Commit the reviewed transition with a descriptive, neutral message.
- [ ] Push the dedicated branch and open a ready pull request.
- [ ] Wait for every required CI check; diagnose and fix failures in the branch.
- [ ] Merge only after the head commit and green checks are verified.
- [ ] Create and push immutable tag `v3.0.0` from the merged commit.
- [ ] Wait for the release workflow and public release to finish.
- [ ] Download the release assets and verify checksums, embedded license,
      version metadata, and ABI metadata.
