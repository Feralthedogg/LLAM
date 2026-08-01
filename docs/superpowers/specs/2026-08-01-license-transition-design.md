# LLAM 3.0.0 License Transition Design

**Date:** 2026-08-01

## Objective

Adopt the LLAM Commercial Reciprocity License 1.0 for new LLAM releases while
preserving the Apache License 2.0 grants already attached to published commits,
tags, releases, and copies.

This document records a repository and release policy. It is not a legal opinion
about the enforceability of the custom license.

## Release boundary

- The first release under the new license is `v3.0.0`.
- `v2.2.1` and every earlier immutable tag, release, commit, and copy retain
  their existing Apache License 2.0 terms.
- Published history is not rewritten and existing tags are not moved.
- The product major version changes because the licensing terms materially
  change. The shared-library ABI remains at major version 2 because this change
  does not alter the binary interface.
- `docs/licensing.md` will make the boundary discoverable without requiring a
  reader to reconstruct it from Git history.

## License application

The repository root `LICENSE` will contain the supplied LLAM Commercial
Reciprocity License 1.0 text. Section 1.4 will use the repository-aware
`Software` definition approved for this transition:

```text
1.4. "Software" means source code, object code, documentation, tests,
examples, build materials, configuration, and other materials included in a
release, branch, commit, package, repository snapshot, or copy to which this
License is expressly applied by a LICENSE file, package metadata, file header,
or other accompanying notice, excluding materials expressly identified as
being governed by another license.
```

The root `LICENSE` therefore applies the new terms to the complete repository
snapshot unless a material is conspicuously identified as separately licensed.
Existing LLAM-authored tracked files that currently carry an Apache identifier
or boilerplate will receive this concise notice in the syntax appropriate to
their file type:

```text
Copyright 2026 Feralthedogg
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
```

The transition audit found no separately owned tracked source that must remain
under Apache 2.0. Future third-party or separately licensed material must retain
its own license notice and must be excluded from automated LLAM-header checks.

## Public positioning

The README license badge and license section will call LLAM
**source-available** and explicitly state that its license is not OSI-approved
open source. The project must not be described as open source without that
qualification.

## Reporting and contribution operations

### Security reporting

`.github/SECURITY.md` will:

- identify `3.x` as supported and earlier release lines as unsupported;
- direct security reports to GitHub Private Vulnerability Reporting;
- explain that public issues must not contain security-sensitive details;
- request version, platform, backend, modification status, impact,
  reproduction steps, and supporting evidence;
- describe the license's seven-day security-defect reporting requirement and
  thirty-day non-security-defect reporting requirement; and
- avoid promising a response or remediation service-level agreement.

Private Vulnerability Reporting will be enabled and verified for the public
repository. General defects will use a structured GitHub issue form with the
same diagnostic fields and a prominent security-reporting redirect.

### Contributions

`CONTRIBUTING.md` will include the approved contribution-license clause
verbatim:

```text
By intentionally submitting a contribution for inclusion in LLAM, you agree
to license that contribution under the LLAM Commercial Reciprocity License
1.0, unless the submission is conspicuously marked "Not a Contribution" or a
separate written agreement applies.
```

Contributors will certify origin and licensing with a Developer Certificate of
Origin `Signed-off-by` trailer, normally added with `git commit -s`.

## Automated enforcement

A repository policy checker will fail when:

- the root license is missing or section 1.4 differs from the approved text;
- a tracked LLAM file still contains the Apache identifier or boilerplate,
  except for explicit historical references in the licensing record;
- a currently license-marked LLAM file lacks the new `LicenseRef`;
- README does not state the source-available/non-OSI status;
- the security policy, contribution guide, issue form, or licensing record is
  missing or incomplete; or
- release package scripts stop including the root `LICENSE`.

The checker will be part of normal `make check`/CI execution. Package tests
will also inspect produced archives so release artifacts contain the exact
license and the expected `3.0.0` product metadata while retaining ABI major 2.

## Release workflow

The implementation is published on a dedicated branch and reviewed by the full
CI matrix. Only after required checks pass will it be merged. The immutable
`v3.0.0` tag is then created from the merged commit and the release workflow
is allowed to build and publish artifacts. Published archives, checksums,
embedded license, version metadata, and ABI metadata are verified before the
transition is considered complete.

## Acceptance criteria

1. Current tracked LLAM materials and generated release archives are governed
   by the supplied custom license, with the approved section 1.4.
2. Historical Apache-licensed releases remain untouched and documented.
3. Private and public defect-reporting routes exist and are usable.
4. Contribution licensing and DCO sign-off are documented.
5. Version `3.0.0` is consistent across code, documentation, workflows, and
   packages while ABI major remains 2.
6. Local validation and the required hosted CI checks pass.
7. The `v3.0.0` release is published and its artifacts verify successfully.
