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
See LICENSES/Apache-2.0.txt.
-->

# Contributing to LLAM

Thank you for improving LLAM. Use an issue to discuss substantial behavior or
API changes before investing in a large patch. Security defects must be
reported through the private route in [.github/SECURITY.md](.github/SECURITY.md),
not through a public issue or pull request.

## Contribution license

By intentionally submitting a contribution for inclusion in LLAM, you agree
to license that contribution under the Apache License, Version 2.0,
unless the submission is conspicuously marked "Not a Contribution" or a
separate written agreement applies.

The controlling license text is in [LICENSE](LICENSE). If material in a
submission is governed by another license, identify it conspicuously before
submission and confirm that its terms permit inclusion.

## New files and third-party material

New LLAM-authored source, test, example, script, build, workflow, and project
policy files must carry the complete current license notice near the top of the
file.
For C-family files, use:

```c
/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSES/Apache-2.0.txt.
 */
```

For formats with `#` comments, use:

```text
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# See LICENSES/Apache-2.0.txt.
```

If a generated or uncommentable file cannot retain a header, add an adjacent
file named `<original-name>.license` containing:

```text
SPDX-FileCopyrightText: 2026 Feralthedogg
SPDX-License-Identifier: Apache-2.0
```

Do not apply that identifier to third-party material. Separately licensed
material must retain its original notices and be isolated under `third_party/`
or `vendor/`, with its origin and governing terms documented before inclusion.

## Developer Certificate of Origin

Every commit must include a Developer Certificate of Origin 1.1
`Signed-off-by` trailer. The trailer certifies that you have the right to
submit the contribution under the stated project terms. Review the
[Developer Certificate of Origin](https://developercertificate.org/) before
signing.

Create the trailer with:

```sh
git commit -s
```

The resulting commit message contains a line like:

```text
Signed-off-by: Your Name <you@example.com>
```

Do not sign for another person.

## Development checks

Build and run the default test suite before submitting:

```sh
make -j4 all test
make check-license-policy
```

For platform-specific setup and the extended verification matrix, see
[docs/getting-started.md](docs/getting-started.md) and the workflows under
`.github/workflows/`.

## Defect reports

Use the structured defect issue form for non-security problems. Include the
affected version, environment, backend, modification status, expected and
observed behavior, a reproducer when available, impact, and a follow-up contact.
Reporting is voluntary, not a condition of the software license. The private
reporting route and sensitive-data guidance are described in
[.github/SECURITY.md](.github/SECURITY.md).
