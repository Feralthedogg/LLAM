/**
 * @file src/core/lifecycle/shutdown.c
 * @brief Runtime shutdown, worker teardown, and shared process hook cleanup.
 *
 * @details
 * Shutdown is intentionally conservative: request runtime stop, join worker
 * threads, drain backend queues/watch state, destroy platform resources, release
 * cached stacks and allocator slabs, restore process-wide signal/affinity state,
 * and finally zero the runtime object.  Legacy default-runtime wrappers and
 * explicit handles both delegate to the runtime-owned shutdown helper here.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

#include "runtime_internal.h"
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#include "windows_async_lifetime.h"
#endif

#include <assert.h>

static void llam_runtime_wait_opaque_helper_exit_ack(llam_shard_t *shard);

/**
 * @brief Close an accepted socket descriptor retained by a backend watch.
 *
 * Accept-watch ready queues store public @c llam_fd_t values, not scheduler
 * wake handles.  On POSIX that is a file descriptor and @c close is correct;
 * on Windows it is a Winsock @c SOCKET and must be released with
 * @c closesocket.  Using the CRT close shim on Windows leaves the socket open
 * and can leak accepted sockets during partial shutdown or watch teardown.
 */
static void llam_runtime_close_ready_fd(llam_fd_t fd) {
    if (LLAM_FD_IS_INVALID(fd)) {
        return;
    }
#if LLAM_PLATFORM_WINDOWS
    (void)closesocket(fd);
#else
    (void)close(fd);
#endif
}

/**
 * @brief Stop the runtime and release every runtime-owned resource.
 *
 * The function tolerates partial initialization so failed init paths can reuse
 * normal shutdown cleanup.
 */
static llam_managed_catalog_t *llam_runtime_shutdown_unlocked(llam_runtime_t *rt) {
    unsigned i;
    llam_managed_catalog_t *retired_catalog = NULL;

    if (!atomic_load_explicit(&rt->initialized, memory_order_acquire) &&
        !atomic_load_explicit(&rt->exec_started, memory_order_acquire) &&
        rt->allowed_cpus == NULL && rt->shards == NULL && rt->nodes == NULL
#if LLAM_RUNTIME_BACKEND_WINDOWS
        && !rt->winsock_started
#endif
    ) {
        llam_runtime_unregister_handle(rt);
        return NULL;
    }

    /*
     * Public task handles point into allocator-owned task slabs.  Disable
     * public task-handle unwrapping before those slabs are walked and released,
     * so stale post-shutdown inspection fails without reading reclaimed task
     * storage.
     */
    atomic_store_explicit(&rt->initialized, false, memory_order_release);

    // The calling thread is no longer considered a runtime worker while
    // teardown is in progress.
    g_llam_tls_shard = NULL;
    g_llam_tls_worker = NULL;
    g_llam_tls_processor = NULL;
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = NULL;
    atomic_store_explicit(&rt->shutdown_requested, true, memory_order_release);
    atomic_store_explicit(&rt->steal_pause_active, 0U, memory_order_release);
    llam_request_stop(rt);

    /*
     * Stop lease grants before waiting for controller or worker teardown.  The
     * coordinator is lock-protected even during partial shutdown; shards whose
     * opaque lock was never initialized have no worker that could race this
     * cleanup and are reclaimed by the final runtime teardown below.
     */
    if (rt->shards != NULL) {
        for (i = 0; i < rt->active_shards; ++i) {
            if (rt->shards[i].opaque_lock_initialized) {
                pthread_mutex_lock(&rt->shards[i].opaque_lock);
                llam_legacy_lease_begin_stop_locked(&rt->shards[i]);
                pthread_mutex_unlock(&rt->shards[i].opaque_lock);
            }
        }
    }

    if (rt->ctrl_thread_started) {
        pthread_join(rt->ctrl_thread, NULL);
        rt->ctrl_thread_started = false;
    }

    // Shard zero is driven by llam_run() on the caller thread, so only auxiliary
    // worker threads are joined here.
    if (atomic_load_explicit(&rt->exec_started, memory_order_acquire)) {
        for (i = 1; i < rt->active_shards; ++i) {
            if (rt->shards[i].thread_started) {
                pthread_join(rt->shards[i].thread, NULL);
                rt->shards[i].thread_started = false;
            }
        }
        atomic_store_explicit(&rt->exec_started, false, memory_order_release);
    }

    if (rt->block_threads != NULL) {
        // Only join block workers whose pthread_create call completed
        // successfully; failed creates may leave an arbitrary pthread_t value.
        for (i = 0; i < rt->block_threads_started; ++i) {
            pthread_join(rt->block_threads[i], NULL);
            rt->block_threads[i] = 0;
        }
        rt->block_threads_started = 0U;
    }

    // Drain any channel cache entries retained by the shutdown caller and the
    // process-wide fallback cache. Scheduler workers drain their own TLS caches
    // as their loops exit before the joins above complete.
    llam_channel_tls_cache_drain();
    llam_channel_global_cache_drain();

    if (rt->shards != NULL) {
        for (i = 0; i < rt->active_shards; ++i) {
            if (rt->shards[i].opaque_helper_thread_started) {
                // Opaque helpers may be asleep on their private wake path; set
                // the stop flag under lock and signal both helper and shard.
                pthread_mutex_lock(&rt->shards[i].opaque_lock);
                rt->shards[i].opaque_helper_stop = true;
                llam_opaque_wake_signal(&rt->shards[i]);
                pthread_mutex_unlock(&rt->shards[i].opaque_lock);
                llam_kick_shard(&rt->shards[i]);
                llam_runtime_wait_opaque_helper_exit_ack(&rt->shards[i]);
                pthread_join(rt->shards[i].opaque_helper_thread, NULL);
                rt->shards[i].opaque_helper_thread_started = false;
            }
            assert(!rt->shards[i].primary_worker.sigaltstack_installed);
            assert(!rt->shards[i].opaque_helper_worker.sigaltstack_installed);
            assert(rt->shards[i].primary_worker.signal_stack_incarnation == 0U);
            assert(rt->shards[i].opaque_helper_worker.signal_stack_incarnation == 0U);
        }
    }

    if (rt->nodes != NULL) {
        for (i = 0; i < rt->active_nodes; ++i) {
            if (rt->nodes[i].thread_started) {
                pthread_join(rt->nodes[i].thread, NULL);
                rt->nodes[i].thread_started = false;
            }
        }
        /*
         * Public owned buffers can outlive the runtime that produced them.
         * Detach any wrappers that still point into backend provided-buffer
         * rings before the node teardown below releases those rings.
         */
        llam_io_buffer_public_detach_runtime_storage(rt);
        for (i = 0; i < rt->active_nodes; ++i) {
#if LLAM_RUNTIME_BACKEND_LINUX
            /*
             * Every encoded control and multishot watch pointer remains
             * kernel-reachable until the ring is dismantled, even when its
             * logical pending slot was already balanced by the opposite side
             * of a cancel race.  Tear the ring down before freeing watch-table
             * storage, then retire controls before their target watch refs.
             */
            if (rt->nodes[i].ring_ready) {
                llam_node_unregister_cq_eventfd(&rt->nodes[i]);
                llam_node_destroy_recv_buf_ring(&rt->nodes[i]);
                io_uring_queue_exit(&rt->nodes[i].ring);
                rt->nodes[i].ring_ready = false;
            }
            if (rt->nodes[i].watch_lock_initialized) {
                llam_linux_retire_backend_controls(&rt->nodes[i]);
                llam_linux_retire_backend_watch_refs(&rt->nodes[i]);
            }
#endif
            if (rt->nodes[i].watch_lock_initialized) {
                pthread_mutex_lock(&rt->nodes[i].watch_lock);
                // Control operations are heap-allocated command nodes owned by the
                // I/O node after enqueue.
                while (rt->nodes[i].control_head != NULL) {
                    llam_io_control_op_t *op = rt->nodes[i].control_head;
                    llam_io_control_op_t *next = op->next;

                    op->next = NULL;
                    llam_io_control_op_destroy(&rt->nodes[i], op);
                    rt->nodes[i].control_head = next;
                }
                /* Workers and controls are drained; no index reader remains. */
                llam_watch_index_reset(&rt->nodes[i].poll_watch_index);
                llam_watch_index_reset(&rt->nodes[i].accept_watch_index);
                llam_watch_index_reset(&rt->nodes[i].recv_watch_index);
                // Watch tables own their watch objects and any buffered readiness
                // data that has not yet been consumed by a task.
                while (rt->nodes[i].poll_watches != NULL) {
                    llam_poll_watch_t *next = rt->nodes[i].poll_watches->next;

                    free(rt->nodes[i].poll_watches);
                    rt->nodes[i].poll_watches = next;
                }
                while (rt->nodes[i].accept_watches != NULL) {
                    llam_accept_watch_t *next = rt->nodes[i].accept_watches->next;

                    while (rt->nodes[i].accept_watches->ready_head != NULL) {
                        llam_accept_ready_t *ready_next = rt->nodes[i].accept_watches->ready_head->next;

                        llam_runtime_close_ready_fd(rt->nodes[i].accept_watches->ready_head->fd);
                        free(rt->nodes[i].accept_watches->ready_head);
                        rt->nodes[i].accept_watches->ready_head = ready_next;
                    }
                    free(rt->nodes[i].accept_watches);
                    rt->nodes[i].accept_watches = next;
                }
                while (rt->nodes[i].recv_watches != NULL) {
                    llam_recv_watch_t *next = rt->nodes[i].recv_watches->next;

                    while (rt->nodes[i].recv_watches->ready_head != NULL) {
                        llam_recv_ready_t *ready_next = rt->nodes[i].recv_watches->ready_head->next;
                        llam_node_t *owner = &rt->nodes[i];

                        if (rt->nodes[i].recv_watches->ready_head->has_buffer &&
                            rt->nodes[i].recv_watches->ready_head->node_index < rt->active_nodes) {
                            owner = &rt->nodes[rt->nodes[i].recv_watches->ready_head->node_index];
                        }
                        if (owner->ring_ready && owner->supports_provided_buffers &&
                            rt->nodes[i].recv_watches->ready_head->has_buffer) {
                            // Provided buffers belong to the node that produced
                            // them, which may differ after live watch migration.
                            (void)llam_node_recycle_recv_buffer(owner, rt->nodes[i].recv_watches->ready_head->bid);
                        }
                        // Shutdown owns queued copied payloads just like the
                        // normal recv-watch destroy path.
                        free(rt->nodes[i].recv_watches->ready_head->copy_data);
                        free(rt->nodes[i].recv_watches->ready_head);
                        rt->nodes[i].recv_watches->ready_head = ready_next;
                    }
                    free(rt->nodes[i].recv_watches);
                    rt->nodes[i].recv_watches = next;
                }
                while (rt->nodes[i].retired_poll_watches != NULL) {
                    llam_poll_watch_t *next = rt->nodes[i].retired_poll_watches->next;

                    free(rt->nodes[i].retired_poll_watches);
                    rt->nodes[i].retired_poll_watches = next;
                }
                while (rt->nodes[i].retired_accept_watches != NULL) {
                    llam_accept_watch_t *next = rt->nodes[i].retired_accept_watches->next;

                    while (rt->nodes[i].retired_accept_watches->ready_head != NULL) {
                        llam_accept_ready_t *ready_next = rt->nodes[i].retired_accept_watches->ready_head->next;

                        llam_runtime_close_ready_fd(rt->nodes[i].retired_accept_watches->ready_head->fd);
                        free(rt->nodes[i].retired_accept_watches->ready_head);
                        rt->nodes[i].retired_accept_watches->ready_head = ready_next;
                    }
                    free(rt->nodes[i].retired_accept_watches);
                    rt->nodes[i].retired_accept_watches = next;
                }
                while (rt->nodes[i].retired_recv_watches != NULL) {
                    llam_recv_watch_t *next = rt->nodes[i].retired_recv_watches->next;

                    while (rt->nodes[i].retired_recv_watches->ready_head != NULL) {
                        llam_recv_ready_t *ready_next = rt->nodes[i].retired_recv_watches->ready_head->next;
                        llam_node_t *owner = &rt->nodes[i];

                        if (rt->nodes[i].retired_recv_watches->ready_head->has_buffer &&
                            rt->nodes[i].retired_recv_watches->ready_head->node_index < rt->active_nodes) {
                            owner = &rt->nodes[rt->nodes[i].retired_recv_watches->ready_head->node_index];
                        }
                        if (owner->ring_ready && owner->supports_provided_buffers &&
                            rt->nodes[i].retired_recv_watches->ready_head->has_buffer) {
                            (void)llam_node_recycle_recv_buffer(owner, rt->nodes[i].retired_recv_watches->ready_head->bid);
                        }
                        free(rt->nodes[i].retired_recv_watches->ready_head->copy_data);
                        free(rt->nodes[i].retired_recv_watches->ready_head);
                        rt->nodes[i].retired_recv_watches->ready_head = ready_next;
                    }
                    free(rt->nodes[i].retired_recv_watches);
                    rt->nodes[i].retired_recv_watches = next;
                }
                pthread_mutex_unlock(&rt->nodes[i].watch_lock);
            }
            if (rt->nodes[i].ring_ready) {
                // liburing resources must be dismantled before node mutexes and
                // wake descriptors disappear.
                llam_node_unregister_cq_eventfd(&rt->nodes[i]);
                llam_node_destroy_recv_buf_ring(&rt->nodes[i]);
                io_uring_queue_exit(&rt->nodes[i].ring);
                rt->nodes[i].ring_ready = false;
            }
            if (rt->nodes[i].event_fd >= 0) {
                llam_wake_handle_close(rt->nodes[i].event_fd);
                rt->nodes[i].event_fd = -1;
            }
            if (rt->nodes[i].submit_lock_initialized) {
                pthread_mutex_destroy(&rt->nodes[i].submit_lock);
                rt->nodes[i].submit_lock_initialized = false;
            }
            if (rt->nodes[i].windows_assoc_lock_initialized) {
                pthread_mutex_destroy(&rt->nodes[i].windows_assoc_lock);
                rt->nodes[i].windows_assoc_lock_initialized = false;
            }
            if (rt->nodes[i].watch_lock_initialized) {
                pthread_mutex_destroy(&rt->nodes[i].watch_lock);
                rt->nodes[i].watch_lock_initialized = false;
            }
            if (rt->nodes[i].recv_buf_lock_initialized) {
                pthread_mutex_destroy(&rt->nodes[i].recv_buf_lock);
                rt->nodes[i].recv_buf_lock_initialized = false;
            }
        }
    }

    if (rt->shards != NULL) {
        for (i = 0; i < rt->active_shards; ++i) {
            if (rt->shards[i].opaque_lock_initialized) {
                pthread_mutex_lock(&rt->shards[i].opaque_lock);
                /* Worker joins above make these stores race-free. */
                llam_legacy_lease_finalize_stop_locked(&rt->shards[i]);
                atomic_store_explicit(&rt->shards[i].primary_worker.native_thread_live,
                                      false,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].primary_worker.binding_incarnation,
                                      0U,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].primary_worker.lease_reacquire_pending,
                                      false,
                                      memory_order_release);
                llam_worker_clear_active_g(&rt->shards[i].primary_worker);
                atomic_store_explicit(&rt->shards[i].primary_worker.last_run_started_ns,
                                      0U,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].primary_worker.last_safepoint_ns,
                                      0U,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].primary_worker.last_idle_wake_ns,
                                      0U,
                                      memory_order_release);
                memset(&rt->shards[i].primary_worker.native_thread,
                       0,
                       sizeof(rt->shards[i].primary_worker.native_thread));
                atomic_store_explicit(&rt->shards[i].opaque_helper_worker.native_thread_live,
                                      false,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].opaque_helper_worker.binding_incarnation,
                                      0U,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].opaque_helper_worker.lease_reacquire_pending,
                                      false,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].merge_pause_requested,
                                      0U,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].merge_pause_ack,
                                      0U,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].merge_pause_ack_sequence,
                                      0U,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].merge_pause_ack_lease_epoch,
                                      0U,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].merge_pause_ack_worker_incarnation,
                                      0U,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].steal_pause_ack,
                                      0U,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].steal_pause_ack_sequence,
                                      0U,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].steal_pause_ack_lease_epoch,
                                      0U,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].steal_pause_ack_worker_incarnation,
                                      0U,
                                      memory_order_release);
                llam_worker_clear_active_g(&rt->shards[i].opaque_helper_worker);
                atomic_store_explicit(&rt->shards[i].opaque_helper_worker.last_run_started_ns,
                                      0U,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].opaque_helper_worker.last_safepoint_ns,
                                      0U,
                                      memory_order_release);
                atomic_store_explicit(&rt->shards[i].opaque_helper_worker.last_idle_wake_ns,
                                      0U,
                                      memory_order_release);
                memset(&rt->shards[i].opaque_helper_worker.native_thread,
                       0,
                       sizeof(rt->shards[i].opaque_helper_worker.native_thread));
                pthread_mutex_unlock(&rt->shards[i].opaque_lock);
            }
            if (rt->shards[i].processor.event_fd >= 0) {
                llam_wake_handle_close(rt->shards[i].processor.event_fd);
                rt->shards[i].processor.event_fd = -1;
            }
            if (rt->shards[i].signal_stack_mapping != NULL) {
                (void)munmap(rt->shards[i].signal_stack_mapping,
                             rt->shards[i].signal_stack_mapping_size);
                rt->shards[i].signal_stack_mapping = NULL;
                rt->shards[i].signal_stack_mapping_size = 0U;
            }
            llam_ctx_destroy_fp_state(&rt->shards[i].scheduler_ctx);
            llam_ctx_destroy_fp_state(&rt->shards[i].opaque_scheduler_ctx);
            free(rt->shards[i].processor.timer_heap);
            rt->shards[i].processor.timer_heap = NULL;
            rt->shards[i].processor.timer_heap_len = 0U;
            rt->shards[i].processor.timer_heap_cap = 0U;
            rt->shards[i].processor.timers = NULL;
            atomic_store_explicit(&rt->shards[i].processor.timer_callbacks_active,
                                  0U,
                                  memory_order_release);
            llam_processor_drain_stack_cache(&rt->shards[i].processor);
            if (rt->shards[i].lock_initialized) {
                pthread_mutex_destroy(&rt->shards[i].lock);
                rt->shards[i].lock_initialized = false;
            }
            if (rt->shards[i].processor.stack_cache_lock_initialized) {
                pthread_mutex_destroy(&rt->shards[i].processor.stack_cache_lock);
                rt->shards[i].processor.stack_cache_lock_initialized = false;
            }
            llam_opaque_wake_destroy(&rt->shards[i]);
            if (rt->shards[i].opaque_cv_initialized) {
                pthread_cond_destroy(&rt->shards[i].opaque_cv);
                rt->shards[i].opaque_cv_initialized = false;
            }
            if (rt->shards[i].opaque_lock_initialized) {
                pthread_mutex_destroy(&rt->shards[i].opaque_lock);
                rt->shards[i].opaque_lock_initialized = false;
            }
        }
    }

    if (rt->stack_cache_lock_initialized) {
        llam_runtime_drain_stack_cache(rt);
    }

    if (rt->shards != NULL) {
        for (i = 0; i < rt->active_shards; ++i) {
            rt->shards[i].all_tasks = NULL;
        }
    }

    if (rt->shards != NULL) {
        for (i = 0; i < rt->active_shards; ++i) {
            // Walk allocation slabs rather than the optional diagnostic lists.
            // This releases resources for never-run and unjoined tasks too.
            llam_allocator_destroy(&rt->shards[i].processor.allocator);
        }
    }

    llam_async_context_shutdown(rt);
    llam_restore_process_signal_handlers(rt);
    llam_restore_init_thread_affinity(rt);

    if (rt->block_lock_initialized) {
        llam_alloc_chunk_t *chunk = rt->block_job_chunks;

        // Blocking-job chunks are runtime-wide, not shard allocator chunks.
        while (chunk != NULL) {
            llam_alloc_chunk_t *next = chunk->next;

            if (chunk->mmapped) {
                (void)munmap(chunk->storage, chunk->bytes);
            } else {
                free(chunk->storage);
            }
            free(chunk);
            chunk = next;
        }
        rt->block_job_chunks = NULL;
        if (rt->block_cv_initialized) {
            pthread_cond_destroy(&rt->block_cv);
            rt->block_cv_initialized = false;
        }
        pthread_mutex_destroy(&rt->block_lock);
        rt->block_lock_initialized = false;
    }
    if (rt->task_list_lock_initialized) {
        pthread_mutex_destroy(&rt->task_list_lock);
        rt->task_list_lock_initialized = false;
    }
    if (rt->stack_cache_lock_initialized) {
        pthread_mutex_destroy(&rt->stack_cache_lock);
        rt->stack_cache_lock_initialized = false;
    }
    if (rt->overflow_lock_initialized) {
        pthread_mutex_destroy(&rt->overflow_lock);
        rt->overflow_lock_initialized = false;
    }
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    if (rt->windows_async_config_lock_initialized) {
        if (rt->windows_async_host_spawn_admits != 0U) {
            abort();
        }
        pthread_mutex_destroy(&rt->windows_async_config_lock);
        rt->windows_async_config_lock_initialized = false;
    }
#endif

    retired_catalog = llam_managed_catalog_detach(rt);
    if (retired_catalog == NULL && errno != 0) abort();
    free(rt->block_threads);
    free(rt->kernel_node_ids);
    free(rt->nodes);
    llam_worker_registry_destroy(rt);
    free(rt->shards);
    free(rt->allowed_cpus);
#if LLAM_RUNTIME_BACKEND_WINDOWS
    if (rt->winsock_started) {
        WSACleanup();
        rt->winsock_started = false;
    }
#endif
    llam_release_xsave_globals(rt);
    /*
     * Remove the runtime from the public handle registry before zeroing it.
     * Otherwise an old pointer can pass handle validation and race into freed
     * scheduler/backend state.
     */
    llam_runtime_unregister_handle(rt);
    // Clear the runtime last so accidental post-shutdown reads fail closed.
    memset(rt, 0, sizeof(*rt));
    return retired_catalog;
}

/**
 * @brief Publish destructive stop before waiting for an active run driver.
 *
 * Destroy/shutdown is stronger than cooperative request_stop: no new blocking
 * helper jobs may be accepted, and idle block workers are allowed to leave their
 * runtime-lifetime loop.  Running callbacks still finish normally before their
 * parked task is woken, preserving callback argument lifetime.
 */
static void llam_runtime_request_destroy_stop(llam_runtime_t *rt) {
    unsigned i;

    atomic_store_explicit(&rt->shutdown_requested, true, memory_order_release);
    llam_request_stop(rt);
    /* Publish STOPPING before waiting for a scheduler run to drain. */
    if (rt->shards == NULL) {
        return;
    }
    for (i = 0; i < rt->active_shards; ++i) {
        if (!rt->shards[i].opaque_lock_initialized) {
            continue;
        }
        pthread_mutex_lock(&rt->shards[i].opaque_lock);
        llam_legacy_lease_begin_stop_locked(&rt->shards[i]);
        pthread_mutex_unlock(&rt->shards[i].opaque_lock);
        if (atomic_load_explicit(&rt->async_preempt_enabled, memory_order_acquire) != 0U) {
            /* The earlier stop wake may precede this P's drain projection. */
            llam_kick_shard(&rt->shards[i]);
        }
    }
}

/**
 * @brief Wait for one opaque helper to publish its exit boundary.
 *
 * The helper publishes this acknowledgement only after active-G cleanup and
 * lease handback have completed.  The wait intentionally performs
 * no scheduler-lock acquisition so a helper can finish the boundary while
 * teardown is waiting.
 */
static void llam_runtime_wait_opaque_helper_exit_ack(llam_shard_t *shard) {
    if (shard == NULL) {
        return;
    }
    while (!atomic_load_explicit(&shard->opaque_helper_exit_ack,
                                 memory_order_acquire)) {
        llam_pause_cpu();
    }
}

/**
 * @brief Wait for llam_runtime_run_rt() to publish exec_started=false.
 *
 * The initial stop request can race with a worker entering an idle wait or a
 * task parking immediately after the cancellation scan. Reissue stop
 * periodically while spinning so destroy cannot silently depend on a single
 * wake/cancel pass.
 */
static void llam_runtime_wait_exec_stopped(llam_runtime_t *rt) {
    unsigned spins = 0U;

    while (atomic_load_explicit(&rt->exec_started, memory_order_acquire)) {
        if ((++spins & 0x3ffU) == 0U) {
            llam_request_stop(rt);
        }
        llam_pause_cpu();
    }
}

void llam_runtime_shutdown_rt(llam_runtime_t *rt) {
    LLAM_NATIVE_API_GUARD();
    llam_runtime_t *current_runtime;

    if (rt == NULL) {
        rt = llam_runtime_default_storage();
    }
    /*
     * A managed task runs on scheduler-owned stack/context state.  Tearing the
     * owning runtime down from that same execution frame can deadlock in worker
     * joins or free the task currently returning from this function. Treat
     * in-runtime calls as a cooperative stop request; the host thread that owns
     * the run call must perform actual resource teardown after the scheduler
     * exits.
     */
    if (g_llam_tls_task != NULL || g_llam_tls_scheduler_ctx != NULL) {
        int saved_errno = errno;

        current_runtime = llam_runtime_current_owner();
        if (rt != current_runtime) {
            /*
             * Runtime handles are host control capabilities.  A task running
             * inside runtime A must not be able to turn a shared runtime B
             * pointer into a cross-runtime stop signal.
             */
            errno = saved_errno;
            return;
        }
        if (llam_runtime_check_handle(rt) != 0) {
            errno = saved_errno;
            return;
        }
        if (atomic_load_explicit(&rt->initialized, memory_order_acquire)) {
            llam_request_stop(rt);
        }
        errno = saved_errno;
        return;
    }

    /*
     * Shutdown must be serialized with runtime construction.  The initialized
     * flag is published late, but partial-init resources are published earlier
     * for failure cleanup; tearing those down from another host thread while
     * init still consumes them can corrupt the runtime object.
     */
    {
        bool heap_runtime = false;

        if (llam_runtime_claim_destroy_handle(rt, &heap_runtime) == 0) {
            (void)heap_runtime;
            if (atomic_load_explicit(&rt->exec_started, memory_order_acquire)) {
                llam_runtime_request_destroy_stop(rt);
                llam_runtime_wait_exec_stopped(rt);
            }
            llam_runtime_lifecycle_lock();
            llam_managed_catalog_t *retired = llam_runtime_shutdown_unlocked(rt);
            llam_runtime_lifecycle_unlock();
            llam_managed_catalog_retire(retired);
        }
    }
}

void llam_runtime_shutdown(void) {
    LLAM_NATIVE_API_GUARD();
    /*
     * Match llam_runtime_request_stop(): unmanaged callers operate on the
     * process-default runtime, while managed tasks request cooperative stop on
     * their owner runtime instead of accidentally targeting the default.
     */
    llam_runtime_shutdown_rt(llam_runtime_current_owner());
}

void llam_runtime_destroy_rt(llam_runtime_t *rt) {
    bool heap_runtime = false;

    if (rt == NULL) {
        llam_runtime_shutdown_rt(llam_runtime_default_storage());
        return;
    }
    /*
     * In-runtime destroy requests can only be cooperative stop requests.  The
     * task is still executing on runtime-owned stack/context state, so claiming
     * and freeing the handle here would turn a benign stop call into UAF.
     */
    if (g_llam_tls_task != NULL || g_llam_tls_scheduler_ctx != NULL) {
        llam_runtime_shutdown_rt(rt);
        return;
    }

    if (llam_runtime_claim_destroy_handle(rt, &heap_runtime) == 0) {
        if (atomic_load_explicit(&rt->exec_started, memory_order_acquire)) {
            llam_runtime_request_destroy_stop(rt);
            llam_runtime_wait_exec_stopped(rt);
        }
        llam_runtime_lifecycle_lock();
        llam_managed_catalog_t *retired = llam_runtime_shutdown_unlocked(rt);
        llam_runtime_lifecycle_unlock();
        llam_managed_catalog_retire(retired);
    }

    if (heap_runtime) {
        llam_runtime_retire_heap_handle(rt);
    }
}
