<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: Apache-2.0
Licensed under the Apache License, Version 2.0.
See LICENSE.
-->

# Licensing

LLAM's first-party code is licensed under Apache-2.0. Third-party material keeps
its own terms.

## First-party license

All current first-party LLAM source code, public headers, documentation,
tests, examples, build files, and tools are licensed under the
[Apache License, Version 2.0](https://www.apache.org/licenses/LICENSE-2.0).
This includes current files that previously carried LLAM's custom license.

The complete, unmodified license is in the root
[`LICENSE`](https://github.com/Feralthedogg/LLAM/blob/main/LICENSE).
The current repository no longer uses a per-path license manifest.
First-party files without an embedded header are also covered by the root
license; generated or uncommentable files may use an adjacent `.license`
sidecar. New-file notices are specified in
[`CONTRIBUTING.md`](https://github.com/Feralthedogg/LLAM/blob/main/CONTRIBUTING.md).

Apache-2.0 permits commercial use. There are no additional LLAM-specific
source-disclosure duties, commercial reciprocity conditions, or mandatory
defect-reporting deadlines. Apache's own redistribution, notice, patent, and
other terms still apply; this summary does not replace the license.

## Contributions and reporting

Contributions are accepted under Apache-2.0 as described in `CONTRIBUTING.md`;
the Developer Certificate of Origin sign-off remains part of the contribution
process. Do not submit material you lack the right to contribute under those
terms.

Bug and security reports are voluntary. Suspected vulnerabilities should be
reported through the private route in
[the security policy](https://github.com/Feralthedogg/LLAM/blob/main/.github/SECURITY.md),
not through public issues containing sensitive details.

## Third-party material

Third-party material retains its original terms and copyright attributions.
The vendored `third_party/magic2/` snapshot remains MIT-licensed; its origin and
pinned revision are recorded in
[`UPSTREAM.md`](https://github.com/Feralthedogg/LLAM/blob/main/third_party/magic2/UPSTREAM.md).
Other algorithm and implementation attributions are in
[`THIRD_PARTY_NOTICES.md`](https://github.com/Feralthedogg/LLAM/blob/main/THIRD_PARTY_NOTICES.md).

Release packages and installed metadata include `LICENSE`, `NOTICE`,
`THIRD_PARTY_NOTICES.md`, and the magic2 license at
`third_party/magic2/LICENSE`. First-party relicensing does not replace
third-party licenses.

## Historical versions

The Apache-2.0 return was authorized for the current working tree on
2026-09-23. Old commits, tags, and already-distributed archives are not
rewritten by this change. Their accompanying terms remain part of those
historical copies; this is not a retroactive revocation of previously granted
rights. The former licensing plans are explicitly marked superseded and do
not govern the current repository. No new release is published by a local
license edit.

Verification records preserve their recorded Git inventories. A deleted file
name in a valid `git_status_before` porcelain snapshot describes that captured
working tree; it does not select the current license. The policy checker keeps
that history intact while still checking active paths, other record fields and
all current source and configuration.
