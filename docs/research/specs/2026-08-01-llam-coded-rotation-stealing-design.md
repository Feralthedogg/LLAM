# LCRS — LLAM Coded-Rotation Stealing

Status: accepted for isolated Phase 0 implementation  
Plane: existing stackful fiber scheduler  
Primary goal: reduce victim-discovery scans and steal thundering-herd behavior without weakening current correctness checks

## 1. Problem statement

The current scheduler searches all same-locality shards for the deepest normal queue and then searches all remote shards if the local steal fails. This is simple and finds the global best candidate in each locality tier, but the discovery cost grows with shard count and many idle thieves can select the same deepest victim from similar snapshots.

LCRS changes only victim discovery:

```text
current:
  full local scan -> deepest local
  full remote scan -> deepest remote

LCRS:
  bounded local coded candidates
  -> bounded remote coded candidates
  -> exact existing full scan when required
```

The existing `llam_steal_from_victim()` remains the authority for eligibility, queue operations, pinned tasks, migration accounting, and race handling.

## 2. Considered approaches

### A. Keep global deepest scan

Advantages:

- best observed queue depth;
- minimal policy complexity;
- strong drain-tail behavior.

Disadvantages:

- O(P) queue-depth reads for each idle thief;
- synchronized choice under skew;
- cache traffic increases with worker count.

### B. Random-k sampling

Advantages:

- very small implementation;
- O(k) probes;
- naturally disperses thieves.

Disadvantages:

- no deterministic bound on instantaneous target fan-in;
- replay depends on PRNG state;
- duplicate samples and uneven topology coverage need handling.

### C. Coded permutation palettes — selected

Advantages:

- each palette column is a derangement/permutation;
- target fan-in is bounded by the number of active columns;
- deterministic replay;
- every shard is covered over a finite epoch cycle;
- arbitrary NUMA topology can be precomputed.

Disadvantages:

- topology builder and table memory;
- may miss the deepest queue;
- requires exact fallback gates.

Selection: C, with B as the mandatory simplicity baseline and A retained as fallback.

## 3. Naming

The production-facing expansion is **LLAM Coded-Rotation Stealing**. Coding- and Ramsey-style separation motivates palette construction, but the implementation does not claim to instantiate or depend on the newly proved bounds.

## 4. Topology model

### 4.1 Immutable topology

At runtime creation, build an immutable table indexed by:

```text
(thief_shard, palette_epoch)
  -> local candidate shard IDs
  -> remote candidate shard IDs
```

Recommended defaults:

```text
palette_epochs       = 16
local_width          = min(4, local_domain_size - 1)
remote_width         = min(4, remote_shard_count)
small_runtime_cutoff = 16 active shards
confirm_rows         = 2
fullscan_miss_limit  = 2
```

For small runtimes, retain full scan because its exact choice is cheap.

### 4.2 Local candidate construction

For a locality domain with `n` ordered shard ranks, each nonzero cyclic offset `d` defines a derangement:

```text
victim(rank, d) = domain[(rank + d) mod n]
```

For a fixed offset, each victim has exactly one preimage. With `k` distinct offsets in one epoch, each victim appears in at most `k` candidate lists.

Offsets are enumerated without repetition until all `n - 1` values are used:

```text
offset(e, j) =
  1 + (((e * k + j) * step + phase) mod (n - 1))
```

Constraints:

- `step` is coprime to `n - 1`;
- `phase` is a deterministic domain hash;
- offsets in one row are distinct;
- rows are verified at initialization.

A deterministic greedy fallback builder is used if the algebraic row cannot satisfy the configured width.

### 4.3 Remote candidate construction

Irregular NUMA domains prevent a simple bijection between all source and target ranks. Build remote rows offline with balanced assignment:

1. Select target locality nodes through nonzero cyclic node offsets.
2. Within each selected target node, assign a target shard with the smallest current epoch indegree.
3. Break ties using a deterministic hash of `(runtime_id, source_shard, epoch, slot)`.
4. Reject self and same-node candidates.
5. Verify maximum remote indegree against a configured bound.

Initialization fails closed to full scan if the table cannot be verified.

### 4.4 Epoch coordination

A runtime-owned relaxed atomic `lcrs_epoch` is advanced by the controller at a coarse cadence. Thieves normally use the current row. After the first miss, they may use the next row. Thus a transition exposes at most two rows concurrently, bounding instantaneous candidate fan-in near `2k`.

A thief never maintains an unbounded private walk that could destroy the global fan-in property.

## 5. Runtime state

### 5.1 Runtime-owned state

```c
typedef enum llam_steal_policy {
    LLAM_STEAL_POLICY_FULLSCAN = 0,
    LLAM_STEAL_POLICY_SHADOW = 1,
    LLAM_STEAL_POLICY_CODED = 2,
} llam_steal_policy_t;

typedef struct llam_lcrs_row {
    uint16_t local_count;
    uint16_t remote_count;
    uint32_t candidate_ids[LLAM_LCRS_MAX_CANDIDATES];
} llam_lcrs_row_t;

typedef struct llam_lcrs_topology {
    uint32_t version;
    uint32_t epoch_count;
    uint32_t shard_count;
    uint32_t max_local_indegree;
    uint32_t max_remote_indegree;
    llam_lcrs_row_t *rows;
} llam_lcrs_topology_t;
```

The row address is:

```text
rows[thief_id * epoch_count + epoch]
```

### 5.2 Shard-owned state

```c
unsigned lcrs_miss_streak;
unsigned lcrs_shadow_sample_countdown;
uint64_t lcrs_last_success_epoch;
```

These fields are owned by the shard scheduler thread and need not be atomic.

### 5.3 Metrics

Add relaxed per-shard counters:

```text
steal_attempts
steal_local_probes
steal_remote_probes
steal_candidate_tries
steal_candidate_hits
steal_selected_race_misses
steal_offline_skips
steal_pause_skips
steal_fullscan_fallbacks
steal_shadow_agreements
steal_shadow_depth_ratio_sum
steal_shadow_samples
```

Optional research-only counters may sample selected-victim concurrency. They are not included in watchdog progress.

## 6. Selection algorithm

```text
try_steal_coded(thief):

  reject if deterministic, paused, offline, or fewer than two online shards

  if small runtime:
      return fullscan()

  row = topology[thief][global_epoch]

  local = snapshot local candidates
  sort up to k candidates by depth descending
  for candidate in local:
      if existing steal_from_victim(candidate) moves work:
          reset miss streak
          return local task

  remote = snapshot remote candidates
  sort by depth descending
  for candidate in remote:
      if existing steal_from_victim(candidate) moves work:
          reset miss streak
          return local task

  increment miss streak

  if pressure is visible
     or miss streak >= fullscan_miss_limit
     or more than half the row was invalid/offline:
        reset miss streak
        return existing fullscan()

  return no work
```

Sorting uses a fixed stack array and insertion sort. No hot allocation occurs.

The selector may try more than one candidate because a depth snapshot can become stale or the candidate can fail the existing busy/eligibility checks.

## 7. Shadow mode

Shadow mode computes both policies:

```text
coded victim and depth
current full-scan victim and depth
```

It always steals from the current full-scan victim. It records:

- agreement rate;
- coded/fullscan depth ratio;
- predicted local versus remote choice;
- estimated candidate indegree;
- whether coded selection would have required fallback.

Sampling must be configurable so shadow mode does not double every steal scan in ordinary profiling.

## 8. Dynamic workers and migration

- Candidate IDs are physical active-shard IDs and remain valid while workers go online or offline.
- Offline, merge-paused, and non-accepting shards are skipped.
- The topology is not rebuilt during Phase 1.
- Excessive invalid skips immediately trigger full scan.
- A future topology rebuild may occur only under controller ownership and a versioned pointer swap; it is out of scope until the static table passes.

LCRS never participates in the merge operation itself. Existing global steal pause remains authoritative.

## 9. Failure handling

| Failure | Behavior |
|---|---|
| topology allocation failure | disable LCRS and use full scan |
| verification failure | disable LCRS and emit diagnostic reason |
| stale/offline candidate | skip and count |
| candidate race loss | try next candidate |
| repeated miss | exact full-scan fallback |
| pressure or overflow | immediate full-scan fallback |
| deterministic runtime | retain existing no-steal behavior |
| unsupported topology | full scan |

No LCRS failure may drop, duplicate, pin-break, or rehome a task by itself.

## 10. File boundaries

### Phase 0

```text
experiments/lcrs/
  lcrs_model.h
  lcrs_topology.c
  lcrs_simulator.c
  test_lcrs_topology.c
  bench_lcrs_model.c

scripts/
  bench_lcrs_model.py
  test_bench_lcrs_model.py
```

### Runtime prototype

```text
src/engine/scheduler/steal_policy.c
src/engine/scheduler/scheduler_engine.c
src/internal/runtime_proto_sched.h
src/internal/runtime_types.h
src/core/lifecycle/init.c
src/core/lifecycle/shutdown.c
src/core/debug/debug.c
src/core/debug/debug_stats_json.c
tests/test_lcrs_selector.c
tests/test_runtime_lcrs.c
```

Extract the current scan into `llam_try_steal_fullscan()` before adding coded mode so the fallback remains byte-for-byte reviewable.

## 11. Correctness tests

### Topology properties

For every shard count 1 through at least 512 and diverse node partitions:

- no local self-candidate;
- no duplicate candidate within a row;
- local candidates stay in the same locality domain;
- remote candidates leave the locality domain;
- every eligible peer is covered before the palette cycle repeats;
- per-row indegree stays within the verified bound;
- construction is deterministic;
- malformed or impossible settings fail closed.

### Selector behavior

- skips offline and merge-paused shards;
- preserves deterministic mode;
- pressure and miss streak invoke full scan;
- candidate race falls through to the next candidate;
- current victim function remains unchanged;
- no allocation after initialization.

### Runtime integration

- one-hot spawn fanout;
- rotating hotspot;
- balanced steady state;
- drain tail;
- pinned-task mixture;
- lock-free queue on and off;
- dynamic worker scale up/down;
- multi-runtime isolation;
- shutdown during steal;
- large shard count.

## 12. Benchmarks and gates

Mandatory baselines:

```text
global_deepest
deterministic_random_k
coded_rotation_k
coded_rotation_k_plus_fullscan
```

Acceptance at 32 or more shards:

- candidate depth probes reduced by at least 60%;
- p99 selected-victim fan-in reduced by at least 50%;
- balanced throughput at least 98% of global-deepest;
- no core benchmark regresses more than 5%;
- one-hot or rotating-hotspot p99 completion improves by at least 10%, or CPU/probe cost improves materially without tail regression;
- remote migration does not increase by more than 5%;
- steady-state full-scan fallback below 15%;
- every correctness, sanitizer, fuzz, and soak gate passes.

Complexity gate:

> If coded rotation does not beat deterministic random-k by at least 3% in one load-balancing or tail metric while matching it elsewhere, remove the coded constructor and ship the simpler random-k policy.

## 13. Interaction with other tracks

- LRPA supplies steal/offline, steal/pin, and hotspot race gadgets.
- LSWG may report tasks stalled during an experimental policy failure, but LCRS counters are never treated as progress.
- LCCF Executor work is not LCRS-stealable in its first implementation.
