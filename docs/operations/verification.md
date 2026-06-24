# Verification

Verification is platform-local. A green Linux run does not prove the kqueue or
Windows IOCP backend.

For script-level parameters and tool usage, see [CLI And Tools](../reference/cli-tools.md).

## Quick Local Gate

```sh
make test
```

This runs focused API, ABI, runtime, broker, and shared-load smoke coverage.

## Platform Gates

Linux:

```sh
make verify-linux CC=gcc
./scripts/docker_verify_linux.sh
```

macOS:

```sh
CC=clang make verify-darwin
```

Windows:

```powershell
.\scripts\verify_windows.ps1 -Native
```

BSD is covered by the VM workflow in `.github/workflows/bsd.yml`.

## Stress And Soak

```sh
make test-quick
make test-full
make test-soak
make test-runtime-soak
```

`test-runtime-soak` exercises LLAM core runtime behavior without depending on
the example chat server policy.

## Hardening Gate

Run before release candidates or security-sensitive changes:

```sh
make analyze-cppcheck
make audit-deps
make test-fuzz-heavy
make test-process-utils
make test-runtime-soak
make test-hardening
```

## Package Smoke

```sh
make clean all test
./scripts/package_release.sh
```

Or:

```sh
make package
```

Release archives should include public headers, static and shared libraries,
CMake config, pkg-config metadata, examples, install scripts, and operations
docs.
