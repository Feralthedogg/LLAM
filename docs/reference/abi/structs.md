# Structs and enums

Pass option and stats structs with their size, so newer and older binaries can
still exchange them.

## Inbound option structs

Public structs returned by LLAM use explicit size handshakes. Public structs
passed into LLAM use `_ex` entry points:

```c
int llam_runtime_init_ex(const llam_runtime_opts_t *opts, size_t opts_size);
int llam_runtime_enable_experimental_windows_async_preempt_ex(
    llam_runtime_t *runtime,
    const llam_windows_async_preempt_opts_t *opts,
    size_t opts_size);
llam_task_t *llam_spawn_ex(llam_task_fn fn, void *arg,
                           const llam_spawn_opts_t *opts,
                           size_t opts_size);
int llam_runtime_opts_init(llam_runtime_opts_t *opts, size_t opts_size);
int llam_spawn_opts_init(llam_spawn_opts_t *opts, size_t opts_size);
```

When `opts` is not `NULL`, `opts_size` must be nonzero. LLAM copies only the
overlapping prefix of the caller's struct and leaves fields beyond that prefix
at the same defaults used by the convenience wrappers. This lets older FFI
bindings pass smaller option structs to newer libraries when fields are
appended at the tail. Passing an option pointer with `opts_size == 0` fails
with `errno = EINVAL`.

The experimental Windows async opt-in requires every v1 field and deliberately
has no initializer that could silently acknowledge process-wide fail-fast.
Callers set its acknowledgement flag explicitly. Its input range array is
copied on success; range semantics are certified by the caller, not inferred
from ABI metadata or PE executable-page checks. The options and ranges must
remain unchanged until the call returns. An older library ignores option
bytes beyond its known v1 prefix; callers requiring future tail semantics
must check the loaded ABI minor before opting in.

The loaded library writes only the ABI prefix it knows. If a newer binding
passes a larger local option, stats, or ABI-info struct to an older LLAM
library, bytes beyond the older library's current struct size are left
untouched rather than zero-filled. Bindings that need deterministic future-tail
defaults must initialize their own full local struct before calling LLAM.

The non-`_ex` functions are C convenience wrappers that pass
`sizeof(llam_runtime_opts_t)` or `sizeof(llam_spawn_opts_t)` when options are
provided. New dynamic bindings should resolve and call the `_ex` symbols.

Use the public size macros rather than spelling `sizeof(...)` in generated
bindings:

```c
LLAM_RUNTIME_OPTS_CURRENT_SIZE
LLAM_WINDOWS_ASYNC_PREEMPT_OPTS_CURRENT_SIZE
LLAM_SPAWN_OPTS_CURRENT_SIZE
LLAM_RUNTIME_STATS_CURRENT_SIZE
LLAM_RUNTIME_RESOURCES_CURRENT_SIZE
LLAM_ABI_INFO_CURRENT_SIZE
LLAM_TIMER_OPTS_CURRENT_SIZE
LLAM_SIGNAL_OPTS_CURRENT_SIZE
LLAM_TASK_GROUP_OPTS_CURRENT_SIZE
LLAM_TASK_RESULT_CURRENT_SIZE
LLAM_WAIT_OPTS_CURRENT_SIZE
LLAM_IO_BUFFER_OPTS_CURRENT_SIZE
LLAM_FILE_STAT_CURRENT_SIZE
LLAM_PREEMPT_OPTS_CURRENT_SIZE
LLAM_PREEMPT_INFO_CURRENT_SIZE
LLAM_MODULE_OPTS_CURRENT_SIZE
```

`llam_runtime_opts_init()` and `llam_spawn_opts_init()` write only the known
compatible prefix, so bindings can ask the loaded library for its current
defaults without assuming that all-zero options equal the default policy.
Current defaults are non-deterministic balanced runtime startup and
default-class, default-stack task spawn.

Public option structs store enum-valued fields and flag words as `uint32_t`,
not C enum or `unsigned` objects. The enum types and constants remain available
for C source readability, but the struct layout does not depend on
compiler-specific enum width choices or non-fixed integer aliases.
`llam_spawn_opts_t::reserved0` explicitly occupies the former padding between
`flags` and `deadline_ns`. Initializers zero it, the current library ignores it,
and callers should leave it at zero so future tail-compatible bindings do not
inherit indeterminate padding bits as semantic input.

Experimental runtime toggles are grouped under
`llam_runtime_opts_t::experimental_flags`, a `uint64_t` bitset of
`LLAM_RUNTIME_EXPERIMENTAL_F_*` macros. The constants are defined with
`UINT64_C(...)` rather than C enum storage so future high-bit flags do not
depend on compiler enum width. This keeps unstable tuning switches inside one
ABI-stable field instead of freezing one public struct member per experiment.

## Outbound stats structs

Runtime statistics use an explicit output size:

```c
int llam_runtime_collect_stats_ex(llam_runtime_stats_t *stats,
                                  size_t stats_size);
```

When `stats` is not `NULL`, `stats_size` must be nonzero. LLAM zeroes the
caller-provided prefix and copies only the overlapping prefix of the current
library's `llam_runtime_stats_t`. This lets older FFI bindings pass smaller
stats structs to newer libraries without buffer overflow when fields are
appended at the tail. `llam_runtime_collect_stats()` is a C convenience wrapper
that passes `sizeof(llam_runtime_stats_t)`.

`llam_runtime_write_stats_json(fd)` writes the same canonical LLAM counter names
as a newline-terminated JSON object. The schema is additive: consumers must
ignore unknown fields and should not require a fixed field order.

All scalar control fields in public option structs and all 32-bit counter/state
fields in public stats structs use fixed-width integer storage. Language
bindings should model these fields as unsigned 32-bit integers, except
`sqpoll_cpu`, which is a signed 32-bit integer where `-1` means automatic CPU
selection.

## Enum-valued functions

Enum-valued public functions use `uint32_t` at the ABI boundary:

```c
int llam_task_set_class(uint32_t task_class);
uint32_t llam_task_class(const llam_task_t *task);
uint32_t llam_task_flags(const llam_task_t *task);
```

Callers should pass one of the `llam_task_class_t` constants to
`llam_task_set_class()` and interpret `llam_task_class()` as a
`llam_task_class_t` value. The fixed-width signature avoids depending on the C
compiler's enum representation. `llam_task_set_class()` returns `-1/EINVAL`
for unknown class values and `-1/ENOTSUP` outside a managed task.
