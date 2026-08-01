// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

/**
 * @file src/io/linux/runtime_io_ring_profile_linux_internal.h
 * @brief Private strict io_uring profile selection for Linux research builds.
 */

#ifndef LLAM_RUNTIME_IO_RING_PROFILE_LINUX_INTERNAL_H
#define LLAM_RUNTIME_IO_RING_PROFILE_LINUX_INTERNAL_H

#include <liburing.h>

#include <stdbool.h>
#include <stdint.h>

#define LLAM_LINUX_RESEARCH_RING_PROFILE_ENV \
    "LLAM_RESEARCH_IO_URING_PROFILE"

typedef enum llam_linux_research_ring_profile {
    LLAM_LINUX_RING_PROFILE_SUBMIT_ALL = 1,
    LLAM_LINUX_RING_PROFILE_COOP_TASKRUN = 2,
    LLAM_LINUX_RING_PROFILE_DEFER_TASKRUN = 3,
} llam_linux_research_ring_profile_t;

enum {
    LLAM_LINUX_RING_CAP_SUBMIT_ALL = UINT32_C(1) << 0,
    LLAM_LINUX_RING_CAP_COOP_TASKRUN = UINT32_C(1) << 1,
    LLAM_LINUX_RING_CAP_SINGLE_ISSUER = UINT32_C(1) << 2,
    LLAM_LINUX_RING_CAP_DEFER_TASKRUN = UINT32_C(1) << 3,
};

typedef struct llam_linux_research_ring_profile_config {
    uint32_t kind;
    unsigned setup_flags;
    const char *name;
} llam_linux_research_ring_profile_config_t;

uint32_t llam_linux_research_ring_profile_compiled_capabilities(void);

int llam_linux_research_ring_profile_select(
    const char *requested,
    bool sqpoll_requested,
    uint32_t capabilities,
    llam_linux_research_ring_profile_config_t *out);

int llam_linux_research_ring_profile_setup_errno(int setup_result);

#endif
