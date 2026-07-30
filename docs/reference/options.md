# Options

Use this page when you need to know what can be configured. For runtime tuning,
prefer C option structs in libraries and environment variables in deployments.
Command-line tools and scripts are listed in [CLI And Tools](cli-tools.md).

## Install Script Options

POSIX:

```sh
curl -fsSL https://github.com/Feralthedogg/LLAM/releases/latest/download/install.sh |
  sh -s -- --prefix "$HOME/.local"
```

| Option | Meaning |
| --- | --- |
| `--prefix <dir>` | Install prefix. Defaults to `/usr/local` on POSIX. |
| `--version <version>` | Release version to install. Defaults to `LLAM_INSTALL_VERSION` or the script default. |
| `--target <target>` | Release target, such as `linux-x86_64` or `macos-aarch64`. |
| `--base-url <url>` | Override the release asset base URL. Useful for mirrors or testing. |
| `--dry-run` | Print planned file operations without installing. |
| `--force` | Allow overwrite of existing installed files. |
| `-h`, `--help` | Print usage. |

Windows PowerShell:

```powershell
.\install.ps1 -Prefix "$env:LOCALAPPDATA\LLAM"
```

| Parameter | Meaning |
| --- | --- |
| `-Prefix <dir>` | Install prefix. Defaults to `%LOCALAPPDATA%\LLAM` when available. |
| `-Version <version>` | Release version to install. Defaults to `LLAM_INSTALL_VERSION` or the script default. |
| `-Target <target>` | Release target, normally `windows-x86_64`. |
| `-BaseUrl <url>` | Override the release asset base URL. |
| `-DryRun` | Print planned file operations without installing. |
| `-Force` | Allow overwrite of existing installed files. |

## CMake Options

| Option | Values | Meaning |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Release`, `Debug`, etc. | Build configuration for single-config generators. Defaults to `Release` when unset. |
| `CMAKE_INSTALL_PREFIX` | path | Install destination used by `cmake --install`. |
| `CMAKE_OSX_ARCHITECTURES` | one architecture | Select macOS architecture. Universal macOS builds are not supported yet. |
| `LLAM_ENABLE_WINDOWS_BACKEND` | `ON`, `OFF` | Enables the native Windows backend. Native Windows builds require `ON`. |

Common Windows build:

```powershell
cmake -S . -B build-windows -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DLLAM_ENABLE_WINDOWS_BACKEND=ON
```

## Makefile Variables

| Variable | Meaning |
| --- | --- |
| `CC` | C compiler, for example `gcc` or `clang`. |
| `CFLAGS` | Extra C compiler flags. |
| `LDLIBS` | Extra link flags and libraries. |
| `OBJDIR` | Object output directory for alternate sanitizer or profiling builds. |
| `WINDOWS_CMAKE_ARGS` | Extra CMake generator/config args when Make delegates on Windows. |
| `LLAM_VERSION` | Version override used by release/package flows. |

Examples:

```sh
make -j4 CC=gcc
make OBJDIR=object-asan CC=clang CFLAGS='-O1 -g -fsanitize=address,undefined' test
```

## Runtime Options

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
| `preempt_mode` | One of `LLAM_PREEMPT_*`. |
| `preempt_poll_period` | Safepoint flag-poll period; `0` selects a profile default. |
| `preempt_quantum_ns` | Global preemption slice override; `0` uses task-class budgets. |
| `worker_min` | Minimum online scheduler workers; `0` selects compatibility policy. |
| `worker_count` | Initial online scheduler workers; a lone nonzero value selects a fixed count. |
| `worker_max` | Allocated scheduler-worker capacity. |
| `blocking_min` | Blocking workers requested at initialization. |
| `blocking_max` | Hard blocking-worker capacity. |
| `affinity_policy` | One of `LLAM_RUNTIME_AFFINITY_*`. |
| `cpu_count` / `cpu_ids` | Ordered process-allowed CPU IDs copied during initialization. |
| `task_prewarm_total` | Exact runtime-total logical task-object target; `0` selects compatibility policy. |
| `stack_prewarm_total` | Exact runtime-total default-stack target, capped at 4096; `0` selects compatibility policy. |
| `timer_prewarm_total` | Exact runtime-total timer-slot target; `0` selects compatibility policy. |
| `stack_cache_budget_bytes` | Runtime-wide retained mapping-byte ceiling, including guard pages; `0` selects 512 MiB. |
| `stack_cache_high_watermark_bytes` | Automatic-trim trigger; `0` selects 384 MiB. |
| `stack_cache_low_watermark_bytes` | Target after crossing the high watermark; `0` selects 256 MiB. |
| `stack_cache_idle_ns` | Minimum idle age for opportunistic release; `0` selects 30 seconds. |
| `stack_cache_flags` | Bitwise OR of `LLAM_RUNTIME_STACK_CACHE_F_*`. |
| `signal_flags` | Bitwise OR of `LLAM_RUNTIME_SIGNAL_F_*`; `0` leaves all host process actions untouched. |
| `preempt_signal` | POSIX cooperative-preemption signal; `0` selects the platform default and is ignored when preemption signal integration is disabled. |

Explicit worker bounds must satisfy
`1 <= worker_min <= worker_count <= worker_max <= cpu_count`. Prewarm fields
are exact only through size-aware lifecycle APIs: an allocation shortfall
fails initialization and unwinds the partial runtime. Environment compatibility
controls remain best-effort.

Stack-cache byte values must be page aligned and satisfy
`low <= high <= budget`. An exact stack prewarm must fit both the budget and
the high watermark, so initialization cannot immediately trim away its own
prewarm contract. The disabled flag requires zero explicit thresholds and
cannot be combined with a nonzero exact stack prewarm.

| Stack-cache flag | Effect |
| --- | --- |
| `LLAM_RUNTIME_STACK_CACHE_F_SECURE_SCRUB` | Zero usable stack bytes before reuse. |
| `LLAM_RUNTIME_STACK_CACHE_F_DISCARD_ON_RETURN` | Discard/decommit usable pages before retention and reactivate them after pop. |
| `LLAM_RUNTIME_STACK_CACHE_F_DISABLED` | Release every returned stack mapping instead of caching it. |

`SECURE_SCRUB` and `DISCARD_ON_RETURN` may be combined. A required scrub,
discard, or reactivation failure fails closed by releasing that mapping.

The option initializer selects `LLAM_RUNTIME_SIGNAL_DEFAULT_FLAGS`, which
enables preemption and guard-fault integration for compatibility. On POSIX,
all runtimes that install signal actions must use exactly the same flags and
resolved preemption signal; otherwise initialization fails with `EBUSY`.
Opted-out runtimes do not participate in that process-wide compatibility
check. Custom preemption signals must be catchable and must not overlap
`SIGSEGV` or another platform guard-fault signal. See
[Embedding LLAM](../guides/embedding.md#own-the-process-signal-policy) for
handler chaining, replacement, and per-thread alternate-stack ownership.

Affinity policies:

| Value | Use |
| --- | --- |
| `LLAM_RUNTIME_AFFINITY_NONE` | Do not change native thread affinity. |
| `LLAM_RUNTIME_AFFINITY_PREFER` | Attempt placement and continue if it fails. |
| `LLAM_RUNTIME_AFFINITY_REQUIRE` | Fail unless placement is supported and succeeds. |

Runtime profiles:

| Value | Use |
| --- | --- |
| `LLAM_RUNTIME_PROFILE_BALANCED` | General-purpose default. |
| `LLAM_RUNTIME_PROFILE_RELEASE_FAST` | Lower overhead release runs. |
| `LLAM_RUNTIME_PROFILE_DEBUG_SAFE` | Conservative diagnostics-focused runs. |
| `LLAM_RUNTIME_PROFILE_IO_LATENCY` | Favor I/O wakeup latency. |

Preemption modes:

| Value | Use |
| --- | --- |
| `LLAM_PREEMPT_OFF` | Disable automatic preemption requests. |
| `LLAM_PREEMPT_COOPERATIVE` | Honor explicit safepoints only. |
| `LLAM_PREEMPT_AUTO` | Request preemption when runtime pressure exists. |
| `LLAM_PREEMPT_STRICT` | Diagnostic mode for finding loops without enough safepoints. |

Experimental flags:

| Flag | Meaning |
| --- | --- |
| `LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS` | Give workers their own I/O rings/queues where supported. |
| `LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS_MULTISHOT` | Allow multishot watches in worker-ring mode. |
| `LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS` | Soft-park idle workers and reactivate on pressure. |
| `LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ` | Use the lock-free normal run queue. |
| `LLAM_RUNTIME_EXPERIMENTAL_F_HUGE_ALLOC` | Prefer hugepage-friendly allocator backing. |
| `LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL` | Enable Linux io_uring SQPOLL experiment. |

## Spawn Options

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
| `LLAM_SPAWN_F_PINNED` | Execute only on the task's logical home shard. This does not promise one stable pthread or CPU; a same-shard opaque helper is allowed. |
| `LLAM_SPAWN_F_NO_PREEMPT` | Restrict cooperative preemption checks. |
| `LLAM_SPAWN_F_SYS_TASK` | Mark runtime-owned helper work. |
| `LLAM_SPAWN_F_LATENCY_CRITICAL` | Promote wakeup and dispatch priority. |

## Timer Options

Use `llam_timer_create()` for the common case, or `llam_timer_create_ex()` for
explicit first deadline and ABI-sized options.

| Field | Meaning |
| --- | --- |
| `first_deadline_ns` | First absolute deadline in `llam_now_ns()` units; `0` means now. |
| `interval_ns` | Repeat interval in nanoseconds; must be non-zero. |
| `flags` | Reserved for future `LLAM_TIMER_F_*`; initialize to `0`. |

## Signal Options

Signal wait sets are Linux-only. Other platforms return `ENOTSUP`.

| Field | Meaning |
| --- | --- |
| `flags` | Reserved for future `LLAM_SIGNAL_F_*`; initialize to `0`. |

## I/O Buffer Options

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

## File Stat Options

`llam_stat_path_ex()` writes portable file metadata into `llam_file_stat_t`.
Use `LLAM_FILE_STAT_CURRENT_SIZE` for the size handshake.

File types:

| Value | Meaning |
| --- | --- |
| `LLAM_FILE_TYPE_REGULAR` | Regular file. |
| `LLAM_FILE_TYPE_DIRECTORY` | Directory. |
| `LLAM_FILE_TYPE_SYMLINK` | Symbolic link or Windows reparse point. |
| `LLAM_FILE_TYPE_OTHER` | Other platform-specific type. |

## Environment Variables

Environment variables are listed separately in [Environment Variables](environment.md).
