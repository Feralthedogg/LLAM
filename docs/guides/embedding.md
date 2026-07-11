# Embedding LLAM

This guide is for applications, plugins, and language runtimes that host LLAM
instead of treating it as the whole process runtime.

## Link From CMake

Installed SDK:

```cmake
find_package(llam CONFIG REQUIRED)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE llam::runtime)
```

Source checkout:

```cmake
add_subdirectory(path/to/LLAM)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE llam_runtime)
```

Use `llam_runtime_shared` when a host loads LLAM dynamically.

## Own The Runtime

The official embedding path is the explicit runtime-handle lifecycle:

1. Create a handle with `llam_runtime_create()`.
2. Attach root work with `llam_runtime_spawn_ex()`.
3. Drive only that handle with `llam_runtime_run_handle()`.
4. Join or detach returned task handles.
5. Release the runtime with `llam_runtime_destroy()`.

```c
llam_runtime_t *rt = NULL;
if (llam_runtime_create(NULL, 0, &rt) != 0) {
    return 1;
}

llam_task_t *root_task = llam_runtime_spawn_ex(rt, root, user_data, NULL, 0);
if (root_task == NULL || llam_runtime_run_handle(rt) != 0) {
    llam_runtime_destroy(rt);
    return 1;
}

if (llam_join(root_task) != 0) {
    llam_runtime_destroy(rt);
    return 1;
}
llam_runtime_destroy(rt);
```

Use one explicit runtime per independent embedding boundary. The legacy
process-default lifecycle remains a convenience path for simple LLAM-owned
programs, but hosts should not repeatedly initialize and shut it down from
concurrent embedding threads.

## Dynamic Loading

Resolve ABI symbols first:

```c
uint32_t abi = llam_abi_version();

llam_abi_info_t info;
llam_abi_get_info(&info, LLAM_ABI_INFO_CURRENT_SIZE);
```

Reject libraries whose `abi_major` differs from the binding. Minor ABI changes
are additive.

## Struct Size Handshakes

Use the public size macros:

```c
llam_spawn_opts_t opts;
llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);

llam_runtime_spawn_ex(rt, fn, arg, &opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);
```

Bindings should model ABI-facing enum values and flags as fixed-width integers,
as described in [ABI Contract](../abi.md).

## Boundary Choice

In-process opaque handles harden against stale use, wrong-family casts, simple
forgery, and owner mismatch. They are not a sandbox against arbitrary
same-process memory read/write.

Use a process boundary and broker-mode isolation when untrusted code must not
read runtime internals. See [Security Model](../security.md).
