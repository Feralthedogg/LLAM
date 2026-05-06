/**
 * @file examples/stress_core_cases.c
 * @brief Core runtime stress cases for task scheduling, joins, sync, cancellation, and I/O paths.
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

#include "stress_internal.h"

void run_spawn_join_storm(void) {
    enum { kStormTasks = 64 };
    storm_state_t state;
    llam_task_t *tasks[kStormTasks];
    unsigned i;

    atomic_init(&state.completed, 0U);
    state.yields_per_task = 4U;
    for (i = 0; i < kStormTasks; ++i) {
        tasks[i] = llam_spawn(storm_child_task,
                            &state,
                            &(llam_spawn_opts_t){
                                .task_class = LLAM_TASK_CLASS_DEFAULT,
                                .stack_class = LLAM_STACK_CLASS_DEFAULT,
                            });
        if (tasks[i] == NULL) {
            stress_fail_msg("storm child spawn failed");
            return;
        }
    }
    for (i = 0; i < kStormTasks; ++i) {
        if (llam_join(tasks[i]) != 0) {
            stress_fail_msg("storm child join failed");
            return;
        }
    }
    if (atomic_load(&state.completed) != kStormTasks) {
        stress_fail_u32("storm completed", atomic_load(&state.completed), kStormTasks);
    }
}

void run_channel_ping_pong(void) {
    ping_state_t state;
    llam_task_t *peer;
    unsigned i;

    state.request = llam_channel_create(1U);
    state.response = llam_channel_create(1U);
    state.rounds = 64U;
    atomic_init(&state.echoed, 0U);
    if (state.request == NULL || state.response == NULL) {
        stress_fail_msg("ping channels create failed");
        if (state.request != NULL) {
            llam_channel_destroy(state.request);
        }
        if (state.response != NULL) {
            llam_channel_destroy(state.response);
        }
        return;
    }

    peer = llam_spawn(ping_peer_task,
                    &state,
                    &(llam_spawn_opts_t){
                        .task_class = LLAM_TASK_CLASS_DEFAULT,
                        .stack_class = LLAM_STACK_CLASS_DEFAULT,
                    });
    if (peer == NULL) {
        stress_fail_msg("ping peer spawn failed");
        llam_channel_destroy(state.request);
        llam_channel_destroy(state.response);
        return;
    }

    for (i = 0; i < state.rounds; ++i) {
        void *token = (void *)(intptr_t)(i + 1U);
        void *echo;

        if (llam_channel_send(state.request, token) != 0) {
            stress_fail_msg("ping request send failed");
            break;
        }
        echo = llam_channel_recv(state.response);
        if (echo != token) {
            stress_fail_msg("ping response mismatch");
            break;
        }
    }

    if (llam_join(peer) != 0) {
        stress_fail_msg("ping peer join failed");
    }
    if (atomic_load(&state.echoed) != state.rounds) {
        stress_fail_u32("ping echoed", atomic_load(&state.echoed), state.rounds);
    }
    llam_channel_destroy(state.request);
    llam_channel_destroy(state.response);
}

void run_mutex_convoy(void) {
    convoy_state_t state;
    llam_task_t *tasks[8];
    unsigned i;

    state.mutex = llam_mutex_create();
    state.iterations = 200U;
    state.workers = 8U;
    state.counter = 0U;
    if (state.mutex == NULL) {
        stress_fail_msg("convoy mutex create failed");
        return;
    }

    for (i = 0; i < state.workers; ++i) {
        tasks[i] = llam_spawn(convoy_worker_task,
                            &state,
                            &(llam_spawn_opts_t){
                                .task_class = LLAM_TASK_CLASS_DEFAULT,
                                .stack_class = LLAM_STACK_CLASS_DEFAULT,
                            });
        if (tasks[i] == NULL) {
            stress_fail_msg("convoy worker spawn failed");
            llam_mutex_destroy(state.mutex);
            return;
        }
    }
    for (i = 0; i < state.workers; ++i) {
        if (llam_join(tasks[i]) != 0) {
            stress_fail_msg("convoy worker join failed");
            llam_mutex_destroy(state.mutex);
            return;
        }
    }

    if (state.counter != state.workers * state.iterations) {
        stress_fail_u32("convoy counter", state.counter, state.workers * state.iterations);
    }
    llam_mutex_destroy(state.mutex);
}

void run_cancel_path(void) {
    cancel_state_t state;
    llam_task_t *waiter;
    llam_task_t *trigger;

    state.token = llam_cancel_token_create();
    atomic_init(&state.cancelled, 0U);
    atomic_init(&state.triggered, 0U);
    if (state.token == NULL) {
        stress_fail_msg("cancel token create failed");
        return;
    }

    waiter = llam_spawn(cancel_waiter_task,
                      &state,
                      &(llam_spawn_opts_t){
                          .task_class = LLAM_TASK_CLASS_DEFAULT,
                          .stack_class = LLAM_STACK_CLASS_DEFAULT,
                          .cancel_token = state.token,
                      });
    trigger = llam_spawn(cancel_trigger_task,
                       &state,
                       &(llam_spawn_opts_t){
                           .task_class = LLAM_TASK_CLASS_DEFAULT,
                           .stack_class = LLAM_STACK_CLASS_DEFAULT,
                       });
    if (waiter == NULL || trigger == NULL) {
        stress_fail_msg("cancel task spawn failed");
        if (waiter != NULL) {
            (void)llam_join(waiter);
        }
        if (trigger != NULL) {
            (void)llam_join(trigger);
        }
        (void)llam_cancel_token_destroy(state.token);
        return;
    }

    if (llam_join(waiter) != 0) {
        stress_fail_msg("cancel waiter join failed");
    }
    if (llam_join(trigger) != 0) {
        stress_fail_msg("cancel trigger join failed");
    }
    if (atomic_load(&state.triggered) != 1U) {
        stress_fail_u32("cancel trigger count", atomic_load(&state.triggered), 1U);
    }
    if (atomic_load(&state.cancelled) != 1U) {
        stress_fail_u32("cancel waiter count", atomic_load(&state.cancelled), 1U);
    }
    (void)llam_cancel_token_destroy(state.token);
}

void run_io_cancel_path(void) {
    io_cancel_state_t state;
    llam_task_t *waiter;
    llam_task_t *trigger;
    int sv[2];

    state.reader_fd = -1;
    state.writer_fd = -1;
    state.token = NULL;
    atomic_init(&state.cancelled, 0U);
    atomic_init(&state.triggered, 0U);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        stress_fail_msg("io cancel socketpair failed");
        return;
    }
    state.reader_fd = sv[0];
    state.writer_fd = sv[1];
    state.token = llam_cancel_token_create();
    if (state.token == NULL) {
        stress_fail_msg("io cancel token create failed");
        close(state.reader_fd);
        close(state.writer_fd);
        return;
    }

    waiter = llam_spawn(io_cancel_waiter_task,
                      &state,
                      &(llam_spawn_opts_t){
                          .task_class = LLAM_TASK_CLASS_DEFAULT,
                          .stack_class = LLAM_STACK_CLASS_DEFAULT,
                          .cancel_token = state.token,
                      });
    trigger = llam_spawn(io_cancel_trigger_task,
                       &state,
                       &(llam_spawn_opts_t){
                           .task_class = LLAM_TASK_CLASS_DEFAULT,
                           .stack_class = LLAM_STACK_CLASS_DEFAULT,
                       });
    if (waiter == NULL || trigger == NULL) {
        stress_fail_msg("io cancel task spawn failed");
        if (waiter != NULL) {
            (void)llam_join(waiter);
        }
        if (trigger != NULL) {
            (void)llam_join(trigger);
        }
        (void)llam_cancel_token_destroy(state.token);
        close(state.reader_fd);
        close(state.writer_fd);
        return;
    }

    if (llam_join(waiter) != 0) {
        stress_fail_msg("io cancel waiter join failed");
    }
    if (llam_join(trigger) != 0) {
        stress_fail_msg("io cancel trigger join failed");
    }
    if (atomic_load(&state.triggered) != 1U) {
        stress_fail_u32("io cancel trigger count", atomic_load(&state.triggered), 1U);
    }
    if (atomic_load(&state.cancelled) != 1U) {
        stress_fail_u32("io cancel waiter count", atomic_load(&state.cancelled), 1U);
    }

    (void)llam_cancel_token_destroy(state.token);
    close(state.reader_fd);
    close(state.writer_fd);
}

void run_owned_read_paths(void) {
    enum { kLargeOwnedBytes = 5000 };
    owned_read_state_t small_state;
    owned_read_state_t large_state;
    llam_task_t *writer;
    llam_io_buffer_t *buffer = NULL;
    char *large_payload;
    int sv[2];
    int small_sock_type = SOCK_STREAM;
    int large_sock_type = SOCK_STREAM;
    ssize_t rc;

#if defined(__APPLE__)
    small_sock_type = SOCK_DGRAM;
#endif
    small_state.fd = -1;
    small_state.payload = "owned";
    small_state.len = 5U;
    small_state.delay_ns = 1000000ULL;
    if (socketpair(AF_UNIX, small_sock_type, 0, sv) != 0) {
        stress_fail_msg("owned read small socketpair failed");
        return;
    }
    small_state.fd = sv[1];
    writer = llam_spawn(owned_read_writer_task,
                      &small_state,
                      &(llam_spawn_opts_t){
                          .task_class = LLAM_TASK_CLASS_DEFAULT,
                          .stack_class = LLAM_STACK_CLASS_DEFAULT,
                      });
    if (writer == NULL) {
        stress_fail_msg("owned read small writer spawn failed");
        close(sv[0]);
        close(sv[1]);
        return;
    }
    rc = llam_read_owned(sv[0], 16U, &buffer);
    if (rc != (ssize_t)small_state.len) {
        stress_fail_msg("owned read small rc failed");
    } else if (buffer == NULL || llam_io_buffer_capacity(buffer) < small_state.len) {
        stress_fail_msg("owned read small capacity failed");
    } else if (memcmp(llam_io_buffer_data(buffer), small_state.payload, small_state.len) != 0) {
        stress_fail_msg("owned read small payload failed");
    }
    llam_io_buffer_release(buffer);
    buffer = NULL;
    if (llam_join(writer) != 0) {
        stress_fail_msg("owned read small writer join failed");
    }
    close(sv[0]);
    close(sv[1]);

    large_payload = malloc(kLargeOwnedBytes);
    if (large_payload == NULL) {
        stress_fail_msg("owned read large malloc failed");
        return;
    }
    memset(large_payload, 'L', kLargeOwnedBytes);
    large_payload[0] = 'B';
    large_payload[kLargeOwnedBytes - 1] = 'E';
    large_state.fd = -1;
    large_state.payload = large_payload;
    large_state.len = kLargeOwnedBytes;
    large_state.delay_ns = 1000000ULL;
    if (socketpair(AF_UNIX, large_sock_type, 0, sv) != 0) {
        stress_fail_msg("owned read large socketpair failed");
        free(large_payload);
        return;
    }
    large_state.fd = sv[1];
    writer = llam_spawn(owned_read_writer_task,
                      &large_state,
                      &(llam_spawn_opts_t){
                          .task_class = LLAM_TASK_CLASS_DEFAULT,
                          .stack_class = LLAM_STACK_CLASS_DEFAULT,
                      });
    if (writer == NULL) {
        stress_fail_msg("owned read large writer spawn failed");
        close(sv[0]);
        close(sv[1]);
        free(large_payload);
        return;
    }
    rc = llam_read_owned(sv[0], kLargeOwnedBytes, &buffer);
    if (rc != kLargeOwnedBytes) {
        stress_fail_msg("owned read large rc failed");
    } else if (buffer == NULL || llam_io_buffer_capacity(buffer) < kLargeOwnedBytes) {
        stress_fail_msg("owned read large capacity failed");
    } else if (((const char *)llam_io_buffer_data(buffer))[0] != 'B' ||
               ((const char *)llam_io_buffer_data(buffer))[kLargeOwnedBytes - 1] != 'E') {
        stress_fail_msg("owned read large payload failed");
    }
    llam_io_buffer_release(buffer);
    buffer = NULL;
    if (llam_join(writer) != 0) {
        stress_fail_msg("owned read large writer join failed");
    }
    close(sv[0]);
    close(sv[1]);
    free(large_payload);
}

void run_dynamic_sleep_fanout(unsigned task_count, unsigned base_yields, uint64_t sleep_ns) {
    dynamic_sleep_wave_state_t wave;
    dynamic_sleep_child_state_t *child_states = NULL;
    llam_task_t **tasks = NULL;
    unsigned spawned = 0U;
    unsigned i;

    atomic_init(&wave.completed, 0U);
    wave.base_yields = base_yields;
    wave.sleep_ns = sleep_ns;
    child_states = calloc(task_count, sizeof(*child_states));
    tasks = calloc(task_count, sizeof(*tasks));
    if (child_states == NULL || tasks == NULL) {
        stress_fail_msg("dynamic sleep alloc failed");
        free(child_states);
        free(tasks);
        return;
    }

    for (i = 0; i < task_count; ++i) {
        child_states[i].wave = &wave;
        child_states[i].extra_yields = i & 3U;
        tasks[i] = llam_spawn(dynamic_sleep_child_task,
                            &child_states[i],
                            &(llam_spawn_opts_t){
                                .task_class = LLAM_TASK_CLASS_DEFAULT,
                                .stack_class = LLAM_STACK_CLASS_DEFAULT,
                            });
        if (tasks[i] == NULL) {
            stress_fail_msg("dynamic sleep child spawn failed");
            break;
        }
        spawned += 1U;
    }
    for (i = 0; i < spawned; ++i) {
        if (llam_join(tasks[i]) != 0) {
            stress_fail_msg("dynamic sleep child join failed");
        }
    }
    if (spawned != task_count) {
        free(child_states);
        free(tasks);
        return;
    }
    if (atomic_load(&wave.completed) != task_count) {
        stress_fail_u32("dynamic sleep completed", atomic_load(&wave.completed), task_count);
    }
    free(child_states);
    free(tasks);
}

void run_recv_owned_peek_path(void) {
    owned_read_state_t state;
    llam_task_t *writer;
    llam_io_buffer_t *buffer = NULL;
    int sv[2];
    ssize_t rc;

    state.fd = -1;
    state.payload = "peek";
    state.len = 4U;
    state.delay_ns = 1000000ULL;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        stress_fail_msg("recv owned peek socketpair failed");
        return;
    }
    state.fd = sv[1];
    writer = llam_spawn(owned_read_writer_task,
                      &state,
                      &(llam_spawn_opts_t){
                          .task_class = LLAM_TASK_CLASS_DEFAULT,
                          .stack_class = LLAM_STACK_CLASS_DEFAULT,
                      });
    if (writer == NULL) {
        stress_fail_msg("recv owned peek writer spawn failed");
        close(sv[0]);
        close(sv[1]);
        return;
    }

    rc = llam_recv_owned(sv[0], 16U, MSG_PEEK, &buffer);
    if (rc != (ssize_t)state.len) {
        stress_fail_msg("recv owned peek rc failed");
    } else if (buffer == NULL || memcmp(llam_io_buffer_data(buffer), state.payload, state.len) != 0) {
        stress_fail_msg("recv owned peek payload failed");
    }
    llam_io_buffer_release(buffer);
    buffer = NULL;

    rc = llam_read_owned(sv[0], 16U, &buffer);
    if (rc != (ssize_t)state.len) {
        stress_fail_msg("recv owned after peek rc failed");
    } else if (buffer == NULL || memcmp(llam_io_buffer_data(buffer), state.payload, state.len) != 0) {
        stress_fail_msg("recv owned after peek payload failed");
    }
    llam_io_buffer_release(buffer);
    buffer = NULL;

    if (llam_join(writer) != 0) {
        stress_fail_msg("recv owned peek writer join failed");
    }
    close(sv[0]);
    close(sv[1]);
}

void run_recv_owned_multishot_path(void) {
    static const char *payloads[] = {"one", "two", "three"};
    llam_io_buffer_t *buffer = NULL;
    int sv[2];
    int sock_type = SOCK_SEQPACKET;
    size_t i;

#if defined(__APPLE__)
    sock_type = SOCK_DGRAM;
#endif
    if (sock_type == SOCK_SEQPACKET && !stress_platform_supports_seqpacket_socketpair()) {
        return;
    }
    if (socketpair(AF_UNIX, sock_type, 0, sv) != 0) {
        stress_fail_msg("recv owned multishot socketpair failed");
        return;
    }

    for (i = 0; i < (sizeof(payloads) / sizeof(payloads[0])); ++i) {
        size_t len = strlen(payloads[i]);

        if (write(sv[1], payloads[i], len) != (ssize_t)len) {
            stress_fail_msg("recv owned multishot write failed");
            close(sv[0]);
            close(sv[1]);
            return;
        }
    }

    for (i = 0; i < (sizeof(payloads) / sizeof(payloads[0])); ++i) {
        size_t len = strlen(payloads[i]);
        ssize_t rc;
        unsigned retry;

        for (retry = 0U; retry < 8U; ++retry) {
            rc = llam_read_owned(sv[0], 16U, &buffer);
            if (rc >= 0 || (errno != EAGAIN && errno != EINTR)) {
                break;
            }
            llam_yield();
        }

        if (rc != (ssize_t)len) {
            char message[128];

            (void)snprintf(message,
                           sizeof(message),
                           "recv owned multishot rc failed rc=%zd errno=%d expected=%zu",
                           rc,
                           errno,
                           len);
            stress_fail_msg(message);
        } else if (buffer == NULL || memcmp(llam_io_buffer_data(buffer), payloads[i], len) != 0) {
            stress_fail_msg("recv owned multishot payload failed");
        }
        llam_io_buffer_release(buffer);
        buffer = NULL;
    }

    close(sv[0]);
    close(sv[1]);
}

void run_block_cancel_path(void) {
    block_cancel_state_t state;
    llam_task_t *waiter;
    llam_task_t *trigger;

    state.token = llam_cancel_token_create();
    atomic_init(&state.cancelled, 0U);
    atomic_init(&state.triggered, 0U);
    atomic_init(&state.blocking_started, 0U);
    if (state.token == NULL) {
        stress_fail_msg("block cancel token create failed");
        return;
    }

    waiter = llam_spawn(block_cancel_waiter_task,
                      &state,
                      &(llam_spawn_opts_t){
                          .task_class = LLAM_TASK_CLASS_DEFAULT,
                          .stack_class = LLAM_STACK_CLASS_DEFAULT,
                          .cancel_token = state.token,
                      });
    trigger = llam_spawn(block_cancel_trigger_task,
                       &state,
                       &(llam_spawn_opts_t){
                           .task_class = LLAM_TASK_CLASS_DEFAULT,
                           .stack_class = LLAM_STACK_CLASS_DEFAULT,
                       });
    if (waiter == NULL || trigger == NULL) {
        stress_fail_msg("block cancel task spawn failed");
        if (waiter != NULL) {
            (void)llam_join(waiter);
        }
        if (trigger != NULL) {
            (void)llam_join(trigger);
        }
        (void)llam_cancel_token_destroy(state.token);
        return;
    }

    if (llam_join(waiter) != 0) {
        stress_fail_msg("block cancel waiter join failed");
    }
    if (llam_join(trigger) != 0) {
        stress_fail_msg("block cancel trigger join failed");
    }
    if (atomic_load(&state.triggered) != 1U) {
        stress_fail_u32("block cancel trigger count", atomic_load(&state.triggered), 1U);
    }
    if (atomic_load(&state.cancelled) != 1U) {
        stress_fail_u32("block cancel waiter count", atomic_load(&state.cancelled), 1U);
    }
    (void)llam_cancel_token_destroy(state.token);
}
