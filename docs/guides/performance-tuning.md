# Performance Tuning

Performance work should be platform-local and benchmark-gated. Tune only after
you can reproduce a workload with stable numbers.

## Start With Profiles

The default profile is balanced. For release-oriented measurements, prefer:

```c
llam_runtime_opts_t opts;
llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE);
opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
```

Environment override:

```sh
LLAM_RUNTIME_PROFILE=release-fast ./service
```

## Bound Native Resources First

For libraries, plugin hosts, and VMs, choose a per-runtime budget before tuning
queues or spinning:

```c
uint32_t cpus[] = {3U, 1U}; /* Example process-allowed IDs. */
llam_runtime_opts_t opts;

llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE);
opts.worker_count = 2U; /* A lone count resolves to fixed 2/2/2. */
opts.blocking_min = 0U;
opts.blocking_max = 4U;
opts.cpu_count = 2U;
opts.cpu_ids = cpus;
opts.affinity_policy = LLAM_RUNTIME_AFFINITY_PREFER;
```

`worker_max` is scheduler capacity, not total native-thread demand. Compare it
with `runtime_owned_threads`, which also includes the live blocking, I/O,
controller, and opaque-helper roles. During `llam_runtime_run_handle()`,
`native_execution_threads` additionally includes the host thread driving shard
0. With multiple runtimes, add these values across instances before evaluating
oversubscription.

Use fixed workers for a stable benchmark baseline. Test a dynamic
`worker_min/count/max` range only after the fixed baseline is correct, and keep
the range's migration/rehome costs separate from ordinary work-stealing
results. Exact CPU binding is currently Linux-only: `PREFER` is portable
best-effort, while `REQUIRE` intentionally rejects unsupported hosts.

## Prewarm High-Fanout Services

```sh
LLAM_TASK_CACHE_PREWARM_TOTAL=65536 \
LLAM_STACK_CACHE_PREWARM_TOTAL=2048 \
LLAM_TIMER_HEAP_PREWARM_TOTAL=65536 \
./service
```

These environment targets are runtime-wide and best-effort. For an embedding
contract that must either allocate the full capacity or fail cleanly, set
`task_prewarm_total`, `stack_prewarm_total`, and `timer_prewarm_total` through
the size-aware runtime options. Compare requested and achieved totals in
`llam_runtime_stats_t`; source fields distinguish exact public options, new
`_TOTAL` inputs, deprecated compatibility inputs, and profile defaults.
Also compare `estimated_metadata_bytes` and
`estimated_stack_mapping_bytes` before raising totals. The latter is a virtual
mapping estimate, so measure RSS separately after touching stacks.

## Bound Retained Stack Memory

Treat stack prewarm and stack caching as one budget. An exact prewarm must fit
below the cache high watermark; otherwise runtime creation fails instead of
silently trimming the requested warm set.

```c
opts.stack_prewarm_total = 512U;
opts.stack_cache_budget_bytes = 128ULL * 1024ULL * 1024ULL;
opts.stack_cache_high_watermark_bytes = 96ULL * 1024ULL * 1024ULL;
opts.stack_cache_low_watermark_bytes = 64ULL * 1024ULL * 1024ULL;
```

For throughput baselines, compare cache hits indirectly through allocation
latency while watching `stack_cache_cached_bytes` and
`stack_cache_budget_rejections`. For memory-constrained services, benchmark
`DISCARD_ON_RETURN` separately: it reduces committed pages but adds VM work
when a stack is returned and reused. Use manual trim between benchmark phases
instead of mixing a pressure event into the measured interval.

Enable stack sampling in staging when validating stack class choices:

```sh
LLAM_STACK_SAMPLING=1 ./service
```

## CPU-Bound Work

Add safepoints to long loops:

```c
size_t poll_counter = 0;

for (uint64_t i = 0; i < count; ++i) {
    work(i);
    LLAM_PREEMPT_POLL_EVERY(poll_counter++, 1024U);
}
```

Use `LLAM_PREEMPT_MODE=strict` to find loops that do not poll often enough.

## Experimental Flags

Treat these as release-gated:

- dynamic workers
- worker rings
- worker-ring multishot watches
- huge allocation
- SQPOLL
- lock-free normal queue

Every change needs before/after data and stress logs on the target kernel.

## Benchmark Discipline

Use isolated benchmark case execution for release-quality comparisons:

```sh
python3 scripts/bench_runtime_compare.py \
  --runtime all \
  --cases spawn_join,select_recv_ready,poll_wake \
  --isolate-cases
```

Keep `scripts/bench_guard.py` as a catastrophic-regression gate, not a
marketing benchmark.
