# Contributing to LLAM

Thank you for improving LLAM. Use an issue to discuss substantial behavior or
API changes before investing in a large patch. Security defects must be
reported through the private route in [.github/SECURITY.md](.github/SECURITY.md),
not through a public issue or pull request.

## Contribution license

By intentionally submitting a contribution for inclusion in LLAM, you agree
to license that contribution under the LLAM Commercial Reciprocity License
1.0, unless the submission is conspicuously marked "Not a Contribution" or a
separate written agreement applies.

The controlling license text is in [LICENSE](LICENSE). If material in a
submission is governed by another license, identify it conspicuously before
submission and confirm that its terms permit inclusion.

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
python3 scripts/test_license_policy.py
python3 scripts/check_license_policy.py
```

For platform-specific setup and the extended verification matrix, see
[docs/getting-started.md](docs/getting-started.md) and the workflows under
`.github/workflows/`.

## Defect reports

Use the structured defect issue form for non-security problems. Include the
affected version, environment, backend, modification status, expected and
observed behavior, a reproducer when available, impact, and a follow-up contact.
The reporting windows and sensitive-data rules are described in
[.github/SECURITY.md](.github/SECURITY.md).
