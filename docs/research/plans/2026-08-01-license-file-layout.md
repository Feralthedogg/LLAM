<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
Licensed under the LLAM Commercial Reciprocity License 1.0.
See the LICENSE file distributed with this Software.
-->

# Mixed-License File Layout Implementation Plan

**Status:** Implemented

**Goal:** Preserve Apache 2.0 for previously published files while applying the
LLAM Commercial Reciprocity License 1.0 to files created after the transition.

## Constraints

- Keep the root `LICENSE` as the LLAM custom license.
- Store the canonical Apache text at
  `LICENSES/OLD-LICENSE/Apache-2.0.txt`.
- Store a sorted manifest at
  `LICENSES/OLD-LICENSE/APACHE-2.0-FILES.txt`.
- Treat the root `LICENSE` as an explicit custom-license exception.
- Preserve unrelated user changes while updating license notices.
- Include both license families and the manifest in release archives and
  installed metadata.

## Work completed

- [x] Classify current files from published Apache lineage tips
  `9062b687d3205ffdff24f3f1c05eac019b2b9a34` and
  `f4b3f4f39d254e2c1463ec33b5b3c58f5fb505b0`, following renames.
- [x] Create the 367-path Apache manifest and verify it is sorted, unique,
  relative, tracked, and free of parent traversal.
- [x] Move the exact Apache text under `LICENSES/OLD-LICENSE/` and retain its
  expected SHA-256
  `7d16370e642185e2eecad74eaf1e15179b27e2690f82644e7d247b395b600430`.
- [x] Restore Apache notices and explicit license-file pointers on manifested
  files without altering their non-license content.
- [x] Keep post-transition files on the custom license notice.
- [x] Extend policy tests for missing metadata, unsafe manifests, mismatched
  notices, modified Apache text, obsolete paths, and sidecars.
- [x] Extend POSIX and Windows package fixtures to require the custom text,
  Apache text, and Apache path manifest.
- [x] Update shell and PowerShell packagers, CMake install rules, Makefile
  security fixtures, and release workflow assertions.
- [x] Update public licensing documentation and changelog language to describe
  the repository as mixed-license.
- [x] Run focused policy and package tests, then run the repository validation
  appropriate to the affected build and packaging surfaces.

## Resulting selection rule

1. A path in `APACHE-2.0-FILES.txt` is Apache-2.0 licensed, including later
   modifications to that file.
2. A first-party path outside that manifest is governed by its custom LLAM
   notice and the root `LICENSE`.
3. Separately licensed third-party material retains its own conspicuous terms.
4. Immutable historical copies keep the license terms supplied with those
   copies.
