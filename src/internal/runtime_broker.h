/**
 * @file src/internal/runtime_broker.h
 * @brief Minimal secure-broker control-plane skeleton.
 *
 * @details
 * The broker owns a runtime, a capability key, and broker-side object storage
 * inside the trusted broker address space. This is the executable boundary for
 * protecting capability authority from an untrusted client process. Transport,
 * shared-memory rings, and descriptor/HANDLE routing are layered around this
 * control object.
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

#ifndef LLAM_RUNTIME_BROKER_H
#define LLAM_RUNTIME_BROKER_H

#include "runtime_capability.h"

#include <stdatomic.h>
#if LLAM_PLATFORM_WINDOWS
#include "runtime_windows_compat.h"
#else
#include <pthread.h>
#endif

#define LLAM_BROKER_WIRE_MAGIC UINT32_C(0x4c4c424b)
#define LLAM_BROKER_WIRE_VERSION 2U
#define LLAM_BROKER_CAP_FAMILY_BUFFER 8U
#define LLAM_BROKER_CAP_FAMILY_DESCRIPTOR 9U
#define LLAM_BROKER_CAP_FAMILY_CHANNEL 10U
#define LLAM_BROKER_CAP_FAMILY_TASK 11U
#define LLAM_BROKER_BUFFER_SLOTS 64U
#define LLAM_BROKER_DESCRIPTOR_SLOTS 64U
#define LLAM_BROKER_CHANNEL_SLOTS 64U
#define LLAM_BROKER_TASK_SLOTS 64U
#define LLAM_BROKER_RING_SESSIONS 16U
#define LLAM_BROKER_RING_MAPPING_NAME_BYTES 128U
#define LLAM_BROKER_TRANSPORT_SESSIONS 64U
#define LLAM_BROKER_BUFFER_MAX_BYTES (1024U * 1024U)
#define LLAM_BROKER_CHANNEL_CAPACITY 64U
#define LLAM_BROKER_CHANNEL_MESSAGE_BYTES 256U
#define LLAM_BROKER_WIRE_DATA_BYTES 256U
/*
 * Broker active_ops is a bounded lifecycle gate, not a request counter. The
 * high half is reserved as corrupted/exhausted state so destroy can fail closed
 * instead of waiting forever on a value no valid broker operation can drain.
 */
#define LLAM_BROKER_ACTIVE_OP_BUSY_SENTINEL (UINT32_MAX / 2U)
#define LLAM_BROKER_BUFFER_TRANSPORT_RIGHTS \
    (LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_WRITE | LLAM_CAP_RIGHT_DESTROY)
#define LLAM_BROKER_CHANNEL_TRANSPORT_RIGHTS \
    (LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_RECV | LLAM_CAP_RIGHT_CLOSE | LLAM_CAP_RIGHT_DESTROY)
#define LLAM_BROKER_DESCRIPTOR_TRANSPORT_RIGHTS \
    (LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_WRITE | LLAM_CAP_RIGHT_DESTROY)
#define LLAM_BROKER_TASK_TRANSPORT_RIGHTS (LLAM_CAP_RIGHT_JOIN | LLAM_CAP_RIGHT_DETACH)

typedef enum llam_broker_task_kind {
    LLAM_BROKER_TASK_KIND_RETURN_U64 = 1,
    LLAM_BROKER_TASK_KIND_INCREMENT_U64 = 2,
    LLAM_BROKER_TASK_KIND_POPCOUNT_U64 = 3,
    LLAM_BROKER_TASK_KIND_SLEEP_NS_RETURN_U64 = 4,
} llam_broker_task_kind_t;

typedef enum llam_broker_task_state {
    LLAM_BROKER_TASK_STATE_EMPTY = 0,
    LLAM_BROKER_TASK_STATE_SPAWNED = 1,
    LLAM_BROKER_TASK_STATE_COMPLETED = 2,
    LLAM_BROKER_TASK_STATE_JOINED = 3,
    LLAM_BROKER_TASK_STATE_DETACHED = 4,
} llam_broker_task_state_t;

typedef struct llam_broker_socket_identity {
    uint64_t dev;
    uint64_t ino;
} llam_broker_socket_identity_t;

typedef struct llam_broker_buffer_slot {
    unsigned char *data;
    size_t length;
    uint64_t id;
    uint64_t generation;
    uint64_t rights;
    bool active;
} llam_broker_buffer_slot_t;

typedef struct llam_broker_descriptor_slot {
#if LLAM_PLATFORM_WINDOWS
    llam_handle_t handle;
#else
    int fd;
#endif
    uint64_t id;
    uint64_t generation;
    uint64_t rights;
    bool active;
    bool close_on_destroy;
} llam_broker_descriptor_slot_t;

typedef struct llam_broker_channel_message {
    size_t length;
    unsigned char data[LLAM_BROKER_CHANNEL_MESSAGE_BYTES];
} llam_broker_channel_message_t;

typedef struct llam_broker_channel_slot {
    llam_broker_channel_message_t messages[LLAM_BROKER_CHANNEL_CAPACITY];
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    uint64_t id;
    uint64_t generation;
    uint64_t rights;
    bool active;
    bool closed;
} llam_broker_channel_slot_t;

typedef struct llam_broker_task_slot {
    struct llam_broker *owner;
    llam_task_t *task;
    _Atomic uint32_t state;
    uint32_t kind;
    uint64_t id;
    uint64_t generation;
    uint64_t rights;
    uint64_t arg0;
    uint64_t result0;
    int error_code;
    bool active;
} llam_broker_task_slot_t;

typedef struct llam_broker_ring_session {
    void *ring;
    uint64_t subject_id;
    uint64_t submit_head;
    uint64_t complete_tail;
    size_t mapping_bytes;
    int mapping_fd;
    llam_handle_t mapping_handle;
    char mapping_name[LLAM_BROKER_RING_MAPPING_NAME_BYTES];
    bool active;
    bool busy;
    bool owns_mapping;
} llam_broker_ring_session_t;

typedef struct llam_broker_transport_session {
    uintptr_t transport_id;
    uint64_t subject_id;
    bool active;
} llam_broker_transport_session_t;

typedef enum llam_broker_wire_op {
    LLAM_BROKER_WIRE_OP_PING = 1,
    /* Reserved for trusted tests; untrusted transports fail this with EACCES. */
    LLAM_BROKER_WIRE_OP_ISSUE_CAP = 2,
    LLAM_BROKER_WIRE_OP_VALIDATE_CAP = 3,
    LLAM_BROKER_WIRE_OP_REVOKE_ALL = 4,
    LLAM_BROKER_WIRE_OP_STOP = 5,
    LLAM_BROKER_WIRE_OP_ATTENUATE_CAP = 6,
    LLAM_BROKER_WIRE_OP_REVOKE_CAP = 7,
    LLAM_BROKER_WIRE_OP_CREATE_BUFFER = 8,
    LLAM_BROKER_WIRE_OP_CREATE_CHANNEL = 9,
    LLAM_BROKER_WIRE_OP_BUFFER_READ = 10,
    LLAM_BROKER_WIRE_OP_BUFFER_WRITE = 11,
    LLAM_BROKER_WIRE_OP_CHANNEL_SEND = 12,
    LLAM_BROKER_WIRE_OP_CHANNEL_RECV = 13,
    LLAM_BROKER_WIRE_OP_CHANNEL_CLOSE = 14,
    LLAM_BROKER_WIRE_OP_TASK_SPAWN = 15,
    LLAM_BROKER_WIRE_OP_TASK_JOIN = 16,
    LLAM_BROKER_WIRE_OP_TASK_DETACH = 17,
    LLAM_BROKER_WIRE_OP_DESCRIPTOR_READ = 18,
    LLAM_BROKER_WIRE_OP_DESCRIPTOR_WRITE = 19,
    LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR = 20,
    LLAM_BROKER_WIRE_OP_CREATE_RING = 21,
    LLAM_BROKER_WIRE_OP_SERVE_RING = 22,
} llam_broker_wire_op_t;

typedef struct llam_broker_wire_request {
    uint32_t magic;
    uint32_t version;
    uint32_t op;
    uint32_t reserved0;
    uint32_t family;
    uint32_t reserved1;
    uint64_t slot;
    uint64_t generation;
    uint64_t rights;
    uint64_t required_rights;
    uint64_t offset;
    uint64_t length;
    llam_capability_token_t token;
    unsigned char data[LLAM_BROKER_WIRE_DATA_BYTES];
} llam_broker_wire_request_t;

typedef struct llam_broker_wire_response {
    uint32_t magic;
    uint32_t version;
    int32_t status;
    int32_t error_code;
    uint64_t runtime_id;
    uint64_t revocation_epoch;
    uint64_t result0;
    uint64_t result1;
    uint64_t result2;
    llam_capability_token_t token;
    unsigned char data[LLAM_BROKER_WIRE_DATA_BYTES];
} llam_broker_wire_response_t;

typedef int (*llam_broker_wire_request_fn_t)(void *transport,
                                             const llam_broker_wire_request_t *request,
                                             llam_broker_wire_response_t *response);

typedef struct llam_broker {
    llam_runtime_t *runtime;
    pthread_mutex_t lock;
    pthread_cond_t idle_cond;
    llam_capability_key_t capability_key;
    atomic_uint_fast64_t revocation_epoch;
    llam_broker_buffer_slot_t buffers[LLAM_BROKER_BUFFER_SLOTS];
    llam_broker_descriptor_slot_t descriptors[LLAM_BROKER_DESCRIPTOR_SLOTS];
    llam_broker_channel_slot_t *channels;
    llam_broker_task_slot_t tasks[LLAM_BROKER_TASK_SLOTS];
    llam_broker_ring_session_t ring_sessions[LLAM_BROKER_RING_SESSIONS];
    llam_broker_transport_session_t transport_sessions[LLAM_BROKER_TRANSPORT_SESSIONS];
    uint64_t next_buffer_id;
    uint64_t next_descriptor_id;
    uint64_t next_channel_id;
    uint64_t next_task_id;
    uint64_t next_transport_subject_nonce;
    uint32_t active_ops;
    uint32_t destroy_waiters;
    bool initialized;
    bool destroying;
    bool lock_initialized;
    bool idle_cond_initialized;
} llam_broker_t;

void llam_broker_process_request(llam_broker_t *broker,
                                 const llam_broker_wire_request_t *request,
                                 llam_broker_wire_response_t *response,
                                 bool *out_should_close);
void llam_broker_process_request_with_descriptor(llam_broker_t *broker,
                                                 const llam_broker_wire_request_t *request,
                                                 llam_broker_wire_response_t *response,
                                                 bool *out_should_close,
                                                 llam_handle_t descriptor_handle);
void llam_broker_process_request_with_descriptors(llam_broker_t *broker,
                                                  const llam_broker_wire_request_t *request,
                                                  llam_broker_wire_response_t *response,
                                                  bool *out_should_close,
                                                  llam_handle_t descriptor_handle,
                                                  llam_handle_t *out_response_descriptor);
void llam_broker_mark_response_failure_clear_outputs(llam_broker_wire_response_t *response, int error_code);
void llam_broker_normalize_response_failure_outputs(llam_broker_wire_response_t *response);
int llam_broker_validate_response_frame_or_clear(llam_broker_wire_response_t *response);
void llam_broker_rollback_created_response(llam_broker_t *broker,
                                           const llam_broker_wire_request_t *request,
                                           const llam_broker_wire_response_t *response,
                                           uint64_t subject_id);

int llam_broker_init(llam_broker_t *broker, const llam_runtime_opts_t *opts, size_t opts_size);
void llam_broker_destroy(llam_broker_t *broker);
int llam_broker_begin_op(llam_broker_t *broker);
int llam_broker_begin_op_subject(llam_broker_t *broker, uint64_t subject_id);
void llam_broker_end_op(llam_broker_t *broker);
int llam_broker_lock(llam_broker_t *broker);
void llam_broker_unlock(llam_broker_t *broker);
uint64_t llam_broker_current_subject(const llam_broker_t *broker);
bool llam_broker_current_thread_has_op(const llam_broker_t *broker);

uint64_t llam_broker_revocation_epoch(const llam_broker_t *broker);
uint64_t llam_broker_revoke_all(llam_broker_t *broker);

int llam_broker_issue_object_cap(llam_broker_t *broker,
                                 uint32_t family,
                                 uint64_t slot,
                                 uint64_t generation,
                                 uint64_t rights,
                                 llam_capability_token_t *out_token);
int llam_broker_validate_object_rights(uint32_t family, uint64_t rights);
int llam_broker_validate_next_object_id(uint64_t next_id);
int llam_broker_issue_object_cap_unlocked(llam_broker_t *broker,
                                          uint32_t family,
                                          uint64_t slot,
                                          uint64_t generation,
                                          uint64_t rights,
                                          llam_capability_token_t *out_token);

int llam_broker_validate_cap(const llam_broker_t *broker,
                             const llam_capability_token_t *token,
                             uint64_t required_rights);
int llam_broker_validate_cap_unlocked(const llam_broker_t *broker,
                                      const llam_capability_token_t *token,
                                      uint64_t required_rights);
/**
 * @brief Validate a token MAC, rights mask, and object family before slot lookup.
 *
 * Typed broker object tables still perform their own live slot/generation
 * search, but this helper keeps the common preflight contract identical across
 * buffers, channels, descriptors, and predefined broker tasks.
 */
int llam_broker_validate_token_family_unlocked(const llam_broker_t *broker,
                                               const llam_capability_token_t *token,
                                               uint32_t expected_family,
                                               uint64_t required_rights);
int llam_broker_validate_live_object_unlocked(const llam_broker_t *broker,
                                             const llam_capability_token_t *token,
                                             uint64_t required_rights);
/**
 * @brief Clear caller-owned output before returning a broker read-style error.
 *
 * Broker data-plane reads must not leak stale caller buffer contents across
 * failed capability checks, revoked tokens, short wire reads, or unsupported
 * platform paths. These helpers centralize that contract so buffer, channel,
 * descriptor, and transport reads fail closed in the same way.
 */
int llam_broker_fail_clear_output(void *out_data, size_t length, int error_code);
ssize_t llam_broker_fail_clear_output_ssize(void *out_data, size_t length, int error_code);
ssize_t llam_broker_finish_read_clear_tail(void *out_data, size_t length, ssize_t result);
int llam_broker_attenuate_cap(const llam_broker_t *broker,
                              const llam_capability_token_t *token,
                              uint64_t subset_rights,
                              llam_capability_token_t *out_token);
int llam_broker_revoke_object_cap(llam_broker_t *broker,
                                  const llam_capability_token_t *token,
                                  uint64_t replacement_rights,
                                  llam_capability_token_t *out_token);

int llam_broker_register_buffer(llam_broker_t *broker,
                                const void *initial_data,
                                size_t length,
                                uint64_t rights,
                                llam_capability_token_t *out_token);
int llam_broker_read_buffer(llam_broker_t *broker,
                            const llam_capability_token_t *token,
                            uint64_t relative_offset,
                            void *out_data,
                            size_t length);
int llam_broker_write_buffer(llam_broker_t *broker,
                             const llam_capability_token_t *token,
                             uint64_t relative_offset,
                             const void *data,
                             size_t length);
void llam_broker_clear_buffers(llam_broker_t *broker);
void llam_broker_buffer_slot_reset(llam_broker_buffer_slot_t *slot);
llam_broker_buffer_slot_t *llam_broker_find_buffer_unlocked(llam_broker_t *broker,
                                                            const llam_capability_token_t *token,
                                                            uint64_t required_rights);
int llam_broker_register_fd(llam_broker_t *broker,
                            int fd,
                            uint64_t rights,
                            bool close_on_destroy,
                            llam_capability_token_t *out_token);
int llam_broker_register_handle(llam_broker_t *broker,
                                llam_handle_t handle,
                                uint64_t rights,
                                bool close_on_destroy,
                                llam_capability_token_t *out_token);
void llam_broker_clear_descriptors(llam_broker_t *broker);
ssize_t llam_broker_read_fd(llam_broker_t *broker,
                            const llam_capability_token_t *token,
                            void *out_data,
                            size_t length);
ssize_t llam_broker_write_fd(llam_broker_t *broker,
                             const llam_capability_token_t *token,
                             const void *data,
                             size_t length);
ssize_t llam_broker_read_handle(llam_broker_t *broker,
                                const llam_capability_token_t *token,
                                void *out_data,
                                size_t length);
ssize_t llam_broker_write_handle(llam_broker_t *broker,
                                 const llam_capability_token_t *token,
                                 const void *data,
                                 size_t length);
int llam_broker_create_channel(llam_broker_t *broker,
                               size_t capacity,
                               uint64_t rights,
                               llam_capability_token_t *out_token);
int llam_broker_channel_send(llam_broker_t *broker,
                             const llam_capability_token_t *token,
                             const void *data,
                             size_t length);
ssize_t llam_broker_channel_recv(llam_broker_t *broker,
                                 const llam_capability_token_t *token,
                                 void *out_data,
                                 size_t capacity);
int llam_broker_channel_close(llam_broker_t *broker, const llam_capability_token_t *token);
void llam_broker_clear_channels(llam_broker_t *broker);
llam_broker_channel_slot_t *llam_broker_find_channel_unlocked(llam_broker_t *broker,
                                                              const llam_capability_token_t *token,
                                                              uint64_t required_rights);
int llam_broker_spawn_task(llam_broker_t *broker,
                           uint32_t kind,
                           uint64_t arg0,
                           uint64_t rights,
                           llam_capability_token_t *out_token);
int llam_broker_task_join_runtime_drive_allowed(llam_broker_t *broker,
                                                const llam_capability_token_t *token,
                                                bool *out_allowed);
int llam_broker_join_task(llam_broker_t *broker,
                          const llam_capability_token_t *token,
                          uint64_t *out_result0);
int llam_broker_detach_task(llam_broker_t *broker, const llam_capability_token_t *token);
void llam_broker_clear_tasks(llam_broker_t *broker);

int llam_broker_serve_one_fd(llam_broker_t *broker, int fd, bool *out_should_close);
int llam_broker_serve_fd(llam_broker_t *broker, int fd);
int llam_broker_request_fd(int fd,
                           const llam_broker_wire_request_t *request,
                           llam_broker_wire_response_t *response);
int llam_broker_request_fd_with_descriptor(int fd,
                                           const llam_broker_wire_request_t *request,
                                           int descriptor_fd,
                                           llam_broker_wire_response_t *response);
int llam_broker_request_fd_with_response_descriptor(int fd,
                                                    const llam_broker_wire_request_t *request,
                                                    llam_broker_wire_response_t *response,
                                                    int *out_descriptor_fd);
int llam_broker_read_request_fd(int fd,
                                llam_broker_wire_request_t *request,
                                int *out_descriptor_fd);
int llam_broker_read_response_fd(int fd,
                                 llam_broker_wire_response_t *response,
                                 int *out_descriptor_fd);
int llam_broker_set_cloexec_fd(int fd);
int llam_broker_dup_cloexec_fd(int fd);
int llam_broker_write_request_with_descriptor(int fd,
                                              const llam_broker_wire_request_t *request,
                                              int descriptor_fd);
int llam_broker_write_response_with_descriptor(int fd,
                                               const llam_broker_wire_response_t *response,
                                               int descriptor_fd);
int llam_broker_write_response_fd(int fd, const llam_broker_wire_response_t *response);
int llam_broker_serve_one_handle(llam_broker_t *broker, llam_handle_t handle, bool *out_should_close);
int llam_broker_serve_handle(llam_broker_t *broker, llam_handle_t handle);
int llam_broker_request_handle(llam_handle_t handle,
                               const llam_broker_wire_request_t *request,
                               llam_broker_wire_response_t *response);
int llam_broker_request_handle_with_descriptor(llam_handle_t handle,
                                               const llam_broker_wire_request_t *request,
                                               llam_handle_t descriptor_handle,
                                               llam_broker_wire_response_t *response);
int llam_broker_transport_create_ring(llam_broker_t *broker,
                                      llam_handle_t *out_descriptor,
                                      uint64_t *out_session_id);
int llam_broker_client_self_test_exchange(llam_broker_wire_request_fn_t request_fn, void *transport);
int llam_broker_listen_unix(const char *path, int *out_fd);
int llam_broker_connect_unix(const char *path, int *out_fd);
int llam_broker_accept_one(int listen_fd, int *out_fd);
void llam_broker_close_fd(int fd);
int llam_broker_capture_owned_socket(const char *path, llam_broker_socket_identity_t *out_identity);
int llam_broker_restrict_owned_socket(const char *path, const llam_broker_socket_identity_t *identity);
void llam_broker_unlink_owned_socket(const char *path, const llam_broker_socket_identity_t *identity);
int llam_broker_listen_pipe(const char *name, llam_handle_t *out_handle);
int llam_broker_listen_pipe_instance(const char *name, bool first_instance, llam_handle_t *out_handle);
int llam_broker_connect_pipe(const char *name, llam_handle_t *out_handle);
void llam_broker_close_handle(llam_handle_t handle);
int llam_broker_windows_pipe_errno(unsigned long error_code);
int llam_broker_serve_unix_once(llam_broker_t *broker, const char *path);
int llam_broker_client_self_test_unix(const char *path);
int llam_broker_serve_local_once(llam_broker_t *broker, const char *path);
int llam_broker_serve_local_n(llam_broker_t *broker, const char *path, size_t max_connections);
int llam_broker_serve_local(llam_broker_t *broker, const char *path);
int llam_broker_client_self_test_local(const char *path);
int llam_broker_transport_subject(llam_broker_t *broker,
                                  uintptr_t transport_id,
                                  uint64_t *out_subject_id);
void llam_broker_forget_transport_subject(llam_broker_t *broker, uintptr_t transport_id);
#if defined(LLAM_ENABLE_TEST_HOOKS)
LLAM_INTERNAL_API uint64_t llam_broker_test_buffer_free_count(void);
LLAM_INTERNAL_API void llam_broker_test_buffer_free_count_reset(void);
LLAM_INTERNAL_API void llam_broker_test_force_subject_entropy_failure(bool enabled);
LLAM_INTERNAL_API void llam_broker_test_force_subject_value(bool enabled, uint64_t subject_id);
#endif

#endif
