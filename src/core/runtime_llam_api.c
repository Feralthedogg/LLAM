/**
 * @file src/core/runtime_llam_api.c
 * @brief Canonical llam_* API wrappers and translation to the internal nm_* runtime implementation.
 *
 * @details
 * The implementation originally exposed the @c nm_* API. The canonical public
 * API now uses @c llam_* names, but the underlying runtime types and behavior
 * are still shared. This file performs narrow type/option/stat translations and
 * forwards every call to the internal implementation without duplicating
 * scheduler logic.
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

#include "llam/runtime.h"

#include "llam/nm_runtime.h"

#include <string.h>

/**
 * @brief Translate public LLAM spawn options into internal nm options.
 *
 * @param opts Public spawn options, or NULL.
 * @return Zero-initialized internal option struct with matching fields copied.
 */
static nm_spawn_opts_t nm_spawn_opts_from_llam(const llam_spawn_opts_t *opts) {
    nm_spawn_opts_t out;

    memset(&out, 0, sizeof(out));
    if (opts == NULL) {
        return out;
    }
    out.task_class = (nm_task_class_t)opts->task_class;
    out.stack_class = (nm_stack_class_t)opts->stack_class;
    out.flags = opts->flags;
    out.deadline_ns = opts->deadline_ns;
    out.cancel_token = (nm_cancel_token_t *)opts->cancel_token;
    return out;
}

/**
 * @brief Translate public LLAM runtime options into internal nm options.
 *
 * @param opts Public runtime options, or NULL.
 * @return Zero-initialized internal option struct with matching fields copied.
 */
static nm_runtime_opts_t nm_runtime_opts_from_llam(const llam_runtime_opts_t *opts) {
    nm_runtime_opts_t out;

    memset(&out, 0, sizeof(out));
    if (opts == NULL) {
        return out;
    }
    out.deterministic = opts->deterministic;
    out.forced_yield_every = opts->forced_yield_every;
    out.experimental_shard_rings = opts->experimental_worker_rings;
    out.experimental_shard_rings_multishot = opts->experimental_worker_rings_multishot;
    out.experimental_dynamic_shards = opts->experimental_dynamic_workers;
    out.experimental_lockfree_normq = opts->experimental_lockfree_normq;
    out.experimental_huge_alloc = opts->experimental_huge_alloc;
    out.idle_spin_ns = opts->idle_spin_ns;
    out.idle_spin_max_iters = opts->idle_spin_max_iters;
    out.experimental_sqpoll = opts->experimental_sqpoll;
    out.sqpoll_cpu = opts->sqpoll_cpu;
    out.profile = (nm_runtime_profile_t)opts->profile;
    return out;
}

/**
 * @brief Translate internal runtime stats into the canonical public shape.
 *
 * @param out   Public stats destination.
 * @param stats Internal stats source.
 */
static void llam_runtime_stats_from_nm(llam_runtime_stats_t *out, const nm_runtime_stats_t *stats) {
    memset(out, 0, sizeof(*out));
    out->ctx_switches = stats->ctx_switches;
    out->yields = stats->yields;
    out->parks = stats->parks;
    out->wakes = stats->wakes;
    out->steals = stats->steals;
    out->migrations = stats->migrations;
    out->blocking_calls = stats->blocking_calls;
    out->blocking_completions = stats->blocking_completions;
    out->io_submits = stats->io_submits;
    out->io_submit_calls = stats->io_submit_calls;
    out->io_submit_syscalls = stats->io_submit_syscalls;
    out->io_completions = stats->io_completions;
    out->idle_polls = stats->idle_polls;
    out->idle_spin_loops = stats->idle_spin_loops;
    out->idle_spin_hits = stats->idle_spin_hits;
    out->idle_spin_fallbacks = stats->idle_spin_fallbacks;
    out->idle_spin_ns = stats->idle_spin_ns;
    out->queue_overflows = stats->queue_overflows;
    out->overflow_depth = stats->overflow_depth;
    out->active_workers = stats->active_shards;
    out->online_workers = stats->online_shards;
    out->online_workers_floor = stats->online_shards_floor;
    out->online_workers_min = stats->online_shards_min;
    out->online_workers_max = stats->online_shards_max;
    out->active_nodes = stats->active_nodes;
    out->dynamic_workers = stats->dynamic_shards;
    out->worker_rings = stats->shard_rings;
    out->worker_rings_multishot = stats->shard_rings_multishot;
    out->lockfree_normq = stats->lockfree_normq;
    out->huge_alloc = stats->huge_alloc;
    out->sqpoll = stats->sqpoll;
    out->opaque_block_ns = stats->opaque_block_ns;
    out->opaque_block_samples = stats->opaque_block_samples;
    out->opaque_block_max_ns = stats->opaque_block_max_ns;
    out->opaque_enter_wait_ns = stats->opaque_enter_wait_ns;
    out->opaque_enter_wait_samples = stats->opaque_enter_wait_samples;
    out->opaque_enter_wait_max_ns = stats->opaque_enter_wait_max_ns;
    out->opaque_leave_wait_ns = stats->opaque_leave_wait_ns;
    out->opaque_leave_wait_samples = stats->opaque_leave_wait_samples;
    out->opaque_leave_wait_max_ns = stats->opaque_leave_wait_max_ns;
}

/* Runtime lifecycle and task scheduling wrappers. */

int llam_runtime_init(const llam_runtime_opts_t *opts) {
    nm_runtime_opts_t nm_opts;

    if (opts == NULL) {
        return nm_runtime_init(NULL);
    }
    nm_opts = nm_runtime_opts_from_llam(opts);
    return nm_runtime_init(&nm_opts);
}

void llam_runtime_shutdown(void) {
    nm_runtime_shutdown();
}

int llam_runtime_collect_stats(llam_runtime_stats_t *stats) {
    nm_runtime_stats_t nm_stats;
    int rc;

    if (stats == NULL) {
        return nm_runtime_collect_stats(NULL);
    }
    rc = nm_runtime_collect_stats(&nm_stats);
    if (rc == 0) {
        llam_runtime_stats_from_nm(stats, &nm_stats);
    }
    return rc;
}

llam_task_t *llam_spawn(llam_task_fn fn, void *arg, const llam_spawn_opts_t *opts) {
    nm_spawn_opts_t nm_opts;

    if (opts == NULL) {
        return (llam_task_t *)nm_spawn((nm_task_fn)fn, arg, NULL);
    }
    nm_opts = nm_spawn_opts_from_llam(opts);
    return (llam_task_t *)nm_spawn((nm_task_fn)fn, arg, &nm_opts);
}

int llam_run(void) {
    return nm_run();
}

void llam_yield(void) {
    nm_yield();
}

int llam_join(llam_task_t *task) {
    return nm_join((nm_task_t *)task);
}

int llam_join_until(llam_task_t *task, uint64_t deadline_ns) {
    return nm_join_until((nm_task_t *)task, deadline_ns);
}

int llam_sleep_until(uint64_t deadline_ns) {
    return nm_sleep_until(deadline_ns);
}

int llam_sleep_ns(uint64_t duration_ns) {
    return nm_sleep_ns(duration_ns);
}

void *llam_call_blocking(llam_blocking_fn fn, void *arg) {
    return nm_call_blocking((nm_blocking_fn)fn, arg);
}

int llam_enter_blocking(void) {
    return nm_enter_blocking();
}

int llam_leave_blocking(void) {
    return nm_leave_blocking();
}

void llam_task_set_class(llam_task_class_t task_class) {
    nm_task_set_class((nm_task_class_t)task_class);
}

void llam_dump_runtime_state(int fd) {
    nm_dump_runtime_state(fd);
}

unsigned llam_task_flags(const llam_task_t *task) {
    return nm_task_flags((const nm_task_t *)task);
}

/* Cancellation-token wrappers. */

llam_cancel_token_t *llam_cancel_token_create(void) {
    return (llam_cancel_token_t *)nm_cancel_token_create();
}

int llam_cancel_token_destroy(llam_cancel_token_t *token) {
    return nm_cancel_token_destroy((nm_cancel_token_t *)token);
}

int llam_cancel_token_cancel(llam_cancel_token_t *token) {
    return nm_cancel_token_cancel((nm_cancel_token_t *)token);
}

int llam_cancel_token_is_cancelled(const llam_cancel_token_t *token) {
    return nm_cancel_token_is_cancelled((const nm_cancel_token_t *)token);
}

/* Runtime-aware mutex wrappers. */

llam_mutex_t *llam_mutex_create(void) {
    return (llam_mutex_t *)nm_mutex_create();
}

void llam_mutex_destroy(llam_mutex_t *mutex) {
    nm_mutex_destroy((nm_mutex_t *)mutex);
}

int llam_mutex_lock(llam_mutex_t *mutex) {
    return nm_mutex_lock((nm_mutex_t *)mutex);
}

int llam_mutex_lock_until(llam_mutex_t *mutex, uint64_t deadline_ns) {
    return nm_mutex_lock_until((nm_mutex_t *)mutex, deadline_ns);
}

int llam_mutex_trylock(llam_mutex_t *mutex) {
    return nm_mutex_trylock((nm_mutex_t *)mutex);
}

int llam_mutex_unlock(llam_mutex_t *mutex) {
    return nm_mutex_unlock((nm_mutex_t *)mutex);
}

/* Runtime-aware condition-variable wrappers. */

llam_cond_t *llam_cond_create(void) {
    return (llam_cond_t *)nm_cond_create();
}

void llam_cond_destroy(llam_cond_t *cond) {
    nm_cond_destroy((nm_cond_t *)cond);
}

int llam_cond_wait(llam_cond_t *cond, llam_mutex_t *mutex) {
    return nm_cond_wait((nm_cond_t *)cond, (nm_mutex_t *)mutex);
}

int llam_cond_wait_until(llam_cond_t *cond, llam_mutex_t *mutex, uint64_t deadline_ns) {
    return nm_cond_wait_until((nm_cond_t *)cond, (nm_mutex_t *)mutex, deadline_ns);
}

int llam_cond_signal(llam_cond_t *cond) {
    return nm_cond_signal((nm_cond_t *)cond);
}

int llam_cond_broadcast(llam_cond_t *cond) {
    return nm_cond_broadcast((nm_cond_t *)cond);
}

/* Channel wrappers. */

llam_channel_t *llam_channel_create(size_t capacity) {
    return (llam_channel_t *)nm_channel_create(capacity);
}

void llam_channel_destroy(llam_channel_t *channel) {
    nm_channel_destroy((nm_channel_t *)channel);
}

int llam_channel_send(llam_channel_t *channel, void *value) {
    return nm_channel_send((nm_channel_t *)channel, value);
}

int llam_channel_send_until(llam_channel_t *channel, void *value, uint64_t deadline_ns) {
    return nm_channel_send_until((nm_channel_t *)channel, value, deadline_ns);
}

void *llam_channel_recv(llam_channel_t *channel) {
    return nm_channel_recv((nm_channel_t *)channel);
}

void *llam_channel_recv_until(llam_channel_t *channel, uint64_t deadline_ns) {
    return nm_channel_recv_until((nm_channel_t *)channel, deadline_ns);
}

int llam_channel_close(llam_channel_t *channel) {
    return nm_channel_close((nm_channel_t *)channel);
}

/* Runtime I/O wrappers. */

ssize_t llam_read(llam_fd_t fd, void *buf, size_t count) {
    return nm_read((nm_fd_t)fd, buf, count);
}

ssize_t llam_write(llam_fd_t fd, const void *buf, size_t count) {
    return nm_write((nm_fd_t)fd, buf, count);
}

ssize_t llam_read_owned(llam_fd_t fd, size_t max_count, llam_io_buffer_t **out) {
    nm_io_buffer_t *buffer = NULL;
    ssize_t rc = nm_read_owned((nm_fd_t)fd, max_count, out != NULL ? &buffer : NULL);

    if (out != NULL) {
        // Ownership transfers through the same buffer object; only the public
        // handle type changes.
        *out = (llam_io_buffer_t *)buffer;
    }
    return rc;
}

ssize_t llam_recv_owned(llam_fd_t fd, size_t max_count, int flags, llam_io_buffer_t **out) {
    nm_io_buffer_t *buffer = NULL;
    ssize_t rc = nm_recv_owned((nm_fd_t)fd, max_count, flags, out != NULL ? &buffer : NULL);

    if (out != NULL) {
        // Keep NULL propagation identical to nm_recv_owned().
        *out = (llam_io_buffer_t *)buffer;
    }
    return rc;
}

void llam_io_buffer_release(llam_io_buffer_t *buffer) {
    nm_io_buffer_release((nm_io_buffer_t *)buffer);
}

void *llam_io_buffer_data(llam_io_buffer_t *buffer) {
    return nm_io_buffer_data((nm_io_buffer_t *)buffer);
}

size_t llam_io_buffer_size(const llam_io_buffer_t *buffer) {
    return nm_io_buffer_size((const nm_io_buffer_t *)buffer);
}

size_t llam_io_buffer_capacity(const llam_io_buffer_t *buffer) {
    return nm_io_buffer_capacity((const nm_io_buffer_t *)buffer);
}

llam_fd_t llam_accept(llam_fd_t fd, struct sockaddr *addr, socklen_t *addrlen) {
    return (llam_fd_t)nm_accept((nm_fd_t)fd, addr, addrlen);
}

int llam_connect(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen) {
    return nm_connect((nm_fd_t)fd, addr, addrlen);
}

int llam_poll_fd(llam_fd_t fd, short events, int timeout_ms, short *revents) {
    return nm_poll_fd((nm_fd_t)fd, events, timeout_ms, revents);
}

/* Time and task-introspection wrappers. */

uint64_t llam_now_ns(void) {
    return nm_now_ns();
}

uint64_t llam_task_id(const llam_task_t *task) {
    return nm_task_id((const nm_task_t *)task);
}

const char *llam_task_state_name(const llam_task_t *task) {
    return nm_task_state_name((const nm_task_t *)task);
}

llam_task_class_t llam_task_class(const llam_task_t *task) {
    return (llam_task_class_t)nm_task_class((const nm_task_t *)task);
}

llam_task_t *llam_current_task(void) {
    return (llam_task_t *)nm_current_task();
}
