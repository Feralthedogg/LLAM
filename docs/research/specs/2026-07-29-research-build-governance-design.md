# Research and Build Governance Design

**Date:** 2026-07-29

**Status:** Approved as part of the runtime-backend productization program.

## Goal

Stable LLAM builds must contain only product runtime code. LEIR and other
research targets remain available for intentional experiments, but they must
not alter the installed API, ABI, package contents, or release decision.
Performance evidence must be immutable enough that a promotion verdict can be
reproduced from the exact source and raw samples.

## Research boundary

`LLAM_BUILD_RESEARCH` is a private build option, defaulting to `OFF` in CMake
and `0` in Make. It is part of every internal compilation signature but is
never present in installed target usage requirements, public headers,
pkg-config metadata, or exported symbols.

When disabled:

- research executables and tests are absent from the default graph;
- Linux native-segment translation units and their private structure members,
  tags, dispatch branches, and test-only completion sinks are absent;
- standard `all`, `test`, package, and CI paths do not compile research code.

When enabled:

- all research implementation and tests are available through an explicit
  target;
- the public installed headers, ABI metadata, dynamic export set, SONAME, and
  package interface remain identical to research-off;
- packaging and release targets fail before creating an artifact.

The option value is included in Make object/link signatures. A reused object
directory therefore cannot mix internal layouts from different modes.

## Canonical build metadata

One declarative manifest owns:

- semantic library version and ABI major;
- stable production source sets;
- platform source sets;
- public tests and internal tests;
- research source and target sets.

Make and CMake consume generated projections or are checked against the same
manifest. An audit fails on missing, extra, duplicated, platform-mismatched, or
differently linked members. Compiler dependency files remain authoritative for
header rebuilds.

The initial parity audit also closes known drift:

- `test_leir_native_plan` has the same link contract in Make and CMake;
- Windows IOCP/HANDLE test objects contribute to Make signatures;
- the Linux native-segment internal header contributes to private-header
  signatures.

## Immutable evidence bundle

Benchmark collection writes to a fresh temporary directory with exclusive file
creation. Successful completion atomically renames it to a previously absent
final directory. Existing or non-empty destinations are rejected.

Every bundle contains:

- `raw.csv`;
- normalized `summary.csv`;
- `metadata.json`;
- `verdict.json`;
- a human-readable report;
- `MANIFEST.sha256`, written last.

Metadata binds source commit, dirty-state digest, architecture, kernel,
toolchain, commands, CPU policy, workload matrix, sample/order schedule,
classifier schema, and frozen thresholds. Audit mode verifies the content
manifest, recomputes summaries and the verdict from raw data, rejects unknown
schema or provenance mismatches, and never rewrites the bundle.

The collection workflow is a nonblocking screen. A manual/release gate uses
`--require-verdict SPECIALIZED` and exits nonzero for `REJECT`, `INCONCLUSIVE`,
missing cells, provenance mismatch, or invalid evidence. Only non-trivial,
precommitted matrix cells count toward promotion. Platform-specific io_uring
benefits and portable runtime benefits are reported separately.

## Structure ratchet

The C structure audit uses structured finding categories rather than matching
message text. Strict mode fails explicit size-budget violations as well as
generic split candidates.

`include`, `src`, `examples`, `tests`, and `experiments` are all in scope.
Existing large test/research files receive a visible baseline, while new files
or growth beyond that baseline fail CI. Fixture tests prove scope selection,
budget promotion, baseline ratcheting, and malformed-configuration failure.

## Verification

- Default Make and CMake graphs contain no research targets or symbols.
- Research-on builds run the complete research suite.
- ABI/header/export/package-interface parity is byte-for-byte or
  contract-equivalent between modes.
- Packaging fails with research enabled and succeeds with it disabled.
- A mode toggle in one object directory forces correct recompilation.
- Manifest parity audits pass on POSIX and Windows target sets.
- Evidence creation, collision, partial failure, tampering, source mismatch,
  missing cells, and hard-gate outcomes have deterministic tests.
- Structure-audit fixtures and the repository ratchet run in normal CI.

No version, tag, or release is produced while the current native evidence
verdict is `REJECT`.
