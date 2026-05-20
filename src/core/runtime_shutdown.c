/**
 * @file src/core/runtime_shutdown.c
 * @brief Runtime shutdown, worker teardown, and global state cleanup.
 *
 * @details
 * Shutdown is intentionally conservative: request global stop, join worker
 * threads, drain backend queues/watch state, destroy platform resources, release
 * cached stacks and allocator slabs, restore process-wide signal/affinity state,
 * and finally zero the global runtime object.
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
static void llam_runtime_shutdown_unlocked(void) {
    llam_runtime_t *rt = &g_llam_runtime;
    llam_task_t *task;
    unsigned i;

    if (!atomic_load_explicit(&rt->initialized, memory_order_acquire) &&
        !atomic_load_explicit(&rt->exec_started, memory_order_acquire) &&
        rt->allowed_cpus == NULL && rt->shards == NULL && rt->nodes == NULL
#if LLAM_RUNTIME_BACKEND_WINDOWS
        && !rt->winsock_started
#endif
    ) {
        return;
    }

    // The calling thread is no longer considered a runtime worker while
    // teardown is in progress.
    g_llam_tls_shard = NULL;
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = NULL;
    atomic_store_explicit(&rt->shutdown_requested, true, memory_order_release);
    llam_request_stop(rt);

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

    if (rt->nodes != NULL) {
        for (i = 0; i < rt->active_nodes; ++i) {
            if (rt->nodes[i].thread_started) {
                pthread_join(rt->nodes[i].thread, NULL);
                rt->nodes[i].thread_started = false;
            }
            if (rt->nodes[i].watch_lock_initialized) {
                pthread_mutex_lock(&rt->nodes[i].watch_lock);
                // Control operations are heap-allocated command nodes owned by the
                // I/O node after enqueue.
                while (rt->nodes[i].control_head != NULL) {
                    llam_io_control_op_t *next = rt->nodes[i].control_head->next;
                    free(rt->nodes[i].control_head);
                    rt->nodes[i].control_head = next;
                }
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
            if (rt->shards[i].opaque_helper_thread_started) {
                // Opaque helpers may be asleep on their private wake path; set
                // the stop flag under lock and signal both helper and shard.
                pthread_mutex_lock(&rt->shards[i].opaque_lock);
                rt->shards[i].opaque_helper_stop = true;
                llam_opaque_wake_signal(&rt->shards[i]);
                pthread_mutex_unlock(&rt->shards[i].opaque_lock);
                llam_kick_shard(&rt->shards[i]);
                pthread_join(rt->shards[i].opaque_helper_thread, NULL);
                rt->shards[i].opaque_helper_thread_started = false;
            }
            if (rt->shards[i].event_fd >= 0) {
                llam_wake_handle_close(rt->shards[i].event_fd);
                rt->shards[i].event_fd = -1;
            }
            if (rt->shards[i].signal_stack != NULL) {
                munmap(rt->shards[i].signal_stack, rt->shards[i].signal_stack_size);
            }
            llam_ctx_destroy_fp_state(&rt->shards[i].scheduler_ctx);
            llam_ctx_destroy_fp_state(&rt->shards[i].opaque_scheduler_ctx);
            free(rt->shards[i].timer_heap);
            rt->shards[i].timer_heap = NULL;
            rt->shards[i].timer_heap_len = 0U;
            rt->shards[i].timer_heap_cap = 0U;
            rt->shards[i].timers = NULL;
            atomic_store_explicit(&rt->shards[i].timer_callbacks_active, 0U, memory_order_release);
            llam_shard_drain_stack_cache(&rt->shards[i]);
            if (rt->shards[i].lock_initialized) {
                pthread_mutex_destroy(&rt->shards[i].lock);
                rt->shards[i].lock_initialized = false;
            }
            if (rt->shards[i].stack_cache_lock_initialized) {
                pthread_mutex_destroy(&rt->shards[i].stack_cache_lock);
                rt->shards[i].stack_cache_lock_initialized = false;
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
            task = rt->shards[i].all_tasks;
            rt->shards[i].all_tasks = NULL;
            while (task != NULL) {
                llam_task_t *next = task->all_next;

                task->all_next = NULL;
                task->all_prev = NULL;
                llam_free_task(task);
                task = next;
            }
        }
    }

    if (rt->shards != NULL) {
        for (i = 0; i < rt->active_shards; ++i) {
            // Allocators are destroyed after tasks are freed because task slabs
            // own the task objects and embedded mutexes.
            llam_allocator_destroy(&rt->shards[i].allocator);
        }
    }

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

    free(rt->block_threads);
    free(rt->kernel_node_ids);
    free(rt->nodes);
    free(rt->shards);
    free(rt->allowed_cpus);
#if LLAM_RUNTIME_BACKEND_WINDOWS
    if (rt->winsock_started) {
        WSACleanup();
        rt->winsock_started = false;
    }
#endif
    llam_clear_xsave_globals();
    // Clear the singleton last so accidental post-shutdown reads fail closed.
    memset(rt, 0, sizeof(*rt));
}

void llam_runtime_shutdown(void) {
    /*
     * A managed task runs on scheduler-owned stack/context state.  Tearing the
     * singleton down from that same execution frame can deadlock in worker joins
     * or free the task currently returning from this function.  Treat in-runtime
     * calls as a cooperative stop request; the host thread that owns llam_run()
     * must perform the actual resource teardown after the scheduler exits.
     */
    if (g_llam_tls_task != NULL || g_llam_tls_scheduler_ctx != NULL) {
        int saved_errno = errno;

        if (atomic_load_explicit(&g_llam_runtime.initialized, memory_order_acquire)) {
            llam_request_stop(&g_llam_runtime);
        }
        errno = saved_errno;
        return;
    }

    /*
     * Shutdown must be serialized with runtime construction.  The initialized
     * flag is published late, but partial-init resources are published earlier
     * for failure cleanup; tearing those down from another host thread while
     * init still consumes them can corrupt the singleton.
     */
    llam_runtime_lifecycle_lock();
    llam_runtime_shutdown_unlocked();
    llam_runtime_lifecycle_unlock();
}
