// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

/**
 * @file src/io/linux/research_ring_profile.c
 * @brief Strict io_uring setup profiles for bounded Linux experiments.
 */

#include "io/linux/runtime_io_ring_profile_linux_internal.h"

#include <errno.h>
#include <string.h>

uint32_t llam_linux_research_ring_profile_compiled_capabilities(void) {
    uint32_t capabilities = 0U;

#if defined(IORING_SETUP_SUBMIT_ALL)
    capabilities |= LLAM_LINUX_RING_CAP_SUBMIT_ALL;
#endif
#if defined(IORING_SETUP_COOP_TASKRUN)
    capabilities |= LLAM_LINUX_RING_CAP_COOP_TASKRUN;
#endif
#if defined(IORING_SETUP_SINGLE_ISSUER)
    capabilities |= LLAM_LINUX_RING_CAP_SINGLE_ISSUER;
#endif
#if defined(IORING_SETUP_DEFER_TASKRUN)
    capabilities |= LLAM_LINUX_RING_CAP_DEFER_TASKRUN;
#endif
    return capabilities;
}

static unsigned llam_linux_research_ring_profile_flag_submit_all(void) {
#if defined(IORING_SETUP_SUBMIT_ALL)
    return IORING_SETUP_SUBMIT_ALL;
#else
    return 0U;
#endif
}

static unsigned llam_linux_research_ring_profile_flag_coop_taskrun(void) {
#if defined(IORING_SETUP_COOP_TASKRUN)
    return IORING_SETUP_COOP_TASKRUN;
#else
    return 0U;
#endif
}

static unsigned llam_linux_research_ring_profile_flag_single_issuer(void) {
#if defined(IORING_SETUP_SINGLE_ISSUER)
    return IORING_SETUP_SINGLE_ISSUER;
#else
    return 0U;
#endif
}

static unsigned llam_linux_research_ring_profile_flag_defer_taskrun(void) {
#if defined(IORING_SETUP_DEFER_TASKRUN)
    return IORING_SETUP_DEFER_TASKRUN;
#else
    return 0U;
#endif
}

int llam_linux_research_ring_profile_select(
    const char *requested,
    bool sqpoll_requested,
    uint32_t capabilities,
    llam_linux_research_ring_profile_config_t *out) {
    uint32_t required_capabilities;
    uint32_t compiled_capabilities;
    llam_linux_research_ring_profile_config_t selected;

    errno = 0;
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (requested == NULL || requested[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (sqpoll_requested) {
        errno = EINVAL;
        return -1;
    }

    memset(&selected, 0, sizeof(selected));
    if (strcmp(requested, "submit_all") == 0) {
        selected.kind = LLAM_LINUX_RING_PROFILE_SUBMIT_ALL;
        selected.name = "submit_all";
        selected.setup_flags =
            llam_linux_research_ring_profile_flag_submit_all();
        required_capabilities = LLAM_LINUX_RING_CAP_SUBMIT_ALL;
    } else if (strcmp(requested, "coop_taskrun") == 0) {
        selected.kind = LLAM_LINUX_RING_PROFILE_COOP_TASKRUN;
        selected.name = "coop_taskrun";
        selected.setup_flags =
            llam_linux_research_ring_profile_flag_submit_all() |
            llam_linux_research_ring_profile_flag_coop_taskrun();
        required_capabilities =
            LLAM_LINUX_RING_CAP_SUBMIT_ALL |
            LLAM_LINUX_RING_CAP_COOP_TASKRUN;
    } else if (strcmp(requested, "defer_taskrun") == 0) {
        selected.kind = LLAM_LINUX_RING_PROFILE_DEFER_TASKRUN;
        selected.name = "defer_taskrun";
        selected.setup_flags =
            llam_linux_research_ring_profile_flag_submit_all() |
            llam_linux_research_ring_profile_flag_single_issuer() |
            llam_linux_research_ring_profile_flag_defer_taskrun();
        required_capabilities =
            LLAM_LINUX_RING_CAP_SUBMIT_ALL |
            LLAM_LINUX_RING_CAP_SINGLE_ISSUER |
            LLAM_LINUX_RING_CAP_DEFER_TASKRUN;
    } else {
        errno = EINVAL;
        return -1;
    }

    compiled_capabilities =
        llam_linux_research_ring_profile_compiled_capabilities();
    if (((capabilities & compiled_capabilities) &
         required_capabilities) != required_capabilities) {
        errno = ENOTSUP;
        return -1;
    }

    *out = selected;
    return 0;
}

int llam_linux_research_ring_profile_setup_errno(int setup_result) {
    int setup_errno;

    if (setup_result >= 0) {
        return 0;
    }
    setup_errno = -setup_result;
    if (setup_errno == EINVAL || setup_errno == EOPNOTSUPP) {
        return ENOTSUP;
    }
    return setup_errno;
}
