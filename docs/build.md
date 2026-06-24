# Build From Source

This path is for working on LLAM itself or validating a platform before a
release.

## Requirements

- CMake 3.20 or newer
- C11 compiler
- GNU Make or a CMake generator
- Python 3 for verification scripts
- Linux: liburing development headers
- macOS: Xcode command-line tools
- BSD: GNU Make, CMake, and the platform compiler
- Windows: MSVC/MASM or MinGW through CMake

## POSIX Makefile Build

Linux:

```sh
sudo apt install build-essential liburing-dev
make -j4 CC=gcc
make test
```

macOS:

```sh
xcode-select --install
CC=clang make -j4
make test
```

BSD:

```sh
gmake -j4 CC=cc
gmake test
```

## CMake Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Install to a local prefix:

```sh
cmake --install build --prefix "$HOME/.local"
```

## Native Windows Build

```powershell
cmake -S . -B build-windows -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DLLAM_ENABLE_WINDOWS_BACKEND=ON
cmake --build build-windows
ctest --test-dir build-windows --output-on-failure
```

The top-level Makefile delegates to this native CMake path on Windows:

```powershell
make all
make test
make verify-windows
```

## Useful Targets

- `make test`: API, ABI, runtime, broker, and shared-load smoke tests.
- `make test-quick`: direct tests plus quick server composite stress.
- `make test-full`: direct tests plus standard server composite stress.
- `make test-soak`: direct tests plus one-hour server composite soak.
- `make test-hardening`: static analysis, dependency audit, sanitizers, TSan, and heavy fuzz.
- `make bench-matrix`: benchmark matrix helper.
- `make package`: release archive shape checks.
