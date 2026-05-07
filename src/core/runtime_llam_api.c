/**
 * @file src/core/runtime_llam_api.c
 * @brief Legacy nm_* compatibility wrappers around the canonical llam_* runtime API.
 *
 * @details
 * The implementation namespace is LLAM-native. This file is the compatibility
 * boundary for the historical @c nm_* public API, translating legacy option and
 * statistics structs where names differ while forwarding behavior to @c llam_*.
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

#include "llam/nm_runtime.h"

#include "llam/runtime.h"

#include <errno.h>
#include <string.h>

/** @brief Translate legacy spawn options into canonical LLAM options. */
static llam_spawn_opts_t llam_spawn_opts_from_nm(const nm_spawn_opts_t *opts) {
    llam_spawn_opts_t out;

    memset(&out, 0, sizeof(out));
    if (opts == NULL) {
        return out;
    }
    out.task_class = (llam_task_class_t)opts->task_class;
    out.stack_class = (llam_stack_class_t)opts->stack_class;
    out.flags = opts->flags;
    out.deadline_ns = opts->deadline_ns;
    out.cancel_token = (llam_cancel_token_t *)opts->cancel_token;
    return out;
}

/** @brief Translate legacy runtime options into canonical LLAM options. */
static llam_runtime_opts_t llam_runtime_opts_from_nm(const nm_runtime_opts_t *opts) {
    llam_runtime_opts_t out;

    memset(&out, 0, sizeof(out));
    if (opts == NULL) {
        return out;
    }
    out.deterministic = opts->deterministic;
    out.forced_yield_every = opts->forced_yield_every;
    if (opts->experimental_shard_rings != 0U) {
        out.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS;
    }
    if (opts->experimental_shard_rings_multishot != 0U) {
        out.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS_MULTISHOT;
    }
    if (opts->experimental_dynamic_shards != 0U) {
        out.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS;
    }
    if (opts->experimental_lockfree_normq != 0U) {
        out.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    }
    if (opts->experimental_huge_alloc != 0U) {
        out.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_HUGE_ALLOC;
    }
    out.idle_spin_ns = opts->idle_spin_ns;
    out.idle_spin_max_iters = opts->idle_spin_max_iters;
    if (opts->experimental_sqpoll != 0U) {
        out.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL;
    }
    out.sqpoll_cpu = opts->sqpoll_cpu;
    out.profile = (llam_runtime_profile_t)opts->profile;
    return out;
}

/** @brief Translate canonical runtime stats into the legacy shard-named shape. */
static void nm_runtime_stats_from_llam(nm_runtime_stats_t *out, const llam_runtime_stats_t *stats) {
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
    out->active_shards = stats->active_workers;
    out->online_shards = stats->online_workers;
    out->online_shards_floor = stats->online_workers_floor;
    out->online_shards_min = stats->online_workers_min;
    out->online_shards_max = stats->online_workers_max;
    out->active_nodes = stats->active_nodes;
    out->dynamic_shards = stats->dynamic_workers;
    out->shard_rings = stats->worker_rings;
    out->shard_rings_multishot = stats->worker_rings_multishot;
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

int nm_runtime_init_ex(const nm_runtime_opts_t *opts, size_t opts_size) {
    nm_runtime_opts_t opts_storage;
    llam_runtime_opts_t llam_opts;
    size_t opts_copy_size;

    if (opts == NULL) {
        return llam_runtime_init_ex(NULL, 0U);
    }
    if (opts_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    memset(&opts_storage, 0, sizeof(opts_storage));
    opts_storage.deterministic = 1U;
    opts_storage.sqpoll_cpu = -1;
    opts_storage.profile = NM_RUNTIME_PROFILE_BALANCED;
    opts_copy_size = opts_size < sizeof(opts_storage) ? opts_size : sizeof(opts_storage);
    memcpy(&opts_storage, opts, opts_copy_size);
    llam_opts = llam_runtime_opts_from_nm(&opts_storage);
    return llam_runtime_init_ex(&llam_opts, sizeof(llam_opts));
}

int nm_runtime_init(const nm_runtime_opts_t *opts) {
    return nm_runtime_init_ex(opts, opts != NULL ? sizeof(*opts) : 0U);
}

void nm_runtime_shutdown(void) {
    llam_runtime_shutdown();
}

int nm_runtime_request_stop(void) {
    return llam_runtime_request_stop();
}

int nm_runtime_collect_stats_ex(nm_runtime_stats_t *stats, size_t stats_size) {
    llam_runtime_stats_t llam_stats;
    nm_runtime_stats_t nm_stats;
    size_t copy_size;
    int rc;

    if (stats == NULL || stats_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    rc = llam_runtime_collect_stats_ex(&llam_stats, sizeof(llam_stats));
    if (rc == 0) {
        nm_runtime_stats_from_llam(&nm_stats, &llam_stats);
        memset(stats, 0, stats_size);
        copy_size = stats_size < sizeof(nm_stats) ? stats_size : sizeof(nm_stats);
        memcpy(stats, &nm_stats, copy_size);
    }
    return rc;
}

int nm_runtime_collect_stats(nm_runtime_stats_t *stats) {
    return nm_runtime_collect_stats_ex(stats, stats != NULL ? sizeof(*stats) : 0U);
}

int nm_runtime_write_stats_json(int fd) {
    return llam_runtime_write_stats_json(fd);
}

nm_task_t *nm_spawn_ex(nm_task_fn fn, void *arg, const nm_spawn_opts_t *opts, size_t opts_size) {
    nm_spawn_opts_t opts_storage;
    llam_spawn_opts_t llam_opts;
    size_t opts_copy_size;

    if (opts == NULL) {
        return (nm_task_t *)llam_spawn_ex((llam_task_fn)fn, arg, NULL, 0U);
    }
    if (opts_size == 0U) {
        errno = EINVAL;
        return NULL;
    }
    memset(&opts_storage, 0, sizeof(opts_storage));
    opts_copy_size = opts_size < sizeof(opts_storage) ? opts_size : sizeof(opts_storage);
    memcpy(&opts_storage, opts, opts_copy_size);
    llam_opts = llam_spawn_opts_from_nm(&opts_storage);
    return (nm_task_t *)llam_spawn_ex((llam_task_fn)fn, arg, &llam_opts, sizeof(llam_opts));
}

nm_task_t *nm_spawn(nm_task_fn fn, void *arg, const nm_spawn_opts_t *opts) {
    return nm_spawn_ex(fn, arg, opts, opts != NULL ? sizeof(*opts) : 0U);
}

int nm_run(void) {
    return llam_run();
}

void nm_yield(void) {
    llam_yield();
}

void nm_task_safepoint(void) {
    llam_task_safepoint();
}

int nm_join(nm_task_t *task) {
    return llam_join((llam_task_t *)task);
}

int nm_join_until(nm_task_t *task, uint64_t deadline_ns) {
    return llam_join_until((llam_task_t *)task, deadline_ns);
}

int nm_detach(nm_task_t *task) {
    return llam_detach((llam_task_t *)task);
}

int nm_sleep_until(uint64_t deadline_ns) {
    return llam_sleep_until(deadline_ns);
}

int nm_sleep_ns(uint64_t duration_ns) {
    return llam_sleep_ns(duration_ns);
}

void *nm_call_blocking(nm_blocking_fn fn, void *arg) {
    return llam_call_blocking((llam_blocking_fn)fn, arg);
}

int nm_call_blocking_result(nm_blocking_fn fn, void *arg, void **out) {
    return llam_call_blocking_result((llam_blocking_fn)fn, arg, out);
}

int nm_enter_blocking(void) {
    return llam_enter_blocking();
}

int nm_leave_blocking(void) {
    return llam_leave_blocking();
}

int nm_task_set_class(uint32_t task_class) {
    return llam_task_set_class(task_class);
}

void nm_dump_runtime_state(int fd) {
    llam_dump_runtime_state(fd);
}

uint32_t nm_task_flags(const nm_task_t *task) {
    return llam_task_flags((const llam_task_t *)task);
}

nm_cancel_token_t *nm_cancel_token_create(void) {
    return (nm_cancel_token_t *)llam_cancel_token_create();
}

int nm_cancel_token_destroy(nm_cancel_token_t *token) {
    return llam_cancel_token_destroy((llam_cancel_token_t *)token);
}

int nm_cancel_token_cancel(nm_cancel_token_t *token) {
    return llam_cancel_token_cancel((llam_cancel_token_t *)token);
}

int nm_cancel_token_is_cancelled(const nm_cancel_token_t *token) {
    return llam_cancel_token_is_cancelled((const llam_cancel_token_t *)token);
}

nm_mutex_t *nm_mutex_create(void) {
    return (nm_mutex_t *)llam_mutex_create();
}

int nm_mutex_destroy(nm_mutex_t *mutex) {
    return llam_mutex_destroy((llam_mutex_t *)mutex);
}

int nm_mutex_lock(nm_mutex_t *mutex) {
    return llam_mutex_lock((llam_mutex_t *)mutex);
}

int nm_mutex_lock_until(nm_mutex_t *mutex, uint64_t deadline_ns) {
    return llam_mutex_lock_until((llam_mutex_t *)mutex, deadline_ns);
}

int nm_mutex_trylock(nm_mutex_t *mutex) {
    return llam_mutex_trylock((llam_mutex_t *)mutex);
}

int nm_mutex_unlock(nm_mutex_t *mutex) {
    return llam_mutex_unlock((llam_mutex_t *)mutex);
}

nm_cond_t *nm_cond_create(void) {
    return (nm_cond_t *)llam_cond_create();
}

int nm_cond_destroy(nm_cond_t *cond) {
    return llam_cond_destroy((llam_cond_t *)cond);
}

int nm_cond_wait(nm_cond_t *cond, nm_mutex_t *mutex) {
    return llam_cond_wait((llam_cond_t *)cond, (llam_mutex_t *)mutex);
}

int nm_cond_wait_until(nm_cond_t *cond, nm_mutex_t *mutex, uint64_t deadline_ns) {
    return llam_cond_wait_until((llam_cond_t *)cond, (llam_mutex_t *)mutex, deadline_ns);
}

int nm_cond_signal(nm_cond_t *cond) {
    return llam_cond_signal((llam_cond_t *)cond);
}

int nm_cond_broadcast(nm_cond_t *cond) {
    return llam_cond_broadcast((llam_cond_t *)cond);
}

nm_channel_t *nm_channel_create(size_t capacity) {
    return (nm_channel_t *)llam_channel_create(capacity);
}

int nm_channel_destroy(nm_channel_t *channel) {
    return llam_channel_destroy((llam_channel_t *)channel);
}

int nm_channel_send(nm_channel_t *channel, void *value) {
    return llam_channel_send((llam_channel_t *)channel, value);
}

int nm_channel_send_until(nm_channel_t *channel, void *value, uint64_t deadline_ns) {
    return llam_channel_send_until((llam_channel_t *)channel, value, deadline_ns);
}

void *nm_channel_recv(nm_channel_t *channel) {
    return llam_channel_recv((llam_channel_t *)channel);
}

int nm_channel_recv_result(nm_channel_t *channel, void **out) {
    return llam_channel_recv_result((llam_channel_t *)channel, out);
}

void *nm_channel_recv_until(nm_channel_t *channel, uint64_t deadline_ns) {
    return llam_channel_recv_until((llam_channel_t *)channel, deadline_ns);
}

int nm_channel_recv_until_result(nm_channel_t *channel, uint64_t deadline_ns, void **out) {
    return llam_channel_recv_until_result((llam_channel_t *)channel, deadline_ns, out);
}

int nm_channel_close(nm_channel_t *channel) {
    return llam_channel_close((llam_channel_t *)channel);
}

ssize_t nm_read(nm_fd_t fd, void *buf, size_t count) {
    return llam_read((llam_fd_t)fd, buf, count);
}

ssize_t nm_write(nm_fd_t fd, const void *buf, size_t count) {
    return llam_write((llam_fd_t)fd, buf, count);
}

ssize_t nm_read_owned(nm_fd_t fd, size_t max_count, nm_io_buffer_t **out) {
    llam_io_buffer_t *buffer = NULL;
    ssize_t rc = llam_read_owned((llam_fd_t)fd, max_count, out != NULL ? &buffer : NULL);

    if (out != NULL) {
        *out = (nm_io_buffer_t *)buffer;
    }
    return rc;
}

ssize_t nm_recv_owned(nm_fd_t fd, size_t max_count, int flags, nm_io_buffer_t **out) {
    llam_io_buffer_t *buffer = NULL;
    ssize_t rc = llam_recv_owned((llam_fd_t)fd, max_count, flags, out != NULL ? &buffer : NULL);

    if (out != NULL) {
        *out = (nm_io_buffer_t *)buffer;
    }
    return rc;
}

void nm_io_buffer_release(nm_io_buffer_t *buffer) {
    llam_io_buffer_release((llam_io_buffer_t *)buffer);
}

void *nm_io_buffer_data(nm_io_buffer_t *buffer) {
    return llam_io_buffer_data((llam_io_buffer_t *)buffer);
}

size_t nm_io_buffer_size(const nm_io_buffer_t *buffer) {
    return llam_io_buffer_size((const llam_io_buffer_t *)buffer);
}

size_t nm_io_buffer_capacity(const nm_io_buffer_t *buffer) {
    return llam_io_buffer_capacity((const llam_io_buffer_t *)buffer);
}

nm_fd_t nm_accept(nm_fd_t fd, struct sockaddr *addr, socklen_t *addrlen) {
    return (nm_fd_t)llam_accept((llam_fd_t)fd, addr, addrlen);
}

int nm_connect(nm_fd_t fd, const struct sockaddr *addr, socklen_t addrlen) {
    return llam_connect((llam_fd_t)fd, addr, addrlen);
}

int nm_poll_fd(nm_fd_t fd, short events, int timeout_ms, short *revents) {
    return llam_poll_fd((llam_fd_t)fd, events, timeout_ms, revents);
}

uint64_t nm_now_ns(void) {
    return llam_now_ns();
}

uint64_t nm_task_id(const nm_task_t *task) {
    return llam_task_id((const llam_task_t *)task);
}

const char *nm_task_state_name(const nm_task_t *task) {
    return llam_task_state_name((const llam_task_t *)task);
}

uint32_t nm_task_class(const nm_task_t *task) {
    return llam_task_class((const llam_task_t *)task);
}

nm_task_t *nm_current_task(void) {
    return (nm_task_t *)llam_current_task();
}
