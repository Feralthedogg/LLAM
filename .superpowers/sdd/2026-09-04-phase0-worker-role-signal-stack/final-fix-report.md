# Phase 0 final fix report

Base: `2ad85acbe6a517f75221b8f874ae87619dd45229`

## Files changed

- `src/core/platform/platform.c`
  - `llam_uninstall_thread_signal_stack()` now validates the non-null worker,
    current TLS shard/worker/processor tuple, and worker/processor associations
    before reading `sigaltstack_installed` or any other mutable signal-stack
    state. A matching worker with no installed stack retains the no-op path;
    wrong-owner or inconsistent calls abort before touching another worker's
    plain fields.
- `tests/test_worker_identity_internal.c`
  - Added a POSIX-only child-process regression. A primary scheduler task forks
    a child with primary TLS and asks the child to uninstall the opaque-helper
    worker's stack. The parent does not inspect the helper's plain installed
    flag; it asserts the child terminates with `SIGABRT`.
- `Makefile`
  - Added focused ASan/UBSan and TSan worker-identity binaries, plus their
    Windows `.exe` names, to `CLEAN_FILES`.

## TDD evidence

RED (before the production reorder):

```text
$ make test_worker_identity_internal
exit 0 (target built)
$ ./test_worker_identity_internal
exit 1
test_worker_identity_internal: wrong-owner uninstall did not terminate the child with SIGABRT
```

GREEN (after the minimal production reorder):

```text
$ make test_worker_identity_internal && ./test_worker_identity_internal
exit 0
test_worker_identity_internal: all legacy checks passed
```

## Verification

```text
$ for run in 1 2 3 4 5 6 7 8 9 10; do ./test_worker_identity_internal; done
exit 0; ten runs printed "all legacy checks passed"

$ make test_runtime_shutdown_internal && ./test_runtime_shutdown_internal
exit 0; test_runtime_shutdown_internal ok

$ git diff --check
exit 0

$ make audit-production-test-hooks
exit 0

$ python3 scripts/test_worker_identity_boundary.py
exit 0; 20 tests, OK
$ python3 scripts/check_worker_identity_boundary.py --root .
exit 0; check_worker_identity_boundary ok: 187 source/header file(s)

$ cmake -S . -B build-phase0-final -DCMAKE_BUILD_TYPE=Debug
$ cmake --build build-phase0-final --target test_worker_identity_internal -j4
$ ctest --test-dir build-phase0-final --output-on-failure -R '^test_worker_identity_internal$'
exit 0; 1/1 test passed

$ make asan-test_worker_identity_internal OBJDIR=object-asan-final TESTHOOK_OBJDIR=object-testhooks-asan-final SANITIZER_TARGETS_ENABLED=1 CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' LDLIBS='-pthread -fsanitize=address,undefined' && ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_worker_identity_internal
exit 0; all legacy checks passed

$ make tsan-test_worker_identity_internal OBJDIR=object-tsan-final TESTHOOK_OBJDIR=object-testhooks-tsan-final SANITIZER_TARGETS_ENABLED=1 CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fno-omit-frame-pointer -fsanitize=thread' LDLIBS='-pthread -fsanitize=thread' && TSAN_OPTIONS=halt_on_error=1 ./tsan-test_worker_identity_internal
exit 0; all legacy checks passed
```

## Self-review

- The ownership guard short-circuits on `g_llam_tls_worker != worker`, so a
  wrong-worker call does not read that worker's `sigaltstack_installed` or
  `signal_stack_incarnation` fields. It also rejects null/incomplete TLS and
  mismatched worker/processor/shard associations.
- The regression checks the externally observable fail-fast contract in a
  child process and uses `waitpid()`; it does not race or sample another
  worker's plain state from the parent.
- Correct-worker no-op behavior remains unchanged for install failures such as
  helper `EAGAIN`.
- No public headers, ABI symbols, scheduling authority, signal-stack layout,
  or Phase 0 boundaries were changed beyond the requested guard, regression,
  and clean-list entries.

## Platform limitations

The child-process regression is guarded by `LLAM_PLATFORM_POSIX`; Windows
builds retain the test target but skip this POSIX-only assertion. Verification
ran on AppleClang/macOS arm64. The focused ASan run emitted the platform/runtime
warning about ASan ignoring `__asan_handle_no_return` for the custom context
stack, but exited 0 without sanitizer findings. Focused TSan also exited 0.
