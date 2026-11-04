# Limits and memory

Size file descriptors and task memory before a high-fanout service starts. Both
can run out before the runtime itself is under stress.

## File descriptor limits

High fanout poll, accept, and inflight-I/O tests can exhaust the process
`RLIMIT_NOFILE` before they stress the runtime. Production services should set a
high soft descriptor limit before startup:

```sh
ulimit -S -n 65535
```

The bundled `stress` binary automatically clamps
`LLAM_STRESS_DYNAMIC_LIVE_POLL_WAITERS` to the current fd budget, reserving a
fixed baseline for listeners, pipes, shared libraries, and runtime internals.
If you need maximum-load validation, raise the limit explicitly and confirm the
stress log no longer prints `fd budget clamped`.

## Memory and stack pressure

Stackful tasks are fast only when task metadata and stacks are reused. For
high-fanout workloads, prewarm caches during service startup:

```sh
LLAM_TASK_CACHE_PREWARM=4096 \
LLAM_STACK_CACHE_PREWARM=4096 \
./service
```

Both values are capped at 4096, and a larger value is clamped without a warning.
Task prewarm applies to each processor, while stack prewarm is the total for the
runtime.

Use larger stack classes only for known deep C call chains. Enable
`LLAM_STACK_SAMPLING=1` during staging to catch near-overflow behavior, then
disable it for release benchmarking unless diagnostics are required.
