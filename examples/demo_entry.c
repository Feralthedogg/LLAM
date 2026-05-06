/**
 * @file examples/demo_entry.c
 * @brief Demo driver that wires runtime setup, I/O fixtures, and example tasks together.
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

#include "demo_internal.h"

int main(void) {
    struct slow_job job = {
        .input = 12,
        .output = 0,
        .pause_ms = 40,
    };
    struct io_pair io_pair = {
        .reader_fd = -1,
        .writer_fd = -1,
    };
    struct io_pair owned_pair = {
        .reader_fd = -1,
        .writer_fd = -1,
    };
    struct io_pair peek_pair = {
        .reader_fd = -1,
        .writer_fd = -1,
    };
    struct poll_pair poll_pair = {
        .reader_fd = -1,
        .writer_fd = -1,
    };
    struct accept_state accept_state = {
        .listener_fd = -1,
        .port = 0,
    };
    struct timed_mutex_state timed_mutex_state = {0};
    struct timed_join_state timed_join_state = {0};
    struct connect_job connect_a = {
        .port = 0,
        .label = "A",
    };
    struct connect_job connect_b = {
        .port = 0,
        .label = "B",
    };
    struct cond_state cond_state = {0};
    struct channel_state channel_state = {0};
    struct channel_state timeout_recv_channel_state = {0};
    struct channel_state timeout_send_channel_state = {0};
    atomic_uint timeout_send_prefilled;
    struct cancel_state cancel_state = {0};
    struct io_cancel_state io_cancel_state = {
        .reader_fd = -1,
        .writer_fd = -1,
        .token = NULL,
    };
    struct block_cancel_state block_cancel_state = {0};
    llam_runtime_opts_t runtime_opts = {
        .deterministic = 0,
        .forced_yield_every = 0,
        .experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ,
        .sqpoll_cpu = -1,
    };
    unsigned worker_rings;
    unsigned worker_rings_multishot;
    unsigned dynamic_workers;
    unsigned lockfree_normq;
    unsigned huge_alloc;
    unsigned sqpoll;
    int sv[2];
    int owned_sv[2];
    int peek_sv[2];
    int poll_sv[2];
    int poll_timeout_sv[2];
    int io_cancel_sv[2];
    int listener_fd = -1;
    int one = 1;
    struct sockaddr_in listener_addr;
    socklen_t listener_len = sizeof(listener_addr);

    setvbuf(stdout, NULL, _IONBF, 0);
    atomic_init(&timeout_send_prefilled, 0U);
    worker_rings = demo_env_flag_default("LLAM_EXPERIMENTAL_WORKER_RINGS", 0U);
    worker_rings_multishot = demo_env_flag_default("LLAM_EXPERIMENTAL_WORKER_RINGS_MULTISHOT", 0U);
    dynamic_workers = demo_env_flag_default("LLAM_EXPERIMENTAL_DYNAMIC_WORKERS", 1U);
    lockfree_normq = demo_env_flag_default("LLAM_EXPERIMENTAL_LOCKFREE_NORMQ", 1U);
    huge_alloc = demo_env_flag_default("LLAM_EXPERIMENTAL_HUGE_ALLOC", 0U);
    sqpoll = demo_env_flag_default("LLAM_EXPERIMENTAL_SQPOLL", 0U);
    runtime_opts.experimental_flags = 0U;
    if (worker_rings != 0U) {
        runtime_opts.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS;
    }
    if (worker_rings_multishot != 0U) {
        runtime_opts.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS_MULTISHOT;
    }
    if (dynamic_workers != 0U) {
        runtime_opts.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS;
    }
    if (lockfree_normq != 0U) {
        runtime_opts.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    }
    if (huge_alloc != 0U) {
        runtime_opts.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_HUGE_ALLOC;
    }
    if (sqpoll != 0U) {
        runtime_opts.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL;
    }
    runtime_opts.sqpoll_cpu = demo_env_i32("LLAM_SQPOLL_CPU", -1, -1, 4096);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        return 1;
    }
    io_pair.reader_fd = sv[0];
    io_pair.writer_fd = sv[1];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, poll_sv) != 0) {
        perror("socketpair");
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    poll_pair.reader_fd = poll_sv[0];
    poll_pair.writer_fd = poll_sv[1];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, poll_timeout_sv) != 0) {
        perror("socketpair");
        close(sv[0]);
        close(sv[1]);
        close(poll_sv[0]);
        close(poll_sv[1]);
        return 1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, io_cancel_sv) != 0) {
        perror("socketpair");
        close(sv[0]);
        close(sv[1]);
        close(poll_sv[0]);
        close(poll_sv[1]);
        close(poll_timeout_sv[0]);
        close(poll_timeout_sv[1]);
        return 1;
    }
    io_cancel_state.reader_fd = io_cancel_sv[0];
    io_cancel_state.writer_fd = io_cancel_sv[1];
    listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_fd < 0) {
        perror("socket");
        close(sv[0]);
        close(sv[1]);
        close(poll_sv[0]);
        close(poll_sv[1]);
        close(poll_timeout_sv[0]);
        close(poll_timeout_sv[1]);
        close(io_cancel_sv[0]);
        close(io_cancel_sv[1]);
        return 1;
    }
    if (setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        perror("setsockopt");
        close(listener_fd);
        close(sv[0]);
        close(sv[1]);
        close(poll_sv[0]);
        close(poll_sv[1]);
        close(poll_timeout_sv[0]);
        close(poll_timeout_sv[1]);
        close(io_cancel_sv[0]);
        close(io_cancel_sv[1]);
        return 1;
    }
    memset(&listener_addr, 0, sizeof(listener_addr));
    listener_addr.sin_family = AF_INET;
    listener_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listener_addr.sin_port = htons(0);
    if (bind(listener_fd, (struct sockaddr *)&listener_addr, sizeof(listener_addr)) != 0) {
        perror("bind");
        close(listener_fd);
        close(sv[0]);
        close(sv[1]);
        close(poll_sv[0]);
        close(poll_sv[1]);
        close(poll_timeout_sv[0]);
        close(poll_timeout_sv[1]);
        close(io_cancel_sv[0]);
        close(io_cancel_sv[1]);
        return 1;
    }
    if (listen(listener_fd, 8) != 0) {
        perror("listen");
        close(listener_fd);
        close(sv[0]);
        close(sv[1]);
        close(poll_sv[0]);
        close(poll_sv[1]);
        close(poll_timeout_sv[0]);
        close(poll_timeout_sv[1]);
        close(io_cancel_sv[0]);
        close(io_cancel_sv[1]);
        return 1;
    }
    if (getsockname(listener_fd, (struct sockaddr *)&listener_addr, &listener_len) != 0) {
        perror("getsockname");
        close(listener_fd);
        close(sv[0]);
        close(sv[1]);
        close(poll_sv[0]);
        close(poll_sv[1]);
        close(poll_timeout_sv[0]);
        close(poll_timeout_sv[1]);
        close(io_cancel_sv[0]);
        close(io_cancel_sv[1]);
        return 1;
    }
    accept_state.listener_fd = listener_fd;
    accept_state.port = ntohs(listener_addr.sin_port);
    connect_a.port = accept_state.port;
    connect_b.port = accept_state.port;

    if (llam_runtime_init(&runtime_opts) != 0) {
        perror("llam_runtime_init");
        close(listener_fd);
        close(sv[0]);
        close(sv[1]);
        close(poll_sv[0]);
        close(poll_sv[1]);
        close(poll_timeout_sv[0]);
        close(poll_timeout_sv[1]);
        close(io_cancel_sv[0]);
        close(io_cancel_sv[1]);
        return 1;
    }

    cond_state.mutex = llam_mutex_create();
    cond_state.cond = llam_cond_create();
    timed_mutex_state.mutex = llam_mutex_create();
    channel_state.channel = llam_channel_create(2U);
    timeout_recv_channel_state.channel = llam_channel_create(1U);
    timeout_send_channel_state.channel = llam_channel_create(1U);
    timeout_send_channel_state.ready = &timeout_send_prefilled;
    cancel_state.token = llam_cancel_token_create();
    io_cancel_state.token = llam_cancel_token_create();
    block_cancel_state.token = llam_cancel_token_create();
    if (cond_state.mutex == NULL || cond_state.cond == NULL || timed_mutex_state.mutex == NULL || channel_state.channel == NULL ||
        timeout_recv_channel_state.channel == NULL || timeout_send_channel_state.channel == NULL || cancel_state.token == NULL ||
        io_cancel_state.token == NULL || block_cancel_state.token == NULL) {
        perror("wait-object-create");
        llam_runtime_shutdown();
        if (cancel_state.token != NULL) {
            (void)llam_cancel_token_destroy(cancel_state.token);
        }
        if (io_cancel_state.token != NULL) {
            (void)llam_cancel_token_destroy(io_cancel_state.token);
        }
        if (block_cancel_state.token != NULL) {
            (void)llam_cancel_token_destroy(block_cancel_state.token);
        }
        close(sv[0]);
        close(sv[1]);
        close(poll_sv[0]);
        close(poll_sv[1]);
        close(poll_timeout_sv[0]);
        close(poll_timeout_sv[1]);
        close(io_cancel_sv[0]);
        close(io_cancel_sv[1]);
        close(listener_fd);
        return 1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, owned_sv) != 0) {
        perror("socketpair");
        llam_channel_destroy(timeout_send_channel_state.channel);
        llam_channel_destroy(timeout_recv_channel_state.channel);
        llam_channel_destroy(channel_state.channel);
        llam_cond_destroy(cond_state.cond);
        llam_mutex_destroy(timed_mutex_state.mutex);
        llam_mutex_destroy(cond_state.mutex);
        llam_runtime_shutdown();
        (void)llam_cancel_token_destroy(cancel_state.token);
        (void)llam_cancel_token_destroy(io_cancel_state.token);
        (void)llam_cancel_token_destroy(block_cancel_state.token);
        close(listener_fd);
        close(io_pair.reader_fd);
        close(io_pair.writer_fd);
        close(poll_pair.reader_fd);
        close(poll_pair.writer_fd);
        close(poll_timeout_sv[0]);
        close(poll_timeout_sv[1]);
        close(io_cancel_state.reader_fd);
        close(io_cancel_state.writer_fd);
        return 1;
    }
    owned_pair.reader_fd = owned_sv[0];
    owned_pair.writer_fd = owned_sv[1];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, peek_sv) != 0) {
        perror("socketpair");
        llam_channel_destroy(timeout_send_channel_state.channel);
        llam_channel_destroy(timeout_recv_channel_state.channel);
        llam_channel_destroy(channel_state.channel);
        llam_cond_destroy(cond_state.cond);
        llam_mutex_destroy(timed_mutex_state.mutex);
        llam_mutex_destroy(cond_state.mutex);
        llam_runtime_shutdown();
        (void)llam_cancel_token_destroy(cancel_state.token);
        (void)llam_cancel_token_destroy(io_cancel_state.token);
        (void)llam_cancel_token_destroy(block_cancel_state.token);
        close(listener_fd);
        close(io_pair.reader_fd);
        close(io_pair.writer_fd);
        close(owned_pair.reader_fd);
        close(owned_pair.writer_fd);
        close(poll_pair.reader_fd);
        close(poll_pair.writer_fd);
        close(poll_timeout_sv[0]);
        close(poll_timeout_sv[1]);
        close(io_cancel_state.reader_fd);
        close(io_cancel_state.writer_fd);
        return 1;
    }
    peek_pair.reader_fd = peek_sv[0];
    peek_pair.writer_fd = peek_sv[1];

    g_worker_task = llam_spawn(counter_task,
                             "worker-A",
                             &(llam_spawn_opts_t){
                                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
                                 .flags = LLAM_SPAWN_F_PINNED,
                             });
    llam_spawn(counter_task,
             "worker-B",
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_BATCH,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(fp_isolation_task,
             NULL,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(sleeper_task,
             NULL,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_LATENCY,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(blocking_task,
             &job,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_LARGE,
             });
    llam_spawn(joiner_task,
             NULL,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_LATENCY,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    timed_join_state.target = llam_spawn(timed_join_target_task,
                                       NULL,
                                       &(llam_spawn_opts_t){
                                           .task_class = LLAM_TASK_CLASS_DEFAULT,
                                           .stack_class = LLAM_STACK_CLASS_DEFAULT,
                                       });
    llam_spawn(timed_join_waiter_task,
             &timed_join_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_LATENCY,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(io_reader_task,
             &io_pair,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_LATENCY,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(io_writer_task,
             &io_pair,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(io_owned_reader_task,
             &owned_pair,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_LATENCY,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(io_owned_writer_task,
             &owned_pair,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(io_owned_peek_task,
             &peek_pair,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_LATENCY,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(io_peek_writer_task,
             &peek_pair,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(poll_waiter_task,
             &poll_pair,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_LATENCY,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(poll_writer_task,
             &poll_pair,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(poll_timeout_task,
             &poll_timeout_sv[0],
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(accept_waiter_task,
             &accept_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_LATENCY,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(accept_waiter_task,
             &accept_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_LATENCY,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(accept_connector_task,
             &connect_a,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(accept_connector_task,
             &connect_b,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(cond_waiter_task,
             &cond_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_LATENCY,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(cond_timeout_task,
             &cond_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(cond_signaler_task,
             &cond_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(channel_recv_task,
             &channel_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_LATENCY,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(channel_send_task,
             &channel_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(channel_timeout_recv_task,
             &timeout_recv_channel_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(channel_timeout_fill_task,
             &timeout_send_channel_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_LATENCY,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(channel_timeout_send_task,
             &timeout_send_channel_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(cancel_waiter_task,
             NULL,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
                 .cancel_token = cancel_state.token,
             });
    llam_spawn(cancel_trigger_task,
             &cancel_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(io_cancel_waiter_task,
             &io_cancel_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
                 .cancel_token = io_cancel_state.token,
             });
    llam_spawn(io_cancel_trigger_task,
             &io_cancel_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(block_cancel_waiter_task,
             NULL,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
                 .cancel_token = block_cancel_state.token,
             });
    llam_spawn(block_cancel_trigger_task,
             &block_cancel_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(opaque_block_task,
             NULL,
             &(llam_spawn_opts_t){
                .task_class = LLAM_TASK_CLASS_DEFAULT,
                .stack_class = LLAM_STACK_CLASS_DEFAULT,
                 .flags = LLAM_SPAWN_F_SYS_TASK | LLAM_SPAWN_F_PINNED,
             });
    llam_spawn(mutex_holder_task,
             &timed_mutex_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_DEFAULT,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });
    llam_spawn(mutex_timeout_task,
             &timed_mutex_state,
             &(llam_spawn_opts_t){
                 .task_class = LLAM_TASK_CLASS_LATENCY,
                 .stack_class = LLAM_STACK_CLASS_DEFAULT,
             });

    if (llam_run() != 0) {
        perror("llam_run");
        llam_dump_runtime_state(STDOUT_FILENO);
        llam_channel_destroy(timeout_send_channel_state.channel);
        llam_channel_destroy(timeout_recv_channel_state.channel);
        llam_channel_destroy(channel_state.channel);
        llam_cond_destroy(cond_state.cond);
        llam_mutex_destroy(timed_mutex_state.mutex);
        llam_mutex_destroy(cond_state.mutex);
        llam_runtime_shutdown();
        (void)llam_cancel_token_destroy(cancel_state.token);
        (void)llam_cancel_token_destroy(io_cancel_state.token);
        (void)llam_cancel_token_destroy(block_cancel_state.token);
        close(sv[0]);
        close(sv[1]);
        close(poll_sv[0]);
        close(poll_sv[1]);
        close(poll_timeout_sv[0]);
        close(poll_timeout_sv[1]);
        close(io_cancel_sv[0]);
        close(io_cancel_sv[1]);
        close(owned_pair.reader_fd);
        close(owned_pair.writer_fd);
        close(peek_pair.reader_fd);
        close(peek_pair.writer_fd);
        close(listener_fd);
        return 1;
    }

    llam_dump_runtime_state(STDOUT_FILENO);
    llam_channel_destroy(timeout_send_channel_state.channel);
    llam_channel_destroy(timeout_recv_channel_state.channel);
    llam_channel_destroy(channel_state.channel);
    llam_cond_destroy(cond_state.cond);
    llam_mutex_destroy(timed_mutex_state.mutex);
    llam_mutex_destroy(cond_state.mutex);
    llam_runtime_shutdown();
    if (llam_cancel_token_destroy(cancel_state.token) != 0) {
        perror("llam_cancel_token_destroy");
    }
    if (llam_cancel_token_destroy(io_cancel_state.token) != 0) {
        perror("llam_cancel_token_destroy(io)");
    }
    if (llam_cancel_token_destroy(block_cancel_state.token) != 0) {
        perror("llam_cancel_token_destroy(block)");
    }
    close(sv[0]);
    close(sv[1]);
    close(poll_sv[0]);
    close(poll_sv[1]);
    close(poll_timeout_sv[0]);
    close(poll_timeout_sv[1]);
    close(io_cancel_sv[0]);
    close(io_cancel_sv[1]);
    close(owned_pair.reader_fd);
    close(owned_pair.writer_fd);
    close(peek_pair.reader_fd);
    close(peek_pair.writer_fd);
    close(listener_fd);
    return 0;
}
