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
 * functions returning @c -1 set @c errno to the failure reason. Managed task
 * context switches preserve @c errno as task-local state so a task does not
 * inherit another worker thread's TLS error value when it resumes.
 *
 * Common errno values:
 *  - @c EINVAL: invalid argument, invalid enum/policy value, or invalid handle use.
 *  - @c ETIMEDOUT: absolute deadline or relative timeout expired.
 *  - @c ECANCELED: cancellation token or cooperative runtime stop was observed.
 *  - @c EPIPE: channel closed or peer-closed equivalent.
 *  - @c EBUSY: object still has live users/waiters or incompatible concurrent use.
 *  - @c ENOTSUP: unsupported backend feature or invalid calling context.
 *  - @c ENOMEM: allocation failure.
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

/** @brief LLAM source/API version major component. */
#define LLAM_VERSION_MAJOR 1U
/** @brief LLAM source/API version minor component. */
#define LLAM_VERSION_MINOR 0U
/** @brief LLAM source/API version patch component. */
#define LLAM_VERSION_PATCH 0U

/** @brief Public ABI major version; incompatible binary changes increment this value. */
#define LLAM_ABI_VERSION_MAJOR 1U
/** @brief Public ABI minor version; additive binary-compatible changes increment this value. */
#define LLAM_ABI_VERSION_MINOR 0U
/** @brief Packed public ABI version used by dynamic loaders. */
#define LLAM_ABI_VERSION ((LLAM_ABI_VERSION_MAJOR << 16U) | LLAM_ABI_VERSION_MINOR)

/**
 * @brief Runtime ABI metadata returned by ::llam_abi_get_info.
 *
 * @details
 * Callers must pass the size of the struct they were compiled against. LLAM
 * copies only the overlapping prefix, so newer libraries can add fields without
 * breaking older language bindings.
 */
typedef struct llam_abi_info {
    uint32_t abi_major;                /**< Public ABI major version. */
    uint32_t abi_minor;                /**< Public ABI minor version. */
    uint32_t version_major;            /**< Source/API version major component. */
    uint32_t version_minor;            /**< Source/API version minor component. */
    uint32_t version_patch;            /**< Source/API version patch component. */
    uint32_t reserved0;                /**< Reserved for future flags; currently 0. */
    size_t struct_size;                /**< Size of this struct in the loaded library. */
    size_t runtime_opts_size;          /**< Size of ::llam_runtime_opts_t in the loaded library. */
    size_t spawn_opts_size;            /**< Size of ::llam_spawn_opts_t in the loaded library. */
    size_t runtime_stats_size;         /**< Size of ::llam_runtime_stats_t in the loaded library. */
    const char *runtime_name;          /**< Stable runtime name string, currently "LLAM". */
    const char *version_string;        /**< Static version string owned by the library. */
    const char *platform_name;         /**< Static platform name string owned by the library. */
} llam_abi_info_t;

/** @brief Current size to pass to ::llam_abi_get_info. */
#define LLAM_ABI_INFO_CURRENT_SIZE ((size_t)sizeof(llam_abi_info_t))

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

/** @brief Opaque handle for an explicit runtime instance. */
typedef struct llam_runtime llam_runtime_t;

/** @brief Opaque handle for structured task groups. */
typedef struct llam_task_group llam_task_group_t;

/** @brief Task-local storage key. */
typedef uint32_t llam_task_local_key_t;

/** @brief Invalid task-local storage key value. */
#define LLAM_TASK_LOCAL_INVALID_KEY UINT32_MAX

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

/* ============================================================================
 * ABI and dynamic loading
 * ============================================================================
 */

/**
 * @brief Return the packed public ABI version implemented by this library.
 *
 * Language runtimes that load LLAM dynamically should resolve this symbol first
 * and require a matching ::LLAM_ABI_VERSION_MAJOR before calling other symbols.
 *
 * @return Packed version `(major << 16) | minor`.
 */
LLAM_API uint32_t llam_abi_version(void);

/**
 * @brief Return the static LLAM source/API version string.
 * @return String owned by the library and valid until process exit.
 */
LLAM_API const char *llam_version_string(void);

/**
 * @brief Fill ABI metadata for dynamic loaders and FFI bindings.
 *
 * @param info      Destination metadata struct.
 * @param info_size Size of the caller's ::llam_abi_info_t definition.
 * @return 0 on success, -1 with @c errno set on invalid arguments.
 */
LLAM_API int llam_abi_get_info(llam_abi_info_t *info, size_t info_size);

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

/** @brief Give workers their own I/O rings/queues where supported. */
#define LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS UINT64_C(1)
/** @brief Allow multishot watches in worker-ring mode. */
#define LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS_MULTISHOT (UINT64_C(1) << 1)
/** @brief Soft-park idle workers and reactivate on pressure. */
#define LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS (UINT64_C(1) << 2)
/** @brief Use the lock-free normal run queue. */
#define LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ (UINT64_C(1) << 3)
/** @brief Prefer hugepage-friendly backing for selected allocators. */
#define LLAM_RUNTIME_EXPERIMENTAL_F_HUGE_ALLOC (UINT64_C(1) << 4)
/** @brief Linux io_uring SQPOLL experiment for node-owned rings. */
#define LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL (UINT64_C(1) << 5)

/** @brief Optional per-task spawn policy. */
typedef struct llam_spawn_opts {
    uint32_t task_class;                /**< Scheduler class; one of ::llam_task_class_t. */
    uint32_t stack_class;               /**< Fiber stack class; one of ::llam_stack_class_t. */
    uint32_t flags;                     /**< Bitwise OR of LLAM_SPAWN_F_* values. */
    uint64_t deadline_ns;               /**< Optional absolute deadline in llam_now_ns() units; 0 disables it. */
    llam_cancel_token_t *cancel_token;  /**< Optional cancellation token observed by waits and I/O. */
} llam_spawn_opts_t;

/** @brief Current size to pass to ::llam_spawn_ex and ::llam_spawn_opts_init. */
#define LLAM_SPAWN_OPTS_CURRENT_SIZE ((size_t)sizeof(llam_spawn_opts_t))

/** @brief Runtime initialization options. */
typedef struct llam_runtime_opts {
    uint32_t deterministic;                         /**< Prefer repeatable scheduling decisions. */
    uint32_t forced_yield_every;                    /**< Force cooperative yield after this many scheduler ticks; 0 disables it. */
    uint64_t experimental_flags;                    /**< Bitwise OR of LLAM_RUNTIME_EXPERIMENTAL_F_* values. */
    uint64_t idle_spin_ns;                          /**< Bounded idle spin duration before kernel sleep. */
    uint32_t idle_spin_max_iters;                   /**< Maximum idle-spin iterations. */
    int32_t sqpoll_cpu;                             /**< Requested SQPOLL CPU, or -1 for automatic selection. */
    uint32_t profile;                               /**< High-level policy; one of ::llam_runtime_profile_t. */
    uint32_t reserved0;                             /**< Reserved for future fixed-width fields; initialize to 0. */
} llam_runtime_opts_t;

/** @brief Current size to pass to ::llam_runtime_init_ex and ::llam_runtime_opts_init. */
#define LLAM_RUNTIME_OPTS_CURRENT_SIZE ((size_t)sizeof(llam_runtime_opts_t))

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
    uint32_t active_workers;            /**< Configured worker count. */
    uint32_t online_workers;            /**< Workers currently online. */
    uint32_t online_workers_floor;      /**< Minimum online-worker floor. */
    uint32_t online_workers_min;        /**< Minimum online workers observed. */
    uint32_t online_workers_max;        /**< Maximum online workers observed. */
    uint32_t active_nodes;              /**< Active platform I/O nodes. */
    uint32_t dynamic_workers;           /**< Whether dynamic workers are active. */
    uint32_t worker_rings;              /**< Whether worker ring mode is active. */
    uint32_t worker_rings_multishot;    /**< Whether worker-ring multishot mode is active. */
    uint32_t lockfree_normq;            /**< Whether lock-free normal queues are active. */
    uint32_t huge_alloc;                /**< Whether huge allocation mode is active. */
    uint32_t sqpoll;                    /**< Whether Linux SQPOLL mode is active. */
    uint64_t opaque_block_ns;           /**< Total opaque blocking time in nanoseconds. */
    uint64_t opaque_block_samples;      /**< Opaque blocking sample count. */
    uint64_t opaque_block_max_ns;       /**< Maximum opaque blocking duration. */
    uint64_t opaque_enter_wait_ns;      /**< Total wait time entering opaque blocking regions. */
    uint64_t opaque_enter_wait_samples; /**< Enter-wait sample count. */
    uint64_t opaque_enter_wait_max_ns;  /**< Maximum enter-wait duration. */
    uint64_t opaque_leave_wait_ns;      /**< Total wait time leaving opaque blocking regions. */
    uint64_t opaque_leave_wait_samples; /**< Leave-wait sample count. */
    uint64_t opaque_leave_wait_max_ns;  /**< Maximum leave-wait duration. */
    uint64_t yield_direct_attempts;     /**< Direct yield handoff attempts. */
    uint64_t yield_direct_fast_hits;    /**< Lock-free direct yield handoff hits. */
    uint64_t yield_direct_locked_hits;  /**< Locked direct yield handoff hits. */
    uint64_t yield_direct_fail_context; /**< Direct handoff failures from invalid context. */
    uint64_t yield_direct_fail_policy;  /**< Direct handoff failures from policy/state guards. */
    uint64_t yield_direct_fail_no_work; /**< Direct handoff failures with no local runnable work. */
    uint64_t yield_direct_fail_self;    /**< Direct handoff failures that only found the caller. */
    uint64_t yield_direct_fail_push;    /**< Direct handoff failures requeueing the caller. */
} llam_runtime_stats_t;

/** @brief Current size to pass to ::llam_runtime_collect_stats_ex. */
#define LLAM_RUNTIME_STATS_CURRENT_SIZE ((size_t)sizeof(llam_runtime_stats_t))

/* ============================================================================
 * Runtime lifecycle and task scheduling
 * ============================================================================
 */

/**
 * @brief Initialize runtime options to the library defaults.
 *
 * @details
 * This is the ABI-stable option initializer preferred by dynamic loaders and
 * FFI bindings. LLAM writes only the overlapping prefix of @p opts and zeroes
 * any caller-side tail beyond the current library's ::llam_runtime_opts_t.
 *
 * @param opts Destination options object. Must not be NULL.
 * @param opts_size Size of the caller's ::llam_runtime_opts_t definition.
 * @return 0 on success, -1 with @c errno set on invalid arguments.
 */
LLAM_API int llam_runtime_opts_init(llam_runtime_opts_t *opts, size_t opts_size);

/**
 * @brief Initialize spawn options to the library defaults.
 *
 * @details
 * This is the ABI-stable option initializer preferred by dynamic loaders and
 * FFI bindings. LLAM writes only the overlapping prefix of @p opts and zeroes
 * any caller-side tail beyond the current library's ::llam_spawn_opts_t.
 *
 * @param opts Destination options object. Must not be NULL.
 * @param opts_size Size of the caller's ::llam_spawn_opts_t definition.
 * @return 0 on success, -1 with @c errno set on invalid arguments.
 */
LLAM_API int llam_spawn_opts_init(llam_spawn_opts_t *opts, size_t opts_size);

/**
 * @brief Initialize global runtime state with an explicit option size.
 *
 * @details
 * This is the ABI-stable form preferred by dynamic loaders and FFI bindings.
 * LLAM copies only the overlapping prefix of @p opts and treats missing tail
 * fields as runtime defaults, so older bindings can run against newer
 * libraries that appended fields to ::llam_runtime_opts_t.
 *
 * @param opts Optional runtime options; pass NULL for defaults.
 * @param opts_size Size of the caller's ::llam_runtime_opts_t definition.
 * @return 0 on success, -1 on failure with errno set.
 */
LLAM_API int llam_runtime_init_ex(const llam_runtime_opts_t *opts, size_t opts_size);

/**
 * @brief Initialize global runtime state.
 * @param opts Optional runtime options; pass NULL for defaults.
 * @details Convenience wrapper around ::llam_runtime_init_ex.
 * @return 0 on success, -1 on failure with errno set.
 */
LLAM_API int llam_runtime_init(const llam_runtime_opts_t *opts);

/**
 * @brief Request cooperative runtime stop and wake all workers.
 *
 * @details
 * This requests a clean scheduler stop. It does not forcibly terminate live
 * tasks; cancellable waits may observe @c ECANCELED and user code is expected
 * to unwind cooperatively.
 *
 * @return 0 on success, -1 with @c errno set when the runtime is not initialized.
 */
LLAM_API int llam_runtime_request_stop(void);

/**
 * @brief Stop workers and release runtime-owned resources.
 *
 * @details
 * Shutdown is idempotent and is also valid after a failed or partial
 * initialization. It requests cooperative stop, joins runtime-owned OS threads
 * that were started, releases backend resources, and invalidates all remaining
 * runtime-owned handles. It is not a task-join API: callers that need graceful
 * task completion should request stop and drive ::llam_run before shutdown.
 */
LLAM_API void llam_runtime_shutdown(void);

/**
 * @brief Collect a best-effort snapshot of runtime counters with explicit size.
 *
 * @details
 * This is the ABI-stable form preferred by dynamic loaders and FFI bindings.
 * LLAM writes only the overlapping prefix of @p stats and zeroes any caller
 * tail beyond the current library's ::llam_runtime_stats_t.
 *
 * @param stats Destination statistics object. Must not be NULL.
 * @param stats_size Size of the caller's ::llam_runtime_stats_t definition.
 * @return 0 on success, -1 on failure with errno set.
 */
LLAM_API int llam_runtime_collect_stats_ex(llam_runtime_stats_t *stats, size_t stats_size);

/**
 * @brief Collect a best-effort snapshot of runtime counters.
 * @param stats Destination statistics object. Must not be NULL.
 * @details Convenience wrapper around ::llam_runtime_collect_stats_ex.
 * @return 0 on success, -1 on failure with errno set.
 */
LLAM_API int llam_runtime_collect_stats(llam_runtime_stats_t *stats);

/**
 * @brief Return the process-global runtime handle.
 *
 * @details
 * LLAM 1.x still uses one runtime singleton internally. The handle API exists
 * so embedders can move to explicit handles without changing call sites later.
 *
 * @return The default runtime handle.
 */
LLAM_API llam_runtime_t *llam_runtime_default(void);

/**
 * @brief Create an explicit runtime handle.
 *
 * @details
 * Current LLAM builds support only one live runtime per process. This function
 * initializes the singleton and returns its handle. A second live runtime fails
 * with @c EBUSY. Future ABI-compatible versions may allocate independent
 * runtime objects behind this handle.
 *
 * @param opts Optional runtime options; pass NULL for defaults.
 * @param opts_size Size of the caller's ::llam_runtime_opts_t definition.
 * @param out Destination runtime handle. Must not be NULL.
 * @return 0 on success, -1 with @c errno set.
 */
LLAM_API int llam_runtime_create(const llam_runtime_opts_t *opts, size_t opts_size, llam_runtime_t **out);

/**
 * @brief Run a runtime handle.
 *
 * @details Current builds accept only ::llam_runtime_default().
 */
LLAM_API int llam_runtime_run_handle(llam_runtime_t *runtime);

/**
 * @brief Destroy a runtime handle.
 *
 * @details Current builds accept only ::llam_runtime_default() and delegate to
 * ::llam_runtime_shutdown.
 */
LLAM_API void llam_runtime_destroy(llam_runtime_t *runtime);

/**
 * @brief Write runtime counters as one JSON object to an fd.
 *
 * @details
 * The JSON field set is additive and intended for dashboards, stress logs, and
 * automation that should not parse ::llam_dump_runtime_state. The call writes a
 * single newline-terminated object. Managed and unmanaged callers are both
 * allowed.
 *
 * @param fd Destination file descriptor.
 * @return 0 on success, -1 on invalid fd/write failure with errno set.
 */
LLAM_API int llam_runtime_write_stats_json(int fd);

/**
 * @brief Create a task and make it runnable with an explicit option size.
 *
 * @details
 * This is the ABI-stable form preferred by dynamic loaders and FFI bindings.
 * LLAM copies only the overlapping prefix of @p opts and treats missing tail
 * fields as spawn defaults, so older bindings can run against newer
 * libraries that appended fields to ::llam_spawn_opts_t.
 *
 * @param fn Task entry point. Must not be NULL.
 * @param arg User pointer passed to fn.
 * @param opts Optional spawn options; pass NULL for defaults.
 * @param opts_size Size of the caller's ::llam_spawn_opts_t definition.
 * @return Task handle on success, NULL on failure with errno set.
 */
LLAM_API llam_task_t *llam_spawn_ex(llam_task_fn fn, void *arg, const llam_spawn_opts_t *opts, size_t opts_size);

/**
 * @brief Create a task and make it runnable.
 * @param fn Task entry point. Must not be NULL.
 * @param arg User pointer passed to fn.
 * @param opts Optional spawn options; pass NULL for defaults.
 * @details Convenience wrapper around ::llam_spawn_ex.
 * @return Task handle on success, NULL on failure with errno set.
 */
LLAM_API llam_task_t *llam_spawn(llam_task_fn fn, void *arg, const llam_spawn_opts_t *opts);

/**
 * @brief Run the scheduler until all runtime work completes or an error occurs.
 *
 * @details
 * A runtime stop requested by ::llam_runtime_request_stop is a clean scheduler
 * outcome and returns @c 0. Backend or fatal runtime failures return @c -1 and
 * set @c errno. This function drives the scheduler on the calling OS thread;
 * joins from unmanaged OS threads do not drive scheduler progress by
 * themselves.
 *
 * @return 0 on clean completion or cooperative stop, -1 on failure with errno set.
 */
LLAM_API int llam_run(void);

/**
 * @brief Cooperatively yield the current task.
 *
 * @details Calls outside a managed LLAM task are a no-op.
 */
LLAM_API void llam_yield(void);

/**
 * @brief Execute a cooperative safepoint without requiring an immediate yield.
 *
 * @details
 * CPU-bound managed tasks should call this periodically from long loops so the
 * watchdog, cancellation, allocator quiescence, and cooperative preemption
 * machinery can observe progress. Calls outside a managed LLAM task are a
 * no-op.
 */
LLAM_API void llam_task_safepoint(void);

/**
 * @brief Wait indefinitely for task completion.
 * @param task Task returned by llam_spawn().
 * @details
 * A successful join consumes @p task; the pointer must not be used again.
 * Managed-task callers park cooperatively. Unmanaged OS-thread callers block
 * until the target is already complete or another thread is actively driving
 * ::llam_run; unmanaged joins do not run the scheduler.
 * @return 0 on completion, -1 on failure with errno set.
 */
LLAM_API int llam_join(llam_task_t *task);

/**
 * @brief Wait for task completion until an absolute deadline.
 * @param task Task returned by llam_spawn().
 * @param deadline_ns Absolute deadline in llam_now_ns() units.
 * @details
 * A successful timed join consumes @p task. If the call fails with @c ETIMEDOUT,
 * the task remains joinable by the same handle. @p deadline_ns is an absolute
 * ::llam_now_ns deadline; @c 0 is treated as an already-expired deadline.
 * @return 0 on completion, -1 on timeout/failure with errno set.
 */
LLAM_API int llam_join_until(llam_task_t *task, uint64_t deadline_ns);

/**
 * @brief Detach a task handle so completion no longer requires join.
 * @param task Task returned by llam_spawn().
 * @details
 * A successful detach consumes @p task; the pointer must not be used again.
 * Detached tasks still run to completion and remain counted as live runtime
 * work until their entry function returns.
 * @return 0 on detach, -1 on failure with errno set.
 */
LLAM_API int llam_detach(llam_task_t *task);

/**
 * @brief Create a structured task group.
 *
 * @details
 * A group owns the task handles spawned through it. Use
 * ::llam_task_group_join to consume all child handles, or
 * ::llam_task_group_cancel to request cooperative cancellation first.
 */
LLAM_API llam_task_group_t *llam_task_group_create(void);

/**
 * @brief Destroy an empty task group.
 *
 * @return 0 on success, or -1 with @c errno set to @c EBUSY if unjoined tasks
 * remain.
 */
LLAM_API int llam_task_group_destroy(llam_task_group_t *group);

/**
 * @brief Spawn a task owned by a group with explicit option size.
 *
 * @details
 * If @p opts does not provide a cancellation token, the group cancellation token
 * is attached automatically. The returned task pointer is borrowed for
 * diagnostics; callers must not join or detach it outside the group.
 */
LLAM_API llam_task_t *llam_task_group_spawn_ex(llam_task_group_t *group,
                                      llam_task_fn fn,
                                      void *arg,
                                      const llam_spawn_opts_t *opts,
                                      size_t opts_size);

/**
 * @brief Spawn a task owned by a group.
 */
LLAM_API llam_task_t *llam_task_group_spawn(llam_task_group_t *group,
                                   llam_task_fn fn,
                                   void *arg,
                                   const llam_spawn_opts_t *opts);

/**
 * @brief Request cooperative cancellation for all group children.
 */
LLAM_API int llam_task_group_cancel(llam_task_group_t *group);

/**
 * @brief Join all tasks owned by a group.
 *
 * @details Successful joins consume the child task handles. If an error occurs,
 * the failed and remaining handles stay owned by the group.
 */
LLAM_API int llam_task_group_join(llam_task_group_t *group);

/**
 * @brief Join all tasks owned by a group until an absolute deadline.
 *
 * @details
 * @p deadline_ns is an absolute ::llam_now_ns deadline. @c 0 is treated as an
 * already-expired deadline, matching ::llam_join_until. Successful joins consume
 * completed child task handles. If the deadline expires or another join error
 * occurs, the failed and remaining handles stay owned by the group.
 *
 * @return 0 when every child was joined, or -1 with @c errno set.
 */
LLAM_API int llam_task_group_join_until(llam_task_group_t *group, uint64_t deadline_ns);

/**
 * @brief Sleep the current task until an absolute deadline.
 *
 * @details
 * Calls from managed LLAM tasks park cooperatively. Calls outside a managed
 * task block the calling OS thread after runtime initialization; before
 * initialization they fail with @c EINVAL. @p deadline_ns is an absolute
 * ::llam_now_ns deadline; @c 0 is treated as an already-expired deadline.
 *
 * @param deadline_ns Absolute deadline in llam_now_ns() units.
 * @return 0 on wake, -1 on cancellation/failure with errno set.
 */
LLAM_API int llam_sleep_until(uint64_t deadline_ns);

/**
 * @brief Sleep the current task for a relative duration.
 *
 * @details
 * Calls from managed LLAM tasks park cooperatively. Calls outside a managed
 * task block the calling OS thread after runtime initialization; before
 * initialization they fail with @c EINVAL.
 *
 * @param duration_ns Sleep duration in nanoseconds.
 * @return 0 on wake, -1 on cancellation/failure with errno set.
 */
LLAM_API int llam_sleep_ns(uint64_t duration_ns);

/**
 * @brief Execute a blocking callback through the runtime blocking path.
 *
 * @details
 * This is the unambiguous FFI-safe blocking API. A callback that legitimately
 * returns @c NULL still succeeds and stores @c NULL in @p out.
 *
 * @param fn Blocking callback. Must not be NULL.
 * @param arg User pointer passed to fn.
 * @param out Destination for the callback result. Must not be NULL.
 * @return 0 on callback completion, -1 on submission/cancellation failure with
 *         @c errno set.
 */
LLAM_API int llam_call_blocking_result(llam_blocking_fn fn, void *arg, void **out);

/**
 * @brief Execute a blocking callback through the runtime blocking path.
 *
 * @details
 * Convenience wrapper around ::llam_call_blocking_result. It cannot distinguish
 * a successful callback result of @c NULL from an API failure; FFI bindings and
 * production code should prefer ::llam_call_blocking_result.
 *
 * @param fn Blocking callback. Must not be NULL.
 * @param arg User pointer passed to fn.
 * @return Callback result, or NULL if the callback returned NULL or submission failed.
 */
LLAM_API void *llam_call_blocking(llam_blocking_fn fn, void *arg);

/**
 * @brief Mark the current task as entering an opaque blocking region.
 *
 * @details Calls outside a managed LLAM task are no-ops and return @c 0.
 *
 * @return 0 on success, -1 on failure with errno set.
 */
LLAM_API int llam_enter_blocking(void);

/**
 * @brief Mark the current task as leaving an opaque blocking region.
 *
 * @details Calls outside a managed LLAM task are no-ops and return @c 0.
 *
 * @return 0 on success, -1 on failure with errno set.
 */
LLAM_API int llam_leave_blocking(void);

/**
 * @brief Change the current task's scheduler class using a ::llam_task_class_t value.
 * @return 0 on success, -1 with @c errno set to @c EINVAL or @c ENOTSUP.
 */
LLAM_API int llam_task_set_class(uint32_t task_class);

/**
 * @brief Write a human-readable runtime dump to an fd.
 * @param fd Destination file descriptor.
 */
LLAM_API void llam_dump_runtime_state(int fd);

/**
 * @brief Return task spawn/runtime flags.
 * @param task Task handle.
 * @return Bitwise OR of LLAM_SPAWN_F_* values, or 0 for NULL.
 */
LLAM_API uint32_t llam_task_flags(const llam_task_t *task);

/**
 * @brief Allocate a task-local storage key.
 */
LLAM_API int llam_task_local_key_create(llam_task_local_key_t *out_key);

/**
 * @brief Delete a task-local storage key.
 *
 * @details Existing per-task values for the key are discarded when each task
 * exits or sets the key to NULL.
 */
LLAM_API int llam_task_local_key_delete(llam_task_local_key_t key);

/**
 * @brief Return the current task's value for a task-local key.
 *
 * @details Must be called from a managed LLAM task; outside task context sets
 * @c errno to @c ENOTSUP and returns NULL.
 */
LLAM_API void *llam_task_local_get(llam_task_local_key_t key);

/**
 * @brief Set the current task's value for a task-local key.
 *
 * @details Passing NULL clears the key for the current task. Must be called
 * from a managed LLAM task.
 */
LLAM_API int llam_task_local_set(llam_task_local_key_t key, void *value);

/* ============================================================================
 * Cancellation tokens
 * ============================================================================
 */

/** @brief Create a cancellation token. */
LLAM_API llam_cancel_token_t *llam_cancel_token_create(void);

/**
 * @brief Destroy a cancellation token.
 *
 * @details
 * The token must not have live waiters or task/I/O observers. Destroying a
 * token with active observers fails with @c EBUSY; it does not invalidate
 * active observers behind the caller's back.
 */
LLAM_API int llam_cancel_token_destroy(llam_cancel_token_t *token);

/** @brief Request cancellation for all current and future observers of token. */
LLAM_API int llam_cancel_token_cancel(llam_cancel_token_t *token);

/** @brief Return non-zero when token has been cancelled. */
LLAM_API int llam_cancel_token_is_cancelled(const llam_cancel_token_t *token);

/* ============================================================================
 * Runtime-aware mutexes and condition variables
 * ============================================================================
 */

/** @brief Create a runtime-aware mutex. */
LLAM_API llam_mutex_t *llam_mutex_create(void);

/**
 * @brief Destroy a runtime-aware mutex.
 *
 * @details
 * Destroy fails with @c EBUSY while a task owns or waits on the mutex.
 */
LLAM_API int llam_mutex_destroy(llam_mutex_t *mutex);

/**
 * @brief Lock a runtime-aware mutex, parking the task if needed.
 *
 * @details Must be called from a managed LLAM task; outside task context fails
 * with @c ENOTSUP. LLAM mutexes are non-recursive; locking a mutex already
 * owned by the current task fails with @c EDEADLK. Contended locks apply a
 * bounded priority-donation hint from latency-class waiters to the owner until
 * unlock.
 */
LLAM_API int llam_mutex_lock(llam_mutex_t *mutex);

/**
 * @brief Lock a runtime-aware mutex until an absolute deadline.
 *
 * @details Must be called from a managed LLAM task; outside task context fails
 * with @c ENOTSUP. @p deadline_ns is an absolute ::llam_now_ns deadline; @c 0
 * is treated as an already-expired deadline. LLAM mutexes are non-recursive;
 * locking a mutex already owned by the current task fails with @c EDEADLK.
 * Contended locks apply the same bounded priority-donation hint as
 * ::llam_mutex_lock.
 */
LLAM_API int llam_mutex_lock_until(llam_mutex_t *mutex, uint64_t deadline_ns);

/**
 * @brief Try to lock a runtime-aware mutex without parking.
 *
 * @return 0 on lock acquisition, or -1 with @c errno set to @c EBUSY when
 * already locked, @c EINVAL for invalid arguments, or @c ENOTSUP outside a
 * managed task.
 */
LLAM_API int llam_mutex_trylock(llam_mutex_t *mutex);

/**
 * @brief Unlock a runtime-aware mutex and wake a waiter if one exists.
 *
 * @return 0 on success, or -1 with @c errno set to @c EPERM when the current
 * task does not own the mutex, @c EINVAL for invalid arguments, or @c ENOTSUP
 * outside a managed task.
 */
LLAM_API int llam_mutex_unlock(llam_mutex_t *mutex);

/** @brief Create a runtime-aware condition variable. */
LLAM_API llam_cond_t *llam_cond_create(void);

/**
 * @brief Destroy a runtime-aware condition variable.
 *
 * @details
 * Destroy fails with @c EBUSY while a task is currently waiting on the
 * condition.
 */
LLAM_API int llam_cond_destroy(llam_cond_t *cond);

/**
 * @brief Wait on a condition variable and atomically release/reacquire mutex.
 *
 * @details Must be called from a managed LLAM task; outside task context fails
 * with @c ENOTSUP. The caller must own @p mutex on entry. LLAM atomically
 * releases it while waiting and reacquires it before returning, including
 * signal, broadcast, timeout, cancellation, and spurious wake paths. Callers
 * must wait in a predicate loop.
 */
LLAM_API int llam_cond_wait(llam_cond_t *cond, llam_mutex_t *mutex);

/**
 * @brief Wait on a condition variable until an absolute deadline.
 *
 * @details Must be called from a managed LLAM task; outside task context fails
 * with @c ENOTSUP. The caller must own @p mutex on entry. LLAM atomically
 * releases it while waiting and reacquires it before returning, including
 * signal, broadcast, timeout, cancellation, and spurious wake paths. Callers
 * must wait in a predicate loop.
 * @p deadline_ns is an absolute ::llam_now_ns deadline; @c 0 is treated as an
 * already-expired deadline.
 */
LLAM_API int llam_cond_wait_until(llam_cond_t *cond, llam_mutex_t *mutex, uint64_t deadline_ns);

/**
 * @brief Wake one condition-variable waiter.
 *
 * @details May be called with or without the associated mutex held. Calls
 * outside a managed LLAM task are allowed.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL for invalid
 * arguments.
 */
LLAM_API int llam_cond_signal(llam_cond_t *cond);

/**
 * @brief Wake all condition-variable waiters.
 *
 * @details May be called with or without the associated mutex held. Calls
 * outside a managed LLAM task are allowed.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL for invalid
 * arguments.
 */
LLAM_API int llam_cond_broadcast(llam_cond_t *cond);

/* ============================================================================
 * Channels
 * ============================================================================
 */

/**
 * @brief Create a pointer-valued channel.
 * @param capacity Number of bounded buffer slots. Must be at least 1.
 * @return Channel handle on success, NULL on failure with errno set.
 */
LLAM_API llam_channel_t *llam_channel_create(size_t capacity);

/**
 * @brief Destroy a channel after all users have stopped accessing it.
 *
 * @details Destroy fails with @c EBUSY while buffered values or parked
 * senders/receivers remain. Close the channel and drain buffered values before
 * destroying when producers may have sent data.
 */
LLAM_API int llam_channel_destroy(llam_channel_t *channel);

/**
 * @brief Send a pointer value, parking the task if the channel is full.
 *
 * @details Must be called from a managed LLAM task; outside task context fails
 * with @c ENOTSUP.
 */
LLAM_API int llam_channel_send(llam_channel_t *channel, void *value);

/**
 * @brief Send a pointer value until an absolute deadline.
 *
 * @details Must be called from a managed LLAM task; outside task context fails
 * with @c ENOTSUP. @p deadline_ns is an absolute ::llam_now_ns deadline; @c 0
 * is treated as an already-expired deadline.
 */
LLAM_API int llam_channel_send_until(llam_channel_t *channel, void *value, uint64_t deadline_ns);

/**
 * @brief Receive a pointer value, parking the task if the channel is empty.
 *
 * @details Must be called from a managed LLAM task; outside task context fails
 * with @c ENOTSUP.
 *
 * @param channel Channel to receive from.
 * @param out Destination for the received pointer. Must not be NULL.
 * @return 0 on receive, -1 on close/cancellation/failure with @c errno set.
 */
LLAM_API int llam_channel_recv_result(llam_channel_t *channel, void **out);

/**
 * @brief Receive a pointer value until an absolute deadline.
 *
 * @details Must be called from a managed LLAM task; outside task context fails
 * with @c ENOTSUP. @p deadline_ns is an absolute ::llam_now_ns deadline; @c 0
 * is treated as an already-expired deadline.
 *
 * @param channel Channel to receive from.
 * @param deadline_ns Absolute deadline in llam_now_ns() units.
 * @param out Destination for the received pointer. Must not be NULL.
 * @return 0 on receive, -1 on close/timeout/cancellation/failure with @c errno set.
 */
LLAM_API int llam_channel_recv_until_result(llam_channel_t *channel, uint64_t deadline_ns, void **out);

/**
 * @brief Receive a pointer value, parking the task if the channel is empty.
 *
 * @details
 * Convenience wrapper around ::llam_channel_recv_result. If @c NULL is a valid
 * payload for your channel, use ::llam_channel_recv_result to avoid ambiguity.
 * Outside task context returns @c NULL with @c errno set to @c ENOTSUP.
 */
LLAM_API void *llam_channel_recv(llam_channel_t *channel);

/**
 * @brief Receive a pointer value until an absolute deadline.
 *
 * @details
 * Convenience wrapper around ::llam_channel_recv_until_result. If @c NULL is a
 * valid payload for your channel, use ::llam_channel_recv_until_result.
 * Outside task context returns @c NULL with @c errno set to @c ENOTSUP.
 * @p deadline_ns is an absolute ::llam_now_ns deadline; @c 0 is treated as an
 * already-expired deadline.
 */
LLAM_API void *llam_channel_recv_until(llam_channel_t *channel, uint64_t deadline_ns);

/**
 * @brief Close a channel and wake blocked senders/receivers.
 *
 * @details
 * Closing is idempotent. Sends after close fail with @c EPIPE. Buffered values
 * sent before close remain drainable; receives fail with @c EPIPE only after
 * the buffer is empty and no parked sender can hand off a value. @c NULL is a
 * valid payload; use result-style receive APIs to distinguish it from failure.
 */
LLAM_API int llam_channel_close(llam_channel_t *channel);

/** @brief Channel select operation kind. */
typedef enum llam_select_op_kind {
    LLAM_SELECT_OP_RECV = 1, /**< Try to receive from channel. */
    LLAM_SELECT_OP_SEND = 2, /**< Try to send to channel. */
} llam_select_op_kind_t;

/**
 * @brief One channel operation passed to ::llam_channel_select.
 */
typedef struct llam_select_op {
    uint32_t kind;                /**< One of ::llam_select_op_kind_t. */
    uint32_t reserved0;           /**< Reserved; initialize to 0. */
    llam_channel_t *channel;      /**< Channel to operate on. */
    void *send_value;             /**< Payload for send operations. */
    void **recv_out;              /**< Output pointer for receive operations. */
    int result_errno;             /**< Per-operation terminal errno, or 0. */
} llam_select_op_t;

/**
 * @brief Select one ready channel send/receive operation.
 *
 * @details
 * The current managed task is parked on all requested channel queues when no
 * operation is immediately ready. The first channel operation to complete wins;
 * the selected operation index is returned and remaining queued wait nodes are
 * removed before this call returns. @p deadline_ns is an absolute ::llam_now_ns
 * deadline; @c 0 performs a single non-blocking scan.
 *
 * @param ops Operation array.
 * @param op_count Number of operations.
 * @param deadline_ns Absolute deadline, or UINT64_MAX for no deadline.
 * @param selected_index Receives selected operation index. Must not be NULL.
 * @return 0 when an operation is selected, -1 on timeout/failure with errno set.
 */
LLAM_API int llam_channel_select(llam_select_op_t *ops,
                        size_t op_count,
                        uint64_t deadline_ns,
                        size_t *selected_index);

/* ============================================================================
 * Runtime I/O and owned buffers
 * ============================================================================
 */

/**
 * @brief Read from fd using the runtime I/O backend where possible.
 *
 * @details Managed tasks first try a direct nonblocking fast path, then the
 * platform backend, then a blocking helper so the scheduler worker is not
 * pinned. Calls outside a managed LLAM task delegate to the platform read
 * primitive directly and may block the calling OS thread.
 */
LLAM_API ssize_t llam_read(llam_fd_t fd, void *buf, size_t count);

/**
 * @brief Wait for read readiness and read in one runtime operation.
 *
 * @details
 * Managed tasks first try an immediate nonblocking read. If the descriptor is
 * not ready, LLAM waits for @c POLLIN and then retries the read directly,
 * avoiding the duplicate safepoint/readiness checks of a separate
 * ::llam_poll_fd + ::llam_read pair. Calls outside a managed LLAM task delegate
 * to platform poll/read primitives and may block the calling OS thread.
 *
 * @param fd         File descriptor to read from.
 * @param buf        Destination buffer.
 * @param count      Maximum bytes to read.
 * @param timeout_ms Timeout in milliseconds; negative means infinite.
 *
 * @return Number of bytes read.
 * @return -1 with @c errno set to @c ETIMEDOUT if the readiness wait expires,
 *         or another error from the poll/read path.
 */
LLAM_API ssize_t llam_read_when_ready(llam_fd_t fd, void *buf, size_t count, int timeout_ms);

/**
 * @brief Write to fd using the runtime I/O backend where possible.
 *
 * @details Managed tasks first try a direct nonblocking fast path, then the
 * platform backend, then a blocking helper so the scheduler worker is not
 * pinned. Calls outside a managed LLAM task delegate to the platform write
 * primitive directly and may block the calling OS thread.
 */
LLAM_API ssize_t llam_write(llam_fd_t fd, const void *buf, size_t count);

/**
 * @brief Read into a runtime-owned buffer.
 *
 * @details
 * On success with a positive byte count, @p out receives a non-NULL buffer that
 * must be released with ::llam_io_buffer_release. On EOF or zero-byte read, the
 * function returns @c 0 and stores @c NULL in @p out. On failure, the function
 * returns @c -1, stores @c NULL in @p out, and sets @c errno.
 */
LLAM_API ssize_t llam_read_owned(llam_fd_t fd, size_t max_count, llam_io_buffer_t **out);

/**
 * @brief Receive into a runtime-owned buffer with recv flags.
 *
 * @details
 * On success with a positive byte count, @p out receives a non-NULL buffer that
 * must be released with ::llam_io_buffer_release. On EOF or zero-byte receive,
 * the function returns @c 0 and stores @c NULL in @p out. On failure, the
 * function returns @c -1, stores @c NULL in @p out, and sets @c errno.
 */
LLAM_API ssize_t llam_recv_owned(llam_fd_t fd, size_t max_count, int flags, llam_io_buffer_t **out);

/** @brief Release a runtime-owned I/O buffer. */
LLAM_API void llam_io_buffer_release(llam_io_buffer_t *buffer);

/** @brief Return the data pointer for a runtime-owned I/O buffer. */
LLAM_API void *llam_io_buffer_data(llam_io_buffer_t *buffer);

/** @brief Return the number of valid bytes in a runtime-owned I/O buffer. */
LLAM_API size_t llam_io_buffer_size(const llam_io_buffer_t *buffer);

/** @brief Return total capacity of a runtime-owned I/O buffer. */
LLAM_API size_t llam_io_buffer_capacity(const llam_io_buffer_t *buffer);

/**
 * @brief Accept a connection from a listener fd using the runtime I/O backend where possible.
 *
 * @details Managed tasks submit to the platform backend where possible and
 * otherwise use a blocking helper so the scheduler worker is not pinned. Calls
 * outside a managed LLAM task delegate to the platform accept primitive directly
 * and may block the calling OS thread.
 *
 * @return Accepted descriptor on success, or @c LLAM_INVALID_FD on failure with
 * @c errno set.
 */
LLAM_API llam_fd_t llam_accept(llam_fd_t fd, struct sockaddr *addr, socklen_t *addrlen);

/**
 * @brief Connect a socket without blocking the scheduler worker.
 *
 * Managed tasks submit the connection attempt to the runtime backend where
 * possible and otherwise use the blocking-worker fallback. Calls outside a
 * managed task delegate to @c connect directly.
 *
 * @param fd      Socket descriptor.
 * @param addr    Peer socket address. Must not be NULL.
 * @param addrlen Size of @p addr in bytes.
 * @return 0 on connection, -1 with @c errno set on failure.
 */
LLAM_API int llam_connect(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen);

/**
 * @brief Wait for fd readiness.
 *
 * @details
 * Calls from managed LLAM tasks park cooperatively. Calls outside a managed
 * task delegate to the platform poll/select backend and may block the calling
 * OS thread. @p timeout_ms < 0 waits indefinitely, @p timeout_ms == 0 performs
 * a non-blocking readiness check, and positive values bound the wait in
 * milliseconds.
 */
LLAM_API int llam_poll_fd(llam_fd_t fd, short events, int timeout_ms, short *revents);

/* ============================================================================
 * Time and task introspection
 * ============================================================================
 */

/** @brief Return a monotonic timestamp in nanoseconds. */
LLAM_API uint64_t llam_now_ns(void);

/** @brief Return the runtime-assigned task id. */
LLAM_API uint64_t llam_task_id(const llam_task_t *task);

/** @brief Return a stable string for the task's current state. */
LLAM_API const char *llam_task_state_name(const llam_task_t *task);

/** @brief Return the task's scheduler class as a ::llam_task_class_t value. */
LLAM_API uint32_t llam_task_class(const llam_task_t *task);

/** @brief Return the currently running task, or NULL outside a LLAM task. */
LLAM_API llam_task_t *llam_current_task(void);

#ifdef __cplusplus
}
#endif

#endif
