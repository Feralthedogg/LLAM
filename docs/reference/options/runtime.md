# Runtime and spawn options

Use this page when you need to know what can be configured in code. For runtime
tuning, prefer C option structs in libraries and environment variables in
deployments. Environment variables are listed in
[Environment variables](../environment.md), and command-line tools in
[CLI tools](../cli/index.md).

## Runtime options

Initialize runtime options through the library before setting fields:

```c
llam_runtime_opts_t opts;
llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE);

opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
opts.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS;
```

| Field | Meaning |
| --- | --- |
| `deterministic` | Prefer repeatable scheduling decisions. |
| `forced_yield_every` | Force cooperative yield after this many scheduler ticks; `0` disables it. |
| `experimental_flags` | Bitwise OR of `LLAM_RUNTIME_EXPERIMENTAL_F_*`. |
| `idle_spin_ns` | Bounded idle spin duration before kernel sleep. |
| `idle_spin_max_iters` | Maximum idle-spin iterations. |
| `sqpoll_cpu` | Requested SQPOLL CPU, or `-1` for automatic selection. |
| `profile` | One of `LLAM_RUNTIME_PROFILE_*`. |
| `preempt_mode` | One of `LLAM_PREEMPT_*`. Defaults to `LLAM_PREEMPT_AUTO`. |
| `preempt_poll_period` | Safepoint flag-poll period; `0` selects a profile default. |
| `preempt_quantum_ns` | Global preemption slice override; `0` uses task-class budgets. |
| `worker_count` | Portable/legacy processor count; `0` chooses available CPUs/profile. Must fit CPUs after SQPOLL reservation; deterministic permits only one. |
| `blocking_worker_count` | Blocking pool threads, 1..1024; `0` retains platform defaults. |
| `max_tasks` | Maximum retained task objects, including completed unjoined tasks; `0` unlimited. Admission failure is `EAGAIN`. |
| `max_blocking_jobs` | Maximum queued plus running callbacks; `0` unlimited. Rejection invokes no callback. |

Runtime profiles:

| Value | Use |
| --- | --- |
| `LLAM_RUNTIME_PROFILE_BALANCED` | General-purpose default. |
| `LLAM_RUNTIME_PROFILE_RELEASE_FAST` | Lower overhead release runs. |
| `LLAM_RUNTIME_PROFILE_DEBUG_SAFE` | Conservative diagnostics-focused runs. |
| `LLAM_RUNTIME_PROFILE_IO_LATENCY` | Favor I/O wakeup latency. |

Preemption modes, where the ordinary constructors are `llam_runtime_create()`,
`llam_runtime_init()`, and `llam_runtime_init_ex()`:

| Value | Use |
| --- | --- |
| `LLAM_PREEMPT_OFF` | Disable automatic preemption requests. The ordinary constructors reject it with `EINVAL`. |
| `LLAM_PREEMPT_COOPERATIVE` | Honor explicit safepoints only; this mode never activates async capture. The ordinary constructors reject it with `EINVAL`. |
| `LLAM_PREEMPT_AUTO` | Request preemption when runtime pressure exists. The ordinary constructors require asynchronous admission. Explicit `*_compat` constructors use cooperative requests; the advanced managed constructor accepts an explicit admission policy. |
| `LLAM_PREEMPT_STRICT` | Diagnostic mode for finding loops without enough safepoints; unsupported code still falls back or defers safely. |

The experimental Linux x86-64 context backend compiles by default on that target.
On a compatibility runtime, selecting `AUTO` or `STRICT` does not enable it. It
requires explicit enablement and safe-range registration through a private path with
no public symbol; unregistered code stays on the cooperative path. Effective policy and the fallback reason are shown in the internal runtime dump without
changing the public stats schema.

On a compatibility runtime, the experimental fixed-primary Win64/MinGW backend requires
the explicit host-thread call `llam_runtime_enable_experimental_windows_async_preempt_ex()`
before its first worker run and before any task is live.
`llam_windows_async_preempt_opts_t` contains a
nonempty copied array of `llam_windows_async_preempt_range_t` half-open PC
intervals, a count, and the mandatory
`LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST` flag. Unknown flags and
overlapping ranges fail. The caller—not LLAM—certifies that every possible
interrupted PC in those PE-image intervals is safe for async capture/restore;
the caller must keep code bytes unchanged until runtime shutdown/destroy. The
backend is one-way and available only with the experimental build option.
Uncertain committed remote context changes terminate the whole process.
`AUTO`/`STRICT` still determine the request policy; code outside certified
ranges remains cooperative.

Experimental flags:

| Flag | Meaning |
| --- | --- |
| `LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS` | Give workers their own I/O rings/queues where supported. |
| `LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS_MULTISHOT` | Allow multishot watches in worker-ring mode. |
| `LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS` | Soft-park idle workers and reactivate on pressure. |
| `LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ` | Use the lock-free normal run queue. Direct yields and parked local handoffs on a compatibility runtime need it. |
| `LLAM_RUNTIME_EXPERIMENTAL_F_HUGE_ALLOC` | Prefer hugepage-friendly allocator backing. |
| `LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL` | Enable Linux io_uring SQPOLL experiment. |

## Spawn options

```c
llam_spawn_opts_t opts;
llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);

opts.task_class = LLAM_TASK_CLASS_LATENCY;
opts.stack_class = LLAM_STACK_CLASS_LARGE;
```

| Field | Meaning |
| --- | --- |
| `task_class` | Scheduler class; one of `LLAM_TASK_CLASS_*`. |
| `stack_class` | Stack class; one of `LLAM_STACK_CLASS_*`. |
| `flags` | Bitwise OR of `LLAM_SPAWN_F_*`. |
| `deadline_ns` | Optional absolute deadline in `llam_now_ns()` units; `0` disables it. |
| `cancel_token` | Optional cancellation token observed by waits and I/O. |
| `local_executor` | ABI 2.6+: optional shared [local executor](../api/executors.md#shared-local-executors). Root and local children must all name the same handle to share one LLAM-owned native thread. `NULL` keeps ordinary scheduling. |

The executor pointer is an optional complete tail field: old 32-byte options
retain their behavior, and the current 64-bit layout is 40 bytes. Initialize with
`llam_spawn_opts_init` from host code, or use a zero-initialized record in qualified
managed code and set the required fields. Other spawn fields keep their usual
defaults. Appended fields require the explicit-size `_ex` APIs; unsized legacy
spawn wrappers read only the original 32-byte prefix. The `NATIVE_AFFINE` flag
alone reserves independent per-task threads;
it does not create a shared local domain.

Task classes:

| Value | Meaning |
| --- | --- |
| `LLAM_TASK_CLASS_LATENCY` | Latency-sensitive task. |
| `LLAM_TASK_CLASS_DEFAULT` | General-purpose task. |
| `LLAM_TASK_CLASS_BATCH` | Throughput-oriented task. |

Stack classes:

| Value | Meaning |
| --- | --- |
| `LLAM_STACK_CLASS_DEFAULT` | Default stack size. |
| `LLAM_STACK_CLASS_LARGE` | Larger stack for deeper C call chains. |
| `LLAM_STACK_CLASS_HUGE` | Largest built-in stack class. |

Spawn flags:

| Flag | Meaning |
| --- | --- |
| `LLAM_SPAWN_F_PINNED` | Prefer keeping the task on its home worker. |
| `LLAM_SPAWN_F_NO_PREEMPT` | Restrict cooperative preemption checks. A runtime that requires asynchronous preemption rejects it with `EINVAL`. |
| `LLAM_SPAWN_F_SYS_TASK` | Mark runtime-owned helper work. |
| `LLAM_SPAWN_F_LATENCY_CRITICAL` | Promote wakeup and dispatch priority. |
| `LLAM_SPAWN_F_NATIVE_AFFINE` | Reserve a dedicated native M for a qualified managed entry. A compatibility runtime rejects it with `ENOTSUP`. See [Managed C](../../guides/managed-c.md). |

## Timer options

Use `llam_timer_create()` for the common case, or `llam_timer_create_ex()` for
explicit first deadline and ABI-sized options.

| Field | Meaning |
| --- | --- |
| `first_deadline_ns` | First absolute deadline in `llam_now_ns()` units; `0` means now. |
| `interval_ns` | Repeat interval in nanoseconds; must be non-zero. |
| `flags` | Reserved for future `LLAM_TIMER_F_*`; initialize to `0`. A nonzero value fails with `ENOTSUP`. |

## Signal options

Signal wait sets are Linux-only. Other platforms return `ENOTSUP`.

| Field | Meaning |
| --- | --- |
| `flags` | Reserved for future `LLAM_SIGNAL_F_*`; initialize to `0`. |

## I/O buffer options

```c
llam_io_buffer_opts_t opts;
llam_io_buffer_opts_init(&opts, LLAM_IO_BUFFER_OPTS_CURRENT_SIZE);

opts.capacity = 4096;
opts.alignment = 4096;
```

| Field | Meaning |
| --- | --- |
| `capacity` | Minimum usable buffer capacity in bytes. |
| `alignment` | Required power-of-two alignment, or `0` for default. |
| `flags` | Bitwise OR of `LLAM_IO_BUFFER_F_*`. |

I/O buffer flags:

| Flag | Meaning |
| --- | --- |
| `LLAM_IO_BUFFER_F_ZERO_FILL` | Zero-fill allocated buffer storage. |

## File stat

`llam_stat_path_ex()` writes portable file metadata into `llam_file_stat_t`.
Use `LLAM_FILE_STAT_CURRENT_SIZE` for the size handshake.

File types:

| Value | Meaning |
| --- | --- |
| `LLAM_FILE_TYPE_REGULAR` | Regular file. |
| `LLAM_FILE_TYPE_DIRECTORY` | Directory. |
| `LLAM_FILE_TYPE_SYMLINK` | Symbolic link or Windows reparse point. |
| `LLAM_FILE_TYPE_OTHER` | Other platform-specific type. |

## Managed runtime options

`<llam/managed.h>` adds caller-sized `llam_preempt_opts_t` without extending
`llam_runtime_opts_t`. Pass it to `llam_runtime_create_managed()`.

| Field | Meaning | `0` means |
| --- | --- | --- |
| `policy` | One of `LLAM_PREEMPT_POLICY_*`. | `ASYNC_AUTO`, so a zeroed struct is **not** the default policy. |
| `flags` | Reserved. A nonzero value fails with `EINVAL`. | Required value. |
| `capsule_budget_bytes` | Capsule storage quota in bytes. | 64 MiB. |
| `native_affine_capacity` | Dedicated native-affine M slots, at most 1024. | 4 slots. |
| `reserved0` | Reserved. A nonzero value fails with `EINVAL`. | Required value. |

`llam_preempt_opts_init()`, and a `NULL` options pointer with size `0`, select
`ASYNC_REQUIRED`, a 64 MiB budget and four slots. `llam_runtime_create()` and
`llam_runtime_init()` use those same defaults. A smaller nonzero size keeps the
default for each field it omits. A `NULL` pointer with a nonzero size, or a non-`NULL`
pointer with size `0`, fails with `EINVAL`.

The budget is a quota, not an eager 64 MiB
allocation or RSS limit. Prepared storage grows from the native-M-scaled initial
pool up to the quota; temporary depletion returns `EAGAIN`, quota exhaustion
returns `ENOMEM`, and rejected spawns never publish a task.

| Policy | Use |
| --- | --- |
| `LLAM_PREEMPT_POLICY_ASYNC_REQUIRED` | Rejects legacy code, incompatible whole-task `NO_PREEMPT` and unavailable native profiles before user entry, and rejects a `preempt_mode` of `OFF` or `COOPERATIVE` with `EINVAL`. |
| `LLAM_PREEMPT_POLICY_ASYNC_AUTO` | Accepts legacy code on one extra compatibility processor. |
| `LLAM_PREEMPT_POLICY_COOPERATIVE` | Explicitly disables native capture and keeps that processor. |

See [managed runtimes](../../guides/managed-c.md) for module pinning, affine refill and
callback lifetime.

### Module options

`llam_runtime_module_register_ex()` reads `llam_module_opts_t`. `opts_size` must
include both metadata fields, so at least 16 bytes.

| Field | Meaning |
| --- | --- |
| `metadata_address` | `(uint64_t)(uintptr_t)` of the image's LLPM2 section. Must not be `0`. |
| `metadata_bytes` | Size of that section. Must not be `0`. |
| `flags` | Reserved; must be `0` when present. |
| `reserved0` | Reserved; must be `0` when present. |

### Preemption info

`llam_runtime_preempt_info_ex()` writes the prefix of `llam_preempt_info_t` that the
caller's `out_size` and the loaded library share.

| Field | Meaning |
| --- | --- |
| `requested_policy` | The `LLAM_PREEMPT_POLICY_*` the runtime was created with. |
| `effective_policy` | Policy of the latest startup attempt. `LLAM_PREEMPT_EFFECTIVE_MIXED` means qualified asynchronous tasks and isolated legacy tasks together. |
| `reason` | One of `LLAM_PREEMPT_REASON_*`, below. |
| `flags` | `LLAM_PREEMPT_INFO_F_RUNNING` during an active run; `LLAM_PREEMPT_INFO_F_COMMITTED` after its committed barrier. |
| `capsule_budget_bytes` | Configured quota. |
| `capsule_charge_bytes` | Storage charged per task. `0` means no native allocation profile is prepared. |
| `conservative_task_charge_bytes` | Per-task charge used to compute the maximum. |
| `maximum_charged_tasks` | Tasks the budget admits. With a positive charge, `0` means not even one, not unlimited. |
| `registered_modules`, `registered_entries` | Catalog contents. |
| `retained_references` | Tasks holding a catalog reference, including in-flight admission and retained, not-yet-reclaimed tasks. |
| `excluded_tasks`, `legacy_tasks`, `native_affine_tasks` | Retained tasks by class. `excluded_tasks` counts `NO_PREEMPT`. |
| `native_affine_capacity`, `native_affine_available` | Configured and currently free affine slots. |
| `portable_processors`, `compatibility_processors` | Logical processors by role. |
| `effective_quantum_ns[3]` | Configured native request interval, indexed by `llam_task_class_t`. All zero when inactive or unqualified. These are not observed latencies. |
| `reserved_capsule_bytes` | Prepared storage including free slots. Not RSS. |
| `prepared_capsules`, `available_capsules` | Prepared pool size and its free slots. |

| Reason | Meaning |
| --- | --- |
| `LLAM_PREEMPT_REASON_NONE` | No limitation applies. |
| `LLAM_PREEMPT_REASON_LEGACY_RUNTIME` | A compatibility runtime, created without managed policy. |
| `LLAM_PREEMPT_REASON_REQUEST_DISABLED` | The runtime's `preempt_mode` disables preemption requests. |
| `LLAM_PREEMPT_REASON_COOPERATIVE_POLICY` | The `COOPERATIVE` policy was requested. |
| `LLAM_PREEMPT_REASON_ADAPTER_UNAVAILABLE` | This build or platform has no native asynchronous adapter. |
| `LLAM_PREEMPT_REASON_ISA_UNAVAILABLE` | A registered image needs an instruction-set feature this CPU lacks. |
| `LLAM_PREEMPT_REASON_PROBE_PENDING` | The native startup probe has not committed yet. |
| `LLAM_PREEMPT_REASON_STARTUP_FAILED` | The startup probe did not commit. |
| `LLAM_PREEMPT_REASON_LEGACY_MIXED` | Qualified and legacy tasks are both running. |
| `LLAM_PREEMPT_REASON_LEGACY_MIX_UNQUALIFIED` | Legacy tasks are present, and this runtime cannot isolate them beside asynchronous tasks. |

## Runtime resources

`llam_runtime_get_resources_ex()` and `llam_runtime_drain_until()` write
`llam_runtime_resources_t`, an advisory sample whose counts can change immediately.

| Field | Meaning |
| --- | --- |
| `worker_count` | Logical processors (P), portable plus compatibility. |
| `blocking_worker_count` | Blocking pool threads. |
| `admitted_tasks` | Tasks counted against `max_tasks`, released only after task object reclamation. |
| `live_tasks` | Tasks that have not finished. |
| `blocking_jobs` | Queued and running callbacks, which may still use caller memory. |
| `max_tasks`, `max_blocking_jobs` | Configured limits; `0` is unlimited. |
| `public_operations` | Public calls pinning the runtime, excluding this query or drain. |
| `bound_native_workers` | Native threads (M) currently bound. Unlike `worker_count`, this is OS threads rather than logical processors. |
| `flags` | Bitwise OR of the flags below, describing this sample only. |

| Flag | Meaning |
| --- | --- |
| `LLAM_RUNTIME_RESOURCE_F_RUNNING` | A driver is running the runtime. |
| `LLAM_RUNTIME_RESOURCE_F_DRAINING` | `llam_runtime_drain_until()` has permanently closed admission. |
| `LLAM_RUNTIME_RESOURCE_F_NEEDS_RUNNER` | No driver is running and live tasks remain, so a driver must run before they can finish. |
| `LLAM_RUNTIME_RESOURCE_F_QUIESCENT` | No driver, live task, blocking job, or other public operation remains. |

`QUIESCENT` does not mean that resources are released or that objects are gone:
completed tasks stay counted in `admitted_tasks` until joined or detached, and
caller-owned handles stay valid. After a successful drain, join the tasks, destroy
those objects, and then call `llam_runtime_destroy()`. See
[Bound resources and shutdown](../../guides/embedding.md#bound-resources-and-shutdown).

## Operation wait options

`llam_wait_opts_init` sets an inheriting deadline, no extra cancellation token and
zero flags.

| Field | Meaning |
| --- | --- |
| `deadline_ns` | Absolute deadline on the monotonic `llam_now_ns` clock; `0` adds no operation limit. A task's persistent spawn budget still applies. |
| `cancel_token` | Optional additional OR source; it never replaces task or group cancellation. |
| `flags` | Reserved; a nonzero value fails with `EINVAL`. |
| `reserved0` | Reserved; a nonzero value fails with `EINVAL`. |

Omitted input tails use defaults. See the
[embedding guide](../../guides/embedding.md#compose-cancellation-and-budgets)
for endpoint capabilities, partial-transfer and buffer-lifetime rules.

## Experimental options

The stable API exposes experiments through `experimental_flags`, but those flags
are still policy switches. Treat these as release-gated:

- Use `LLAM_RUNTIME_PROFILE_RELEASE_FAST` or defaults for production baselines.
- Enable `LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS` only after dynamic stress
  passes on the target kernel.
- Enable worker rings, multishot watches, huge allocation, or SQPOLL only with a
  before/after benchmark table and stress logs.
- Keep legacy `NM_*` environment aliases only for old automation; new scripts
  should use canonical `LLAM_*` names.
