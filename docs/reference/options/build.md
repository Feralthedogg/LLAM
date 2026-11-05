# Build and install options

Configure the install scripts, CMake, and the Makefile.

## Install script

POSIX:

```sh
curl -fsSL https://github.com/Feralthedogg/LLAM/releases/download/v3.0.0/install.sh |
  sh -s -- --version 3.0.0 --prefix "$HOME/.local"
```

| Option | Meaning |
| --- | --- |
| `--prefix <dir>` | Install prefix. Defaults to `/usr/local` on POSIX. |
| `--version <version>` | Release version to install. Defaults to `LLAM_INSTALL_VERSION` or the script default. |
| `--target <target>` | Release target, such as `linux-x86_64` or `macos-aarch64`. |
| `--base-url <url>` | Override the release asset base URL. Useful for mirrors or testing. |
| `--dry-run` | Print planned file operations without installing. |
| `--force` | Allow overwrite of existing installed files. |
| `-h`, `--help` | Print usage. |

These commands install 3.0.0. A [source build](../../start/build.md) is also available.

Choose a release or target explicitly when automatic host detection is not what
you want:

```sh
curl -fsSL https://github.com/Feralthedogg/LLAM/releases/download/v3.0.0/install.sh |
  sh -s -- --version 3.0.0 --target macos-aarch64 --prefix "$HOME/.local"
```

Windows PowerShell:

```powershell
Invoke-WebRequest "https://github.com/Feralthedogg/LLAM/releases/download/v3.0.0/install.ps1" -OutFile install.ps1
.\install.ps1 -Version 3.0.0 -Prefix "$env:LOCALAPPDATA\LLAM"
```

| Parameter | Meaning |
| --- | --- |
| `-Prefix <dir>` | Install prefix. Defaults to `%LOCALAPPDATA%\LLAM` when available. |
| `-Version <version>` | Release version to install. Defaults to `LLAM_INSTALL_VERSION` or the script default. |
| `-Target <target>` | Release target, normally `windows-x86_64`. |
| `-BaseUrl <url>` | Override the release asset base URL. |
| `-DryRun` | Print planned file operations without installing. |
| `-Force` | Allow overwrite of existing installed files. |

## CMake

| Option | Values | Meaning |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Release`, `Debug`, etc. | Build configuration for single-config generators. Defaults to `Release` when unset. |
| `CMAKE_INSTALL_PREFIX` | path | Install destination used by `cmake --install`. |
| `CMAKE_OSX_ARCHITECTURES` | one architecture | Select macOS architecture. Universal macOS builds are not supported yet. |
| `LLAM_ENABLE_WINDOWS_BACKEND` | `ON`, `OFF` | Enables the native Windows backend. Defaults to `ON`, and native Windows builds require it. |
| `LLAM_EXPERIMENTAL_ASYNC_PREEMPT` | `ON`, `OFF` | Build native asynchronous preemption backends. Defaults to `ON` on Linux x86_64 and aarch64, macOS arm64, and Windows x86_64 with MinGW. Explicit `ON` also builds experimental adapters such as Intel macOS. In a build without a backend, `llam_runtime_create()` and `llam_runtime_init()` fail with `ENOTSUP` and the `*_compat` constructors still work. |
| `LLAM_MANAGED_TOOLING` | `off`, `auto`, `required` | Build the LLVM 23.1.2 managed C tooling. Defaults to `off`. See [Managed C](../../guides/managed-c.md). |
| `LLAM_PREEMPT_TOOLING` | `off`, `auto`, `required` | Build the optional compiler metadata tooling. Defaults to `off`. See [Preemption metadata](../../guides/preempt-metadata.md). |
| `LLAM_WINDOWS_ASYNC_SCHEDULER_ORACLE` | `ON`, `OFF` | Build the test-only Windows x64 native scheduler async-restore oracle. Defaults to `OFF`. |

Common Windows build:

```powershell
cmake -S . -B build-windows -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DLLAM_ENABLE_WINDOWS_BACKEND=ON
```

## Makefile

| Variable | Meaning |
| --- | --- |
| `CC` | C compiler, for example `gcc` or `clang`. |
| `CFLAGS` | Extra C compiler flags. |
| `LDLIBS` | Extra link flags and libraries. |
| `OBJDIR` | Object output directory for alternate sanitizer or profiling builds. |
| `WINDOWS_CMAKE_ARGS` | Extra CMake generator/config args when Make delegates on Windows. |
| `LLAM_VERSION` | Version override used by release/package flows. |
| `LLAM_EXPERIMENTAL_ASYNC_PREEMPT` | `auto`, `0`, or `1`. `auto` builds native asynchronous preemption on Linux x86_64 and aarch64 and on macOS arm64. In a build without a backend, `llam_runtime_create()` and `llam_runtime_init()` fail with `ENOTSUP` and the `*_compat` constructors still work. |
| `LLAM_PREEMPT_TOOLING` | `off`, `auto`, or `required` for the optional compiler metadata tooling. Defaults to `off`. |

Examples:

```sh
make -j4 CC=gcc
make test-asan CC=clang
make all WINDOWS_CMAKE_ARGS="-G Ninja"
```
