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
curl -fsSL https://github.com/Feralthedogg/LLAM/releases/download/v2.1.0/install.sh |
  sh -s -- --version 2.1.0 --target macos-aarch64 --prefix "$HOME/.local"
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

```c
#include <llam/runtime.h>

static void child(void *arg) {
    (void)arg;
    llam_sleep_ns(1000000);
}

int main(void) {
    if (llam_runtime_init(NULL) != 0) {
        return 1;
    }

    llam_task_t *task = llam_spawn(child, NULL, NULL);
    if (task == NULL) {
        llam_runtime_shutdown();
        return 1;
    }

    llam_run();
    llam_join(task);
    llam_runtime_shutdown();
    return 0;
}
```

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
