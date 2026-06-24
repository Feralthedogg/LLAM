# Embedding

Use explicit runtime handles when the host owns lifecycle or when multiple
independent schedulers live in one process.

## Explicit Runtime

```c
#include <llam/runtime.h>

static void root(void *arg) {
    (void)arg;
    llam_sleep_ns(1000000);
}

int main(void) {
    llam_runtime_t *runtime = NULL;

    if (llam_runtime_create(NULL, 0, &runtime) != 0) {
        return 1;
    }

    llam_task_t *task = llam_runtime_spawn_ex(runtime, root, NULL, NULL, 0);
    int rc = llam_runtime_run_handle(runtime);

    if (task != NULL) {
        (void)llam_join(task);
    }

    llam_runtime_destroy(runtime);
    return rc;
}
```

## Runtime Options

```c
llam_runtime_opts_t opts;
llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE);

opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

llam_runtime_t *runtime = NULL;
llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime);
```

## Dynamic Loading Check

Resolve ABI metadata before binding the rest of the API:

```c
llam_abi_info_t info;

if (llam_abi_get_info(&info, LLAM_ABI_INFO_CURRENT_SIZE) != 0) {
    return 1;
}
if (info.abi_major != LLAM_ABI_VERSION_MAJOR) {
    return 1;
}
```

Use [ABI Contract](../abi.md) for the full dynamic-loader rules.
