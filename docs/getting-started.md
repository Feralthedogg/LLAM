# Getting Started

Install LLAM, link it from CMake, and run one task. That is the normal path for
application code.

<div class="llam-step-grid">
  <a class="llam-step" href="#install-release">
    <span>1</span>
    <strong>Install</strong>
    <p>Put headers, libraries, CMake config, and pkg-config metadata in a prefix.</p>
  </a>
  <a class="llam-step" href="#use-from-cmake">
    <span>2</span>
    <strong>Link</strong>
    <p>Use the installed <code>llam::runtime</code> CMake target.</p>
  </a>
  <a class="llam-step" href="#minimal-program">
    <span>3</span>
    <strong>Run</strong>
    <p>Initialize a runtime, spawn work, drive it, then shut it down.</p>
  </a>
</div>

## Install Release

```sh
curl -fsSL https://github.com/Feralthedogg/LLAM/releases/latest/download/install.sh |
  sh -s -- --prefix "$HOME/.local"
```

Use a system prefix when the machine expects globally installed SDKs:

```sh
curl -fsSL https://github.com/Feralthedogg/LLAM/releases/latest/download/install.sh |
  sudo sh -s -- --prefix /usr/local
```

Choose a release or target explicitly when automatic host detection is not what
you want:

```sh
curl -fsSL https://github.com/Feralthedogg/LLAM/releases/download/v3.0.0/install.sh |
  sh -s -- --version 3.0.0 --target macos-aarch64 --prefix "$HOME/.local"
```

On Windows:

```powershell
Invoke-WebRequest "https://github.com/Feralthedogg/LLAM/releases/latest/download/install.ps1" -OutFile install.ps1
.\install.ps1 -Prefix "$env:LOCALAPPDATA\LLAM"
```

## Use From CMake

```cmake
find_package(llam CONFIG REQUIRED)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE llam::runtime)
```

Build with:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build
```

## Minimal Program

Use an explicit runtime and a result-returning join. This constructor retains
boundary/safepoint preemption; compiler-qualified asynchronous execution uses
the separate [managed C workflow](reference/managed-runtime.md).

```c
#include <llam/runtime.h>
#include <errno.h>

static void child(void *arg) {
    (void)arg;
    int error = llam_sleep_ns(1000000) == 0 ? 0 : errno;
    (void)llam_task_set_result(NULL, error);
}

int main(void) {
    llam_runtime_t *runtime = NULL;
    if (llam_runtime_create(NULL, 0, &runtime) != 0) return 1;
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
[embedding guide](guides/embedding.md) before adding a separate host driver or
sharing request state with callbacks.

## Verify The Install

Use a separate consumer project to verify installed package metadata. For source
trees, run the platform-local gate:

```sh
make verify-linux CC=gcc
CC=clang make verify-darwin
./scripts/docker_verify_linux.sh
```

On Windows:

```powershell
.\scripts\verify_windows.ps1 -Native
```
