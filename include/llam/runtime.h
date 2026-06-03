/**
 * @file include/llam/runtime.h
 * @brief Canonical public LLAM runtime API: lifecycle, task scheduling, synchronization, I/O, time, debug, and platform hooks.
 *
 * @details
 * LLAM exposes stackful cooperative tasks backed by a scheduler/runtime that can
 * integrate blocking callbacks, runtime-aware synchronization primitives, and
 * platform I/O backends. This header is the canonical public API.
 *
 * Typical lifecycle: initialize a runtime, spawn work, drive it, then shut it
 * down. Legacy wrappers target ::llam_runtime_default; embedders should prefer
 * explicit runtime handles.
 *
 * All absolute deadlines use ::llam_now_ns units. Unless explicitly documented,
 * functions returning @c -1 set @c errno to the failure reason. Managed task
 * context switches preserve @c errno as task-local state so a task does not
 * inherit another worker thread's TLS error value when it resumes.
 *
 * Common errno values: @c EINVAL invalid input/handle, @c EAGAIN would block,
 * @c ETIMEDOUT deadline expired, @c ECANCELED cancellation/runtime stop,
 * @c EPIPE closed channel/peer, @c EBUSY live users or incompatible concurrent
 * use, @c EXDEV wrong runtime owner, @c ENOTSUP unsupported context/backend,
 * and @c ENOMEM allocation failure.
 *
 * Runtime-aware objects are bound to the runtime that created them; using an
 * object from another managed runtime fails with @c EXDEV.
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

#define LLAM_VERSION_MAJOR 2U
#define LLAM_VERSION_MINOR 0U
#define LLAM_VERSION_PATCH 0U

#define LLAM_ABI_VERSION_MAJOR 2U
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

/* Public opaque handles. */
typedef struct llam_task llam_task_t;
typedef struct llam_mutex llam_mutex_t;
typedef struct llam_cond llam_cond_t;
typedef struct llam_channel llam_channel_t;
typedef struct llam_cancel_token llam_cancel_token_t;
typedef struct llam_io_buffer llam_io_buffer_t;
typedef struct llam_runtime llam_runtime_t;
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

/** @brief Cooperative automatic preemption policy. */
typedef enum llam_preempt_mode {
    LLAM_PREEMPT_OFF = 0,         /**< Disable automatic preemption requests. */
    LLAM_PREEMPT_COOPERATIVE = 1, /**< Honor explicit safepoints, but do not auto-request preemption. */
    LLAM_PREEMPT_AUTO = 2,        /**< Request preemption only when runnable/timer/I/O pressure exists. */
    LLAM_PREEMPT_STRICT = 3,      /**< Diagnostic mode: preempt over-budget tasks even without pressure. */
} llam_preempt_mode_t;

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
    uint32_t reserved0;                 /**< Reserved ABI padding; initialize to 0, ignored by this version. */
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
    uint32_t preempt_mode;                          /**< Cooperative preemption policy; one of ::llam_preempt_mode_t. */
    uint32_t preempt_poll_period;                   /**< Safepoint flag-poll period; 0 selects a profile default. */
    uint64_t preempt_quantum_ns;                    /**< Global preempt slice override; 0 uses task-class budgets. */
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
    uint64_t preempt_requests;          /**< Automatic preemption requests published by safepoints/watchdog. */
    uint64_t preempt_yields;            /**< Safepoints that yielded because of automatic preemption. */
    uint64_t preempt_suppressed;        /**< Over-budget observations suppressed by policy or lack of pressure. */
    uint64_t preempt_signals;           /**< Worker wake signals sent for watchdog preemption requests. */
    uint32_t preempt_mode;              /**< Active ::llam_preempt_mode_t policy. */
    uint32_t preempt_poll_period;       /**< Active preempt flag-poll period. */
    uint64_t preempt_quantum_ns;        /**< Active global preempt slice override, or 0 for task-class budgets. */
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
 * FFI bindings. LLAM writes only the overlapping prefix known to the loaded
 * library; caller-side tail bytes from newer struct definitions are left
 * untouched.
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
 * FFI bindings. LLAM writes only the overlapping prefix known to the loaded
 * library; caller-side tail bytes from newer struct definitions are left
 * untouched.
 *
 * @param opts Destination options object. Must not be NULL.
 * @param opts_size Size of the caller's ::llam_spawn_opts_t definition.
 * @return 0 on success, -1 with @c errno set on invalid arguments.
 */
LLAM_API int llam_spawn_opts_init(llam_spawn_opts_t *opts, size_t opts_size);

/**
 * @brief Initialize the process-default runtime with an explicit option size.
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
 * @brief Initialize the process-default runtime.
 * @param opts Optional runtime options; pass NULL for defaults.
 * @details Convenience wrapper around ::llam_runtime_init_ex.
 * @return 0 on success, -1 on failure with errno set.
 */
LLAM_API int llam_runtime_init(const llam_runtime_opts_t *opts);

/**
 * @brief Request cooperative runtime stop and wake all workers.
 *
 * @details
 * This requests clean scheduler stop without forcibly terminating live tasks.
 * Waits may observe @c ECANCELED; unmanaged callers target the default runtime,
 * while managed tasks target their owner runtime.
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
 * that were started, and releases backend resources. It is not a task-join API:
 * callers that need graceful task completion should request stop and drive
 * ::llam_run before shutdown. Public cleanup handles remain governed by their
 * own contracts after shutdown: task handles are no longer joinable, owned I/O
 * buffers remain valid until release, and channels may still be drained with
 * ::llam_channel_try_recv_result before destroy.
 *
 * Calls from a managed LLAM task or scheduler frame do not tear the owning
 * runtime down directly. They are treated as ::llam_runtime_request_stop; the
 * host thread that owns the run call remains responsible for final shutdown.
 */
LLAM_API void llam_runtime_shutdown(void);

/**
 * @brief Collect a best-effort snapshot of runtime counters with explicit size.
 *
 * @details
 * This is the ABI-stable form preferred by dynamic loaders and FFI bindings.
 * LLAM writes only the overlapping prefix known to the loaded library; caller
 * tail bytes from newer struct definitions are left untouched. If @p stats and
 * @p stats_size are valid, failure paths clear the known caller-visible prefix
 * before returning so stale counters are not mistaken for a fresh snapshot.
 *
 * @param stats Destination statistics object. Must not be NULL.
 * @param stats_size Size of the caller's ::llam_runtime_stats_t definition.
 * @return 0 on success, -1 on failure with errno set.
 */
LLAM_API int llam_runtime_collect_stats_ex(llam_runtime_stats_t *stats, size_t stats_size);

/**
 * @brief Collect a best-effort snapshot from an explicit runtime handle.
 *
 * @details
 * This is the handle-scoped form for embedders that use
 * ::llam_runtime_create/::llam_runtime_run_handle instead of the legacy default
 * runtime wrappers.  Unknown runtime handles fail with @c EINVAL. If @p stats
 * and @p stats_size are valid, failure paths clear the caller-visible prefix.
 *
 * @param runtime Runtime returned by ::llam_runtime_create or ::llam_runtime_default; not NULL.
 * @param stats Destination statistics object. Must not be NULL.
 * @param stats_size Size of the caller's ::llam_runtime_stats_t definition.
 * @return 0 on success, -1 on failure with errno set.
 */
LLAM_API int llam_runtime_collect_stats_ex_handle(llam_runtime_t *runtime,
                                                  llam_runtime_stats_t *stats,
                                                  size_t stats_size);

/**
 * @brief Collect a best-effort snapshot of runtime counters.
 * @param stats Destination statistics object. Must not be NULL.
 * @details Convenience wrapper around ::llam_runtime_collect_stats_ex.
 * @return 0 on success, -1 on failure with errno set.
 */
LLAM_API int llam_runtime_collect_stats(llam_runtime_stats_t *stats);

/**
 * @brief Return the process-default runtime handle.
 *
 * @details
 * This handle names the legacy process-default runtime used by
 * ::llam_runtime_init, ::llam_spawn, ::llam_run, and
 * ::llam_runtime_shutdown. It is also accepted by explicit-handle APIs when an
 * embedder wants to drive the default runtime through the canonical handle
 * surface.
 *
 * @return The default runtime handle.
 */
LLAM_API llam_runtime_t *llam_runtime_default(void);

/**
 * @brief Create an explicit runtime handle.
 *
 * @details
 * Allocates and initializes an independent runtime instance. The returned
 * handle owns scheduler state, caches, blocking pool, and backend resources.
 * Public registries validate owner-stamped objects without exposing raw storage
 * addresses. Multiple explicit runtimes may run concurrently on different host
 * threads. Cross-runtime object use fails with @c EXDEV.
 *
 * @param opts Optional runtime options; pass NULL for defaults.
 * @param opts_size Size of the caller's ::llam_runtime_opts_t definition.
 * @param out Destination runtime handle. Must not be NULL.
 * @return 0 on success, -1 with @c errno set.
 */
LLAM_API int llam_runtime_create(const llam_runtime_opts_t *opts, size_t opts_size, llam_runtime_t **out);

/**
 * @brief Spawn a task on an explicit runtime handle.
 *
 * @details
 * This is the canonical 2.x embedding entry point for host threads managing
 * more than one runtime. The legacy ::llam_spawn and ::llam_spawn_ex wrappers
 * still target the current managed runtime when called from a task, otherwise
 * the default runtime. If @p opts supplies a cancellation token, that token
 * must belong to @p runtime; a foreign-runtime token is rejected with
 * @c EXDEV before the task is published.
 *
 * @param runtime Runtime returned by ::llam_runtime_create or ::llam_runtime_default; not NULL.
 * @param fn Task entry point. Must not be NULL.
 * @param arg User pointer passed to @p fn.
 * @param opts Optional spawn policy.
 * @param opts_size Size of @p opts when non-NULL.
 * @return Task handle on success, NULL on failure with @c errno set.
 */
LLAM_API llam_task_t *llam_runtime_spawn_ex(llam_runtime_t *runtime,
                                            llam_task_fn fn,
                                            void *arg,
                                            const llam_spawn_opts_t *opts,
                                            size_t opts_size);

/**
 * @brief Run a runtime handle.
 *
 * @details
 * Drives the scheduler associated with @p runtime until all work drains,
 * cooperative stop is requested, or a backend/runtime error is recorded.
 * Passing NULL fails with @c EINVAL.
 */
LLAM_API int llam_runtime_run_handle(llam_runtime_t *runtime);

/**
 * @brief Destroy a runtime handle.
 *
 * @details
 * Requests cooperative stop, tears down runtime-owned resources, and
 * invalidates the handle. Heap-backed handle storage returned by
 * ::llam_runtime_create is retired for the process lifetime rather than
 * immediately reused, so stale raw pointers cannot alias a later runtime.
 * Passing NULL is a legacy default-runtime shutdown alias. Managed tasks may
 * only stop their owner runtime through this API; foreign runtime handles and
 * unknown handles are ignored because this function has no errno channel.
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
 * libraries that appended fields to ::llam_spawn_opts_t. If @p opts supplies a
 * cancellation token, it must belong to the target runtime selected by this
 * wrapper. Calls from unmanaged host threads target the default runtime and are
 * rejected if that runtime is not initialized or is already in teardown.
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
 * @brief Low-overhead preemption poll for CPU-bound loops.
 *
 * @details
 * This macro intentionally expands to the public safepoint API so callers do
 * not depend on private scheduler state.  It is safe outside managed LLAM tasks
 * and becomes a no-op there.
 */
#define LLAM_PREEMPT_POLL() llam_task_safepoint()

/**
 * @brief Inline implementation for ::LLAM_PREEMPT_POLL_EVERY.
 *
 * Keeping the state in an inline function avoids public macro hygiene hazards:
 * caller variables cannot collide with implementation-local names, and both
 * arguments are evaluated exactly once before the helper is entered.
 */
static inline void llam_preempt_poll_every(size_t counter, size_t interval) {
    if (interval <= 1U || (counter % interval) == 0U) {
        llam_task_safepoint();
    }
}

/**
 * @brief Poll for cooperative preemption every @p interval loop iterations.
 *
 * @details
 * @p counter is evaluated once.  @p interval values 0 and 1 poll every call.
 * Use this in tight CPU loops that would otherwise not call any LLAM API.
 */
#define LLAM_PREEMPT_POLL_EVERY(counter, interval) \
    llam_preempt_poll_every((size_t)(counter), (size_t)(interval))

/**
 * @brief Wait indefinitely for task completion.
 * @param task Task returned by llam_spawn().
 * @details
 * A successful join consumes @p task; the pointer must not be used again.
 * Managed-task callers park cooperatively. Unmanaged OS-thread callers block
 * until the target is already complete or another thread is actively driving
 * ::llam_run; unmanaged joins do not run the scheduler. Concurrent joins on
 * the same handle are rejected with @c EBUSY so the handle is consumed exactly
 * once.
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
 * Concurrent joins on the same handle fail with @c EBUSY.
 * @return 0 on completion, -1 on timeout/failure with errno set.
 */
LLAM_API int llam_join_until(llam_task_t *task, uint64_t deadline_ns);

/**
 * @brief Detach a task handle so completion no longer requires join.
 * @param task Task returned by llam_spawn().
 * @details
 * A successful detach consumes @p task; the pointer must not be used again.
 * Detached tasks still run to completion and remain counted as live runtime
 * work until their entry function returns. Detach fails with @c EBUSY if a
 * join caller already owns the handle.
 * @return 0 on detach, -1 on failure with errno set.
 */
LLAM_API int llam_detach(llam_task_t *task);

/**
 * @brief Create a structured task group.
 *
 * @details
 * A group owns the task handles spawned through it. Use
 * ::llam_task_group_join to consume all child handles, or
 * ::llam_task_group_cancel to request cooperative cancellation first. Group
 * cancellation is delivered through the group's own token; children spawned
 * with an explicit caller-owned token remain controlled by that token.
 */
LLAM_API llam_task_group_t *llam_task_group_create(void);

/**
 * @brief Destroy an empty task group.
 *
 * @return 0 on success, or -1 with @c errno set to @c EBUSY if unjoined tasks
 * remain, another thread is currently spawning/cancelling through the group, or
 * a group join is in progress.
 */
LLAM_API int llam_task_group_destroy(llam_task_group_t *group);

/**
 * @brief Spawn a task owned by a group with explicit option size.
 *
 * @details
 * If @p opts does not provide a cancellation token, the group cancellation token
 * is attached automatically. The returned task pointer is borrowed for
 * diagnostics; regular ::llam_join and ::llam_detach reject it with @c EBUSY so
 * only the owning group can consume the child handle. The child is marked as
 * group-owned before it can execute, so a self-join/self-detach path cannot
 * consume the borrowed handle first. Spawning while another thread is joining
 * the group fails with @c EBUSY. Calls racing with a completed group destroy
 * fail with @c EINVAL instead of dereferencing a reclaimed handle. A saturated
 * public-operation lifecycle sentinel also fails closed with @c EBUSY. Children
 * are always spawned on the group's owner runtime, including when the caller is
 * an unmanaged host thread.
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
 * @brief Request cooperative cancellation for children using the group token.
 *
 * @details
 * The group cancels the token automatically attached to children that did not
 * provide @c opts.cancel_token. It does not cancel caller-owned explicit tokens,
 * because those tokens may be shared with tasks outside the group.
 */
LLAM_API int llam_task_group_cancel(llam_task_group_t *group);

/**
 * @brief Join all tasks owned by a group.
 *
 * @details Successful joins consume the child task handles. If an error occurs,
 * the failed and remaining handles stay owned by the group. The function fails
 * with @c EBUSY while another spawn or join is in progress for the same group.
 */
LLAM_API int llam_task_group_join(llam_task_group_t *group);

/**
 * @brief Join all tasks owned by a group until an absolute deadline.
 *
 * @details
 * @p deadline_ns is an absolute ::llam_now_ns deadline. @c 0 is treated as an
 * already-expired deadline, matching ::llam_join_until. Successful joins consume
 * completed child task handles. If the deadline expires or another join error
 * occurs, the failed and remaining handles stay owned by the group. The
 * function fails with @c EBUSY while another spawn or join is in progress for
 * the same group.
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
 * returns @c NULL still succeeds and stores @c NULL in @p out. If cancellation
 * reaches a queued callback before a worker starts it, the callback is skipped.
 * If cancellation reaches a callback that is already running, LLAM reports
 * @c ECANCELED only after the callback returns, so @p arg remains owned by the
 * callback for its full execution. @p out is cleared to @c NULL before
 * validation/submission so failure paths never expose a stale callback result.
 *
 * @param fn Blocking callback. Must not be NULL.
 * @param arg User pointer passed to fn.
 * @param out Destination for the callback result. Must not be NULL.
 * @return 0 on callback completion, -1 on submission/cancellation failure with
 *         @c errno set.
 */
LLAM_API int llam_call_blocking_result(llam_blocking_fn fn, void *arg, void **out);

/**
 * @brief Convenience wrapper around ::llam_call_blocking_result.
 * @details Cannot distinguish a successful @c NULL result from API failure;
 * production/FFI callers should use ::llam_call_blocking_result unless @c NULL
 * is outside the callback's valid result domain.
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
 *
 * @details
 * The dump is intended for rare hang and crash diagnostics. It includes
 * lifecycle/stop state, active I/O waiter counts, node submit/watch queues,
 * shard wake and I/O ownership state, and per-task wait ownership including
 * cancellation, select, blocking-job, and I/O request details.
 *
 * @param fd Destination file descriptor.
 */
LLAM_API void llam_dump_runtime_state(int fd);

/**
 * @brief Return task spawn/runtime flags.
 * @param task Task handle.
 * @return Bitwise OR of LLAM_SPAWN_F_* values, or 0 for invalid,
 * foreign-runtime, or NULL handles.
 */
LLAM_API uint32_t llam_task_flags(const llam_task_t *task);

/**
 * @brief Allocate a task-local storage key.
 *
 * @details On failure, @p out_key is set to ::LLAM_TASK_LOCAL_INVALID_KEY.
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
 * active observers behind the caller's back. Destroy also fails with @c EBUSY
 * while another thread is cancelling, querying, or retaining the token for a
 * new task. Calls that race with a completed destroy fail with @c EINVAL, and
 * managed cross-runtime destroy attempts fail with @c EXDEV.
 */
LLAM_API int llam_cancel_token_destroy(llam_cancel_token_t *token);

/**
 * @brief Request cancellation for all current and future observers of token.
 *
 * @details Calls racing with a completed destroy fail with @c EINVAL instead
 * of dereferencing reclaimed token storage. Cancellation also fails with
 * @c EBUSY while the public handle is already being torn down, and managed
 * cross-runtime cancellation fails with @c EXDEV. Unmanaged host calls that
 * need to wake waiters require a live owner runtime and fail with @c ENOTSUP
 * after the owner runtime can no longer service wakeups.
 */
LLAM_API int llam_cancel_token_cancel(llam_cancel_token_t *token);

/**
 * @brief Return non-zero when token has been cancelled.
 *
 * @details Calls racing with a completed destroy fail with @c EINVAL instead
 * of dereferencing reclaimed token storage. Queries also fail with @c EBUSY
 * while the public handle is already being torn down, and managed
 * cross-runtime queries fail with @c EXDEV.
 */
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
 * Destroy fails with @c EBUSY while a task owns or waits on the mutex, or while
 * another public mutex operation is still pinned inside the handle registry.
 * Managed cross-runtime destroy attempts fail with @c EXDEV.
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
 * condition, including the interval after signal/broadcast has selected the
 * waiter but before that task has returned from llam_cond_wait(), or while
 * another public condition operation is still pinned inside the handle registry.
 * Managed cross-runtime destroy attempts fail with @c EXDEV.
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
 * outside a managed LLAM task are allowed while the owner runtime is live.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL for invalid
 * arguments, @c EBUSY when the public handle is already being torn down,
 * @c EXDEV for cross-runtime managed use, or @c ENOTSUP when an unmanaged host
 * caller touches a condition whose owner runtime can no longer service waiter
 * wakeups.
 */
LLAM_API int llam_cond_signal(llam_cond_t *cond);

/**
 * @brief Wake all condition-variable waiters.
 *
 * @details May be called with or without the associated mutex held. Calls
 * outside a managed LLAM task are allowed while the owner runtime is live.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL for invalid
 * arguments, @c EBUSY when the public handle is already being torn down,
 * @c EXDEV for cross-runtime managed use, or @c ENOTSUP when an unmanaged host
 * caller touches a condition whose owner runtime can no longer service waiter
 * wakeups.
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
 * @details Destroy fails with @c EBUSY while buffered values, parked
 * senders/receivers, close-woken waiters, or another public channel operation
 * remain. Close the channel and drain buffered values before destroying when
 * producers may have sent data.
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
 * @brief Try to send a pointer value without parking.
 *
 * @details This operation never parks. Calls from managed tasks and unmanaged
 * OS threads are both allowed after runtime initialization. Full channels fail
 * with @c EAGAIN. Timed send APIs use @c ETIMEDOUT only when a real deadline
 * expires.
 */
LLAM_API int llam_channel_try_send(llam_channel_t *channel, void *value);

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
 * with @c ENOTSUP. @p out is cleared to @c NULL before validation/waiting so
 * failure paths never expose a stale payload pointer.
 *
 * @param channel Channel to receive from.
 * @param out Destination for the received pointer. Must not be NULL.
 * @return 0 on receive, -1 on close/cancellation/failure with @c errno set.
 */
LLAM_API int llam_channel_recv_result(llam_channel_t *channel, void **out);

/**
 * @brief Try to receive a pointer value without parking.
 *
 * @details This operation never parks. Calls from managed tasks and unmanaged
 * OS threads are both allowed after runtime initialization. Already-buffered
 * values may also be drained after runtime shutdown so host cleanup code can
 * destroy channels after stopping the scheduler. Empty open channels fail with
 * @c EAGAIN while the runtime is alive and @c ENOTSUP after shutdown. Timed
 * receive APIs use @c ETIMEDOUT only when a real deadline expires. @p out is
 * cleared to @c NULL before validation so failure paths never expose a stale
 * payload pointer.
 *
 * @param channel Channel to receive from.
 * @param out Destination for the received pointer. Must not be NULL.
 * @return 0 on receive, -1 on empty/close/cancellation/failure with @c errno set.
 */
LLAM_API int llam_channel_try_recv_result(llam_channel_t *channel, void **out);

/**
 * @brief Receive a pointer value until an absolute deadline.
 *
 * @details Must be called from a managed LLAM task; outside task context fails
 * with @c ENOTSUP. @p deadline_ns is an absolute ::llam_now_ns deadline; @c 0
 * is treated as an already-expired deadline. @p out is cleared to @c NULL
 * before validation/waiting so failure paths never expose a stale payload
 * pointer.
 *
 * @param channel Channel to receive from.
 * @param deadline_ns Absolute deadline in llam_now_ns() units.
 * @param out Destination for the received pointer. Must not be NULL.
 * @return 0 on receive, -1 on close/timeout/cancellation/failure with @c errno set.
 */
LLAM_API int llam_channel_recv_until_result(llam_channel_t *channel, uint64_t deadline_ns, void **out);

/**
 * @brief Convenience wrapper around ::llam_channel_recv_result.
 * @details Use the result API when @c NULL is a valid payload; outside task
 * context returns @c NULL with @c errno set to @c ENOTSUP.
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
 * Closing an idle channel is cleanup-safe after explicit runtime destruction;
 * closing a channel with parked waiters still requires a live owner runtime.
 */
LLAM_API int llam_channel_close(llam_channel_t *channel);

/** @brief Channel select operation kind. */
typedef enum llam_select_op_kind {
    LLAM_SELECT_OP_RECV = 1, /**< Try to receive from channel. */
    LLAM_SELECT_OP_SEND = 2, /**< Try to send to channel. */
} llam_select_op_kind_t;

/**
 * @brief Maximum operation count accepted by ::llam_channel_select.
 *
 * @details The bound keeps malformed FFI calls from forcing unbounded array
 * walks or allocation attempts before LLAM can validate each operation.
 */
#define LLAM_CHANNEL_SELECT_MAX_OPS 4096U

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
 * removed before this call returns. A selected receive on a closed, drained
 * channel sets the operation's @c result_errno to @c EPIPE and stores @c NULL
 * in @c recv_out; a selected send on a closed channel sets @c result_errno to
 * @c EPIPE without enqueueing @c send_value. @p deadline_ns is an absolute
 * ::llam_now_ns deadline; @c 0 performs a single non-blocking scan. @p op_count
 * must be in the range @c 1..LLAM_CHANNEL_SELECT_MAX_OPS.
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

#include "llam/io.h"

/* ============================================================================
 * Time and task introspection
 * ============================================================================
 */

/** @brief Return a monotonic timestamp in nanoseconds. */
LLAM_API uint64_t llam_now_ns(void);

/** @brief Return the runtime-assigned task id, or 0 for invalid or foreign handles. */
LLAM_API uint64_t llam_task_id(const llam_task_t *task);

/** @brief Return a stable task state string, or "UNKNOWN" for invalid or foreign handles. */
LLAM_API const char *llam_task_state_name(const llam_task_t *task);

/** @brief Return the scheduler class, or default class for invalid or foreign handles. */
LLAM_API uint32_t llam_task_class(const llam_task_t *task);

/** @brief Return the currently running task, or NULL outside a LLAM task. */
LLAM_API llam_task_t *llam_current_task(void);

#ifdef __cplusplus
}
#endif

#endif
