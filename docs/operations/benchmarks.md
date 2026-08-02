# Benchmarks

Benchmarks are for regression tracking and platform tuning. They should not be
mixed with correctness claims.

For every benchmark script parameter, see [CLI And Tools](../reference/cli-tools.md#benchmark-scripts).

## Run All Cases

```sh
./bench
```

Run one case:

```sh
LLAM_BENCH_ONLY=spawn_join ./bench
LLAM_BENCH_ONLY=channel_pingpong ./bench
LLAM_BENCH_ONLY=io_echo ./bench
LLAM_BENCH_ONLY=poll_wake ./bench
LLAM_BENCH_ONLY=sleep_fanout ./bench
LLAM_BENCH_ONLY=opaque_block ./bench
```

## Scale Workload Size

```sh
LLAM_BENCH_ROUNDS=31 LLAM_BENCH_WARMUP_ROUNDS=5 ./bench
LLAM_BENCH_SPAWN_TASKS=512 ./bench
LLAM_BENCH_CHANNEL_MESSAGES=4096 ./bench
LLAM_BENCH_IO_MESSAGES=512 ./bench
LLAM_BENCH_POLL_EVENTS=512 ./bench
```

## Runtime Comparisons

Go comparison:

```sh
go run scripts/bench_go_compare.go
```

LLAM, Go, and Tokio:

```sh
python3 scripts/bench_runtime_compare.py --runtime all --isolate-cases
```

Release-quality comparison numbers should use isolated case execution:

```sh
python3 scripts/bench_runtime_compare.py \
  --runtime all \
  --cases spawn_join,select_recv_ready,poll_wake \
  --isolate-cases
```

The scheduled `Runtime Benchmarks` workflow uploads CSV and PNG artifacts for
Linux, macOS, and Windows lanes.

## Connected LEIR Native Pipeline

The connected-pipeline gate measures compiled LEIR `RECV`→`SEND` effect
segments against the public LLAM I/O path on Linux:

```sh
make bench_leir_native_pipeline
python3 scripts/bench_leir_native_pipeline.py \
  --binary ./bench_leir_native_pipeline \
  --samples 9 \
  --activations 32 \
  --min-mode-ms 20 \
  --output-dir object/leir-native-pipeline
python3 scripts/bench_leir_native_pipeline.py \
  --audit-existing object/leir-native-pipeline
```

Collection and ordinary audit are screening operations. A valid bundle exits
zero even when its portable verdict is `REJECT` or `INCONCLUSIVE`. Promotion is
a separate, explicit operation:

```sh
python3 scripts/bench_leir_native_pipeline.py \
  --audit-existing object/leir-native-pipeline \
  --require-source "$(git rev-parse HEAD)" \
  --require-verdict SPECIALIZED
```

The promotion command exits `1` when the stored `portable_verdict` is not
exactly `SPECIALIZED`, and exits `2` when the evidence or required source is
invalid. Pull-request CI performs collection and audit without the promotion
option. Manual and release promotion use the hard gate. Release artifact and
publication jobs depend on evidence measured and audited at the exact release
SHA by the local reusable workflow.

`--require-source COMMIT` is the promotion shorthand for the exact provenance
pair `(COMMIT, clean)`. It cannot be combined with
`--require-source-commit` or `--require-source-dirty-digest`. Audits that
intentionally accept a non-clean source must instead supply both granular
flags with the exact stored commit and dirty digest.

This is explicitly a **Linux/io_uring specialized evidence** run. It covers
`link_skip` and `fixed_link_skip`, widths 1/2/4/8, concurrency 1/4/16, and
payloads 64/512/4096. Each cell has one discarded fresh-process warmup and
nine measured fresh-process pairs with alternating ABBA/BAAB order.

CI constrains the runner and its peer process to the first two available CPUs.
Using one CPU for both processes serializes the socket peer with the runtime
under test and is not valid evidence. The job fails closed when fewer than two
CPUs are available and records both the selected and allowed affinity masks.
The earlier whole-matrix native-segment screen is no longer a release input;
its focused direct, ASan, and TSan mechanism checks remain in the workflow.

The output directory contains:

- `raw.csv`: every accepted process result and structural counter.
- `summary.csv`: deterministic paired-log bootstrap estimates and 95%
  confidence intervals.
- `metadata.json`: invocation, source, environment, frozen matrix, sample
  schedule, and classifier metadata.
- `verdict.json`: the exact machine-readable promotion and platform verdicts.
- `report.md`: human-readable gates and cell results.
- `MANIFEST.sha256`: the final content seal.

The audit command recomputes the summaries, verdicts, and report from
`raw.csv`, checks every manifest member, and never rewrites the bundle.
`verdict.json` uses `llam.performance-verdict.v2` exact-schema JSON containing
`portable_verdict`,
`platform_verdict`, `required_cells`, and `classifier_thresholds` in addition
to the compatibility `verdict` alias and reasons. Legacy three-field v1
evidence remains auditable by the shared bundle reader, while scoped fields
require the complete v2 shape. The alias is required to equal
`portable_verdict`; unknown or partially extended variants fail closed.

Verdicts have narrow meanings:

- `portable_verdict` evaluates the frozen classifier over only explicit
  non-trivial cells, where `min(batch_width, concurrency) > 1`. Missing cells,
  insufficient samples, structural errors, supported wall regressions, or
  CPU/p99 regressions prevent promotion.
- `platform_verdict` independently adds the full Linux/io_uring fixed-resource
  and width-4/8 batching comparisons.
- `SPECIALIZED` means the applicable precommitted wall, CPU, p99, coverage,
  and structural gates all passed.
- `INCONCLUSIVE` means coverage or sample evidence was incomplete, a required
  comparative win was absent, or the confidence bounds were insufficient.
- `REJECT` means a structural/correctness gate failed, a wall regression was
  statistically supported, or the matrix CPU/p99 regression limit was
  exceeded.

The hard gate compares only `portable_verdict`; a Linux/io_uring-specific
`SPECIALIZED` result cannot override a portable `REJECT` or `INCONCLUSIVE`.

Evidence is create-once: collection writes a sealed bundle, while every audit
is read-only and recomputes or verifies without rewriting it. Promotion
requires the exact stored `portable_verdict` to be `SPECIALIZED`. The current
native performance verdict is `REJECT`, so this plan does not authorize a
version change, tag, package, publication, or release.

The sealed run at source revision
`ca122928ac943d5731b03d66c75c059c2f8bc865` produced both a portable `REJECT`
and a Linux/io_uring `REJECT`. Every measured fixed-resource cell had a
statistically supported wall regression, while all 36 non-fixed `link_skip`
cells were unavailable because `READ_EXACT` requires an
`exact_result_semantic_barrier`. The immutable interpretation and successor
research direction are recorded in
[LEIR Native Pipeline Decision](../research/reports/2026-08-01-leir-native-pipeline-decision.md).

If the kernel, liburing, memlock/resource limits, registered files/buffers, or
CQE-skip support cannot run `fixed_link_skip`, the benchmark exits with the
native skip code and the matrix records unavailable fixed cells. It never
substitutes non-fixed measurements. Such missing coverage cannot produce
`SPECIALIZED`.

These results still come from one Linux backend mechanism. Even a
`SPECIALIZED` portable promotion verdict does not by itself establish a
portable LLAM speedup or a general language-runtime advantage; cross-platform
evidence and the rest of release CI remain separate requirements.

## LEIR Portable And Linux Compiled-Executor Evidence

This screen evaluates two independent axes for the same generated
`CONNECT -> WRITE` module:

- the portable axis compares the phase-0 interpreter oracle with portable
  compiled B;
- the Linux axis compares portable compiled B with the linked io_uring B
  implementation under each strict ring profile.

Build and run the frozen matrix on Linux:

```sh
make LLAM_BUILD_RESEARCH=1 -j4 bench_leir_aot_connect
python3 -m unittest scripts/test_bench_leir_aot_connect.py -v
python3 scripts/bench_leir_aot_connect.py \
  --binary ./bench_leir_aot_connect \
  --output-dir object/leir-portable-compiled-executor \
  --profiles submit_all,coop_taskrun,defer_taskrun
python3 scripts/bench_leir_aot_connect.py \
  --audit-only \
  --output-dir object/leir-portable-compiled-executor
```

Collection and audit return zero for a structurally valid bundle even when a
performance result is `INCONCLUSIVE` or `FAIL`. Add `--enforce` only when a
caller intentionally requires the overall portable verdict to be exactly
`PASS`; every other verdict then exits nonzero.

The default matrix covers TCP and Unix stream sockets, concurrency 1 and 16,
payloads 64 and 4096 bytes, 256 activations, and five paired fresh-process
samples per candidate. Candidate order follows a deterministic ABBA/BAAB
schedule. Each pair carries the same block identity and seed.

For a capability smoke test, use a separate create-once output directory:

```sh
python3 scripts/bench_leir_aot_connect.py \
  --binary ./bench_leir_aot_connect \
  --output-dir object/leir-portable-compiled-smoke \
  --profiles submit_all \
  --transports unix \
  --concurrency 1 \
  --payloads 64 \
  --activations 8 \
  --samples 1
```

Every schema-3 row records common semantic receipts: result and peer
checksums, logical operations, dispatches, completion normalizations, site
lookups, parks, wakes, allocations, wall time, CPU time, p50, and p99. Only the
Linux candidate may add prepared SQEs, observed CQEs, suppressed successful
CQEs, queue publications, and submit syscalls. Portable rows containing Linux
fields fail parsing.

Successful compiled rows require zero interpreter dispatches and hot
allocations, one normalization and site lookup per activation, matching
park/wake ownership, and the same checksums as their paired baseline. A
successful Linux row additionally requires exactly two SQEs, one visible CQE,
one suppressed successful CQE, and one queue publication per activation.

The bundle contains `attempts.json`, `raw.csv`, `summary.csv`, `metadata.json`,
`verdict.json`, and `summary.md`. Metadata records the exact revision,
source-tree digest and cleanliness, binary digest, frozen matrix, classifier
policy, kernel, compiler, Python, and liburing versions. Audit is read-only: it
validates every expected worklist receipt and regenerates all projections in
memory before comparing them byte for byte.

Correctness and performance verdicts stay separate. The portable result is
authoritative for the general direction; a Linux specialization cannot upgrade
a failed or inconclusive portable result. Linux profiles are still reported
independently, and `UNAVAILABLE` never becomes a synthetic sample.

With the current setup-task/dedicated-I/O-worker topology, Linux
`defer_taskrun` is unavailable because `SINGLE_ISSUER` ownership cannot be
silently transferred. Measuring it requires a separate worker-owned ring
construction experiment.

The clean run at revision `4ebf252549566415ac6911862fe3b9bd074c7c84`
produced portable correctness `PASS`, Linux `submit_all` and `coop_taskrun`
correctness `PASS`, and favorable median ratios. Performance remained
`INCONCLUSIVE` because at least one metric in the required cells exceeded the
predeclared 20% ratio-spread limit. `defer_taskrun` was `UNAVAILABLE`.

See [LEIR Portable Compiled Executor Results](../research/reports/2026-08-02-leir-portable-compiled-executor-results.md)
and [Decision](../research/reports/2026-08-02-leir-portable-compiled-executor-decision.md).
No result authorizes a version change, tag, package, publication, or 3.0.0
release.

## Guardrails

`scripts/bench_guard.py` is a catastrophic-regression gate. Keep it
conservative. Public performance tables should come from scheduled benchmark
artifacts, not local one-off runs.
