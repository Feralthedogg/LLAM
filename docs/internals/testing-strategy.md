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

## Release Rule

A release artifact should be published only after the target's platform-local
gate passes. Do not infer target readiness from another backend.
