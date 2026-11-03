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
- Windows: MSVC/MASM or MinGW through CMake
- Optional Linux x86-64 compiler metadata tooling: LLVM development files, a
  same-major clang, Bash, and `objcopy` for `LLAM_PREEMPT_TOOLING=required`

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

BSD support ended in 3.0.0. Current builds reject BSD targets; see the
[platform support policy](reference/platform-support.md#bsd-retirement-in-300).

## CMake Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

The optional LLVM preemption pass and metadata inspector are separate from the
runtime ABI and are disabled by default. See
[Compiler preemption metadata](tooling/preempt-metadata.md) for the `off`,
`auto`, and `required` modes, `llam-clang`, and supported-target limits.
To install just the metadata tooling component after building it:

```sh
cmake --build build --target llam_preempt_pass llam_preempt_metadata_inspect
cmake --install build --component PreemptTools --prefix /tmp/llam-sdk
```

When CMake finds Python 3, CTest also registers the Phase 0/Phase 1A source
audits, shared-export audit, and `audit_production_test_hooks` for both
production static/shared artifacts.  A configure with `-DPython3_ROOT_DIR=...`
or a toolchain that cannot find Python omits those Python-backed tests; release
validation must therefore require Python explicitly.  The production-hook
audit uses `nm -a` (or `llvm-nm`), rejects uppercase externally defined
`llam_*_test_*` symbols, and permits documented lowercase local fallback
symbols.  If neither symbol tool exists, CTest marks that audit skipped (77),
which is a platform limitation rather than a clean symbol result.  Make uses
the configured `$(PYTHON)` interpreter to invoke the same checker for both
artifacts, so its `llvm-nm` fallback and local-symbol exception are identical
to CMake's; a missing symbol tool is reported as the same documented platform
limitation.  The Phase 1A source checker is likewise a lexical
canonical-allowlist gate: grouped/cast/comma lvalues, indirect aliases,
noncanonical lock indices, and hidden wrappers are not inferred.  P-resource
migration (Phase 1B), foreign detach/reattach (Phase 2), and Rust, Python,
JVM, .NET, and Go wrappers are outside this audit.

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

For compiler-qualified C, use the [managed runtime build and SDK
workflow](reference/managed-runtime.md). Qualified MinGW x64, Linux x64/ARM64
and macOS ARM64 builds compile native async support by default. The managed
constructor's default is `ASYNC_AUTO`, while the compatibility constructors
retain their existing boundary/safepoint behavior. MSVC and Intel macOS
profile limits are listed in [platform support](reference/platform-support.md).

For the experimental Win64/MinGW async-preemption qualification,
configure with `-DLLAM_EXPERIMENTAL_ASYNC_PREEMPT=ON` and
`-DLLAM_WINDOWS_ASYNC_SCHEDULER_ORACLE=ON`, then run the
`test_windows_async_public_*`, `test_windows_async_config_*`,
`test_windows_async_lifetime_*`,
`test_windows_async_safe_pc_*`,
`test_windows_async_transaction_*`, `test_windows_async_auto_*`, and
`test_windows_async_destroy_phase_*`, and `test_windows_async_pe_*` CTests.
The PE cases exercise the private loaded-image LLPM v1 collector: exact
range decoding, absent/malformed/unsupported metadata, loader-reference
lifetime, and handoff to the separate public expert enablement call. They
do not certify arbitrary Windows code or emit metadata for it. The destroy cases hold five
test-only boundaries from pre-first-dispatch through a second run on a new
caller M, then overlap the real public runtime destroy; those pauses are not
compiled into the ordinary runtime.
The configuration cases exercise enablement against host/foreign spawn, run
start, public destroy, and failed controller startup. Registration and
enablement pin the runtime, serialize their private state, and reject an
in-flight spawn before it can publish a capsule-less task into an enabled
runtime. The delayed-controller case lets a first enabled run drain before
the controller's first tick, then checks that its late start ignores the old
binding and a second run can still be automatically captured and restored.
For compatibility constructors, compiling the backend does not activate it
merely by setting `LLAM_PREEMPT_AUTO` or `STRICT`. The expert-only
`llam_runtime_enable_experimental_windows_async_preempt_ex()` call atomically
copies caller-certified, nonoverlapping PE-image ranges and enables the
backend before the first worker run and before any G is live. It requires the explicit
`LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST` flag and returns `ENOTSUP`
in builds without this backend. LLAM checks image backing and pins loader
references through runtime shutdown; it cannot prove the code is safe to
interrupt. JIT/private executable mappings are rejected and the caller must
not modify certified code while enabled. With this explicit enablement,
the existing watchdog rotates per-M persistent operations, services existing
restores/witnesses first, and admits at most two new probes per controller tick
(minimum per-M probe interval 5 ms). Native Suspend/Get/Set/Resume transactions
remain serialized even while several admitted operations coexist. An
empty registry makes zero remote context probes. Code outside the registered
ranges remains cooperatively preemptible only at LLAM boundaries/safepoints;
arbitrary C busy loops, C++ unwinding, dynamic worker/helper async operation, and bounded stop of
an uncooperative G are **not** supported by this experiment. An uncertain
committed remote context/suspension outcome in this opt-in
path terminates the hosting process through Windows fail-fast; normal stop,
cancellation, and pre-mutation refusals do not.

Fixed two- and three-primary configurations have native protocol qualification
with the pinned R8 scalar-leaf DLL. Every actual native primary registers before
OPEN/user dispatch; partial startup aborts without executing NEW tasks. Async
full-context restoration stays on the capture M. A completed eligible explicit
user yield can release only a freshly saved cooperative continuation with no old
bank/operation ownership. Runtime-internal yields and park/offload wakes stay
affine; inline foreign detach is refused. All primary Ms remain service owners
until global execution/operation/finalizer debt drains. This is a narrow
experimental profile, not arbitrary C/native-TLS migration or every worker count.

With `LLAM_WINDOWS_ASYNC_CERTIFIED_DLL` pointing to the pinned compiler-produced
DLL, build `test_windows_async_multim_oracle` and run
`ctest --test-dir build --output-on-failure -R '^test_windows_async_multim_'`.
The 14 bounded cases exercise simultaneous real Ms, independent operations,
fresh-yield migration, three distinct owners, delayed old-reference release,
rebind/stale identities, stop/destroy and nth-startup rollback. Each child limits
its own CPU affinity and has a 12-second deadline. The observer forces only
placement of an already-qualified ordinary READY entry, never capture/restore.

After manually certifying a bounded PE-image leaf, an expert caller opts in
from a host thread before spawning work:

```c
llam_windows_async_preempt_range_t range = {
    (uint64_t)(uintptr_t)certified_leaf_begin,
    (uint64_t)(uintptr_t)certified_leaf_end,
    0, 0
};
llam_windows_async_preempt_opts_t async_opts = {
    LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST, 0, &range, 1
};
if (llam_runtime_enable_experimental_windows_async_preempt_ex(
        runtime, &async_opts, sizeof(async_opts)) != 0) {
    /* Handle EBUSY, ENOTSUP, EINVAL, EALREADY or allocation failure. */
}
```

Passing a range is a caller certification, not a runtime proof of instruction
or unwind safety. The input array may be released after the call; the backend
copies it and freezes the registry on successful enablement.

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
- `make LLAM_PREEMPT_TOOLING=auto preempt-tools`: configure, build, and smoke-test optional LLVM preemption tooling.
- `make bench-matrix`: benchmark matrix helper.
- `make package`: release archive shape checks and the available metadata tools; set `LLAM_PREEMPT_TOOLING=auto|required` to request the optional Linux x86-64 plugin.
