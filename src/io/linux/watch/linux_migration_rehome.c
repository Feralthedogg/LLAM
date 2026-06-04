/**
 * @file src/io/linux/watch/linux_migration_rehome.c
 * @brief Linux I/O watch rehoming between nodes for load and idle balancing.
 *
 * @details
 * Linux uses the shared rehome state machine and provides io_uring-specific
 * completion payload adapters for buffered poll, accept, and recv readiness.
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

#include "io/linux/runtime_io_watch_linux_internal.h"

#define LLAM_REHOME_POLL_COMPLETION_T llam_poll_watch_completion_t
#define LLAM_REHOME_ACCEPT_COMPLETION_T llam_accept_watch_completion_t
#define LLAM_REHOME_RECV_COMPLETION_T llam_recv_watch_completion_t
#define LLAM_REHOME_POLL_COMPLETION_PUSH llam_poll_watch_completion_push
#define LLAM_REHOME_ACCEPT_COMPLETION_PUSH llam_accept_watch_completion_push
#define LLAM_REHOME_RECV_COMPLETION_PUSH_FROM_READY(head_, tail_, waiter_, ready_) \
    llam_recv_watch_completion_push((head_),                                       \
                                    (tail_),                                       \
                                    (waiter_),                                     \
                                    (ready_)->size,                                \
                                    (ready_)->bid,                                 \
                                    (ready_)->has_buffer,                          \
                                    (ready_)->node_index)
#define LLAM_REHOME_POLL_COMPLETION_DRAIN(target_, completions_) \
    llam_poll_watch_completion_drain((target_), (completions_))
#define LLAM_REHOME_ACCEPT_COMPLETION_DRAIN(target_, completions_) \
    llam_accept_watch_completion_drain((target_), (completions_))
#define LLAM_REHOME_RECV_COMPLETION_DRAIN(target_, completions_) \
    llam_recv_watch_completion_drain((target_)->runtime, (target_), (completions_))

#include "io/watch/rehome_template.inc"

#undef LLAM_REHOME_RECV_COMPLETION_DRAIN
#undef LLAM_REHOME_ACCEPT_COMPLETION_DRAIN
#undef LLAM_REHOME_POLL_COMPLETION_DRAIN
#undef LLAM_REHOME_RECV_COMPLETION_PUSH_FROM_READY
#undef LLAM_REHOME_ACCEPT_COMPLETION_PUSH
#undef LLAM_REHOME_POLL_COMPLETION_PUSH
#undef LLAM_REHOME_RECV_COMPLETION_T
#undef LLAM_REHOME_ACCEPT_COMPLETION_T
#undef LLAM_REHOME_POLL_COMPLETION_T
