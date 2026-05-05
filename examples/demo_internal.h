/**
 * @file examples/demo_internal.h
 * @brief Shared declarations and state objects for the demo program.
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

#ifndef LLAM_EXAMPLES_DEMO_INTERNAL_H
#define LLAM_EXAMPLES_DEMO_INTERNAL_H

#include "llam/runtime.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fenv.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "env_compat.h"

struct slow_job {
    int input;
    int output;
    unsigned pause_ms;
};

struct io_pair {
    int reader_fd;
    int writer_fd;
};

struct poll_pair {
    int reader_fd;
    int writer_fd;
};

struct cond_state {
    llam_mutex_t *mutex;
    llam_cond_t *cond;
    int ready;
};

struct channel_state {
    llam_channel_t *channel;
    atomic_uint *ready;
};

struct cancel_state {
    llam_cancel_token_t *token;
};

struct io_cancel_state {
    int reader_fd;
    int writer_fd;
    llam_cancel_token_t *token;
};

struct block_cancel_state {
    llam_cancel_token_t *token;
};

struct timed_mutex_state {
    llam_mutex_t *mutex;
};

struct timed_join_state {
    llam_task_t *target;
};

struct accept_state {
    int listener_fd;
    uint16_t port;
};

struct connect_job {
    uint16_t port;
    const char *label;
};

struct fp_round_job {
    const char *label;
    unsigned mode;
};

extern llam_task_t *g_worker_task;

unsigned demo_env_flag_default(const char *name, unsigned default_value);
int demo_env_i32(const char *name, int default_value, int min_value, int max_value);

void counter_task(void *arg);
void fp_isolation_task(void *arg);
void sleeper_task(void *arg);
void blocking_task(void *arg);
void joiner_task(void *arg);
void timed_join_target_task(void *arg);
void timed_join_waiter_task(void *arg);
void io_reader_task(void *arg);
void io_owned_reader_task(void *arg);
void io_owned_peek_task(void *arg);
void io_writer_task(void *arg);
void io_owned_writer_task(void *arg);
void io_peek_writer_task(void *arg);
void poll_waiter_task(void *arg);
void poll_writer_task(void *arg);
void poll_timeout_task(void *arg);
void accept_waiter_task(void *arg);
void accept_connector_task(void *arg);
void cond_waiter_task(void *arg);
void cond_timeout_task(void *arg);
void cond_signaler_task(void *arg);
void channel_recv_task(void *arg);
void channel_send_task(void *arg);
void channel_timeout_recv_task(void *arg);
void channel_timeout_fill_task(void *arg);
void channel_timeout_send_task(void *arg);
void cancel_waiter_task(void *arg);
void cancel_trigger_task(void *arg);
void io_cancel_waiter_task(void *arg);
void io_cancel_trigger_task(void *arg);
void block_cancel_waiter_task(void *arg);
void block_cancel_trigger_task(void *arg);
void mutex_holder_task(void *arg);
void mutex_timeout_task(void *arg);
void opaque_block_task(void *arg);

#endif
