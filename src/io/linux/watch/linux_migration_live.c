/**
 * @file src/io/linux/watch/linux_migration_live.c
 * @brief Linux live I/O watch migration adapter.
 *
 * @details
 * The live migration state machine is shared with Darwin. Linux supplies the
 * completion payload adapters for io_uring provided-buffer receive readiness.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 * Licensed under the LLAM Commercial Reciprocity License 1.0.
 * See the LICENSE file distributed with this Software.
 */

#include "io/linux/runtime_io_watch_linux_internal.h"

#define LLAM_LIVE_POLL_COMPLETION_T llam_poll_watch_completion_t
#define LLAM_LIVE_ACCEPT_COMPLETION_T llam_accept_watch_completion_t
#define LLAM_LIVE_RECV_COMPLETION_T llam_recv_watch_completion_t
#define LLAM_LIVE_POLL_COMPLETION_PUSH llam_poll_watch_completion_push
#define LLAM_LIVE_ACCEPT_COMPLETION_PUSH llam_accept_watch_completion_push
#define LLAM_LIVE_RECV_FORWARD_COMPLETION_PUSH llam_recv_watch_completion_push
#define LLAM_LIVE_RECV_FORWARD_READY_PUSH(watch_, size_, bid_, has_buffer_, node_) \
    llam_recv_watch_push_ready((watch_), (size_), (bid_), (has_buffer_), (node_), NULL, 0U)
#define LLAM_LIVE_RECV_FORWARD_READY_CALL(watch_) \
    llam_recv_watch_push_ready((watch_), size, bid, has_buffer, ready_node_index, NULL, 0U)
#define LLAM_LIVE_RECV_FORWARD_PARAMS size_t size, unsigned short bid, bool has_buffer, unsigned ready_node_index
#define LLAM_LIVE_RECV_FORWARD_ARGS size, bid, has_buffer, ready_node_index
#define LLAM_LIVE_RECV_COMPLETION_PUSH_FROM_READY(head_, tail_, req_, ready_) \
    llam_recv_watch_completion_push((head_),                                      \
                                    (tail_),                                      \
                                    (req_),                                       \
                                    (ready_)->size,                               \
                                    (ready_)->bid,                                \
                                    (ready_)->has_buffer,                         \
                                    (ready_)->node_index)
#define LLAM_LIVE_RECV_READY_COMPLETION_OWNED(ready_) ((void)(ready_))
#define LLAM_LIVE_POLL_COMPLETION_DRAIN(target_, completions_) \
    llam_poll_watch_completion_drain((target_), (completions_))
#define LLAM_LIVE_ACCEPT_COMPLETION_DRAIN(target_, completions_) \
    llam_accept_watch_completion_drain((target_), (completions_))
#define LLAM_LIVE_RECV_COMPLETION_DRAIN(rt_, target_, completions_) \
    llam_recv_watch_completion_drain((rt_), (target_), (completions_))

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
