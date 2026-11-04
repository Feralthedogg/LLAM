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
 * See LICENSE.
 *
 * Home page sample. It builds and runs as written; the part after the fold
 * marker starts folded on the page.
 */
#include <llam/runtime.h>
#include <stdio.h>

static void producer(void *arg) {
    llam_channel_t *channel = arg;
    (void)llam_channel_send(channel, "ping");
    (void)llam_channel_send(channel, "pong");
    (void)llam_channel_close(channel);
}

static void consumer(void *arg) {
    llam_channel_t *channel = arg;
    void *value = NULL;

    while (llam_channel_recv_result(channel, &value) == 0) {
        printf("recv=%s\n", (const char *)value);
    }
}

/* fold */
static void root(void *arg) {
    (void)arg;
    llam_channel_t *channel = llam_channel_create(2);
    if (channel == NULL) return;
    llam_task_t *a = llam_spawn(producer, channel, NULL);
    llam_task_t *b = llam_spawn(consumer, channel, NULL);
    if (a != NULL) (void)llam_join(a);
    if (b != NULL) (void)llam_join(b);
    (void)llam_channel_destroy(channel);
}

int main(void) {
    llam_runtime_t *runtime = NULL;
    if (llam_runtime_create_compat(NULL, 0, &runtime) != 0) return 1;
    llam_task_t *task = llam_runtime_spawn_ex(runtime, root, NULL, NULL, 0);
    if (task == NULL || llam_runtime_run_handle(runtime) != 0) {
        llam_runtime_destroy(runtime);
        return 1;
    }
    int rc = llam_join(task);
    llam_runtime_destroy(runtime);
    return rc;
}
