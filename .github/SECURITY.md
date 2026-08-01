<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
Licensed under the LLAM Commercial Reciprocity License 1.0.
See the LICENSE file distributed with this Software.
-->

# Security Policy

## Supported versions

| Release line | Security updates |
| --- | --- |
| 3.x | Supported |
| 2.2.1 and earlier | Not supported |

The older releases remain available under the terms shipped with their
immutable tags, but they no longer receive security updates.

## Report a security defect privately

Do not disclose security-sensitive details in a public issue. Use
[GitHub Private Vulnerability Reporting](https://github.com/Feralthedogg/LLAM/security/advisories/new)
for suspected vulnerabilities, memory-safety defects, capability bypasses,
cross-runtime isolation failures, denial-of-service conditions, unsafe package
or installer behavior, and other security-impacting defects.

If the private channel is unavailable, open a public issue containing only:

- the affected release or commit;
- a statement that you need a private security contact; and
- a safe way to contact you.

Do not include exploit details, credentials, secrets, personal data, customer
data, regulated data, or other sensitive evidence in that public issue.

## What to include

Provide what is reasonably known and available:

- affected release, tag, commit, or package;
- operating system, architecture, compiler, build mode, and I/O/runtime
  backend;
- whether LLAM was modified and a high-level description of relevant changes;
- expected and observed behavior;
- reproduction steps, a minimal reproducer, logs, stack traces, crash data, or
  other technical evidence when safe and available;
- known or suspected impact; and
- a contact method for reasonable follow-up.

Sanitize anything you cannot lawfully or safely disclose. An initial report may
say that the issue is suspected, the root cause is uncertain, or investigation
is ongoing.

## License reporting windows

If Section 8 of the LLAM Commercial Reciprocity License 1.0 applies to your
Commercial Deployment, an initial Security Defect report is due without
unreasonable delay and no later than **7 calendar days** after Actual Knowledge.
Any other Reportable Defect must be reported no later than **45 calendar days**
after Actual Knowledge. See the repository [LICENSE](../LICENSE) for the
controlling definitions, exceptions, unavailable-channel procedure, and
complete terms.

This reporting channel does not promise confirmation, remediation, disclosure,
support, or a response schedule.
