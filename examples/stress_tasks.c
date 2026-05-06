/**
 * @file examples/stress_tasks.c
 * @brief Reusable stress-test task bodies for scheduler, sync, blocking, and I/O coverage.
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

void storm_child_task(void *arg) {
    storm_state_t *state = arg;
    unsigned i;

    for (i = 0; i < state->yields_per_task; ++i) {
        nm_yield();
    }
    atomic_fetch_add(&state->completed, 1U);
}

void ping_peer_task(void *arg) {
    ping_state_t *state = arg;
    unsigned i;

    for (i = 0; i < state->rounds; ++i) {
        void *value = nm_channel_recv(state->request);

        if (value == NULL) {
            stress_fail_msg("ping-peer recv returned NULL");
            return;
        }
        atomic_fetch_add(&state->echoed, 1U);
        if (nm_channel_send(state->response, value) != 0) {
            stress_fail_msg("ping-peer send failed");
            return;
        }
    }
}

void convoy_worker_task(void *arg) {
    convoy_state_t *state = arg;
    unsigned i;

    for (i = 0; i < state->iterations; ++i) {
        if (nm_mutex_lock(state->mutex) != 0) {
            stress_fail_msg("convoy mutex lock failed");
            return;
        }
        state->counter += 1U;
        if (nm_mutex_unlock(state->mutex) != 0) {
            stress_fail_msg("convoy mutex unlock failed");
            return;
        }
        if ((i & 3U) == 0U) {
            nm_yield();
        }
    }
}

void cancel_waiter_task(void *arg) {
    (void)arg;

    if (nm_sleep_ns(50ULL * 1000ULL * 1000ULL) != 0) {
        if (errno == ECANCELED) {
            atomic_fetch_add(&((cancel_state_t *)arg)->cancelled, 1U);
            return;
        }
        stress_fail_msg("cancel waiter woke with wrong errno");
        return;
    }
    stress_fail_msg("cancel waiter unexpectedly completed sleep");
}

void cancel_trigger_task(void *arg) {
    cancel_state_t *state = arg;

    nm_sleep_ns(1ULL * 1000ULL * 1000ULL);
    if (nm_cancel_token_cancel(state->token) != 0) {
        stress_fail_msg("cancel trigger failed");
        return;
    }
    atomic_fetch_add(&state->triggered, 1U);
}

void io_cancel_waiter_task(void *arg) {
    io_cancel_state_t *state = arg;
    char byte = 0;
    ssize_t rc = nm_read(state->reader_fd, &byte, 1U);

    if (rc < 0) {
        if (errno == ECANCELED) {
            atomic_fetch_add(&state->cancelled, 1U);
            return;
        }
        stress_fail_msg("io cancel waiter woke with wrong errno");
        return;
    }
    stress_fail_msg("io cancel waiter unexpectedly completed read");
}

void io_cancel_trigger_task(void *arg) {
    io_cancel_state_t *state = arg;

    nm_sleep_ns(1ULL * 1000ULL * 1000ULL);
    if (nm_cancel_token_cancel(state->token) != 0) {
        stress_fail_msg("io cancel trigger failed");
        return;
    }
    atomic_fetch_add(&state->triggered, 1U);
}

void owned_read_writer_task(void *arg) {
    owned_read_state_t *state = arg;
    ssize_t rc;

    if (state->delay_ns > 0U && nm_sleep_ns(state->delay_ns) != 0) {
        stress_fail_msg("owned read writer sleep failed");
        return;
    }
    rc = write(state->fd, state->payload, state->len);
    if (rc != (ssize_t)state->len) {
        stress_fail_msg("owned read writer write failed");
    }
}

void dynamic_sleep_child_task(void *arg) {
    dynamic_sleep_child_state_t *state = arg;
    unsigned yields = state->wave->base_yields + state->extra_yields;
    unsigned i;

    for (i = 0; i < yields; ++i) {
        nm_yield();
    }
    if (nm_sleep_ns(state->wave->sleep_ns) != 0) {
        stress_fail_msg("dynamic sleep child sleep failed");
        return;
    }
    atomic_fetch_add(&state->wave->completed, 1U);
}

void *stress_blocking_pause(void *arg) {
    (void)arg;
    usleep(10U * 1000U);
    return arg;
}

int stress_connect_loopback(dynamic_accept_connector_state_t *state) {
    struct sockaddr_in addr;
    int fd;
    int rc;

    if (state == NULL) {
        errno = EINVAL;
        return -1;
    }

    state->result = 0;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        state->result = errno != 0 ? errno : EIO;
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(state->port);
    rc = nm_connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0) {
        state->result = errno != 0 ? errno : EIO;
    }
    close(fd);
    return rc;
}

void block_cancel_waiter_task(void *arg) {
    block_cancel_state_t *state = arg;

    if (nm_call_blocking(stress_blocking_pause, NULL) == NULL) {
        if (errno == ECANCELED) {
            atomic_fetch_add(&state->cancelled, 1U);
            return;
        }
        stress_fail_msg("block cancel waiter woke with wrong errno");
        return;
    }
    stress_fail_msg("block cancel waiter unexpectedly completed");
}

void block_cancel_trigger_task(void *arg) {
    block_cancel_state_t *state = arg;

    nm_sleep_ns(1ULL * 1000ULL * 1000ULL);
    if (nm_cancel_token_cancel(state->token) != 0) {
        stress_fail_msg("block cancel trigger failed");
        return;
    }
    atomic_fetch_add(&state->triggered, 1U);
}

void stress_sleep_task(void *arg) {
    uint64_t duration_ns = *(uint64_t *)arg;

    if (nm_sleep_ns(duration_ns) != 0) {
        stress_fail_msg("sleep task failed");
    }
}

void mutex_holder_task(void *arg) {
    mutex_timeout_state_t *state = arg;

    if (nm_mutex_lock(state->mutex) != 0) {
        stress_fail_msg("mutex holder lock failed");
        return;
    }
    if (nm_sleep_ns(state->hold_ns) != 0) {
        stress_fail_msg("mutex holder sleep failed");
    }
    if (nm_mutex_unlock(state->mutex) != 0) {
        stress_fail_msg("mutex holder unlock failed");
    }
}

void cond_cancel_waiter_task(void *arg) {
    cond_cancel_state_t *state = arg;
    int rc;
    int saved_errno = 0;

    if (nm_mutex_lock(state->mutex) != 0) {
        stress_fail_msg("cond cancel waiter lock failed");
        return;
    }
    rc = nm_cond_wait(state->cond, state->mutex);
    saved_errno = errno;
    if (nm_mutex_unlock(state->mutex) != 0) {
        stress_fail_msg("cond cancel waiter unlock failed");
        return;
    }
    if (rc != 0) {
        if (saved_errno == ECANCELED) {
            atomic_fetch_add(&state->cancelled, 1U);
            atomic_fetch_add(&state->reacquired, 1U);
            return;
        }
        stress_fail_errno("cond cancel waiter errno", saved_errno, ECANCELED);
        return;
    }
    stress_fail_msg("cond cancel waiter unexpectedly completed");
}

void cond_cancel_trigger_task(void *arg) {
    cond_cancel_state_t *state = arg;

    nm_sleep_ns(1ULL * 1000ULL * 1000ULL);
    if (nm_cancel_token_cancel(state->token) != 0) {
        stress_fail_msg("cond cancel trigger failed");
        return;
    }
    atomic_fetch_add(&state->triggered, 1U);
}

void poll_writer_task(void *arg) {
    int fd = *(int *)arg;
    char byte = 'x';
    ssize_t rc;

    nm_sleep_ns(1ULL * 1000ULL * 1000ULL);
    rc = write(fd, &byte, 1U);
    if (rc != 1) {
        stress_fail_msg("poll writer failed");
    }
}

void dynamic_poll_writer_task(void *arg) {
    dynamic_poll_writer_state_t *state = arg;
    char byte = 'd';
    ssize_t rc;

    if (state->delay_ns > 0U && nm_sleep_ns(state->delay_ns) != 0) {
        stress_fail_msg("dynamic poll writer sleep failed");
        return;
    }
    rc = write(state->fd, &byte, 1U);
    if (rc != 1) {
        stress_fail_msg("dynamic poll writer failed");
    }
}

unsigned stress_dynamic_live_wait_floor(const nm_runtime_stats_t *stats) {
    unsigned floor;

    if (stats == NULL) {
        return 0U;
    }
    floor = stats->online_shards_floor;
    if (stats->active_shards > 8U) {
        unsigned io_floor = stats->active_shards - (stats->active_shards / 4U);

        if (io_floor > floor) {
            floor = io_floor;
        }
    }
    return floor;
}

void dynamic_live_poll_waiter_task(void *arg) {
    dynamic_live_poll_waiter_state_t *state = arg;
    short revents = 0;
    int rc;

    if (state == NULL) {
        stress_fail_msg("dynamic live poll waiter state missing");
        return;
    }

    rc = nm_poll_fd(state->fd, POLLIN, -1, &revents);
    if (rc != 1 || (revents & (POLLIN | POLLHUP)) == 0) {
        stress_fail_msg("dynamic live poll waiter failed");
        return;
    }
    atomic_fetch_add(state->completed, 1U);
}

void dynamic_live_inflight_waiter_task(void *arg) {
    dynamic_live_inflight_waiter_state_t *state = arg;
    char byte = '\0';
    ssize_t rc;

    if (state == NULL) {
        stress_fail_msg("dynamic live inflight waiter state missing");
        return;
    }

    rc = nm_read(state->fd, &byte, 1U);
    if (rc != 1 || byte != 'i') {
        stress_fail_msg("dynamic live inflight waiter failed");
        return;
    }
    atomic_fetch_add(state->completed, 1U);
}

void dynamic_live_accept_waiter_task(void *arg) {
    dynamic_live_accept_waiter_state_t *state = arg;
    int fd;

    if (state == NULL) {
        stress_fail_msg("dynamic live accept waiter state missing");
        return;
    }

    fd = nm_accept(state->listener_fd, NULL, NULL);
    if (fd < 0) {
        stress_fail_msg("dynamic live accept waiter failed");
        return;
    }
    close(fd);
    atomic_fetch_add(state->completed, 1U);
}

void dynamic_accept_connector_task(void *arg) {
    dynamic_accept_connector_state_t *state = arg;

    if (state == NULL) {
        stress_fail_msg("dynamic accept connector state missing");
        return;
    }
    if (state->delay_ns > 0U && nm_sleep_ns(state->delay_ns) != 0) {
        stress_fail_msg("dynamic accept connector sleep failed");
        return;
    }
    if (stress_connect_loopback(state) != 0) {
        stress_fail_msg("dynamic accept connector connect failed");
        return;
    }
    if (state->result != 0) {
        stress_fail_errno("dynamic accept connector errno", state->result, 0);
        return;
    }
    if (state->completed != NULL) {
        atomic_fetch_add(state->completed, 1U);
    }
}

void fp_round_task(void *arg) {
    fp_round_state_t *state = arg;
    unsigned i;

    stress_set_fp_round(state->mode);
    for (i = 0; i < state->yields; ++i) {
        if (stress_fp_round_mode() != state->mode) {
            stress_fail_u32("fp isolation mode", stress_fp_round_mode(), state->mode);
            return;
        }
        nm_yield();
    }
    if (stress_fp_round_mode() != state->mode) {
        stress_fail_u32("fp isolation final mode", stress_fp_round_mode(), state->mode);
        return;
    }
    atomic_fetch_add(&state->completed, 1U);
}

void fp_inherit_child_task(void *arg) {
    fp_inherit_state_t *state = arg;
    unsigned observed = stress_fp_round_mode();

    atomic_store(&state->observed_mode, observed);
    if (observed != state->expected_mode) {
        stress_fail_u32("fp inherit child mode", observed, state->expected_mode);
        return;
    }
    stress_set_fp_round((state->expected_mode + 1U) & 3U);
    nm_yield();
    atomic_fetch_add(&state->completed, 1U);
}

void poll_cancel_race_waiter_task(void *arg) {
    poll_cancel_race_state_t *state = arg;
    short revents = 0;
    int rc = nm_poll_fd(state->fd, POLLIN, 1, &revents);

    if (rc == 0 && revents == 0) {
        atomic_fetch_add(&state->timeout_hits, 1U);
        return;
    }
    if (rc < 0 && errno == ECANCELED) {
        atomic_fetch_add(&state->cancel_hits, 1U);
        return;
    }
    stress_fail_msg("poll cancel-timeout waiter returned unexpected result");
}

void poll_cancel_race_trigger_task(void *arg) {
    poll_cancel_race_state_t *state = arg;

    nm_sleep_ns(1ULL * 1000ULL * 1000ULL);
    if (nm_cancel_token_cancel(state->token) != 0) {
        stress_fail_msg("poll cancel-timeout trigger failed");
        return;
    }
    atomic_fetch_add(&state->triggered, 1U);
}

void opaque_companion_task(void *arg) {
    opaque_state_t *state = arg;
    unsigned i;

    for (i = 0; i < 4U; ++i) {
        atomic_fetch_add(&state->companion_steps, 1U);
        nm_yield();
    }
}

void nested_opaque_companion_task(void *arg) {
    nested_opaque_state_t *state = arg;
    unsigned i;

    for (i = 0; i < 4U; ++i) {
        atomic_fetch_add(&state->companion_steps, 1U);
        nm_yield();
    }
}

void opaque_scope_task(void *arg) {
    opaque_state_t *state = arg;
    unsigned i;

    for (i = 0; i < state->scopes; ++i) {
        nm_task_t *companion = nm_spawn(nested_opaque_companion_task,
                                        state,
                                        &(nm_spawn_opts_t){
                                            .task_class = NM_TASK_CLASS_DEFAULT,
                                            .stack_class = NM_STACK_CLASS_DEFAULT,
                                        });

        if (companion == NULL) {
            stress_fail_msg("opaque companion spawn failed");
            return;
        }
        if (nm_enter_blocking() != 0) {
            stress_fail_msg("opaque enter blocking failed");
            return;
        }
        usleep(2U * 1000U);
        if (nm_leave_blocking() != 0) {
            stress_fail_msg("opaque leave blocking failed");
            return;
        }
        if (nm_join(companion) != 0) {
            stress_fail_msg("opaque companion join failed");
            return;
        }
        nm_yield();
    }
}

void nested_opaque_scope_task(void *arg) {
    nested_opaque_state_t *state = arg;
    unsigned i;

    for (i = 0; i < state->scopes; ++i) {
        nm_task_t *companion = nm_spawn(opaque_companion_task,
                                        state,
                                        &(nm_spawn_opts_t){
                                            .task_class = NM_TASK_CLASS_DEFAULT,
                                            .stack_class = NM_STACK_CLASS_DEFAULT,
                                        });

        if (companion == NULL) {
            stress_fail_msg("nested opaque companion spawn failed");
            return;
        }
        if (nm_enter_blocking() != 0) {
            stress_fail_msg("nested opaque enter failed");
            return;
        }
        if (nm_enter_blocking() != 0) {
            stress_fail_msg("nested opaque re-enter failed");
            return;
        }
        usleep(1000U);
        if (nm_leave_blocking() != 0) {
            stress_fail_msg("nested opaque inner leave failed");
            return;
        }
        usleep(1000U);
        if (nm_leave_blocking() != 0) {
            stress_fail_msg("nested opaque outer leave failed");
            return;
        }
        if (nm_join(companion) != 0) {
            stress_fail_msg("nested opaque companion join failed");
            return;
        }
        atomic_fetch_add(&state->completed_scopes, 1U);
        nm_yield();
    }
}
