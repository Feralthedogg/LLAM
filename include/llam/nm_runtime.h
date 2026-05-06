/**
 * @file include/llam/nm_runtime.h
 * @brief Compatibility public API that exposes the legacy nm_* runtime names.
 *
 * @details
 * This header mirrors the canonical @c llam_* API with the historical @c nm_*
 * names. It is kept source-compatible for existing users; new projects may
 * include @c <llam/runtime.h> directly.
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

#ifndef NM_RUNTIME_H
#define NM_RUNTIME_H

#include "llam/nm_platform.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Legacy API source/API version major component. */
#define NM_VERSION_MAJOR 1U
/** @brief Legacy API source/API version minor component. */
#define NM_VERSION_MINOR 0U
/** @brief Legacy API source/API version patch component. */
#define NM_VERSION_PATCH 0U

/** @brief Legacy public ABI major version. */
#define NM_ABI_VERSION_MAJOR 1U
/** @brief Legacy public ABI minor version. */
#define NM_ABI_VERSION_MINOR 0U
/** @brief Packed legacy public ABI version. */
#define NM_ABI_VERSION ((NM_ABI_VERSION_MAJOR << 16U) | NM_ABI_VERSION_MINOR)

/** @brief Legacy ABI metadata returned by ::nm_abi_get_info. */
typedef struct nm_abi_info {
    uint32_t abi_major;             /**< Public ABI major version. */
    uint32_t abi_minor;             /**< Public ABI minor version. */
    uint32_t version_major;         /**< Source/API version major component. */
    uint32_t version_minor;         /**< Source/API version minor component. */
    uint32_t version_patch;         /**< Source/API version patch component. */
    uint32_t reserved0;             /**< Reserved for future flags; currently 0. */
    size_t struct_size;             /**< Size of this struct in the loaded library. */
    size_t runtime_opts_size;       /**< Size of ::nm_runtime_opts_t in the loaded library. */
    size_t spawn_opts_size;         /**< Size of ::nm_spawn_opts_t in the loaded library. */
    size_t runtime_stats_size;      /**< Size of ::nm_runtime_stats_t in the loaded library. */
    const char *runtime_name;       /**< Stable runtime name string, currently "LLAM". */
    const char *version_string;     /**< Static version string owned by the library. */
    const char *platform_name;      /**< Static platform name string owned by the library. */
} nm_abi_info_t;

/** @brief Current size to pass to ::nm_abi_get_info. */
#define NM_ABI_INFO_CURRENT_SIZE ((size_t)sizeof(nm_abi_info_t))

/** @brief Opaque task handle returned by ::nm_spawn. */
typedef struct nm_task nm_task_t;
/** @brief Runtime-aware mutex handle. */
typedef struct nm_mutex nm_mutex_t;
/** @brief Runtime-aware condition variable handle. */
typedef struct nm_cond nm_cond_t;
/** @brief Bounded pointer channel handle. */
typedef struct nm_channel nm_channel_t;
/** @brief Cancellation token shared by spawned tasks and blocking waits. */
typedef struct nm_cancel_token nm_cancel_token_t;
/** @brief Runtime-owned I/O buffer returned by owned-read APIs. */
typedef struct nm_io_buffer nm_io_buffer_t;

/** @brief Task entry point signature. */
typedef void (*nm_task_fn)(void *arg);
/** @brief Blocking callback signature used by ::nm_call_blocking. */
typedef void *(*nm_blocking_fn)(void *arg);

/* ABI and dynamic loading. */
uint32_t nm_abi_version(void);
const char *nm_version_string(void);
int nm_abi_get_info(nm_abi_info_t *info, size_t info_size);

/** @brief Scheduler priority class used for slice and latency policy. */
typedef enum nm_task_class {
    NM_TASK_CLASS_LATENCY = 0, /**< Latency-sensitive task class. */
    NM_TASK_CLASS_DEFAULT = 1, /**< Default balanced task class. */
    NM_TASK_CLASS_BATCH = 2,   /**< Batch/background task class. */
} nm_task_class_t;

/** @brief Requested stack size class for newly spawned tasks. */
typedef enum nm_stack_class {
    NM_STACK_CLASS_DEFAULT = 0, /**< Default stack profile. */
    NM_STACK_CLASS_LARGE = 1,   /**< Larger stack for deeper call chains. */
    NM_STACK_CLASS_HUGE = 2,    /**< Largest stack class for exceptional tasks. */
} nm_stack_class_t;

/** @brief Runtime policy profile selected at initialization. */
typedef enum nm_runtime_profile {
    NM_RUNTIME_PROFILE_BALANCED = 0,     /**< Balanced default policy. */
    NM_RUNTIME_PROFILE_RELEASE_FAST = 1, /**< Lower overhead release-oriented policy. */
    NM_RUNTIME_PROFILE_DEBUG_SAFE = 2,   /**< Heavier diagnostics and stricter safepoints. */
    NM_RUNTIME_PROFILE_IO_LATENCY = 3,   /**< Bias toward I/O wake latency. */
} nm_runtime_profile_t;

/** @brief Spawn option flags. */
enum {
    NM_SPAWN_F_PINNED = 1U << 0,           /**< Keep the task on its home shard. */
    NM_SPAWN_F_NO_PREEMPT = 1U << 1,       /**< Disable watchdog preemption for the task. */
    NM_SPAWN_F_SYS_TASK = 1U << 2,         /**< Internal/system task hint. */
    NM_SPAWN_F_LATENCY_CRITICAL = 1U << 3, /**< Prefer latency-oriented placement. */
};

/** @brief Optional task spawn configuration. */
typedef struct nm_spawn_opts {
    uint32_t task_class;              /**< Initial class; one of ::nm_task_class_t. */
    uint32_t stack_class;             /**< Requested stack class; one of ::nm_stack_class_t. */
    uint32_t flags;                   /**< Bitwise OR of @c NM_SPAWN_F_* flags. */
    uint64_t deadline_ns;             /**< Optional absolute deadline for caller-defined use. */
    nm_cancel_token_t *cancel_token;  /**< Optional cancellation token inherited by the task. */
} nm_spawn_opts_t;

/** @brief Current size to pass to ::nm_spawn_ex and ::nm_spawn_opts_init. */
#define NM_SPAWN_OPTS_CURRENT_SIZE ((size_t)sizeof(nm_spawn_opts_t))

/** @brief Process-wide runtime initialization options. */
typedef struct nm_runtime_opts {
    uint32_t deterministic;                         /**< Run with one scheduler shard when non-zero. */
    uint32_t forced_yield_every;                    /**< Force cooperative yields every N safepoints. */
    uint32_t experimental_shard_rings;              /**< Use one I/O backend node per shard. */
    uint32_t experimental_shard_rings_multishot;    /**< Allow multishot shared-fd watches with shard rings. */
    uint32_t experimental_dynamic_shards;           /**< Park/reactivate idle shards based on pressure. */
    uint32_t experimental_lockfree_normq;           /**< Use the Chase-Lev normal queue implementation. */
    uint32_t experimental_huge_alloc;               /**< Prefer hugepage-friendly allocator backing. */
    uint64_t idle_spin_ns;                          /**< Optional bounded idle spin before sleeping. */
    uint32_t idle_spin_max_iters;                   /**< Maximum pause-loop iterations during idle spin. */
    uint32_t experimental_sqpoll;                   /**< Use io_uring SQPOLL for node-owned rings. */
    int32_t sqpoll_cpu;                             /**< SQPOLL CPU, or -1 to let the runtime choose. */
    uint32_t profile;                               /**< Runtime profile; one of ::nm_runtime_profile_t. */
} nm_runtime_opts_t;

/** @brief Current size to pass to ::nm_runtime_init_ex and ::nm_runtime_opts_init. */
#define NM_RUNTIME_OPTS_CURRENT_SIZE ((size_t)sizeof(nm_runtime_opts_t))

/** @brief Aggregated runtime counters returned by ::nm_runtime_collect_stats. */
typedef struct nm_runtime_stats {
    uint64_t ctx_switches;               /**< Total task/scheduler context switches. */
    uint64_t yields;                     /**< Explicit or runtime-driven task yields. */
    uint64_t parks;                      /**< Task park operations. */
    uint64_t wakes;                      /**< Task wake operations. */
    uint64_t steals;                     /**< Work-stealing batches. */
    uint64_t migrations;                 /**< Tasks migrated between shards. */
    uint64_t blocking_calls;             /**< Blocking-worker submissions. */
    uint64_t blocking_completions;       /**< Blocking-worker completions. */
    uint64_t io_submits;                 /**< Runtime I/O submissions. */
    uint64_t io_submit_calls;            /**< Backend submit calls. */
    uint64_t io_submit_syscalls;         /**< Backend submit syscalls. */
    uint64_t io_completions;             /**< Runtime I/O completions. */
    uint64_t idle_polls;                 /**< Scheduler idle waits. */
    uint64_t idle_spin_loops;            /**< Idle spin pause-loop iterations. */
    uint64_t idle_spin_hits;             /**< Idle spins that found work before sleeping. */
    uint64_t idle_spin_fallbacks;        /**< Idle spins that fell back to sleeping. */
    uint64_t idle_spin_ns;               /**< Time spent in idle-spin windows. */
    uint64_t queue_overflows;            /**< Runtime overflow queue insertions. */
    uint64_t overflow_depth;             /**< Current overflow queue depth. */
    uint32_t active_shards;              /**< Configured shard count. */
    uint32_t online_shards;              /**< Currently online shard count. */
    uint32_t online_shards_floor;        /**< Dynamic-shard online floor. */
    uint32_t online_shards_min;          /**< Minimum observed online shard count. */
    uint32_t online_shards_max;          /**< Maximum observed online shard count. */
    uint32_t active_nodes;               /**< Active I/O node count. */
    uint32_t dynamic_shards;             /**< Whether dynamic shard scaling is enabled. */
    uint32_t shard_rings;                /**< Whether per-shard I/O rings are enabled. */
    uint32_t shard_rings_multishot;      /**< Whether shard-ring multishot mode is enabled. */
    uint32_t lockfree_normq;             /**< Whether lock-free normal queues are enabled. */
    uint32_t huge_alloc;                 /**< Whether huge allocator backing is active. */
    uint32_t sqpoll;                     /**< Whether SQPOLL is active. */
    uint64_t opaque_block_ns;            /**< Total opaque-block duration. */
    uint64_t opaque_block_samples;       /**< Number of opaque-block samples. */
    uint64_t opaque_block_max_ns;        /**< Maximum observed opaque-block duration. */
    uint64_t opaque_enter_wait_ns;       /**< Total wait time entering helper compensation. */
    uint64_t opaque_enter_wait_samples;  /**< Number of enter-wait samples. */
    uint64_t opaque_enter_wait_max_ns;   /**< Maximum enter-wait duration. */
    uint64_t opaque_leave_wait_ns;       /**< Total wait time leaving helper compensation. */
    uint64_t opaque_leave_wait_samples;  /**< Number of leave-wait samples. */
    uint64_t opaque_leave_wait_max_ns;   /**< Maximum leave-wait duration. */
} nm_runtime_stats_t;

/** @brief Current size to pass to ::nm_runtime_collect_stats_ex. */
#define NM_RUNTIME_STATS_CURRENT_SIZE ((size_t)sizeof(nm_runtime_stats_t))

/* Runtime lifecycle and task scheduling. */
int nm_runtime_opts_init(nm_runtime_opts_t *opts, size_t opts_size);
int nm_spawn_opts_init(nm_spawn_opts_t *opts, size_t opts_size);
int nm_runtime_init_ex(const nm_runtime_opts_t *opts, size_t opts_size);
int nm_runtime_init(const nm_runtime_opts_t *opts);
int nm_runtime_request_stop(void);
void nm_runtime_shutdown(void);
int nm_runtime_collect_stats_ex(nm_runtime_stats_t *stats, size_t stats_size);
int nm_runtime_collect_stats(nm_runtime_stats_t *stats);

nm_task_t *nm_spawn_ex(nm_task_fn fn, void *arg, const nm_spawn_opts_t *opts, size_t opts_size);
nm_task_t *nm_spawn(nm_task_fn fn, void *arg, const nm_spawn_opts_t *opts);
int nm_run(void);
void nm_yield(void);
int nm_join(nm_task_t *task);
int nm_join_until(nm_task_t *task, uint64_t deadline_ns);
int nm_detach(nm_task_t *task);
int nm_sleep_until(uint64_t deadline_ns);
int nm_sleep_ns(uint64_t duration_ns);
int nm_call_blocking_result(nm_blocking_fn fn, void *arg, void **out);
void *nm_call_blocking(nm_blocking_fn fn, void *arg);
int nm_enter_blocking(void);
int nm_leave_blocking(void);
int nm_task_set_class(uint32_t task_class);
void nm_dump_runtime_state(int fd);
uint32_t nm_task_flags(const nm_task_t *task);

/* Cancellation tokens. */
nm_cancel_token_t *nm_cancel_token_create(void);
int nm_cancel_token_destroy(nm_cancel_token_t *token);
int nm_cancel_token_cancel(nm_cancel_token_t *token);
int nm_cancel_token_is_cancelled(const nm_cancel_token_t *token);

/* Runtime-aware mutexes. */
nm_mutex_t *nm_mutex_create(void);
int nm_mutex_destroy(nm_mutex_t *mutex);
int nm_mutex_lock(nm_mutex_t *mutex);
int nm_mutex_lock_until(nm_mutex_t *mutex, uint64_t deadline_ns);
int nm_mutex_trylock(nm_mutex_t *mutex);
int nm_mutex_unlock(nm_mutex_t *mutex);

/* Runtime-aware condition variables. */
nm_cond_t *nm_cond_create(void);
int nm_cond_destroy(nm_cond_t *cond);
int nm_cond_wait(nm_cond_t *cond, nm_mutex_t *mutex);
int nm_cond_wait_until(nm_cond_t *cond, nm_mutex_t *mutex, uint64_t deadline_ns);
int nm_cond_signal(nm_cond_t *cond);
int nm_cond_broadcast(nm_cond_t *cond);

/* Bounded pointer channels. Capacity must be at least 1. */
nm_channel_t *nm_channel_create(size_t capacity);
int nm_channel_destroy(nm_channel_t *channel);
int nm_channel_send(nm_channel_t *channel, void *value);
int nm_channel_send_until(nm_channel_t *channel, void *value, uint64_t deadline_ns);
int nm_channel_recv_result(nm_channel_t *channel, void **out);
int nm_channel_recv_until_result(nm_channel_t *channel, uint64_t deadline_ns, void **out);
void *nm_channel_recv(nm_channel_t *channel);
void *nm_channel_recv_until(nm_channel_t *channel, uint64_t deadline_ns);
int nm_channel_close(nm_channel_t *channel);

/* Runtime-integrated I/O helpers. */
ssize_t nm_read(nm_fd_t fd, void *buf, size_t count);
ssize_t nm_write(nm_fd_t fd, const void *buf, size_t count);
ssize_t nm_read_owned(nm_fd_t fd, size_t max_count, nm_io_buffer_t **out);
ssize_t nm_recv_owned(nm_fd_t fd, size_t max_count, int flags, nm_io_buffer_t **out);
void nm_io_buffer_release(nm_io_buffer_t *buffer);
void *nm_io_buffer_data(nm_io_buffer_t *buffer);
size_t nm_io_buffer_size(const nm_io_buffer_t *buffer);
size_t nm_io_buffer_capacity(const nm_io_buffer_t *buffer);
/** @brief Accept a connection, returning @c NM_INVALID_FD on failure with @c errno set. */
nm_fd_t nm_accept(nm_fd_t fd, struct sockaddr *addr, socklen_t *addrlen);
int nm_connect(nm_fd_t fd, const struct sockaddr *addr, socklen_t addrlen);
int nm_poll_fd(nm_fd_t fd, short events, int timeout_ms, short *revents);

/* Time and task introspection. */
uint64_t nm_now_ns(void);
uint64_t nm_task_id(const nm_task_t *task);
const char *nm_task_state_name(const nm_task_t *task);
uint32_t nm_task_class(const nm_task_t *task);
nm_task_t *nm_current_task(void);

#ifdef __cplusplus
}
#endif

#endif
