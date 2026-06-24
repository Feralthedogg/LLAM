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

## Prewarm High-Fanout Services

```sh
LLAM_TASK_CACHE_PREWARM=65536 \
LLAM_STACK_CACHE_PREWARM=8192 \
./service
```

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
