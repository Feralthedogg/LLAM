<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
Licensed under the LLAM Commercial Reciprocity License 1.0.
See the LICENSE file distributed with this Software.
-->

# Licensing

The current LLAM repository is **mixed-license**. A file's governing terms are
selected by its file notice and by the explicit Apache path manifest described
below; the root license does not replace rights already granted for previously
published Apache-licensed files.

## LLAM Commercial Reciprocity License 1.0

The root [LICENSE](https://github.com/Feralthedogg/LLAM/blob/main/LICENSE) is
the human-facing copy of the LLAM Commercial Reciprocity License 1.0. The same
text is provided byte-for-byte for SPDX-aware tooling at
[LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt](https://github.com/Feralthedogg/LLAM/blob/main/LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt).

The custom license applies to files carrying this identifier and to newly
created first-party files that are not in the Apache manifest:

```text
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
```

This is a source-available license. It is not an OSI-approved open source
license.

## Apache License 2.0 files

Files already distributed under Apache License 2.0 remain under those terms,
including later modifications to the same files. Their exact current paths are
listed in
[LICENSES/OLD-LICENSE/APACHE-2.0-FILES.txt](https://github.com/Feralthedogg/LLAM/blob/main/LICENSES/OLD-LICENSE/APACHE-2.0-FILES.txt),
and the governing text is stored at
[LICENSES/OLD-LICENSE/Apache-2.0.txt](https://github.com/Feralthedogg/LLAM/blob/main/LICENSES/OLD-LICENSE/Apache-2.0.txt).

Apache-marked source files use `SPDX-License-Identifier: Apache-2.0` and point
to that text. For formats that cannot carry a header, an adjacent `.license`
sidecar supplies the same notice.

The manifest is authoritative for **Untagged** files as well: a listed path is
Apache-licensed even if its format has no in-file notice. A path not listed in
the manifest is not made Apache-licensed merely because a similarly named file
existed in another revision.

## Historical boundary and lineage

Immutable tags, commits, releases, and copies keep the terms under which they
were received. In particular, `v2.2.1` and earlier published snapshots remain
under the Apache License 2.0 terms shipped with those snapshots.

For the transition audit, Apache lineage was reconstructed from the published
tips `9062b687d3205ffdff24f3f1c05eac019b2b9a34` and
`f4b3f4f39d254e2c1463ec33b5b3c58f5fb505b0`. The root `LICENSE` is an explicit
custom-license exception. Files created after that boundary use the custom
license unless they contain separately licensed material.

## Release packages and third-party material

Release archives and installed metadata include the root `LICENSE`, its
`LicenseRef` mirror, the Apache 2.0 text, and the Apache file manifest. This
keeps the mixed-license selection rules available alongside every distributed
copy.

Separately licensed third-party material must retain its own copyright and
license notices and be recorded conspicuously. Neither LLAM license replaces
those terms.
