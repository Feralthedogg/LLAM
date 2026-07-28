# Runtime Resource Governance Design

**Date:** 2026-07-29

**Status:** Approved as part of the runtime-backend productization program.

## Goal

An embedder must be able to predict and bound LLAM's scheduler capacity,
runtime-owned threads, CPU placement, and preallocated objects before a
runtime publishes resources.

## Compatibility boundary

Public option and diagnostic structures remain size-prefixed. The legacy
`llam_runtime_init(opts *)` convenience entry point consumes only the frozen
2.2 option prefix; new fields are honored through the size-aware entry point.
Partially present fields are ignored. Zeroed new fields preserve legacy
defaults, while `llam_runtime_opts_init()` fills explicit current defaults.

The additive options express:

- worker minimum, initial count, and maximum;
- blocking-worker minimum and maximum;
- affinity policy and an ordered CPU-ID set copied during initialization;
- runtime-total task, stack, and timer prewarm targets.

## Pure resource plan

Before allocating shards, mappings, or threads, a pure resolver accepts the
caller prefix, discovered allowed CPUs, platform capabilities, deterministic
mode, and SQPOLL reservation. It returns a complete immutable plan.

Resolved invariants are:

```text
1 <= worker_min <= worker_count <= worker_max <= selected_cpu_count
0 <= blocking_min <= blocking_max <= hard_limit
all selected CPU IDs are unique and process-allowed
all aggregate counts and byte estimates use checked arithmetic
```

An explicit `worker_count` with zero bounds is fixed
`min == count == max`. Dynamic scaling operates only inside resolved bounds;
it changes online scheduler loops, not the meaning of shard capacity.
Deterministic mode accepts only `1/1/1`. Conflicts fail with `EINVAL`.

Affinity policies are:

- `NONE`: never modify affinity;
- `PREFER`: continue and count an apply/restore failure;
- `REQUIRE`: reject unsupported platforms and propagate binding failure.

Caller CPU order defines shard-to-CPU order. Shard 0 captures the host
thread's affinity before a run and restores it on every success and error exit.
Affinity decisions are per runtime, not process-global cached environment
state.

## Thread lifecycle

Scheduler auxiliary threads are created according to the plan. Startup failure
stops and joins confirmed starts before the host scheduler enters.

Blocking workers start at `blocking_min`, grow under the blocking-pool lock as
queued pressure exceeds available workers, and never exceed `blocking_max`.
A zero minimum is valid. First submission creates a worker before it may park;
creation failure returns the job to a defined failure path and cannot strand
it. All confirmed workers are joined at shutdown.

Actual thread diagnostics are atomically updated at thread-main entry/exit and
distinguish:

- configured shard capacity and online target;
- scheduler loops including and excluding the host;
- blocking, I/O, controller, and opaque-helper threads;
- total runtime-owned native threads;
- native execution threads including the host loop;
- requested and achieved prewarm totals;
- selected CPUs and affinity failures.

Existing diagnostic fields retain their old meaning.

## Prewarm semantics and bounds

New public prewarm fields are exact runtime totals. Exact requests fail
initialization before publication if they cannot be satisfied. Deprecated
environment variables retain their historical semantics only through a
compatibility resolver, are reported as legacy inputs, and are best-effort.
New environment names include `_TOTAL`.

The plan applies hard aggregate limits and checked multiply/add before
allocation. The first policy uses:

- at most 256 scheduler shards/selected CPUs;
- at most 256 blocking workers;
- at most 4096 warmed stacks;
- documented total task and timer ceilings derived from byte budgets.

Resolved and achieved counts and estimated metadata/mapping bytes appear in
diagnostics.

## Test seams and verification

Internal-only seams provide injected CPU discovery, a pure planner, typed
thread creation failure, affinity apply/restore failure, and prewarm allocation
failure.

Deterministic tests cover:

- legacy and partially present ABI prefixes;
- 1, 8, and 64 CPU plans, sparse CPU order, SQPOLL reservation, duplicates,
  out-of-domain CPUs, hard caps, and arithmetic overflow;
- fixed and dynamic worker ranges and deterministic conflicts;
- lazy blocking growth from zero through the maximum and creation rollback;
- scheduler failure-on-Nth cleanup and exact enter/exit counters;
- preferred and required affinity failures and shard-0 restoration;
- task/stack/timer runtime-total distribution and exact failure;
- resource-plan diagnostics before, during, and after run/shutdown.

The resource plan becomes a prerequisite for stack-budget and external-driver
work.
