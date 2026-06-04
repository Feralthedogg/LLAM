/**
 * @file src/core/debug/trace.c
 * @brief Per-worker trace-ring recording for debug and fault analysis.
 *
 * @details
 * Each shard owns a fixed-size trace ring. Runtime hot paths append compact
 * transition events when tracing is enabled, and fault/diagnostic code can read
 * recent entries without allocating memory.
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

#include "runtime_internal.h"

/**
 * @brief Append a task transition event to a shard trace ring.
 *
 * @param shard  Shard whose ring receives the event.
 * @param task   Task associated with the event, or NULL for shard-level events.
 * @param kind   Event kind.
 * @param from   Previous task state.
 * @param to     New task state.
 * @param reason Wait/block reason associated with the transition.
 */
void llam_trace_shard(llam_shard_t *shard,
                           llam_task_t *task,
                           llam_trace_kind_t kind,
                           llam_task_state_id_t from,
                           llam_task_state_id_t to,
                           llam_wait_reason_t reason) {
    llam_trace_event_t *event;
    unsigned seq;
    unsigned slot;

    if (LLAM_LIKELY(shard == NULL || shard->runtime == NULL || shard->runtime->trace_events_enabled == 0U)) {
        return;
    }

    // Ring overwrite is intentional: trace data is best-effort and must never
    // block scheduler progress or allocate on the hot path.  Trace writes can
    // come from peer wake threads as well as the shard owner, so each field is
    // atomic and the head is reserved with fetch-add instead of a plain increment.
    seq = atomic_fetch_add_explicit(&shard->trace_head, 1U, memory_order_relaxed);
    slot = seq % LLAM_TRACE_RING_CAP;
    event = &shard->trace_ring[slot];
    atomic_store_explicit(&event->ts_ns, llam_now_ns(), memory_order_relaxed);
    atomic_store_explicit(&event->task_id, task != NULL ? task->id : 0U, memory_order_relaxed);
    atomic_store_explicit(&event->from_state, (unsigned)from, memory_order_relaxed);
    atomic_store_explicit(&event->to_state, (unsigned)to, memory_order_relaxed);
    atomic_store_explicit(&event->reason, (unsigned)reason, memory_order_relaxed);
    atomic_store_explicit(&event->shard, shard->id, memory_order_relaxed);
    atomic_store_explicit(&event->kind, (unsigned)kind, memory_order_release);
}
