<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: Apache-2.0
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
See LICENSES/OLD-LICENSE/Apache-2.0.txt.
-->

# LLAM Mixed-License Transition Design

**Date:** 2026-08-01

**Revised:** 2026-08-03

## Decision

The root `LICENSE` contains the LLAM Commercial Reciprocity License 1.0, but
the repository is mixed-license. Files already published under Apache License
2.0 keep that license, including later modifications. First-party files created
after the transition use the custom license unless they are conspicuously
identified as separately licensed.

This supersedes the earlier all-current-files custom-license interpretation.
It preserves previously granted rights without changing immutable historical
tags, releases, commits, or received copies.

## Lineage classification

The transition audit compares the last published Apache lineages rooted at:

- `9062b687d3205ffdff24f3f1c05eac019b2b9a34`
- `f4b3f4f39d254e2c1463ec33b5b3c58f5fb505b0`

A current path descended from a file in either lineage remains Apache-licensed.
Renames are followed by Git history. The root `LICENSE` is an explicit
custom-license exception. A current first-party path with no qualifying
lineage is custom-licensed.

## Repository layout

```text
LICENSE
LICENSES/
  LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt
  OLD-LICENSE/
    Apache-2.0.txt
    APACHE-2.0-FILES.txt
```

- `LICENSE` is the human-facing custom license.
- The `LicenseRef` file is its byte-identical SPDX lookup copy.
- `Apache-2.0.txt` is the exact Apache text previously distributed by LLAM.
- `APACHE-2.0-FILES.txt` is the sorted, duplicate-free, repository-relative
  list of all current Apache-licensed paths.

The manifest is an express license selector and covers formats that cannot
carry a notice. Apache-capable files carry `SPDX-License-Identifier:
Apache-2.0` and point to `LICENSES/OLD-LICENSE/Apache-2.0.txt`. Custom-capable
files carry `SPDX-License-Identifier:
LicenseRef-LLAM-Commercial-Reciprocity-1.0`.

## Enforcement and distribution

The policy checker verifies the canonical Apache text hash, the exact custom
license mirror, manifest ordering and path safety, tracked-file membership,
license/header consistency, documentation, and packaging markers. It rejects
obsolete layout references and new unclassified first-party implementation
files.

Release archives, platform packages, archive-local installers, and CMake
install rules include all four pieces of license metadata. Package integration
tests verify their exact contents. This ensures recipients can resolve the
governing terms without reconstructing repository history.

## Public positioning

Public documentation describes LLAM as mixed-license. The custom portion is
source-available and not OSI-approved open source; the paths in the Apache
manifest remain Apache-2.0 licensed.

## Acceptance criteria

1. The manifest contains every and only current Apache-lineage path.
2. Apache paths retain complete Apache notices or valid sidecars where the file
   format permits them.
3. New first-party paths use the custom notice.
4. Root and packaged license metadata identify both license families.
5. Policy and package tests pass without weakening existing safety checks.
6. Unrelated worktree changes remain byte-for-byte unchanged apart from their
   required license notice.
