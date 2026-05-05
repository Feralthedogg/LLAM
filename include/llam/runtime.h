/**
 * @file include/llam/runtime.h
 * @brief Canonical public LLAM runtime API: lifecycle, task scheduling, synchronization, I/O, time, debug, and platform hooks.
 *
 * @details
 * LLAM exposes stackful cooperative tasks backed by a scheduler/runtime that can
 * integrate blocking callbacks, runtime-aware synchronization primitives, and
 * platform I/O backends. This header is the canonical public API. Legacy
 * @c nm_* names are provided by compatibility headers and forward to the same
 * implementation.
 *
 * Typical lifecycle:
 *  - call ::llam_runtime_init once,
 *  - create work with ::llam_spawn,
 *  - drive workers with ::llam_run,
 *  - and release global runtime resources with ::llam_runtime_shutdown.
 *
 * All absolute deadlines use ::llam_now_ns units. Unless explicitly documented,
 * functions returning @c -1 set @c errno to the failure reason.
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

#ifndef LLAM_RUNTIME_H
#define LLAM_RUNTIME_H

#include "llam/platform.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque handle for a stackful LLAM task. */
typedef struct llam_task llam_task_t;

/** @brief Opaque handle for a runtime-aware mutex. */
typedef struct llam_mutex llam_mutex_t;

/** @brief Opaque handle for a runtime-aware condition variable. */
typedef struct llam_cond llam_cond_t;

/** @brief Opaque handle for a pointer-valued runtime channel. */
typedef struct llam_channel llam_channel_t;

/** @brief Opaque handle for a shared cancellation token. */
typedef struct llam_cancel_token llam_cancel_token_t;

/** @brief Opaque handle for a runtime-owned I/O buffer. */
typedef struct llam_io_buffer llam_io_buffer_t;

/**
 * @brief Task entry point executed on a LLAM-managed stackful user thread.
 * @param arg User pointer passed to llam_spawn().
 */
typedef void (*llam_task_fn)(void *arg);

/**
 * @brief Blocking callback executed by the runtime blocking/offload path.
 * @param arg User pointer passed to llam_call_blocking().
 * @return User-defined result pointer returned to the waiting task.
 */
typedef void *(*llam_blocking_fn)(void *arg);

/** @brief Scheduler priority class used as a policy hint. */
typedef enum llam_task_class {
    LLAM_TASK_CLASS_LATENCY = 0, /**< Favor wakeup and dispatch latency. */
    LLAM_TASK_CLASS_DEFAULT = 1, /**< General-purpose task class. */
    LLAM_TASK_CLASS_BATCH = 2,   /**< Throughput-oriented task class. */
} llam_task_class_t;

/** @brief Fiber stack size class used when a task is created. */
typedef enum llam_stack_class {
    LLAM_STACK_CLASS_DEFAULT = 0, /**< Default stack size. */
    LLAM_STACK_CLASS_LARGE = 1,   /**< Larger stack for deeper C call chains. */
    LLAM_STACK_CLASS_HUGE = 2,    /**< Largest built-in stack class. */
} llam_stack_class_t;

/** @brief High-level runtime tuning profile. */
typedef enum llam_runtime_profile {
    LLAM_RUNTIME_PROFILE_BALANCED = 0,     /**< Default latency/throughput balance. */
    LLAM_RUNTIME_PROFILE_RELEASE_FAST = 1, /**< Favor low overhead for release runs. */
    LLAM_RUNTIME_PROFILE_DEBUG_SAFE = 2,   /**< Favor diagnostics and conservative behavior. */
    LLAM_RUNTIME_PROFILE_IO_LATENCY = 3,   /**< Favor I/O wakeup latency. */
} llam_runtime_profile_t;

/** @brief Bit flags accepted by llam_spawn_opts_t::flags. */
enum {
    LLAM_SPAWN_F_PINNED = 1U << 0,           /**< Prefer keeping the task on its home worker. */
    LLAM_SPAWN_F_NO_PREEMPT = 1U << 1,       /**< Restrict cooperative preemption checks. */
    LLAM_SPAWN_F_SYS_TASK = 1U << 2,         /**< Mark runtime-owned helper work. */
    LLAM_SPAWN_F_LATENCY_CRITICAL = 1U << 3, /**< Promote wakeup and dispatch priority. */
};

/** @brief Optional per-task spawn policy. */
typedef struct llam_spawn_opts {
    llam_task_class_t task_class;       /**< Scheduler class for the new task. */
    llam_stack_class_t stack_class;     /**< Fiber stack size class. */
    unsigned flags;                     /**< Bitwise OR of LLAM_SPAWN_F_* values. */
    uint64_t deadline_ns;               /**< Optional absolute deadline in llam_now_ns() units; 0 disables it. */
    llam_cancel_token_t *cancel_token;  /**< Optional cancellation token observed by waits and I/O. */
} llam_spawn_opts_t;

/** @brief Runtime initialization options. */
typedef struct llam_runtime_opts {
    unsigned deterministic;                         /**< Prefer repeatable scheduling decisions. */
    unsigned forced_yield_every;                    /**< Force cooperative yield after this many scheduler ticks; 0 disables it. */
    unsigned experimental_worker_rings;             /**< Give workers their own I/O rings/queues where supported. */
    unsigned experimental_worker_rings_multishot;   /**< Allow multishot watches in worker-ring mode. */
    unsigned experimental_dynamic_workers;          /**< Soft-park idle workers and reactivate on pressure. */
    unsigned experimental_lockfree_normq;           /**< Use the lock-free normal run queue. */
    unsigned experimental_huge_alloc;               /**< Prefer hugepage-friendly backing for selected allocators. */
    uint64_t idle_spin_ns;                          /**< Bounded idle spin duration before kernel sleep. */
    unsigned idle_spin_max_iters;                   /**< Maximum idle-spin iterations. */
    unsigned experimental_sqpoll;                   /**< Linux io_uring SQPOLL experiment for node-owned rings. */
    int sqpoll_cpu;                                 /**< Requested SQPOLL CPU, or -1 for automatic selection. */
    llam_runtime_profile_t profile;                 /**< High-level runtime policy profile. */
} llam_runtime_opts_t;

/** @brief Cumulative runtime statistics snapshot. */
typedef struct llam_runtime_stats {
    uint64_t ctx_switches;              /**< Number of fiber context switches. */
    uint64_t yields;                    /**< Cooperative yields. */
    uint64_t parks;                     /**< Task park operations. */
    uint64_t wakes;                     /**< Task wake operations. */
    uint64_t steals;                    /**< Cross-worker steal attempts that succeeded. */
    uint64_t migrations;                /**< Task or I/O ownership migrations. */
    uint64_t blocking_calls;            /**< Blocking/offload calls submitted. */
    uint64_t blocking_completions;      /**< Blocking/offload calls completed. */
    uint64_t io_submits;                /**< Logical I/O requests submitted. */
    uint64_t io_submit_calls;           /**< Backend submit attempts. */
    uint64_t io_submit_syscalls;        /**< Backend submit syscalls. */
    uint64_t io_completions;            /**< Logical I/O completions. */
    uint64_t idle_polls;                /**< Idle worker kernel-poll iterations. */
    uint64_t idle_spin_loops;           /**< Idle spin loop iterations. */
    uint64_t idle_spin_hits;            /**< Idle spins that found work. */
    uint64_t idle_spin_fallbacks;       /**< Idle spins that fell back to kernel sleep. */
    uint64_t idle_spin_ns;              /**< Total idle spin time in nanoseconds. */
    uint64_t queue_overflows;           /**< Scheduler queue overflow events. */
    uint64_t overflow_depth;            /**< Current overflow queue depth. */
    unsigned active_workers;            /**< Configured worker count. */
    unsigned online_workers;            /**< Workers currently online. */
    unsigned online_workers_floor;      /**< Minimum online-worker floor. */
    unsigned online_workers_min;        /**< Minimum online workers observed. */
    unsigned online_workers_max;        /**< Maximum online workers observed. */
    unsigned active_nodes;              /**< Active platform I/O nodes. */
    unsigned dynamic_workers;           /**< Whether dynamic workers are active. */
    unsigned worker_rings;              /**< Whether worker ring mode is active. */
    unsigned worker_rings_multishot;    /**< Whether worker-ring multishot mode is active. */
    unsigned lockfree_normq;            /**< Whether lock-free normal queues are active. */
    unsigned huge_alloc;                /**< Whether huge allocation mode is active. */
    unsigned sqpoll;                    /**< Whether Linux SQPOLL mode is active. */
    uint64_t opaque_block_ns;           /**< Total opaque blocking time in nanoseconds. */
    uint64_t opaque_block_samples;      /**< Opaque blocking sample count. */
    uint64_t opaque_block_max_ns;       /**< Maximum opaque blocking duration. */
    uint64_t opaque_enter_wait_ns;      /**< Total wait time entering opaque blocking regions. */
    uint64_t opaque_enter_wait_samples; /**< Enter-wait sample count. */
    uint64_t opaque_enter_wait_max_ns;  /**< Maximum enter-wait duration. */
    uint64_t opaque_leave_wait_ns;      /**< Total wait time leaving opaque blocking regions. */
    uint64_t opaque_leave_wait_samples; /**< Leave-wait sample count. */
    uint64_t opaque_leave_wait_max_ns;  /**< Maximum leave-wait duration. */
} llam_runtime_stats_t;

/* ============================================================================
 * Runtime lifecycle and task scheduling
 * ============================================================================
 */

/**
 * @brief Initialize global runtime state.
 * @param opts Optional runtime options; pass NULL for defaults.
 * @return 0 on success, -1 on failure with errno set.
 */
int llam_runtime_init(const llam_runtime_opts_t *opts);

/** @brief Stop workers and release runtime-owned resources. */
void llam_runtime_shutdown(void);

/**
 * @brief Collect a best-effort snapshot of runtime counters.
 * @param stats Destination statistics object. Must not be NULL.
 * @return 0 on success, -1 on failure with errno set.
 */
int llam_runtime_collect_stats(llam_runtime_stats_t *stats);

/**
 * @brief Create a task and make it runnable.
 * @param fn Task entry point. Must not be NULL.
 * @param arg User pointer passed to fn.
 * @param opts Optional spawn options; pass NULL for defaults.
 * @return Task handle on success, NULL on failure with errno set.
 */
llam_task_t *llam_spawn(llam_task_fn fn, void *arg, const llam_spawn_opts_t *opts);

/**
 * @brief Run the scheduler until all runtime work completes or an error occurs.
 * @return 0 on clean completion, -1 on failure with errno set.
 */
int llam_run(void);

/** @brief Cooperatively yield the current task. */
void llam_yield(void);

/**
 * @brief Wait indefinitely for task completion.
 * @param task Task returned by llam_spawn().
 * @return 0 on completion, -1 on failure with errno set.
 */
int llam_join(llam_task_t *task);

/**
 * @brief Wait for task completion until an absolute deadline.
 * @param task Task returned by llam_spawn().
 * @param deadline_ns Absolute deadline in llam_now_ns() units.
 * @return 0 on completion, -1 on timeout/failure with errno set.
 */
int llam_join_until(llam_task_t *task, uint64_t deadline_ns);

/**
 * @brief Sleep the current task until an absolute deadline.
 * @param deadline_ns Absolute deadline in llam_now_ns() units.
 * @return 0 on wake, -1 on cancellation/failure with errno set.
 */
int llam_sleep_until(uint64_t deadline_ns);

/**
 * @brief Sleep the current task for a relative duration.
 * @param duration_ns Sleep duration in nanoseconds.
 * @return 0 on wake, -1 on cancellation/failure with errno set.
 */
int llam_sleep_ns(uint64_t duration_ns);

/**
 * @brief Execute a blocking callback through the runtime blocking path.
 * @param fn Blocking callback. Must not be NULL.
 * @param arg User pointer passed to fn.
 * @return Callback result, or NULL if the callback returned NULL or submission failed.
 */
void *llam_call_blocking(llam_blocking_fn fn, void *arg);

/**
 * @brief Mark the current task as entering an opaque blocking region.
 * @return 0 on success, -1 on failure with errno set.
 */
int llam_enter_blocking(void);

/**
 * @brief Mark the current task as leaving an opaque blocking region.
 * @return 0 on success, -1 on failure with errno set.
 */
int llam_leave_blocking(void);

/** @brief Change the current task's scheduler class. */
void llam_task_set_class(llam_task_class_t task_class);

/**
 * @brief Write a human-readable runtime dump to an fd.
 * @param fd Destination file descriptor.
 */
void llam_dump_runtime_state(int fd);

/**
 * @brief Return task spawn/runtime flags.
 * @param task Task handle.
 * @return Bitwise OR of LLAM_SPAWN_F_* values, or 0 for NULL.
 */
unsigned llam_task_flags(const llam_task_t *task);

/* ============================================================================
 * Cancellation tokens
 * ============================================================================
 */

/** @brief Create a cancellation token. */
llam_cancel_token_t *llam_cancel_token_create(void);

/** @brief Destroy a cancellation token. */
int llam_cancel_token_destroy(llam_cancel_token_t *token);

/** @brief Request cancellation for all current and future observers of token. */
int llam_cancel_token_cancel(llam_cancel_token_t *token);

/** @brief Return non-zero when token has been cancelled. */
int llam_cancel_token_is_cancelled(const llam_cancel_token_t *token);

/* ============================================================================
 * Runtime-aware mutexes and condition variables
 * ============================================================================
 */

/** @brief Create a runtime-aware mutex. */
llam_mutex_t *llam_mutex_create(void);

/** @brief Destroy a runtime-aware mutex. */
void llam_mutex_destroy(llam_mutex_t *mutex);

/** @brief Lock a runtime-aware mutex, parking the task if needed. */
int llam_mutex_lock(llam_mutex_t *mutex);

/** @brief Lock a runtime-aware mutex until an absolute deadline. */
int llam_mutex_lock_until(llam_mutex_t *mutex, uint64_t deadline_ns);

/** @brief Try to lock a runtime-aware mutex without parking. */
int llam_mutex_trylock(llam_mutex_t *mutex);

/** @brief Unlock a runtime-aware mutex and wake a waiter if one exists. */
int llam_mutex_unlock(llam_mutex_t *mutex);

/** @brief Create a runtime-aware condition variable. */
llam_cond_t *llam_cond_create(void);

/** @brief Destroy a runtime-aware condition variable. */
void llam_cond_destroy(llam_cond_t *cond);

/** @brief Wait on a condition variable and atomically release/reacquire mutex. */
int llam_cond_wait(llam_cond_t *cond, llam_mutex_t *mutex);

/** @brief Wait on a condition variable until an absolute deadline. */
int llam_cond_wait_until(llam_cond_t *cond, llam_mutex_t *mutex, uint64_t deadline_ns);

/** @brief Wake one condition-variable waiter. */
int llam_cond_signal(llam_cond_t *cond);

/** @brief Wake all condition-variable waiters. */
int llam_cond_broadcast(llam_cond_t *cond);

/* ============================================================================
 * Channels
 * ============================================================================
 */

/**
 * @brief Create a pointer-valued channel.
 * @param capacity 0 for rendezvous behavior, non-zero for bounded buffering.
 * @return Channel handle on success, NULL on failure with errno set.
 */
llam_channel_t *llam_channel_create(size_t capacity);

/** @brief Destroy a channel after all users have stopped accessing it. */
void llam_channel_destroy(llam_channel_t *channel);

/** @brief Send a pointer value, parking the task if the channel is full. */
int llam_channel_send(llam_channel_t *channel, void *value);

/** @brief Send a pointer value until an absolute deadline. */
int llam_channel_send_until(llam_channel_t *channel, void *value, uint64_t deadline_ns);

/** @brief Receive a pointer value, parking the task if the channel is empty. */
void *llam_channel_recv(llam_channel_t *channel);

/** @brief Receive a pointer value until an absolute deadline. */
void *llam_channel_recv_until(llam_channel_t *channel, uint64_t deadline_ns);

/** @brief Close a channel and wake blocked senders/receivers. */
int llam_channel_close(llam_channel_t *channel);

/* ============================================================================
 * Runtime I/O and owned buffers
 * ============================================================================
 */

/** @brief Read from fd using the runtime I/O backend where possible. */
ssize_t llam_read(llam_fd_t fd, void *buf, size_t count);

/** @brief Write to fd using the runtime I/O backend where possible. */
ssize_t llam_write(llam_fd_t fd, const void *buf, size_t count);

/** @brief Read into a runtime-owned buffer. */
ssize_t llam_read_owned(llam_fd_t fd, size_t max_count, llam_io_buffer_t **out);

/** @brief Receive into a runtime-owned buffer with recv flags. */
ssize_t llam_recv_owned(llam_fd_t fd, size_t max_count, int flags, llam_io_buffer_t **out);

/** @brief Release a runtime-owned I/O buffer. */
void llam_io_buffer_release(llam_io_buffer_t *buffer);

/** @brief Return the data pointer for a runtime-owned I/O buffer. */
void *llam_io_buffer_data(llam_io_buffer_t *buffer);

/** @brief Return the number of valid bytes in a runtime-owned I/O buffer. */
size_t llam_io_buffer_size(const llam_io_buffer_t *buffer);

/** @brief Return total capacity of a runtime-owned I/O buffer. */
size_t llam_io_buffer_capacity(const llam_io_buffer_t *buffer);

/** @brief Accept a connection from a listener fd using the runtime I/O backend where possible. */
llam_fd_t llam_accept(llam_fd_t fd, struct sockaddr *addr, socklen_t *addrlen);

/** @brief Wait for fd readiness. */
int llam_poll_fd(llam_fd_t fd, short events, int timeout_ms, short *revents);

/* ============================================================================
 * Time and task introspection
 * ============================================================================
 */

/** @brief Return a monotonic timestamp in nanoseconds. */
uint64_t llam_now_ns(void);

/** @brief Return the runtime-assigned task id. */
uint64_t llam_task_id(const llam_task_t *task);

/** @brief Return a stable string for the task's current state. */
const char *llam_task_state_name(const llam_task_t *task);

/** @brief Return the task's scheduler class. */
llam_task_class_t llam_task_class(const llam_task_t *task);

/** @brief Return the currently running task, or NULL outside a LLAM task. */
llam_task_t *llam_current_task(void);

#ifdef __cplusplus
}
#endif

#endif
