/**
 * @file src/internal/runtime_types.h
 * @brief Internal runtime type definitions for tasks, queues, waiters, I/O objects, and scheduler state.
 *
 * @details
 * This header defines the private data model shared by the scheduler, I/O
 * engine, synchronization primitives, allocator, watchdog, and diagnostics.
 * Most structures are intentionally visible to internal translation units so hot
 * paths can avoid indirection. Public users must include @c llam/runtime.h
 * instead; none of these layouts are ABI-stable.
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

#ifndef LLAM_RUNTIME_TYPES_H
#define LLAM_RUNTIME_TYPES_H

#include "llam_internal.h"
#include "runtime_platform.h"

#include <errno.h>
#include <limits.h>
#if LLAM_RUNTIME_BACKEND_WINDOWS
#include "runtime_windows_compat.h"
#include <mswsock.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#endif
#if !LLAM_RUNTIME_BACKEND_WINDOWS
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "runtime_public_slot.h"

#if LLAM_RUNTIME_BACKEND_WINDOWS
/**
 * @brief Runtime-local Windows socket nonblocking cache capacity.
 *
 * @details
 * The cache is intentionally scoped to a runtime object, not process-global,
 * because true multi-runtime execution must not let one runtime's direct-I/O
 * fast path trust socket state observed by another runtime.
 */
#define LLAM_WINDOWS_NONBLOCK_CACHE_CAP 4096U
#endif

#ifdef __linux__
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#include <liburing.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#else
/** @brief Non-Linux placeholder used so I/O node layout compiles without liburing. */
struct io_uring {
    int unused;
};

/** @brief Non-Linux placeholder for Linux provided-buffer ring state. */
struct io_uring_buf_ring {
    int unused;
};

/**
 * @brief Non-Linux no-op stand-in for liburing cleanup.
 *
 * @param ring Placeholder ring pointer.
 */
static inline void io_uring_queue_exit(struct io_uring *ring) {
    (void)ring;
}
#endif

#if defined(__linux__)
/** @brief CPU set representation used by affinity and NUMA discovery on Linux. */
typedef cpu_set_t llam_cpu_set_t;
#else
/** @brief Placeholder CPU set on platforms without Linux @c cpu_set_t. */
typedef struct llam_cpu_set {
    unsigned placeholder;
} llam_cpu_set_t;
#endif

#ifndef MAP_STACK
/** @brief Some Unix platforms do not define MAP_STACK; zero keeps mmap portable. */
#define MAP_STACK 0
#endif

#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
/** @brief Darwin/BSD compatibility alias for anonymous mmap. */
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

/** Trace events retained per shard for diagnostics. */
#define LLAM_TRACE_RING_CAP 512U
/** Alignment used to separate contended atomics from neighboring fields. */
#define LLAM_CACHELINE_BYTES 64U
/** Backend I/O submission/completion ring depth. */
#define LLAM_IO_RING_DEPTH 256U
/** Maximum remote-inject tasks drained in one scheduler pass. */
#define LLAM_INJECT_DRAIN_BUDGET 32U
/** Watchdog tick interval in nanoseconds. */
#define LLAM_WATCHDOG_INTERVAL_NS 1000000ULL
/** Kernel wait timeout used by idle I/O polling loops. */
#define LLAM_IDLE_POLL_TIMEOUT_MS 10
/** Per-thread alternate signal stack size used for fault diagnostics. */
#define LLAM_ALTSTACK_BYTES (64U * 1024U)
/** Bounded queue capacity for cross-shard injected work. */
#define LLAM_INJECT_QUEUE_CAP 1024U
/** Bounded queue capacity for latency-critical runnable work. */
#define LLAM_HOT_QUEUE_CAP 1024U
/** Bounded normal queue capacity; must remain a power of two. */
#define LLAM_NORM_QUEUE_CAP 4096U
/** Number of task objects allocated per task slab. */
#define LLAM_TASK_SLAB_COUNT 16U
/** Number of wait nodes allocated per wait-node slab. */
#define LLAM_WAIT_NODE_SLAB_COUNT 64U
/** Number of multi-channel select wait nodes embedded in each task. */
#define LLAM_TASK_EMBEDDED_SELECT_NODES 4U
/** Number of timer nodes allocated per timer-node slab. */
#define LLAM_TIMER_NODE_SLAB_COUNT 64U
/** Number of I/O requests allocated per request slab. */
#define LLAM_IO_REQ_SLAB_COUNT 64U
/** Normal number of I/O buffer wrappers allocated per slab. */
#define LLAM_IO_BUFFER_SLAB_COUNT 16U
/** Experimental huge-allocation I/O buffer wrapper slab width. */
#define LLAM_IO_BUFFER_HUGE_SLAB_COUNT 512U
/** Inline bytes embedded in each owned I/O buffer wrapper. */
#define LLAM_IO_BUFFER_INLINE_BYTES 4096U
/** Provided-buffer ring entries per Linux I/O node. */
#define LLAM_IO_RECV_BUF_RING_ENTRIES 128U
/** Blocking-job objects allocated per runtime-wide slab. */
#define LLAM_BLOCK_JOB_SLAB_COUNT 64U
/** Signal used internally to request task preemption. */
#define LLAM_PREEMPT_SIGNAL SIGUSR1
/** Consecutive watchdog observations before a deadlock suspicion is recorded. */
#define LLAM_DEADLOCK_SUSPECT_STREAK 4U
/** Consecutive pressure observations needed to scale dynamic workers up. */
#define LLAM_DYNAMIC_SCALE_UP_STREAK 2U
/** Consecutive idle observations needed to scale dynamic workers down. */
#define LLAM_DYNAMIC_SCALE_DOWN_STREAK 12U
/** Cooldown ticks after changing dynamic worker online count. */
#define LLAM_DYNAMIC_SCALE_COOLDOWN_TICKS 4U
/** Internal alias for a pinned task flag. */
#define LLAM_TASK_FLAG_PINNED LLAM_SPAWN_F_PINNED
/** Internal alias for no-preempt task flag. */
#define LLAM_TASK_FLAG_NO_PREEMPT LLAM_SPAWN_F_NO_PREEMPT
/** Internal alias for runtime/system task flag. */
#define LLAM_TASK_FLAG_SYS_TASK LLAM_SPAWN_F_SYS_TASK
/** Internal alias for latency-critical task flag. */
#define LLAM_TASK_FLAG_LATENCY_CRITICAL LLAM_SPAWN_F_LATENCY_CRITICAL

_Static_assert((LLAM_NORM_QUEUE_CAP & (LLAM_NORM_QUEUE_CAP - 1U)) == 0U, "LLAM_NORM_QUEUE_CAP must be a power of two");

/** @brief Private runtime object type. */
typedef struct llam_runtime llam_runtime_t;
/** @brief Scheduler worker/shard state type. */
typedef struct llam_shard llam_shard_t;
/** @brief Platform I/O node state type. */
typedef struct llam_node llam_node_t;
/** @brief Metadata entry describing a cached stack mapping. */
typedef struct llam_stack_cache_entry llam_stack_cache_entry_t;
/** @brief Per-task task-local storage entry. */
typedef struct llam_task_local_entry llam_task_local_entry_t;
/** @brief Active multi-channel select wait owned by a parked task. */
typedef struct llam_channel_select_state llam_channel_select_state_t;
/** @brief Synchronization wait node shared by wait/wake handshakes. */
typedef struct llam_wait_node llam_wait_node_t;

/** @brief Simple FIFO task queue used for hot, inject, overflow, and blocking queues. */
typedef struct llam_queue {
    llam_task_t *head;
    llam_task_t *tail;
    unsigned depth;
} llam_queue_t;

/** @brief Chase-Lev style bounded deque used by the optional lock-free normal queue. */
typedef struct llam_cldeque {
    _Alignas(LLAM_CACHELINE_BYTES) _Atomic size_t top;
    _Alignas(LLAM_CACHELINE_BYTES) _Atomic size_t bottom;
    _Alignas(LLAM_CACHELINE_BYTES) _Atomic(llam_task_t *) buffer[LLAM_NORM_QUEUE_CAP];
} llam_cldeque_t;

/** @brief Compact scheduler trace event stored in a per-shard ring buffer. */
typedef struct llam_trace_event {
    atomic_uint_fast64_t ts_ns;
    atomic_uint_fast64_t task_id;
    atomic_uint kind;
    atomic_uint from_state;
    atomic_uint to_state;
    atomic_uint reason;
    atomic_uint shard;
} llam_trace_event_t;

/** @brief Metadata for a cached stack mapping and its usable stack range. */
struct llam_stack_cache_entry {
    void *mapping;
    size_t mapping_size;
    void *stack_base;
    size_t stack_size;
    llam_stack_cache_entry_t *next;
    bool heap_allocated;
};

/** @brief Backing allocation tracked for allocator teardown. */
typedef struct llam_alloc_chunk {
    void *storage;
    size_t bytes;
    unsigned item_kind;
    unsigned item_count;
    bool mmapped;
    struct llam_alloc_chunk *next;
} llam_alloc_chunk_t;

/** @brief Allocation chunk kinds that require specialized cleanup. */
enum {
    LLAM_ALLOC_CHUNK_GENERIC = 0,
    LLAM_ALLOC_CHUNK_TASK = 1,
};

/** @brief Deferred I/O control operation sent to an I/O node. */
typedef struct llam_io_control_op llam_io_control_op_t;
/** @brief Shared poll readiness watch for an fd identity. */
typedef struct llam_poll_watch llam_poll_watch_t;
/** @brief Shared accept watch for a listener fd. */
typedef struct llam_accept_watch llam_accept_watch_t;
/** @brief Buffered accepted fd entry. */
typedef struct llam_accept_ready llam_accept_ready_t;
/** @brief Shared recv/read readiness watch for an fd identity. */
typedef struct llam_recv_watch llam_recv_watch_t;
/** @brief Buffered receive completion entry. */
typedef struct llam_recv_ready llam_recv_ready_t;

/** @brief Per-shard counters collected into public runtime stats and diagnostics. */
typedef struct llam_metrics {
    atomic_uint_fast64_t ctx_switches;
    atomic_uint_fast64_t yields;
    atomic_uint_fast64_t parks;
    atomic_uint_fast64_t wakes;
    atomic_uint_fast64_t timeout_wakes;
    atomic_uint_fast64_t cancel_wakes;
    atomic_uint_fast64_t sleeps;
    atomic_uint_fast64_t joins;
    atomic_uint_fast64_t steals;
    atomic_uint_fast64_t migrations;
    atomic_uint_fast64_t blocking_calls;
    atomic_uint_fast64_t blocking_completions;
    atomic_uint_fast64_t io_submits;
    atomic_uint_fast64_t io_completions;
    atomic_uint_fast64_t io_completion_latency_ns;
    atomic_uint_fast64_t io_completion_samples;
    atomic_uint_fast64_t io_fallbacks;
    atomic_uint_fast64_t hot_enqueues;
    atomic_uint_fast64_t norm_enqueues;
    atomic_uint_fast64_t inject_enqueues;
    atomic_uint_fast64_t wake_latency_ns;
    atomic_uint_fast64_t wake_samples;
    atomic_uint_fast64_t idle_polls;
    atomic_uint_fast64_t idle_spin_loops;
    atomic_uint_fast64_t idle_spin_hits;
    atomic_uint_fast64_t idle_spin_fallbacks;
    atomic_uint_fast64_t idle_spin_ns;
    atomic_uint_fast64_t watchdog_hits;
    atomic_uint_fast64_t long_no_safepoint;
    atomic_uint_fast64_t preempt_requests;
    atomic_uint_fast64_t preempt_yields;
    atomic_uint_fast64_t preempt_suppressed;
    atomic_uint_fast64_t preempt_signals;
    atomic_uint_fast64_t yield_direct_attempts;
    atomic_uint_fast64_t yield_direct_fast_hits;
    atomic_uint_fast64_t yield_direct_locked_hits;
    atomic_uint_fast64_t yield_direct_fail_context;
    atomic_uint_fast64_t yield_direct_fail_policy;
    atomic_uint_fast64_t yield_direct_fail_no_work;
    atomic_uint_fast64_t yield_direct_fail_self;
    atomic_uint_fast64_t yield_direct_fail_push;
    atomic_uint_fast64_t opaque_compensations;
    atomic_uint_fast64_t deadlock_suspicions;
    atomic_uint_fast64_t queue_overflows;
    atomic_uint_fast64_t slice_budget_ns;
    atomic_uint_fast64_t max_run_ns;
    atomic_uint_fast64_t slice_overruns;
    atomic_uint_fast64_t total_run_ns;
    atomic_uint_fast64_t opaque_block_ns;
    atomic_uint_fast64_t opaque_block_samples;
    atomic_uint_fast64_t opaque_block_max_ns;
    atomic_uint_fast64_t opaque_enter_wait_ns;
    atomic_uint_fast64_t opaque_enter_wait_samples;
    atomic_uint_fast64_t opaque_enter_wait_max_ns;
    atomic_uint_fast64_t opaque_leave_wait_ns;
    atomic_uint_fast64_t opaque_leave_wait_samples;
    atomic_uint_fast64_t opaque_leave_wait_max_ns;
    atomic_uint_fast64_t opaque_redirect_activations;
    atomic_uint_fast64_t wake_reason_hist[LLAM_WAIT_TIMEOUT + 1U];
} llam_metrics_t;

/** @brief Logical I/O operation kind. */
typedef enum llam_io_kind {
    LLAM_IO_KIND_READ = 0,
    LLAM_IO_KIND_WRITE = 1,
    LLAM_IO_KIND_ACCEPT = 2,
    LLAM_IO_KIND_POLL = 3,
    LLAM_IO_KIND_CONNECT = 4,
    LLAM_IO_KIND_HANDLE_READ = 5,
    LLAM_IO_KIND_HANDLE_WRITE = 6,
    LLAM_IO_KIND_PREAD = 7,
    LLAM_IO_KIND_PWRITE = 8,
    LLAM_IO_KIND_HANDLE_PREAD = 9,
    LLAM_IO_KIND_HANDLE_PWRITE = 10,
} llam_io_kind_t;

/** @brief Current ownership/wait state for an I/O request. */
typedef enum llam_io_wait_mode {
    LLAM_IO_WAIT_MODE_NONE = 0,
    LLAM_IO_WAIT_MODE_SUBMIT_QUEUE = 1,
    LLAM_IO_WAIT_MODE_INFLIGHT = 2,
    LLAM_IO_WAIT_MODE_POLL_WATCH = 3,
    LLAM_IO_WAIT_MODE_ACCEPT_WATCH = 4,
    LLAM_IO_WAIT_MODE_RECV_WATCH = 5,
} llam_io_wait_mode_t;

/** @brief Reason an I/O request was aborted before normal completion. */
typedef enum llam_io_abort_reason {
    LLAM_IO_ABORT_NONE = 0,
    LLAM_IO_ABORT_CANCEL = 1,
    LLAM_IO_ABORT_TIMEOUT = 2,
    LLAM_IO_ABORT_ERROR = 3,
} llam_io_abort_reason_t;

/** @brief State machine for runtime-wide blocking jobs. */
typedef enum llam_block_job_state {
    LLAM_BLOCK_JOB_QUEUED = 0,
    LLAM_BLOCK_JOB_RUNNING = 1,
    LLAM_BLOCK_JOB_FINISHED = 2,
    LLAM_BLOCK_JOB_ABORTED = 3,
} llam_block_job_state_t;

/**
 * @brief Blocking jobs bridge cooperative tasks and OS-level blocking work.  The task
 * parks while a helper thread runs fn(arg), then the scheduler wakes the task
 * once state reaches FINISHED or ABORTED.
 */
typedef struct llam_block_job {
    llam_blocking_fn fn;
    void *arg;
    _Atomic(void *) result;
    atomic_int error_code;
    llam_task_t *task;
    llam_wait_node_t *wait_node;
    atomic_uint state;
    bool holds_task_ref;
    struct llam_block_job *next;
} llam_block_job_t;

/**
 * @brief One logical I/O operation.  Requests are allocated per shard but may become
 * attached to a platform I/O node while inflight.  The wait_mode and
 * abort_reason atomics are the cross-thread handshake used by cancellation,
 * timeout, migration, and completion paths.
 */
typedef struct llam_io_req {
    llam_runtime_t *owner_runtime;
    llam_io_kind_t kind;
    llam_fd_t fd;
    llam_handle_t handle;
    void *buf;
    size_t count;
    uint64_t offset;
    struct sockaddr *addr;
    socklen_t *addrlen;
    socklen_t addr_len;
    llam_fd_t fd_result;
    ssize_t result;
    int error_code;
    short poll_events;
    short poll_revents;
    int timeout_ms;
    int recv_flags;
    llam_task_t *task;
    struct llam_io_req *next;
    struct llam_io_req *alloc_next;
    llam_poll_watch_t *poll_watch;
    llam_accept_watch_t *accept_watch;
    llam_recv_watch_t *recv_watch;
    llam_io_buffer_t *owned_buffer;
    unsigned owner_shard;
    unsigned alloc_owner_shard;
    unsigned attached_node_index;
    atomic_uint inflight_owner_shard;
    uint64_t submit_ts_ns;
    uint64_t deadline_ns;
    unsigned short provided_bid;
    void *platform_data;
    atomic_uint wait_mode;
    atomic_uint abort_reason;
    atomic_uint cancel_queued;
    bool use_recv_op;
    bool use_provided_buffer;
} llam_io_req_t;

/**
 * @brief Generic wait-list node used by mutexes, condition variables, channels, and
 * timed waits.  The value/scalar fields let higher-level primitives return a
 * small payload without allocating a separate completion object.
 */
typedef struct llam_wait_node {
    llam_runtime_t *owner_runtime;
    llam_task_t *task;
    struct llam_wait_node *next;
    struct llam_wait_node *alloc_next;
    void *value;
    llam_channel_select_state_t *select_state;
    int error_code;
    uint32_t select_kind;
    intptr_t scalar_value;
    unsigned owner_shard;
    atomic_uint wake_armed;
    atomic_uint wake_completed;
    atomic_uint wake_queued;
} llam_wait_node_t;

/** @brief FIFO waiter queue guarded by the owning primitive's lock. */
typedef struct llam_wait_queue {
    llam_wait_node_t *head;
    llam_wait_node_t *tail;
    unsigned depth;
} llam_wait_queue_t;

/** @brief Parked state for a task waiting on multiple channel operations. */
struct llam_channel_select_state {
    llam_runtime_t *owner_runtime;
    llam_select_op_t *ops;
    llam_wait_node_t **nodes;
    llam_channel_t **channels;
    llam_channel_t **op_channels;
    size_t op_count;
    size_t channel_count;
    size_t selected_index;
    void *selected_value;
    int error_code;
    atomic_uint completed;
    atomic_uint wake_armed;
    atomic_uint wake_queued;
};

/** @brief Control messages processed by an I/O node thread. */
typedef enum llam_io_control_kind {
    LLAM_IO_CONTROL_POLL_ACTIVATE = 1,     /**< Activate a poll watch in the backend. */
    LLAM_IO_CONTROL_POLL_DEACTIVATE = 2,   /**< Deactivate a poll watch in the backend. */
    LLAM_IO_CONTROL_ACCEPT_ACTIVATE = 3,   /**< Activate an accept watch in the backend. */
    LLAM_IO_CONTROL_ACCEPT_DEACTIVATE = 4, /**< Deactivate an accept watch in the backend. */
    LLAM_IO_CONTROL_RECV_ACTIVATE = 5,     /**< Activate a receive watch in the backend. */
    LLAM_IO_CONTROL_RECV_DEACTIVATE = 6,   /**< Deactivate a receive watch in the backend. */
    LLAM_IO_CONTROL_REQ_CANCEL = 7,        /**< Cancel a specific in-flight request. */
} llam_io_control_kind_t;

/** @brief Intrusive I/O control queue node. */
struct llam_io_control_op {
    llam_io_control_kind_t kind;
    void *target;
    llam_io_control_op_t *next;
};

/** @brief Completed accept results buffered by a watch until a task consumes them. */
struct llam_accept_ready {
    llam_fd_t fd;
    llam_accept_ready_t *next;
};

/**
 * @brief Completed recv/read readiness buffered by a watch.  Linux may use provided
 * buffers; portable paths can fall back to copy_data when ownership must cross
 * node boundaries safely.
 */
struct llam_recv_ready {
    size_t size;
    unsigned short bid;
    unsigned node_index;
    size_t copy_capacity;
    unsigned char *copy_data;
    bool has_buffer;
    llam_recv_ready_t *next;
};

/**
 * @brief Readiness watch shared by poll waiters for the same fd identity.  Device and
 * inode are tracked so fd reuse does not accidentally satisfy stale waiters.
 */
struct llam_poll_watch {
    llam_fd_t fd;
    dev_t st_dev;
    ino_t st_ino;
    short events;
    short sticky_revents;
    unsigned migrate_target_node_index;
    bool live_transferred;
    bool active;
    bool activating;
    bool deactivate_queued;
    llam_io_req_t *wait_head;
    llam_io_req_t *wait_tail;
    llam_poll_watch_t *next;
};

/** @brief Accept watch with a ready queue for accepted sockets and a waiter queue. */
struct llam_accept_watch {
    llam_fd_t fd;
    unsigned migrate_target_node_index;
    bool live_transferred;
    bool active;
    bool activating;
    bool deactivate_queued;
    unsigned ready_depth;
    llam_io_req_t *wait_head;
    llam_io_req_t *wait_tail;
    llam_accept_ready_t *ready_head;
    llam_accept_ready_t *ready_tail;
    llam_accept_watch_t *next;
};

/** @brief Receive watch with ready-buffer ownership and waiter tracking. */
struct llam_recv_watch {
    llam_fd_t fd;
    dev_t st_dev;
    ino_t st_ino;
    unsigned migrate_target_node_index;
    bool live_transferred;
    bool active;
    bool activating;
    bool deactivate_queued;
    unsigned ready_depth;
    llam_io_req_t *wait_head;
    llam_io_req_t *wait_tail;
    llam_recv_ready_t *ready_head;
    llam_recv_ready_t *ready_tail;
    llam_recv_watch_t *next;
};

/** @brief Timer heap node embedded in tasks when possible and allocated otherwise. */
typedef struct llam_timer_node {
    llam_runtime_t *owner_runtime;
    llam_task_t *task;
    uint64_t deadline_ns;
    struct llam_timer_node *next;
    struct llam_timer_node *alloc_next;
    size_t heap_index;
    unsigned owner_shard;
} llam_timer_node_t;

/**
 * @brief Per-shard allocator with remote-free queues.  Objects are usually returned
 * to the shard that allocated them; cross-shard frees are batched through
 * lock-free remote lists and drained at safe points.
 */
typedef struct llam_allocator {
    pthread_mutex_t lock;
    bool lock_initialized;
    llam_alloc_chunk_t *chunks;
    llam_task_t *task_free;
    llam_task_t *task_external_free;
    llam_wait_node_t *wait_free;
    llam_timer_node_t *timer_free;
    llam_io_req_t *io_req_free;
    llam_io_buffer_t *io_buffer_free;
    _Alignas(LLAM_CACHELINE_BYTES) atomic_uint remote_free_pending;
    _Alignas(LLAM_CACHELINE_BYTES) _Atomic(llam_task_t *) task_remote_free;
    _Alignas(LLAM_CACHELINE_BYTES) _Atomic(llam_wait_node_t *) wait_remote_free;
    _Alignas(LLAM_CACHELINE_BYTES) _Atomic(llam_timer_node_t *) timer_remote_free;
    _Alignas(LLAM_CACHELINE_BYTES) _Atomic(llam_io_req_t *) io_req_remote_free;
    _Alignas(LLAM_CACHELINE_BYTES) _Atomic(llam_io_buffer_t *) io_buffer_remote_free;
    atomic_uint_fast64_t local_epoch;
    uint64_t task_allocs;
    uint64_t task_reuses;
    uint64_t task_frees;
    uint64_t task_remote_frees;
    uint64_t task_remote_drains;
    uint64_t wait_allocs;
    uint64_t wait_reuses;
    uint64_t wait_frees;
    uint64_t wait_remote_frees;
    uint64_t wait_remote_drains;
    uint64_t timer_allocs;
    uint64_t timer_reuses;
    uint64_t timer_frees;
    uint64_t timer_remote_frees;
    uint64_t timer_remote_drains;
    uint64_t io_req_allocs;
    uint64_t io_req_reuses;
    uint64_t io_req_frees;
    uint64_t io_req_remote_frees;
    uint64_t io_req_remote_drains;
    uint64_t io_buffer_allocs;
    uint64_t io_buffer_reuses;
    uint64_t io_buffer_frees;
    uint64_t io_buffer_remote_frees;
    uint64_t io_buffer_remote_drains;
    uint64_t lock_acquires;
    uint64_t lock_contentions;
    uint64_t slab_grows;
    uint64_t slab_grow_failures;
    uint64_t task_remote_burst_max;
    uint64_t wait_remote_burst_max;
    uint64_t timer_remote_burst_max;
    uint64_t io_req_remote_burst_max;
    uint64_t io_buffer_remote_burst_max;
} llam_allocator_t;

/** @brief Runtime-owned I/O buffer.  Small reads use inline_data; large/provided buffers can attach external storage. */
struct llam_io_buffer {
    llam_runtime_t *owner_runtime;
    struct llam_io_buffer *alloc_next;
    struct llam_io_buffer *public_registry_next;
    size_t public_handle_slot;
    uint32_t public_handle_generation;
    _Atomic size_t public_active_ops;
    unsigned alloc_owner_shard;
    unsigned provided_node_index;
    size_t size;
    size_t capacity;
    unsigned char *data;
    size_t alignment;
    bool external_storage;
    bool aligned_storage;
    bool detached_wrapper;
    bool provided_storage;
    unsigned short provided_bid;
    _Alignas(16) unsigned char inline_data[LLAM_IO_BUFFER_INLINE_BYTES];
};

/** @brief Shared cancellation state.  Waiters are task links protected by lock. */
struct llam_cancel_token {
    llam_runtime_t *owner_runtime;
    struct llam_cancel_token *registry_next;
    size_t public_handle_slot;
    uint32_t public_handle_generation;
    pthread_mutex_t lock;
    bool cancelled;
    unsigned refcount;
    _Atomic size_t active_ops;
    llam_task_t *waiters;
};

/** @brief Runtime-aware mutex: owner is atomic for the fast path, waiters are lock protected. */
struct llam_mutex {
    llam_runtime_t *owner_runtime;
    struct llam_mutex *registry_next;
    size_t public_handle_slot;
    uint32_t public_handle_generation;
    _Atomic size_t active_ops;
    atomic_uintptr_t owner;
    pthread_mutex_t lock;
    llam_wait_queue_t waiters;
};

/** @brief Runtime-aware condition variable with a FIFO waiter queue. */
struct llam_cond {
    llam_runtime_t *owner_runtime;
    struct llam_cond *registry_next;
    size_t public_handle_slot;
    uint32_t public_handle_generation;
    _Atomic size_t active_ops;
    pthread_mutex_t lock;
    llam_wait_queue_t waiters;
    /*
     * A signal/broadcast removes waiters from waiters before the task returns
     * from llam_cond_wait(). Keep destroy from freeing the cond while those
     * wake results are still being consumed by resumed waiters.
     */
    atomic_uint inflight_waiters;
};

/** @brief Bounded pointer channel with separate sender and receiver wait queues. */
struct llam_channel {
    llam_runtime_t *owner_runtime;
    struct llam_channel *registry_next;
    size_t public_handle_slot;
    uint32_t public_handle_generation;
    _Atomic size_t active_ops;
    pthread_mutex_t lock;
    void **buffer;
    struct llam_channel *cache_next;
    size_t capacity;
    size_t ring_capacity;
    size_t mask;
    size_t head;
    size_t tail;
    size_t count;
    atomic_uint inflight_waiters;
    bool closed;
    llam_wait_queue_t send_waiters;
    llam_wait_queue_t recv_waiters;
};

/**
 * @brief Task object owned by the runtime allocator.  A task carries its fiber
 * context, stack allocation, scheduler links, wait state, optional active I/O
 * request, optional blocking job, and diagnostic timing counters.
 */
struct llam_task {
    llam_runtime_t *owner_runtime;
    struct llam_task *registry_next;
    uint64_t id;
    size_t public_handle_slot;
    atomic_uint public_handle_generation;
    _Atomic size_t active_ops;
    /*
     * The scheduler owns most task state transitions, but wake producers,
     * watchdog/debug, timeout, and dynamic-rehome paths can sample or rewrite
     * wait diagnostics concurrently. Keep the externally sampled state fields
     * atomic so those control paths stay race-free without taking every hot
     * scheduler-path lock.
     */
    atomic_uint state;
    atomic_uint wait_reason;
    unsigned flags;
    unsigned home_shard;
    unsigned live_shard;
    unsigned last_shard;
    atomic_uint parked_shard;
    atomic_uint task_class;
    atomic_uint base_task_class;
    uint64_t deadline_ns;
    llam_cancel_token_t *cancel_token;
    llam_task_fn entry;
    void *arg;
    _Alignas(16) llam_ctx_t ctx;
    void *stack_mapping;
    size_t mapping_size;
    void *stack_base;
    size_t stack_size;
    llam_stack_cache_entry_t stack_cache_entry;
    pthread_mutex_t lock;
    bool lock_initialized;
    atomic_uint task_listed;
    atomic_uint scan_refs;
    llam_task_t *all_next;
    llam_task_t *all_prev;
    llam_task_t *alloc_next;
    llam_task_t *queue_next;
    llam_task_t *queue_prev;
    llam_task_t *join_waiters;
    unsigned join_waiter_count;
    atomic_uint join_waiter_hint;
    llam_task_t *join_target;
    llam_task_t *wait_next;
    llam_task_t *cancel_next;
    llam_task_t *cancel_prev;
    llam_wait_node_t embedded_wait_node;
    llam_wait_node_t embedded_select_nodes[LLAM_TASK_EMBEDDED_SELECT_NODES];
    llam_wait_node_t *active_wait_node;
    llam_wait_queue_t *active_wait_queue;
    pthread_mutex_t *active_wait_queue_lock;
    llam_channel_select_state_t *active_select_state;
    llam_io_req_t embedded_io_req;
    _Atomic(llam_io_req_t *) active_io_req;
    _Atomic(llam_block_job_t *) active_block_job;
    llam_task_local_entry_t *task_locals;
    bool cancel_registered;
    unsigned enqueue_hot;
    unsigned alloc_owner_shard;
    bool alloc_external_pool;
    uint64_t last_runnable_ns;
    uint64_t last_yield_ns;
    uint64_t last_started_ns;
    uint64_t last_run_ns;
    uint64_t total_run_ns;
    uint64_t opaque_block_started_ns;
    uint64_t last_opaque_block_ns;
    uint64_t max_opaque_block_ns;
    uint64_t opaque_block_count;
    void *blocking_result;
    int saved_errno;
    int blocking_errno;
    int wake_error_code;
    atomic_uint opaque_blocking_depth;
    bool opaque_uses_helper;
    bool opaque_uses_redirect;
    unsigned safepoint_tick;
    unsigned preempt_poll_tick;
    size_t last_stack_used;
    size_t stack_high_water;
    llam_timer_node_t embedded_timer_node;
    llam_timer_node_t *active_timer;
    atomic_uint preempt_requested;
    atomic_uint completed;
    atomic_uint reclaim_ready;
    atomic_uint reclaim_claimed;
    atomic_uint join_claimed;
    atomic_uint detached;
    unsigned join_waiter_count_at_exit;
    unsigned forced_yield_budget;
};

/** @brief Intrusive task-local storage value linked from its owning task. */
struct llam_task_local_entry {
    llam_task_local_entry_t *next;
    llam_task_local_key_t key;
    void *value;
};

/** @brief Structured-concurrency group that owns a set of child task handles. */
struct llam_task_group {
    llam_runtime_t *owner_runtime;
    pthread_mutex_t lock;
    struct llam_task_group *registry_next;
    llam_cancel_token_t *cancel_token;
    llam_task_t **tasks;
    size_t count;
    size_t capacity;
    size_t active_spawns;
    _Atomic size_t active_ops;
    size_t public_handle_slot;
    uint32_t public_handle_generation;
    bool joining;
    bool lock_initialized;
};

/**
 * @brief Scheduler shard.  Each shard owns a worker thread, runnable queues, timer
 * heap, allocator, stack caches, metrics, and opaque-blocking compensation
 * state.  Most hot scheduler state is shard-local to avoid global locks.
 */
struct llam_shard {
    llam_runtime_t *runtime;
    unsigned id;
    unsigned cpu_id;
    unsigned node_index;
    unsigned io_node_index;
    atomic_uint online;
    atomic_uint inflight_io_waiters;
    atomic_uint merge_pause_requested;
    atomic_uint merge_pause_ack;
    atomic_uint steal_pause_ack;
    bool thread_started;
    pthread_t thread;
    int event_fd;
    _Alignas(LLAM_CACHELINE_BYTES) atomic_uint event_pending;
    pthread_mutex_t lock;
    pthread_mutex_t opaque_lock;
    pthread_cond_t opaque_cv;
    bool lock_initialized;
    bool opaque_lock_initialized;
    bool opaque_cv_initialized;
#if LLAM_RUNTIME_BACKEND_LINUX || LLAM_PLATFORM_WINDOWS || LLAM_RUNTIME_BACKEND_KQUEUE
    atomic_uint opaque_wake_seq;
#endif
#if defined(__APPLE__)
    semaphore_t opaque_sem;
    bool opaque_sem_initialized;
#endif
    pthread_t opaque_helper_thread;
    pthread_t primary_thread;
    void *signal_stack;
    size_t signal_stack_size;
    stack_t previous_sigaltstack;
    bool sigaltstack_installed;
    bool opaque_helper_thread_started;
    bool opaque_helper_ready;
    bool opaque_helper_active;
    bool stack_cache_lock_initialized;
    atomic_uint opaque_helper_active_hint;
#if defined(__linux__)
    atomic_uint opaque_helper_opaque_wait;
#endif
    bool opaque_helper_stop;
    bool opaque_helper_failed;
    bool opaque_redirect_active;
    unsigned opaque_redirect_target_id;
    unsigned opaque_compensation_depth;
    unsigned opaque_compensation_depth_peak;
    unsigned opaque_redirect_depth;
    unsigned opaque_redirect_depth_peak;
    llam_queue_t inject_q;
    atomic_uint inject_depth;
    llam_queue_t hot_q;
    llam_queue_t norm_q;
    llam_cldeque_t norm_cldeque;
    llam_task_t *all_tasks;
    llam_timer_node_t *timers;
    llam_timer_node_t **timer_heap;
    size_t timer_heap_len;
    size_t timer_heap_cap;
    atomic_uint timer_count;
    atomic_uint timer_callbacks_active;
    llam_stack_cache_entry_t *stack_cache_default;
    llam_stack_cache_entry_t *stack_cache_large;
    llam_stack_cache_entry_t *stack_cache_huge;
    llam_stack_cache_entry_t *stack_cache_entry_free;
    unsigned stack_cache_default_count;
    unsigned stack_cache_large_count;
    unsigned stack_cache_huge_count;
    pthread_mutex_t stack_cache_lock;
    _Alignas(16) llam_ctx_t scheduler_ctx;
    _Alignas(16) llam_ctx_t opaque_scheduler_ctx;
    _Atomic(llam_task_t *) current;
    _Alignas(LLAM_CACHELINE_BYTES) atomic_uint live_tasks;
    atomic_uint_fast64_t last_safepoint_ns;
    atomic_uint_fast64_t last_run_started_ns;
    uint64_t last_idle_wake_ns;
    uint64_t next_task_seq;
    atomic_uint norm_depth;
    unsigned hot_streak;
    unsigned direct_handoff_streak;
    llam_allocator_t allocator;
    llam_metrics_t metrics;
    llam_trace_event_t trace_ring[LLAM_TRACE_RING_CAP];
    atomic_uint trace_head;
};

/**
 * @brief I/O node.  Nodes own the platform event backend (io_uring on Linux, kqueue
 * state on Darwin) plus watch tables and submission/control queues.  Shards
 * submit requests to nodes, and nodes complete requests back to task owners.
 */
struct llam_node {
    llam_runtime_t *runtime;
    unsigned index;
    unsigned kernel_node_id;
    bool ring_ready;
    bool thread_started;
    bool supports_read;
    bool supports_recv;
    bool supports_write;
    bool supports_accept;
    bool supports_connect;
    bool supports_poll;
    bool supports_multishot_recv;
    bool supports_multishot_accept;
    bool supports_multishot_poll;
    bool supports_provided_buffers;
    bool cq_eventfd_registered;
    bool mach_wake_enabled;
    pthread_t thread;
    int event_fd;
    uint32_t mach_wake_port;
    uint32_t mach_wake_pset;
    void *windows_iocp_handle;
    void *windows_fd_assoc_head;
    void *windows_io_op_free;
    void *windows_accept_socket_free;
#if LLAM_RUNTIME_BACKEND_WINDOWS
    LPFN_ACCEPTEX windows_acceptex;
    LPFN_CONNECTEX windows_connectex;
#else
    void *windows_acceptex;
    void *windows_connectex;
#endif
    unsigned windows_completion_batch;
    unsigned windows_use_skip_completion_on_success;
    unsigned windows_io_op_free_count;
    unsigned windows_io_op_free_max;
    unsigned windows_accept_socket_free_count;
    unsigned windows_accept_socket_free_max;
    unsigned windows_accept_prepost;
    unsigned windows_poll_timeout_ms;
    _Alignas(LLAM_CACHELINE_BYTES) atomic_uint event_pending;
    pthread_mutex_t submit_lock;
    pthread_mutex_t windows_assoc_lock;
    pthread_mutex_t watch_lock;
    pthread_mutex_t recv_buf_lock;
    bool submit_lock_initialized;
    bool windows_assoc_lock_initialized;
    bool watch_lock_initialized;
    bool recv_buf_lock_initialized;
    llam_io_req_t *submit_head;
    llam_io_req_t *submit_tail;
    llam_io_control_op_t *control_head;
    llam_io_control_op_t *control_tail;
    llam_poll_watch_t *poll_watches;
    llam_accept_watch_t *accept_watches;
    llam_recv_watch_t *recv_watches;
    struct io_uring ring;
    struct io_uring_buf_ring *recv_buf_ring;
    unsigned char *recv_buf_storage;
    unsigned recv_buf_entries;
    unsigned recv_buf_mask;
    int recv_buf_group;
    _Alignas(LLAM_CACHELINE_BYTES) atomic_uint pending_ops;
    bool sqpoll_enabled;
    unsigned sqpoll_cpu;
    atomic_uint_fast64_t submit_batches;
    atomic_uint_fast64_t submit_entries;
    atomic_uint_fast64_t submit_calls;
    atomic_uint_fast64_t submit_syscalls;
    atomic_uint_fast64_t windows_cancel_controls;
    atomic_uint_fast64_t windows_cancel_success;
    atomic_uint_fast64_t windows_cancel_failures;
    atomic_uint_fast64_t windows_cancel_not_found;
    atomic_uint_fast64_t windows_immediate_completions;
    atomic_uint_fast64_t windows_skip_completion_handles;
    atomic_uint_fast64_t windows_skip_completion_failures;
    atomic_uint max_submit_batch;
    atomic_uint last_cq_depth;
    atomic_uint max_cq_depth;
    atomic_uint_fast64_t unsupported_ops;
    atomic_uint_fast64_t provided_buf_acquires;
    atomic_uint_fast64_t provided_buf_returns;
};

/**
 * @brief Global runtime state.  The runtime object is initialized once, then owns all
 * shards, I/O nodes, helper threads, global task lists, overflow queues, and
 * process-wide signal/affinity state until shutdown.
 */
struct llam_runtime {
    llam_runtime_t *registry_next;
    uint64_t runtime_id;
    uint64_t public_handle_secret;
    bool heap_allocated;
    atomic_bool destroy_claimed;
    _Atomic size_t active_ops;
    atomic_bool initialized;
    atomic_bool exec_started;
    unsigned observed_shards;
    unsigned active_shards;
    atomic_uint online_shards;
    atomic_uint online_shards_min;
    atomic_uint online_shards_max;
    unsigned dynamic_online_floor;
    unsigned active_nodes;
    unsigned deterministic;
    unsigned forced_yield_every;
    unsigned experimental_shard_rings;
    unsigned experimental_shard_rings_multishot;
    unsigned experimental_dynamic_shards;
    unsigned experimental_lockfree_normq;
    unsigned experimental_huge_alloc_requested;
    unsigned experimental_huge_alloc_active;
    unsigned experimental_sqpoll_requested;
    unsigned experimental_sqpoll_active;
    llam_runtime_profile_t profile;
    unsigned sqpoll_cpu_reserved;
    int sqpoll_cpu;
    bool xsave_enabled;
    bool winsock_started;
    unsigned windows_unsafe_skip_task_simd;
#if LLAM_RUNTIME_BACKEND_WINDOWS
    _Atomic(uintptr_t) windows_nonblock_cache[LLAM_WINDOWS_NONBLOCK_CACHE_CAP];
#endif
    uint64_t xsave_mask;
    size_t xsave_area_size;
    size_t xsave_area_alloc_size;
    uint64_t idle_spin_ns;
    unsigned idle_spin_max_iters;
    atomic_uint next_spawn_shard;
    unsigned block_worker_count;
    unsigned block_threads_started;
    pthread_t init_thread;
    llam_cpu_set_t init_thread_affinity;
    bool init_thread_affinity_valid;
    unsigned *allowed_cpus;
    unsigned *kernel_node_ids;
    llam_shard_t *shards;
    llam_node_t *nodes;
    pthread_t *block_threads;
    pthread_t ctrl_thread;
    bool ctrl_thread_started;
    bool task_list_lock_initialized;
    bool stack_cache_lock_initialized;
    bool block_lock_initialized;
    bool overflow_lock_initialized;
    pthread_mutex_t task_list_lock;
    pthread_mutex_t stack_cache_lock;
    llam_task_t *all_tasks;
    llam_stack_cache_entry_t *stack_cache_default;
    llam_stack_cache_entry_t *stack_cache_large;
    llam_stack_cache_entry_t *stack_cache_huge;
    llam_stack_cache_entry_t *stack_cache_entry_free;
    unsigned stack_cache_default_count;
    unsigned stack_cache_large_count;
    unsigned stack_cache_huge_count;
    pthread_mutex_t block_lock;
    pthread_cond_t block_cv;
    pthread_mutex_t overflow_lock;
    llam_queue_t overflow_q;
    llam_block_job_t *block_head;
    llam_block_job_t *block_tail;
    llam_alloc_chunk_t *block_job_chunks;
    _Alignas(LLAM_CACHELINE_BYTES) _Atomic(llam_block_job_t *) block_job_free;
    _Alignas(LLAM_CACHELINE_BYTES) atomic_uint block_wake_seq;
    struct sigaction previous_preempt_action;
    struct sigaction previous_segv_action;
    bool preempt_signal_installed;
    bool segv_signal_installed;
    bool block_cv_initialized;
    _Alignas(LLAM_CACHELINE_BYTES) atomic_uint block_pending;
    _Alignas(LLAM_CACHELINE_BYTES) atomic_uint block_active;
    _Alignas(LLAM_CACHELINE_BYTES) atomic_uint block_active_peak;
    _Alignas(LLAM_CACHELINE_BYTES) atomic_uint live_tasks;
    _Alignas(LLAM_CACHELINE_BYTES) atomic_uint live_task_shards;
    _Alignas(LLAM_CACHELINE_BYTES) atomic_uint active_io_waiters;
    _Alignas(LLAM_CACHELINE_BYTES) atomic_uint next_task_id;
    _Alignas(LLAM_CACHELINE_BYTES) atomic_uint overflow_depth;
    atomic_uint_fast64_t global_epoch;
    atomic_bool stop_requested;
    atomic_bool shutdown_requested;
    atomic_int fatal_errno;
    uint64_t deadlock_progress_snapshot;
    unsigned deadlock_probe_streak;
    unsigned dynamic_scale_up_streak;
    unsigned dynamic_scale_down_streak;
    unsigned dynamic_scale_cooldown;
    unsigned trace_events_enabled;
    unsigned wake_latency_metrics_enabled;
    unsigned run_timing_enabled;
    unsigned stack_sampling_enabled;
    unsigned task_list_eager;
    unsigned direct_handoff_stats_enabled;
    unsigned direct_handoff_burst;
    unsigned direct_handoff_live_limit;
    unsigned direct_handoff_allow_timers;
    unsigned cheap_safepoint;
    unsigned safepoint_clock_period;
    unsigned preempt_mode;
    unsigned preempt_poll_period;
    uint64_t preempt_quantum_ns;
    unsigned spawn_fanout_wake_interval;
    unsigned spawn_fanout_wake_interval_forced;
    unsigned spawn_fanout_adaptive;
    unsigned channel_local_handoff_enabled;
    unsigned channel_safepoint_interval;
    atomic_uint steal_pause_active;
};



#endif
