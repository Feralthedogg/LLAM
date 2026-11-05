# Quickstart

Install LLAM, link it from CMake, and run one task. That is the normal path for
application code.

<div class="steps" markdown>

1. [Install](#install-the-release)

    Put headers, libraries, CMake config, and pkg-config metadata in a prefix.

2. [Link](#use-from-cmake)

    Use the installed `llam::runtime` CMake target.

3. [Run](#run-a-minimal-program)

    Create a runtime, spawn work, drive it, then destroy it.

</div>

## Match the SDK version

This quickstart targets **LLAM 3.0.0 (ABI 2.4)**. A 2.2.1 SDK does not
provide `llam_runtime_create_compat()` or the task-result APIs used below, so these
examples will not compile against it.

The installer commands below select **v3.0.0**. Until its release assets are
published, [build and install the 3.0.0 source](build.md) to use these examples.
Keep the headers and library from that same SDK together.
For a POSIX install, `pkg-config --modversion llam` reports the selected SDK version.

## Install the release

=== "Linux, macOS"

    ```sh
    curl -fsSL https://github.com/Feralthedogg/LLAM/releases/download/v3.0.0/install.sh |
      sh -s -- --version 3.0.0 --prefix "$HOME/.local"
    ```

    Use a system prefix when the machine expects globally installed SDKs:

    ```sh
    curl -fsSL https://github.com/Feralthedogg/LLAM/releases/download/v3.0.0/install.sh |
      sudo sh -s -- --version 3.0.0 --prefix /usr/local
    ```

=== "Windows"

    ```powershell
    Invoke-WebRequest "https://github.com/Feralthedogg/LLAM/releases/download/v3.0.0/install.ps1" -OutFile install.ps1
    .\install.ps1 -Version 3.0.0 -Prefix "$env:LOCALAPPDATA\LLAM"
    ```

Choose a release or target explicitly when automatic host detection is not what
you want. See [Install script options](../reference/options/build.md#install-script).

## Use from CMake

```cmake title="CMakeLists.txt"
find_package(llam CONFIG REQUIRED)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE llam::runtime)
```

Build with:

=== "Linux, macOS"

    ```sh
    cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local"
    cmake --build build
    ```

=== "Windows"

    ```powershell
    cmake -S . -B build -DCMAKE_PREFIX_PATH="$env:LOCALAPPDATA\LLAM"
    cmake --build build
    ```

Without CMake, use the installed `pkg-config` metadata:

```sh
cc main.c $(pkg-config --cflags --libs llam) -o my_app
```

## Run a minimal program

Use an explicit runtime and a result-returning join.

This program is plain C, so it selects `llam_runtime_create_compat()`. The default
`llam_runtime_create()` refuses a plain C task with `ENOTSUP`: it runs only task code
from the [managed C toolchain](../guides/managed-c.md), which release archives do not
include.

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

> This constructor keeps boundary and safepoint preemption, so a long CPU loop needs
> [safepoints](../production/cpu-bound.md).

For application code, prefer an explicit runtime, `*_result` calls and groups
with `LLAM_TASK_GROUP_F_LINK_CANCEL`. The complete
[`examples/explicit_runtime.c`](https://github.com/Feralthedogg/LLAM/blob/main/examples/explicit_runtime.c)
example checks creation, run and join failures, nullable channel payloads,
borrowed child results, cancellation, channel drain and destruction:

```sh
make example_explicit_runtime
./example_explicit_runtime
./example_explicit_runtime --cancel
```

Both modes are registered in CTest and `make test`. Read the
[embedding guide](../guides/embedding.md) before adding a separate host driver or
sharing request state with callbacks.

## Verify the install

Use a separate consumer project to verify installed package metadata. For source
trees, run the platform-local gate:

=== "Linux"

    ```sh
    make verify-linux CC=gcc
    ./scripts/docker_verify_linux.sh
    ```

=== "macOS"

    ```sh
    CC=clang make verify-darwin
    ```

=== "Windows"

    ```powershell
    .\scripts\verify_windows.ps1 -Native
    ```

## What's next

- [Execution model](../concepts/execution-model.md) explains when LLAM parks, resumes, and migrates a task.
- [Examples](../examples/index.md) has focused task, channel, I/O, and embedding snippets.
- [Embed LLAM](../guides/embedding.md) covers host-owned runtimes and shutdown.
