# Embedding

Use explicit runtime handles when the host owns lifecycle or when multiple
independent schedulers live in one process.

## Explicit runtime

**Complete program.** The host creates a runtime, runs one task, and destroys the
runtime.

```c title="main.c"
#include <llam/runtime.h>
#include <errno.h>

static void child(void *arg) {
    (void)arg;
    int error = llam_sleep_ns(1000000) == 0 ? 0 : errno;
    (void)llam_task_set_result(NULL, error);
}

int main(void) {
    llam_runtime_t *runtime = NULL;
    if (llam_runtime_create_compat(NULL, 0, &runtime) != 0) return 1;
    llam_task_t *task = llam_runtime_spawn_ex(runtime, child, NULL, NULL, 0);
    if (task == NULL || llam_runtime_run_handle(runtime) != 0) {
        llam_runtime_destroy(runtime);
        return 1;
    }
    llam_task_result_t result;
    int failed = llam_join_result(task, &result, sizeof(result)) != 0;
    if (!failed) failed = result.flags != LLAM_TASK_RESULT_F_SET || result.error != 0;
    llam_runtime_destroy(runtime);
    return failed;
}
```

For a group, explicit host-created channel/token, cancellation and complete
cleanup, build the checked `example_explicit_runtime` target and run both
`./example_explicit_runtime` and `./example_explicit_runtime --cancel`.
See the [embedding guide](../guides/embedding.md) for ownership and drain rules.

## Runtime options

**Focused snippet.** Fill options with ABI-safe defaults, then set the fields you
need:

```c
llam_runtime_opts_t opts;
if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) return 1;

opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

llam_runtime_t *runtime = NULL;
if (llam_runtime_create_compat(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) return 1;
```

## Dynamic loading check

**Focused snippet.** Resolve ABI metadata before binding the rest of the API:

```c
llam_abi_info_t info;

if (llam_abi_get_info(&info, LLAM_ABI_INFO_CURRENT_SIZE) != 0) {
    return 1;
}
if (info.abi_major != LLAM_ABI_VERSION_MAJOR) {
    return 1;
}
/* llam_runtime_create_compat() and the asynchronous default arrived in ABI 2.4. */
if (info.abi_minor < 4U) {
    return 1;
}
```

Use [ABI contract](../reference/abi/index.md) for the full dynamic-loader rules.
