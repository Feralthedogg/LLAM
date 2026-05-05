/**
 * @file src/core/runtime_run.c
 * @brief Top-level scheduler run loop coordination.
 *
 * @details
 * ::nm_run starts secondary shard workers, runs shard 0 on the calling thread,
 * joins the workers when the scheduler drains, and propagates any fatal runtime
 * error recorded by worker threads.
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
 * @brief Run the initialized runtime until all scheduled work completes.
 *
 * The caller owns shard 0's scheduler loop. Additional shards are started as
 * pthread workers before shard 0 enters ::nm_scheduler_loop. After the primary
 * scheduler exits, all secondary workers are joined and any recorded fatal error
 * is surfaced through @c errno.
 *
 * @return 0 when the runtime drains normally.
 * @return -1 with @c errno set if the runtime is not initialized, already
 *         running, a worker cannot be started, or a worker records a fatal
 *         runtime error.
 *
 * @see nm_runtime_init
 * @see nm_scheduler_loop
 */
int nm_run(void) {
    nm_runtime_t *rt = &g_nm_runtime;
    unsigned i;

    if (!rt->initialized || rt->exec_started) {
        errno = EINVAL;
        return -1;
    }

    rt->exec_started = true;
    for (i = 1; i < rt->active_shards; ++i) {
        if (pthread_create(&rt->shards[i].thread, NULL, nm_shard_worker_main, &rt->shards[i]) != 0) {
            nm_record_fatal(rt, errno);
            rt->exec_started = false;
            return -1;
        }
        rt->shards[i].thread_started = true;
    }

    nm_scheduler_loop(&rt->shards[0]);

    for (i = 1; i < rt->active_shards; ++i) {
        if (rt->shards[i].thread_started) {
            pthread_join(rt->shards[i].thread, NULL);
            rt->shards[i].thread_started = false;
        }
    }
    rt->exec_started = false;

    if (atomic_load(&rt->fatal_errno) != 0) {
        errno = atomic_load(&rt->fatal_errno);
        return -1;
    }

    return 0;
}
