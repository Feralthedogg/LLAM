/**
 * @file examples/stress_timeout_cases.c
 * @brief Timeout and deadline-oriented stress cases for waits, channels, mutexes, condvars, and I/O.
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

void run_opaque_reuse(void) {
    opaque_state_t state;
    llam_task_t *task;

    atomic_init(&state.companion_steps, 0U);
    state.scopes = 3U;
    task = llam_spawn(opaque_scope_task,
                    &state,
                    &(llam_spawn_opts_t){
                        .task_class = LLAM_TASK_CLASS_DEFAULT,
                        .stack_class = LLAM_STACK_CLASS_DEFAULT,
                        .flags = LLAM_SPAWN_F_SYS_TASK | LLAM_SPAWN_F_PINNED,
                    });
    if (task == NULL) {
        stress_fail_msg("opaque scope task spawn failed");
        return;
    }
    if (llam_join(task) != 0) {
        stress_fail_msg("opaque scope task join failed");
        return;
    }
    if (atomic_load(&state.companion_steps) != state.scopes * 4U) {
        stress_fail_u32("opaque companion steps", atomic_load(&state.companion_steps), state.scopes * 4U);
    }
}

void run_join_timeout_path(void) {
    uint64_t sleep_ns = 5ULL * 1000ULL * 1000ULL;
    llam_task_t *target = llam_spawn(stress_sleep_task,
                                 &sleep_ns,
                                 &(llam_spawn_opts_t){
                                     .task_class = LLAM_TASK_CLASS_DEFAULT,
                                     .stack_class = LLAM_STACK_CLASS_DEFAULT,
                                 });

    if (target == NULL) {
        stress_fail_msg("join timeout target spawn failed");
        return;
    }
    if (llam_join_until(target, llam_now_ns() + 1ULL * 1000ULL * 1000ULL) == 0) {
        stress_fail_msg("join timeout unexpectedly succeeded");
    } else if (errno != ETIMEDOUT) {
        stress_fail_errno("join timeout errno", errno, ETIMEDOUT);
    }
    if (llam_join(target) != 0) {
        stress_fail_msg("join timeout target join failed");
    }
}

void run_mutex_timeout_path(void) {
    mutex_timeout_state_t state;
    llam_task_t *holder;

    state.mutex = llam_mutex_create();
    state.hold_ns = 5ULL * 1000ULL * 1000ULL;
    if (state.mutex == NULL) {
        stress_fail_msg("mutex timeout create failed");
        return;
    }

    holder = llam_spawn(mutex_holder_task,
                      &state,
                      &(llam_spawn_opts_t){
                          .task_class = LLAM_TASK_CLASS_DEFAULT,
                          .stack_class = LLAM_STACK_CLASS_DEFAULT,
                      });
    if (holder == NULL) {
        stress_fail_msg("mutex timeout holder spawn failed");
        llam_mutex_destroy(state.mutex);
        return;
    }

    if (llam_sleep_ns(1ULL * 1000ULL * 1000ULL) != 0) {
        stress_fail_msg("mutex timeout sync sleep failed");
    }
    if (llam_mutex_lock_until(state.mutex, llam_now_ns() + 1ULL * 1000ULL * 1000ULL) == 0) {
        stress_fail_msg("mutex timeout unexpectedly succeeded");
        (void)llam_mutex_unlock(state.mutex);
    } else if (errno != ETIMEDOUT) {
        stress_fail_errno("mutex timeout errno", errno, ETIMEDOUT);
    }
    if (llam_join(holder) != 0) {
        stress_fail_msg("mutex timeout holder join failed");
    }
    llam_mutex_destroy(state.mutex);
}

void run_cond_cancel_path(void) {
    cond_cancel_state_t state;
    llam_task_t *waiter;
    llam_task_t *trigger;

    state.mutex = llam_mutex_create();
    state.cond = llam_cond_create();
    state.token = llam_cancel_token_create();
    atomic_init(&state.cancelled, 0U);
    atomic_init(&state.triggered, 0U);
    atomic_init(&state.reacquired, 0U);
    if (state.mutex == NULL || state.cond == NULL || state.token == NULL) {
        stress_fail_msg("cond cancel objects create failed");
        if (state.token != NULL) {
            (void)llam_cancel_token_destroy(state.token);
        }
        if (state.cond != NULL) {
            llam_cond_destroy(state.cond);
        }
        if (state.mutex != NULL) {
            llam_mutex_destroy(state.mutex);
        }
        return;
    }

    waiter = llam_spawn(cond_cancel_waiter_task,
                      &state,
                      &(llam_spawn_opts_t){
                          .task_class = LLAM_TASK_CLASS_DEFAULT,
                          .stack_class = LLAM_STACK_CLASS_DEFAULT,
                          .cancel_token = state.token,
                      });
    trigger = llam_spawn(cond_cancel_trigger_task,
                       &state,
                       &(llam_spawn_opts_t){
                           .task_class = LLAM_TASK_CLASS_DEFAULT,
                           .stack_class = LLAM_STACK_CLASS_DEFAULT,
                       });
    if (waiter == NULL || trigger == NULL) {
        stress_fail_msg("cond cancel task spawn failed");
        if (waiter != NULL) {
            (void)llam_join(waiter);
        }
        if (trigger != NULL) {
            (void)llam_join(trigger);
        }
        (void)llam_cancel_token_destroy(state.token);
        llam_cond_destroy(state.cond);
        llam_mutex_destroy(state.mutex);
        return;
    }

    if (llam_join(waiter) != 0) {
        stress_fail_msg("cond cancel waiter join failed");
    }
    if (llam_join(trigger) != 0) {
        stress_fail_msg("cond cancel trigger join failed");
    }
    if (atomic_load(&state.triggered) != 1U) {
        stress_fail_u32("cond cancel trigger count", atomic_load(&state.triggered), 1U);
    }
    if (atomic_load(&state.cancelled) != 1U) {
        stress_fail_u32("cond cancel waiter count", atomic_load(&state.cancelled), 1U);
    }
    if (atomic_load(&state.reacquired) != 1U) {
        stress_fail_u32("cond cancel reacquired count", atomic_load(&state.reacquired), 1U);
    }
    (void)llam_cancel_token_destroy(state.token);
    llam_cond_destroy(state.cond);
    llam_mutex_destroy(state.mutex);
}

void run_channel_timeout_paths(void) {
    llam_channel_t *recv_channel = llam_channel_create(1U);
    llam_channel_t *send_channel = llam_channel_create(1U);
    void *value;

    if (recv_channel == NULL || send_channel == NULL) {
        stress_fail_msg("channel timeout create failed");
        if (recv_channel != NULL) {
            llam_channel_destroy(recv_channel);
        }
        if (send_channel != NULL) {
            llam_channel_destroy(send_channel);
        }
        return;
    }

    value = llam_channel_recv_until(recv_channel, llam_now_ns() + 1ULL * 1000ULL * 1000ULL);
    if (value != NULL) {
        stress_fail_msg("channel recv timeout unexpectedly received value");
    } else if (errno != ETIMEDOUT) {
        stress_fail_errno("channel recv timeout errno", errno, ETIMEDOUT);
    }

    if (llam_channel_send(send_channel, (void *)(intptr_t)1) != 0) {
        stress_fail_msg("channel timeout prefill failed");
    } else if (llam_channel_send_until(send_channel, (void *)(intptr_t)2, llam_now_ns() + 1ULL * 1000ULL * 1000ULL) == 0) {
        stress_fail_msg("channel send timeout unexpectedly succeeded");
    } else if (errno != ETIMEDOUT) {
        stress_fail_errno("channel send timeout errno", errno, ETIMEDOUT);
    }

    llam_channel_destroy(recv_channel);
    llam_channel_destroy(send_channel);
}

void run_dynamic_join_timeout_path(void) {
    uint64_t sleep_ns = 20ULL * 1000ULL * 1000ULL;
    llam_task_t *target = llam_spawn(stress_sleep_task,
                                 &sleep_ns,
                                 &(llam_spawn_opts_t){
                                     .task_class = LLAM_TASK_CLASS_DEFAULT,
                                     .stack_class = LLAM_STACK_CLASS_DEFAULT,
                                 });

    if (target == NULL) {
        stress_fail_msg("dynamic join timeout target spawn failed");
        return;
    }
    if (llam_join_until(target, llam_now_ns() + 4ULL * 1000ULL * 1000ULL) == 0) {
        stress_fail_msg("dynamic join timeout unexpectedly succeeded");
    } else if (errno != ETIMEDOUT) {
        stress_fail_errno("dynamic join timeout errno", errno, ETIMEDOUT);
    }
    if (llam_join(target) != 0) {
        stress_fail_msg("dynamic join timeout target join failed");
    }
}

void run_dynamic_mutex_timeout_path(void) {
    mutex_timeout_state_t state;
    llam_task_t *holder;

    state.mutex = llam_mutex_create();
    state.hold_ns = 20ULL * 1000ULL * 1000ULL;
    if (state.mutex == NULL) {
        stress_fail_msg("dynamic mutex timeout create failed");
        return;
    }

    holder = llam_spawn(mutex_holder_task,
                      &state,
                      &(llam_spawn_opts_t){
                          .task_class = LLAM_TASK_CLASS_DEFAULT,
                          .stack_class = LLAM_STACK_CLASS_DEFAULT,
                      });
    if (holder == NULL) {
        stress_fail_msg("dynamic mutex timeout holder spawn failed");
        llam_mutex_destroy(state.mutex);
        return;
    }

    if (llam_sleep_ns(2ULL * 1000ULL * 1000ULL) != 0) {
        stress_fail_msg("dynamic mutex timeout sync sleep failed");
    }
    if (llam_mutex_lock_until(state.mutex, llam_now_ns() + 4ULL * 1000ULL * 1000ULL) == 0) {
        stress_fail_msg("dynamic mutex timeout unexpectedly succeeded");
        (void)llam_mutex_unlock(state.mutex);
    } else if (errno != ETIMEDOUT) {
        stress_fail_errno("dynamic mutex timeout errno", errno, ETIMEDOUT);
    }
    if (llam_join(holder) != 0) {
        stress_fail_msg("dynamic mutex timeout holder join failed");
    }
    llam_mutex_destroy(state.mutex);
}

void run_dynamic_cond_timeout_path(void) {
    llam_mutex_t *mutex = llam_mutex_create();
    llam_cond_t *cond = llam_cond_create();
    int rc;
    int saved_errno;

    if (mutex == NULL || cond == NULL) {
        stress_fail_msg("dynamic cond timeout create failed");
        if (cond != NULL) {
            llam_cond_destroy(cond);
        }
        if (mutex != NULL) {
            llam_mutex_destroy(mutex);
        }
        return;
    }
    if (llam_mutex_lock(mutex) != 0) {
        stress_fail_msg("dynamic cond timeout lock failed");
        llam_cond_destroy(cond);
        llam_mutex_destroy(mutex);
        return;
    }
    rc = llam_cond_wait_until(cond, mutex, llam_now_ns() + 4ULL * 1000ULL * 1000ULL);
    saved_errno = errno;
    if (llam_mutex_unlock(mutex) != 0) {
        stress_fail_msg("dynamic cond timeout unlock failed");
        llam_cond_destroy(cond);
        llam_mutex_destroy(mutex);
        return;
    }
    if (rc == 0) {
        stress_fail_msg("dynamic cond timeout unexpectedly succeeded");
    } else if (saved_errno != ETIMEDOUT) {
        stress_fail_errno("dynamic cond timeout errno", saved_errno, ETIMEDOUT);
    }
    llam_cond_destroy(cond);
    llam_mutex_destroy(mutex);
}

void run_dynamic_channel_timeout_paths(void) {
    llam_channel_t *recv_channel = llam_channel_create(1U);
    llam_channel_t *send_channel = llam_channel_create(1U);
    void *value;

    if (recv_channel == NULL || send_channel == NULL) {
        stress_fail_msg("dynamic channel timeout create failed");
        if (recv_channel != NULL) {
            llam_channel_destroy(recv_channel);
        }
        if (send_channel != NULL) {
            llam_channel_destroy(send_channel);
        }
        return;
    }

    value = llam_channel_recv_until(recv_channel, llam_now_ns() + 4ULL * 1000ULL * 1000ULL);
    if (value != NULL) {
        stress_fail_msg("dynamic channel recv timeout unexpectedly received value");
    } else if (errno != ETIMEDOUT) {
        stress_fail_errno("dynamic channel recv timeout errno", errno, ETIMEDOUT);
    }

    if (llam_channel_send(send_channel, (void *)(intptr_t)1) != 0) {
        stress_fail_msg("dynamic channel timeout prefill failed");
    } else if (llam_channel_send_until(send_channel, (void *)(intptr_t)2, llam_now_ns() + 4ULL * 1000ULL * 1000ULL) == 0) {
        stress_fail_msg("dynamic channel send timeout unexpectedly succeeded");
    } else if (errno != ETIMEDOUT) {
        stress_fail_errno("dynamic channel send timeout errno", errno, ETIMEDOUT);
    }

    llam_channel_destroy(recv_channel);
    llam_channel_destroy(send_channel);
}

void run_dynamic_poll_paths(void) {
    int ready_sv[2];
    int timeout_sv[2];
    dynamic_poll_writer_state_t writer_state;
    llam_task_t *writer;
    short revents = 0;
    int rc;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ready_sv) != 0) {
        stress_fail_msg("dynamic poll ready socketpair failed");
        return;
    }
    writer_state.fd = ready_sv[1];
    writer_state.delay_ns = 2ULL * 1000ULL * 1000ULL;
    writer = llam_spawn(dynamic_poll_writer_task,
                      &writer_state,
                      &(llam_spawn_opts_t){
                          .task_class = LLAM_TASK_CLASS_DEFAULT,
                          .stack_class = LLAM_STACK_CLASS_DEFAULT,
                      });
    if (writer == NULL) {
        stress_fail_msg("dynamic poll writer spawn failed");
        close(ready_sv[0]);
        close(ready_sv[1]);
        return;
    }
    rc = llam_poll_fd(ready_sv[0], POLLIN, stress_platform_prefers_indefinite_ready_poll() ? -1 : 20, &revents);
    if (rc != 1 || (revents & POLLIN) == 0) {
        stress_fail_msg("dynamic poll ready path failed");
    }
    if (llam_join(writer) != 0) {
        stress_fail_msg("dynamic poll writer join failed");
    }
    close(ready_sv[0]);
    close(ready_sv[1]);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, timeout_sv) != 0) {
        stress_fail_msg("dynamic poll timeout socketpair failed");
        return;
    }
    revents = 0;
    rc = llam_poll_fd(timeout_sv[0], POLLIN, 4, &revents);
    if (rc != 0 || revents != 0) {
        stress_fail_msg("dynamic poll timeout path failed");
    }
    close(timeout_sv[0]);
    close(timeout_sv[1]);
}

void dynamic_idle_poll_watch_task(void *arg) {
    dynamic_idle_poll_watch_state_t *state = arg;
    dynamic_poll_writer_state_t writer_state;
    llam_task_t *writer;
    short revents = 0;
    char byte = 0;
    int rc;

    if (state == NULL) {
        stress_fail_msg("dynamic idle poll state missing");
        return;
    }
    if (!stress_runtime_supports_multishot_poll()) {
        stress_print_phase_skipped("dynamic_idle_poll_watch", "multishot poll unavailable");
        run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);
        return;
    }
    state->sv[0] = -1;
    state->sv[1] = -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, state->sv) != 0) {
        stress_fail_msg("dynamic idle poll socketpair failed");
        return;
    }

    writer_state.fd = state->sv[1];
    writer_state.delay_ns = 2ULL * 1000ULL * 1000ULL;
    writer = llam_spawn(dynamic_poll_writer_task,
                      &writer_state,
                      &(llam_spawn_opts_t){
                          .task_class = LLAM_TASK_CLASS_DEFAULT,
                          .stack_class = LLAM_STACK_CLASS_DEFAULT,
                      });
    if (writer == NULL) {
        stress_fail_msg("dynamic idle poll writer spawn failed");
        return;
    }

    rc = llam_poll_fd(state->sv[0], POLLIN, -1, &revents);
    if (rc != 1 || (revents & POLLIN) == 0) {
        stress_fail_msg("dynamic idle poll wait failed");
        return;
    }
    if (llam_read(state->sv[0], &byte, 1U) != 1 || byte != 'd') {
        stress_fail_msg("dynamic idle poll read failed");
        return;
    }
    if (llam_join(writer) != 0) {
        stress_fail_msg("dynamic idle poll writer join failed");
        return;
    }

    run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);
}

void dynamic_idle_recv_watch_task(void *arg) {
    static const char *payloads[] = {"alpha", "bravo", "charlie"};
    dynamic_idle_recv_watch_state_t *state = arg;
    llam_io_buffer_t *buffer = NULL;
    int sock_type = SOCK_SEQPACKET;
    ssize_t rc;
    size_t i;

    if (state == NULL) {
        stress_fail_msg("dynamic idle recv state missing");
        return;
    }
    if (!stress_runtime_supports_multishot_recv()) {
        stress_print_phase_skipped("dynamic_idle_recv_watch", "multishot recv unavailable");
        run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);
        return;
    }
    state->sv[0] = -1;
    state->sv[1] = -1;
#if defined(__APPLE__)
    sock_type = SOCK_DGRAM;
#endif
    if (socketpair(AF_UNIX, sock_type, 0, state->sv) != 0) {
        stress_fail_msg("dynamic idle recv socketpair failed");
        return;
    }

    for (i = 0; i < (sizeof(payloads) / sizeof(payloads[0])); ++i) {
        size_t len = strlen(payloads[i]);

        if (write(state->sv[1], payloads[i], len) != (ssize_t)len) {
            stress_fail_msg("dynamic idle recv write failed");
            goto cleanup;
        }
    }

    rc = llam_read_owned(state->sv[0], 16U, &buffer);
    if (rc != (ssize_t)strlen(payloads[0])) {
        stress_fail_msg("dynamic idle recv first rc failed");
        goto cleanup;
    }
    if (buffer == NULL || memcmp(llam_io_buffer_data(buffer), payloads[0], strlen(payloads[0])) != 0) {
        stress_fail_msg("dynamic idle recv first payload failed");
        goto cleanup;
    }
    llam_io_buffer_release(buffer);
    buffer = NULL;

    run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);

    for (i = 1U; i < (sizeof(payloads) / sizeof(payloads[0])); ++i) {
        size_t len = strlen(payloads[i]);

        rc = llam_read_owned(state->sv[0], 16U, &buffer);
        if (rc != (ssize_t)len) {
            stress_fail_msg("dynamic idle recv follow rc failed");
            goto cleanup;
        }
        if (buffer == NULL || memcmp(llam_io_buffer_data(buffer), payloads[i], len) != 0) {
            stress_fail_msg("dynamic idle recv follow payload failed");
            goto cleanup;
        }
        llam_io_buffer_release(buffer);
        buffer = NULL;
    }

cleanup:
    llam_io_buffer_release(buffer);
    stress_close_fd_pair(state->sv);
}

void dynamic_idle_accept_watch_task(void *arg) {
    dynamic_idle_accept_watch_state_t *state = arg;
    dynamic_accept_connector_state_t connectors[3];
    llam_task_t *connector_tasks[3] = {NULL, NULL, NULL};
    static const uint64_t connector_delays_ns[] = {
        2ULL * 1000ULL * 1000ULL,
        4ULL * 1000ULL * 1000ULL,
        6ULL * 1000ULL * 1000ULL,
    };
    int accepted_fd = -1;
    size_t spawned = 0U;
    size_t i;

    if (state == NULL) {
        stress_fail_msg("dynamic idle accept state missing");
        return;
    }
    if (!stress_runtime_supports_multishot_accept()) {
        stress_print_phase_skipped("dynamic_idle_accept_watch", "multishot accept unavailable");
        run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);
        return;
    }
    state->listener_fd = stress_create_loopback_listener(&state->port);
    if (state->listener_fd < 0) {
        stress_fail_msg("dynamic idle accept listener create failed");
        return;
    }

    memset(connectors, 0, sizeof(connectors));
    for (i = 0; i < (sizeof(connectors) / sizeof(connectors[0])); ++i) {
        connectors[i].port = state->port;
        connectors[i].delay_ns = connector_delays_ns[i];
        connector_tasks[i] = llam_spawn(dynamic_accept_connector_task,
                                      &connectors[i],
                                      &(llam_spawn_opts_t){
                                          .task_class = LLAM_TASK_CLASS_DEFAULT,
                                          .stack_class = LLAM_STACK_CLASS_DEFAULT,
                                      });
        if (connector_tasks[i] == NULL) {
            stress_fail_msg("dynamic idle accept connector spawn failed");
            goto cleanup;
        }
        spawned += 1U;
    }

    accepted_fd = llam_accept(state->listener_fd, NULL, NULL);
    if (accepted_fd < 0) {
        stress_fail_msg("dynamic idle accept first accept failed");
        goto cleanup;
    }
    close(accepted_fd);
    accepted_fd = -1;

    run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);

    for (i = 1U; i < (sizeof(connectors) / sizeof(connectors[0])); ++i) {
        accepted_fd = llam_accept(state->listener_fd, NULL, NULL);
        if (accepted_fd < 0) {
            stress_fail_msg("dynamic idle accept follow accept failed");
            goto cleanup;
        }
        close(accepted_fd);
        accepted_fd = -1;
    }

cleanup:
    if (accepted_fd >= 0) {
        close(accepted_fd);
    }
    for (i = 0; i < spawned; ++i) {
        if (llam_join(connector_tasks[i]) != 0) {
            stress_fail_msg("dynamic idle accept connector join failed");
        }
    }
    if (state->listener_fd >= 0) {
        close(state->listener_fd);
        state->listener_fd = -1;
    }
}
