# License File Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the LLAM custom license machine-discoverable, archive the former Apache 2.0 text without implying dual licensing, and make CI reject new LLAM implementation files that lack the current SPDX identifier.

**Architecture:** The root `LICENSE` remains the controlling current text. A byte-identical copy under `LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt` provides the standard SPDX/REUSE lookup location, while `OLD-LICENSES/Apache-2.0.txt` is explicitly historical. The existing Python policy checker enforces the layout and source-header rules, and packaging/install paths carry only the active license metadata.

**Tech Stack:** Python 3 `unittest`, Git tracked-file discovery, POSIX shell, PowerShell 7, CMake, Make, SPDX/REUSE file conventions.

## Global Constraints

- The root `LICENSE` remains the legally controlling text for the current repository snapshot.
- `LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt` must be byte-identical to `LICENSE`.
- `OLD-LICENSES/Apache-2.0.txt` must be the exact text shipped in `v2.2.1`, whose SHA-256 is `7d16370e642185e2eecad74eaf1e15179b27e2690f82644e7d247b395b600430`.
- `LICENSES/` contains only active license texts; Apache 2.0 stays outside it.
- Custom release archives and installed metadata include `LICENSE` and the active `LICENSES/` text, but exclude `OLD-LICENSES/`.
- New LLAM implementation, test, example, script, build, and GitHub policy files require `SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0` in the file or an adjacent `.license` file.
- Separately licensed material must live in a conspicuous `third_party/` or `vendor/` location with its own terms.
- Product metadata remains `3.0.0`; shared-library ABI major remains `2`.
- Do not merge the draft PR, create `v3.0.0`, or publish a release until the owner explicitly lifts the hold.
- Preserve the user's unrelated changes in the original `main` worktree.

---

### Task 1: Canonical and historical license files

**Files:**
- Create: `LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt`
- Create: `OLD-LICENSES/Apache-2.0.txt`
- Create: `OLD-LICENSES/README.md`
- Modify: `scripts/test_license_policy.py`
- Modify: `scripts/check_license_policy.py`

**Interfaces:**
- Consumes: root `LICENSE`; the exact `v2.2.1:LICENSE` bytes.
- Produces: `ACTIVE_LICENSE_PATH`, `HISTORICAL_LICENSE_PATH`, `HISTORICAL_APACHE_SHA256`, and policy errors for missing, mismatched, or misplaced license texts.

- [x] **Step 1: Add the immutable text assets used by the tests**

Create the active text as an exact copy of root `LICENSE`. Create the historical text from the exact bytes returned by `git show v2.2.1:LICENSE`. Add this scope notice in `OLD-LICENSES/README.md`:

```markdown
<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
-->

# Historical licenses

`Apache-2.0.txt` is retained only as a reference for `v2.2.1` and earlier
immutable releases. It is not an alternative license for the current
repository snapshot. For an earlier version, the `LICENSE` file stored at that
exact tag or commit controls.
```

- [x] **Step 2: Write failing layout tests**

Extend `_write_valid_repository()` so its root and active license copies are identical and it contains the exact historical Apache fixture and scope notice. Add these tests:

```python
SOURCE_ROOT = CHECKER.parent.parent
ACTIVE_LICENSE_RELATIVE = "LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt"
HISTORICAL_LICENSE_RELATIVE = "OLD-LICENSES/Apache-2.0.txt"
CURRENT_LICENSE_TEXT = (SOURCE_ROOT / "LICENSE").read_text(encoding="utf-8")
HISTORICAL_APACHE_TEXT = (
    SOURCE_ROOT / HISTORICAL_LICENSE_RELATIVE
).read_text(encoding="utf-8")
```

```python
def test_rejects_missing_active_license_text(self) -> None:
    (self.root / ACTIVE_LICENSE_RELATIVE).unlink()
    result = self._run_checker()
    self.assertNotEqual(0, result.returncode)
    self.assertIn("active LicenseRef text is missing", result.stderr)

def test_rejects_active_license_text_that_differs_from_root(self) -> None:
    self._write(ACTIVE_LICENSE_RELATIVE, "different license text\n")
    result = self._run_checker()
    self.assertNotEqual(0, result.returncode)
    self.assertIn("must be byte-identical to LICENSE", result.stderr)

def test_rejects_unexpected_active_license_file(self) -> None:
    self._write("LICENSES/Apache-2.0.txt", HISTORICAL_APACHE_TEXT)
    self._track("LICENSES/Apache-2.0.txt")
    result = self._run_checker()
    self.assertNotEqual(0, result.returncode)
    self.assertIn("LICENSES/Apache-2.0.txt: inactive license text", result.stderr)

def test_rejects_modified_historical_apache_text(self) -> None:
    self._write(HISTORICAL_LICENSE_RELATIVE, HISTORICAL_APACHE_TEXT + "modified\n")
    result = self._run_checker()
    self.assertNotEqual(0, result.returncode)
    self.assertIn("historical Apache text does not match v2.2.1", result.stderr)
```

- [x] **Step 3: Run the tests and verify the new cases fail**

Run: `python3 -m unittest -v scripts.test_license_policy`

Expected: the four new tests fail because the checker does not yet enforce the new layout.

- [x] **Step 4: Implement minimal layout enforcement**

Add constants and checks equivalent to:

```python
import hashlib

ACTIVE_LICENSE_RELATIVE = Path(
    "LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt"
)
HISTORICAL_LICENSE_RELATIVE = Path("OLD-LICENSES/Apache-2.0.txt")
HISTORICAL_NOTICE_RELATIVE = Path("OLD-LICENSES/README.md")
HISTORICAL_APACHE_SHA256 = (
    "7d16370e642185e2eecad74eaf1e15179b27e2690f82644e7d247b395b600430"
)
```

The checker must:

1. require all three files;
2. compare `LICENSE` and the active text with `read_bytes()`;
3. hash the historical Apache bytes with `hashlib.sha256()`;
4. require the scope notice to contain `v2.2.1`, `not an alternative license`, and `exact tag or commit`; and
5. reject every tracked file under `LICENSES/` except the active custom text.

- [x] **Step 5: Run the focused tests**

Run: `python3 -m unittest -v scripts.test_license_policy`

Expected: all layout and existing policy tests pass.

- [x] **Step 6: Commit the canonical layout**

```bash
git add LICENSES OLD-LICENSES scripts/check_license_policy.py scripts/test_license_policy.py
git commit -m "build: formalize active and historical licenses"
```

### Task 2: Default license policy for new LLAM files

**Files:**
- Modify: `scripts/test_license_policy.py`
- Modify: `scripts/check_license_policy.py`
- Modify: `.github/SECURITY.md`
- Modify: `.github/ISSUE_TEMPLATE/config.yml`
- Modify: `.github/ISSUE_TEMPLATE/defect-report.yml`
- Modify: `.github/workflows/docs.yml`
- Modify: `cmake/llam-config.cmake.in`
- Modify: `cmake/llam.pc.in`
- Modify: `scripts/bench_tokio_compare/Cargo.toml`
- Create: `scripts/bench_tokio_compare/Cargo.lock.license`
- Modify: `scripts/install.ps1`
- Modify: `scripts/package_release_windows.ps1`
- Modify: `scripts/verify_windows.ps1`
- Modify: `CONTRIBUTING.md`
- Modify: `docs/licensing.md`

**Interfaces:**
- Consumes: `tracked_files(root)` and the current `LICENSE_REF` constant.
- Produces: `requires_current_license(relative: Path) -> bool` and `has_current_license(root: Path, path: Path, text: str) -> bool`.

- [x] **Step 1: Write failing new-file tests**

Add this helper and the two cases below:

```python
def _track(self, *relative_paths: str) -> None:
    subprocess.run(
        ["git", "-C", str(self.root), "add", "--", *relative_paths],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
```

```python
def test_rejects_new_llam_script_without_current_license(self) -> None:
    self._write("scripts/new_tool.py", "print('hello')\n")
    self._track("scripts/new_tool.py")
    result = self._run_checker()
    self.assertNotEqual(0, result.returncode)
    self.assertIn("scripts/new_tool.py: current LicenseRef is missing", result.stderr)

def test_accepts_adjacent_license_file_for_generated_content(self) -> None:
    self._write("scripts/generated.lock", "generated = true\n")
    self._write(
        "scripts/generated.lock.license",
        "SPDX-FileCopyrightText: 2026 Feralthedogg\n"
        f"SPDX-License-Identifier: {LICENSE_REF}\n",
    )
    self._track("scripts/generated.lock", "scripts/generated.lock.license")
    result = self._run_checker()
    self.assertEqual(0, result.returncode, result.stdout + result.stderr)
```

- [x] **Step 2: Run the two tests and verify the missing-header case fails**

Run: `python3 -m unittest -v scripts.test_license_policy.LicensePolicyTest.test_rejects_new_llam_script_without_current_license scripts.test_license_policy.LicensePolicyTest.test_accepts_adjacent_license_file_for_generated_content`

Expected: the missing-header test fails because unmarked new files are not currently rejected.

- [x] **Step 3: Implement covered-path enforcement**

Require the current identifier for all tracked regular files under these prefixes:

```python
HEADER_REQUIRED_PREFIXES = (
    Path("src"),
    Path("include"),
    Path("tests"),
    Path("examples"),
    Path("scripts"),
    Path("cmake"),
    Path(".github"),
)
HEADER_REQUIRED_FILES = frozenset({Path("Makefile"), Path("CMakeLists.txt")})
```

Accept the identifier either inside the file or inside a tracked adjacent `<name>.license` file. Skip a `.license` file as an independent payload after validating its identifier. Continue rejecting stale Apache comment headers. Paths under future top-level `third_party/` and `vendor/` remain outside these LLAM-owned prefixes.

- [x] **Step 4: Bring existing covered files into compliance**

Use the comment syntax native to YAML, CMake/TOML, and PowerShell. Use an HTML comment for Markdown. Add `scripts/bench_tokio_compare/Cargo.lock.license` instead of editing the generated lockfile.

- [x] **Step 5: Document how contributors license new files**

Add a “New files and third-party material” section to `CONTRIBUTING.md` containing the exact current SPDX identifier, examples for C and `#`-commented files, the `.license` sidecar rule, and the requirement to isolate separately licensed material under `third_party/` or `vendor/`. Extend `docs/licensing.md` with links to the active and historical text locations and an explicit statement that the historical Apache copy is not a current licensing option.

- [x] **Step 6: Run the policy suite**

Run: `make check-license-policy`

Expected: all Python tests pass and the live repository reports `license policy ok`.

- [x] **Step 7: Commit the new-file default policy**

```bash
git add .github cmake scripts CONTRIBUTING.md docs/licensing.md
git commit -m "ci: enforce the current license on new files"
```

### Task 3: Package and install only active license metadata

**Files:**
- Create: `scripts/test_package_license_layout.py`
- Create: `scripts/test_package_license_layout_windows.ps1`
- Modify: `scripts/test_license_policy.py`
- Modify: `scripts/check_license_policy.py`
- Modify: `scripts/package_release.sh`
- Modify: `scripts/package_release_windows.ps1`
- Modify: `scripts/install.sh`
- Modify: `scripts/install.ps1`
- Modify: `CMakeLists.txt`
- Modify: `Makefile`
- Modify: `.github/workflows/stress.yml`
- Modify: `.github/workflows/release.yml`

**Interfaces:**
- Consumes: root `LICENSE` and `LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt`.
- Produces: archives and installations with `LICENSE` plus `LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt`, and without `OLD-LICENSES/`.

- [x] **Step 1: Write failing package behavior tests**

Create `scripts/test_package_license_layout.py`. It must build a temporary repository with the real POSIX packager and metadata generator, minimal safe build artifacts for the current host, distinct literal contents for the current and historical license files, and then execute the packager. Inspect the real `.tar.xz` with Python `tarfile` and assert:

```python
self.assertEqual(b"current license\n", archive.extractfile(f"{package}/LICENSE").read())
self.assertEqual(
    b"current license\n",
    archive.extractfile(
        f"{package}/LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt"
    ).read(),
)
self.assertFalse(any("/OLD-LICENSES/" in name for name in archive.getnames()))
```

Extract the controlled archive, run its real `install.sh`, and assert the same two current texts exist under `share/llam/`, while `share/llam/OLD-LICENSES` does not.

Create `scripts/test_package_license_layout_windows.ps1`. It must create a temporary repository and stub Windows build tree, run the real copied PowerShell packager, expand the resulting ZIP, and throw unless root `LICENSE` and the active `LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt` have the literal current text and `OLD-LICENSES` is absent.

- [x] **Step 2: Run the package tests and verify failure**

Run:

```bash
python3 -m unittest -v scripts.test_package_license_layout
docker run --rm -v "$PWD:/workspace" -w /workspace \
  mcr.microsoft.com/powershell:7.5-ubuntu-24.04 \
  pwsh -NoProfile -File scripts/test_package_license_layout_windows.ps1
```

Expected: both tests fail because the active `LICENSES/` text is absent from their archives.

- [x] **Step 3: Update POSIX and Windows packagers**

Both packagers must require and validate the active text, create `LICENSES/` in the staging directory, and copy only the active text into it. Do not copy `OLD-LICENSES/`.

- [x] **Step 4: Update installers and CMake install rules**

Archive installers copy `LICENSES/` to `share/llam/LICENSES/`. CMake installs the active text to `${CMAKE_INSTALL_DATADIR}/llam/LICENSES`. Root `LICENSE` remains installed at `${CMAKE_INSTALL_DATADIR}/llam/LICENSE`.

- [x] **Step 5: Repair package security fixtures**

Every temporary repository in `Makefile` that creates a root `LICENSE` before invoking `scripts/package_release.sh` must also create `LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt`. Preserve each fixture's original expected failure and do not weaken symlink, hard-link, mode, or path-safety assertions.

- [x] **Step 6: Wire package behavior tests into normal validation**

Run the POSIX integration test from `test-license-policy`. In the Windows stress job, build `llam_runtime` and `llam_runtime_shared`, then run `scripts/test_package_license_layout_windows.ps1` so the hosted Windows runner verifies real ZIP behavior. Extend the release workflow's existing archive assertions to require the active text and reject `OLD-LICENSES`.

- [x] **Step 7: Run package and installer tests**

Run: `make -j4 all test CC=clang`

Expected: existing runtime, installer, and package security tests pass.

- [x] **Step 8: Build and inspect a real local archive**

Run:

```bash
make package CC=clang
tar -tf target/dist/llam-3.0.0-macos-aarch64.tar.xz | rg 'LICENSE|OLD-LICENSES'
```

Expected: output contains the root `LICENSE` and active `LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt`, with no `OLD-LICENSES` entry. Extract both current texts and verify them with `cmp` against the repository copies.

- [ ] **Step 9: Commit packaging support**

```bash
git add CMakeLists.txt Makefile .github/workflows/stress.yml .github/workflows/release.yml scripts/check_license_policy.py scripts/test_license_policy.py scripts/test_package_license_layout.py scripts/test_package_license_layout_windows.ps1 scripts/package_release.sh scripts/package_release_windows.ps1 scripts/install.sh scripts/install.ps1
git commit -m "build: ship active license metadata"
```

### Task 4: Final validation and draft-PR update

**Files:**
- Verify only; modify earlier files only if a test exposes a defect.

**Interfaces:**
- Consumes: all commits from Tasks 1–3.
- Produces: a clean pushed branch and green draft-PR checks, with no tag or release.

- [ ] **Step 1: Run focused and structural validation**

```bash
make check-license-policy
git diff --check origin/main...HEAD
actionlint
ruby -e 'require "yaml"; Dir[".github/**/*.{yml,yaml}"].each { |f| YAML.load_file(f) }'
```

Expected: every command succeeds.

- [ ] **Step 2: Run full local build validation**

```bash
make clean
make -j4 all test CC=clang
cmake -S . -B target/cmake-license-layout -DCMAKE_BUILD_TYPE=Release
cmake --build target/cmake-license-layout -j4
ctest --test-dir target/cmake-license-layout --output-on-failure
```

Expected: Make tests and all CTest cases pass.

- [ ] **Step 3: Verify PowerShell syntax and install output**

Parse the scripts with PowerShell 7:

```bash
docker run --rm -v "$PWD:/workspace" -w /workspace \
  mcr.microsoft.com/powershell:7.5-ubuntu-24.04 \
  pwsh -NoProfile -Command '$failed = $false; foreach ($file in @("scripts/package_release_windows.ps1", "scripts/install.ps1", "scripts/verify_windows.ps1")) { $tokens = $null; $errors = $null; [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path $file), [ref]$tokens, [ref]$errors) | Out-Null; if ($errors.Count -ne 0) { $errors | ForEach-Object { Write-Error "${file}: $($_.Message)" }; $failed = $true } }; if ($failed) { exit 1 }'
```

Install into a temporary prefix and verify the metadata layout:

```bash
prefix="$(mktemp -d "${TMPDIR:-/tmp}/llam-license-install.XXXXXX")"
cmake --install target/cmake-license-layout --prefix "$prefix"
cmp LICENSE "$prefix/share/llam/LICENSE"
cmp LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt \
  "$prefix/share/llam/LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt"
test ! -e "$prefix/share/llam/OLD-LICENSES"
```

Expected: PowerShell reports no parser errors, both `cmp` commands succeed, and the historical directory is absent.

- [ ] **Step 4: Verify repository boundaries**

Confirm:

1. the feature worktree is clean after commits;
2. the original `main` worktree still contains exactly the user's pre-existing modifications;
3. the branch name and commit messages contain no prohibited automation branding;
4. no remote `v3.0.0` tag exists; and
5. no GitHub release named `v3.0.0` exists.

- [ ] **Step 5: Push and monitor the existing draft PR**

```bash
git push origin legal/relicense-3.0.0
gh pr checks 8 --watch --interval 10
```

Expected: required checks pass. The PR remains open and draft. Do not merge, tag, or release.
