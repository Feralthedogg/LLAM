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

#include <llam/runtime.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

static int test_profile_topology_contract(void) {
    static const struct {
        uint32_t kind;
        bool creator_is_submitter;
        int expected_result;
        int expected_errno;
    } cases[] = {
        {
            LLAM_LINUX_RING_PROFILE_SUBMIT_ALL,
            false,
            0,
            0,
        },
        {
            LLAM_LINUX_RING_PROFILE_COOP_TASKRUN,
            false,
            0,
            0,
        },
        {
            LLAM_LINUX_RING_PROFILE_DEFER_TASKRUN,
            true,
            0,
            0,
        },
        {
            LLAM_LINUX_RING_PROFILE_DEFER_TASKRUN,
            false,
            -1,
            ENOTSUP,
        },
        {UINT32_MAX, true, -1, EINVAL},
    };
    size_t i;

    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); i += 1U) {
        llam_linux_research_ring_profile_config_t config;
        int result;

        memset(&config, 0, sizeof(config));
        config.kind = cases[i].kind;
        errno = 0;
        result = llam_linux_research_ring_profile_validate_topology(
            &config, cases[i].creator_is_submitter);
        if (result != cases[i].expected_result ||
            errno != cases[i].expected_errno) {
            fprintf(
                stderr,
                "profile topology contract failed for kind %u\n",
                cases[i].kind);
            return 1;
        }
    }

    errno = 0;
    if (llam_linux_research_ring_profile_validate_topology(
            NULL, true) == 0 ||
        errno != EINVAL) {
        fputs("null topology profile was accepted\n", stderr);
        return 1;
    }
    return 0;
}

static int child_runtime_init(
    const char *profile,
    bool expect_invalid,
    bool sqpoll_requested) {
    llam_runtime_opts_t options;
    int init_errno;
    int init_result;

    if ((profile == NULL
             ? unsetenv(LLAM_LINUX_RESEARCH_RING_PROFILE_ENV)
             : setenv(
                   LLAM_LINUX_RESEARCH_RING_PROFILE_ENV,
                   profile,
                   1)) != 0) {
        return 1;
    }
    if (llam_runtime_opts_init(
            &options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 1;
    }
    options.worker_min = 1U;
    options.worker_count = 1U;
    options.worker_max = 1U;
    if (sqpoll_requested) {
        options.experimental_flags |=
            LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL;
    }

    errno = 0;
    init_result = llam_runtime_init_ex(
        &options, LLAM_RUNTIME_OPTS_CURRENT_SIZE);
    init_errno = errno;
    if (init_result == 0) {
        llam_runtime_shutdown();
        return expect_invalid ? 1 : 0;
    }
    llam_runtime_shutdown();
    if (expect_invalid) {
        return init_errno == EINVAL ? 0 : 1;
    }
    if (init_errno == ENOTSUP ||
        init_errno == EPERM ||
        init_errno == EACCES) {
        return 77;
    }
    return 1;
}

static int run_runtime_init_child(
    const char *profile,
    bool expect_invalid,
    bool sqpoll_requested) {
    pid_t child;
    pid_t waited;
    int status;

    child = fork();
    if (child < 0) {
        perror("fork ring-profile runtime probe");
        return -1;
    }
    if (child == 0) {
        int result = child_runtime_init(
            profile, expect_invalid, sqpoll_requested);

        fflush(NULL);
        _exit(result);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

static int test_runtime_rejects_invalid_profile(void) {
    int status = run_runtime_init_child("invalid", true, false);

    if (status != 0) {
        fputs(
            "runtime did not reject an invalid explicit ring profile\n",
            stderr);
        return 1;
    }
    return 0;
}

static int test_runtime_rejects_profile_with_sqpoll(void) {
    int status = run_runtime_init_child("submit_all", true, true);

    if (status != 0) {
        fputs(
            "runtime accepted an explicit ring profile with SQPOLL\n",
            stderr);
        return 1;
    }
    return 0;
}

static int test_default_runtime_control(void) {
    int status = run_runtime_init_child(NULL, false, false);

    if (status == 77) {
        fputs(
            "SKIP: default io_uring runtime unavailable on this host\n",
            stderr);
        return 77;
    }
    if (status != 0) {
        fputs("default runtime control failed\n", stderr);
        return 1;
    }
    return 0;
}

static int test_compiled_profile_setup(void) {
    static const struct {
        const char *name;
        uint32_t required_capabilities;
    } profiles[] = {
        {"submit_all", LLAM_LINUX_RING_CAP_SUBMIT_ALL},
        {
            "coop_taskrun",
            LLAM_LINUX_RING_CAP_SUBMIT_ALL |
                LLAM_LINUX_RING_CAP_COOP_TASKRUN,
        },
        {
            "defer_taskrun",
            LLAM_LINUX_RING_CAP_SUBMIT_ALL |
                LLAM_LINUX_RING_CAP_SINGLE_ISSUER |
                LLAM_LINUX_RING_CAP_DEFER_TASKRUN,
        },
    };
    uint32_t compiled_capabilities =
        llam_linux_research_ring_profile_compiled_capabilities();
    size_t i;

    for (i = 0U; i < sizeof(profiles) / sizeof(profiles[0]); i += 1U) {
        int status;

        if ((compiled_capabilities &
             profiles[i].required_capabilities) !=
            profiles[i].required_capabilities) {
            fprintf(
                stderr,
                "SKIP: %s is absent from the build headers\n",
                profiles[i].name);
            continue;
        }
        status = run_runtime_init_child(
            profiles[i].name, false, false);
        if (status == 77) {
            fprintf(
                stderr,
                "SKIP: %s is unavailable on this host\n",
                profiles[i].name);
            continue;
        }
        if (status != 0) {
            fprintf(
                stderr,
                "runtime setup failed for %s\n",
                profiles[i].name);
            return 1;
        }
    }
    return 0;
}

int main(void) {
    int control_result;

    if (test_exact_profiles() != 0 ||
        test_rejects_invalid_requests() != 0 ||
        test_requires_every_capability() != 0 ||
        test_rejects_explicit_sqpoll() != 0 ||
        test_normalizes_setup_errors() != 0 ||
        test_profile_topology_contract() != 0 ||
        test_runtime_rejects_invalid_profile() != 0 ||
        test_runtime_rejects_profile_with_sqpoll() != 0) {
        return 1;
    }
    control_result = test_default_runtime_control();
    if (control_result != 0) {
        return control_result;
    }
    if (test_compiled_profile_setup() != 0) {
        return 1;
    }
    puts("LEIR AOT ring-profile selector tests passed");
    return 0;
}

#endif
