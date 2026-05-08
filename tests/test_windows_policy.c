/**
 * @file tests/test_windows_policy.c
 * @brief Verify Windows 10/11 IOCP policy classification.
 */

#include "runtime_windows.h"
#include "runtime_windows_iocp.h"

#include "llam/platform.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *message) {
    fprintf(stderr, "[test_windows_policy] %s\n", message);
    return 1;
}

static int test_static_policy(void) {
    llam_windows_iocp_policy_t win10;
    llam_windows_iocp_policy_t win11;

    if (llam_windows_generation_from_version(10U, 0U, 21999U) != LLAM_WINDOWS_GENERATION_10 ||
        llam_windows_generation_from_version(10U, 0U, LLAM_WINDOWS_11_MIN_BUILD) != LLAM_WINDOWS_GENERATION_11 ||
        llam_windows_generation_from_version(6U, 3U, 9600U) != LLAM_WINDOWS_GENERATION_UNSUPPORTED) {
        return fail("Windows generation boundary classification mismatch");
    }

    llam_windows_default_iocp_policy(LLAM_WINDOWS_GENERATION_10, 10U, 0U, 19045U, 16U, &win10);
    llam_windows_default_iocp_policy(LLAM_WINDOWS_GENERATION_11, 10U, 0U, 22631U, 16U, &win11);

    if (strcmp(llam_windows_generation_name(win10.generation), "windows10") != 0) {
        return fail("Windows 10 generation name mismatch");
    }
    if (strcmp(llam_windows_generation_name(win11.generation), "windows11") != 0) {
        return fail("Windows 11 generation name mismatch");
    }
    if (strcmp(llam_windows_iocp_strategy_name(win10.strategy), "win10-conservative") != 0 ||
        strcmp(llam_windows_iocp_strategy_name(win11.strategy), "win11-batched") != 0) {
        return fail("Windows 10/11 strategy name mismatch");
    }
    if (win10.completion_batch != 64U || win11.completion_batch != 128U) {
        return fail("Windows 10/11 completion batch policy mismatch");
    }
    if (win10.iocp_concurrency >= win11.iocp_concurrency) {
        return fail("Windows 10/11 concurrency policy mismatch");
    }
    if (win10.control_batch >= win11.control_batch ||
        win10.accept_prepost >= win11.accept_prepost ||
        win10.recv_prepost >= win11.recv_prepost ||
        win10.poll_timeout_ms <= win11.poll_timeout_ms ||
        win10.timer_granularity_ms <= win11.timer_granularity_ms) {
        return fail("Windows 10/11 explicit tuning split mismatch");
    }
    if (win10.use_skip_completion_on_success != 0U || win11.use_skip_completion_on_success != 1U) {
        return fail("Windows 10/11 skip-completion policy mismatch");
    }
    if (win10.use_gqcs_ex == 0U || win11.use_gqcs_ex == 0U) {
        return fail("GQCSEx must be enabled for Windows 10/11");
    }
    return 0;
}

static int test_runtime_detection_contract(void) {
    llam_windows_iocp_policy_t policy;

    errno = 0;
    if (llam_windows_detect_iocp_policy(&policy) == 0) {
        if (policy.generation != LLAM_WINDOWS_GENERATION_10 &&
            policy.generation != LLAM_WINDOWS_GENERATION_11) {
            return fail("native detection returned unsupported generation");
        }
        return 0;
    }

#if LLAM_PLATFORM_WINDOWS
    return fail("native Windows detection failed");
#else
    if (errno != ENOTSUP) {
        return fail("non-Windows detection must fail with ENOTSUP");
    }
    return 0;
#endif
}

static int test_iocp_primitive_contract(void) {
    llam_windows_iocp_policy_t policy;
    void *handle = NULL;

    llam_windows_default_iocp_policy(LLAM_WINDOWS_GENERATION_11, 10U, 0U, 22631U, 8U, &policy);
    errno = 0;
    if (llam_windows_iocp_create(&policy, &handle) == 0) {
        llam_windows_iocp_completion_t completion;
        size_t count = 0U;

        if (llam_windows_iocp_post(handle, 0x11U, 0U, 7U) != 0) {
            llam_windows_iocp_close(handle);
            return fail("IOCP post failed");
        }
        if (llam_windows_iocp_drain(handle, &completion, 1U, 1000U, &count) != 0 || count != 1U) {
            llam_windows_iocp_close(handle);
            return fail("IOCP drain failed");
        }
        llam_windows_iocp_close(handle);
        if (completion.key != 0x11U || completion.bytes != 7U) {
            return fail("IOCP completion payload mismatch");
        }
        return 0;
    }

#if LLAM_PLATFORM_WINDOWS
    return fail("IOCP create failed on native Windows");
#else
    if (errno != ENOTSUP || handle != NULL) {
        return fail("non-Windows IOCP create must fail with ENOTSUP and NULL handle");
    }
    return 0;
#endif
}

int main(void) {
    if (test_static_policy() != 0 ||
        test_runtime_detection_contract() != 0 ||
        test_iocp_primitive_contract() != 0) {
        return 1;
    }
    puts("[test_windows_policy] ok");
    return 0;
}
