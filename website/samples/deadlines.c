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
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>

static void root(void *arg) {
    (void)arg;
    int sv[2];
    char buf[64];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
    llam_fd_t fd = sv[0];

    uint64_t deadline = llam_now_ns() + 250ULL * 1000000ULL;
    ssize_t n = llam_read_until(fd, buf, sizeof buf, deadline);
    if (n < 0 && errno == ETIMEDOUT) {
        printf("read timed out\n");
    }
    (void)llam_close(sv[0]);
    (void)llam_close(sv[1]);
}

/* fold */
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
