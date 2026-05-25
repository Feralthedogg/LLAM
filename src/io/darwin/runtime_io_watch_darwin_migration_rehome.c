/**
 * @file src/io/darwin/runtime_io_watch_darwin_migration_rehome.c
 * @brief Darwin I/O watch rehoming between nodes for load and idle balancing.
 *
 * @details
 * Darwin uses the shared rehome state machine and provides kqueue-specific
 * completion payload adapters. Recv readiness owns copied bytes, so the adapter
 * transfers copy_data into the completion list on successful handoff.
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

#include "runtime_io_watch_darwin_internal.h"

static bool llam_darwin_rehome_recv_completion_push_from_ready(llam_darwin_recv_completion_t **head,
                                                               llam_darwin_recv_completion_t **tail,
                                                               llam_io_req_t *waiter,
                                                               llam_recv_ready_t *ready) {
    if (ready == NULL) {
        return false;
    }
    if (!llam_darwin_recv_completion_push(head,
                                          tail,
                                          waiter,
                                          ready->size,
                                          ready->copy_data,
                                          ready->copy_capacity)) {
        return false;
    }
    ready->copy_data = NULL;
    return true;
}

#define LLAM_REHOME_POLL_COMPLETION_T llam_darwin_poll_completion_t
#define LLAM_REHOME_ACCEPT_COMPLETION_T llam_darwin_accept_completion_t
#define LLAM_REHOME_RECV_COMPLETION_T llam_darwin_recv_completion_t
#define LLAM_REHOME_POLL_COMPLETION_PUSH llam_darwin_poll_completion_push
#define LLAM_REHOME_ACCEPT_COMPLETION_PUSH llam_darwin_accept_completion_push
#define LLAM_REHOME_RECV_COMPLETION_PUSH_FROM_READY llam_darwin_rehome_recv_completion_push_from_ready
#define LLAM_REHOME_POLL_COMPLETION_DRAIN(target_, completions_) \
    llam_darwin_poll_completion_drain((target_), (completions_))
#define LLAM_REHOME_ACCEPT_COMPLETION_DRAIN(target_, completions_) \
    llam_darwin_complete_accept_completions((target_), (completions_))
#define LLAM_REHOME_RECV_COMPLETION_DRAIN(target_, completions_) \
    llam_darwin_recv_completion_drain((target_), (completions_))

#include "../runtime_io_watch_rehome_template.inc"

#undef LLAM_REHOME_RECV_COMPLETION_DRAIN
#undef LLAM_REHOME_ACCEPT_COMPLETION_DRAIN
#undef LLAM_REHOME_POLL_COMPLETION_DRAIN
#undef LLAM_REHOME_RECV_COMPLETION_PUSH_FROM_READY
#undef LLAM_REHOME_ACCEPT_COMPLETION_PUSH
#undef LLAM_REHOME_POLL_COMPLETION_PUSH
#undef LLAM_REHOME_RECV_COMPLETION_T
#undef LLAM_REHOME_ACCEPT_COMPLETION_T
#undef LLAM_REHOME_POLL_COMPLETION_T
