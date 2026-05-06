/**
 * @file examples/stress_internal.h
 * @brief Shared stress-test state, declarations, and helper contracts.
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

#ifndef LLAM_EXAMPLES_STRESS_INTERNAL_H
#define LLAM_EXAMPLES_STRESS_INTERNAL_H

#include "llam/nm_runtime.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fenv.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "runtime_internal.h"

#include "env_compat.h"

typedef struct storm_state {
    atomic_uint completed;
    unsigned yields_per_task;
} storm_state_t;

typedef struct ping_state {
    nm_channel_t *request;
    nm_channel_t *response;
    unsigned rounds;
    atomic_uint echoed;
} ping_state_t;

typedef struct convoy_state {
    nm_mutex_t *mutex;
    unsigned iterations;
    unsigned workers;
    unsigned counter;
} convoy_state_t;

typedef struct cancel_state {
    nm_cancel_token_t *token;
    atomic_uint cancelled;
    atomic_uint triggered;
} cancel_state_t;

typedef struct opaque_state {
    atomic_uint companion_steps;
    unsigned scopes;
} opaque_state_t;

typedef struct cond_cancel_state {
    nm_mutex_t *mutex;
    nm_cond_t *cond;
    nm_cancel_token_t *token;
    atomic_uint cancelled;
    atomic_uint triggered;
    atomic_uint reacquired;
} cond_cancel_state_t;

typedef struct io_cancel_state {
    int reader_fd;
    int writer_fd;
    nm_cancel_token_t *token;
    atomic_uint cancelled;
    atomic_uint triggered;
} io_cancel_state_t;

typedef struct block_cancel_state {
    nm_cancel_token_t *token;
    atomic_uint cancelled;
    atomic_uint triggered;
} block_cancel_state_t;

typedef struct mutex_timeout_state {
    nm_mutex_t *mutex;
    uint64_t hold_ns;
} mutex_timeout_state_t;

typedef struct fp_round_state {
    unsigned mode;
    unsigned yields;
    atomic_uint completed;
} fp_round_state_t;

typedef struct fp_inherit_state {
    unsigned expected_mode;
    atomic_uint observed_mode;
    atomic_uint completed;
} fp_inherit_state_t;

typedef struct poll_cancel_race_state {
    int fd;
    nm_cancel_token_t *token;
    atomic_uint cancel_hits;
    atomic_uint timeout_hits;
    atomic_uint triggered;
} poll_cancel_race_state_t;

typedef struct nested_opaque_state {
    atomic_uint companion_steps;
    atomic_uint completed_scopes;
    unsigned scopes;
} nested_opaque_state_t;

typedef struct owned_read_state {
    int fd;
    const char *payload;
    size_t len;
    uint64_t delay_ns;
} owned_read_state_t;

typedef struct dynamic_sleep_wave_state {
    atomic_uint completed;
    unsigned base_yields;
    uint64_t sleep_ns;
} dynamic_sleep_wave_state_t;

typedef struct dynamic_sleep_child_state {
    dynamic_sleep_wave_state_t *wave;
    unsigned extra_yields;
} dynamic_sleep_child_state_t;

typedef struct dynamic_poll_writer_state {
    int fd;
    uint64_t delay_ns;
} dynamic_poll_writer_state_t;

typedef struct dynamic_suite_state {
    unsigned round_count;
    unsigned sleep_tasks;
    unsigned sleep_yields;
    uint64_t sleep_ns;
} dynamic_suite_state_t;

typedef struct dynamic_idle_poll_watch_state {
    unsigned sleep_tasks;
    unsigned sleep_yields;
    uint64_t sleep_ns;
    int sv[2];
} dynamic_idle_poll_watch_state_t;

typedef struct dynamic_idle_recv_watch_state {
    unsigned sleep_tasks;
    unsigned sleep_yields;
    uint64_t sleep_ns;
    int sv[2];
} dynamic_idle_recv_watch_state_t;

typedef struct dynamic_idle_accept_watch_state {
    unsigned sleep_tasks;
    unsigned sleep_yields;
    uint64_t sleep_ns;
    int listener_fd;
    uint16_t port;
} dynamic_idle_accept_watch_state_t;

typedef struct dynamic_live_poll_waiter_state {
    int fd;
    atomic_uint *completed;
} dynamic_live_poll_waiter_state_t;

typedef struct dynamic_live_inflight_waiter_state {
    int fd;
    atomic_uint *completed;
} dynamic_live_inflight_waiter_state_t;

typedef struct dynamic_live_accept_waiter_state {
    int listener_fd;
    atomic_uint *completed;
} dynamic_live_accept_waiter_state_t;

typedef struct dynamic_live_poll_watch_state {
    unsigned waiter_count;
    unsigned sleep_tasks;
    unsigned sleep_yields;
    uint64_t sleep_ns;
    unsigned monitor_rounds;
    uint64_t monitor_sleep_ns;
    int sv[2];
} dynamic_live_poll_watch_state_t;

typedef struct dynamic_live_inflight_io_state {
    unsigned waiter_count;
    unsigned sleep_tasks;
    unsigned sleep_yields;
    uint64_t sleep_ns;
    unsigned monitor_rounds;
    uint64_t monitor_sleep_ns;
} dynamic_live_inflight_io_state_t;

typedef struct dynamic_live_accept_watch_state {
    unsigned waiter_count;
    unsigned sleep_tasks;
    unsigned sleep_yields;
    uint64_t sleep_ns;
    unsigned monitor_rounds;
    uint64_t monitor_sleep_ns;
    int listener_fd;
    uint16_t port;
} dynamic_live_accept_watch_state_t;

typedef struct dynamic_foreign_poll_watch_state {
    unsigned waiter_count;
    unsigned sleep_tasks;
    unsigned sleep_yields;
    uint64_t sleep_ns;
    unsigned monitor_rounds;
    uint64_t monitor_sleep_ns;
    dynamic_live_poll_waiter_state_t *waiter_states;
    nm_task_t **waiters;
    int sv[2];
    atomic_uint completed;
    atomic_uint spawned;
    atomic_uint setup_done;
    atomic_uint setup_failed;
    atomic_uint observed_foreign_owner_waiters;
    atomic_uint skipped;
    atomic_uint source_shard;
    atomic_uint source_node;
    atomic_uint owner_node;
} dynamic_foreign_poll_watch_state_t;

typedef struct dynamic_accept_connector_state {
    uint16_t port;
    uint64_t delay_ns;
    atomic_uint *completed;
    int result;
} dynamic_accept_connector_state_t;

typedef struct stress_phase_entry {
    nm_task_fn task_fn;
    void *arg;
} stress_phase_entry_t;

extern atomic_uint g_failures;

void stress_fail_msg(const char *label);
void stress_fail_u32(const char *label, unsigned got, unsigned expected);
void stress_fail_errno(const char *label, int got, int expected);
void stress_close_fd_pair(int sv[2]);
void stress_reset_dynamic_foreign_poll_state(dynamic_foreign_poll_watch_state_t *state);
void stress_cleanup_dynamic_foreign_poll_state(dynamic_foreign_poll_watch_state_t *state);
nm_task_t *stress_spawn_on_shard(unsigned shard_id, nm_task_fn fn, void *arg, const nm_spawn_opts_t *opts);
bool stress_open_foreign_poll_pair(unsigned source_node_index, int sv_out[2], unsigned *owner_node_out);
bool stress_poll_watch_waiter_counts(unsigned node_index,
                                     int fd,
                                     short events,
                                     unsigned source_shard,
                                     unsigned *total_waiters_out,
                                     unsigned *source_owned_waiters_out);
int stress_create_loopback_listener(uint16_t *out_port);
unsigned stress_round_count(void);
unsigned stress_env_u32(const char *name, unsigned default_value, unsigned max_value);
unsigned stress_env_flag_default(const char *name, unsigned default_value);
int stress_env_i32(const char *name, int default_value, int min_value, int max_value);
bool stress_platform_prefers_indefinite_ready_poll(void);
bool stress_platform_supports_foreign_poll_watch(void);
bool stress_platform_supports_recv_watch(void);
bool stress_platform_supports_nested_opaque(void);
bool stress_platform_supports_owned_buffer_stress(void);
bool stress_platform_supports_basic_poll_stress(void);
bool stress_platform_supports_poll_watch_stress(void);
bool stress_platform_supports_io_cancel_stress(void);
bool stress_platform_supports_inflight_io_stress(void);
bool stress_platform_supports_seqpacket_socketpair(void);
bool stress_runtime_supports_multishot_accept(void);
bool stress_runtime_supports_multishot_poll(void);
bool stress_runtime_supports_multishot_recv(void);
bool stress_runtime_supports_async_read(void);
void stress_print_phase_skipped(const char *phase_name, const char *reason);
void stress_trace_step(const char *name);
void stress_set_fp_round(unsigned mode);
unsigned stress_fp_round_mode(void);

void storm_child_task(void *arg);
void ping_peer_task(void *arg);
void convoy_worker_task(void *arg);
void cancel_waiter_task(void *arg);
void cancel_trigger_task(void *arg);
void io_cancel_waiter_task(void *arg);
void io_cancel_trigger_task(void *arg);
void owned_read_writer_task(void *arg);
void dynamic_sleep_child_task(void *arg);
void *stress_blocking_pause(void *arg);
int stress_connect_loopback(dynamic_accept_connector_state_t *state);
void block_cancel_waiter_task(void *arg);
void block_cancel_trigger_task(void *arg);
void stress_sleep_task(void *arg);
void mutex_holder_task(void *arg);
void cond_cancel_waiter_task(void *arg);
void cond_cancel_trigger_task(void *arg);
void poll_writer_task(void *arg);
void dynamic_poll_writer_task(void *arg);
unsigned stress_dynamic_live_wait_floor(const nm_runtime_stats_t *stats);
void dynamic_live_poll_waiter_task(void *arg);
void dynamic_live_inflight_waiter_task(void *arg);
void dynamic_live_accept_waiter_task(void *arg);
void dynamic_accept_connector_task(void *arg);
void fp_round_task(void *arg);
void fp_inherit_child_task(void *arg);
void poll_cancel_race_waiter_task(void *arg);
void poll_cancel_race_trigger_task(void *arg);
void opaque_companion_task(void *arg);
void nested_opaque_companion_task(void *arg);
void opaque_scope_task(void *arg);
void nested_opaque_scope_task(void *arg);

void run_spawn_join_storm(void);
void run_channel_ping_pong(void);
void run_mutex_convoy(void);
void run_cancel_path(void);
void run_io_cancel_path(void);
void run_owned_read_paths(void);
void run_dynamic_sleep_fanout(unsigned task_count, unsigned base_yields, uint64_t sleep_ns);
void run_recv_owned_peek_path(void);
void run_recv_owned_multishot_path(void);
void run_block_cancel_path(void);
void run_opaque_reuse(void);
void run_join_timeout_path(void);
void run_mutex_timeout_path(void);
void run_cond_cancel_path(void);
void run_channel_timeout_paths(void);
void run_dynamic_join_timeout_path(void);
void run_dynamic_mutex_timeout_path(void);
void run_dynamic_cond_timeout_path(void);
void run_dynamic_channel_timeout_paths(void);
void run_dynamic_poll_paths(void);
void dynamic_idle_poll_watch_task(void *arg);
void dynamic_idle_recv_watch_task(void *arg);
void dynamic_idle_accept_watch_task(void *arg);
void dynamic_live_poll_watch_task(void *arg);
void dynamic_foreign_poll_watch_setup_task(void *arg);
void dynamic_foreign_poll_watch_scale_task(void *arg);
void dynamic_foreign_poll_watch_monitor_task(void *arg);
void dynamic_live_accept_watch_task(void *arg);
void dynamic_live_inflight_io_task(void *arg);
void run_poll_paths(void);
void run_fp_isolation_path(void);
void run_fp_inherit_path(void);
void run_poll_cancel_timeout_race(void);
void run_nested_opaque_path(void);
void stress_suite_task(void *arg);
void dynamic_suite_task(void *arg);
void stress_print_phase_stats(const char *phase_name, const nm_runtime_stats_t *stats);
int stress_run_phase(const char *phase_name,
                     nm_task_fn task_fn,
                     void *arg,
                     const nm_runtime_opts_t *opts,
                     unsigned require_dynamic_motion,
                     unsigned require_floor_reach);
int stress_run_multi_phase(const char *phase_name,
                           const stress_phase_entry_t *entries,
                           unsigned entry_count,
                           unsigned spawn_start_shard,
                           unsigned override_online_floor,
                           const nm_runtime_opts_t *opts,
                           unsigned require_dynamic_motion,
                           unsigned require_floor_reach);

#endif
