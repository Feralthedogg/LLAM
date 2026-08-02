/**
 * @file tests/test_host_process_cases.inc
 * @brief POSIX fork/exec host-boundary regression cases.
 *
 * @details
 * A child created after runtime initialization may use only async-signal-safe
 * preparation before exec or exit. The parent must retain a usable runtime.
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

static void host_process_parent_probe_task(void *arg) {
    atomic_store_explicit((atomic_uint *)arg, 1U, memory_order_release);
}

static int test_host_process_fork_exec_contract(void) {
#if LLAM_PLATFORM_POSIX
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    atomic_uint parent_ran;
    pid_t pid;
    int status = 0;
    int rc = 1;

    atomic_init(&parent_ran, 0U);
    if (init_runtime_opts(&opts) != 0 ||
        llam_runtime_create(
            &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
        return test_fail_errno("fork contract runtime create failed");
    }
    pid = fork();
    if (pid < 0) {
        rc = test_fail_errno("fork contract fork failed");
        goto cleanup;
    }
    if (pid == 0) {
        char *const child_argv[] = {(char *)"true", NULL};
        char *const child_env[] = {NULL};

        (void)alarm(3U);
        (void)execve("/usr/bin/true", child_argv, child_env);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            rc = test_fail_errno("fork contract waitpid failed");
            goto cleanup;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        rc = test_fail("forked child did not exec and exit cleanly");
        goto cleanup;
    }
    task = llam_runtime_spawn_ex(
        runtime, host_process_parent_probe_task, &parent_ran, NULL, 0U);
    if (task == NULL || llam_runtime_run_handle(runtime) != 0) {
        rc = test_fail_errno("parent runtime did not survive child exec");
        goto cleanup;
    }
    if (llam_join(task) != 0) {
        rc = test_fail_errno("parent task join failed after child exec");
        goto cleanup;
    }
    task = NULL;
    if (atomic_load_explicit(&parent_ran, memory_order_acquire) != 1U) {
        rc = test_fail("parent task did not run after child exec");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (task != NULL) {
        (void)llam_detach(task);
    }
    llam_runtime_destroy(runtime);
    return rc;
#else
    return 0;
#endif
}
