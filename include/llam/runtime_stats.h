/**
 * @file include/llam/runtime_stats.h
 * @brief ABI-stable runtime statistics snapshot layout.
 *
 * This header is included by <llam/runtime.h>; applications should include
 * that primary header rather than including this file directly.
 */

#ifndef LLAM_RUNTIME_STATS_H
#define LLAM_RUNTIME_STATS_H

#include <stddef.h>
#include <stdint.h>

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
    uint64_t wake_handoff_attempts;     /**< Same-shard wake-to-task handoff attempts. */
    uint64_t wake_handoff_hits;         /**< Same-shard wake-to-task handoffs that switched directly. */
    uint64_t wake_handoff_fail_context; /**< Wake handoffs rejected by caller/task context. */
    uint64_t wake_handoff_fail_policy;  /**< Wake handoffs rejected by runtime policy guards. */
    uint64_t wake_handoff_fail_race;    /**< Wake handoffs that lost a queue/state race and fell back. */
    uint32_t configured_worker_min;      /**< Resolved minimum online scheduler workers. */
    uint32_t configured_worker_count;    /**< Resolved initial online scheduler workers. */
    uint32_t configured_worker_max;      /**< Resolved maximum scheduler-worker capacity. */
    uint32_t configured_blocking_min;    /**< Resolved initial blocking-worker count. */
    uint32_t configured_blocking_max;    /**< Resolved maximum blocking-worker count. */
    uint32_t selected_cpu_count;         /**< Number of process-allowed CPUs selected for this runtime. */
    uint32_t affinity_policy;            /**< Active ::llam_runtime_affinity_policy_t policy. */
    uint32_t resource_reserved0;         /**< Reserved ABI padding; currently 0. */
    uint32_t scheduler_threads;          /**< Live runtime-owned scheduler threads, excluding the host loop. */
    uint32_t blocking_threads;           /**< Live runtime-owned blocking worker threads. */
    uint32_t io_threads;                 /**< Live runtime-owned I/O threads. */
    uint32_t controller_threads;         /**< Live runtime-owned controller threads. */
    uint32_t opaque_helper_threads;      /**< Live helpers covering opaque blocking regions. */
    uint32_t runtime_owned_threads;      /**< Total live native threads owned by the runtime. */
    uint32_t native_execution_threads;   /**< Live native execution threads including an active host loop. */
    uint32_t resource_reserved1;         /**< Reserved ABI padding; currently 0. */
    uint64_t affinity_failures;          /**< Preferred or required affinity apply/restore failures. */
    uint64_t requested_task_prewarm_total;  /**< Resolved runtime-total task-object prewarm target. */
    uint64_t achieved_task_prewarm_total;   /**< Task objects successfully prewarmed. */
    uint64_t requested_stack_prewarm_total; /**< Resolved runtime-total stack prewarm target. */
    uint64_t achieved_stack_prewarm_total;  /**< Stacks successfully prewarmed. */
    uint64_t requested_timer_prewarm_total; /**< Resolved runtime-total timer-slot prewarm target. */
    uint64_t achieved_timer_prewarm_total;  /**< Timer slots successfully prewarmed. */
    uint64_t estimated_metadata_bytes;      /**< Checked metadata-byte estimate for the resource plan. */
    uint64_t estimated_stack_mapping_bytes; /**< Checked virtual mapping estimate for prewarmed stacks. */
    uint32_t task_prewarm_source;            /**< Authority for task prewarm. */
    uint32_t stack_prewarm_source;           /**< Authority for stack prewarm. */
    uint32_t timer_prewarm_source;           /**< Authority for timer prewarm. */
    uint32_t prewarm_reserved0;              /**< Reserved ABI padding; currently 0. */
    uint64_t stack_cache_budget_bytes;         /**< Resolved retained mapping-byte budget. */
    uint64_t stack_cache_high_watermark_bytes; /**< Resolved automatic-trim trigger. */
    uint64_t stack_cache_low_watermark_bytes;  /**< Resolved automatic-trim target. */
    uint64_t stack_cache_idle_ns;              /**< Resolved idle trim age. */
    uint32_t stack_cache_flags;                /**< Active stack-cache policy flags. */
    uint32_t stack_cache_resident_valid;       /**< Whether resident sampling is current. */
    uint64_t stack_cache_cached_bytes;         /**< Exact retained mapping bytes. */
    uint64_t stack_cache_cached_mappings;      /**< Exact retained mapping count. */
    uint64_t stack_cache_committed_bytes;      /**< Exact known committed usable bytes. */
    uint64_t stack_cache_trim_requests;        /**< Manual and automatic trim requests. */
    uint64_t stack_cache_discarded_bytes;      /**< Usable bytes discarded/decommitted. */
    uint64_t stack_cache_released_bytes;       /**< Mapping bytes returned to the platform. */
    uint64_t stack_cache_budget_rejections;    /**< Mappings rejected by byte authority. */
    uint64_t stack_cache_secure_return_failures; /**< Required VM transition failures. */
    uint64_t stack_cache_resident_bytes;       /**< Last optional resident-byte sample. */
    uint64_t stack_cache_resident_sample_ns;   /**< Resident sample timestamp. */
    uint64_t stack_cache_process_quarantine_bytes; /**< Process-owned bytes awaiting release retry. */
    uint64_t stack_cache_process_quarantine_mappings; /**< Process-owned mappings awaiting release retry. */
} llam_runtime_stats_t;

/** @brief Current size to pass to ::llam_runtime_collect_stats_ex. */
#define LLAM_RUNTIME_STATS_CURRENT_SIZE ((size_t)sizeof(llam_runtime_stats_t))

#endif
