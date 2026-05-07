/**
 * @file tests/test_nm_compat_runtime.c
 * @brief Legacy nm_* runtime compatibility wrapper behavior tests.
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

#include "llam/nm_runtime.h"

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if NM_PLATFORM_POSIX
#include <unistd.h>
#endif

extern const char *llam_env_get(const char *name);

typedef struct nm_state {
    nm_channel_t *channel;
    atomic_uint failures;
    atomic_uint sender_done;
    atomic_uint receiver_done;
    atomic_uint detached_done;
    atomic_uint received;
    int first_errno;
    char first_case[128];
} nm_state_t;

static int test_fail(const char *message) {
    fprintf(stderr, "[test_nm_compat_runtime] %s\n", message);
    return 1;
}

static int test_fail_errno(const char *message) {
    fprintf(stderr, "[test_nm_compat_runtime] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static int test_nm_env_aliases(void) {
#if NM_PLATFORM_WINDOWS
    return 0;
#else
    const char *value;

    (void)unsetenv("LLAM_TEST_ALIAS");
    (void)unsetenv("NM_TEST_ALIAS");
    if (setenv("NM_TEST_ALIAS", "ok", 1) != 0) {
        return test_fail_errno("setenv NM_TEST_ALIAS failed");
    }
    value = llam_env_get("LLAM_TEST_ALIAS");
    if (value == NULL || strcmp(value, "ok") != 0) {
        (void)unsetenv("NM_TEST_ALIAS");
        return test_fail("LLAM_TEST_ALIAS did not fall back to NM_TEST_ALIAS");
    }
    if (setenv("LLAM_TEST_ALIAS", "canonical", 1) != 0) {
        (void)unsetenv("NM_TEST_ALIAS");
        return test_fail_errno("setenv LLAM_TEST_ALIAS failed");
    }
    value = llam_env_get("LLAM_TEST_ALIAS");
    if (value == NULL || strcmp(value, "canonical") != 0) {
        (void)unsetenv("LLAM_TEST_ALIAS");
        (void)unsetenv("NM_TEST_ALIAS");
        return test_fail("canonical LLAM env did not win over NM alias");
    }
    (void)unsetenv("LLAM_TEST_ALIAS");
    (void)unsetenv("NM_TEST_ALIAS");

    (void)unsetenv("LLAM_EXPERIMENTAL_WORKER_RINGS");
    (void)unsetenv("LLAM_EXPERIMENTAL_SHARD_RINGS");
    (void)unsetenv("NM_EXPERIMENTAL_SHARD_RINGS");
    if (setenv("NM_EXPERIMENTAL_SHARD_RINGS", "1", 1) != 0) {
        return test_fail_errno("setenv NM_EXPERIMENTAL_SHARD_RINGS failed");
    }
    value = llam_env_get("LLAM_EXPERIMENTAL_WORKER_RINGS");
    if (value == NULL || strcmp(value, "1") != 0) {
        (void)unsetenv("NM_EXPERIMENTAL_SHARD_RINGS");
        return test_fail("worker-ring env did not accept NM shard alias");
    }
    (void)unsetenv("NM_EXPERIMENTAL_SHARD_RINGS");
    return 0;
#endif
}

static void task_fail(nm_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static void nm_sender_task(void *arg) {
    nm_state_t *state = arg;
    nm_task_t *self = nm_current_task();

    if (self == NULL || nm_task_id(self) == 0U) {
        task_fail(state, "nm_current_task/nm_task_id", EINVAL);
        return;
    }
    if (nm_task_class(self) != NM_TASK_CLASS_LATENCY) {
        task_fail(state, "initial nm_task_class", EINVAL);
        return;
    }
    if (nm_task_set_class(NM_TASK_CLASS_BATCH) != 0) {
        task_fail(state, "nm_task_set_class failed", errno);
        return;
    }
    if (nm_task_class(self) != NM_TASK_CLASS_BATCH) {
        task_fail(state, "nm_task_set_class", EINVAL);
        return;
    }
    if (nm_channel_send(state->channel, (void *)(uintptr_t)77U) != 0) {
        task_fail(state, "nm_channel_send", errno);
        return;
    }
    nm_task_safepoint();
    nm_yield();
    atomic_fetch_add_explicit(&state->sender_done, 1U, memory_order_relaxed);
}

static void nm_receiver_task(void *arg) {
    nm_state_t *state = arg;
    void *value = nm_channel_recv(state->channel);

    if (value != (void *)(uintptr_t)77U) {
        task_fail(state, "nm_channel_recv", value == NULL ? errno : EPROTO);
        return;
    }
    atomic_store_explicit(&state->received, (unsigned)(uintptr_t)value, memory_order_relaxed);
    atomic_fetch_add_explicit(&state->receiver_done, 1U, memory_order_relaxed);
}

static void nm_detached_task(void *arg) {
    nm_state_t *state = arg;

    nm_yield();
    atomic_fetch_add_explicit(&state->detached_done, 1U, memory_order_relaxed);
}

static int test_nm_cancel_token_contract(void) {
    nm_cancel_token_t *token = nm_cancel_token_create();

    if (token == NULL) {
        return test_fail_errno("nm_cancel_token_create failed");
    }
    if (nm_cancel_token_is_cancelled(token) != 0) {
        (void)nm_cancel_token_destroy(token);
        return test_fail("new nm cancel token was already cancelled");
    }
    if (nm_cancel_token_cancel(token) != 0) {
        (void)nm_cancel_token_destroy(token);
        return test_fail_errno("nm_cancel_token_cancel failed");
    }
    if (nm_cancel_token_is_cancelled(token) == 0) {
        (void)nm_cancel_token_destroy(token);
        return test_fail("cancelled nm token did not report cancellation");
    }
    if (nm_cancel_token_destroy(token) != 0) {
        return test_fail_errno("nm_cancel_token_destroy failed");
    }
    errno = 0;
    if (nm_cancel_token_destroy(NULL) != -1 || errno != EINVAL) {
        return test_fail("nm_cancel_token_destroy(NULL) did not fail with EINVAL");
    }
    return 0;
}

static int test_nm_runtime_wrappers(void) {
    nm_state_t state;
    nm_runtime_opts_t runtime_opts;
    nm_spawn_opts_t spawn_opts;
    nm_runtime_stats_t stats;
    nm_runtime_stats_t prefix_stats;
    nm_task_t *sender;
    nm_task_t *detached;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.sender_done, 0U);
    atomic_init(&state.receiver_done, 0U);
    atomic_init(&state.detached_done, 0U);
    atomic_init(&state.received, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.forced_yield_every = 1U;
    runtime_opts.experimental_lockfree_normq = 1U;
    runtime_opts.profile = NM_RUNTIME_PROFILE_RELEASE_FAST;
    if (nm_runtime_init_ex(&runtime_opts, sizeof(runtime_opts)) != 0) {
        return test_fail_errno("nm_runtime_init_ex failed");
    }
    errno = 0;
    if (nm_runtime_write_stats_json(-1) != -1 || errno != EINVAL) {
        nm_runtime_shutdown();
        return test_fail("nm_runtime_write_stats_json invalid fd did not fail with EINVAL");
    }

    state.channel = nm_channel_create(1U);
    if (state.channel == NULL) {
        nm_runtime_shutdown();
        return test_fail_errno("nm_channel_create failed");
    }
    memset(&spawn_opts, 0, sizeof(spawn_opts));
    spawn_opts.task_class = NM_TASK_CLASS_LATENCY;
    spawn_opts.stack_class = NM_STACK_CLASS_DEFAULT;
    spawn_opts.flags = NM_SPAWN_F_NO_PREEMPT;
    sender = nm_spawn_ex(nm_sender_task, &state, &spawn_opts, sizeof(spawn_opts));
    detached = nm_spawn(nm_detached_task, &state, NULL);
    if (sender == NULL || detached == NULL || nm_spawn(nm_receiver_task, &state, NULL) == NULL) {
        nm_channel_destroy(state.channel);
        nm_runtime_shutdown();
        return test_fail_errno("nm_spawn failed");
    }
    if (nm_detach(detached) != 0) {
        nm_channel_destroy(state.channel);
        nm_runtime_shutdown();
        return test_fail_errno("nm_detach before run failed");
    }
    if (nm_task_class(sender) != NM_TASK_CLASS_LATENCY ||
        (nm_task_flags(sender) & NM_SPAWN_F_NO_PREEMPT) == 0U) {
        nm_channel_destroy(state.channel);
        nm_runtime_shutdown();
        return test_fail("nm task metadata mismatch before run");
    }
    if (nm_run() != 0) {
        nm_channel_destroy(state.channel);
        nm_runtime_shutdown();
        return test_fail_errno("nm_run failed");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_nm_compat_runtime] task failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        nm_channel_destroy(state.channel);
        nm_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.sender_done, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.receiver_done, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.detached_done, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.received, memory_order_relaxed) != 77U) {
        nm_channel_destroy(state.channel);
        nm_runtime_shutdown();
        return test_fail("nm wrapper task results were unexpected");
    }
    if (nm_detach(sender) != 0) {
        nm_channel_destroy(state.channel);
        nm_runtime_shutdown();
        return test_fail_errno("nm_detach after run failed");
    }
    if (nm_runtime_collect_stats_ex(&stats, sizeof(stats)) != 0) {
        nm_channel_destroy(state.channel);
        nm_runtime_shutdown();
        return test_fail_errno("nm_runtime_collect_stats_ex failed");
    }
    if (stats.active_shards == 0U || stats.active_nodes == 0U || stats.yields == 0U) {
        nm_channel_destroy(state.channel);
        nm_runtime_shutdown();
        return test_fail("nm runtime stats translation was unexpected");
    }
    memset(&prefix_stats, 0xA5, sizeof(prefix_stats));
    if (nm_runtime_collect_stats_ex(&prefix_stats, offsetof(nm_runtime_stats_t, active_nodes)) != 0) {
        nm_channel_destroy(state.channel);
        nm_runtime_shutdown();
        return test_fail_errno("nm_runtime_collect_stats_ex prefix failed");
    }
    if (prefix_stats.ctx_switches == 0U || prefix_stats.yields == 0U ||
        prefix_stats.active_nodes != 0xA5A5A5A5U) {
        nm_channel_destroy(state.channel);
        nm_runtime_shutdown();
        return test_fail("nm runtime stats prefix copy was not bounded");
    }
    if (nm_runtime_collect_stats(&stats) != 0) {
        nm_channel_destroy(state.channel);
        nm_runtime_shutdown();
        return test_fail_errno("nm_runtime_collect_stats wrapper failed");
    }
#if NM_PLATFORM_POSIX
    {
        int pipe_fds[2];
        char json[1024];
        ssize_t nread;

        if (pipe(pipe_fds) != 0) {
            nm_channel_destroy(state.channel);
            nm_runtime_shutdown();
            return test_fail_errno("pipe for nm stats json failed");
        }
        if (nm_runtime_write_stats_json(pipe_fds[1]) != 0) {
            int saved_errno = errno;

            close(pipe_fds[0]);
            close(pipe_fds[1]);
            nm_channel_destroy(state.channel);
            nm_runtime_shutdown();
            errno = saved_errno;
            return test_fail_errno("nm_runtime_write_stats_json failed");
        }
        close(pipe_fds[1]);
        nread = read(pipe_fds[0], json, sizeof(json) - 1U);
        close(pipe_fds[0]);
        if (nread <= 0) {
            nm_channel_destroy(state.channel);
            nm_runtime_shutdown();
            return test_fail("nm stats json read produced no data");
        }
        json[nread] = '\0';
        if (json[0] != '{' ||
            strstr(json, "\"ctx_switches\":") == NULL ||
            strstr(json, "\"active_workers\":") == NULL) {
            nm_channel_destroy(state.channel);
            nm_runtime_shutdown();
            return test_fail("nm stats json did not contain expected fields");
        }
    }
#endif

    nm_channel_destroy(state.channel);
    nm_runtime_shutdown();
    return 0;
}

int main(void) {
    if (test_nm_env_aliases() != 0 ||
        test_nm_cancel_token_contract() != 0 ||
        test_nm_runtime_wrappers() != 0) {
        return 1;
    }
    printf("[test_nm_compat_runtime] ok\n");
    return 0;
}
