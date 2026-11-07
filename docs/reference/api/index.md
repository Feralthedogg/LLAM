# API overview

This page maps the stable public surface in `include/llam/runtime.h`,
`include/llam/scope.h` (ABI 2.5+) and `include/llam/executor.h` (development ABI 2.6), `include/llam/io.h`, `include/llam/managed.h`, and `include/llam/platform.h`. Use it to find the right API
group quickly, then check [Options](../options/runtime.md) and [ABI contract](../abi/index.md)
when you need field-level or binary-compatibility rules.

<div class="cards" markdown>

- [Lifecycle](lifecycle.md)

    Create, run, stop, and destroy runtimes.

- [Tasks](tasks.md)

    Spawn, join, results, groups, and task-local values.

- [Execution controls](executors.md)

    Development ABI 2.6: short no-preemption regions.

- [Owned scopes and typed outcomes](scopes.md)

    Development ABI 2.5: owned inputs, independent futures, and cancellation-safe drain.

- [Time and cancellation](time.md)

    Clocks, sleeps, timers, signals, and cancellation tokens.

- [Sync and channels](sync.md)

    Mutexes, condition variables, channels, and select.

- [I/O](io.md)

    Streams, sockets, owned buffers, and files.

- [Blocking and diagnostics](blocking.md)

    Foreign blocking calls, stats, and dumps.

</div>

## Contracts

| Contract | Rule |
| --- | --- |
| Deadlines | Absolute deadlines use `llam_now_ns()` units. `0` means already expired unless the function documents another meaning. |
| Errors | Functions returning `-1` set `errno`; common values include `EINVAL`, `EAGAIN`, `ETIMEDOUT`, `ECANCELED`, `EPIPE`, `EBUSY`, `EXDEV`, `ENOTSUP`, and `ENOMEM`. |
| Runtime ownership | Runtime-aware objects are bound to the runtime that created them. Cross-runtime managed use fails with `EXDEV`. |
| ABI-sized structs | Use a matching `*_opts_init()` helper where provided and pass `*_CURRENT_SIZE`. Windows async opt-in deliberately has no initializer that could silently acknowledge process-wide fail-fast. |
| Nullable payloads | Channel and blocking APIs with `_result` suffix distinguish a successful `NULL` payload from failure. |
| Windows files | `llam_fd_t` is a socket on Windows. Use HANDLE APIs for file and pipe I/O. |

## ABI and versioning

| API | Purpose |
| --- | --- |
| `llam_abi_version` | Return packed public ABI version. Dynamic loaders should resolve this first. |
| `llam_version_string` | Return the static source/API version string. |
| `llam_abi_get_info` | Fill version, platform, and struct-size metadata for loaders and bindings. |
| `LLAM_ABI_INFO_CURRENT_SIZE` | Size token for `llam_abi_get_info`. |
| `LLAM_VERSION_*` / `LLAM_ABI_VERSION_*` | Compile-time source and ABI version macros. |
