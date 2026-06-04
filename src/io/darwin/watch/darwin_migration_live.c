/**
 * @file src/io/darwin/watch/darwin_migration_live.c
 * @brief Darwin live I/O watch migration adapter.
 *
 * @details
 * The live migration state machine is shared with Linux. Darwin supplies the
 * completion payload adapters for copied kqueue receive readiness.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "io/darwin/runtime_io_watch_darwin_internal.h"

/** @brief Pick the owner node for multishot watch state. */
unsigned llam_multishot_owner_node_index(llam_runtime_t *rt, unsigned fallback_node_index, int fd) {
    struct stat st;
    uint64_t hash;

    if (rt == NULL) {
        return 0U;
    }
    if (fallback_node_index >= rt->active_nodes) {
        fallback_node_index = rt->active_nodes > 0U ? 0U : fallback_node_index;
    }
    if (rt->experimental_shard_rings == 0U ||
        rt->experimental_shard_rings_multishot == 0U ||
        rt->active_nodes <= 1U ||
        fd < 0) {
        return fallback_node_index;
    }
    if (fstat(fd, &st) != 0) {
        return fallback_node_index;
    }

    hash = llam_hash_watch_identity_u64((uint64_t)st.st_dev);
    hash ^= llam_hash_watch_identity_u64((uint64_t)st.st_ino);
    hash ^= llam_hash_watch_identity_u64((uint64_t)(unsigned)fd);
    return (unsigned)(hash % rt->active_nodes);
}

#define LLAM_LIVE_POLL_COMPLETION_T llam_darwin_poll_completion_t
#define LLAM_LIVE_ACCEPT_COMPLETION_T llam_darwin_accept_completion_t
#define LLAM_LIVE_RECV_COMPLETION_T llam_darwin_recv_completion_t
#define LLAM_LIVE_POLL_COMPLETION_PUSH llam_darwin_poll_completion_push
#define LLAM_LIVE_ACCEPT_COMPLETION_PUSH llam_darwin_accept_completion_push
#define LLAM_LIVE_RECV_FORWARD_COMPLETION_PUSH llam_darwin_recv_completion_push_copy
#define LLAM_LIVE_RECV_FORWARD_READY_PUSH(watch_, data_, size_) \
    llam_recv_watch_push_ready_copy((watch_), (data_), (size_))
#define LLAM_LIVE_RECV_FORWARD_READY_CALL(watch_) llam_recv_watch_push_ready_copy((watch_), data, size)
#define LLAM_LIVE_RECV_FORWARD_PARAMS const unsigned char *data, size_t size
#define LLAM_LIVE_RECV_FORWARD_ARGS data, size
#define LLAM_LIVE_RECV_COMPLETION_PUSH_FROM_READY(head_, tail_, req_, ready_) \
    llam_darwin_recv_completion_push((head_),                                 \
                                     (tail_),                                 \
                                     (req_),                                  \
                                     (ready_)->size,                          \
                                     (ready_)->copy_data,                     \
                                     (ready_)->copy_capacity)
#define LLAM_LIVE_RECV_READY_COMPLETION_OWNED(ready_) ((ready_)->copy_data = NULL)
#define LLAM_LIVE_POLL_COMPLETION_DRAIN(target_, completions_) \
    llam_darwin_poll_completion_drain((target_), (completions_))
#define LLAM_LIVE_ACCEPT_COMPLETION_DRAIN(target_, completions_) \
    llam_darwin_complete_accept_completions((target_), (completions_))
#define LLAM_LIVE_RECV_COMPLETION_DRAIN(rt_, target_, completions_) \
    do {                                                            \
        (void)(rt_);                                                \
        llam_darwin_recv_completion_drain((target_), (completions_));\
    } while (0)

#include "io/watch/migration_live_forward_template.inc"
#include "io/watch/migration_live_finalize_template.inc"

#undef LLAM_LIVE_RECV_COMPLETION_DRAIN
#undef LLAM_LIVE_ACCEPT_COMPLETION_DRAIN
#undef LLAM_LIVE_POLL_COMPLETION_DRAIN
#undef LLAM_LIVE_RECV_READY_COMPLETION_OWNED
#undef LLAM_LIVE_RECV_COMPLETION_PUSH_FROM_READY
#undef LLAM_LIVE_RECV_FORWARD_ARGS
#undef LLAM_LIVE_RECV_FORWARD_PARAMS
#undef LLAM_LIVE_RECV_FORWARD_READY_CALL
#undef LLAM_LIVE_RECV_FORWARD_READY_PUSH
#undef LLAM_LIVE_RECV_FORWARD_COMPLETION_PUSH
#undef LLAM_LIVE_ACCEPT_COMPLETION_PUSH
#undef LLAM_LIVE_POLL_COMPLETION_PUSH
#undef LLAM_LIVE_RECV_COMPLETION_T
#undef LLAM_LIVE_ACCEPT_COMPLETION_T
#undef LLAM_LIVE_POLL_COMPLETION_T
