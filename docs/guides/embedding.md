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

Embedders should use explicit runtime handles:

```c
llam_runtime_t *rt = NULL;
llam_runtime_create(NULL, 0, &rt);

llam_task_t *root_task = llam_runtime_spawn_ex(rt, root, user_data, NULL, 0);
llam_runtime_run_handle(rt);

if (root_task != NULL) {
    llam_join(root_task);
}
llam_runtime_destroy(rt);
```

Avoid repeated concurrent init/shutdown of the legacy default runtime from host
threads. Use one explicit runtime per independent embedding boundary.

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
