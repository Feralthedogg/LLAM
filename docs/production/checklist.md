# Production checklist

Use this checklist before you treat a LLAM build as production ready. Each item
links to the page that explains the risk, the runtime control, and the test.

- [ ] Raise the file descriptor limit. See [Limits and memory](limits.md#file-descriptor-limits).
- [ ] Set stack and task budgets. See [Limits and memory](limits.md#memory-and-stack-pressure).
- [ ] Test timeouts and cancellation under dynamic workers. See [Timeouts and FD reuse](timeouts.md).
- [ ] Close descriptors through `llam_close`. See [File descriptor reuse](timeouts.md#file-descriptor-reuse).
- [ ] Build task code with managed C, or add safepoints to compatibility tasks. See [CPU-bound tasks](cpu-bound.md).
- [ ] Read the differences for your platform. See [Platform differences](platforms.md).
- [ ] Keep runtime handles inside their owner. See [Handle boundaries](handles.md).
- [ ] Capture stats and dumps in production. See [Diagnostics](../guides/diagnostics.md).
- [ ] Run the platform-local gates. See [Verification](verification.md).
- [ ] Keep benchmark artifacts for each release. See [Benchmarks](benchmarks.md).

## Release gate

Before tagging a release build, require:

- `make verify-linux CC=gcc` or `./scripts/docker_verify_linux.sh` on Linux.
- `CC=clang make verify-darwin` or the macOS GitHub Actions matrix.
- Verify BSD targets are rejected with `make test-platform-support`.
- Native Windows CMake/CTest through `scripts/verify_windows.ps1 -Native` and
  the Windows stress jobs (2022 in `Stress`, 2022 and 2025 in `Nightly Deep CI`),
  including forced Windows 10/11 policies and opt-in TCP `POLLIN` IOCP smoke.
- `Nightly Deep CI` on at least one recent `main` commit before a release
  candidate: POSIX standard composite, Windows 2022/2025 policy stress,
  deterministic runtime fuzz, conservative scheduler/channel/select/I/O
  benchmark guardrails, ASan/UBSan quick gate, and experimental allowed-failure
  Linux TSan diagnostics.
- `Weekly Soak` direct runtime soak plus hour-long server composite on Linux
  x86_64 and macOS arm64. The direct runtime soak repeats fuzz,
  multi-runtime ownership/isolation, runtime stress, shutdown, and owned-buffer
  tests with changing seeds. The server profile is the long-running
  stability/accounting gate: it keeps the high-rate best-effort flood load, but
  absolute delivery-MPS regression checks belong to `Stress`, `Nightly Deep CI`,
  and scheduled benchmark jobs. The soak still fails on zero traffic, missing
  stats, accounting gaps, closed-outbox drops, forced server stop,
  resource-limit violations, or unexpected edge client errors. Require this
  gate before claiming long-running runtime/server stability.
- `python3 scripts/stress_server_composite.py --quick --seed 1234` on at least one POSIX
  platform. Quick mode is a hosted-runner smoke gate and uses a lower absolute
  flood delivery threshold than standard mode; delivery ratio must still remain
  exact. Keep the printed seed from failed randomized edge runs so the same
  churn/reset sequence can be replayed.
- `python3 scripts/bench_runtime_compare.py --runtime all --isolate-cases` for
  the public LLAM/Go/Tokio comparison graph. Keep the default multi-sample
  median mode for release numbers.
- No unexplained `skipped=` phases. A skipped phase is acceptable only when the
  platform contract explicitly lacks that backend feature.
