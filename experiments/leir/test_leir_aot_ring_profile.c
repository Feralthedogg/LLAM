// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

/**
 * @file experiments/leir/test_leir_aot_ring_profile.c
 * @brief Strict Linux io_uring research-profile selector tests.
 */

#include <stdio.h>

#if !defined(__linux__)

int main(void) {
    puts("SKIP: LEIR AOT ring-profile tests require Linux");
    return 0;
}

#else

#include "io/linux/runtime_io_ring_profile_linux_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct selector_case {
    const char *requested;
    uint32_t required_capabilities;
    llam_linux_research_ring_profile_t expected_kind;
    unsigned expected_flags;
} selector_case_t;

static int test_exact_profiles(void) {
    static const selector_case_t cases[] = {
        {
            "submit_all",
            LLAM_LINUX_RING_CAP_SUBMIT_ALL,
            LLAM_LINUX_RING_PROFILE_SUBMIT_ALL,
            IORING_SETUP_SUBMIT_ALL,
        },
        {
            "coop_taskrun",
            LLAM_LINUX_RING_CAP_SUBMIT_ALL |
                LLAM_LINUX_RING_CAP_COOP_TASKRUN,
            LLAM_LINUX_RING_PROFILE_COOP_TASKRUN,
            IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN,
        },
        {
            "defer_taskrun",
            LLAM_LINUX_RING_CAP_SUBMIT_ALL |
                LLAM_LINUX_RING_CAP_SINGLE_ISSUER |
                LLAM_LINUX_RING_CAP_DEFER_TASKRUN,
            LLAM_LINUX_RING_PROFILE_DEFER_TASKRUN,
            IORING_SETUP_SUBMIT_ALL | IORING_SETUP_SINGLE_ISSUER |
                IORING_SETUP_DEFER_TASKRUN,
        },
    };
    size_t i;

    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); i += 1U) {
        llam_linux_research_ring_profile_config_t config;

        memset(&config, 0xa5, sizeof(config));
        errno = 0;
        if (llam_linux_research_ring_profile_select(
                cases[i].requested,
                false,
                cases[i].required_capabilities,
                &config) != 0 ||
            config.kind != cases[i].expected_kind ||
            config.setup_flags != cases[i].expected_flags ||
            strcmp(config.name, cases[i].requested) != 0 ||
            errno != 0) {
            fprintf(
                stderr,
                "exact profile selection failed for %s\n",
                cases[i].requested);
            return 1;
        }
    }
    return 0;
}

static int test_rejects_invalid_requests(void) {
    static const char *const invalid[] = {NULL, "", "unknown"};
    llam_linux_research_ring_profile_config_t config;
    size_t i;

    for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i += 1U) {
        memset(&config, 0xa5, sizeof(config));
        errno = 0;
        if (llam_linux_research_ring_profile_select(
                invalid[i],
                false,
                UINT32_MAX,
                &config) == 0 ||
            errno != EINVAL ||
            config.kind != 0U ||
            config.setup_flags != 0U ||
            config.name != NULL) {
            fputs("invalid profile request was accepted\n", stderr);
            return 1;
        }
    }

    errno = 0;
    if (llam_linux_research_ring_profile_select(
            "submit_all", false, UINT32_MAX, NULL) == 0 ||
        errno != EINVAL) {
        fputs("null profile output was accepted\n", stderr);
        return 1;
    }
    return 0;
}

static int test_requires_every_capability(void) {
    static const struct {
        const char *requested;
        uint32_t incomplete_capabilities;
    } cases[] = {
        {"submit_all", 0U},
        {"coop_taskrun", LLAM_LINUX_RING_CAP_SUBMIT_ALL},
        {
            "defer_taskrun",
            LLAM_LINUX_RING_CAP_SUBMIT_ALL |
                LLAM_LINUX_RING_CAP_SINGLE_ISSUER,
        },
    };
    size_t i;

    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); i += 1U) {
        llam_linux_research_ring_profile_config_t config;

        memset(&config, 0xa5, sizeof(config));
        errno = 0;
        if (llam_linux_research_ring_profile_select(
                cases[i].requested,
                false,
                cases[i].incomplete_capabilities,
                &config) == 0 ||
            errno != ENOTSUP ||
            config.kind != 0U ||
            config.setup_flags != 0U ||
            config.name != NULL) {
            fprintf(
                stderr,
                "incomplete capabilities accepted for %s\n",
                cases[i].requested);
            return 1;
        }
    }
    return 0;
}

static int test_rejects_explicit_sqpoll(void) {
    static const char *const profiles[] = {
        "submit_all",
        "coop_taskrun",
        "defer_taskrun",
    };
    size_t i;

    for (i = 0U; i < sizeof(profiles) / sizeof(profiles[0]); i += 1U) {
        llam_linux_research_ring_profile_config_t config;

        memset(&config, 0xa5, sizeof(config));
        errno = 0;
        if (llam_linux_research_ring_profile_select(
                profiles[i], true, UINT32_MAX, &config) == 0 ||
            errno != EINVAL ||
            config.kind != 0U ||
            config.setup_flags != 0U ||
            config.name != NULL) {
            fprintf(
                stderr,
                "SQPOLL profile request accepted for %s\n",
                profiles[i]);
            return 1;
        }
    }
    return 0;
}

static int test_normalizes_setup_errors(void) {
    static const struct {
        int setup_result;
        int expected_errno;
    } cases[] = {
        {0, 0},
        {-EINVAL, ENOTSUP},
        {-EOPNOTSUPP, ENOTSUP},
        {-EPERM, EPERM},
        {-EACCES, EACCES},
    };
    size_t i;

    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); i += 1U) {
        if (llam_linux_research_ring_profile_setup_errno(
                cases[i].setup_result) != cases[i].expected_errno) {
            fprintf(
                stderr,
                "setup error mapping failed for %d\n",
                cases[i].setup_result);
            return 1;
        }
    }
    return 0;
}

int main(void) {
    if (test_exact_profiles() != 0 ||
        test_rejects_invalid_requests() != 0 ||
        test_requires_every_capability() != 0 ||
        test_rejects_explicit_sqpoll() != 0 ||
        test_normalizes_setup_errors() != 0) {
        return 1;
    }
    puts("LEIR AOT ring-profile selector tests passed");
    return 0;
}

#endif
