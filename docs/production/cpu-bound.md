# CPU-bound tasks

A long C loop can delay cancellation, watchdog progress, and other tasks. Use
managed C, or add safepoints to compatibility tasks.

## Use managed C

Compiler-qualified tasks use native asynchronous preemption by default through
`llam_runtime_create()` or `llam_runtime_init()`, after image admission and
native-thread probes. `llam_runtime_create_managed()` exposes advanced policy
and budget settings. Select the requested managed policy through `llam_preempt_opts_t` and
query the effective policy with `llam_runtime_preempt_info_ex()`; see the
[managed C guide](../guides/managed-c.md) and its native profile limits.

## Add safepoints to compatibility tasks

Explicit `*_compat` constructors retain boundary/safepoint preemption. Their
watchdog/runtime policy requests a switch, but the task must still hit a
boundary such as `llam_task_safepoint()`, `LLAM_PREEMPT_POLL`,
`LLAM_PREEMPT_POLL_EVERY`, `llam_yield()`, a wait, or an I/O call. Long C loops
that never reach one of those boundaries can delay cancellation, watchdog
progress, allocator quiescence, and runnable task handoff.

Use `LLAM_PREEMPT_POLL_EVERY` inside CPU-heavy loops when an immediate yield
would be too expensive, and use `llam_yield()` when fairness matters more than
raw loop throughput.

```c
size_t poll_counter = 0;

for (uint64_t i = 0; i < work_items; ++i) {
    process_item(i);
    LLAM_PREEMPT_POLL_EVERY(poll_counter++, 1024U);
}
```

## Tune safepoints

Compatibility/safepoint tuning knobs:

- `LLAM_PREEMPT_MODE=off|cooperative|auto|strict` selects the preemption
  policy. `auto` is the normal diagnostic-safe mode; `strict` is for finding
  loops that do not poll often enough. `off` and `cooperative` apply only to
  compatibility runtimes; the ordinary constructors reject them with `EINVAL`.
- `LLAM_PREEMPT_POLL_PERIOD` overrides the task-local safepoint flag-poll
  period. Use this only when measuring CPU-loop latency vs overhead.
- `LLAM_PREEMPT_QUANTUM_NS` overrides the runtime slice budget. Keep it at `0`
  unless a benchmark or incident investigation needs a fixed quantum.
- `LLAM_SAFEPOINT_CLOCK_PERIOD` controls cheap-safepoint clock sampling and can
  reduce timestamp overhead in very hot loops.
