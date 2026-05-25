/**
 * @file src/internal/runtime_broker_ring.h
 * @brief Shared-memory-ready broker submission/completion rings.
 *
 * @details
 * This header defines the fixed layout that can later be placed in a POSIX
 * shm/mmap region or Windows file mapping. The first implementation is a
 * single-producer/single-consumer ring because it matches one client transport
 * connection to one broker worker and keeps synchronization cheap.
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

#ifndef LLAM_RUNTIME_BROKER_RING_H
#define LLAM_RUNTIME_BROKER_RING_H

#include "runtime_broker.h"

#include <stdatomic.h>

#define LLAM_BROKER_RING_MAGIC UINT32_C(0x4c4c5251)
#define LLAM_BROKER_RING_VERSION 2U
#define LLAM_BROKER_RING_CAP 256U
#define LLAM_BROKER_RING_MASK (LLAM_BROKER_RING_CAP - 1U)
#define LLAM_BROKER_RING_DATA_BYTES 4096U
#define LLAM_BROKER_RING_CACHELINE 64U

typedef enum llam_broker_ring_op {
    LLAM_BROKER_RING_OP_NOP = 0,
    LLAM_BROKER_RING_OP_CAP_VALIDATE = 1,
    LLAM_BROKER_RING_OP_BUFFER_READ = 2,
    LLAM_BROKER_RING_OP_BUFFER_WRITE = 3,
    LLAM_BROKER_RING_OP_DESCRIPTOR_READ = 4,
    LLAM_BROKER_RING_OP_DESCRIPTOR_WRITE = 5,
    LLAM_BROKER_RING_OP_CHANNEL_SEND = 6,
    LLAM_BROKER_RING_OP_CHANNEL_RECV = 7,
    LLAM_BROKER_RING_OP_CHANNEL_CLOSE = 8,
    LLAM_BROKER_RING_OP_TASK_SPAWN = 9,
    LLAM_BROKER_RING_OP_TASK_JOIN = 10,
    LLAM_BROKER_RING_OP_TASK_DETACH = 11,
    LLAM_BROKER_RING_OP_CAP_ATTENUATE = 12,
    LLAM_BROKER_RING_OP_CAP_REVOKE = 13,
    LLAM_BROKER_RING_OP_FD_READ = LLAM_BROKER_RING_OP_DESCRIPTOR_READ,
    LLAM_BROKER_RING_OP_FD_WRITE = LLAM_BROKER_RING_OP_DESCRIPTOR_WRITE,
} llam_broker_ring_op_t;

typedef struct llam_broker_ring_submission {
    uint64_t request_id;
    uint32_t op;
    uint32_t reserved0;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    llam_capability_token_t token;
} llam_broker_ring_submission_t;

typedef struct llam_broker_ring_completion {
    uint64_t request_id;
    int32_t status;
    int32_t error_code;
    uint64_t result0;
    uint64_t result1;
} llam_broker_ring_completion_t;

typedef enum llam_broker_ring_client_stat {
    LLAM_BROKER_RING_CLIENT_STAT_SUBMIT_PUSHES = 0,
    LLAM_BROKER_RING_CLIENT_STAT_SUBMIT_FULL = 1,
    LLAM_BROKER_RING_CLIENT_STAT_SUBMIT_TAIL_PUBLISHES = 2,
    LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_DRAIN_CALLS = 3,
    LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_DRAIN_ENTRIES = 4,
    LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_EMPTY = 5,
    LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_HEAD_PUBLISHES = 6,
    LLAM_BROKER_RING_CLIENT_STAT_COMPLETE_BATCH_MAX = 7,
    LLAM_BROKER_RING_CLIENT_STAT_COUNT = 8,
} llam_broker_ring_client_stat_t;

typedef enum llam_broker_ring_broker_stat {
    LLAM_BROKER_RING_BROKER_STAT_SERVE_CALLS = 0,
    LLAM_BROKER_RING_BROKER_STAT_SERVE_SUCCESS = 1,
    LLAM_BROKER_RING_BROKER_STAT_SUBMIT_EMPTY = 2,
    LLAM_BROKER_RING_BROKER_STAT_COMPLETE_FULL = 3,
    LLAM_BROKER_RING_BROKER_STAT_SUBMIT_HEAD_PUBLISHES = 4,
    LLAM_BROKER_RING_BROKER_STAT_COMPLETE_TAIL_PUBLISHES = 5,
    LLAM_BROKER_RING_BROKER_STAT_SERVE_LATENCY_NS_TOTAL = 6,
    LLAM_BROKER_RING_BROKER_STAT_SERVE_LATENCY_NS_MAX = 7,
    LLAM_BROKER_RING_BROKER_STAT_COUNT = 8,
} llam_broker_ring_broker_stat_t;

typedef struct llam_broker_ring_cursor_line {
    /*
     * Each shared progress cursor is written by a different side of the SPSC
     * transport. Keep every cursor on its own cache line so client submits,
     * client drains, and broker progress publication do not invalidate each
     * other under high request rates.
     */
    _Alignas(LLAM_BROKER_RING_CACHELINE) _Atomic uint64_t value;
    unsigned char pad[LLAM_BROKER_RING_CACHELINE - sizeof(_Atomic uint64_t)];
} llam_broker_ring_cursor_line_t;

typedef struct llam_broker_ring_counter_line {
    /*
     * Diagnostic counters are grouped by writer side. They are client-visible
     * and therefore never authoritative for security, but keeping each side's
     * counters on separate lines prevents the diagnostics from reintroducing
     * the cursor false-sharing that they are meant to expose.
     */
    _Alignas(LLAM_BROKER_RING_CACHELINE) _Atomic uint64_t values[8];
} llam_broker_ring_counter_line_t;

typedef struct llam_broker_ring_stats {
    uint64_t client_submit_pushes;
    uint64_t client_submit_full;
    uint64_t client_submit_tail_publishes;
    uint64_t client_complete_drain_calls;
    uint64_t client_complete_drain_entries;
    uint64_t client_complete_empty;
    uint64_t client_complete_head_publishes;
    uint64_t client_complete_batch_max;
    uint64_t broker_serve_calls;
    uint64_t broker_serve_success;
    uint64_t broker_submit_empty;
    uint64_t broker_complete_full;
    uint64_t broker_submit_head_publishes;
    uint64_t broker_complete_tail_publishes;
    uint64_t broker_serve_latency_ns_total;
    uint64_t broker_serve_latency_ns_max;
    uint64_t cursor_write_estimate;
} llam_broker_ring_stats_t;

typedef struct llam_broker_ring_doorbell {
    /*
     * Optional kernel-assisted wait handle for shared-ring transports. The
     * shared ring remains the source of truth; the doorbell only prevents
     * clients or brokers from burning CPU while waiting for empty/full
     * transitions. The pending bit coalesces repeated notifications.
     */
    int handle;
    atomic_uint pending;
} llam_broker_ring_doorbell_t;

_Static_assert(sizeof(llam_broker_ring_cursor_line_t) == LLAM_BROKER_RING_CACHELINE,
               "broker ring cursor lines must occupy exactly one cache line");
_Static_assert(_Alignof(llam_broker_ring_cursor_line_t) >= LLAM_BROKER_RING_CACHELINE,
               "broker ring cursor lines must be cache-line aligned");
_Static_assert(sizeof(llam_broker_ring_counter_line_t) == LLAM_BROKER_RING_CACHELINE,
               "broker ring counter lines must occupy exactly one cache line");
_Static_assert(_Alignof(llam_broker_ring_counter_line_t) >= LLAM_BROKER_RING_CACHELINE,
               "broker ring counter lines must be cache-line aligned");

typedef struct llam_broker_ring {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t reserved0;
    llam_broker_ring_cursor_line_t submit_head;
    llam_broker_ring_cursor_line_t submit_tail;
    llam_broker_ring_cursor_line_t complete_head;
    llam_broker_ring_cursor_line_t complete_tail;
    llam_broker_ring_counter_line_t client_stats;
    llam_broker_ring_counter_line_t broker_stats;
    llam_broker_ring_submission_t submissions[LLAM_BROKER_RING_CAP];
    llam_broker_ring_completion_t completions[LLAM_BROKER_RING_CAP];
    /* Bounded client-visible staging area; broker buffers stay private. */
    unsigned char data[LLAM_BROKER_RING_DATA_BYTES];
} llam_broker_ring_t;

typedef struct llam_broker_ring_mapping {
    llam_broker_ring_t *ring;
    size_t bytes;
    int fd;
    llam_handle_t mapping_handle;
    bool owner;
    char name[128];
} llam_broker_ring_mapping_t;

typedef struct llam_broker_buffer_grant {
    uint64_t grant_id;
    uint64_t generation;
    uint64_t offset;
    uint64_t length;
    uint64_t rights;
    uint64_t revocation_epoch;
} llam_broker_buffer_grant_t;

static inline bool llam_broker_ring_valid(const llam_broker_ring_t *ring) {
    return ring != NULL &&
           ring->magic == LLAM_BROKER_RING_MAGIC &&
           ring->version == LLAM_BROKER_RING_VERSION &&
           ring->capacity == LLAM_BROKER_RING_CAP;
}

static inline bool llam_broker_ring_window_valid(uint64_t head, uint64_t tail) {
    return tail >= head && tail - head <= LLAM_BROKER_RING_CAP;
}

int llam_broker_ring_init(llam_broker_ring_t *ring);
int llam_broker_ring_submit_push(llam_broker_ring_t *ring, const llam_broker_ring_submission_t *entry);
int llam_broker_ring_submit_pop(llam_broker_ring_t *ring, llam_broker_ring_submission_t *out_entry);
int llam_broker_ring_complete_push(llam_broker_ring_t *ring, const llam_broker_ring_completion_t *entry);
int llam_broker_ring_complete_pop(llam_broker_ring_t *ring, llam_broker_ring_completion_t *out_entry);
int llam_broker_ring_complete_drain(llam_broker_ring_t *ring,
                                    llam_broker_ring_completion_t *out_entries,
                                    size_t max_entries,
                                    size_t *out_count);
int llam_broker_ring_doorbell_init(llam_broker_ring_doorbell_t *doorbell);
void llam_broker_ring_doorbell_destroy(llam_broker_ring_doorbell_t *doorbell);
int llam_broker_ring_doorbell_signal(llam_broker_ring_doorbell_t *doorbell);
int llam_broker_ring_wait_submit_available(const llam_broker_ring_t *ring,
                                           llam_broker_ring_doorbell_t *doorbell,
                                           int timeout_ms);
int llam_broker_ring_wait_submit_space(const llam_broker_ring_t *ring,
                                       llam_broker_ring_doorbell_t *doorbell,
                                       int timeout_ms);
int llam_broker_ring_wait_completion_available(const llam_broker_ring_t *ring,
                                               llam_broker_ring_doorbell_t *doorbell,
                                               int timeout_ms);
int llam_broker_ring_wait_completion_space(const llam_broker_ring_t *ring,
                                           llam_broker_ring_doorbell_t *doorbell,
                                           int timeout_ms);
void llam_broker_ring_client_stat_add(llam_broker_ring_t *ring,
                                      llam_broker_ring_client_stat_t stat,
                                      uint64_t delta);
void llam_broker_ring_client_stat_max(llam_broker_ring_t *ring,
                                      llam_broker_ring_client_stat_t stat,
                                      uint64_t value);
void llam_broker_ring_broker_stat_add(llam_broker_ring_t *ring,
                                      llam_broker_ring_broker_stat_t stat,
                                      uint64_t delta);
void llam_broker_ring_broker_stat_max(llam_broker_ring_t *ring,
                                      llam_broker_ring_broker_stat_t stat,
                                      uint64_t value);
int llam_broker_ring_collect_stats(const llam_broker_ring_t *ring, llam_broker_ring_stats_t *out_stats);
int llam_broker_ring_serve_one(llam_broker_t *broker, llam_broker_ring_t *ring);
int llam_broker_ring_serve_one_subject(llam_broker_t *broker,
                                       llam_broker_ring_t *ring,
                                       uint64_t subject_id);
int llam_broker_ring_register_mapping(llam_broker_t *broker,
                                      llam_broker_ring_mapping_t *mapping,
                                      uint64_t subject_id,
                                      uint64_t *out_session_id);
int llam_broker_ring_forget_session(llam_broker_t *broker,
                                    uint64_t session_id,
                                    uint64_t subject_id);
int llam_broker_ring_serve_session(llam_broker_t *broker,
                                   uint64_t session_id,
                                   uint64_t subject_id);
int llam_broker_ring_serve_locked_session(llam_broker_t *broker,
                                          llam_broker_ring_t *ring,
                                          llam_broker_ring_session_t *session);

bool llam_broker_ring_mapping_ring_valid(const llam_broker_ring_t *ring);
bool llam_broker_ring_name_valid(const char *name);
int llam_broker_ring_private_name(char *out_name, size_t out_name_len);
void llam_broker_ring_mapping_reset(llam_broker_ring_mapping_t *mapping);

int llam_broker_ring_create_shm(const char *name, llam_broker_ring_mapping_t *out_mapping);
int llam_broker_ring_create_private_shm(llam_broker_ring_mapping_t *out_mapping);
int llam_broker_ring_open_shm(const char *name, llam_broker_ring_mapping_t *out_mapping);
#if !LLAM_PLATFORM_WINDOWS
int llam_broker_ring_create_private_fd(llam_broker_ring_mapping_t *out_mapping);
int llam_broker_ring_map_fd(int fd, bool take_ownership, llam_broker_ring_mapping_t *out_mapping);
#else
int llam_broker_ring_create_private_handle(llam_broker_ring_mapping_t *out_mapping);
int llam_broker_ring_map_handle(llam_handle_t handle,
                                bool take_ownership,
                                llam_broker_ring_mapping_t *out_mapping);
#endif
void llam_broker_ring_unmap(llam_broker_ring_mapping_t *mapping);

int llam_broker_buffer_grant_init(llam_broker_buffer_grant_t *grant,
                                  uint64_t grant_id,
                                  uint64_t generation,
                                  uint64_t offset,
                                  uint64_t length,
                                  uint64_t rights,
                                  uint64_t revocation_epoch);
int llam_broker_buffer_grant_validate(const llam_broker_buffer_grant_t *grant,
                                      uint64_t required_rights,
                                      uint64_t relative_offset,
                                      uint64_t length,
                                      uint64_t current_revocation_epoch);

#endif
