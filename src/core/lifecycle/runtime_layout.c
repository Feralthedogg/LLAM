// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

/**
 * @file src/core/lifecycle/runtime_layout.c
 * @brief Allocates optional and over-aligned runtime layout storage.
 */

#include "runtime_internal.h"

#include <errno.h>

int llam_runtime_allocate_layout_storage(llam_runtime_t *rt) {
    unsigned i;

    if (rt == NULL || rt->active_shards == 0U) {
        errno = EINVAL;
        return -1;
    }
    rt->shards = llam_aligned_zalloc(
        _Alignof(llam_shard_t),
        rt->active_shards,
        sizeof(*rt->shards));
    if (rt->shards == NULL) {
        goto fail;
    }
    /*
     * Partial-init shutdown scans the full published shard array. Initialize
     * every descriptor before any later allocation can fail so zero-filled
     * entries cannot be mistaken for the caller's stdin.
     */
    for (i = 0U; i < rt->active_shards; ++i) {
        rt->shards[i].event_fd = -1;
    }
    if (rt->experimental_lockfree_normq != 0U) {
        rt->norm_cldeques = llam_aligned_zalloc(
            _Alignof(llam_cldeque_t),
            rt->active_shards,
            sizeof(*rt->norm_cldeques));
        if (rt->norm_cldeques == NULL) {
            goto fail;
        }
    }
    if (rt->trace_events_enabled != 0U) {
        rt->trace_events = llam_aligned_zalloc(
            LLAM_CACHELINE_BYTES,
            rt->active_shards,
            sizeof(*rt->trace_events) * LLAM_TRACE_RING_CAP);
        if (rt->trace_events == NULL) {
            goto fail;
        }
    }
    return 0;

fail:
    /*
     * allowed_cpus may already be published. Normal partial-init teardown
     * consumes every successfully allocated array exactly once.
     */
    llam_runtime_shutdown_rt(rt);
    errno = ENOMEM;
    return -1;
}
