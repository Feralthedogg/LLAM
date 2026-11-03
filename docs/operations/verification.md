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

BSD support ended in 3.0.0. `make test-platform-support` checks early rejection
in Make, CMake, public headers, installation and package metadata.

## Managed Compiler And Installed SDK

The Linux workflow's native build matrix depends on `linux-managed-compiler-sdk`.
Its pinned LLVM 23.1.2 job requires all four stages to execute successfully:
`test_managed_compiler`, `test_managed_runtime_contracts`,
`test_managed_image_internal` and `test_managed_sdk`. A missing or skipped stage
is a failure. This requirement is separate from the existing LLVM 14 metadata
checks and from hardware-specific native asynchronous profile qualification.

Use `scripts/verify_managed_ci.py` on a required-mode build to run the identical
chain locally; see [managed C build and CI](../reference/managed-runtime.md#required-compiler-and-sdk-ci).

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
docs. On Linux x86-64, an `LLAM_PREEMPT_TOOLING=auto` or `required` package may
also include the optional `llam-clang` driver, metadata inspector, LLVM plugin,
and `share/llam/preempt/tooling.conf`; the release fixture checks that absent
optional tooling is represented as unavailable rather than silently enabled.

## Preemption capability gate

```sh
make test-preempt-metadata
make test_preempt_capabilities test_default_policy_internal
make LLAM_PREEMPT_TOOLING=auto preempt-tools
```

For a Linux x86-64 toolchain with LLVM installed, use
`LLAM_PREEMPT_TOOLING=required` and run both CMake and Meson. The gate validates
that `OFF` and `COOPERATIVE` remain permanent compatibility policies, while
`AUTO`/`STRICT` become hybrid only after the experimental backend is explicitly
enabled and safe ranges are registered. Unsupported platforms must report
cooperative fallback or `ENOTSUP`.
