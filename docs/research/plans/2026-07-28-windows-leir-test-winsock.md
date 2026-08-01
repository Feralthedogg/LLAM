# Windows LEIR Test Winsock Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the portable portion of `test_leir_native_segment` run under Windows by satisfying the standalone test process's Winsock lifecycle.

**Architecture:** Keep Winsock ownership in the test executable that creates sockets before runtime initialization. Add one Windows-only startup at process entry, preserve the existing test groups, and route every post-startup result through one balanced cleanup path.

**Tech Stack:** C11, Winsock 2.2, CMake, MinGW-w64, Wine, CTest

## Global Constraints

- The change is test-only and Windows-only.
- Do not alter the public API, runtime initialization, native segment semantics, or POSIX behavior.
- Native Windows CI remains the authority for Windows behavior.

---

### Task 1: Balance Winsock around the standalone native segment test

**Files:**
- Modify: `experiments/leir/test_leir_native_segment.c:2901`
- Test: `experiments/leir/test_leir_native_segment.c`

**Interfaces:**
- Consumes: Winsock `WSAStartup(MAKEWORD(2, 2), &winsock_data)` and `WSACleanup()`.
- Produces: A standalone `main` that initializes sockets before the first `leir_test_socketpair_type` call and cleans up on every post-startup exit.

- [x] **Step 1: Verify the existing integration test fails for the missing lifecycle**

Run:

```bash
env WINEDEBUG=+winsock \
  WINEPATH=/opt/homebrew/Cellar/mingw-w64/14.0.0/toolchain-x86_64/x86_64-w64-mingw32/bin \
  /opt/homebrew/bin/wine \
  build/final-verify-windows/test_leir_native_segment.exe
```

Expected: exit 1 with `WSASocketW not initialised` followed by
`wrong-count fixture init: Input/output error`.

- [x] **Step 2: Add the minimal Windows process lifecycle**

Replace the early-return structure in `main` with a `failed` accumulator and
add these Windows-only boundaries:

```c
int main(void) {
    int failed = 0;

#if LLAM_PLATFORM_WINDOWS
    WSADATA winsock_data;
    int winsock_result =
        WSAStartup(MAKEWORD(2, 2), &winsock_data);

    if (winsock_result != 0) {
        fprintf(
            stderr,
            "WSAStartup failed: %d\n",
            winsock_result);
        return 1;
    }
#endif

    if (test_init_validates_storage() != 0 ||
        test_bind_rejects_wrong_value_count() != 0 ||
        test_bind_rejects_negative_signed_length() != 0 ||
        test_bind_rejects_zero_length() != 0 ||
        test_bind_rejects_length_beyond_buffer() != 0 ||
        test_bind_rejects_length_above_uint() != 0 ||
        test_bind_rejects_null_nonempty_buffer() != 0 ||
        test_bind_rejects_invalid_fd() != 0 ||
        test_bind_accepts_repeated_fd() != 0 ||
        test_batch_validates_width_and_members() != 0 ||
        test_batch_rejects_unbound_member_and_rolls_back() != 0 ||
        test_valid_batch_without_runtime_releases_instances() != 0) {
        failed = 1;
    }

#if defined(__linux__)
    if (failed == 0 &&
        (test_bind_rejects_regular_file_with_enotsock() != 0 ||
         test_bind_rejects_stream_socket_with_eprototype() != 0 ||
         test_bind_rejects_unconnected_seqpacket() != 0 ||
         test_destroy_releases_pinned_fd() != 0 ||
         test_native_runtime_link_and_skip() != 0 ||
         test_native_runtime_pins_bound_fd() != 0 ||
         test_native_runtime_batches_width_two() != 0 ||
         test_direct_recv_send_pipeline_requires_semantic_barrier() != 0 ||
         test_fixed_recv_send_pipeline() != 0 ||
         test_fixed_short_read_does_not_copy_prior_activation() != 0 ||
         test_native_cancel_batch_ownership() != 0)) {
        failed = 1;
    }
    if (failed == 0 &&
        test_destroyed_instance_cannot_reenter_bind() != 0) {
        failed = 1;
    }
#endif

#if LLAM_PLATFORM_WINDOWS
    if (WSACleanup() == SOCKET_ERROR) {
        fprintf(
            stderr,
            "WSACleanup failed: %d\n",
            WSAGetLastError());
        failed = 1;
    }
#endif
    if (failed != 0) {
        return 1;
    }
    puts("LEIR native segment binding tests passed");
    return 0;
}
```

- [x] **Step 3: Rebuild and verify the Windows regression turns green**

Run:

```bash
cmake --build build/final-verify-windows \
  --target test_leir_native_segment -j4
env WINEDEBUG=-all \
  WINEPATH=/opt/homebrew/Cellar/mingw-w64/14.0.0/toolchain-x86_64/x86_64-w64-mingw32/bin \
  /opt/homebrew/bin/wine \
  build/final-verify-windows/test_leir_native_segment.exe
```

Expected: exit 0 with `LEIR native segment binding tests passed`.

- [x] **Step 4: Verify adjacent Windows and host suites**

Run:

```bash
env WINEDEBUG=-all \
  WINEPATH=/opt/homebrew/Cellar/mingw-w64/14.0.0/toolchain-x86_64/x86_64-w64-mingw32/bin \
  /opt/homebrew/bin/wine \
  build/final-verify-windows/test_windows_iocp_io.exe
env WINEDEBUG=-all \
  WINEPATH=/opt/homebrew/Cellar/mingw-w64/14.0.0/toolchain-x86_64/x86_64-w64-mingw32/bin \
  /opt/homebrew/bin/wine \
  build/final-verify-windows/test_windows_handle_io.exe
make -j4 test
LLAM_SELECT_RACE_ROUNDS=4 \
  ctest --test-dir build/final-verify-host \
  --output-on-failure --timeout 180
git diff --check
```

Expected: every command exits 0; Linux-only native tests may report their
documented platform skip on macOS.

- [x] **Step 5: Commit**

```bash
git add experiments/leir/test_leir_native_segment.c
git commit -m "test: initialize Winsock for native segment checks"
```
