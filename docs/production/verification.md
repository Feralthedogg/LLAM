# Verification

Verification is platform-local. A green Linux run does not prove the kqueue or
Windows IOCP backend.

For script-level parameters and tool usage, see [CLI tools](../reference/cli/index.md).

## Quick local gate

```sh
make test
```

This runs focused API, ABI, runtime, broker, and shared-load smoke coverage.

## Platform gates

=== "Linux"

    ```sh
    make verify-linux CC=gcc
    LLAM_VERIFY_LINUX_EXPERIMENTAL=1 make verify-linux CC=gcc
    ./scripts/docker_verify_linux.sh
    ```

=== "macOS"

    ```sh
    CC=clang make verify-darwin
    LLAM_VERIFY_DARWIN_EXPERIMENTAL=1 CC=clang make verify-darwin
    ```

=== "Windows"

    ```powershell
    .\scripts\verify_windows.ps1
    .\scripts\verify_windows.ps1 -Native
    ```

    The default command verifies the Linux backend through WSL when available.
    `-Native` builds native Windows targets and runs the Windows CTest suite.

BSD support ended in 3.0.0. `make test-platform-support` checks early rejection
in Make, CMake, public headers, installation and package metadata.

## Managed compiler and SDK

The Linux workflow's native build matrix depends on `linux-managed-compiler-sdk`.
Its pinned LLVM 23.1.2 job requires all four stages to execute successfully:
`test_managed_compiler`, `test_managed_runtime_contracts`,
`test_managed_image_internal` and `test_managed_sdk`. A missing or skipped stage
is a failure. This requirement is separate from the existing LLVM 14 metadata
checks and from hardware-specific native asynchronous profile qualification.

Use `scripts/verify_managed_ci.py` on a required-mode build to run the identical
chain locally; see [managed C build and CI](../guides/managed-c.md#required-compiler-and-sdk-ci).

On hosts with a real admitted image, `test_managed_async_compiler_default`,
`test_managed_async_compiler_default_global` and
`test_managed_async_compiler_default_global_prefix` run a poll-free compiled workload
through `llam_runtime_create()`, `llam_runtime_init()` and `llam_runtime_init_ex()`,
and fail unless native capture accounts for its progress.

## Stress and soak

```sh
make test-quick
make test-full
make test-soak
make test-runtime-soak
```

`test-runtime-soak` exercises LLAM core runtime behavior without depending on
the example chat server policy.

## Hardening gate

Run before release candidates or security-sensitive changes:

```sh
make analyze-cppcheck
make audit-deps
make test-fuzz-heavy
make test-process-utils
make test-runtime-soak
make test-hardening
```

## Package smoke

Use a clean checkout for packaging. Preserve any measurements you need before
running `make clean`, which also removes profiling outputs.

```sh
make clean all test
./scripts/package_release.sh
```

Or:

```sh
make package
```

Tag pushes such as `v3.0.0` build and publish `.tar.xz` archives for Linux x86_64,
Linux aarch64, macOS x86_64, and macOS arm64, plus a native Windows x86_64 `.zip`,
through `.github/workflows/release.yml`.

Release archives should stay self-contained: public headers, static and shared
libraries, CMake config, pkg-config metadata, examples, install scripts, and
operations docs. On Linux x86-64, an `LLAM_PREEMPT_TOOLING=auto` or `required` package may
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
that, on a compatibility runtime, `OFF` and `COOPERATIVE` remain permanent policies,
while `AUTO`/`STRICT` become hybrid only after the experimental backend is explicitly
enabled and safe ranges are registered. Unsupported platforms must report
cooperative fallback or `ENOTSUP`.

`test_default_policy_internal` also checks the ordinary constructors. They default to
`ASYNC_REQUIRED`, refuse an unqualified task entry and the `COOPERATIVE` mode, and
return `ENOTSUP` in a build without a native backend and in a Linux TSan build, while
the `*_compat` constructors
keep running plain C.

## Bug-hunter validation profile

When investigating LLAM itself, reproduce failures against focused runtime
tests before changing the implementation. The current high-signal profile is:

```sh
make -B test

make OBJDIR=object-asan-hunt CC=clang \
  CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  test_runtime_api_edges test_runtime_core test_runtime_fuzz \
  test_runtime_invariants test_runtime_shutdown_internal \
  test_runtime_stress test_io_buffers test_sync_primitives stress

ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  LLAM_RUNTIME_FUZZ_SCENARIOS=512 ./test_runtime_fuzz

ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ./test_runtime_stress

ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  LLAM_STRESS_ROUNDS=6 \
  LLAM_STRESS_DYNAMIC_ROUNDS=4 \
  ./stress
```

`test_runtime_stress` takes no round counts; repeat the binary to scale it, or set
`LLAM_RUNTIME_STRESS_CASE` to run a single case. The `LLAM_STRESS_*` counts belong
to the `stress` program.

The Makefile records object-build and link signatures, so custom sanitizer or
`OBJDIR=object-*` builds cannot silently reuse normal test binaries or leave a
normal target linked from mismatched objects. Run `make clean` between unrelated
profiles when you want a fully empty tree; it removes `object-*`, analyzer
`.plist` files, and link-signature files in addition to ordinary build outputs.

Use ThreadSanitizer on focused race tests rather than the longest stackful
edge tests. In GitHub Actions, the Linux TSan nightly job is intentionally
allowed-failure and diagnostic-only because stackful fiber switching can produce
environment-dependent sanitizer reports. Treat its uploaded logs as required
triage input, but keep ASan/UBSan, direct runtime tests, and regular stress CI
as the hard gates. Linux TSan builds refuse native asynchronous preparation, so
focused race tests select a `*_compat` constructor.

```sh
make OBJDIR=object-tsan-hunt CC=clang \
  CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=thread -fno-omit-frame-pointer' \
  LDLIBS='-pthread -fsanitize=thread' \
  test_runtime_invariants test_runtime_fuzz test_sync_primitives

LLAM_RUNTIME_INVARIANT_SCENARIOS=24 ./test_runtime_invariants
LLAM_RUNTIME_FUZZ_SCENARIOS=64 ./test_runtime_fuzz
./test_sync_primitives
```

Treat a finding as actionable only after it is reproduced by one of the above
tests, a deterministic seed, or a small targeted reproducer. Static review
findings should become tests first unless the failure is an obvious compile-time
or contract violation.

Run the static-analysis and dependency-audit gates before release candidates or
security hardening merges:

```sh
make analyze-cppcheck
make audit-deps
make test-fuzz-heavy
make test-process-utils
make test-runtime-soak
make test-hardening
```

`make analyze-cppcheck` pins cppcheck to a 64-bit platform model because the
public handle ABI intentionally rejects 32-bit `uintptr_t` builds at
preprocessor time. `make audit-deps` audits the locked Rust comparison-harness
dependencies; LLAM's C runtime does not vendor those crates.
`make test-fuzz-heavy` runs the deterministic runtime fuzz suite at maximum
built-in single-runtime and multi-runtime scenario counts. `make
test-process-utils` verifies CI helper timeout cleanup, including descendant
process cleanup for hung benchmark/soak wrappers. `make test-runtime-soak`
repeats direct LLAM core tests for a configurable time window without the
example server policy in the loop; use `RUNTIME_SOAK_SECONDS`,
`RUNTIME_SOAK_SEED`, `RUNTIME_SOAK_FUZZ_SCENARIOS`, and
`RUNTIME_SOAK_MULTI_FUZZ_SCENARIOS` to scale it. `make test-hardening` is the
one-command local hardening gate for release candidates: it runs static
analysis, dependency audit, process-helper regression tests, ASan/UBSan, TSan,
and the heavy fuzz profile.

The current direct runtime tests are intended to catch bugs independent of the
example chat server policy: lifecycle races, task handle ownership, cancellation
token destroy/cancel races, lost wakeups, channel close/select races, blocking
callback cancellation, managed/unmanaged boundary behavior, POSIX I/O edge
semantics, owned-buffer lifetime after shutdown, and runtime dump ownership.

## Development scope/result gate

ABI 2.5 adds `test_scope_values`, `test_scope`, `test_scope_native_io` (CTest),
and optional C++17 `test_scope_cpp`. `make test` builds and executes the value,
scope, native I/O variant, C++ host and `example_scoped_results` checks.
The scope suite covers admission/close races, cancellation before/during cleanup,
expired request budgets, bounded/unbounded cleanup, helper and I/O retirement,
concurrent consumption, wrong-runtime handles and runtime teardown.

`make test-asan` includes scope and owned-value tests in its separate sanitizer
object directory. The managed installed-SDK gate compiles a real managed C scope
fixture, verifies its final image, and runs a C++ host consumer against relocated
headers and shared runtime. It requires default asynchronous admission, progress
through a no-safepoint rendezvous, and successful typed take after runtime teardown.
Native Windows CTest executes scope and IOCP poll-retirement cases; it does not
claim that the POSIX managed SDK launcher is qualified on Windows.


## Development borrowed/local execution gate

ABI 2.6 adds `test_cavo_lifetime` and `test_cavo_execution` to Make and CTest.
The lifetime suite covers borrowed group cancellation, expired request budgets,
runtime stop, active admission, real callback/cleanup retirement and owner checks.
Its I/O variants reuse buffers immediately after pending read/write cancel or close;
a later write would expose the replacement marker and a later read would corrupt it.
`--io-helper` forces the POSIX capability fallback before request publication;
Windows exercises overlapped named-pipe HANDLEs in native CI.

The execution suite checks nested regions, refusal from guarded native callbacks,
refused suspension, pending cancellation and exact exit status 125 for unbalanced
return. It also checks a real shared local thread, close/admission, stale handles,
self-drain refusal, independent task handles and delayed TLS/FLS destructors.
Both suites run with the separate `make test-asan` objects.

With required LLVM tooling, `test_cavo_preempt_regions` and
`test_cavo_local_native` run qualified no-poll fixtures on a single processor.
The mandatory managed CI driver requires both tests and a relocated SDK consumer;
missing or skipped tests fail that gate. Native Windows CTest validates the core
executor/region and IOCP/HANDLE lifetime paths. The Unix LLVM fixture does not
claim to qualify a Windows compiler profile or a new language frontend.
