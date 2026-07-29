# Testing Strategy

LLAM keeps coverage layered by cost and failure mode.

## Direct Runtime Tests

Direct tests own runtime correctness:

- lifecycle and shutdown
- task handle ownership
- cancellation
- lost wakeups
- channel close/select
- blocking callbacks
- multi-runtime isolation
- owned I/O buffers
- runtime diagnostics

When a server stress failure exposes a runtime bug, reduce it into a focused
runtime test.

## Integration Workloads

The example chat server is an integration workload. Lossless modes prove exact
fanout under backpressure. Best-effort flood modes allow bounded outbox drops,
but those drops must be accounted for by server-side stats.

## CI Layers

| Layer | Purpose |
| --- | --- |
| PR and push gates | Fast platform tests, package smoke, static checks. |
| Stress workflow | Repeated runtime and server stress with diagnostics artifacts. |
| Nightly deep CI | Longer stress, deterministic fuzz, sanitizer gates, benchmark guardrails. |
| Weekly soak | Direct runtime soak plus hour-long composite server soak. |
| Runtime benchmarks | Scheduled LLAM/Go/Tokio comparisons with CSV/PNG artifacts. |

## Security Tests

Broker and public-handle hardening live in `test_security_capability` and
`llam_broker --self-test`. Transport paths also run local client/server smoke
coverage.

## Stable And Research Build Boundary

`LLAM_BUILD_RESEARCH` is private and defaults to `OFF`. The stable and
research-on build graphs are tested separately, then their installed public
contracts are compared: the complete `include` tree, dynamic `llam_*` exports,
ABI probe result, ABI-major-2 shared-library identity/SONAME, `llam.pc`, and
the CMake imported-target consumer contract must be identical. The stable
install contains no experiment files, and a research-enabled package command
must fail before creating an archive.

Run the local boundary builds with:

```bash
make -j4 all test
make -j4 LLAM_BUILD_RESEARCH=1 research-test
cmake -S . -B build -DLLAM_BUILD_RESEARCH=OFF
cmake -S . -B build-research -DLLAM_BUILD_RESEARCH=ON
```

The research suite is experimental coverage, not a release surface. It cannot
alter the stable `2.2.0` public contract or authorize packaging.

## C Structure Ratchet

The structure audit scans C sources and headers under `include`, `src`,
`examples`, `tests`, and `experiments`. Findings are deterministic JSON
objects in relative-path order:

- `size_budget` identifies a file that exceeds its explicit budget.
- `split_candidate` identifies a file at or above the generic 800-line limit.
- `growth` identifies grandfathered debt that grew beyond its exact recorded
  line count.

`config/c-structure-baseline.json` records visible existing debt with schema
`llam.c-structure-baseline.v1`. It keeps large security and shutdown tests,
LEIR tests, `wait_tracking.c`, `issue.c`, and `linux_segment.c` visible without
allowing new debt or a one-line increase. Reducing or removing an entry's debt
passes the ratchet.

On POSIX hosts, source and baseline reads retain a root-to-leaf directory
descriptor chain and use no-follow opens. Hosts without directory-relative
no-follow support reject symlink components and verify file identity before
and after each read, but treat the selected checkout as a trusted authority
that is not concurrently renamed by an attacker.

Run the fixture tests and repository ratchet together with:

```sh
make audit-c-structure
```

For local assessment, `--mode report` emits warnings and exits zero when the
input and structural boundaries are valid. CI uses `--mode ratchet` with the
checked-in baseline. `--mode strict` promotes every explicit-budget and
generic split finding to an error and is opt-in until a scope's debt has been
split. Valid policy failures return 1; invalid roots, CLI arguments, unsafe
paths, or malformed baselines return 2.

## Release Rule

A release artifact should be published only after the target's platform-local
gate passes. Do not infer target readiness from another backend.
