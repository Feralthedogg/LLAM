# Blocking Work

Use LLAM blocking helpers when a managed task needs to call code that can block
an OS thread.

## Blocking Callback

```c
#include <llam/runtime.h>

typedef struct work_result {
    int value;
} work_result_t;

static void *slow_call(void *arg) {
    work_result_t *result = arg;

    result->value = 42;
    return result;
}

static void task(void *arg) {
    work_result_t result = {0};
    void *out = NULL;

    (void)arg;

    if (llam_call_blocking_result(slow_call, &result, &out) == 0) {
        consume_result(((work_result_t *)out)->value);
    }
}
```

Prefer `llam_call_blocking_result()` for FFI and bindings because it separates
LLAM submission errors from a callback that legitimately returns `NULL`.

## Manual Blocking Region

Use a manual region when an external API must run on the current OS thread:

```c
if (llam_enter_blocking() == 0) {
    call_external_library();
    llam_leave_blocking();
}
```

The runtime compensates so the shard can keep scheduling other tasks while the
current worker is pinned in foreign code.

## DNS And Filesystem Wrappers

Prefer LLAM wrappers for common blocking syscalls:

```c
struct addrinfo *result = NULL;
int gai_error = 0;

if (llam_getaddrinfo_result("example.com",
                            "443",
                            NULL,
                            &result,
                            &gai_error) == 0) {
    llam_freeaddrinfo_result(result);
}
```

Filesystem open/stat wrappers follow the same principle and route through the
blocking path from managed tasks.
