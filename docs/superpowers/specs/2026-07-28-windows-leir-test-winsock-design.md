# Windows LEIR Test Winsock Lifecycle Design

## Problem

`test_leir_native_segment` is built and registered with CTest on Windows, but
its platform-neutral binding tests create a TCP socket pair before any LLAM
runtime is initialized. Windows requires a successful `WSAStartup` first, so a
full Windows CTest invocation fails with `WSANOTINITIALISED`.

The runtime and the native segment implementation are not at fault. The missing
lifecycle belongs to this standalone test process.

## Design

Initialize Winsock once at the start of `test_leir_native_segment` on Windows
and balance it with `WSACleanup` before the process returns. Preserve the
existing test order and short-circuit behavior, but route all post-startup
results through one cleanup path.

The change is test-only and Windows-only. It does not alter the public API,
runtime initialization, native segment semantics, or POSIX builds.

## Alternatives Rejected

- Lazy initialization in `leir_test_socketpair_type` would hide process-global
  state inside a resource constructor and provide no clear matching cleanup.
- Initializing the LLAM runtime before the binding tests would change the
  contract under test and interfere with cases that deliberately initialize and
  shut down runtimes themselves.
- Excluding the test from Windows CTest would discard portable binding coverage
  instead of satisfying the platform prerequisite.

## Verification

The existing MinGW-built test under Wine is the regression:

- Before the change it fails at `wrong-count fixture init` with
  `WSANOTINITIALISED`.
- After the change it must complete with
  `LEIR native segment binding tests passed`.

The full MinGW build and the canonical macOS and Linux suites must remain
green. Native Windows CI remains the authority for Windows behavior.
