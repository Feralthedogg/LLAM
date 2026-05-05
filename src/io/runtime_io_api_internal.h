/**
 * @file src/io/runtime_io_api_internal.h
 * @brief Internal declarations shared by public I/O APIs and backend submission paths.
 *
 * @details
 * Public I/O calls pass through a layered path: direct nonblocking attempts,
 * cooperative/backend parking, multishot watch setup, and finally blocking
 * fallbacks when the backend cannot handle an operation. This header exposes
 * only the private hooks needed across those layers.
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

#ifndef NM_RUNTIME_IO_API_INTERNAL_H
#define NM_RUNTIME_IO_API_INTERNAL_H

#include "runtime_internal.h"

#if defined(__APPLE__)
#include <sys/event.h>
#endif

/* Request ownership helpers used by public I/O entry points. */
nm_io_req_t *nm_api_io_req_acquire(nm_shard_t *shard);
void nm_api_io_req_release(nm_shard_t *shard, nm_io_req_t *req);

/* Direct syscall and poll probes used before parking a task. */
int nm_platform_poll_fd(int fd, short events, int timeout_ms, short *revents);
int nm_platform_poll_now(int fd, short events, short *revents);
int nm_try_direct_rw(int fd,
                     void *buf,
                     size_t count,
                     bool is_write,
                     bool socket_recv,
                     int recv_flags,
                     ssize_t *result_out,
                     bool *socket_out);
bool nm_io_coop_yield_enabled(void);
bool nm_io_poll_coop_yield_enabled(void);
bool nm_io_poll_extra_yield_enabled(void);
bool nm_io_shard_has_local_work(void);
void nm_maybe_handoff_after_socket_write(int fd, size_t count, bool known_socket);
int nm_try_direct_blocking_rw(int fd,
                              void *buf,
                              size_t count,
                              bool is_write,
                              bool socket_recv,
                              int recv_flags,
                              ssize_t *result_out);
int nm_try_socket_pollin_now(int fd, short events, short *revents);
int nm_try_direct_blocking_poll(int fd, short events, int timeout_ms, short *revents);

/* Blocking fallback callbacks executed on the runtime blocking pool. */
void *nm_blocking_read_impl(void *arg);
void *nm_blocking_write_impl(void *arg);
void *nm_blocking_accept_impl(void *arg);
void *nm_blocking_poll_impl(void *arg);
ssize_t nm_read_owned_impl(int fd,
                           size_t max_count,
                           int recv_flags,
                           bool socket_recv,
                           nm_io_buffer_t **out);

/* Cooperative I/O parking and backend issue paths. */
void nm_cleanup_io_wait_setup(nm_task_t *task, nm_io_req_t *req);
int nm_park_io_req(nm_io_req_t *req, bool has_deadline, uint64_t deadline_ns, nm_node_t *wake_node);
int nm_issue_multishot_poll(nm_io_req_t *req);
int nm_issue_multishot_accept(nm_io_req_t *req);
int nm_issue_multishot_recv(nm_io_req_t *req);
int nm_issue_io(nm_io_req_t *req, bool has_deadline, uint64_t deadline_ns);
bool nm_drop_node_control_locked(nm_node_t *node, nm_io_control_kind_t kind, const void *target);

#endif
