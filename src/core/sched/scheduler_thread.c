/**
 * @file src/core/sched/scheduler_thread.c
 * @brief Native-thread scope setup for scheduler execution.
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

static bool llam_scheduler_signal_stack_failure_is_fatal(int err) {
    /*
     * The alternate signal stack is a diagnostics surface for guard-page fault
     * reports, not a correctness prerequisite for running tasks. DragonFlyBSD
     * can transiently report EAGAIN when shard 0 moves across host threads.
     */
    return err != EAGAIN;
}

int llam_scheduler_try_install_signal_stack(llam_shard_t *shard) {
    int saved_errno;

    if (llam_install_thread_signal_stack(shard) == 0) {
        return 0;
    }
    saved_errno = errno;
    if (!llam_scheduler_signal_stack_failure_is_fatal(saved_errno)) {
        errno = 0;
        return 0;
    }
    errno = saved_errno;
    return -1;
}

#if !LLAM_RUNTIME_BACKEND_WINDOWS
void llam_shard_publish_preempt_thread(llam_shard_t *shard,
                                       pthread_t thread) {
    pthread_mutex_lock(&shard->lock);
    shard->preempt_thread = thread;
    pthread_mutex_unlock(&shard->lock);
}
#endif

int llam_scheduler_thread_enter(llam_shard_t *shard,
                                atomic_uint *thread_counter,
                                bool *signal_stack_installed) {
    llam_runtime_t *rt;
    bool thread_counted = false;
    int saved_errno = 0;

    if (shard == NULL || shard->runtime == NULL ||
        thread_counter == NULL || signal_stack_installed == NULL) {
        errno = EINVAL;
        return -1;
    }
    rt = shard->runtime;
    *signal_stack_installed = false;
    g_llam_tls_shard = shard;
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = &shard->scheduler_ctx;
    thread_counted = llam_runtime_native_thread_enter(rt, thread_counter);
    if (!thread_counted) {
        saved_errno = errno != 0 ? errno : EOVERFLOW;
        goto fail;
    }
#if !LLAM_RUNTIME_BACKEND_WINDOWS
    llam_shard_publish_preempt_thread(shard, pthread_self());
#endif
    shard->primary_thread = pthread_self();
    if (llam_runtime_apply_worker_affinity(rt, shard->cpu_id) != 0) {
        saved_errno = errno != 0 ? errno : EIO;
        goto fail;
    }
    if (!rt->external_driver.enabled) {
        llam_tune_scheduler_thread(shard, false);
    }
    if (llam_scheduler_try_install_signal_stack(shard) != 0) {
        saved_errno = errno != 0 ? errno : EIO;
        goto fail;
    }
    *signal_stack_installed = true;
    return 0;

fail:
    llam_channel_tls_cache_drain();
    g_llam_tls_shard = NULL;
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = NULL;
    if (thread_counted) {
        llam_runtime_native_thread_exit(rt, thread_counter);
    }
    errno = saved_errno;
    return -1;
}

void llam_scheduler_thread_leave(llam_shard_t *shard,
                                 atomic_uint *thread_counter,
                                 bool signal_stack_installed) {
    llam_runtime_t *rt =
        shard != NULL ? shard->runtime : NULL;

    if (shard == NULL || rt == NULL || thread_counter == NULL) {
        return;
    }
    if (signal_stack_installed) {
        llam_uninstall_thread_signal_stack(shard);
    }
    llam_channel_tls_cache_drain();
    g_llam_tls_shard = NULL;
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = NULL;
    llam_runtime_native_thread_exit(rt, thread_counter);
}
