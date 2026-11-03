/*
 * Copyright 2026 Feralthedogg
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
 * See LICENSES/Apache-2.0.txt.
 */

#include "llam/runtime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct request {
    llam_channel_t *channel;
} request_t;

static void producer(void *argument) {
    request_t *request = argument;
    int error = 0;
    if (llam_sleep_ns(UINT64_C(1000000)) != 0 ||
        llam_channel_send(request->channel, NULL) != 0) error = errno;
    (void)llam_task_set_result(NULL, error);
}

static void consumer(void *argument) {
    request_t *request = argument;
    void *message = request;
    int error = 0;
    if (llam_channel_recv_result(request->channel, &message) != 0) error = errno;
    else if (message != NULL) error = EINVAL;
    (void)llam_task_set_result(message, error);
}

int main(int argc, char **argv) {
    int status = EXIT_FAILURE;
    int cancel = argc == 2 && strcmp(argv[1], "--cancel") == 0;
    int ran = 0, joined = 0;
    llam_runtime_t *runtime = NULL;
    llam_task_group_t *group = NULL;
    llam_cancel_token_t *external = NULL;
    request_t request = {0};
    llam_runtime_opts_t options;
    llam_spawn_opts_t spawn;
    llam_task_group_opts_t group_options = {.flags = LLAM_TASK_GROUP_F_LINK_CANCEL};
    if (llam_runtime_opts_init(&options, sizeof(options)) != 0 ||
        llam_spawn_opts_init(&spawn, sizeof(spawn)) != 0) goto cleanup;
    options.worker_count = 1U;
    options.blocking_worker_count = 1U;
    options.max_tasks = 16U;
    if (llam_runtime_create(&options, sizeof(options), &runtime) != 0) goto cleanup;
    request.channel = llam_runtime_channel_create(runtime, 1U);
    external = llam_runtime_cancel_token_create(runtime);
    group = llam_runtime_task_group_create_ex(runtime, &group_options, sizeof(group_options));
    if (request.channel == NULL || external == NULL || group == NULL) goto cleanup;
    spawn.cancel_token = external;
    spawn.deadline_ns = llam_now_ns() + UINT64_C(1000000000);
    /* These child handles are borrowed: the group consumes them at group_join. */
    llam_task_t *writer = llam_task_group_spawn_ex(group, producer, &request, &spawn, sizeof(spawn));
    llam_task_t *reader = llam_task_group_spawn_ex(group, consumer, &request, &spawn, sizeof(spawn));
    if (writer == NULL || reader == NULL) goto cleanup;
    if (cancel && llam_task_group_cancel(group) != 0) goto cleanup;
    /* Host joins do not drive scheduling. Run first (or use a separate driver). */
    if (llam_runtime_run_handle(runtime) != 0) goto cleanup;
    ran = 1;
    llam_task_result_t writer_result, reader_result;
    if (llam_task_get_result(writer, &writer_result, sizeof(writer_result)) != 0 ||
        llam_task_get_result(reader, &reader_result, sizeof(reader_result)) != 0 ||
        writer_result.flags != LLAM_TASK_RESULT_F_SET || reader_result.flags != LLAM_TASK_RESULT_F_SET ||
        writer_result.error != (cancel ? ECANCELED : 0) ||
        reader_result.error != (cancel ? ECANCELED : 0) ||
        llam_cancel_token_is_cancelled(external) != 0) goto cleanup;
    if (llam_task_group_join(group) != 0) goto cleanup;
    joined = 1;
    status = EXIT_SUCCESS;
cleanup:
    if (status != EXIT_SUCCESS) perror("explicit runtime request");
    if (group != NULL && !joined) {
        if (llam_task_group_cancel(group) != 0) perror("cancel group during cleanup");
        if (!ran && llam_runtime_run_handle(runtime) != 0) perror("run accepted work during cleanup");
        if (llam_task_group_join_until(group, llam_now_ns() + UINT64_C(1000000000)) != 0) {
            /* A runtime/backend failure can require final teardown. Caller-owned
             * group and channel handles remain available for cleanup afterward. */
            llam_runtime_destroy(runtime);
            runtime = NULL;
        }
    }
    if (request.channel != NULL) {
        if (llam_channel_close(request.channel) != 0) status = EXIT_FAILURE;
        void *discard = NULL;
        while (llam_channel_try_recv_result(request.channel, &discard) == 0) { }
        if (llam_channel_destroy(request.channel) != 0) { perror("destroy channel"); status = EXIT_FAILURE; }
    }
    if (group != NULL && llam_task_group_destroy(group) != 0) { perror("destroy group"); status = EXIT_FAILURE; }
    if (external != NULL && llam_cancel_token_destroy(external) != 0) { perror("destroy token"); status = EXIT_FAILURE; }
    if (runtime != NULL) llam_runtime_destroy(runtime);
    return status;
}
