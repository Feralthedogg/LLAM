/**
 * @file tests/test_security_capability.c
 * @brief Broker capability token regression tests.
 *
 * @details
 * These tests exercise the security boundary that can exist only when tokens
 * are validated outside the untrusted caller address space. They intentionally
 * do not claim that in-process sealed handles stop arbitrary memory R/W.
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

#include "runtime_capability.h"
#include "runtime_broker.h"
#include "runtime_broker_ring.h"
#include "runtime_proto_core.h"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if LLAM_PLATFORM_WINDOWS
#include <aclapi.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

/*
 * Broker values are intentionally valid caller-owned objects, so examples and
 * tests may place them on the stack. Keep the control object below conservative
 * Windows thread-stack limits; large data-plane tables must be broker-owned heap
 * storage instead of inline fields.
 */
_Static_assert(sizeof(llam_broker_t) < (256U * 1024U), "llam_broker_t must remain stack-safe");
_Static_assert(offsetof(llam_broker_ring_t, submit_head) % LLAM_BROKER_RING_CACHELINE == 0U,
               "broker submit_head cursor must start on a cache-line boundary");
_Static_assert(offsetof(llam_broker_ring_t, submit_tail) - offsetof(llam_broker_ring_t, submit_head) >=
                   LLAM_BROKER_RING_CACHELINE,
               "broker submit_head and submit_tail must not share a cache line");
_Static_assert(offsetof(llam_broker_ring_t, complete_head) - offsetof(llam_broker_ring_t, submit_tail) >=
                   LLAM_BROKER_RING_CACHELINE,
               "broker submit_tail and complete_head must not share a cache line");
_Static_assert(offsetof(llam_broker_ring_t, complete_tail) - offsetof(llam_broker_ring_t, complete_head) >=
                   LLAM_BROKER_RING_CACHELINE,
               "broker complete_head and complete_tail must not share a cache line");
_Static_assert(offsetof(llam_broker_ring_t, client_stats) - offsetof(llam_broker_ring_t, complete_tail) >=
                   LLAM_BROKER_RING_CACHELINE,
               "broker completion cursor and client stats must not share a cache line");
_Static_assert(offsetof(llam_broker_ring_t, broker_stats) - offsetof(llam_broker_ring_t, client_stats) >=
                   LLAM_BROKER_RING_CACHELINE,
               "broker client and broker stats must not share a cache line");

static uint64_t test_env_u64(const char *name, uint64_t fallback, uint64_t max_value) {
    const char *value;
    char *end = NULL;
    unsigned long long parsed;

    value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0U || (uint64_t)parsed > max_value) {
        return fallback;
    }
    return (uint64_t)parsed;
}

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if defined(__SANITIZE_ADDRESS__) || \
    defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || \
    __has_feature(thread_sanitizer)
#define LLAM_TEST_SANITIZER_BUILD 1
#else
#define LLAM_TEST_SANITIZER_BUILD 0
#endif

static uint64_t test_broker_ring_flood_iters(void) {
    return test_env_u64("LLAM_BROKER_RING_FLOOD_ITERS", UINT64_C(20000), UINT64_C(1000000));
}

static uint64_t test_broker_ring_replay_iters(void) {
    return test_env_u64("LLAM_BROKER_RING_REPLAY_ITERS", UINT64_C(1024), UINT64_C(100000));
}

static uint64_t test_broker_ring_batch_perf_iters(void) {
    return test_env_u64("LLAM_BROKER_RING_BATCH_PERF_ITERS", UINT64_C(32768), UINT64_C(1000000));
}

static uint64_t test_broker_ring_batch_min_ops(void) {
#if LLAM_TEST_SANITIZER_BUILD
    return test_env_u64("LLAM_BROKER_RING_BATCH_MIN_OPS", UINT64_C(1000), UINT64_C(1000000000));
#else
    return test_env_u64("LLAM_BROKER_RING_BATCH_MIN_OPS", UINT64_C(100000), UINT64_C(1000000000));
#endif
}

static uint64_t test_broker_ring_batch_max_p50_us(void) {
#if LLAM_TEST_SANITIZER_BUILD
    return test_env_u64("LLAM_BROKER_RING_BATCH_MAX_P50_US", UINT64_C(50000), UINT64_C(60000000));
#else
    return test_env_u64("LLAM_BROKER_RING_BATCH_MAX_P50_US", UINT64_C(5000), UINT64_C(60000000));
#endif
}

static uint64_t test_broker_ring_batch_max_p99_us(void) {
#if LLAM_TEST_SANITIZER_BUILD
    return test_env_u64("LLAM_BROKER_RING_BATCH_MAX_P99_US", UINT64_C(500000), UINT64_C(60000000));
#else
    return test_env_u64("LLAM_BROKER_RING_BATCH_MAX_P99_US", UINT64_C(50000), UINT64_C(60000000));
#endif
}

static size_t broker_active_task_count(const llam_broker_t *broker) {
    size_t count = 0U;

    if (broker == NULL) {
        return 0U;
    }
    for (size_t i = 0U; i < LLAM_BROKER_TASK_SLOTS; ++i) {
        count += broker->tasks[i].active ? 1U : 0U;
    }
    return count;
}

static int compare_u64(const void *lhs, const void *rhs) {
    const uint64_t a = *(const uint64_t *)lhs;
    const uint64_t b = *(const uint64_t *)rhs;

    return (a > b) - (a < b);
}

static llam_capability_key_t test_key(void) {
    llam_capability_key_t key;

    key.words[0] = UINT64_C(0x0706050403020100);
    key.words[1] = UINT64_C(0x0f0e0d0c0b0a0908);
    key.words[2] = UINT64_C(0x1716151413121110);
    key.words[3] = UINT64_C(0x1f1e1d1c1b1a1918);
    return key;
}

static llam_capability_object_t test_object(void) {
    llam_capability_object_t object;

    memset(&object, 0, sizeof(object));
    object.runtime_id = 7U;
    object.family = 4U;
    object.slot = 11U;
    object.generation = 13U;
    object.revocation_epoch = 3U;
    return object;
}

static bool capability_key_is_zero(const llam_capability_key_t *key) {
    return key != NULL &&
           (key->words[0] | key->words[1] | key->words[2] | key->words[3]) == 0U;
}

static bool memory_is_byte(const void *data, size_t length, unsigned char value) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t i;

    if (data == NULL) {
        return false;
    }
    for (i = 0U; i < length; ++i) {
        if (bytes[i] != value) {
            return false;
        }
    }
    return true;
}

static bool broker_ring_mapping_is_reset(const llam_broker_ring_mapping_t *mapping) {
    if (mapping == NULL) {
        return false;
    }
    return mapping->ring == NULL &&
           mapping->bytes == 0U &&
           mapping->fd == -1 &&
           LLAM_HANDLE_IS_INVALID(mapping->mapping_handle) &&
           !mapping->owner &&
           mapping->name[0] == '\0';
}

static int expect_errno(int rc, int expected_errno, const char *message) {
    if (rc == 0 || errno != expected_errno) {
        fprintf(stderr,
                "[test_security_capability] %s: rc=%d errno=%d expected=%d\n",
                message,
                rc,
                errno,
                expected_errno);
        return -1;
    }
    return 0;
}

#if !LLAM_PLATFORM_WINDOWS
static int expect_errno_either(int rc, int first_errno, int second_errno, const char *message) {
    if (rc == 0 || (errno != first_errno && errno != second_errno)) {
        fprintf(stderr,
                "[test_security_capability] %s: rc=%d errno=%d expected=%d|%d\n",
                message,
                rc,
                errno,
                first_errno,
                second_errno);
        return -1;
    }
    return 0;
}

static bool broker_fd_is_closed(int fd) {
    int saved_errno;

    errno = 0;
    if (fcntl(fd, F_GETFD) >= 0) {
        return false;
    }
    saved_errno = errno;
    return saved_errno == EBADF;
}
#endif

static int test_broker_destroy_scrubs_authority_state(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    uint64_t subject_id = 0U;
    size_t i;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (capability_key_is_zero(&broker.capability_key)) {
        llam_broker_destroy(&broker);
        return -1;
    }
    if (llam_broker_transport_subject(&broker, UINTPTR_MAX - 7U, &subject_id) != 0 ||
        subject_id == 0U ||
        llam_broker_ring_init(&ring) != 0) {
        llam_broker_destroy(&broker);
        return -1;
    }
    memset(&submission, 0, sizeof(submission));
    submission.request_id = 123U;
    submission.op = LLAM_BROKER_RING_OP_NOP;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one_subject(&broker, &ring, subject_id) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0) {
        llam_broker_destroy(&broker);
        return -1;
    }

    llam_broker_destroy(&broker);

    if (!capability_key_is_zero(&broker.capability_key) ||
        atomic_load_explicit(&broker.revocation_epoch, memory_order_acquire) != 0U ||
        broker.next_buffer_id != 0U ||
        broker.next_descriptor_id != 0U ||
        broker.next_channel_id != 0U ||
        broker.next_task_id != 0U ||
        broker.next_transport_subject_nonce != 0U) {
        return -1;
    }
    for (i = 0U; i < LLAM_BROKER_TRANSPORT_SESSIONS; ++i) {
        const llam_broker_transport_session_t *session = &broker.transport_sessions[i];

        if (session->active || session->transport_id != 0U || session->subject_id != 0U) {
            return -1;
        }
    }
    for (i = 0U; i < LLAM_BROKER_RING_SESSIONS; ++i) {
        const llam_broker_ring_session_t *session = &broker.ring_sessions[i];

        if (session->active ||
            session->busy ||
            session->ring != NULL ||
            session->subject_id != 0U ||
            session->submit_head != 0U ||
            session->complete_tail != 0U) {
            return -1;
        }
    }
    return 0;
}

static int test_broker_transport_subject_rejects_destroying_broker(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    uint64_t subject_id = 0U;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_lock(&broker) != 0) {
        llam_broker_destroy(&broker);
        return -1;
    }
    broker.destroying = true;
    llam_broker_unlock(&broker);

    errno = 0;
    if (expect_errno(llam_broker_transport_subject(&broker, UINTPTR_MAX - 9U, &subject_id),
                     EINVAL,
                     "destroying broker accepted a new transport subject") != 0 ||
        subject_id != 0U) {
        goto done;
    }
    rc = 0;

done:
    if (llam_broker_lock(&broker) == 0) {
        broker.destroying = false;
        llam_broker_unlock(&broker);
    }
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_transport_subject_requires_os_entropy(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    uint64_t subject_id = UINT64_MAX;
    uintptr_t transport_id = UINTPTR_MAX - 17U;
    size_t i;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }

    llam_broker_test_force_subject_entropy_failure(true);
    errno = 0;
    if (expect_errno(llam_broker_transport_subject(&broker, transport_id, &subject_id),
                     EIO,
                     "broker transport subject used deterministic entropy fallback") != 0 ||
        subject_id != 0U) {
        llam_broker_test_force_subject_entropy_failure(false);
        goto done;
    }
    llam_broker_test_force_subject_entropy_failure(false);

    /*
     * Failing closed must not reserve a half-initialized audience slot. A later
     * successful subject allocation for the same transport should create the
     * first live session, not reuse attacker-influenced partial state.
     */
    for (i = 0U; i < LLAM_BROKER_TRANSPORT_SESSIONS; ++i) {
        if (broker.transport_sessions[i].active &&
            broker.transport_sessions[i].transport_id == transport_id) {
            goto done;
        }
    }
    subject_id = 0U;
    if (llam_broker_transport_subject(&broker, transport_id, &subject_id) != 0 ||
        subject_id == 0U) {
        goto done;
    }
    rc = 0;

done:
    llam_broker_test_force_subject_entropy_failure(false);
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_transport_subject_collision_fails_closed(void) {
    const uintptr_t first_transport = UINTPTR_MAX - 101U;
    const uintptr_t second_transport = UINTPTR_MAX - 102U;
    const uint64_t forced_subject = UINT64_C(0xfeedfacecafebeef);
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    uint64_t first_subject = 0U;
    uint64_t second_subject = UINT64_MAX;
    uint64_t check_subject = 0U;
    size_t i;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }

    /*
     * A transport subject is a capability-token audience. Force a deterministic
     * collision so the test proves the allocator rejects duplicate live
     * audiences instead of relying on practical 64-bit collision odds.
     */
    llam_broker_test_force_subject_value(true, forced_subject);
    if (llam_broker_transport_subject(&broker, first_transport, &first_subject) != 0 ||
        first_subject != forced_subject) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_transport_subject(&broker, second_transport, &second_subject),
                     EIO,
                     "broker transport subject collision was accepted") != 0 ||
        second_subject != 0U) {
        goto done;
    }
    for (i = 0U; i < LLAM_BROKER_TRANSPORT_SESSIONS; ++i) {
        if (broker.transport_sessions[i].active &&
            broker.transport_sessions[i].transport_id == second_transport) {
            goto done;
        }
    }
    if (llam_broker_transport_subject(&broker, first_transport, &check_subject) != 0 ||
        check_subject != first_subject) {
        goto done;
    }

    llam_broker_test_force_subject_value(false, 0U);
    if (llam_broker_transport_subject(&broker, second_transport, &second_subject) != 0 ||
        second_subject == 0U ||
        second_subject == first_subject) {
        goto done;
    }
    rc = 0;

done:
    llam_broker_test_force_subject_value(false, 0U);
    llam_broker_destroy(&broker);
    return rc;
}

static void request_init(llam_broker_wire_request_t *request, llam_broker_wire_op_t op) {
    memset(request, 0, sizeof(*request));
    request->magic = LLAM_BROKER_WIRE_MAGIC;
    request->version = LLAM_BROKER_WIRE_VERSION;
    request->op = (uint32_t)op;
}

static bool broker_failure_authority_outputs_are_clear(const llam_broker_wire_response_t *response) {
    return response != NULL &&
           response->status != 0 &&
           response->error_code != 0 &&
           response->result0 == 0U &&
           response->result1 == 0U &&
           response->result2 == 0U &&
           memory_is_byte(&response->token, sizeof(response->token), 0U) &&
           memory_is_byte(response->data, sizeof(response->data), 0U);
}

static bool broker_ring_failure_completion_outputs_are_clear(const llam_broker_ring_completion_t *completion) {
    return completion != NULL &&
           completion->status != 0 &&
           completion->error_code != 0 &&
           completion->result0 == 0U &&
           completion->result1 == 0U;
}

static uint64_t broker_malformed_fuzz_next(uint64_t *state) {
    uint64_t x = *state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x != 0U ? x : UINT64_C(0x9e3779b97f4a7c15);
    return *state;
}

static int test_token_validation_and_rights(void) {
    llam_capability_key_t key = test_key();
    llam_capability_object_t object = test_object();
    llam_capability_token_t token;

    if (llam_capability_issue(&key, &object, LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_RECV, &token) != 0) {
        return -1;
    }
    if (llam_capability_validate(&key, &token, LLAM_CAP_RIGHT_SEND, object.revocation_epoch) != 0) {
        return -1;
    }
    if (llam_capability_validate(&key, &token, LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_RECV, object.revocation_epoch) != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_capability_validate(&key, &token, LLAM_CAP_RIGHT_ADMIN, object.revocation_epoch),
                     EACCES,
                     "admin right unexpectedly accepted") != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_capability_validate(&key, &token, LLAM_CAP_RIGHT_SEND, object.revocation_epoch + 1U),
                     EACCES,
                     "revoked token unexpectedly accepted") != 0) {
        return -1;
    }
    return 0;
}

static int test_raw_capability_validate_requires_nonzero_rights(void) {
    llam_capability_key_t key = test_key();
    llam_capability_object_t object = test_object();
    llam_capability_token_t token;

    if (llam_capability_issue(&key, &object, LLAM_CAP_RIGHT_SEND, &token) != 0) {
        return -1;
    }

    /*
     * Raw helpers are still internal, but accepting a zero-right validation
     * makes them a live-token oracle for future direct users. Broker-visible
     * paths already reject this; keep the primitive fail-closed too.
     */
    errno = 0;
    if (expect_errno(llam_capability_validate(&key, &token, 0U, object.revocation_epoch),
                     EINVAL,
                     "raw capability accepted rightless validation") != 0) {
        return -1;
    }

    object.subject_id = 42U;
    if (llam_capability_issue(&key, &object, LLAM_CAP_RIGHT_SEND, &token) != 0) {
        return -1;
    }
    errno = 0;
    return expect_errno(llam_capability_validate_subject(&key,
                                                        &token,
                                                        0U,
                                                        object.revocation_epoch,
                                                        object.subject_id),
                        EINVAL,
                        "subject capability accepted rightless validation");
}

static int test_token_tamper_rejected(void) {
    llam_capability_key_t key = test_key();
    llam_capability_object_t object = test_object();
    llam_capability_token_t token;

    if (llam_capability_issue(&key, &object, LLAM_CAP_RIGHT_SEND, &token) != 0) {
        return -1;
    }

    token.slot ^= 1U;
    errno = 0;
    if (expect_errno(llam_capability_validate(&key, &token, LLAM_CAP_RIGHT_SEND, object.revocation_epoch),
                     EACCES,
                     "slot tamper accepted") != 0) {
        return -1;
    }
    token.slot ^= 1U;
    token.rights |= LLAM_CAP_RIGHT_DESTROY;
    errno = 0;
    if (expect_errno(llam_capability_validate(&key, &token, LLAM_CAP_RIGHT_DESTROY, object.revocation_epoch),
                     EACCES,
                     "rights escalation accepted") != 0) {
        return -1;
    }
    token.rights &= ~LLAM_CAP_RIGHT_DESTROY;
    token.revocation_epoch = 0U;
    errno = 0;
    if (expect_errno(llam_capability_validate(&key, &token, LLAM_CAP_RIGHT_SEND, object.revocation_epoch),
                     EINVAL,
                     "zero-epoch token accepted") != 0) {
        return -1;
    }
    token.revocation_epoch = object.revocation_epoch;
    token.mac[0] ^= 0x80U;
    errno = 0;
    if (expect_errno(llam_capability_validate(&key, &token, LLAM_CAP_RIGHT_SEND, object.revocation_epoch),
                     EACCES,
                     "mac tamper accepted") != 0) {
        return -1;
    }
    return 0;
}

static int test_token_subject_binding(void) {
    llam_capability_key_t key = test_key();
    llam_capability_object_t object = test_object();
    llam_capability_token_t token;

    object.subject_id = 42U;
    if (llam_capability_issue(&key, &object, LLAM_CAP_RIGHT_SEND, &token) != 0) {
        return -1;
    }
    if (llam_capability_validate_subject(&key,
                                         &token,
                                         LLAM_CAP_RIGHT_SEND,
                                         object.revocation_epoch,
                                         object.subject_id) != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_capability_validate(&key, &token, LLAM_CAP_RIGHT_SEND, object.revocation_epoch),
                     EACCES,
                     "subject-bound token accepted as bearer") != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_capability_validate_subject(&key,
                                                     &token,
                                                     LLAM_CAP_RIGHT_SEND,
                                                     object.revocation_epoch,
                                                     object.subject_id + 1U),
                     EACCES,
                     "token accepted for wrong subject") != 0) {
        return -1;
    }
    token.subject_id ^= 1U;
    errno = 0;
    return expect_errno(llam_capability_validate_subject(&key,
                                                        &token,
                                                        LLAM_CAP_RIGHT_SEND,
                                                        object.revocation_epoch,
                                                        object.subject_id),
                        EACCES,
                        "subject tamper accepted");
}

static int test_attenuation_cannot_expand_rights(void) {
    llam_capability_key_t key = test_key();
    llam_capability_object_t object = test_object();
    llam_capability_token_t token;
    llam_capability_token_t attenuated;

    if (llam_capability_issue(&key,
                              &object,
                              LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_RECV | LLAM_CAP_RIGHT_CLOSE,
                              &token) != 0) {
        return -1;
    }
    if (llam_capability_attenuate(&key, &token, LLAM_CAP_RIGHT_SEND, object.revocation_epoch, &attenuated) != 0) {
        return -1;
    }
    if (llam_capability_validate(&key, &attenuated, LLAM_CAP_RIGHT_SEND, object.revocation_epoch) != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_capability_validate(&key, &attenuated, LLAM_CAP_RIGHT_RECV, object.revocation_epoch),
                     EACCES,
                     "attenuated recv right accepted") != 0) {
        return -1;
    }
    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_capability_attenuate(&key,
                                               &attenuated,
                                               LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_DESTROY,
                                               object.revocation_epoch,
                                               &token),
                     EACCES,
                     "attenuation expanded rights") != 0) {
        return -1;
    }
    if (!memory_is_byte(&token, sizeof(token), 0U)) {
        fprintf(stderr, "[test_security_capability] failed attenuation left stale output token\n");
        return -1;
    }
    token = attenuated;
    errno = 0;
    if (expect_errno(llam_capability_attenuate(&key,
                                               &token,
                                               LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_DESTROY,
                                               object.revocation_epoch,
                                               &token),
                     EACCES,
                     "in-place attenuation expanded rights") != 0) {
        return -1;
    }
    if (!memory_is_byte(&token, sizeof(token), 0U)) {
        fprintf(stderr, "[test_security_capability] failed in-place attenuation kept authority\n");
        return -1;
    }
    return 0;
}

static int test_wrong_key_rejected(void) {
    llam_capability_key_t key = test_key();
    llam_capability_key_t wrong_key = test_key();
    llam_capability_object_t object = test_object();
    llam_capability_token_t token;

    wrong_key.words[2] ^= UINT64_C(0xfeedfacecafebeef);
    if (llam_capability_issue(&key, &object, LLAM_CAP_RIGHT_CLOSE, &token) != 0) {
        return -1;
    }
    errno = 0;
    return expect_errno(llam_capability_validate(&wrong_key, &token, LLAM_CAP_RIGHT_CLOSE, object.revocation_epoch),
                        EACCES,
                        "wrong broker key accepted");
}

static int test_capability_key_requires_os_entropy(void) {
    llam_capability_key_t key;
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    int rc;

    memset(&key, 0x5a, sizeof(key));
    llam_capability_test_force_entropy_failure(true);
    errno = 0;
    rc = llam_capability_key_init(&key, &key, 17U);
    llam_capability_test_force_entropy_failure(false);
    if (expect_errno(rc, EIO, "broker capability key used weak fallback entropy") != 0) {
        return -1;
    }
    if (!capability_key_is_zero(&key)) {
        fprintf(stderr, "[test_security_capability] failed key initialization left key material\n");
        return -1;
    }
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    llam_capability_test_force_entropy_failure(true);
    errno = 0;
    rc = llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE);
    llam_capability_test_force_entropy_failure(false);
    if (rc == 0) {
        llam_broker_destroy(&broker);
    }
    if (expect_errno(rc, EIO, "broker initialized with weak deterministic capability key") != 0) {
        return -1;
    }
    return 0;
}

static int test_in_process_runtime_does_not_require_broker_entropy(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    int rc;
    int saved_errno;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    /*
     * The default in-process runtime is not a capability boundary. Broker MAC
     * entropy failure must fail broker setup, not ordinary runtime creation.
     */
    llam_capability_test_force_entropy_failure(true);
    errno = 0;
    rc = llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime);
    saved_errno = errno;
    llam_capability_test_force_entropy_failure(false);

    if (rc != 0) {
        fprintf(stderr,
                "[test_security_capability] in-process runtime depended on broker entropy: errno=%d\n",
                saved_errno);
        return -1;
    }
    llam_runtime_destroy(runtime);
    return 0;
}

static int test_capability_issue_requires_os_entropy(void) {
    llam_capability_key_t key = test_key();
    llam_capability_object_t object = test_object();
    llam_capability_token_t token;
    memset(&token, 0xa5, sizeof(token));

    llam_capability_test_force_entropy_failure(true);
    errno = 0;
    if (expect_errno(llam_capability_issue(&key, &object, LLAM_CAP_RIGHT_READ, &token),
                     EIO,
                     "capability issue used deterministic nonce fallback") != 0) {
        llam_capability_test_force_entropy_failure(false);
        return -1;
    }
    llam_capability_test_force_entropy_failure(false);
    if (!memory_is_byte(&token, sizeof(token), 0U)) {
        fprintf(stderr, "[test_security_capability] failed issue left partial token bytes\n");
        return -1;
    }
    return 0;
}

static int test_capability_issue_clears_output_on_invalid_input(void) {
    llam_capability_key_t key = test_key();
    llam_capability_object_t object = test_object();
    llam_capability_token_t token;

    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_capability_issue(&key, &object, 0U, &token),
                     EINVAL,
                     "capability issue accepted zero rights") != 0) {
        return -1;
    }
    if (!memory_is_byte(&token, sizeof(token), 0U)) {
        fprintf(stderr, "[test_security_capability] invalid capability issue left stale output token\n");
        return -1;
    }

    memset(&token, 0xa5, sizeof(token));
    object.slot = 0U;
    errno = 0;
    if (expect_errno(llam_capability_issue(&key, &object, LLAM_CAP_RIGHT_READ, &token),
                     EINVAL,
                     "capability issue accepted zero object slot") != 0) {
        return -1;
    }
    if (!memory_is_byte(&token, sizeof(token), 0U)) {
        fprintf(stderr, "[test_security_capability] zero-slot issue left stale output token\n");
        return -1;
    }

    memset(&token, 0xa5, sizeof(token));
    object = test_object();
    object.generation = 0U;
    errno = 0;
    if (expect_errno(llam_capability_issue(&key, &object, LLAM_CAP_RIGHT_READ, &token),
                     EINVAL,
                     "capability issue accepted zero generation") != 0) {
        return -1;
    }
    if (!memory_is_byte(&token, sizeof(token), 0U)) {
        fprintf(stderr, "[test_security_capability] zero-generation issue left stale output token\n");
        return -1;
    }

    memset(&token, 0xa5, sizeof(token));
    object = test_object();
    object.revocation_epoch = 0U;
    errno = 0;
    if (expect_errno(llam_capability_issue(&key, &object, LLAM_CAP_RIGHT_READ, &token),
                     EINVAL,
                     "capability issue accepted zero revocation epoch") != 0) {
        return -1;
    }
    if (!memory_is_byte(&token, sizeof(token), 0U)) {
        fprintf(stderr, "[test_security_capability] zero-epoch issue left stale output token\n");
        return -1;
    }
    return 0;
}

static int test_broker_issue_validate_and_revoke(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_token_t token;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_create_channel(&broker,
                                   2U,
                                   LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_CLOSE,
                                   &token) != 0) {
        llam_broker_destroy(&broker);
        return -1;
    }
    if (llam_broker_validate_cap(&broker, &token, LLAM_CAP_RIGHT_SEND) != 0) {
        llam_broker_destroy(&broker);
        return -1;
    }
    (void)llam_broker_revoke_all(&broker);
    errno = 0;
    if (expect_errno(llam_broker_validate_cap(&broker, &token, LLAM_CAP_RIGHT_SEND),
                     EACCES,
                     "revoked broker token accepted") != 0) {
        llam_broker_destroy(&broker);
        return -1;
    }
    llam_broker_destroy(&broker);
    return 0;
}

static int test_broker_create_paths_clear_output_on_invalid_input(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_token_t token;
#if !LLAM_PLATFORM_WINDOWS
    int pipe_fds[2] = {-1, -1};
#endif
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }

    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_broker_register_buffer(&broker, NULL, 0U, LLAM_CAP_RIGHT_READ, &token),
                     EINVAL,
                     "broker buffer create accepted zero length") != 0 ||
        !memory_is_byte(&token, sizeof(token), 0U)) {
        goto done;
    }

    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_broker_register_buffer(&broker, NULL, 16U, LLAM_CAP_RIGHT_SEND, &token),
                     EACCES,
                     "broker buffer create accepted channel rights") != 0 ||
        !memory_is_byte(&token, sizeof(token), 0U)) {
        goto done;
    }

    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_broker_register_buffer(&broker,
                                                 NULL,
                                                 (size_t)LLAM_BROKER_BUFFER_MAX_BYTES + 1U,
                                                 LLAM_CAP_RIGHT_READ,
                                                 &token),
                     EINVAL,
                     "broker buffer create accepted oversized direct buffer") != 0 ||
        !memory_is_byte(&token, sizeof(token), 0U)) {
        goto done;
    }

    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_broker_create_channel(&broker, 0U, LLAM_CAP_RIGHT_SEND, &token),
                     EINVAL,
                     "broker channel create accepted zero capacity") != 0 ||
        !memory_is_byte(&token, sizeof(token), 0U)) {
        goto done;
    }

    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_broker_create_channel(&broker, 2U, LLAM_CAP_RIGHT_READ, &token),
                     EACCES,
                     "broker channel create accepted buffer rights") != 0 ||
        !memory_is_byte(&token, sizeof(token), 0U)) {
        goto done;
    }

#if !LLAM_PLATFORM_WINDOWS
    if (pipe(pipe_fds) != 0) {
        goto done;
    }
    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_broker_register_fd(&broker,
                                             pipe_fds[0],
                                             LLAM_CAP_RIGHT_SEND,
                                             false,
                                             &token),
                     EACCES,
                     "broker descriptor register accepted channel rights") != 0 ||
        !memory_is_byte(&token, sizeof(token), 0U)) {
        goto done;
    }
    close(pipe_fds[0]);
    pipe_fds[0] = -1;
    close(pipe_fds[1]);
    pipe_fds[1] = -1;
#else
    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_broker_register_fd(&broker,
                                             -1,
                                             LLAM_CAP_RIGHT_READ,
                                             false,
                                             &token),
                     ENOTSUP,
                     "unsupported Windows broker fd register did not fail with ENOTSUP") != 0 ||
        !memory_is_byte(&token, sizeof(token), 0U)) {
        goto done;
    }
#endif

    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_broker_spawn_task(&broker,
                                            UINT32_MAX,
                                            0U,
                                            LLAM_CAP_RIGHT_JOIN,
                                            &token),
                     EINVAL,
                     "broker task spawn accepted invalid task kind") != 0 ||
        !memory_is_byte(&token, sizeof(token), 0U)) {
        goto done;
    }

    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_broker_spawn_task(&broker,
                                            LLAM_BROKER_TASK_KIND_RETURN_U64,
                                            0U,
                                            LLAM_CAP_RIGHT_JOIN | LLAM_CAP_RIGHT_READ,
                                            &token),
                     EACCES,
                     "broker task spawn accepted buffer rights") != 0 ||
        !memory_is_byte(&token, sizeof(token), 0U)) {
        goto done;
    }

    rc = 0;

done:
#if !LLAM_PLATFORM_WINDOWS
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
    }
#endif
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_transport_grants_require_explicit_rights(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    llam_handle_t response_descriptor = LLAM_INVALID_HANDLE;
#if LLAM_PLATFORM_WINDOWS
    HANDLE pipe_read = INVALID_HANDLE_VALUE;
    HANDLE pipe_write = INVALID_HANDLE_VALUE;
#else
    int pipe_fds[2] = {-1, -1};
#endif
    bool broker_initialized = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;

    /*
     * Wire grants cross an untrusted transport boundary.  A zero rights field
     * must mean "invalid request", not "mint all allowed rights" for a
     * zero-initialized or truncated client request.
     */
    request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_BUFFER);
    request.slot = 16U;
    llam_broker_process_request_with_descriptors(&broker,
                                                 &request,
                                                 &response,
                                                 NULL,
                                                 LLAM_INVALID_HANDLE,
                                                 &response_descriptor);
    if (response.status == 0 || response.error_code != EACCES) {
        fprintf(stderr,
                "[test_security_capability] zero-right CREATE_BUFFER minted authority status=%d errno=%d\n",
                response.status,
                response.error_code);
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_CHANNEL);
    request.slot = 2U;
    llam_broker_process_request_with_descriptors(&broker,
                                                 &request,
                                                 &response,
                                                 NULL,
                                                 LLAM_INVALID_HANDLE,
                                                 &response_descriptor);
    if (response.status == 0 || response.error_code != EACCES) {
        fprintf(stderr,
                "[test_security_capability] zero-right CREATE_CHANNEL minted authority status=%d errno=%d\n",
                response.status,
                response.error_code);
        goto done;
    }

#if LLAM_PLATFORM_WINDOWS
    if (!CreatePipe(&pipe_read, &pipe_write, NULL, 0U)) {
        goto done;
    }
    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    llam_broker_process_request_with_descriptors(&broker,
                                                 &request,
                                                 &response,
                                                 NULL,
                                                 (llam_handle_t)pipe_read,
                                                 &response_descriptor);
    pipe_read = INVALID_HANDLE_VALUE;
#else
    if (pipe(pipe_fds) != 0) {
        goto done;
    }
    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    llam_broker_process_request_with_descriptors(&broker,
                                                 &request,
                                                 &response,
                                                 NULL,
                                                 (llam_handle_t)pipe_fds[0],
                                                 &response_descriptor);
    pipe_fds[0] = -1;
#endif
    if (response.status == 0 || response.error_code != EACCES) {
        fprintf(stderr,
                "[test_security_capability] zero-right REGISTER_DESCRIPTOR minted authority status=%d errno=%d\n",
                response.status,
                response.error_code);
        goto done;
    }

    rc = 0;

done:
#if LLAM_PLATFORM_WINDOWS
    if (pipe_read != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_read);
    }
    if (pipe_write != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_write);
    }
#else
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
    }
#endif
    if (!llam_handle_is_invalid(response_descriptor)) {
        llam_broker_close_handle(response_descriptor);
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_transport_malformed_requests_fail_closed(void) {
    static const uint32_t ops[] = {
        LLAM_BROKER_WIRE_OP_ISSUE_CAP,
        LLAM_BROKER_WIRE_OP_VALIDATE_CAP,
        LLAM_BROKER_WIRE_OP_REVOKE_ALL,
        LLAM_BROKER_WIRE_OP_ATTENUATE_CAP,
        LLAM_BROKER_WIRE_OP_REVOKE_CAP,
        LLAM_BROKER_WIRE_OP_CREATE_BUFFER,
        LLAM_BROKER_WIRE_OP_CREATE_CHANNEL,
        LLAM_BROKER_WIRE_OP_BUFFER_READ,
        LLAM_BROKER_WIRE_OP_BUFFER_WRITE,
        LLAM_BROKER_WIRE_OP_CHANNEL_SEND,
        LLAM_BROKER_WIRE_OP_CHANNEL_RECV,
        LLAM_BROKER_WIRE_OP_CHANNEL_CLOSE,
        LLAM_BROKER_WIRE_OP_TASK_SPAWN,
        LLAM_BROKER_WIRE_OP_TASK_JOIN,
        LLAM_BROKER_WIRE_OP_TASK_DETACH,
        LLAM_BROKER_WIRE_OP_DESCRIPTOR_READ,
        LLAM_BROKER_WIRE_OP_DESCRIPTOR_WRITE,
        LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR,
        LLAM_BROKER_WIRE_OP_CREATE_RING,
        LLAM_BROKER_WIRE_OP_SERVE_RING,
        UINT32_C(0xfffffff0),
        UINT32_C(0xffffffff),
    };
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    bool broker_initialized = false;
    uint64_t rng = UINT64_C(0x2d3c4b5a69788796);
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;

    for (size_t i = 0U; i < 512U; ++i) {
        llam_broker_wire_request_t request;
        llam_broker_wire_response_t response;
        bool should_close = true;
        uint64_t a = broker_malformed_fuzz_next(&rng);
        uint64_t b = broker_malformed_fuzz_next(&rng);
        uint64_t c = broker_malformed_fuzz_next(&rng);

        /*
         * Keep framing valid so every case reaches the dispatcher body, then
         * make the operation-specific authority inputs malformed. This guards
         * future op handlers against accidentally publishing stale token/result
         * fields on request validation failures.
         */
        memset(&request, 0, sizeof(request));
        request.magic = LLAM_BROKER_WIRE_MAGIC;
        request.version = LLAM_BROKER_WIRE_VERSION;
        request.op = ops[i % (sizeof(ops) / sizeof(ops[0]))];
        request.slot = a;
        request.generation = b;
        request.rights = (i & 1U) != 0U ? 0U : LLAM_CAP_RIGHT_ADMIN;
        request.required_rights = (i & 2U) != 0U ? 0U : LLAM_CAP_RIGHT_ADMIN;
        request.offset = b;
        request.length = (i & 4U) != 0U ? 0U : ((uint64_t)LLAM_BROKER_WIRE_DATA_BYTES + 1U + (c & 7U));

        switch ((llam_broker_wire_op_t)request.op) {
        case LLAM_BROKER_WIRE_OP_CREATE_BUFFER:
            request.slot = (i & 8U) != 0U ? 0U : ((uint64_t)LLAM_BROKER_BUFFER_MAX_BYTES + 1U);
            break;
        case LLAM_BROKER_WIRE_OP_CREATE_CHANNEL:
            request.slot = (i & 8U) != 0U ? 0U : ((uint64_t)LLAM_BROKER_CHANNEL_CAPACITY + 1U);
            break;
        case LLAM_BROKER_WIRE_OP_TASK_SPAWN:
            request.slot = (i & 8U) != 0U
                ? ((uint64_t)UINT32_MAX + 1U)
                : (uint64_t)LLAM_BROKER_TASK_KIND_RETURN_U64;
            break;
        case LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR:
            request.rights = (i & 8U) != 0U ? LLAM_BROKER_DESCRIPTOR_TRANSPORT_RIGHTS : 0U;
            break;
        case LLAM_BROKER_WIRE_OP_CREATE_RING:
            request.length = 0U;
            break;
        case LLAM_BROKER_WIRE_OP_SERVE_RING:
            request.slot = (i & 8U) != 0U ? 0U : ((uint64_t)LLAM_BROKER_RING_SESSIONS + 1U);
            request.length = (uint64_t)LLAM_BROKER_RING_SERVE_BATCH_MAX + 1U;
            break;
        default:
            break;
        }

        memset(&response, 0x5a, sizeof(response));
        llam_broker_process_request_with_descriptors(&broker,
                                                     &request,
                                                     &response,
                                                     &should_close,
                                                     LLAM_INVALID_HANDLE,
                                                     NULL);
        if (response.magic != LLAM_BROKER_WIRE_MAGIC ||
            response.version != LLAM_BROKER_WIRE_VERSION ||
            should_close ||
            !broker_failure_authority_outputs_are_clear(&response)) {
            fprintf(stderr,
                    "[test_security_capability] malformed broker op %u leaked authority "
                    "status=%d errno=%d should_close=%d iter=%zu\n",
                    request.op,
                    response.status,
                    response.error_code,
                    should_close ? 1 : 0,
                    i);
            goto done;
        }
    }

    rc = 0;

done:
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_direct_issue_clears_output_on_invalid_input(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_token_t token;
    int rc = -1;

    /*
     * Direct broker mint helpers are internal, but they still cross module and
     * FFI-test boundaries. Failed minting must not leave a stale authority token
     * in caller-owned storage that a later path might accidentally reuse.
     */
    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_broker_issue_object_cap(NULL,
                                                  LLAM_BROKER_CAP_FAMILY_BUFFER,
                                                  1U,
                                                  1U,
                                                  LLAM_CAP_RIGHT_READ,
                                                  &token),
                     EINVAL,
                     "broker direct issue accepted NULL broker") != 0) {
        return -1;
    }
    if (!memory_is_byte(&token, sizeof(token), 0U)) {
        fprintf(stderr, "[test_security_capability] failed direct issue left stale output token\n");
        return -1;
    }

    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_broker_issue_object_cap_unlocked(NULL,
                                                           LLAM_BROKER_CAP_FAMILY_BUFFER,
                                                           1U,
                                                           1U,
                                                           LLAM_CAP_RIGHT_READ,
                                                           &token),
                     EINVAL,
                     "broker direct unlocked issue accepted NULL broker") != 0) {
        return -1;
    }
    if (!memory_is_byte(&token, sizeof(token), 0U)) {
        fprintf(stderr, "[test_security_capability] failed unlocked direct issue left stale output token\n");
        return -1;
    }

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }

    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_broker_issue_object_cap(&broker,
                                                  LLAM_BROKER_CAP_FAMILY_BUFFER,
                                                  1U,
                                                  1U,
                                                  LLAM_CAP_RIGHT_SEND,
                                                  &token),
                     EACCES,
                     "broker direct issue accepted out-of-family rights") != 0 ||
        !memory_is_byte(&token, sizeof(token), 0U)) {
        goto done;
    }

    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_validate_requires_nonzero_rights(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    llam_broker_ring_t ring;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    llam_capability_token_t token;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_create_channel(&broker, 2U, LLAM_CAP_RIGHT_SEND, &token) != 0) {
        goto done;
    }

    errno = 0;
    if (expect_errno(llam_broker_validate_cap(&broker, &token, 0U),
                     EINVAL,
                     "broker accepted rightless direct validation") != 0) {
        goto done;
    }

    /*
     * Client-visible transports must also reject zero-right validation. A
     * token holder should prove a specific authority, not merely ask the
     * broker whether a serialized token is structurally live.
     */
    request_init(&request, LLAM_BROKER_WIRE_OP_VALIDATE_CAP);
    request.token = token;
    request.required_rights = 0U;
    llam_broker_process_request(&broker, &request, &response, NULL);
    if (response.status == 0 || response.error_code != EINVAL) {
        fprintf(stderr,
                "[test_security_capability] rightless transport validation accepted: status=%d errno=%d\n",
                response.status,
                response.error_code);
        goto done;
    }

    if (llam_broker_ring_init(&ring) != 0) {
        goto done;
    }
    memset(&submission, 0, sizeof(submission));
    submission.request_id = 41U;
    submission.op = LLAM_BROKER_RING_OP_CAP_VALIDATE;
    submission.arg0 = 0U;
    submission.token = token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status == 0 ||
        completion.error_code != EINVAL) {
        fprintf(stderr,
                "[test_security_capability] rightless ring validation accepted: status=%d errno=%d\n",
                completion.status,
                completion.error_code);
        goto done;
    }

    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_attenuate_capability(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_token_t token;
    llam_capability_token_t send_only;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_create_channel(&broker,
                                   2U,
                                   LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_RECV | LLAM_CAP_RIGHT_CLOSE,
                                   &token) != 0) {
        goto done;
    }
    if (llam_broker_attenuate_cap(&broker, &token, LLAM_CAP_RIGHT_SEND, &send_only) != 0 ||
        llam_broker_validate_cap(&broker, &send_only, LLAM_CAP_RIGHT_SEND) != 0) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_validate_cap(&broker, &send_only, LLAM_CAP_RIGHT_RECV),
                     EACCES,
                     "broker attenuation preserved dropped recv right") != 0) {
        goto done;
    }
    memset(&token, 0xa5, sizeof(token));
    errno = 0;
    if (expect_errno(llam_broker_attenuate_cap(&broker,
                                               &send_only,
                                               LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_DESTROY,
                                               &token),
                     EACCES,
                     "broker attenuation expanded rights") != 0) {
        goto done;
    }
    if (!memory_is_byte(&token, sizeof(token), 0U)) {
        fprintf(stderr, "[test_security_capability] failed broker attenuation left stale output token\n");
        goto done;
    }
    token = send_only;
    errno = 0;
    if (expect_errno(llam_broker_attenuate_cap(&broker,
                                               &token,
                                               LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_DESTROY,
                                               &token),
                     EACCES,
                     "in-place broker attenuation expanded rights") != 0) {
        goto done;
    }
    if (!memory_is_byte(&token, sizeof(token), 0U)) {
        fprintf(stderr, "[test_security_capability] failed in-place broker attenuation kept authority\n");
        goto done;
    }
    memset(&token, 0xa5, sizeof(token));
    (void)llam_broker_revoke_all(&broker);
    errno = 0;
    if (expect_errno(llam_broker_attenuate_cap(&broker, &send_only, LLAM_CAP_RIGHT_SEND, &token),
                     EACCES,
                     "broker attenuation accepted revoked token") != 0) {
        goto done;
    }
    if (!memory_is_byte(&token, sizeof(token), 0U)) {
        fprintf(stderr, "[test_security_capability] revoked broker attenuation left stale output token\n");
        goto done;
    }
    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_subject_bound_tokens(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_token_t token;
    llam_capability_token_t transformed;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }

    /*
     * Transport-issued tokens are bound to the current broker subject. This
     * models a control connection audience: replaying the serialized token on a
     * different connection must fail even though the token remains well-formed.
     */
    if (llam_broker_begin_op_subject(&broker, 101U) != 0) {
        goto done;
    }
    if (llam_broker_create_channel(&broker,
                                   2U,
                                   LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_DESTROY,
                                   &token) != 0) {
        llam_broker_end_op(&broker);
        goto done;
    }
    llam_broker_end_op(&broker);

    errno = 0;
    if (expect_errno(llam_broker_validate_cap(&broker, &token, LLAM_CAP_RIGHT_SEND),
                     EACCES,
                     "subject-bound broker token accepted without subject") != 0) {
        goto done;
    }
    if (llam_broker_begin_op_subject(&broker, 101U) != 0) {
        goto done;
    }
    if (llam_broker_validate_cap(&broker, &token, LLAM_CAP_RIGHT_SEND) != 0) {
        llam_broker_end_op(&broker);
        goto done;
    }
    llam_broker_end_op(&broker);

    if (llam_broker_begin_op_subject(&broker, 202U) != 0) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_validate_cap(&broker, &token, LLAM_CAP_RIGHT_SEND),
                     EACCES,
                     "subject-bound broker token accepted on another subject") != 0) {
        llam_broker_end_op(&broker);
        goto done;
    }
    if (expect_errno(llam_broker_attenuate_cap(&broker, &token, LLAM_CAP_RIGHT_SEND, &transformed),
                     EACCES,
                     "wrong subject attenuated a subject-bound token") != 0) {
        llam_broker_end_op(&broker);
        goto done;
    }
    if (expect_errno(llam_broker_revoke_object_cap(&broker, &token, LLAM_CAP_RIGHT_SEND, &transformed),
                     EACCES,
                     "wrong subject revoked a subject-bound token") != 0) {
        llam_broker_end_op(&broker);
        goto done;
    }
    llam_broker_end_op(&broker);
    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_nested_subject_scope_restores_outer(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }

    if (llam_broker_begin_op(&broker) != 0) {
        goto done_destroy;
    }
    if (llam_broker_current_subject(&broker) != 0U) {
        llam_broker_end_op(&broker);
        goto done_destroy;
    }
    if (llam_broker_begin_op_subject(&broker, 777U) != 0) {
        llam_broker_end_op(&broker);
        goto done_destroy;
    }
    if (llam_broker_current_subject(&broker) != 777U) {
        llam_broker_end_op(&broker);
        llam_broker_end_op(&broker);
        goto done_destroy;
    }
    llam_broker_end_op(&broker);

    /*
     * A transport/ring subject introduced by an inner operation must not leak
     * back into the already-running bearer operation. Otherwise a later bearer
     * helper could accidentally validate or mint authority for the wrong
     * broker session.
     */
    if (llam_broker_current_subject(&broker) != 0U) {
        llam_broker_end_op(&broker);
        goto done_destroy;
    }
    llam_broker_end_op(&broker);
    rc = 0;

done_destroy:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_nested_subject_conflict_preserves_outer(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }

    if (llam_broker_begin_op_subject(&broker, 101U) != 0) {
        goto done_destroy;
    }
    errno = 0;
    if (expect_errno(llam_broker_begin_op_subject(&broker, 202U),
                     EACCES,
                     "conflicting nested broker subject was accepted") != 0) {
        llam_broker_end_op(&broker);
        goto done_destroy;
    }
    if (llam_broker_current_subject(&broker) != 101U) {
        llam_broker_end_op(&broker);
        goto done_destroy;
    }
    llam_broker_end_op(&broker);
    rc = 0;

done_destroy:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_nested_subject_depth_overflow_preserves_scope(void) {
    const uint64_t subject = 4242U;
    /*
     * Keep this mirror explicit so a future broker TLS depth change has to
     * update the security regression that validates overflow fail-closed state.
     */
    const size_t tls_op_depth = 8U;
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    size_t opened = 0U;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }

    for (opened = 0U; opened < tls_op_depth; ++opened) {
        if (llam_broker_begin_op_subject(&broker, subject) != 0) {
            goto done_unwind;
        }
        if (llam_broker_current_subject(&broker) != subject) {
            ++opened;
            goto done_unwind;
        }
    }

    errno = 0;
    if (expect_errno(llam_broker_begin_op_subject(&broker, subject),
                     EOVERFLOW,
                     "broker subject stack overflow was accepted") != 0) {
        goto done_unwind;
    }
    if (llam_broker_current_subject(&broker) != subject) {
        goto done_unwind;
    }

    while (opened > 0U) {
        llam_broker_end_op(&broker);
        --opened;
        if (llam_broker_current_subject(&broker) != (opened > 0U ? subject : 0U)) {
            goto done_unwind;
        }
    }

    /*
     * The failed overflow attempt must not poison the broker TLS slot or leak
     * active-op state; a new operation should work after the stack is unwound.
     */
    if (llam_broker_begin_op_subject(&broker, subject) != 0) {
        goto done_unwind;
    }
    opened = 1U;
    if (llam_broker_current_subject(&broker) != subject) {
        goto done_unwind;
    }
    llam_broker_end_op(&broker);
    opened = 0U;
    rc = 0;

done_unwind:
    while (opened > 0U) {
        llam_broker_end_op(&broker);
        --opened;
    }
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_object_revocation_rotates_generation(void) {
    static const char initial[] = "revocable";
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_token_t token;
    llam_capability_token_t replacement;
    char out[sizeof(initial)];
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_register_buffer(&broker,
                                    initial,
                                    sizeof(initial),
                                    LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_WRITE | LLAM_CAP_RIGHT_DESTROY,
                                    &token) != 0) {
        goto done;
    }

    errno = 0;
    memset(&replacement, 0xa5, sizeof(replacement));
    if (expect_errno(llam_broker_revoke_object_cap(&broker, &token, LLAM_CAP_RIGHT_SEND, &replacement),
                     EACCES,
                     "invalid replacement rights revoked object") != 0) {
        goto done;
    }
    if (!memory_is_byte(&replacement, sizeof(replacement), 0U)) {
        fprintf(stderr, "[test_security_capability] failed broker revoke left stale replacement token\n");
        goto done;
    }
    if (llam_broker_read_buffer(&broker, &token, 0U, out, sizeof(out)) != 0 ||
        memcmp(out, initial, sizeof(initial)) != 0) {
        goto done;
    }
    replacement = token;
    errno = 0;
    if (expect_errno(llam_broker_revoke_object_cap(&broker, &replacement, LLAM_CAP_RIGHT_SEND, &replacement),
                     EACCES,
                     "in-place invalid broker revoke kept stale authority") != 0) {
        goto done;
    }
    if (!memory_is_byte(&replacement, sizeof(replacement), 0U)) {
        fprintf(stderr, "[test_security_capability] failed in-place broker revoke kept authority\n");
        goto done;
    }
    if (llam_broker_read_buffer(&broker, &token, 0U, out, sizeof(out)) != 0 ||
        memcmp(out, initial, sizeof(initial)) != 0) {
        goto done;
    }

    llam_capability_test_force_entropy_failure(true);
    errno = 0;
    memset(&replacement, 0xa5, sizeof(replacement));
    if (expect_errno(llam_broker_revoke_object_cap(&broker, &token, LLAM_CAP_RIGHT_READ, &replacement),
                     EIO,
                     "entropy-failed broker revoke reported wrong error") != 0) {
        llam_capability_test_force_entropy_failure(false);
        goto done;
    }
    llam_capability_test_force_entropy_failure(false);
    if (!memory_is_byte(&replacement, sizeof(replacement), 0U)) {
        fprintf(stderr, "[test_security_capability] entropy-failed broker revoke left stale replacement token\n");
        goto done;
    }
    if (llam_broker_validate_cap(&broker, &token, LLAM_CAP_RIGHT_READ) != 0 ||
        llam_broker_read_buffer(&broker, &token, 0U, out, sizeof(out)) != 0 ||
        memcmp(out, initial, sizeof(initial)) != 0) {
        fprintf(stderr, "[test_security_capability] failed broker revoke mutated object generation\n");
        goto done;
    }

    if (llam_broker_revoke_object_cap(&broker, &token, LLAM_CAP_RIGHT_READ, &replacement) != 0) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_validate_cap(&broker, &token, LLAM_CAP_RIGHT_READ),
                     EACCES,
                     "old token still validated after object generation rotation") != 0) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_read_buffer(&broker, &token, 0U, out, sizeof(out)),
                     EACCES,
                     "old token survived object generation rotation") != 0) {
        goto done;
    }
    if (llam_broker_validate_cap(&broker, &replacement, LLAM_CAP_RIGHT_READ) != 0) {
        goto done;
    }
    if (llam_broker_read_buffer(&broker, &replacement, 0U, out, sizeof(out)) != 0 ||
        memcmp(out, initial, sizeof(initial)) != 0) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_write_buffer(&broker, &replacement, 0U, initial, sizeof(initial)),
                     EACCES,
                     "replacement token kept dropped write right") != 0) {
        goto done;
    }
    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_destroy_drains_unjoined_task_slots(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_token_t token;
    size_t i;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_spawn_task(&broker,
                               LLAM_BROKER_TASK_KIND_SLEEP_NS_RETURN_U64,
                               1000U,
                               LLAM_CAP_RIGHT_JOIN | LLAM_CAP_RIGHT_DETACH,
                               &token) != 0) {
        llam_broker_destroy(&broker);
        return -1;
    }
    if (llam_broker_detach_task(&broker, &token) != 0) {
        llam_broker_destroy(&broker);
        return -1;
    }

    /*
     * Detached broker task slots still carry trampoline arguments until the
     * runtime executes the task. Destroy must drain that runtime work before it
     * invalidates broker-private slot storage and synchronization primitives.
     */
    llam_broker_destroy(&broker);
    if (broker.initialized || broker.runtime != NULL || broker.lock_initialized) {
        return -1;
    }
    for (i = 0U; i < LLAM_BROKER_TASK_SLOTS; ++i) {
        if (broker.tasks[i].active ||
            broker.tasks[i].task != NULL ||
            atomic_load_explicit(&broker.tasks[i].state, memory_order_acquire) !=
                LLAM_BROKER_TASK_STATE_EMPTY) {
            return -1;
        }
    }
    return 0;
}

static int test_broker_destroy_cancels_sleeping_task_slots(void) {
    const uint64_t sleep_ns = 2ULL * 1000ULL * 1000ULL * 1000ULL;
#if LLAM_TEST_SANITIZER_BUILD
    const uint64_t max_destroy_ns = 1500ULL * 1000ULL * 1000ULL;
#else
    const uint64_t max_destroy_ns = 1000ULL * 1000ULL * 1000ULL;
#endif
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_token_t token;
    uint64_t start_ns;
    uint64_t elapsed_ns;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_spawn_task(&broker,
                               LLAM_BROKER_TASK_KIND_SLEEP_NS_RETURN_U64,
                               sleep_ns,
                               LLAM_CAP_RIGHT_JOIN | LLAM_CAP_RIGHT_DETACH,
                               &token) != 0) {
        llam_broker_destroy(&broker);
        return -1;
    }

    /*
     * Broker destroy must be bounded even if a client leaves predefined work in
     * a long cancellable sleep. Otherwise a client can create a shutdown-delay
     * denial of service by spawning a sleeping command and disconnecting.
     */
    start_ns = llam_now_ns();
    llam_broker_destroy(&broker);
    elapsed_ns = llam_now_ns() - start_ns;
    if (elapsed_ns > max_destroy_ns) {
        fprintf(stderr,
                "[test_security_capability] broker destroy waited %.3fs for sleeping task\n",
                (double)elapsed_ns / 1000000000.0);
        return -1;
    }
    if (broker.initialized || broker.runtime != NULL || broker.lock_initialized) {
        return -1;
    }
    (void)token;
    return 0;
}

static int test_broker_failed_task_join_consumes_slot(void) {
    const uint64_t sleep_ns = 2ULL * 1000ULL * 1000ULL * 1000ULL;
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_token_t token;
    uint64_t result = UINT64_C(0xfeedfacecafebeef);
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_spawn_task(&broker,
                               LLAM_BROKER_TASK_KIND_SLEEP_NS_RETURN_U64,
                               sleep_ns,
                               LLAM_CAP_RIGHT_JOIN,
                               &token) != 0) {
        goto done;
    }
    /*
     * A command error is terminal. Joining it must still consume the broker
     * task handle; otherwise a client can fill the bounded task table with
     * permanently failed tasks that report the same error forever.
     */
    if (llam_runtime_request_stop_rt(broker.runtime) != 0 ||
        llam_runtime_run_handle(broker.runtime) != 0) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_join_task(&broker, &token, &result),
                     ECANCELED,
                     "failed broker task join did not surface command cancellation") != 0 ||
        result != 0U ||
        broker_active_task_count(&broker) != 0U) {
        fprintf(stderr,
                "[test_security_capability] failed task join left result=%" PRIu64 " active_tasks=%zu\n",
                result,
                broker_active_task_count(&broker));
        goto done;
    }
    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

#if !LLAM_PLATFORM_WINDOWS
typedef struct broker_wire_join_thread_state {
    llam_broker_t *broker;
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    atomic_int done;
} broker_wire_join_thread_state_t;

static void *broker_wire_join_thread(void *arg) {
    broker_wire_join_thread_state_t *state = (broker_wire_join_thread_state_t *)arg;

    llam_broker_process_request_with_descriptors(state->broker,
                                                 &state->request,
                                                 &state->response,
                                                 NULL,
                                                 LLAM_INVALID_HANDLE,
                                                 NULL);
    atomic_store_explicit(&state->done, 1, memory_order_release);
    return NULL;
}

static bool broker_wait_for_atomic_done(atomic_int *done, uint64_t timeout_ns) {
    uint64_t start = llam_now_ns();

    while (llam_now_ns() - start < timeout_ns) {
        if (atomic_load_explicit(done, memory_order_acquire) != 0) {
            return true;
        }
        usleep(1000U);
    }
    return atomic_load_explicit(done, memory_order_acquire) != 0;
}

static int test_broker_wire_task_join_sleep_returns_eagain(void) {
    const uint64_t sleep_ns = 2ULL * 1000ULL * 1000ULL * 1000ULL;
    const uint64_t max_join_wait_ns = 100ULL * 1000ULL * 1000ULL;
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    broker_wire_join_thread_state_t join_state;
    pthread_t join_thread;
    bool broker_initialized = false;
    bool join_started = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;

    request_init(&request, LLAM_BROKER_WIRE_OP_TASK_SPAWN);
    request.slot = LLAM_BROKER_TASK_KIND_SLEEP_NS_RETURN_U64;
    request.offset = sleep_ns;
    request.rights = LLAM_CAP_RIGHT_JOIN | LLAM_CAP_RIGHT_DETACH;
    llam_broker_process_request_with_descriptors(&broker,
                                                 &request,
                                                 &response,
                                                 NULL,
                                                 LLAM_INVALID_HANDLE,
                                                 NULL);
    if (response.status != 0 ||
        response.token.family != LLAM_BROKER_CAP_FAMILY_TASK) {
        goto done;
    }

    memset(&join_state, 0, sizeof(join_state));
    join_state.broker = &broker;
    request_init(&join_state.request, LLAM_BROKER_WIRE_OP_TASK_JOIN);
    join_state.request.token = response.token;
    atomic_init(&join_state.done, 0);
    if (pthread_create(&join_thread, NULL, broker_wire_join_thread, &join_state) != 0) {
        goto done;
    }
    join_started = true;

    /*
     * Transport request handlers are broker control-plane work.  Joining an
     * uncompleted long sleep command must not drive the runtime to completion,
     * otherwise one client can pin the broker's serve thread for the sleep
     * duration.  The safe contract is a prompt EAGAIN so clients can retry.
     */
    if (!broker_wait_for_atomic_done(&join_state.done, max_join_wait_ns)) {
        (void)llam_runtime_request_stop_rt(broker.runtime);
        (void)pthread_join(join_thread, NULL);
        join_started = false;
        fprintf(stderr,
                "[test_security_capability] wire TASK_JOIN blocked on long sleep task\n");
        goto done;
    }
    (void)pthread_join(join_thread, NULL);
    join_started = false;
    if (join_state.response.status == 0 ||
        join_state.response.error_code != EAGAIN ||
        join_state.response.result0 != 0U) {
        fprintf(stderr,
                "[test_security_capability] wire TASK_JOIN sleep response status=%d errno=%d result=%" PRIu64 "\n",
                join_state.response.status,
                join_state.response.error_code,
                join_state.response.result0);
        goto done;
    }
    rc = 0;

done:
    if (join_started) {
        (void)llam_runtime_request_stop_rt(broker.runtime);
        (void)pthread_join(join_thread, NULL);
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_wire_task_join_does_not_drain_peer_sleep(void) {
    const uint64_t sleep_ns = 2ULL * 1000ULL * 1000ULL * 1000ULL;
    const uint64_t max_join_wait_ns = 100ULL * 1000ULL * 1000ULL;
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    llam_capability_token_t quick_token;
    broker_wire_join_thread_state_t join_state;
    pthread_t join_thread;
    bool broker_initialized = false;
    bool join_started = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;

    request_init(&request, LLAM_BROKER_WIRE_OP_TASK_SPAWN);
    request.slot = LLAM_BROKER_TASK_KIND_SLEEP_NS_RETURN_U64;
    request.offset = sleep_ns;
    request.rights = LLAM_CAP_RIGHT_JOIN | LLAM_CAP_RIGHT_DETACH;
    llam_broker_process_request_with_descriptors(&broker,
                                                 &request,
                                                 &response,
                                                 NULL,
                                                 LLAM_INVALID_HANDLE,
                                                 NULL);
    if (response.status != 0 ||
        response.token.family != LLAM_BROKER_CAP_FAMILY_TASK) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_TASK_SPAWN);
    request.slot = LLAM_BROKER_TASK_KIND_POPCOUNT_U64;
    request.offset = UINT64_C(0xf0f0f00f);
    request.rights = LLAM_CAP_RIGHT_JOIN | LLAM_CAP_RIGHT_DETACH;
    llam_broker_process_request_with_descriptors(&broker,
                                                 &request,
                                                 &response,
                                                 NULL,
                                                 LLAM_INVALID_HANDLE,
                                                 NULL);
    if (response.status != 0 ||
        response.token.family != LLAM_BROKER_CAP_FAMILY_TASK) {
        goto done;
    }
    quick_token = response.token;

    memset(&join_state, 0, sizeof(join_state));
    join_state.broker = &broker;
    request_init(&join_state.request, LLAM_BROKER_WIRE_OP_TASK_JOIN);
    join_state.request.token = quick_token;
    atomic_init(&join_state.done, 0);
    if (pthread_create(&join_thread, NULL, broker_wire_join_thread, &join_state) != 0) {
        goto done;
    }
    join_started = true;

    /*
     * Synchronous wire joins may only opportunistically drive short work when
     * doing so cannot also wait out another client's long sleeping command in
     * the same broker runtime. Otherwise a cheap quick-task join becomes a
     * control-plane delay primitive.
     */
    if (!broker_wait_for_atomic_done(&join_state.done, max_join_wait_ns)) {
        (void)llam_runtime_request_stop_rt(broker.runtime);
        (void)pthread_join(join_thread, NULL);
        join_started = false;
        fprintf(stderr,
                "[test_security_capability] quick TASK_JOIN drained peer sleep task\n");
        goto done;
    }
    (void)pthread_join(join_thread, NULL);
    join_started = false;
    if (join_state.response.status == 0 ||
        join_state.response.error_code != EAGAIN ||
        join_state.response.result0 != 0U) {
        fprintf(stderr,
                "[test_security_capability] quick TASK_JOIN with peer sleep status=%d errno=%d result=%" PRIu64 "\n",
                join_state.response.status,
                join_state.response.error_code,
                join_state.response.result0);
        goto done;
    }
    rc = 0;

done:
    if (join_started) {
        (void)llam_runtime_request_stop_rt(broker.runtime);
        (void)pthread_join(join_thread, NULL);
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}
#endif

static int test_broker_rejects_foreign_runtime_token(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_object_t object;
    llam_capability_token_t valid_token;
    llam_capability_token_t token;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_issue_object_cap(&broker,
                                     LLAM_BROKER_CAP_FAMILY_BUFFER,
                                     1U,
                                     1U,
                                     LLAM_CAP_RIGHT_READ,
                                     &valid_token) != 0) {
        llam_broker_destroy(&broker);
        return -1;
    }
    memset(&object, 0, sizeof(object));
    object.runtime_id = valid_token.runtime_id + 1U;
    if (object.runtime_id == 0U) {
        object.runtime_id = valid_token.runtime_id + 2U;
    }
    object.family = LLAM_BROKER_CAP_FAMILY_BUFFER;
    object.slot = 1U;
    object.generation = 1U;
    object.revocation_epoch = llam_broker_revocation_epoch(&broker);

    /*
     * Internal tests can mint a token with the broker key to prove that MAC
     * validity alone is not enough: validation must also bind the token to the
     * live broker runtime instance.
     */
    if (llam_capability_issue(&broker.capability_key, &object, LLAM_CAP_RIGHT_READ, &token) != 0) {
        llam_broker_destroy(&broker);
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_broker_validate_cap(&broker, &token, LLAM_CAP_RIGHT_READ),
                     EACCES,
                     "foreign-runtime broker token accepted") != 0) {
        llam_broker_destroy(&broker);
        return -1;
    }
    llam_broker_destroy(&broker);
    return 0;
}

static int test_broker_failure_marker_clears_authority_outputs(void) {
    llam_broker_wire_response_t response;

    memset(&response, 0, sizeof(response));
    response.magic = LLAM_BROKER_WIRE_MAGIC;
    response.version = LLAM_BROKER_WIRE_VERSION;
    response.status = 0;
    response.error_code = 0;
    response.runtime_id = UINT64_C(0x1111222233334444);
    response.revocation_epoch = UINT64_C(0x5555666677778888);
    response.result0 = UINT64_C(0x0102030405060708);
    response.result1 = UINT64_C(0x1112131415161718);
    response.result2 = UINT64_C(0x2122232425262728);
    memset(&response.token, 0xa5, sizeof(response.token));
    memset(response.data, 0x5a, sizeof(response.data));

    llam_broker_mark_response_failure_clear_outputs(&response, EPIPE);
    if (response.magic != LLAM_BROKER_WIRE_MAGIC ||
        response.version != LLAM_BROKER_WIRE_VERSION ||
        response.status != -1 ||
        response.error_code != EPIPE ||
        response.runtime_id != UINT64_C(0x1111222233334444) ||
        response.revocation_epoch != UINT64_C(0x5555666677778888) ||
        response.result0 != 0U ||
        response.result1 != 0U ||
        response.result2 != 0U ||
        !memory_is_byte(&response.token, sizeof(response.token), 0U) ||
        !memory_is_byte(response.data, sizeof(response.data), 0U)) {
        fprintf(stderr, "[test_security_capability] broker failure marker leaked authority outputs\n");
        return -1;
    }

    llam_broker_mark_response_failure_clear_outputs(&response, 0);
    if (response.status != -1 || response.error_code != EIO) {
        fprintf(stderr, "[test_security_capability] broker failure marker did not normalize zero errno\n");
        return -1;
    }

    return 0;
}

static int test_broker_ring_private_name_clears_short_output(void) {
    char name[4];

    memset(name, 0x5a, sizeof(name));
    errno = 0;
    if (expect_errno(llam_broker_ring_private_name(name, sizeof(name)),
                     ENAMETOOLONG,
                     "short broker ring private-name buffer did not fail with ENAMETOOLONG") != 0) {
        return -1;
    }
    if (!memory_is_byte(name, sizeof(name), 0U)) {
        fprintf(stderr, "[test_security_capability] failed private-name generation left partial output\n");
        return -1;
    }
    return 0;
}

static int test_broker_ring_unmap_handles_unterminated_name(void) {
#if !LLAM_PLATFORM_WINDOWS
    llam_broker_ring_mapping_t mapping;

    memset(&mapping, 0, sizeof(mapping));
    mapping.fd = -1;
    mapping.mapping_handle = LLAM_INVALID_HANDLE;
    mapping.owner = false;
    memset(mapping.name, 'x', sizeof(mapping.name));

    /*
     * Cleanup paths must treat the fixed-size name as bounded storage. A
     * partially initialized mapping may not contain a trailing NUL byte.
     */
    llam_broker_ring_unmap(&mapping);
    if (!broker_ring_mapping_is_reset(&mapping)) {
        fprintf(stderr, "[test_security_capability] ring unmap did not reset unterminated mapping\n");
        return -1;
    }
#endif
    return 0;
}

static int test_broker_control_outputs_clear_on_invalid_input(void) {
    llam_broker_t uninitialized_broker;
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    uint64_t subject_id = UINT64_C(0x1111222233334444);
    uint64_t session_id = UINT64_C(0x5555666677778888);
    uint64_t task_result = UINT64_C(0x9999aaaabbbbcccc);
    llam_handle_t descriptor = (llam_handle_t)123;
    size_t count = 17U;
    size_t oversized_count;
    llam_broker_ring_mapping_t mapping;
    llam_broker_ring_stats_t stats;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    llam_broker_ring_completion_t completions[2];

    request_init(&request, LLAM_BROKER_WIRE_OP_PING);
    memset(&response, 0x5a, sizeof(response));
    llam_broker_process_request(NULL, &request, &response, NULL);
    if (response.magic != LLAM_BROKER_WIRE_MAGIC ||
        response.version != LLAM_BROKER_WIRE_VERSION ||
        response.status == 0 ||
        response.error_code != EINVAL ||
        response.result0 != 0U ||
        response.result1 != 0U ||
        response.result2 != 0U ||
        !memory_is_byte(&response.token, sizeof(response.token), 0U) ||
        !memory_is_byte(response.data, sizeof(response.data), 0U)) {
        fprintf(stderr, "[test_security_capability] invalid broker context accepted control request\n");
        return -1;
    }

    memset(&uninitialized_broker, 0, sizeof(uninitialized_broker));
    request_init(&request, LLAM_BROKER_WIRE_OP_PING);
    memset(&response, 0x5a, sizeof(response));
    llam_broker_process_request(&uninitialized_broker, &request, &response, NULL);
    if (response.magic != LLAM_BROKER_WIRE_MAGIC ||
        response.version != LLAM_BROKER_WIRE_VERSION ||
        response.status == 0 ||
        response.error_code != EINVAL ||
        response.result0 != 0U ||
        response.result1 != 0U ||
        response.result2 != 0U ||
        !memory_is_byte(&response.token, sizeof(response.token), 0U) ||
        !memory_is_byte(response.data, sizeof(response.data), 0U)) {
        fprintf(stderr, "[test_security_capability] uninitialized broker accepted control request\n");
        return -1;
    }

    errno = 0;
    if (expect_errno(llam_broker_transport_subject(NULL, UINT64_C(1), &subject_id),
                     EINVAL,
                     "invalid broker transport subject did not fail with EINVAL") != 0 ||
        subject_id != 0U) {
        fprintf(stderr, "[test_security_capability] failed broker subject lookup left stale subject id\n");
        return -1;
    }

    errno = 0;
    if (expect_errno(llam_broker_ring_register_mapping(NULL, NULL, UINT64_C(1), &session_id),
                     EINVAL,
                     "invalid broker ring mapping registration did not fail with EINVAL") != 0 ||
        session_id != 0U) {
        fprintf(stderr, "[test_security_capability] failed ring mapping registration left stale session id\n");
        return -1;
    }

    count = 17U;
    errno = 0;
    if (expect_errno(llam_broker_ring_serve_batch_subject(NULL, NULL, UINT64_C(1), 1U, &count),
                     EINVAL,
                     "invalid broker ring batch serve did not fail with EINVAL") != 0 ||
        count != 0U) {
        fprintf(stderr, "[test_security_capability] failed ring batch serve left stale served count\n");
        return -1;
    }

    errno = 0;
    if (expect_errno(llam_broker_transport_create_ring(NULL, &descriptor, &session_id),
                     EINVAL,
                     "invalid broker ring transport creation did not fail with EINVAL") != 0 ||
        !LLAM_HANDLE_IS_INVALID(descriptor) ||
        session_id != 0U) {
        fprintf(stderr, "[test_security_capability] failed ring transport creation left stale authority outputs\n");
        return -1;
    }

    errno = 0;
    if (expect_errno(llam_broker_join_task(NULL, NULL, &task_result),
                     EINVAL,
                     "invalid broker task join did not fail with EINVAL") != 0 ||
        task_result != 0U) {
        fprintf(stderr, "[test_security_capability] failed task join left stale result output\n");
        return -1;
    }

    memset(&mapping, 0x5a, sizeof(mapping));
    mapping.fd = 123;
    mapping.mapping_handle = (llam_handle_t)123;
    errno = 0;
    if (expect_errno(llam_broker_ring_create_shm(NULL, &mapping),
                     EINVAL,
                     "invalid broker ring shm creation did not fail with EINVAL") != 0 ||
        !broker_ring_mapping_is_reset(&mapping)) {
        fprintf(stderr, "[test_security_capability] failed ring shm creation left stale mapping output\n");
        return -1;
    }

    memset(&mapping, 0x5a, sizeof(mapping));
    mapping.fd = 123;
    mapping.mapping_handle = (llam_handle_t)123;
    errno = 0;
    if (expect_errno(llam_broker_ring_open_shm(NULL, &mapping),
                     EINVAL,
                     "invalid broker ring shm open did not fail with EINVAL") != 0 ||
        !broker_ring_mapping_is_reset(&mapping)) {
        fprintf(stderr, "[test_security_capability] failed ring shm open left stale mapping output\n");
        return -1;
    }

    memset(&mapping, 0x5a, sizeof(mapping));
    mapping.fd = 123;
    mapping.mapping_handle = (llam_handle_t)123;
    errno = 0;
#if LLAM_PLATFORM_WINDOWS
    if (expect_errno(llam_broker_ring_map_handle(LLAM_INVALID_HANDLE, false, &mapping),
                     EINVAL,
                     "invalid broker ring handle mapping did not fail with EINVAL") != 0 ||
        !broker_ring_mapping_is_reset(&mapping)) {
#else
    if (expect_errno(llam_broker_ring_map_fd(-1, false, &mapping),
                     EINVAL,
                     "invalid broker ring fd mapping did not fail with EINVAL") != 0 ||
        !broker_ring_mapping_is_reset(&mapping)) {
#endif
        fprintf(stderr, "[test_security_capability] failed ring mapping import left stale mapping output\n");
        return -1;
    }

#if LLAM_PLATFORM_WINDOWS
    {
        HANDLE event_handle = CreateEventA(NULL, TRUE, FALSE, NULL);
        bool leaked = false;

        if (event_handle == NULL) {
            fprintf(stderr, "[test_security_capability] failed to create invalid mapping handle fixture\n");
            return -1;
        }
        memset(&mapping, 0x5a, sizeof(mapping));
        errno = 0;
        if (expect_errno(llam_broker_ring_map_handle((llam_handle_t)event_handle, true, &mapping),
                         EINVAL,
                         "owned invalid broker ring handle mapping did not fail with EINVAL") != 0 ||
            !broker_ring_mapping_is_reset(&mapping)) {
            CloseHandle(event_handle);
            return -1;
        }
        if (WaitForSingleObject(event_handle, 0U) != WAIT_FAILED ||
            GetLastError() != ERROR_INVALID_HANDLE) {
            leaked = true;
            CloseHandle(event_handle);
        }
        if (leaked) {
            fprintf(stderr, "[test_security_capability] failed owned ring mapping left HANDLE open\n");
            return -1;
        }
    }
#else
    {
        char path[] = "/tmp/llam-broker-ring-small-XXXXXX";
        int fd = mkstemp(path);
        bool leaked = false;

        if (fd < 0) {
            fprintf(stderr, "[test_security_capability] failed to create invalid mapping fd fixture\n");
            return -1;
        }
        (void)unlink(path);
        memset(&mapping, 0x5a, sizeof(mapping));
        errno = 0;
        if (expect_errno(llam_broker_ring_map_fd(fd, true, &mapping),
                         EINVAL,
                         "owned invalid broker ring fd mapping did not fail with EINVAL") != 0 ||
            !broker_ring_mapping_is_reset(&mapping)) {
            close(fd);
            return -1;
        }
        leaked = !broker_fd_is_closed(fd);
        if (leaked) {
            close(fd);
            fprintf(stderr, "[test_security_capability] failed owned ring mapping left fd open\n");
            return -1;
        }
    }
#endif

    memset(&stats, 0x5a, sizeof(stats));
    errno = 0;
    if (expect_errno(llam_broker_ring_collect_stats(NULL, &stats),
                     EINVAL,
                     "invalid broker ring stats collection did not fail with EINVAL") != 0 ||
        !memory_is_byte(&stats, sizeof(stats), 0U)) {
        fprintf(stderr, "[test_security_capability] failed ring stats collection left stale stats output\n");
        return -1;
    }

    memset(&submission, 0x5a, sizeof(submission));
    errno = 0;
    if (expect_errno(llam_broker_ring_submit_pop(NULL, &submission),
                     EINVAL,
                     "invalid broker ring submit pop did not fail with EINVAL") != 0 ||
        !memory_is_byte(&submission, sizeof(submission), 0U)) {
        fprintf(stderr, "[test_security_capability] failed ring submit pop left stale submission output\n");
        return -1;
    }

    memset(&completion, 0x5a, sizeof(completion));
    errno = 0;
    if (expect_errno(llam_broker_ring_complete_pop(NULL, &completion),
                     EINVAL,
                     "invalid broker ring complete pop did not fail with EINVAL") != 0 ||
        !memory_is_byte(&completion, sizeof(completion), 0U)) {
        fprintf(stderr, "[test_security_capability] failed ring complete pop left stale completion output\n");
        return -1;
    }

    memset(completions, 0x5a, sizeof(completions));
    errno = 0;
    if (expect_errno(llam_broker_ring_complete_drain(NULL, completions, 2U, &count),
                     EINVAL,
                     "invalid broker ring complete drain did not fail with EINVAL") != 0 ||
        count != 0U ||
        !memory_is_byte(completions, sizeof(completions), 0U)) {
        fprintf(stderr, "[test_security_capability] failed ring complete drain left stale outputs\n");
        return -1;
    }

    /*
     * Output scrubbing must fail closed before computing the memset size. A
     * hostile FFI caller can provide a nonsensical element count even when the
     * pointer itself is non-NULL.
     */
    memset(completions, 0x5a, sizeof(completions));
    count = 17U;
    oversized_count = SIZE_MAX / sizeof(completions[0]) + 1U;
    errno = 0;
    if (expect_errno(llam_broker_ring_complete_drain(NULL, completions, oversized_count, &count),
                     EOVERFLOW,
                     "oversized broker ring complete drain did not fail with EOVERFLOW") != 0 ||
        count != 0U ||
        !memory_is_byte(&completions[0], sizeof(completions[0]), 0U)) {
        fprintf(stderr, "[test_security_capability] oversized ring complete drain left stale first output\n");
        return -1;
    }

    return 0;
}

#if !LLAM_PLATFORM_WINDOWS
#define BROKER_CONCURRENT_SENDERS 4U
#define BROKER_CONCURRENT_MESSAGES 512U
#define BROKER_CONCURRENT_REVOKES 256U
#define BROKER_CONCURRENT_VALIDATIONS 4096U

typedef struct broker_concurrent_channel_state {
    llam_broker_t *broker;
    llam_capability_token_t token;
    atomic_uint completed_senders;
    atomic_int error_code;
} broker_concurrent_channel_state_t;

static void *broker_concurrent_channel_sender(void *arg) {
    broker_concurrent_channel_state_t *state = (broker_concurrent_channel_state_t *)arg;
    unsigned i;

    for (i = 0U; i < BROKER_CONCURRENT_MESSAGES; ++i) {
        unsigned char message[2];

        message[0] = (unsigned char)(i & 0xffU);
        message[1] = (unsigned char)((i >> 8U) & 0xffU);
        for (;;) {
            if (llam_broker_channel_send(state->broker, &state->token, message, sizeof(message)) == 0) {
                break;
            }
            if (errno != EAGAIN) {
                atomic_store_explicit(&state->error_code, errno == 0 ? EINVAL : errno, memory_order_release);
                atomic_fetch_add_explicit(&state->completed_senders, 1U, memory_order_acq_rel);
                return NULL;
            }
            if (atomic_load_explicit(&state->error_code, memory_order_acquire) != 0) {
                atomic_fetch_add_explicit(&state->completed_senders, 1U, memory_order_acq_rel);
                return NULL;
            }
            sched_yield();
        }
    }
    atomic_fetch_add_explicit(&state->completed_senders, 1U, memory_order_acq_rel);
    return NULL;
}

static void *broker_concurrent_channel_receiver(void *arg) {
    broker_concurrent_channel_state_t *state = (broker_concurrent_channel_state_t *)arg;
    unsigned total = BROKER_CONCURRENT_SENDERS * BROKER_CONCURRENT_MESSAGES;
    unsigned received = 0U;

    while (received < total) {
        unsigned char message[2];
        ssize_t nread = llam_broker_channel_recv(state->broker, &state->token, message, sizeof(message));

        if (nread == (ssize_t)sizeof(message)) {
            received += 1U;
            continue;
        }
        if (nread < 0 && errno == EAGAIN) {
            if (atomic_load_explicit(&state->error_code, memory_order_acquire) != 0) {
                return NULL;
            }
            sched_yield();
            continue;
        }
        atomic_store_explicit(&state->error_code, errno == 0 ? EINVAL : errno, memory_order_release);
        return NULL;
    }
    return NULL;
}

static int test_broker_concurrent_channel_state(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    broker_concurrent_channel_state_t state;
    pthread_t senders[BROKER_CONCURRENT_SENDERS];
    pthread_t receiver;
    unsigned i;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    memset(&state, 0, sizeof(state));
    state.broker = &broker;
    atomic_init(&state.completed_senders, 0U);
    atomic_init(&state.error_code, 0);
    if (llam_broker_create_channel(&broker,
                                   8U,
                                   LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_RECV | LLAM_CAP_RIGHT_CLOSE,
                                   &state.token) != 0) {
        goto done_destroy;
    }
    if (pthread_create(&receiver, NULL, broker_concurrent_channel_receiver, &state) != 0) {
        goto done_destroy;
    }
    for (i = 0U; i < BROKER_CONCURRENT_SENDERS; ++i) {
        if (pthread_create(&senders[i], NULL, broker_concurrent_channel_sender, &state) != 0) {
            atomic_store_explicit(&state.error_code, EINVAL, memory_order_release);
            break;
        }
    }
    for (unsigned j = 0U; j < i; ++j) {
        (void)pthread_join(senders[j], NULL);
    }
    (void)pthread_join(receiver, NULL);
    if (atomic_load_explicit(&state.error_code, memory_order_acquire) == 0 &&
        atomic_load_explicit(&state.completed_senders, memory_order_acquire) == BROKER_CONCURRENT_SENDERS) {
        rc = 0;
    }

done_destroy:
    llam_broker_destroy(&broker);
    return rc;
}

typedef struct broker_validate_revoke_state {
    llam_broker_t *broker;
    llam_capability_token_t stale_token;
    llam_capability_token_t revoker_token;
    atomic_bool start;
    atomic_int error_code;
} broker_validate_revoke_state_t;

static void *broker_revoke_worker(void *arg) {
    broker_validate_revoke_state_t *state = (broker_validate_revoke_state_t *)arg;
    llam_capability_token_t current = state->revoker_token;
    unsigned i;

    while (!atomic_load_explicit(&state->start, memory_order_acquire)) {
        sched_yield();
    }
    for (i = 0U; i < BROKER_CONCURRENT_REVOKES; ++i) {
        llam_capability_token_t replacement;

        if (llam_broker_revoke_object_cap(state->broker,
                                          &current,
                                          LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_DESTROY,
                                          &replacement) != 0) {
            atomic_store_explicit(&state->error_code, errno == 0 ? EINVAL : errno, memory_order_release);
            return NULL;
        }
        current = replacement;
    }
    return NULL;
}

static void *broker_validate_worker(void *arg) {
    broker_validate_revoke_state_t *state = (broker_validate_revoke_state_t *)arg;
    unsigned i;

    while (!atomic_load_explicit(&state->start, memory_order_acquire)) {
        sched_yield();
    }
    for (i = 0U; i < BROKER_CONCURRENT_VALIDATIONS; ++i) {
        int rc = llam_broker_validate_cap(state->broker, &state->stale_token, LLAM_CAP_RIGHT_SEND);

        /*
         * The stale token may validate before the first generation rotation and
         * must fail afterwards. TSan covers the security-critical invariant:
         * public validation must not race with revoke-time slot mutation.
         */
        if (rc != 0 && errno != EACCES) {
            atomic_store_explicit(&state->error_code, errno == 0 ? EINVAL : errno, memory_order_release);
            return NULL;
        }
    }
    return NULL;
}

static int test_broker_validate_revoke_race_guard(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    broker_validate_revoke_state_t state;
    pthread_t revoker;
    pthread_t validator;
    bool revoker_started = false;
    bool validator_started = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    memset(&state, 0, sizeof(state));
    state.broker = &broker;
    atomic_init(&state.start, false);
    atomic_init(&state.error_code, 0);
    if (llam_broker_create_channel(&broker,
                                   2U,
                                   LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_DESTROY,
                                   &state.revoker_token) != 0) {
        goto done;
    }
    state.stale_token = state.revoker_token;
    if (pthread_create(&revoker, NULL, broker_revoke_worker, &state) != 0) {
        goto done;
    }
    revoker_started = true;
    if (pthread_create(&validator, NULL, broker_validate_worker, &state) != 0) {
        goto done;
    }
    validator_started = true;
    atomic_store_explicit(&state.start, true, memory_order_release);

done:
    if (revoker_started) {
        (void)pthread_join(revoker, NULL);
    }
    if (validator_started) {
        (void)pthread_join(validator, NULL);
    }
    if (revoker_started && validator_started &&
        atomic_load_explicit(&state.error_code, memory_order_acquire) == 0) {
        rc = 0;
    }
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_listen_unix_preserves_existing_file(void) {
    char path[128];
    char data[8] = {0};
    FILE *file;
    int fd = -1;
    int written;

    written = snprintf(path, sizeof(path), "/tmp/llam-broker-regular-%ld.sock", (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return -1;
    }
    (void)unlink(path);
    file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    if (fwrite("keep", 1U, 4U, file) != 4U || fclose(file) != 0) {
        (void)unlink(path);
        return -1;
    }

    errno = 0;
    if (expect_errno(llam_broker_listen_unix(path, &fd),
                     EEXIST,
                     "broker listen unlinked a non-socket path") != 0) {
        if (fd >= 0) {
            close(fd);
        }
        (void)unlink(path);
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        (void)unlink(path);
        return -1;
    }
    if (fread(data, 1U, 4U, file) != 4U || memcmp(data, "keep", 4U) != 0) {
        (void)fclose(file);
        (void)unlink(path);
        return -1;
    }
    (void)fclose(file);
    (void)unlink(path);
    return 0;
}

static int test_broker_listen_unix_owner_only_mode(void) {
    char path[128];
    struct stat info;
    int fd = -1;
    int written;
    int rc = -1;

    written = snprintf(path, sizeof(path), "/tmp/llam-broker-mode-%ld.sock", (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return -1;
    }
    (void)unlink(path);
    if (llam_broker_listen_unix(path, &fd) != 0) {
        goto done;
    }
    if (lstat(path, &info) != 0) {
        goto done;
    }
    if ((info.st_mode & 0777U) != 0600U) {
        fprintf(stderr,
                "[test_security_capability] broker socket mode %03o expected 600\n",
                (unsigned)(info.st_mode & 0777U));
        goto done;
    }
    rc = 0;

done:
    if (fd >= 0) {
        close(fd);
    }
    (void)unlink(path);
    return rc;
}

static int test_broker_restrict_owned_socket_rejects_symlink(void) {
    char target_path[128];
    char link_path[128];
    struct stat before;
    struct stat after;
    int target_fd = -1;
    int written;
    int rc = -1;
    int saved_errno;

    written = snprintf(target_path, sizeof(target_path), "/tmp/llam-broker-chmod-target-%ld", (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(target_path)) {
        return -1;
    }
    written = snprintf(link_path, sizeof(link_path), "/tmp/llam-broker-chmod-link-%ld.sock", (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(link_path)) {
        return -1;
    }
    (void)unlink(link_path);
    (void)unlink(target_path);
    target_fd = open(target_path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (target_fd < 0) {
        goto done;
    }
    close(target_fd);
    target_fd = -1;
    if (chmod(target_path, 0644) != 0 || stat(target_path, &before) != 0) {
        goto done;
    }
    if (symlink(target_path, link_path) != 0) {
        goto done;
    }
    /*
     * This exercises the chmod hardening helper directly. A broker endpoint
     * mode change must never follow a swapped-in symlink and chmod an unrelated
     * target file.
     */
    errno = 0;
    if (llam_broker_restrict_owned_socket(link_path, NULL) != -1) {
        fprintf(stderr, "[test_security_capability] broker socket chmod followed symlink path\n");
        goto done;
    }
    saved_errno = errno;
    if (saved_errno != ELOOP && saved_errno != EINVAL) {
        fprintf(stderr,
                "[test_security_capability] broker socket chmod symlink errno=%d expected ELOOP/EINVAL\n",
                saved_errno);
        goto done;
    }
    if (stat(target_path, &after) != 0) {
        goto done;
    }
    if ((before.st_mode & 0777U) != (after.st_mode & 0777U)) {
        fprintf(stderr,
                "[test_security_capability] broker socket chmod changed symlink target mode %03o -> %03o\n",
                (unsigned)(before.st_mode & 0777U),
                (unsigned)(after.st_mode & 0777U));
        goto done;
    }
    rc = 0;

done:
    if (target_fd >= 0) {
        close(target_fd);
    }
    (void)unlink(link_path);
    (void)unlink(target_path);
    return rc;
}

static int test_broker_restrict_owned_socket_requires_identity(void) {
    char path[128];
    struct sockaddr_un addr;
    int fd = -1;
    int written;
    int rc = -1;
    int saved_errno;

    written = snprintf(path, sizeof(path), "/tmp/llam-broker-chmod-noid-%ld.sock", (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return -1;
    }
    (void)unlink(path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        goto done;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1U);
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        goto done;
    }

    /*
     * The permission helper is only safe when tied to the exact socket node
     * captured immediately after bind. A NULL identity would silently weaken
     * the no-swap ownership check, so keep that misuse failing closed.
     */
    errno = 0;
    if (llam_broker_restrict_owned_socket(path, NULL) != -1) {
        fprintf(stderr, "[test_security_capability] broker socket chmod accepted NULL identity\n");
        goto done;
    }
    saved_errno = errno;
    if (saved_errno != EINVAL) {
        fprintf(stderr,
                "[test_security_capability] broker socket chmod NULL identity errno=%d expected EINVAL\n",
                saved_errno);
        goto done;
    }
    rc = 0;

done:
    if (fd >= 0) {
        close(fd);
    }
    (void)unlink(path);
    return rc;
}

static int test_broker_capture_owned_socket_clears_identity_on_failure(void) {
    llam_broker_socket_identity_t identity;

    memset(&identity, 0xa5, sizeof(identity));
    errno = 0;
    if (expect_errno(llam_broker_capture_owned_socket(NULL, &identity),
                     EINVAL,
                     "broker socket identity capture accepted NULL path") != 0) {
        return -1;
    }
    if (!memory_is_byte(&identity, sizeof(identity), 0U)) {
        fprintf(stderr, "[test_security_capability] failed socket identity capture left stale identity\n");
        return -1;
    }
    return 0;
}

static int test_broker_unlink_owned_socket_uses_path_identity(void) {
    char path[128];
    char replacement_path[128];
    struct sockaddr_un addr;
    llam_broker_socket_identity_t identity;
    int fd = -1;
    int replacement_fd = -1;
    int written;
    int rc = -1;

    written = snprintf(path, sizeof(path), "/tmp/llam-broker-owned-unlink-%ld.sock", (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return -1;
    }
    written = snprintf(replacement_path,
                       sizeof(replacement_path),
                       "/tmp/llam-broker-owned-unlink-repl-%ld.sock",
                       (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(replacement_path)) {
        return -1;
    }
    (void)unlink(path);
    (void)unlink(replacement_path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        goto done;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1U);
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        llam_broker_capture_owned_socket(path, &identity) != 0) {
        goto done;
    }

    /*
     * AF_UNIX listener fds do not portably identify their filesystem socket
     * node through fstat(). Cleanup must therefore use the captured path
     * identity, and must not unlink a later path replacement.
     */
    llam_broker_unlink_owned_socket(path, &identity);
    if (lstat(path, &(struct stat){0}) == 0 || errno != ENOENT) {
        fprintf(stderr, "[test_security_capability] broker owned socket was not unlinked by path identity\n");
        goto done;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, replacement_path, strlen(replacement_path) + 1U);
    replacement_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (replacement_fd < 0 || bind(replacement_fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        goto done;
    }
    if (rename(replacement_path, path) != 0) {
        goto done;
    }
    replacement_path[0] = '\0';
    llam_broker_unlink_owned_socket(path, &identity);
    if (lstat(path, &(struct stat){0}) != 0) {
        fprintf(stderr, "[test_security_capability] broker unlink removed a replaced socket path\n");
        goto done;
    }
    rc = 0;

done:
    if (fd >= 0) {
        close(fd);
    }
    if (replacement_fd >= 0) {
        close(replacement_fd);
    }
    if (replacement_path[0] != '\0') {
        (void)unlink(replacement_path);
    }
    (void)unlink(path);
    return rc;
}

static bool broker_fd_has_cloexec(int fd) {
    int flags;

    if (fd < 0) {
        return false;
    }
    flags = fcntl(fd, F_GETFD);
    return flags >= 0 && (flags & FD_CLOEXEC) != 0;
}

static size_t broker_active_descriptor_count(const llam_broker_t *broker) {
    size_t count = 0U;

    if (broker == NULL) {
        return 0U;
    }
    for (size_t i = 0U; i < LLAM_BROKER_DESCRIPTOR_SLOTS; ++i) {
        count += broker->descriptors[i].active ? 1U : 0U;
    }
    return count;
}

static size_t broker_active_buffer_count(const llam_broker_t *broker) {
    size_t count = 0U;

    if (broker == NULL) {
        return 0U;
    }
    for (size_t i = 0U; i < LLAM_BROKER_BUFFER_SLOTS; ++i) {
        count += broker->buffers[i].active ? 1U : 0U;
    }
    return count;
}

static size_t broker_active_channel_count(const llam_broker_t *broker) {
    size_t count = 0U;

    if (broker == NULL || broker->channels == NULL) {
        return 0U;
    }
    for (size_t i = 0U; i < LLAM_BROKER_CHANNEL_SLOTS; ++i) {
        count += broker->channels[i].active ? 1U : 0U;
    }
    return count;
}

static int broker_write_request_plain(int fd, const llam_broker_wire_request_t *request);

static int test_broker_posix_transport_fds_are_cloexec(void) {
    char path[128];
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_wire_request_t request;
    llam_broker_wire_request_t received_request;
    llam_broker_wire_response_t response;
    int listen_fd = -1;
    int client_fd = -1;
    int server_fd = -1;
    int sockets[2] = {-1, -1};
    int pipe_fds[2] = {-1, -1};
    int broker_owned_pipe[2] = {-1, -1};
    int cloexec_duplicate = -1;
    int received_fd = -1;
    llam_handle_t response_descriptor = LLAM_INVALID_HANDLE;
    llam_capability_token_t descriptor_token;
    int written;
    bool broker_initialized = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;

    written = snprintf(path, sizeof(path), "/tmp/llam-broker-cloexec-%ld.sock", (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return -1;
    }
    (void)unlink(path);
    if (llam_broker_listen_unix(path, &listen_fd) != 0) {
        goto done;
    }
    if (!broker_fd_has_cloexec(listen_fd)) {
        fprintf(stderr, "[test_security_capability] broker listen fd is inheritable across exec\n");
        goto done;
    }
    if (llam_broker_connect_unix(path, &client_fd) != 0 ||
        !broker_fd_has_cloexec(client_fd)) {
        fprintf(stderr, "[test_security_capability] broker client fd is inheritable across exec\n");
        goto done;
    }
    if (llam_broker_accept_one(listen_fd, &server_fd) != 0 ||
        !broker_fd_has_cloexec(server_fd)) {
        fprintf(stderr, "[test_security_capability] broker accepted fd is inheritable across exec\n");
        goto done;
    }
    cloexec_duplicate = llam_broker_dup_cloexec_fd(client_fd);
    if (cloexec_duplicate < 0 ||
        cloexec_duplicate == client_fd ||
        !broker_fd_has_cloexec(cloexec_duplicate)) {
        fprintf(stderr, "[test_security_capability] broker duplicate fd is inheritable across exec\n");
        goto done;
    }

    /*
     * Descriptor grants cross the process boundary as ambient OS authority.
     * The broker must make the received duplicate close-on-exec immediately so
     * later helper execs cannot inherit a capability-bearing fd by accident.
     */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
        pipe(pipe_fds) != 0) {
        goto done;
    }
    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    request.rights = LLAM_CAP_RIGHT_READ;
    if (llam_broker_write_request_with_descriptor(sockets[0], &request, pipe_fds[0]) != 0 ||
        llam_broker_read_request_fd(sockets[1], &received_request, &received_fd) != 0 ||
        received_fd < 0 ||
        !broker_fd_has_cloexec(received_fd)) {
        fprintf(stderr, "[test_security_capability] received SCM_RIGHTS fd is inheritable across exec\n");
        goto done;
    }

    if (pipe(broker_owned_pipe) != 0 ||
        llam_broker_register_fd(&broker,
                                broker_owned_pipe[0],
                                LLAM_CAP_RIGHT_READ,
                                true,
                                &descriptor_token) != 0 ||
        !broker_fd_has_cloexec(broker_owned_pipe[0])) {
        fprintf(stderr, "[test_security_capability] broker-owned descriptor fd is inheritable across exec\n");
        goto done;
    }
    broker_owned_pipe[0] = -1;

    request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_RING);
    memset(&response, 0, sizeof(response));
    llam_broker_process_request_with_descriptors(&broker,
                                                 &request,
                                                 &response,
                                                 NULL,
                                                 LLAM_INVALID_HANDLE,
                                                 &response_descriptor);
    if (response.status != 0 ||
        llam_handle_is_invalid(response_descriptor) ||
        !broker_fd_has_cloexec((int)response_descriptor)) {
        fprintf(stderr, "[test_security_capability] broker response descriptor is inheritable across exec\n");
        goto done;
    }
    rc = 0;

done:
    if (!llam_handle_is_invalid(response_descriptor)) {
        llam_broker_close_handle(response_descriptor);
    }
    if (received_fd >= 0) {
        close(received_fd);
    }
    if (cloexec_duplicate >= 0) {
        close(cloexec_duplicate);
    }
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
    }
    if (broker_owned_pipe[0] >= 0) {
        close(broker_owned_pipe[0]);
    }
    if (broker_owned_pipe[1] >= 0) {
        close(broker_owned_pipe[1]);
    }
    if (sockets[0] >= 0) {
        close(sockets[0]);
    }
    if (sockets[1] >= 0) {
        close(sockets[1]);
    }
    if (server_fd >= 0) {
        close(server_fd);
    }
    if (client_fd >= 0) {
        close(client_fd);
    }
    if (listen_fd >= 0) {
        close(listen_fd);
    }
    (void)unlink(path);
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int broker_send_create_request_then_close(llam_broker_t *broker,
                                                 uint32_t op,
                                                 uint64_t slot,
                                                 uint64_t offset,
                                                 uint64_t length,
                                                 uint64_t rights) {
    llam_broker_wire_request_t request;
    int sockets[2] = {-1, -1};
    bool should_close = false;
    int rc = -1;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        return -1;
    }
    request_init(&request, op);
    request.slot = slot;
    request.offset = offset;
    request.length = length;
    request.rights = rights;
    if (broker_write_request_plain(sockets[0], &request) != 0) {
        goto done;
    }
    close(sockets[0]);
    sockets[0] = -1;

    errno = 0;
    if (llam_broker_serve_one_fd(broker, sockets[1], &should_close) != -1) {
        fprintf(stderr,
                "[test_security_capability] disconnected create op %u unexpectedly succeeded\n",
                op);
        goto done;
    }
    rc = 0;

done:
    if (sockets[0] >= 0) {
        close(sockets[0]);
    }
    if (sockets[1] >= 0) {
        close(sockets[1]);
    }
    return rc;
}

static int test_broker_create_response_failure_rolls_back_memory_grants(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    bool broker_initialized = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;

    /*
     * Buffer/channel grants are minted before the control response is written.
     * A disconnected peer never receives the token, so these slots must be
     * rolled back instead of becoming unreachable authority inside the broker.
     */
    if (broker_send_create_request_then_close(&broker,
                                              LLAM_BROKER_WIRE_OP_CREATE_BUFFER,
                                              16U,
                                              0U,
                                              0U,
                                              LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_WRITE) != 0 ||
        broker_active_buffer_count(&broker) != 0U) {
        fprintf(stderr,
                "[test_security_capability] failed buffer response left %zu active buffer slot(s)\n",
                broker_active_buffer_count(&broker));
        goto done;
    }
    if (broker_send_create_request_then_close(&broker,
                                              LLAM_BROKER_WIRE_OP_CREATE_CHANNEL,
                                              4U,
                                              0U,
                                              0U,
                                              LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_RECV) != 0 ||
        broker_active_channel_count(&broker) != 0U) {
        fprintf(stderr,
                "[test_security_capability] failed channel response left %zu active channel slot(s)\n",
                broker_active_channel_count(&broker));
        goto done;
    }
    rc = 0;

done:
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_register_descriptor_response_failure_rolls_back(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_wire_request_t request;
    int sockets[2] = {-1, -1};
    int pipe_fds[2] = {-1, -1};
    bool broker_initialized = false;
    bool should_close = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
        pipe(pipe_fds) != 0) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    request.rights = LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_DESTROY;
    if (llam_broker_write_request_with_descriptor(sockets[0], &request, pipe_fds[0]) != 0) {
        goto done;
    }
    close(pipe_fds[0]);
    pipe_fds[0] = -1;
    close(sockets[0]);
    sockets[0] = -1;

    /*
     * The broker creates the descriptor slot before sending the response. If
     * that response fails because the peer has already disconnected, the client
     * never receives the token; keeping the slot would leak broker-owned fd
     * authority until broker destroy.
     */
    errno = 0;
    if (llam_broker_serve_one_fd(&broker, sockets[1], &should_close) != -1) {
        fprintf(stderr, "[test_security_capability] disconnected descriptor registration unexpectedly succeeded\n");
        goto done;
    }
    if (broker_active_descriptor_count(&broker) != 0U) {
        fprintf(stderr,
                "[test_security_capability] failed descriptor response left %zu active descriptor slot(s)\n",
                broker_active_descriptor_count(&broker));
        goto done;
    }
    rc = 0;

done:
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
    }
    if (sockets[0] >= 0) {
        close(sockets[0]);
    }
    if (sockets[1] >= 0) {
        close(sockets[1]);
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

typedef struct broker_runtime_pump_state {
    llam_runtime_t *runtime;
    atomic_int stop;
    atomic_int error_code;
} broker_runtime_pump_state_t;

static void *broker_runtime_pump_thread(void *arg) {
    broker_runtime_pump_state_t *state = (broker_runtime_pump_state_t *)arg;

    while (atomic_load_explicit(&state->stop, memory_order_acquire) == 0) {
        if (llam_runtime_run_handle(state->runtime) != 0) {
            atomic_store_explicit(&state->error_code, errno == 0 ? EINVAL : errno, memory_order_release);
            break;
        }
        sched_yield();
    }
    return NULL;
}

static bool broker_wait_for_no_active_tasks(llam_broker_t *broker) {
    for (uint32_t spin = 0U; spin < 10000U; ++spin) {
        if (broker_active_task_count(broker) == 0U) {
            return true;
        }
        if ((spin & 63U) == 0U) {
            usleep(100U);
        } else {
            sched_yield();
        }
    }
    return broker_active_task_count(broker) == 0U;
}

static int test_broker_detach_reclaims_racing_completed_task_slots(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    broker_runtime_pump_state_t pump_state;
    pthread_t pump;
    uint64_t rounds;
    bool broker_initialized = false;
    bool pump_started = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;
    memset(&pump_state, 0, sizeof(pump_state));
    pump_state.runtime = broker.runtime;
    atomic_init(&pump_state.stop, 0);
    atomic_init(&pump_state.error_code, 0);
    if (pthread_create(&pump, NULL, broker_runtime_pump_thread, &pump_state) != 0) {
        goto done;
    }
    pump_started = true;

    rounds = test_env_u64("LLAM_SECURITY_TASK_DETACH_RACE_ROUNDS", 4096U, 32768U);
    for (uint64_t i = 0U; i < rounds; ++i) {
        llam_capability_token_t token;

        if (llam_broker_spawn_task(&broker,
                                   LLAM_BROKER_TASK_KIND_RETURN_U64,
                                   i,
                                   LLAM_CAP_RIGHT_DETACH,
                                   &token) != 0) {
            goto done;
        }
        /*
         * Detach races a broker task that may complete without taking the
         * broker lock. A leaked active slot here means detach observed SPAWNED,
         * completion concurrently published COMPLETED, and detach then wrote
         * DETACHED after clearing the task pointer.
         */
        if (llam_broker_detach_task(&broker, &token) != 0 ||
            !broker_wait_for_no_active_tasks(&broker)) {
            fprintf(stderr,
                    "[test_security_capability] racing detach leaked %zu active task slot(s) at round %" PRIu64 "\n",
                    broker_active_task_count(&broker),
                    i);
            goto done;
        }
        if (atomic_load_explicit(&pump_state.error_code, memory_order_acquire) != 0) {
            goto done;
        }
    }
    rc = 0;

done:
    if (pump_started) {
        atomic_store_explicit(&pump_state.stop, 1, memory_order_release);
        (void)pthread_join(pump, NULL);
    }
    if (atomic_load_explicit(&pump_state.error_code, memory_order_acquire) != 0) {
        fprintf(stderr,
                "[test_security_capability] broker runtime pump failed errno=%d\n",
                atomic_load_explicit(&pump_state.error_code, memory_order_acquire));
        rc = -1;
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_task_spawn_response_failure_rolls_back_race(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    broker_runtime_pump_state_t pump_state;
    pthread_t pump;
    uint64_t rounds;
    bool broker_initialized = false;
    bool pump_started = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;
    memset(&pump_state, 0, sizeof(pump_state));
    pump_state.runtime = broker.runtime;
    atomic_init(&pump_state.stop, 0);
    atomic_init(&pump_state.error_code, 0);
    if (pthread_create(&pump, NULL, broker_runtime_pump_thread, &pump_state) != 0) {
        goto done;
    }
    pump_started = true;

    rounds = test_env_u64("LLAM_SECURITY_TASK_ROLLBACK_ROUNDS", 512U, 4096U);
    for (uint64_t i = 0U; i < rounds; ++i) {
        if (broker_send_create_request_then_close(&broker,
                                                  LLAM_BROKER_WIRE_OP_TASK_SPAWN,
                                                  LLAM_BROKER_TASK_KIND_RETURN_U64,
                                                  i,
                                                  0U,
                                                  LLAM_CAP_RIGHT_JOIN | LLAM_CAP_RIGHT_DETACH) != 0) {
            goto done;
        }
        /*
         * Drive completed detached tasks back through the trampoline cleanup.
         * The invariant is stronger than "no immediate crash": every failed
         * response must leave no reachable task authority once runtime work is
         * quiescent.
         */
        if (!broker_wait_for_no_active_tasks(&broker)) {
            fprintf(stderr,
                    "[test_security_capability] failed task response left %zu active task slot(s) at round %" PRIu64 "\n",
                    broker_active_task_count(&broker),
                    i);
            goto done;
        }
        if (atomic_load_explicit(&pump_state.error_code, memory_order_acquire) != 0) {
            goto done;
        }
    }
    rc = 0;

done:
    if (pump_started) {
        atomic_store_explicit(&pump_state.stop, 1, memory_order_release);
        (void)pthread_join(pump, NULL);
    }
    if (atomic_load_explicit(&pump_state.error_code, memory_order_acquire) != 0) {
        fprintf(stderr,
                "[test_security_capability] broker runtime pump failed errno=%d\n",
                atomic_load_explicit(&pump_state.error_code, memory_order_acquire));
        rc = -1;
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_failed_wire_reads_clear_outputs(void) {
    llam_broker_wire_request_t partial_request;
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t partial_response;
    llam_broker_wire_response_t response;
    int sockets[2] = {-1, -1};
    int descriptor_fd = -1;
    int rc = -1;

    request_init(&partial_request, LLAM_BROKER_WIRE_OP_CREATE_BUFFER);
    memset(&request, 0xa5, sizeof(request));
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        goto done;
    }
    if (write(sockets[0], &partial_request, sizeof(partial_request) / 2U) !=
        (ssize_t)(sizeof(partial_request) / 2U)) {
        goto done;
    }
    (void)shutdown(sockets[0], SHUT_WR);
    errno = 0;
    if (expect_errno(llam_broker_read_request_fd(sockets[1], &request, &descriptor_fd),
                     EPIPE,
                     "partial broker request read did not fail closed") != 0 ||
        descriptor_fd != -1 ||
        !memory_is_byte(&request, sizeof(request), 0U)) {
        fprintf(stderr, "[test_security_capability] failed request read left attacker bytes in output\n");
        goto done;
    }
    close(sockets[0]);
    close(sockets[1]);
    sockets[0] = -1;
    sockets[1] = -1;

    memset(&partial_response, 0, sizeof(partial_response));
    partial_response.magic = LLAM_BROKER_WIRE_MAGIC;
    partial_response.version = LLAM_BROKER_WIRE_VERSION;
    partial_response.status = 0;
    partial_response.result0 = UINT64_C(0xfeedfacecafebeef);
    memset(&response, 0x5a, sizeof(response));
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        goto done;
    }
    if (write(sockets[0], &partial_response, sizeof(partial_response) / 2U) !=
        (ssize_t)(sizeof(partial_response) / 2U)) {
        goto done;
    }
    (void)shutdown(sockets[0], SHUT_WR);
    errno = 0;
    if (expect_errno(llam_broker_read_response_fd(sockets[1], &response, &descriptor_fd),
                     EPIPE,
                     "partial broker response read did not fail closed") != 0 ||
        descriptor_fd != -1 ||
        !memory_is_byte(&response, sizeof(response), 0U)) {
        fprintf(stderr, "[test_security_capability] failed response read left attacker bytes in output\n");
        goto done;
    }

    rc = 0;

done:
    if (descriptor_fd >= 0) {
        close(descriptor_fd);
    }
    if (sockets[0] >= 0) {
        close(sockets[0]);
    }
    if (sockets[1] >= 0) {
        close(sockets[1]);
    }
    return rc;
}

static int test_broker_request_helpers_clear_response_on_failure(void) {
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    int sockets[2] = {-1, -1};
    int pipe_fds[2] = {-1, -1};
    int descriptor_fd = 123;
    int rc = -1;

    request_init(&request, LLAM_BROKER_WIRE_OP_PING);

    memset(&response, 0x5a, sizeof(response));
    errno = 0;
    if (expect_errno(llam_broker_request_fd(-1, &request, &response),
                     EINVAL,
                     "invalid broker request fd did not fail with EINVAL") != 0 ||
        !memory_is_byte(&response, sizeof(response), 0U)) {
        fprintf(stderr, "[test_security_capability] invalid request fd left stale response bytes\n");
        goto done;
    }

    memset(&response, 0x5a, sizeof(response));
    errno = 0;
    if (expect_errno(llam_broker_request_fd_with_descriptor(-1, &request, 0, &response),
                     EINVAL,
                     "invalid descriptor request fd did not fail with EINVAL") != 0 ||
        !memory_is_byte(&response, sizeof(response), 0U)) {
        fprintf(stderr, "[test_security_capability] invalid descriptor request left stale response bytes\n");
        goto done;
    }

    memset(&response, 0x5a, sizeof(response));
    descriptor_fd = 123;
    errno = 0;
    if (expect_errno(llam_broker_request_fd_with_response_descriptor(-1, &request, &response, &descriptor_fd),
                     EINVAL,
                     "invalid response-descriptor request fd did not fail with EINVAL") != 0 ||
        descriptor_fd != -1 ||
        !memory_is_byte(&response, sizeof(response), 0U)) {
        fprintf(stderr,
                "[test_security_capability] invalid response-descriptor request left stale outputs\n");
        goto done;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        goto done;
    }
    close(sockets[1]);
    sockets[1] = -1;
    memset(&response, 0x5a, sizeof(response));
    errno = 0;
    if (expect_errno_either(llam_broker_request_fd(sockets[0], &request, &response),
                            EPIPE,
                            ECONNRESET,
                            "closed peer broker request did not fail closed") != 0 ||
        !memory_is_byte(&response, sizeof(response), 0U)) {
        fprintf(stderr, "[test_security_capability] closed peer request left stale response bytes\n");
        goto done;
    }
    close(sockets[0]);
    sockets[0] = -1;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
        pipe(pipe_fds) != 0) {
        goto done;
    }
    close(sockets[1]);
    sockets[1] = -1;
    memset(&response, 0x5a, sizeof(response));
    errno = 0;
    if (expect_errno_either(llam_broker_request_fd_with_descriptor(sockets[0],
                                                                   &request,
                                                                   pipe_fds[0],
                                                                   &response),
                            EPIPE,
                            ECONNRESET,
                            "closed peer descriptor request did not fail closed") != 0 ||
        !memory_is_byte(&response, sizeof(response), 0U)) {
        fprintf(stderr, "[test_security_capability] closed peer descriptor request left stale response bytes\n");
        goto done;
    }
    close(sockets[0]);
    sockets[0] = -1;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        goto done;
    }
    close(sockets[1]);
    sockets[1] = -1;
    memset(&response, 0x5a, sizeof(response));
    descriptor_fd = 123;
    errno = 0;
    if (expect_errno_either(llam_broker_request_fd_with_response_descriptor(sockets[0],
                                                                            &request,
                                                                            &response,
                                                                            &descriptor_fd),
                            EPIPE,
                            ECONNRESET,
                            "closed peer response-descriptor request did not fail closed") != 0 ||
        descriptor_fd != -1 ||
        !memory_is_byte(&response, sizeof(response), 0U)) {
        fprintf(stderr, "[test_security_capability] closed peer response-descriptor request left stale outputs\n");
        goto done;
    }
    close(sockets[0]);
    sockets[0] = -1;
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
        pipe_fds[0] = -1;
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
        pipe_fds[1] = -1;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        goto done;
    }
    memset(&response, 0, sizeof(response));
    response.magic = LLAM_BROKER_WIRE_MAGIC;
    response.version = LLAM_BROKER_WIRE_VERSION;
    response.status = -1;
    response.error_code = EACCES;
    response.token.family = LLAM_BROKER_CAP_FAMILY_DESCRIPTOR;
    response.result0 = UINT64_C(0xfeedfacecafebeef);
    response.result1 = UINT64_C(0x1112131415161718);
    response.result2 = UINT64_C(0x2122232425262728);
    memset(response.data, 0xa5, sizeof(response.data));
    if (llam_broker_write_response_fd(sockets[1], &response) != 0) {
        goto done;
    }
    memset(&response, 0x5a, sizeof(response));
    if (llam_broker_request_fd(sockets[0], &request, &response) != 0 ||
        response.status != -1 ||
        response.error_code != EACCES ||
        response.token.family != 0U ||
        response.result0 != 0U ||
        response.result1 != 0U ||
        response.result2 != 0U ||
        !memory_is_byte(response.data, sizeof(response.data), 0U)) {
        fprintf(stderr, "[test_security_capability] plain error response kept stale authority fields\n");
        goto done;
    }
    close(sockets[0]);
    close(sockets[1]);
    sockets[0] = -1;
    sockets[1] = -1;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        goto done;
    }
    memset(&response, 0, sizeof(response));
    response.magic = UINT32_C(0xdeadbeef);
    response.version = LLAM_BROKER_WIRE_VERSION;
    response.status = 0;
    response.token.family = LLAM_BROKER_CAP_FAMILY_DESCRIPTOR;
    response.result0 = UINT64_C(0xfeedfacecafebeef);
    response.result1 = UINT64_C(0x1112131415161718);
    response.result2 = UINT64_C(0x2122232425262728);
    memset(response.data, 0xa5, sizeof(response.data));
    if (llam_broker_write_response_fd(sockets[1], &response) != 0) {
        goto done;
    }
    memset(&response, 0x5a, sizeof(response));
    errno = 0;
    if (expect_errno(llam_broker_request_fd(sockets[0], &request, &response),
                     EINVAL,
                     "malformed success response was accepted") != 0 ||
        !memory_is_byte(&response, sizeof(response), 0U)) {
        fprintf(stderr, "[test_security_capability] malformed success response left stale authority fields\n");
        goto done;
    }
    close(sockets[0]);
    close(sockets[1]);
    sockets[0] = -1;
    sockets[1] = -1;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
        pipe(pipe_fds) != 0) {
        goto done;
    }
    memset(&response, 0, sizeof(response));
    response.magic = UINT32_C(0xdeadbeef);
    response.version = LLAM_BROKER_WIRE_VERSION;
    response.status = 0;
    response.token.family = LLAM_BROKER_CAP_FAMILY_DESCRIPTOR;
    response.result0 = UINT64_C(0xfeedfacecafebeef);
    response.result1 = UINT64_C(0x1112131415161718);
    response.result2 = UINT64_C(0x2122232425262728);
    memset(response.data, 0xa5, sizeof(response.data));
    if (llam_broker_write_response_with_descriptor(sockets[1], &response, pipe_fds[0]) != 0) {
        goto done;
    }
    memset(&response, 0x5a, sizeof(response));
    descriptor_fd = 123;
    errno = 0;
    if (expect_errno(llam_broker_request_fd_with_response_descriptor(sockets[0],
                                                                     &request,
                                                                     &response,
                                                                     &descriptor_fd),
                     EINVAL,
                     "malformed descriptor response was accepted") != 0 ||
        descriptor_fd != -1 ||
        !memory_is_byte(&response, sizeof(response), 0U)) {
        fprintf(stderr,
                "[test_security_capability] malformed descriptor response left stale outputs\n");
        goto done;
    }
    close(sockets[0]);
    close(sockets[1]);
    sockets[0] = -1;
    sockets[1] = -1;
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
        pipe(pipe_fds) != 0) {
        goto done;
    }
    memset(&response, 0, sizeof(response));
    response.magic = LLAM_BROKER_WIRE_MAGIC;
    response.version = LLAM_BROKER_WIRE_VERSION;
    response.status = -1;
    response.error_code = EACCES;
    response.token.family = LLAM_BROKER_CAP_FAMILY_DESCRIPTOR;
    response.result0 = UINT64_C(0xfeedfacecafebeef);
    response.result1 = UINT64_C(0x1112131415161718);
    response.result2 = UINT64_C(0x2122232425262728);
    memset(response.data, 0xa5, sizeof(response.data));
    if (llam_broker_write_response_fd(sockets[1], &response) != 0) {
        goto done;
    }
    memset(&response, 0x5a, sizeof(response));
    if (llam_broker_request_fd_with_descriptor(sockets[0], &request, pipe_fds[0], &response) != 0 ||
        response.status != -1 ||
        response.error_code != EACCES ||
        response.token.family != 0U ||
        response.result0 != 0U ||
        response.result1 != 0U ||
        response.result2 != 0U ||
        !memory_is_byte(response.data, sizeof(response.data), 0U)) {
        fprintf(stderr, "[test_security_capability] descriptor error response kept stale authority fields\n");
        goto done;
    }
    close(sockets[0]);
    close(sockets[1]);
    sockets[0] = -1;
    sockets[1] = -1;
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
        pipe(pipe_fds) != 0) {
        goto done;
    }
    memset(&response, 0, sizeof(response));
    response.magic = LLAM_BROKER_WIRE_MAGIC;
    response.version = LLAM_BROKER_WIRE_VERSION;
    response.status = -1;
    response.error_code = EACCES;
    response.result0 = UINT64_C(0xfeedfacecafebeef);
    response.result1 = UINT64_C(0x1112131415161718);
    response.result2 = UINT64_C(0x2122232425262728);
    memset(response.data, 0xa5, sizeof(response.data));
    if (llam_broker_write_response_with_descriptor(sockets[1], &response, pipe_fds[0]) != 0) {
        goto done;
    }
    memset(&response, 0x5a, sizeof(response));
    descriptor_fd = 123;
    if (llam_broker_request_fd_with_response_descriptor(sockets[0],
                                                        &request,
                                                        &response,
                                                        &descriptor_fd) != 0 ||
        response.status != -1 ||
        response.error_code != EACCES ||
        response.result0 != 0U ||
        response.result1 != 0U ||
        response.result2 != 0U ||
        descriptor_fd != -1) {
        fprintf(stderr,
                "[test_security_capability] error response kept descriptor or stale authority fields\n");
        goto done;
    }

    rc = 0;

done:
    if (sockets[0] >= 0) {
        close(sockets[0]);
    }
    if (sockets[1] >= 0) {
        close(sockets[1]);
    }
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
    }
    return rc;
}

static int test_broker_endpoint_helpers_clear_outputs_on_failure(void) {
    int fd = 123;
    llam_handle_t handle = (llam_handle_t)123;

    errno = 0;
    if (expect_errno(llam_broker_listen_unix(NULL, &fd),
                     EINVAL,
                     "invalid Unix listen path did not fail with EINVAL") != 0 ||
        fd != -1) {
        fprintf(stderr, "[test_security_capability] failed Unix listen left stale fd output\n");
        return -1;
    }

    fd = 123;
    errno = 0;
    if (expect_errno(llam_broker_connect_unix(NULL, &fd),
                     EINVAL,
                     "invalid Unix connect path did not fail with EINVAL") != 0 ||
        fd != -1) {
        fprintf(stderr, "[test_security_capability] failed Unix connect left stale fd output\n");
        return -1;
    }

    fd = 123;
    errno = 0;
    if (expect_errno(llam_broker_accept_one(-1, &fd),
                     EINVAL,
                     "invalid Unix accept fd did not fail with EINVAL") != 0 ||
        fd != -1) {
        fprintf(stderr, "[test_security_capability] failed Unix accept left stale fd output\n");
        return -1;
    }

    errno = 0;
    if (expect_errno(llam_broker_listen_pipe("unsupported", &handle),
                     ENOTSUP,
                     "unsupported POSIX pipe listen did not fail with ENOTSUP") != 0 ||
        !LLAM_HANDLE_IS_INVALID(handle)) {
        fprintf(stderr, "[test_security_capability] unsupported pipe listen left stale handle output\n");
        return -1;
    }

    handle = (llam_handle_t)123;
    errno = 0;
    if (expect_errno(llam_broker_connect_pipe("unsupported", &handle),
                     ENOTSUP,
                     "unsupported POSIX pipe connect did not fail with ENOTSUP") != 0 ||
        !LLAM_HANDLE_IS_INVALID(handle)) {
        fprintf(stderr, "[test_security_capability] unsupported pipe connect left stale handle output\n");
        return -1;
    }

    return 0;
}

static int test_broker_direct_failed_outputs_are_cleared(void) {
    static const char initial[] = "direct-output";
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_token_t buffer_token;
    llam_capability_token_t replacement;
    llam_capability_token_t channel_token;
    unsigned char out[32];
    bool broker_initialized = false;
    int rc = -1;
#if !LLAM_PLATFORM_WINDOWS
    int pipe_fds[2] = {-1, -1};
    llam_capability_token_t descriptor_token;
#endif

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;

    if (llam_broker_register_buffer(&broker,
                                    initial,
                                    sizeof(initial),
                                    LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_DESTROY,
                                    &buffer_token) != 0 ||
        llam_broker_revoke_object_cap(&broker, &buffer_token, LLAM_CAP_RIGHT_READ, &replacement) != 0) {
        goto done;
    }
    memset(out, 0xa5, sizeof(out));
    errno = 0;
    if (expect_errno(llam_broker_read_buffer(&broker, &buffer_token, 0U, out, sizeof(initial)),
                     EACCES,
                     "stale broker buffer token unexpectedly read") != 0 ||
        !memory_is_byte(out, sizeof(initial), 0U)) {
        fprintf(stderr, "[test_security_capability] failed broker buffer read left stale output bytes\n");
        goto done;
    }

    if (llam_broker_create_channel(&broker,
                                   1U,
                                   LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_RECV | LLAM_CAP_RIGHT_CLOSE,
                                   &channel_token) != 0) {
        goto done;
    }
    memset(out, 0xa5, sizeof(out));
    errno = 0;
    if (expect_errno((int)llam_broker_channel_recv(&broker, &channel_token, out, sizeof(out)),
                     EAGAIN,
                     "empty broker channel unexpectedly received") != 0 ||
        !memory_is_byte(out, sizeof(out), 0U)) {
        fprintf(stderr, "[test_security_capability] failed broker channel recv left stale output bytes\n");
        goto done;
    }

#if !LLAM_PLATFORM_WINDOWS
    if (pipe(pipe_fds) != 0 ||
        llam_broker_register_fd(&broker,
                                pipe_fds[1],
                                LLAM_CAP_RIGHT_WRITE,
                                false,
                                &descriptor_token) != 0) {
        goto done;
    }
    memset(out, 0xa5, sizeof(out));
    errno = 0;
    if (expect_errno((int)llam_broker_read_fd(&broker, &descriptor_token, out, sizeof(out)),
                     EACCES,
                     "write-only broker descriptor unexpectedly read") != 0 ||
        !memory_is_byte(out, sizeof(out), 0U)) {
        fprintf(stderr, "[test_security_capability] failed broker descriptor read left stale output bytes\n");
        goto done;
    }
#endif

    rc = 0;

done:
#if !LLAM_PLATFORM_WINDOWS
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
    }
#endif
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int broker_count_open_fds(void) {
    long max_fd = sysconf(_SC_OPEN_MAX);
    int count = 0;
    long fd;

    if (max_fd <= 0 || max_fd > 4096) {
        max_fd = 4096;
    }
    for (fd = 0; fd < max_fd; ++fd) {
        if (fcntl((int)fd, F_GETFD) >= 0 || errno != EBADF) {
            count += 1;
        }
    }
    return count;
}

static int broker_write_request_with_fd_array(int fd,
                                              const llam_broker_wire_request_t *request,
                                              const int *fds,
                                              size_t fd_count) {
    unsigned char control[CMSG_SPACE(sizeof(int) * 4U)];
    struct iovec iov;
    struct msghdr msg;
    struct cmsghdr *cmsg;

    if (fd < 0 || request == NULL || fds == NULL || fd_count == 0U || fd_count > 4U) {
        errno = EINVAL;
        return -1;
    }
    memset(control, 0, sizeof(control));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = (void *)request;
    iov.iov_len = sizeof(*request);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1U;
    msg.msg_control = control;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
    memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fd_count);
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
    if (sendmsg(fd, &msg, 0) != (ssize_t)sizeof(*request)) {
        return -1;
    }
    return 0;
}

#if !LLAM_PLATFORM_WINDOWS
static int broker_write_partial_payload_with_fd(int fd,
                                                const void *payload,
                                                size_t payload_len,
                                                int descriptor_fd) {
    unsigned char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov;
    struct msghdr msg;
    struct cmsghdr *cmsg;

    if (fd < 0 || payload == NULL || payload_len == 0U || descriptor_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    memset(control, 0, sizeof(control));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = (void *)payload;
    iov.iov_len = payload_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1U;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &descriptor_fd, sizeof(descriptor_fd));
    msg.msg_controllen = CMSG_SPACE(sizeof(int));
    if (sendmsg(fd, &msg, 0) != (ssize_t)payload_len) {
        return -1;
    }
    return 0;
}

static int test_broker_partial_descriptor_wire_reads_close_received_fd(void) {
    llam_broker_wire_request_t partial_request;
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t partial_response;
    llam_broker_wire_response_t response;
    int sockets[2] = {-1, -1};
    int pipe_fds[2] = {-1, -1};
    int descriptor_fd = -1;
    int with_originals;
    int after_read;
    int rc = -1;

    request_init(&partial_request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    memset(&request, 0xa5, sizeof(request));
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
        pipe(pipe_fds) != 0) {
        goto done;
    }
    with_originals = broker_count_open_fds();
    /*
     * The receiving side owns an SCM_RIGHTS duplicate as soon as the first
     * partial frame is read. EOF must close that duplicate before returning
     * EPIPE, otherwise a malformed peer can leak broker fds without issuing a
     * valid broker request.
     */
    if (broker_write_partial_payload_with_fd(sockets[0],
                                             &partial_request,
                                             sizeof(partial_request) / 2U,
                                             pipe_fds[0]) != 0) {
        goto done;
    }
    (void)shutdown(sockets[0], SHUT_WR);
    errno = 0;
    if (expect_errno(llam_broker_read_request_fd(sockets[1], &request, &descriptor_fd),
                     EPIPE,
                     "partial descriptor broker request did not fail closed") != 0 ||
        descriptor_fd != -1 ||
        !memory_is_byte(&request, sizeof(request), 0U)) {
        fprintf(stderr,
                "[test_security_capability] partial descriptor request left authority or bytes in output\n");
        goto done;
    }
    after_read = broker_count_open_fds();
    if (after_read != with_originals) {
        fprintf(stderr,
                "[test_security_capability] partial descriptor request leaked fd count %d -> %d\n",
                with_originals,
                after_read);
        goto done;
    }
    close(sockets[0]);
    close(sockets[1]);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    sockets[0] = -1;
    sockets[1] = -1;
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;

    memset(&partial_response, 0, sizeof(partial_response));
    partial_response.magic = LLAM_BROKER_WIRE_MAGIC;
    partial_response.version = LLAM_BROKER_WIRE_VERSION;
    partial_response.status = 0;
    partial_response.result0 = UINT64_C(0xfeedfacecafebeef);
    memset(&response, 0x5a, sizeof(response));
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
        pipe(pipe_fds) != 0) {
        goto done;
    }
    with_originals = broker_count_open_fds();
    if (broker_write_partial_payload_with_fd(sockets[0],
                                             &partial_response,
                                             sizeof(partial_response) / 2U,
                                             pipe_fds[0]) != 0) {
        goto done;
    }
    (void)shutdown(sockets[0], SHUT_WR);
    errno = 0;
    if (expect_errno(llam_broker_read_response_fd(sockets[1], &response, &descriptor_fd),
                     EPIPE,
                     "partial descriptor broker response did not fail closed") != 0 ||
        descriptor_fd != -1 ||
        !memory_is_byte(&response, sizeof(response), 0U)) {
        fprintf(stderr,
                "[test_security_capability] partial descriptor response left authority or bytes in output\n");
        goto done;
    }
    after_read = broker_count_open_fds();
    if (after_read != with_originals) {
        fprintf(stderr,
                "[test_security_capability] partial descriptor response leaked fd count %d -> %d\n",
                with_originals,
                after_read);
        goto done;
    }
    rc = 0;

done:
    if (descriptor_fd >= 0) {
        close(descriptor_fd);
    }
    if (sockets[0] >= 0) {
        close(sockets[0]);
    }
    if (sockets[1] >= 0) {
        close(sockets[1]);
    }
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
    }
    return rc;
}
#endif

static int test_broker_invalid_register_descriptor_closes_received_fd(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_wire_request_t request;
    bool should_close = false;
    int sockets[2] = {-1, -1};
    int pipe_fds[2] = {-1, -1};
    int before_fds;
    int after_fds;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    before_fds = broker_count_open_fds();
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        goto done;
    }
    if (pipe(pipe_fds) != 0) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    request.magic = 0U;
    request.rights = LLAM_CAP_RIGHT_READ;
    if (llam_broker_write_request_with_descriptor(sockets[0], &request, pipe_fds[0]) != 0) {
        goto done;
    }
    if (llam_broker_serve_one_fd(&broker, sockets[1], &should_close) != 0) {
        goto done;
    }

    close(sockets[0]);
    close(sockets[1]);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    sockets[0] = -1;
    sockets[1] = -1;
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
    after_fds = broker_count_open_fds();
    if (after_fds != before_fds) {
        fprintf(stderr,
                "[test_security_capability] invalid descriptor register leaked fd count %d -> %d\n",
                before_fds,
                after_fds);
        goto done;
    }
    rc = 0;

done:
    if (sockets[0] >= 0) {
        close(sockets[0]);
    }
    if (sockets[1] >= 0) {
        close(sockets[1]);
    }
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
    }
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_overauthorized_descriptor_array_closes_all_received_fds(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_wire_request_t request;
    bool should_close = false;
    int sockets[2] = {-1, -1};
    int pipes[4][2] = {{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}};
    int descriptor_fds[4] = {-1, -1, -1, -1};
    int before_fds;
    int after_fds;
    size_t i;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    before_fds = broker_count_open_fds();
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        goto done;
    }
    for (i = 0U; i < 4U; ++i) {
        if (pipe(pipes[i]) != 0) {
            goto done;
        }
        descriptor_fds[i] = pipes[i][0];
    }

    /*
     * A malicious client can over-authorize a register request by attaching
     * multiple SCM_RIGHTS fds. The broker must reject the request and close
     * every received duplicate, not just the first one it notices.
     */
    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    request.rights = LLAM_CAP_RIGHT_READ;
    if (broker_write_request_with_fd_array(sockets[0], &request, descriptor_fds, 4U) != 0) {
        goto done;
    }
    errno = 0;
    if (llam_broker_serve_one_fd(&broker, sockets[1], &should_close) == 0 || errno != EINVAL) {
        fprintf(stderr,
                "[test_security_capability] multi-fd descriptor grant was not rejected: errno=%d\n",
                errno);
        goto done;
    }

    close(sockets[0]);
    close(sockets[1]);
    sockets[0] = -1;
    sockets[1] = -1;
    for (i = 0U; i < 4U; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
        pipes[i][0] = -1;
        pipes[i][1] = -1;
    }
    after_fds = broker_count_open_fds();
    if (after_fds != before_fds) {
        fprintf(stderr,
                "[test_security_capability] multi-fd descriptor grant leaked fd count %d -> %d\n",
                before_fds,
                after_fds);
        goto done;
    }
    rc = 0;

done:
    if (sockets[0] >= 0) {
        close(sockets[0]);
    }
    if (sockets[1] >= 0) {
        close(sockets[1]);
    }
    for (i = 0U; i < 4U; ++i) {
        if (pipes[i][0] >= 0) {
            close(pipes[i][0]);
        }
        if (pipes[i][1] >= 0) {
            close(pipes[i][1]);
        }
    }
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_unclaimed_descriptor_is_rejected_and_closed(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    int pipe_fds[2] = {-1, -1};
    int probed_fd = -1;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (pipe(pipe_fds) != 0) {
        goto done;
    }

    /*
     * A descriptor attached to any operation other than REGISTER_DESCRIPTOR is
     * unclaimed authority. The dispatcher must reject the request and close the
     * received fd immediately, otherwise malformed clients can leak broker fds.
     */
    request_init(&request, LLAM_BROKER_WIRE_OP_PING);
    memset(&response, 0, sizeof(response));
    probed_fd = pipe_fds[0];
    llam_broker_process_request_with_descriptor(&broker,
                                                &request,
                                                &response,
                                                NULL,
                                                (llam_handle_t)pipe_fds[0]);
    pipe_fds[0] = -1;
    if (response.status == 0 || response.error_code != EINVAL) {
        fprintf(stderr,
                "[test_security_capability] unclaimed descriptor accepted: status=%d errno=%d\n",
                response.status,
                response.error_code);
        goto done;
    }

    errno = 0;
    if (fcntl(probed_fd, F_GETFD) >= 0 || errno != EBADF) {
        fprintf(stderr, "[test_security_capability] unclaimed descriptor fd remained open\n");
        goto done;
    }
    close(pipe_fds[1]);
    pipe_fds[1] = -1;
    if (pipe(pipe_fds) != 0) {
        goto done;
    }
    memset(&response, 0, sizeof(response));
    probed_fd = pipe_fds[0];
    llam_broker_process_request_with_descriptor(&broker,
                                                NULL,
                                                &response,
                                                NULL,
                                                (llam_handle_t)pipe_fds[0]);
    pipe_fds[0] = -1;
    if (response.status == 0 || response.error_code != EINVAL) {
        fprintf(stderr,
                "[test_security_capability] null request with descriptor accepted: status=%d errno=%d\n",
                response.status,
                response.error_code);
        goto done;
    }
    errno = 0;
    if (fcntl(probed_fd, F_GETFD) >= 0 || errno != EBADF) {
        fprintf(stderr, "[test_security_capability] null-request descriptor fd remained open\n");
        goto done;
    }
    rc = 0;

done:
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
    }
    llam_broker_destroy(&broker);
    return rc;
}

static int broker_write_request_plain(int fd, const llam_broker_wire_request_t *request) {
    const unsigned char *cursor = (const unsigned char *)request;
    size_t done = 0U;

    while (done < sizeof(*request)) {
        ssize_t nwritten = write(fd, cursor + done, sizeof(*request) - done);

        if (nwritten > 0) {
            done += (size_t)nwritten;
            continue;
        }
        if (nwritten == 0) {
            errno = EPIPE;
            return -1;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static int broker_read_response_plain(int fd, llam_broker_wire_response_t *response) {
    unsigned char *cursor = (unsigned char *)response;
    size_t done = 0U;

    while (done < sizeof(*response)) {
        ssize_t nread = read(fd, cursor + done, sizeof(*response) - done);

        if (nread > 0) {
            done += (size_t)nread;
            continue;
        }
        if (nread == 0) {
            errno = EPIPE;
            return -1;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static int broker_roundtrip_serve_one_fd(llam_broker_t *broker,
                                         int client_fd,
                                         int server_fd,
                                         const llam_broker_wire_request_t *request,
                                         llam_broker_wire_response_t *response,
                                         bool *out_should_close) {
    if (broker_write_request_plain(client_fd, request) != 0) {
        return -1;
    }
    if (llam_broker_serve_one_fd(broker, server_fd, out_should_close) != 0) {
        return -1;
    }
    return broker_read_response_plain(client_fd, response);
}

static int test_broker_serve_one_fd_keeps_session_subject(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    llam_capability_token_t token;
    bool should_close = false;
    int fds[2] = {-1, -1};
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_BUFFER);
    request.slot = 32U;
    request.rights = LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_WRITE;
    if (broker_roundtrip_serve_one_fd(&broker, fds[0], fds[1], &request, &response, &should_close) != 0 ||
        response.status != 0 ||
        response.token.subject_id == 0U) {
        goto done;
    }
    token = response.token;

    request_init(&request, LLAM_BROKER_WIRE_OP_VALIDATE_CAP);
    request.token = token;
    request.required_rights = LLAM_CAP_RIGHT_WRITE;
    if (broker_roundtrip_serve_one_fd(&broker, fds[0], fds[1], &request, &response, &should_close) != 0 ||
        response.status != 0) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_STOP);
    if (broker_roundtrip_serve_one_fd(&broker, fds[0], fds[1], &request, &response, &should_close) != 0 ||
        response.status != 0 ||
        !should_close) {
        goto done;
    }
    rc = 0;

done:
    if (fds[0] >= 0) {
        close(fds[0]);
    }
    if (fds[1] >= 0) {
        close(fds[1]);
    }
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_transport_rejects_cross_session_replay(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    llam_capability_token_t token;
    bool should_close_a = false;
    bool should_close_b = false;
    int session_a[2] = {-1, -1};
    int session_b[2] = {-1, -1};
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, session_a) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, session_b) != 0) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_BUFFER);
    request.slot = 32U;
    request.rights = LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_WRITE;
    if (broker_roundtrip_serve_one_fd(&broker,
                                      session_a[0],
                                      session_a[1],
                                      &request,
                                      &response,
                                      &should_close_a) != 0 ||
        response.status != 0 ||
        response.token.subject_id == 0U) {
        goto done;
    }
    token = response.token;

    request_init(&request, LLAM_BROKER_WIRE_OP_VALIDATE_CAP);
    request.token = token;
    request.required_rights = LLAM_CAP_RIGHT_READ;
    if (broker_roundtrip_serve_one_fd(&broker,
                                      session_a[0],
                                      session_a[1],
                                      &request,
                                      &response,
                                      &should_close_a) != 0 ||
        response.status != 0) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_VALIDATE_CAP);
    request.token = token;
    request.required_rights = LLAM_CAP_RIGHT_READ;
    if (broker_roundtrip_serve_one_fd(&broker,
                                      session_b[0],
                                      session_b[1],
                                      &request,
                                      &response,
                                      &should_close_b) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }
    rc = 0;

done:
    if (session_a[0] >= 0 && !should_close_a) {
        request_init(&request, LLAM_BROKER_WIRE_OP_STOP);
        (void)broker_roundtrip_serve_one_fd(&broker,
                                            session_a[0],
                                            session_a[1],
                                            &request,
                                            &response,
                                            &should_close_a);
    }
    if (session_b[0] >= 0 && !should_close_b) {
        request_init(&request, LLAM_BROKER_WIRE_OP_STOP);
        (void)broker_roundtrip_serve_one_fd(&broker,
                                            session_b[0],
                                            session_b[1],
                                            &request,
                                            &response,
                                            &should_close_b);
    }
    if (session_a[0] >= 0) {
        close(session_a[0]);
    }
    if (session_a[1] >= 0) {
        close(session_a[1]);
    }
    if (session_b[0] >= 0) {
        close(session_b[0]);
    }
    if (session_b[1] >= 0) {
        close(session_b[1]);
    }
    llam_broker_destroy(&broker);
    return rc;
}
#endif

static int test_broker_transport_subject_table_fails_closed(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    uint64_t subjects[LLAM_BROKER_TRANSPORT_SESSIONS];
    uint64_t subject = 0U;
    size_t i;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    /*
     * Transport-issued capabilities are valid only if the broker can remember
     * the session subject. A full session table must fail closed instead of
     * returning a one-shot subject that cannot validate the next request.
     */
    for (i = 0U; i < LLAM_BROKER_TRANSPORT_SESSIONS; ++i) {
        if (llam_broker_transport_subject(&broker, ((uintptr_t)0x1000U) + (uintptr_t)i, &subjects[i]) != 0 ||
            subjects[i] == 0U) {
            goto done;
        }
    }
    if (llam_broker_transport_subject(&broker, (uintptr_t)0x1000U, &subject) != 0 ||
        subject != subjects[0]) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_transport_subject(&broker,
                                                   ((uintptr_t)0x1000U) + (uintptr_t)LLAM_BROKER_TRANSPORT_SESSIONS,
                                                   &subject),
                     ENOSPC,
                     "full transport subject table") != 0) {
        goto done;
    }
    llam_broker_forget_transport_subject(&broker, (uintptr_t)0x1000U);
    if (llam_broker_transport_subject(&broker, (uintptr_t)0x1000U, &subject) != 0 ||
        subject == 0U ||
        subject == subjects[0]) {
        goto done;
    }
    llam_broker_forget_transport_subject(&broker, (uintptr_t)0x1000U);
    if (llam_broker_transport_subject(&broker,
                                      ((uintptr_t)0x2000U) + (uintptr_t)LLAM_BROKER_TRANSPORT_SESSIONS,
                                      &subject) != 0 ||
        subject == 0U) {
        goto done;
    }
    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_ring_and_buffer_grants(void) {
    llam_broker_ring_t ring;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_submission_t popped_submission;
    llam_broker_ring_completion_t completion;
    llam_broker_ring_completion_t popped_completion;
    llam_broker_ring_completion_t drained_completions[3];
    llam_broker_ring_stats_t stats;
    llam_broker_buffer_grant_t grant;
    size_t drained_count;
    unsigned i;

    if (llam_broker_ring_init(&ring) != 0) {
        return -1;
    }
    memset(&submission, 0, sizeof(submission));
    for (i = 0U; i < LLAM_BROKER_RING_CAP; ++i) {
        submission.request_id = (uint64_t)i + 1U;
        submission.op = LLAM_BROKER_RING_OP_CAP_VALIDATE;
        if (llam_broker_ring_submit_push(&ring, &submission) != 0) {
            return -1;
        }
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_submit_push(&ring, &submission),
                     EAGAIN,
                     "full broker submission ring accepted push") != 0) {
        return -1;
    }
    for (i = 0U; i < LLAM_BROKER_RING_CAP; ++i) {
        if (llam_broker_ring_submit_pop(&ring, &popped_submission) != 0 ||
            popped_submission.request_id != (uint64_t)i + 1U) {
            return -1;
        }
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_submit_pop(&ring, &popped_submission),
                     EAGAIN,
                     "empty broker submission ring popped entry") != 0) {
        return -1;
    }

    /*
     * Shared-memory rings are client-writable, so broker-side consumers must
     * reject impossible counter windows instead of interpreting stale slots as
     * fresh commands.
     */
    atomic_store_explicit(&ring.submit_head.value, 5U, memory_order_relaxed);
    atomic_store_explicit(&ring.submit_tail.value, 4U, memory_order_relaxed);
    errno = 0;
    if (expect_errno(llam_broker_ring_submit_pop(&ring, &popped_submission),
                     EINVAL,
                     "corrupt submission ring window popped entry") != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_submit_push(&ring, &submission),
                     EINVAL,
                     "corrupt submission ring window accepted push") != 0) {
        return -1;
    }

    if (llam_broker_ring_init(&ring) != 0) {
        return -1;
    }
    memset(&completion, 0, sizeof(completion));
    completion.request_id = 77U;
    completion.status = 0;
    completion.result0 = 99U;
    if (llam_broker_ring_complete_push(&ring, &completion) != 0 ||
        llam_broker_ring_complete_pop(&ring, &popped_completion) != 0 ||
        popped_completion.request_id != completion.request_id ||
        popped_completion.result0 != completion.result0) {
        return -1;
    }
    if (llam_broker_ring_init(&ring) != 0) {
        return -1;
    }
    for (i = 0U; i < 3U; ++i) {
        memset(&completion, 0, sizeof(completion));
        completion.request_id = (uint64_t)i + 100U;
        completion.status = 0;
        completion.result0 = (uint64_t)i + 200U;
        if (llam_broker_ring_complete_push(&ring, &completion) != 0) {
            return -1;
        }
    }
    drained_count = 0U;
    if (llam_broker_ring_complete_drain(&ring,
                                        drained_completions,
                                        sizeof(drained_completions) / sizeof(drained_completions[0]),
                                        &drained_count) != 0 ||
        drained_count != 3U ||
        drained_completions[0].request_id != 100U ||
        drained_completions[2].result0 != 202U ||
        atomic_load_explicit(&ring.complete_head.value, memory_order_acquire) != 3U) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_complete_pop(&ring, &popped_completion),
                     EAGAIN,
                     "drained broker completion ring popped entry") != 0) {
        return -1;
    }

    if (llam_broker_ring_init(&ring) != 0) {
        return -1;
    }
    memset(&submission, 0, sizeof(submission));
    submission.op = LLAM_BROKER_RING_OP_NOP;
    for (i = 0U; i < 2U; ++i) {
        submission.request_id = (uint64_t)i + 1U;
        if (llam_broker_ring_submit_push(&ring, &submission) != 0) {
            return -1;
        }
    }
    for (i = 0U; i < 3U; ++i) {
        memset(&completion, 0, sizeof(completion));
        completion.request_id = (uint64_t)i + 10U;
        if (llam_broker_ring_complete_push(&ring, &completion) != 0) {
            return -1;
        }
    }
    if (llam_broker_ring_complete_drain(&ring, drained_completions, 2U, &drained_count) != 0 ||
        drained_count != 2U ||
        llam_broker_ring_complete_pop(&ring, &popped_completion) != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_complete_pop(&ring, &popped_completion),
                     EAGAIN,
                     "stats broker completion ring empty pop succeeded") != 0 ||
        llam_broker_ring_collect_stats(&ring, &stats) != 0 ||
        stats.client_submit_pushes != 2U ||
        stats.client_submit_tail_publishes != 2U ||
        stats.client_complete_drain_calls != 3U ||
        stats.client_complete_drain_entries != 3U ||
        stats.client_complete_empty != 1U ||
        stats.client_complete_head_publishes != 2U ||
        stats.client_complete_batch_max != 2U ||
        stats.broker_complete_tail_publishes != 3U ||
        stats.cursor_write_estimate != 7U) {
        return -1;
    }

    atomic_store_explicit(&ring.complete_head.value, 0U, memory_order_relaxed);
    atomic_store_explicit(&ring.complete_tail.value, (uint64_t)LLAM_BROKER_RING_CAP + 1U, memory_order_relaxed);
    errno = 0;
    if (expect_errno(llam_broker_ring_complete_pop(&ring, &popped_completion),
                     EINVAL,
                     "oversized completion ring window popped entry") != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_complete_push(&ring, &completion),
                     EINVAL,
                     "oversized completion ring window accepted push") != 0) {
        return -1;
    }

    if (llam_broker_buffer_grant_init(&grant,
                                      1U,
                                      1U,
                                      4096U,
                                      1024U,
                                      LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_WRITE,
                                      3U) != 0) {
        return -1;
    }
    if (llam_broker_buffer_grant_validate(&grant, LLAM_CAP_RIGHT_READ, 128U, 512U, 3U) != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_broker_buffer_grant_validate(&grant, LLAM_CAP_RIGHT_DESTROY, 0U, 1U, 3U),
                     EACCES,
                     "buffer grant accepted missing right") != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_broker_buffer_grant_validate(&grant, LLAM_CAP_RIGHT_READ, 0U, 1U, 4U),
                     EACCES,
                     "buffer grant accepted revoked epoch") != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_broker_buffer_grant_validate(&grant, LLAM_CAP_RIGHT_READ, 900U, 200U, 3U),
                     EINVAL,
                     "buffer grant accepted out-of-bounds range") != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_broker_buffer_grant_init(&grant,
                                                   2U,
                                                   1U,
                                                   UINT64_MAX - 8U,
                                                   16U,
                                                   LLAM_CAP_RIGHT_READ,
                                                   3U),
                     EINVAL,
                     "buffer grant accepted overflowing absolute range") != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_broker_buffer_grant_init(&grant,
                                                   2U,
                                                   1U,
                                                   (uint64_t)LLAM_BROKER_BUFFER_MAX_BYTES - 8U,
                                                   16U,
                                                   LLAM_CAP_RIGHT_READ,
                                                   3U),
                     EINVAL,
                     "buffer grant accepted range beyond broker buffer maximum") != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_broker_buffer_grant_init(&grant,
                                                   2U,
                                                   1U,
                                                   0U,
                                                   16U,
                                                   LLAM_CAP_RIGHT_ADMIN,
                                                   3U),
                     EACCES,
                     "buffer grant accepted non-buffer rights") != 0) {
        return -1;
    }
    errno = 0;
    if (expect_errno(llam_broker_buffer_grant_validate(&grant,
                                                       LLAM_CAP_RIGHT_READ,
                                                       UINT64_MAX,
                                                       2U,
                                                       3U),
                     EINVAL,
                     "buffer grant accepted overflowing relative range") != 0) {
        return -1;
    }
    return 0;
}

static int test_broker_ring_reinit_stale_session_fails_closed(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    llam_broker_ring_completion_t completions[2];
    size_t drained_count = 0U;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_ring_init(&ring) != 0) {
        goto done;
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 1001U;
    submission.op = LLAM_BROKER_RING_OP_NOP;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status != 0) {
        goto done;
    }

    /*
     * Reusing the same shared ring address after initialization resets the
     * client-visible cursors but not the broker's private session cursors. That
     * must fail closed: otherwise the broker can skip the first new submission
     * and publish a completion-tail gap containing stale success-looking slots.
     */
    if (llam_broker_ring_init(&ring) != 0) {
        goto done;
    }
    submission.request_id = 1002U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0) {
        goto done;
    }
    submission.request_id = 1003U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_serve_one(&broker, &ring),
                     EINVAL,
                     "broker ring accepted a reinitialized ring with stale private cursors") != 0) {
        goto done;
    }
    if (llam_broker_ring_complete_drain(&ring, completions, 2U, &drained_count) != 0 ||
        drained_count != 0U) {
        fprintf(stderr, "[test_security_capability] stale ring serve published completions after failure\n");
        goto done;
    }

    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

typedef struct broker_ring_delayed_submit_state {
    llam_broker_ring_t *ring;
    llam_broker_ring_doorbell_t *doorbell;
    uint64_t request_id;
    unsigned delay_ms;
    atomic_int error_code;
} broker_ring_delayed_submit_state_t;

static void broker_test_sleep_ms(unsigned delay_ms) {
#if LLAM_PLATFORM_WINDOWS
    Sleep((DWORD)delay_ms);
#else
    usleep((useconds_t)delay_ms * 1000U);
#endif
}

#if LLAM_PLATFORM_WINDOWS
static DWORD WINAPI broker_ring_delayed_submit_thread(LPVOID arg) {
    broker_ring_delayed_submit_state_t *state = (broker_ring_delayed_submit_state_t *)arg;
#else
static void *broker_ring_delayed_submit_thread(void *arg) {
    broker_ring_delayed_submit_state_t *state = (broker_ring_delayed_submit_state_t *)arg;
#endif
    llam_broker_ring_submission_t submission;

    broker_test_sleep_ms(state->delay_ms);
    memset(&submission, 0, sizeof(submission));
    submission.request_id = state->request_id;
    submission.op = LLAM_BROKER_RING_OP_NOP;
    if (llam_broker_ring_submit_push(state->ring, &submission) != 0 ||
        llam_broker_ring_doorbell_signal(state->doorbell) != 0) {
        atomic_store_explicit(&state->error_code, errno == 0 ? EINVAL : errno, memory_order_release);
    }
#if LLAM_PLATFORM_WINDOWS
    return 0U;
#else
    return NULL;
#endif
}

static int test_broker_ring_doorbell_waits(void) {
    llam_broker_ring_t ring;
    llam_broker_ring_doorbell_t doorbell;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_submission_t popped_submission;
    llam_broker_ring_completion_t completion;
    llam_broker_ring_completion_t popped_completion;
    bool doorbell_initialized = false;
    unsigned i;
    int rc = -1;

    if (llam_broker_ring_init(&ring) != 0 ||
        llam_broker_ring_doorbell_init(&doorbell) != 0) {
        return -1;
    }
    doorbell_initialized = true;

    /*
     * The doorbell is intentionally only a wait accelerator. The cursor state
     * is still authoritative, so a stale or unrelated wake must not make an
     * empty/full condition look ready.
     */
    errno = 0;
    if (expect_errno(llam_broker_ring_wait_submit_available(&ring, &doorbell, 0),
                     ETIMEDOUT,
                     "empty submit ring wait reported available work") != 0) {
        goto done;
    }
    /*
     * A stale wake is not readiness, but it also must not collapse a nonzero
     * timeout into an immediate failure. Otherwise a benign unrelated signal can
     * make a subsequent producer look like a lost wakeup.
     */
    {
        broker_ring_delayed_submit_state_t delayed_state;
#if LLAM_PLATFORM_WINDOWS
        HANDLE delayed_thread;
#else
        pthread_t delayed_thread;
#endif

        memset(&delayed_state, 0, sizeof(delayed_state));
        delayed_state.ring = &ring;
        delayed_state.doorbell = &doorbell;
        delayed_state.request_id = 42U;
        delayed_state.delay_ms = 25U;
        atomic_init(&delayed_state.error_code, 0);
        if (llam_broker_ring_doorbell_signal(&doorbell) != 0) {
            goto done;
        }
#if LLAM_PLATFORM_WINDOWS
        delayed_thread = CreateThread(NULL, 0U, broker_ring_delayed_submit_thread, &delayed_state, 0U, NULL);
        if (delayed_thread == NULL) {
            goto done;
        }
        if (llam_broker_ring_wait_submit_available(&ring, &doorbell, 1000) != 0) {
            WaitForSingleObject(delayed_thread, INFINITE);
            CloseHandle(delayed_thread);
            goto done;
        }
        WaitForSingleObject(delayed_thread, INFINITE);
        CloseHandle(delayed_thread);
#else
        if (pthread_create(&delayed_thread, NULL, broker_ring_delayed_submit_thread, &delayed_state) != 0) {
            goto done;
        }
        if (llam_broker_ring_wait_submit_available(&ring, &doorbell, 1000) != 0) {
            pthread_join(delayed_thread, NULL);
            goto done;
        }
        pthread_join(delayed_thread, NULL);
#endif
        if (atomic_load_explicit(&delayed_state.error_code, memory_order_acquire) != 0 ||
            llam_broker_ring_submit_pop(&ring, &popped_submission) != 0 ||
            popped_submission.request_id != delayed_state.request_id) {
            goto done;
        }
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 1U;
    submission.op = LLAM_BROKER_RING_OP_NOP;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_doorbell_signal(&doorbell) != 0 ||
        llam_broker_ring_wait_submit_available(&ring, &doorbell, 1000) != 0 ||
        llam_broker_ring_submit_pop(&ring, &popped_submission) != 0 ||
        popped_submission.request_id != submission.request_id) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_wait_submit_available(&ring, &doorbell, 0),
                     ETIMEDOUT,
                     "drained submit doorbell left a stale wake") != 0) {
        goto done;
    }

    for (i = 0U; i < LLAM_BROKER_RING_CAP; ++i) {
        submission.request_id = (uint64_t)i + 100U;
        if (llam_broker_ring_submit_push(&ring, &submission) != 0) {
            goto done;
        }
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_wait_submit_space(&ring, &doorbell, 0),
                     ETIMEDOUT,
                     "full submit ring wait reported free space") != 0) {
        goto done;
    }
    if (llam_broker_ring_submit_pop(&ring, &popped_submission) != 0 ||
        llam_broker_ring_doorbell_signal(&doorbell) != 0 ||
        llam_broker_ring_wait_submit_space(&ring, &doorbell, 1000) != 0) {
        goto done;
    }

    if (llam_broker_ring_init(&ring) != 0) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_wait_completion_available(&ring, &doorbell, 0),
                     ETIMEDOUT,
                     "empty completion ring wait reported available work") != 0) {
        goto done;
    }
    memset(&completion, 0, sizeof(completion));
    completion.request_id = 7U;
    if (llam_broker_ring_complete_push(&ring, &completion) != 0 ||
        llam_broker_ring_doorbell_signal(&doorbell) != 0 ||
        llam_broker_ring_wait_completion_available(&ring, &doorbell, 1000) != 0 ||
        llam_broker_ring_complete_pop(&ring, &popped_completion) != 0 ||
        popped_completion.request_id != completion.request_id) {
        goto done;
    }

    for (i = 0U; i < LLAM_BROKER_RING_CAP; ++i) {
        completion.request_id = (uint64_t)i + 200U;
        if (llam_broker_ring_complete_push(&ring, &completion) != 0) {
            goto done;
        }
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_wait_completion_space(&ring, &doorbell, 0),
                     ETIMEDOUT,
                     "full completion ring wait reported free space") != 0) {
        goto done;
    }
    if (llam_broker_ring_complete_pop(&ring, &popped_completion) != 0 ||
        llam_broker_ring_doorbell_signal(&doorbell) != 0 ||
        llam_broker_ring_wait_completion_space(&ring, &doorbell, 1000) != 0) {
        goto done;
    }
    rc = 0;

done:
    if (doorbell_initialized) {
        llam_broker_ring_doorbell_destroy(&doorbell);
    }
    return rc;
}

typedef struct broker_ring_doorbell_flood_state {
    llam_broker_ring_t *ring;
    llam_broker_ring_doorbell_t *submit_available;
    llam_broker_ring_doorbell_t *submit_space;
    llam_broker_ring_doorbell_t *completion_available;
    llam_broker_ring_doorbell_t *completion_space;
    uint64_t iterations;
    atomic_int stop;
    atomic_int error_code;
} broker_ring_doorbell_flood_state_t;

static int broker_ring_doorbell_flood_serve(broker_ring_doorbell_flood_state_t *state) {
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    uint64_t served = 0U;
    unsigned idle_rounds = 0U;

    while (served < state->iterations &&
           atomic_load_explicit(&state->stop, memory_order_acquire) == 0) {
        if (llam_broker_ring_submit_pop(state->ring, &submission) != 0) {
            if (errno != EAGAIN) {
                return errno == 0 ? EINVAL : errno;
            }
            if (atomic_load_explicit(&state->stop, memory_order_acquire) != 0) {
                return ECANCELED;
            }
            if (llam_broker_ring_wait_submit_available(state->ring, state->submit_available, 5000) != 0) {
                if (errno == ETIMEDOUT && ++idle_rounds < 10000U) {
                    continue;
                }
                return errno == 0 ? ETIMEDOUT : errno;
            }
            continue;
        }
        idle_rounds = 0U;
        if (llam_broker_ring_doorbell_signal(state->submit_space) != 0) {
            return errno == 0 ? EIO : errno;
        }

        memset(&completion, 0, sizeof(completion));
        completion.request_id = submission.request_id;
        completion.status = 0;
        completion.result0 = submission.request_id ^ UINT64_C(0xa55aa55aa55aa55a);
        for (;;) {
            if (llam_broker_ring_complete_push(state->ring, &completion) == 0) {
                if (llam_broker_ring_doorbell_signal(state->completion_available) != 0) {
                    return errno == 0 ? EIO : errno;
                }
                break;
            }
            if (errno != EAGAIN) {
                return errno == 0 ? EINVAL : errno;
            }
            if (atomic_load_explicit(&state->stop, memory_order_acquire) != 0) {
                return ECANCELED;
            }
            if (llam_broker_ring_wait_completion_space(state->ring, state->completion_space, 5000) != 0) {
                if (errno == ETIMEDOUT && ++idle_rounds < 10000U) {
                    continue;
                }
                return errno == 0 ? ETIMEDOUT : errno;
            }
        }
        ++served;
    }
    return 0;
}

#if LLAM_PLATFORM_WINDOWS
static DWORD WINAPI broker_ring_doorbell_flood_thread(LPVOID arg) {
    broker_ring_doorbell_flood_state_t *state = (broker_ring_doorbell_flood_state_t *)arg;
    int rc = broker_ring_doorbell_flood_serve(state);

    atomic_store_explicit(&state->error_code, rc, memory_order_release);
    return rc == 0 ? 0U : 1U;
}
#else
static void *broker_ring_doorbell_flood_thread(void *arg) {
    broker_ring_doorbell_flood_state_t *state = (broker_ring_doorbell_flood_state_t *)arg;
    int rc = broker_ring_doorbell_flood_serve(state);

    atomic_store_explicit(&state->error_code, rc, memory_order_release);
    return NULL;
}
#endif

static int test_broker_ring_doorbell_flood(void) {
    enum { DRAIN_BATCH = 32U };
    llam_broker_ring_t ring;
    llam_broker_ring_doorbell_t submit_available;
    llam_broker_ring_doorbell_t submit_space;
    llam_broker_ring_doorbell_t completion_available;
    llam_broker_ring_doorbell_t completion_space;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completions[DRAIN_BATCH];
    llam_broker_ring_stats_t stats;
    broker_ring_doorbell_flood_state_t state;
    const uint64_t flood_iters = test_broker_ring_flood_iters();
    uint64_t next_submit = 1U;
    uint64_t next_complete = 1U;
    unsigned idle_rounds = 0U;
    bool submit_available_initialized = false;
    bool submit_space_initialized = false;
    bool completion_available_initialized = false;
    bool completion_space_initialized = false;
    bool thread_started = false;
    int rc = -1;
#if LLAM_PLATFORM_WINDOWS
    HANDLE thread = NULL;
    DWORD wait_rc;
    DWORD thread_rc = 1U;
#else
    pthread_t thread;
#endif

    if (llam_broker_ring_init(&ring) != 0 ||
        llam_broker_ring_doorbell_init(&submit_available) != 0) {
        fprintf(stderr, "[test_security_capability] broker ring doorbell flood init failed errno=%d\n", errno);
        goto done;
    }
    submit_available_initialized = true;
    if (llam_broker_ring_doorbell_init(&submit_space) != 0) {
        fprintf(stderr, "[test_security_capability] broker ring doorbell flood submit-space init failed errno=%d\n", errno);
        goto done;
    }
    submit_space_initialized = true;
    if (llam_broker_ring_doorbell_init(&completion_available) != 0) {
        fprintf(stderr, "[test_security_capability] broker ring doorbell flood completion-available init failed errno=%d\n", errno);
        goto done;
    }
    completion_available_initialized = true;
    if (llam_broker_ring_doorbell_init(&completion_space) != 0) {
        fprintf(stderr, "[test_security_capability] broker ring doorbell flood completion-space init failed errno=%d\n", errno);
        goto done;
    }
    completion_space_initialized = true;

    memset(&state, 0, sizeof(state));
    state.ring = &ring;
    state.submit_available = &submit_available;
    state.submit_space = &submit_space;
    state.completion_available = &completion_available;
    state.completion_space = &completion_space;
    state.iterations = flood_iters;
    atomic_init(&state.stop, 0);
    atomic_init(&state.error_code, 0);
#if LLAM_PLATFORM_WINDOWS
    thread = CreateThread(NULL, 0U, broker_ring_doorbell_flood_thread, &state, 0U, NULL);
    if (thread == NULL) {
        fprintf(stderr, "[test_security_capability] broker ring doorbell flood CreateThread failed error=%lu\n", (unsigned long)GetLastError());
        goto done;
    }
#else
    if (pthread_create(&thread, NULL, broker_ring_doorbell_flood_thread, &state) != 0) {
        fprintf(stderr, "[test_security_capability] broker ring doorbell flood pthread_create failed\n");
        goto done;
    }
#endif
    thread_started = true;

    while (next_complete <= flood_iters) {
        bool progressed = false;

        while (next_submit <= flood_iters) {
            memset(&submission, 0, sizeof(submission));
            submission.request_id = next_submit;
            submission.op = LLAM_BROKER_RING_OP_NOP;
            if (llam_broker_ring_submit_push(&ring, &submission) == 0) {
                if (llam_broker_ring_doorbell_signal(&submit_available) != 0) {
                    fprintf(stderr,
                            "[test_security_capability] broker ring doorbell flood submit signal failed errno=%d\n",
                            errno);
                    goto done;
                }
                ++next_submit;
                progressed = true;
                continue;
            } else if (errno != EAGAIN) {
                fprintf(stderr,
                        "[test_security_capability] broker ring doorbell flood submit failed errno=%d\n",
                        errno);
                goto done;
            }
            break;
        }

        for (;;) {
            size_t count = 0U;

            if (llam_broker_ring_complete_drain(&ring, completions, DRAIN_BATCH, &count) != 0) {
                fprintf(stderr,
                        "[test_security_capability] broker ring doorbell flood drain failed errno=%d\n",
                        errno);
                goto done;
            }
            if (count == 0U) {
                break;
            }
            if (llam_broker_ring_doorbell_signal(&completion_space) != 0) {
                fprintf(stderr,
                        "[test_security_capability] broker ring doorbell flood completion-space signal failed errno=%d\n",
                        errno);
                goto done;
            }
            for (size_t i = 0U; i < count; ++i) {
                if (completions[i].request_id != next_complete ||
                    completions[i].status != 0 ||
                    completions[i].result0 != (next_complete ^ UINT64_C(0xa55aa55aa55aa55a))) {
                    fprintf(stderr,
                            "[test_security_capability] broker ring doorbell flood completion mismatch got=%llu expected=%llu\n",
                            (unsigned long long)completions[i].request_id,
                            (unsigned long long)next_complete);
                    goto done;
                }
                ++next_complete;
            }
            progressed = true;
        }

        if (!progressed) {
            if (next_submit <= flood_iters) {
                if (llam_broker_ring_wait_submit_space(&ring, &submit_space, 5000) != 0) {
                    if (errno == ETIMEDOUT && ++idle_rounds < 10000U) {
                        continue;
                    }
                    fprintf(stderr,
                            "[test_security_capability] broker ring doorbell flood wait submit-space failed errno=%d next_submit=%llu next_complete=%llu\n",
                            errno,
                            (unsigned long long)next_submit,
                            (unsigned long long)next_complete);
                    goto done;
                }
            } else if (llam_broker_ring_wait_completion_available(&ring, &completion_available, 5000) != 0) {
                if (errno == ETIMEDOUT && ++idle_rounds < 10000U) {
                    continue;
                }
                fprintf(stderr,
                        "[test_security_capability] broker ring doorbell flood wait completion-available failed errno=%d next_submit=%llu next_complete=%llu\n",
                        errno,
                        (unsigned long long)next_submit,
                        (unsigned long long)next_complete);
                goto done;
            }
        } else {
            idle_rounds = 0U;
        }
    }

#if LLAM_PLATFORM_WINDOWS
    wait_rc = WaitForSingleObject(thread, 5000U);
    if (wait_rc != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &thread_rc) ||
        thread_rc != 0U) {
        fprintf(stderr,
                "[test_security_capability] broker ring doorbell flood thread failed wait=%lu rc=%lu state_error=%d\n",
                (unsigned long)wait_rc,
                (unsigned long)thread_rc,
                atomic_load_explicit(&state.error_code, memory_order_acquire));
        goto done;
    }
    CloseHandle(thread);
    thread = NULL;
#else
    (void)pthread_join(thread, NULL);
#endif
    thread_started = false;

    if (atomic_load_explicit(&state.error_code, memory_order_acquire) != 0 ||
        llam_broker_ring_collect_stats(&ring, &stats) != 0 ||
        stats.client_submit_pushes != flood_iters ||
        stats.broker_complete_tail_publishes != flood_iters ||
        stats.client_complete_drain_entries != flood_iters ||
        stats.cursor_write_estimate < flood_iters * 2U) {
        fprintf(stderr,
                "[test_security_capability] broker ring doorbell flood stats failed: "
                "error=%d submit=%llu complete_tail=%llu drain_entries=%llu cursor_writes=%llu\n",
                atomic_load_explicit(&state.error_code, memory_order_acquire),
                (unsigned long long)stats.client_submit_pushes,
                (unsigned long long)stats.broker_complete_tail_publishes,
                (unsigned long long)stats.client_complete_drain_entries,
                (unsigned long long)stats.cursor_write_estimate);
        goto done;
    }
    rc = 0;

done:
    if (thread_started) {
        atomic_store_explicit(&state.stop, 1, memory_order_release);
        if (submit_available_initialized) {
            (void)llam_broker_ring_doorbell_signal(&submit_available);
        }
        if (completion_space_initialized) {
            (void)llam_broker_ring_doorbell_signal(&completion_space);
        }
    }
#if LLAM_PLATFORM_WINDOWS
    if (thread != NULL) {
        (void)WaitForSingleObject(thread, 5000U);
        CloseHandle(thread);
    }
#else
    if (thread_started) {
        (void)pthread_join(thread, NULL);
    }
#endif
    if (completion_space_initialized) {
        llam_broker_ring_doorbell_destroy(&completion_space);
    }
    if (completion_available_initialized) {
        llam_broker_ring_doorbell_destroy(&completion_available);
    }
    if (submit_space_initialized) {
        llam_broker_ring_doorbell_destroy(&submit_space);
    }
    if (submit_available_initialized) {
        llam_broker_ring_doorbell_destroy(&submit_available);
    }
    return rc;
}

static int test_broker_ring_batch_perf_gate(void) {
    enum { DRAIN_BATCH = LLAM_BROKER_RING_SERVE_BATCH_MAX };
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completions[DRAIN_BATCH];
    llam_broker_ring_stats_t stats;
    uint64_t *submit_ns = NULL;
    uint64_t *latency_ns = NULL;
    const uint64_t iterations = test_broker_ring_batch_perf_iters();
    const uint64_t min_ops = test_broker_ring_batch_min_ops();
    const uint64_t max_p50_us = test_broker_ring_batch_max_p50_us();
    const uint64_t max_p99_us = test_broker_ring_batch_max_p99_us();
    const uint64_t max_cursor_x1000 = test_env_u64("LLAM_BROKER_RING_BATCH_MAX_CURSOR_X1000",
                                                   UINT64_C(1200),
                                                   UINT64_C(100000));
    const uint64_t expected_broker_batches =
        (iterations + (uint64_t)LLAM_BROKER_RING_SERVE_BATCH_MAX - 1U) /
        (uint64_t)LLAM_BROKER_RING_SERVE_BATCH_MAX;
    uint64_t next_submit = 1U;
    uint64_t next_complete = 1U;
    uint64_t completed = 0U;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t elapsed_ns;
    uint64_t ops_per_sec;
    uint64_t p50_us;
    uint64_t p99_us;
    uint64_t cursor_x1000;
    bool broker_initialized = false;
    int rc = -1;

    submit_ns = (uint64_t *)calloc((size_t)iterations, sizeof(*submit_ns));
    latency_ns = (uint64_t *)calloc((size_t)iterations, sizeof(*latency_ns));
    if (submit_ns == NULL || latency_ns == NULL) {
        goto done;
    }
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        goto done;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        goto done;
    }
    broker_initialized = true;
    if (llam_broker_ring_init(&ring) != 0) {
        goto done;
    }

    start_ns = llam_now_ns();
    while (completed < iterations) {
        bool progressed = false;

        while (next_submit <= iterations) {
            memset(&submission, 0, sizeof(submission));
            submission.request_id = next_submit;
            submission.op = LLAM_BROKER_RING_OP_NOP;
            if (llam_broker_ring_submit_push(&ring, &submission) == 0) {
                submit_ns[(size_t)(next_submit - 1U)] = llam_now_ns();
                ++next_submit;
                progressed = true;
                continue;
            }
            if (errno != EAGAIN) {
                fprintf(stderr,
                        "[test_security_capability] broker ring batch perf submit failed errno=%d\n",
                        errno);
                goto done;
            }
            break;
        }

        for (;;) {
            size_t served = 0U;

            if (llam_broker_ring_serve_batch(&broker,
                                             &ring,
                                             LLAM_BROKER_RING_SERVE_BATCH_MAX,
                                             &served) == 0) {
                if (served == 0U) {
                    fprintf(stderr,
                            "[test_security_capability] broker ring batch perf served zero requests\n");
                    goto done;
                }
                progressed = true;
                continue;
            }
            if (errno != EAGAIN) {
                fprintf(stderr,
                        "[test_security_capability] broker ring batch perf serve failed errno=%d\n",
                        errno);
                goto done;
            }
            break;
        }

        for (;;) {
            size_t count = 0U;
            uint64_t now_ns;

            if (llam_broker_ring_complete_drain(&ring, completions, DRAIN_BATCH, &count) != 0) {
                fprintf(stderr,
                        "[test_security_capability] broker ring batch perf drain failed errno=%d\n",
                        errno);
                goto done;
            }
            if (count == 0U) {
                break;
            }
            now_ns = llam_now_ns();
            for (size_t i = 0U; i < count; ++i) {
                if (completions[i].request_id != next_complete ||
                    completions[i].status != 0 ||
                    completions[i].error_code != 0) {
                    fprintf(stderr,
                            "[test_security_capability] broker ring batch perf completion mismatch got=%llu expected=%llu status=%d errno=%d\n",
                            (unsigned long long)completions[i].request_id,
                            (unsigned long long)next_complete,
                            completions[i].status,
                            completions[i].error_code);
                    goto done;
                }
                latency_ns[(size_t)completed] = now_ns - submit_ns[(size_t)(next_complete - 1U)];
                ++completed;
                ++next_complete;
            }
            progressed = true;
        }

        if (!progressed) {
            fprintf(stderr,
                    "[test_security_capability] broker ring batch perf made no progress submit=%llu complete=%llu\n",
                    (unsigned long long)next_submit,
                    (unsigned long long)next_complete);
            goto done;
        }
    }
    end_ns = llam_now_ns();
    elapsed_ns = end_ns > start_ns ? end_ns - start_ns : 1U;
    qsort(latency_ns, (size_t)iterations, sizeof(*latency_ns), compare_u64);
    ops_per_sec = (uint64_t)(((long double)iterations * 1000000000.0L) / (long double)elapsed_ns);
    p50_us = latency_ns[(size_t)(iterations / 2U)] / 1000U;
    p99_us = latency_ns[(size_t)(((iterations * 99U) / 100U) < iterations
                                     ? ((iterations * 99U) / 100U)
                                     : (iterations - 1U))] /
             1000U;

    if (llam_broker_ring_collect_stats(&ring, &stats) != 0) {
        goto done;
    }
    cursor_x1000 = stats.cursor_write_estimate * 1000U / iterations;
    printf("[test_security_capability] broker ring batch perf: "
           "requests=%llu ops_per_sec=%llu p50_us=%llu p99_us=%llu "
           "cursor_publish_per_request_x1000=%llu broker_complete_tail_publishes=%llu\n",
           (unsigned long long)iterations,
           (unsigned long long)ops_per_sec,
           (unsigned long long)p50_us,
           (unsigned long long)p99_us,
           (unsigned long long)cursor_x1000,
           (unsigned long long)stats.broker_complete_tail_publishes);

    /*
     * This is intentionally a conservative perf guard. It makes the batching
     * contract observable in CI without turning sanitizer or VM jitter into a
     * false failure source: cursor publication count is strict, latency/ops
     * thresholds are loose and env-tunable.
     */
    if (stats.client_submit_pushes != iterations ||
        stats.broker_serve_success != iterations ||
        stats.broker_submit_head_publishes > expected_broker_batches + 2U ||
        stats.broker_complete_tail_publishes > expected_broker_batches + 2U ||
        stats.client_complete_drain_entries != iterations ||
        stats.client_complete_batch_max < LLAM_BROKER_RING_SERVE_BATCH_MAX ||
        cursor_x1000 > max_cursor_x1000 ||
        ops_per_sec < min_ops ||
        p50_us > max_p50_us ||
        p99_us > max_p99_us) {
        fprintf(stderr,
                "[test_security_capability] broker ring batch perf gate failed: "
                "ops=%llu min=%llu p50=%llu max_p50=%llu p99=%llu max_p99=%llu "
                "cursor_x1000=%llu max_cursor_x1000=%llu submit=%llu serve_success=%llu "
                "submit_head_publishes=%llu complete_tail_publishes=%llu drain_entries=%llu batch_max=%llu expected_batches=%llu\n",
                (unsigned long long)ops_per_sec,
                (unsigned long long)min_ops,
                (unsigned long long)p50_us,
                (unsigned long long)max_p50_us,
                (unsigned long long)p99_us,
                (unsigned long long)max_p99_us,
                (unsigned long long)cursor_x1000,
                (unsigned long long)max_cursor_x1000,
                (unsigned long long)stats.client_submit_pushes,
                (unsigned long long)stats.broker_serve_success,
                (unsigned long long)stats.broker_submit_head_publishes,
                (unsigned long long)stats.broker_complete_tail_publishes,
                (unsigned long long)stats.client_complete_drain_entries,
                (unsigned long long)stats.client_complete_batch_max,
                (unsigned long long)expected_broker_batches);
        goto done;
    }
    rc = 0;

done:
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    free(latency_ns);
    free(submit_ns);
    return rc;
}

#if !LLAM_PLATFORM_WINDOWS
static int test_broker_ring_multiprocess_flood(void) {
    typedef struct broker_ring_flood_shared {
        llam_broker_ring_t ring;
    } broker_ring_flood_shared_t;

    broker_ring_flood_shared_t *shared = NULL;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    llam_broker_ring_stats_t stats;
    const uint64_t flood_iters = test_broker_ring_flood_iters();
    uint64_t next_submit = 1U;
    uint64_t next_complete = 1U;
    pid_t child;
    int status = 0;
    int rc = -1;

    shared = mmap(NULL,
                  sizeof(*shared),
                  PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS,
                  -1,
                  0);
    if (shared == MAP_FAILED) {
        return -1;
    }
    if (llam_broker_ring_init(&shared->ring) != 0) {
        goto done;
    }

    child = fork();
    if (child < 0) {
        goto done;
    }
    if (child == 0) {
        uint64_t served = 0U;

        while (served < flood_iters) {
            if (llam_broker_ring_submit_pop(&shared->ring, &submission) == 0) {
                memset(&completion, 0, sizeof(completion));
                completion.request_id = submission.request_id;
                completion.status = 0;
                completion.result0 = submission.request_id ^ UINT64_C(0x5a5a5a5a5a5a5a5a);
                for (;;) {
                    if (llam_broker_ring_complete_push(&shared->ring, &completion) == 0) {
                        break;
                    }
                    if (errno != EAGAIN) {
                        _exit(3);
                    }
                    sched_yield();
                }
                ++served;
                continue;
            }
            if (errno != EAGAIN) {
                _exit(2);
            }
            sched_yield();
        }
        _exit(0);
    }

    /*
     * This is a real process-shared ring stress: the parent owns submit/drain
     * cursors and the child owns pop/complete cursors. It catches cursor window
     * corruption and false-sharing-sensitive publication regressions without
     * involving broker policy code.
     */
    while (next_complete <= flood_iters) {
        bool progressed = false;

        while (next_submit <= flood_iters) {
            memset(&submission, 0, sizeof(submission));
            submission.request_id = next_submit;
            submission.op = LLAM_BROKER_RING_OP_NOP;
            if (llam_broker_ring_submit_push(&shared->ring, &submission) == 0) {
                ++next_submit;
                progressed = true;
                continue;
            }
            if (errno == EAGAIN) {
                break;
            }
            goto wait_child;
        }

        while (next_complete <= flood_iters) {
            if (llam_broker_ring_complete_pop(&shared->ring, &completion) == 0) {
                if (completion.request_id != next_complete ||
                    completion.status != 0 ||
                    completion.result0 != (next_complete ^ UINT64_C(0x5a5a5a5a5a5a5a5a))) {
                    goto wait_child;
                }
                ++next_complete;
                progressed = true;
                continue;
            }
            if (errno == EAGAIN) {
                break;
            }
            goto wait_child;
        }

        if (!progressed) {
            sched_yield();
        }
    }
    rc = 0;

wait_child:
    if (rc != 0) {
        (void)kill(child, SIGTERM);
    }
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        rc = -1;
    }
    if (rc == 0 &&
        (llam_broker_ring_collect_stats(&shared->ring, &stats) != 0 ||
         stats.client_submit_pushes != flood_iters ||
         stats.broker_complete_tail_publishes != flood_iters ||
         stats.client_complete_drain_entries != flood_iters ||
         stats.cursor_write_estimate < flood_iters * 2U)) {
        rc = -1;
    }

done:
    if (shared != NULL && shared != MAP_FAILED) {
        munmap(shared, sizeof(*shared));
    }
    return rc;
}

static int test_broker_ring_multiprocess_session_replay_guard(void) {
    enum { MAX_PHASE_SPINS = 1000000U };
    typedef struct broker_ring_session_shared {
        llam_broker_ring_t ring;
        atomic_uint phase;
    } broker_ring_session_shared_t;

    broker_ring_session_shared_t *shared = NULL;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    const uint64_t replay_iters = test_broker_ring_replay_iters();
    pid_t child;
    int status = 0;
    int rc = -1;
    uint64_t next_submit = 1U;
    uint64_t next_complete = 1U;

    shared = mmap(NULL,
                  sizeof(*shared),
                  PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS,
                  -1,
                  0);
    if (shared == MAP_FAILED) {
        return -1;
    }
    if (llam_broker_ring_init(&shared->ring) != 0) {
        goto done;
    }
    atomic_init(&shared->phase, 0U);

    child = fork();
    if (child < 0) {
        goto done;
    }
    if (child == 0) {
        llam_runtime_opts_t opts;
        llam_broker_t broker;
        bool broker_initialized = false;
        uint64_t served = 0U;
        unsigned spins;

        if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
            _exit(2);
        }
        opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
        if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
            _exit(2);
        }
        broker_initialized = true;

        while (served < replay_iters) {
            if (llam_broker_ring_serve_one(&broker, &shared->ring) == 0) {
                ++served;
                continue;
            }
            if (errno != EAGAIN) {
                if (broker_initialized) {
                    llam_broker_destroy(&broker);
                }
                _exit(3);
            }
            sched_yield();
        }

        /*
         * The parent rewinds the client-visible cursors after the broker has
         * served the flood. A real broker session must reject that stale replay
         * from its private cursor even though the ring memory is process-shared.
         */
        for (spins = 0U; spins < MAX_PHASE_SPINS; ++spins) {
            if (atomic_load_explicit(&shared->phase, memory_order_acquire) == 1U) {
                break;
            }
            sched_yield();
        }
        if (spins == MAX_PHASE_SPINS) {
            if (broker_initialized) {
                llam_broker_destroy(&broker);
            }
            _exit(4);
        }

        errno = 0;
        if (expect_errno(llam_broker_ring_serve_one(&broker, &shared->ring),
                         EINVAL,
                         "cross-process broker session replayed stale cursor") != 0) {
            if (broker_initialized) {
                llam_broker_destroy(&broker);
            }
            _exit(5);
        }
        if (broker_initialized) {
            llam_broker_destroy(&broker);
        }
        _exit(0);
    }

    while (next_complete <= replay_iters) {
        bool progressed = false;

        while (next_submit <= replay_iters) {
            memset(&submission, 0, sizeof(submission));
            submission.request_id = next_submit;
            submission.op = LLAM_BROKER_RING_OP_NOP;
            if (llam_broker_ring_submit_push(&shared->ring, &submission) == 0) {
                ++next_submit;
                progressed = true;
                continue;
            }
            if (errno == EAGAIN) {
                break;
            }
            goto wait_child;
        }

        while (next_complete <= replay_iters) {
            if (llam_broker_ring_complete_pop(&shared->ring, &completion) == 0) {
                if (completion.request_id != next_complete || completion.status != 0) {
                    goto wait_child;
                }
                ++next_complete;
                progressed = true;
                continue;
            }
            if (errno == EAGAIN) {
                break;
            }
            goto wait_child;
        }

        if (!progressed) {
            sched_yield();
        }
    }

    atomic_store_explicit(&shared->ring.submit_head.value, 0U, memory_order_relaxed);
    atomic_store_explicit(&shared->ring.submit_tail.value, 1U, memory_order_release);
    atomic_store_explicit(&shared->phase, 1U, memory_order_release);
    rc = 0;

wait_child:
    if (rc != 0) {
        (void)kill(child, SIGTERM);
    }
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        rc = -1;
    }

done:
    if (shared != NULL && shared != MAP_FAILED) {
        munmap(shared, sizeof(*shared));
    }
    return rc;
}

static int test_broker_ring_multiprocess_teardown_guard(void) {
    enum {
        TEARDOWN_SERVED = 64U,
        TEARDOWN_SUBMITTED = 128U,
        MAX_DRAIN_SPINS = 1000000U
    };
    typedef struct broker_ring_teardown_shared {
        llam_broker_ring_t ring;
    } broker_ring_teardown_shared_t;

    broker_ring_teardown_shared_t *shared = NULL;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    llam_broker_ring_stats_t stats;
    pid_t child;
    int status = 0;
    int rc = -1;
    uint64_t next_submit;
    uint64_t next_complete = 1U;
    unsigned spins;

    shared = mmap(NULL,
                  sizeof(*shared),
                  PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS,
                  -1,
                  0);
    if (shared == MAP_FAILED) {
        return -1;
    }
    if (llam_broker_ring_init(&shared->ring) != 0) {
        goto done;
    }

    child = fork();
    if (child < 0) {
        goto done;
    }
    if (child == 0) {
        llam_runtime_opts_t opts;
        llam_broker_t broker;
        bool broker_initialized = false;
        uint64_t served = 0U;

        if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
            _exit(2);
        }
        opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
        if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
            _exit(2);
        }
        broker_initialized = true;

        while (served < TEARDOWN_SERVED) {
            if (llam_broker_ring_serve_one(&broker, &shared->ring) == 0) {
                ++served;
                continue;
            }
            if (errno != EAGAIN) {
                if (broker_initialized) {
                    llam_broker_destroy(&broker);
                }
                _exit(3);
            }
            sched_yield();
        }
        if (broker_initialized) {
            llam_broker_destroy(&broker);
        }
        _exit(0);
    }

    /*
     * Leave half the submitted requests unserved when the broker process exits.
     * The client side must be able to drain the completed prefix, observe normal
     * child teardown, and unmap the ring without waiting forever on stale slots.
     */
    for (next_submit = 1U; next_submit <= TEARDOWN_SUBMITTED; ++next_submit) {
        memset(&submission, 0, sizeof(submission));
        submission.request_id = next_submit;
        submission.op = LLAM_BROKER_RING_OP_NOP;
        while (llam_broker_ring_submit_push(&shared->ring, &submission) != 0) {
            if (errno != EAGAIN) {
                goto wait_child;
            }
            sched_yield();
        }
    }

    for (spins = 0U; next_complete <= TEARDOWN_SERVED && spins < MAX_DRAIN_SPINS; ++spins) {
        if (llam_broker_ring_complete_pop(&shared->ring, &completion) == 0) {
            if (completion.request_id != next_complete || completion.status != 0) {
                goto wait_child;
            }
            ++next_complete;
            continue;
        }
        if (errno != EAGAIN) {
            goto wait_child;
        }
        sched_yield();
    }
    if (next_complete <= TEARDOWN_SERVED) {
        goto wait_child;
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_complete_pop(&shared->ring, &completion),
                     EAGAIN,
                     "teardown guard found unexpected completion after broker exit") != 0) {
        goto wait_child;
    }
    rc = 0;

wait_child:
    if (rc != 0) {
        (void)kill(child, SIGTERM);
    }
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        rc = -1;
    }
    if (rc == 0 &&
        (llam_broker_ring_collect_stats(&shared->ring, &stats) != 0 ||
         stats.client_submit_pushes != TEARDOWN_SUBMITTED ||
         stats.broker_serve_success != TEARDOWN_SERVED ||
         stats.client_complete_drain_entries != TEARDOWN_SERVED)) {
        rc = -1;
    }

done:
    if (shared != NULL && shared != MAP_FAILED) {
        munmap(shared, sizeof(*shared));
    }
    return rc;
}
#endif

static int test_broker_ring_capability_validate_op(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_capability_token_t token;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    llam_broker_ring_stats_t stats;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_ring_init(&ring) != 0 ||
        llam_broker_create_channel(&broker, 2U, LLAM_CAP_RIGHT_SEND, &token) != 0) {
        goto done;
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 1U;
    submission.op = LLAM_BROKER_RING_OP_CAP_VALIDATE;
    submission.arg0 = LLAM_CAP_RIGHT_SEND;
    submission.token = token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status != 0 ||
        completion.result0 != 1U) {
        goto done;
    }
    if (llam_broker_ring_collect_stats(&ring, &stats) != 0 ||
        stats.broker_serve_calls != 1U ||
        stats.broker_serve_success != 1U ||
        stats.broker_submit_head_publishes != 1U ||
        stats.broker_complete_tail_publishes != 1U ||
        stats.client_complete_head_publishes != 1U ||
        stats.cursor_write_estimate != 4U) {
        goto done;
    }

    /*
     * The client can write shared counters. Broker serving must use private
     * cursors and reject a broker-published cursor mismatch instead of treating
     * the corrupted window as an empty queue or replaying a previous request.
     */
    atomic_store_explicit(&ring.submit_head.value, 0U, memory_order_relaxed);
    atomic_store_explicit(&ring.submit_tail.value, 1U, memory_order_relaxed);
    errno = 0;
    if (expect_errno(llam_broker_ring_serve_one(&broker, &ring),
                     EINVAL,
                     "broker accepted submit_head rewind against private cursor") != 0) {
        goto done;
    }
    atomic_store_explicit(&ring.submit_tail.value, 0U, memory_order_relaxed);
    errno = 0;
    if (expect_errno(llam_broker_ring_serve_one(&broker, &ring),
                     EINVAL,
                     "broker accepted submit_tail behind private cursor") != 0) {
        goto done;
    }
    atomic_store_explicit(&ring.submit_head.value, 1U, memory_order_relaxed);
    atomic_store_explicit(&ring.submit_tail.value, 1U, memory_order_relaxed);

    submission.request_id = 2U;
    submission.token = token;
    submission.token.rights |= LLAM_CAP_RIGHT_DESTROY;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status == 0 ||
        completion.error_code != EACCES) {
        goto done;
    }

    errno = 0;
    if (expect_errno(llam_broker_ring_serve_one(&broker, &ring),
                     EAGAIN,
                     "empty broker ring serve accepted work") != 0) {
        goto done;
    }

    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_ring_capability_attenuate_op(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_capability_token_t token;
    llam_capability_token_t attenuated;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_ring_init(&ring) != 0 ||
        llam_broker_create_channel(&broker,
                                   2U,
                                   LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_RECV,
                                   &token) != 0) {
        goto done;
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 1U;
    submission.op = LLAM_BROKER_RING_OP_CAP_ATTENUATE;
    submission.arg0 = LLAM_CAP_RIGHT_SEND;
    submission.arg2 = 0U;
    submission.token = token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status != 0 ||
        completion.result0 != sizeof(attenuated)) {
        goto done;
    }
    memcpy(&attenuated, ring.data, sizeof(attenuated));
    if (llam_broker_validate_cap(&broker, &attenuated, LLAM_CAP_RIGHT_SEND) != 0) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_validate_cap(&broker, &attenuated, LLAM_CAP_RIGHT_RECV),
                     EACCES,
                     "ring attenuation preserved dropped recv right") != 0) {
        goto done;
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 2U;
    submission.op = LLAM_BROKER_RING_OP_CAP_ATTENUATE;
    submission.arg0 = LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_DESTROY;
    submission.arg2 = 128U;
    submission.token = attenuated;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status == 0 ||
        completion.error_code != EACCES) {
        goto done;
    }
    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_ring_capability_revoke_op(void) {
    static const char initial[] = "ring revoke";
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_capability_token_t token;
    llam_capability_token_t replacement;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    char out[sizeof(initial)];
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_ring_init(&ring) != 0 ||
        llam_broker_register_buffer(&broker,
                                    initial,
                                    sizeof(initial),
                                    LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_DESTROY,
                                    &token) != 0) {
        goto done;
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 1U;
    submission.op = LLAM_BROKER_RING_OP_CAP_REVOKE;
    submission.arg0 = LLAM_CAP_RIGHT_READ;
    submission.arg2 = 0U;
    submission.token = token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status != 0 ||
        completion.result0 != sizeof(replacement)) {
        goto done;
    }
    memcpy(&replacement, ring.data, sizeof(replacement));
    errno = 0;
    if (expect_errno(llam_broker_read_buffer(&broker, &token, 0U, out, sizeof(out)),
                     EACCES,
                     "ring revoke left old token usable") != 0) {
        goto done;
    }
    if (llam_broker_read_buffer(&broker, &replacement, 0U, out, sizeof(out)) != 0 ||
        memcmp(out, initial, sizeof(initial)) != 0) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_validate_cap(&broker, &token, LLAM_CAP_RIGHT_READ),
                     EACCES,
                     "ring revoke left old token structurally valid") != 0) {
        goto done;
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 2U;
    submission.op = LLAM_BROKER_RING_OP_CAP_REVOKE;
    submission.arg0 = LLAM_CAP_RIGHT_READ;
    submission.arg2 = 128U;
    submission.token = replacement;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status == 0 ||
        completion.error_code != EACCES) {
        goto done;
    }
    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_ring_failed_output_windows_are_cleared(void) {
    static const char initial[] = "clear failed ring output";
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_capability_token_t buffer_token;
    llam_capability_token_t send_only_channel;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_ring_init(&ring) != 0 ||
        llam_broker_register_buffer(&broker,
                                    initial,
                                    sizeof(initial),
                                    LLAM_CAP_RIGHT_READ,
                                    &buffer_token) != 0 ||
        llam_broker_create_channel(&broker,
                                   1U,
                                   LLAM_CAP_RIGHT_SEND,
                                   &send_only_channel) != 0) {
        goto done;
    }

    /*
     * These are output-producing ring operations. On failure the completion is
     * authoritative, but clearing a validated output window prevents a buggy or
     * malicious client from treating stale shared-memory bytes as fresh broker
     * output after ignoring the failed completion.
     */
    memset(ring.data + 64U, 0xa5, sizeof(llam_capability_token_t));
    memset(&submission, 0, sizeof(submission));
    submission.request_id = 301U;
    submission.op = LLAM_BROKER_RING_OP_CAP_ATTENUATE;
    submission.arg0 = LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_WRITE;
    submission.arg2 = 64U;
    submission.token = buffer_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EACCES ||
        !memory_is_byte(ring.data + 64U, sizeof(llam_capability_token_t), 0U)) {
        goto done;
    }

    (void)llam_broker_revoke_all(&broker);
    memset(ring.data + 192U, 0xa5, 16U);
    memset(&submission, 0, sizeof(submission));
    submission.request_id = 302U;
    submission.op = LLAM_BROKER_RING_OP_BUFFER_READ;
    submission.arg0 = 0U;
    submission.arg1 = 16U;
    submission.arg2 = 192U;
    submission.token = buffer_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EACCES ||
        !memory_is_byte(ring.data + 192U, 16U, 0U)) {
        goto done;
    }

    memset(ring.data + 256U, 0xa5, sizeof(llam_capability_token_t));
    memset(&submission, 0, sizeof(submission));
    submission.request_id = 303U;
    submission.op = LLAM_BROKER_RING_OP_TASK_SPAWN;
    submission.arg0 = (uint64_t)UINT32_MAX + (uint64_t)LLAM_BROKER_TASK_KIND_RETURN_U64 + 1U;
    submission.arg2 = 256U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EINVAL ||
        !memory_is_byte(ring.data + 256U, sizeof(llam_capability_token_t), 0U)) {
        goto done;
    }

    memset(ring.data + 384U, 0xa5, 8U);
    memset(&submission, 0, sizeof(submission));
    submission.request_id = 304U;
    submission.op = LLAM_BROKER_RING_OP_CHANNEL_RECV;
    submission.arg1 = 8U;
    submission.arg2 = 384U;
    submission.token = send_only_channel;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EACCES ||
        !memory_is_byte(ring.data + 384U, 8U, 0U)) {
        goto done;
    }

    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_ring_malformed_submissions_fail_closed(void) {
    typedef struct broker_ring_malformed_case {
        uint32_t op;
        size_t output_len;
        uint64_t arg1;
    } broker_ring_malformed_case_t;
    static const broker_ring_malformed_case_t cases[] = {
        {LLAM_BROKER_RING_OP_CAP_VALIDATE, 0U, LLAM_CAP_RIGHT_READ},
        {LLAM_BROKER_RING_OP_CAP_ATTENUATE, sizeof(llam_capability_token_t), 0U},
        {LLAM_BROKER_RING_OP_CAP_REVOKE, sizeof(llam_capability_token_t), 0U},
        {LLAM_BROKER_RING_OP_BUFFER_READ, 16U, 16U},
        {LLAM_BROKER_RING_OP_BUFFER_WRITE, 0U, 16U},
        {LLAM_BROKER_RING_OP_DESCRIPTOR_READ, 16U, 16U},
        {LLAM_BROKER_RING_OP_DESCRIPTOR_WRITE, 0U, 16U},
        {LLAM_BROKER_RING_OP_CHANNEL_SEND, 0U, 16U},
        {LLAM_BROKER_RING_OP_CHANNEL_RECV, 16U, 16U},
        {LLAM_BROKER_RING_OP_CHANNEL_CLOSE, 0U, 0U},
        {LLAM_BROKER_RING_OP_TASK_SPAWN, sizeof(llam_capability_token_t), 0U},
        {LLAM_BROKER_RING_OP_TASK_JOIN, 0U, 0U},
        {LLAM_BROKER_RING_OP_TASK_DETACH, 0U, 0U},
        {UINT32_C(0xfffffff0), 0U, 0U},
        {UINT32_C(0xffffffff), 0U, 0U},
    };
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_ring_init(&ring) != 0) {
        goto done;
    }

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const broker_ring_malformed_case_t *item = &cases[i];
        llam_broker_ring_submission_t submission;
        llam_broker_ring_completion_t completion;
        size_t output_offset = 128U + (i * 128U);

        if (LLAM_UNLIKELY(item->output_len > 0U &&
                          output_offset + item->output_len > LLAM_BROKER_RING_DATA_BYTES)) {
            goto done;
        }
        /*
         * Use valid output windows with invalid authority. This proves failure
         * completions cannot publish stale result fields and output-producing
         * op handlers clear the exact client-visible window before completion.
         */
        if (item->output_len > 0U) {
            memset(ring.data + output_offset, 0xa5, item->output_len);
        }
        memset(&submission, 0, sizeof(submission));
        submission.request_id = UINT64_C(9000) + (uint64_t)i;
        submission.op = item->op;
        submission.arg0 = item->op == LLAM_BROKER_RING_OP_TASK_SPAWN
            ? ((uint64_t)UINT32_MAX + 1U)
            : LLAM_CAP_RIGHT_READ;
        submission.arg1 = item->arg1;
        submission.arg2 = (uint64_t)output_offset;
        memset(&completion, 0x5a, sizeof(completion));
        if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
            llam_broker_ring_serve_one(&broker, &ring) != 0 ||
            llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
            completion.request_id != submission.request_id ||
            !broker_ring_failure_completion_outputs_are_clear(&completion)) {
            fprintf(stderr,
                    "[test_security_capability] malformed ring op %u leaked completion authority iter=%zu\n",
                    item->op,
                    i);
            goto done;
        }
        if (item->output_len > 0U &&
            !memory_is_byte(ring.data + output_offset, item->output_len, 0U)) {
            fprintf(stderr,
                    "[test_security_capability] malformed ring op %u left stale output window iter=%zu\n",
                    item->op,
                    i);
            goto done;
        }
    }

    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_ring_buffer_data_plane(void) {
    static const char initial[] = "hello secure broker";
    static const char patch[] = "ringed";
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_capability_token_t token;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    char buffer[sizeof(initial)];
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_ring_init(&ring) != 0 ||
        llam_broker_register_buffer(&broker,
                                    initial,
                                    sizeof(initial),
                                    LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_WRITE,
                                    &token) != 0) {
        goto done;
    }

    memcpy(ring.data + 256U, patch, sizeof(patch) - 1U);
    memset(&submission, 0, sizeof(submission));
    submission.request_id = 10U;
    submission.op = LLAM_BROKER_RING_OP_BUFFER_WRITE;
    submission.arg0 = 6U;
    submission.arg1 = sizeof(patch) - 1U;
    submission.arg2 = 256U;
    submission.token = token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != sizeof(patch) - 1U) {
        goto done;
    }
    if (llam_broker_read_buffer(&broker, &token, 0U, buffer, sizeof(buffer)) != 0 ||
        memcmp(buffer, "hello ringed broker", sizeof(initial)) != 0) {
        goto done;
    }

    memset(ring.data + 512U, 0, sizeof(initial));
    submission.request_id = 11U;
    submission.op = LLAM_BROKER_RING_OP_BUFFER_READ;
    submission.arg0 = 0U;
    submission.arg1 = sizeof(initial);
    submission.arg2 = 512U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != sizeof(initial) ||
        memcmp(ring.data + 512U, "hello ringed broker", sizeof(initial)) != 0) {
        goto done;
    }

    submission.request_id = 12U;
    submission.op = LLAM_BROKER_RING_OP_BUFFER_READ;
    submission.arg0 = 0U;
    submission.arg1 = 16U;
    submission.arg2 = LLAM_BROKER_RING_DATA_BYTES - 8U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EINVAL) {
        goto done;
    }

    (void)llam_broker_revoke_all(&broker);
    submission.request_id = 13U;
    submission.op = LLAM_BROKER_RING_OP_BUFFER_READ;
    submission.arg0 = 0U;
    submission.arg1 = 1U;
    submission.arg2 = 0U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EACCES) {
        goto done;
    }

    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_ring_channel_data_plane(void) {
    static const char first[] = "broker channel one";
    static const char second[] = "broker channel two";
    static const char third[] = "broker channel three";
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_capability_token_t token;
    llam_capability_token_t recv_only;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_ring_init(&ring) != 0 ||
        llam_broker_create_channel(&broker,
                                   2U,
                                   LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_RECV | LLAM_CAP_RIGHT_CLOSE,
                                   &token) != 0) {
        goto done;
    }

    memcpy(ring.data + 32U, first, sizeof(first));
    memset(&submission, 0, sizeof(submission));
    submission.request_id = 40U;
    submission.op = LLAM_BROKER_RING_OP_CHANNEL_SEND;
    submission.arg1 = sizeof(first);
    submission.arg2 = 32U;
    submission.token = token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != sizeof(first)) {
        goto done;
    }

    memcpy(ring.data + 64U, second, sizeof(second));
    submission.request_id = 41U;
    submission.arg1 = sizeof(second);
    submission.arg2 = 64U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != sizeof(second)) {
        goto done;
    }

    memcpy(ring.data + 96U, third, sizeof(third));
    submission.request_id = 42U;
    submission.arg1 = sizeof(third);
    submission.arg2 = 96U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EAGAIN) {
        goto done;
    }

    memset(ring.data + 128U, 0xa5, sizeof(first) + 8U);
    submission.request_id = 43U;
    submission.op = LLAM_BROKER_RING_OP_CHANNEL_RECV;
    submission.arg1 = sizeof(first) + 8U;
    submission.arg2 = 128U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != sizeof(first) ||
        memcmp(ring.data + 128U, first, sizeof(first)) != 0 ||
        !memory_is_byte(ring.data + 128U + sizeof(first), 8U, 0U)) {
        goto done;
    }

    if (llam_capability_attenuate(&broker.capability_key,
                                  &token,
                                  LLAM_CAP_RIGHT_RECV,
                                  llam_broker_revocation_epoch(&broker),
                                  &recv_only) != 0) {
        goto done;
    }
    memcpy(ring.data + 160U, third, sizeof(third));
    submission.request_id = 44U;
    submission.op = LLAM_BROKER_RING_OP_CHANNEL_SEND;
    submission.arg1 = sizeof(third);
    submission.arg2 = 160U;
    submission.token = recv_only;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EACCES) {
        goto done;
    }

    submission.request_id = 45U;
    submission.op = LLAM_BROKER_RING_OP_CHANNEL_CLOSE;
    submission.token = token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0) {
        goto done;
    }

    submission.request_id = 46U;
    submission.op = LLAM_BROKER_RING_OP_CHANNEL_SEND;
    submission.arg1 = sizeof(third);
    submission.arg2 = 160U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EPIPE) {
        goto done;
    }

    memset(ring.data + 192U, 0, sizeof(second));
    submission.request_id = 47U;
    submission.op = LLAM_BROKER_RING_OP_CHANNEL_RECV;
    submission.arg1 = sizeof(second);
    submission.arg2 = 192U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != sizeof(second) ||
        memcmp(ring.data + 192U, second, sizeof(second)) != 0) {
        goto done;
    }

    submission.request_id = 48U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EPIPE) {
        goto done;
    }

    (void)llam_broker_revoke_all(&broker);
    submission.request_id = 49U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EACCES) {
        goto done;
    }

    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_ring_task_data_plane(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_capability_token_t task_token;
    llam_capability_token_t tampered_token;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_ring_init(&ring) != 0) {
        goto done;
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 50U;
    submission.op = LLAM_BROKER_RING_OP_TASK_SPAWN;
    submission.arg0 = LLAM_BROKER_TASK_KIND_RETURN_U64;
    submission.arg1 = UINT64_C(0x123456789abcdef0);
    submission.arg2 = 256U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != sizeof(task_token)) {
        goto done;
    }
    memcpy(&task_token, ring.data + 256U, sizeof(task_token));

    submission.request_id = 51U;
    submission.op = LLAM_BROKER_RING_OP_TASK_JOIN;
    submission.token = task_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EAGAIN) {
        goto done;
    }

    if (llam_runtime_run_handle(broker.runtime) != 0) {
        goto done;
    }
    submission.request_id = 52U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != UINT64_C(0x123456789abcdef0)) {
        goto done;
    }

    submission.request_id = 53U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EACCES) {
        goto done;
    }

    tampered_token = task_token;
    tampered_token.rights |= LLAM_CAP_RIGHT_ADMIN;
    submission.request_id = 54U;
    submission.token = tampered_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EACCES) {
        goto done;
    }

    submission.request_id = 55U;
    submission.op = LLAM_BROKER_RING_OP_TASK_SPAWN;
    submission.arg0 = UINT32_C(0xffffffff);
    submission.arg1 = 0U;
    submission.arg2 = 0U;
    memset(&submission.token, 0, sizeof(submission.token));
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EINVAL) {
        goto done;
    }

    submission.request_id = 56U;
    submission.op = LLAM_BROKER_RING_OP_TASK_SPAWN;
    submission.arg0 = (uint64_t)UINT32_MAX + (uint64_t)LLAM_BROKER_TASK_KIND_RETURN_U64 + 1U;
    submission.arg1 = UINT64_C(0xdeadbeef);
    submission.arg2 = 0U;
    memset(&submission.token, 0, sizeof(submission.token));
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EINVAL) {
        goto done;
    }

    submission.request_id = 57U;
    submission.op = LLAM_BROKER_RING_OP_TASK_SPAWN;
    submission.arg0 = LLAM_BROKER_TASK_KIND_RETURN_U64;
    submission.arg1 = UINT64_C(0xfedcba9876543210);
    submission.arg2 = 384U;
    memset(&submission.token, 0, sizeof(submission.token));
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != sizeof(task_token)) {
        goto done;
    }
    memcpy(&task_token, ring.data + 384U, sizeof(task_token));

    submission.request_id = 58U;
    submission.op = LLAM_BROKER_RING_OP_TASK_DETACH;
    submission.token = task_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0) {
        goto done;
    }

    submission.request_id = 59U;
    submission.op = LLAM_BROKER_RING_OP_TASK_JOIN;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EACCES) {
        goto done;
    }
    if (llam_runtime_run_handle(broker.runtime) != 0) {
        goto done;
    }

    submission.request_id = 60U;
    submission.op = LLAM_BROKER_RING_OP_TASK_SPAWN;
    submission.arg0 = LLAM_BROKER_TASK_KIND_RETURN_U64;
    submission.arg1 = UINT64_C(0x55aa55aa55aa55aa);
    submission.arg2 = 512U;
    memset(&submission.token, 0, sizeof(submission.token));
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0) {
        goto done;
    }
    memcpy(&task_token, ring.data + 512U, sizeof(task_token));
    if (llam_runtime_run_handle(broker.runtime) != 0) {
        goto done;
    }
    submission.request_id = 61U;
    submission.op = LLAM_BROKER_RING_OP_TASK_JOIN;
    submission.token = task_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != UINT64_C(0x55aa55aa55aa55aa)) {
        goto done;
    }

    submission.request_id = 62U;
    submission.op = LLAM_BROKER_RING_OP_TASK_SPAWN;
    submission.arg0 = LLAM_BROKER_TASK_KIND_INCREMENT_U64;
    submission.arg1 = UINT64_C(0x41);
    submission.arg2 = 640U;
    memset(&submission.token, 0, sizeof(submission.token));
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0) {
        goto done;
    }
    memcpy(&task_token, ring.data + 640U, sizeof(task_token));
    if (llam_runtime_run_handle(broker.runtime) != 0) {
        goto done;
    }
    submission.request_id = 63U;
    submission.op = LLAM_BROKER_RING_OP_TASK_JOIN;
    submission.token = task_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != UINT64_C(0x42)) {
        goto done;
    }

    submission.request_id = 64U;
    submission.op = LLAM_BROKER_RING_OP_TASK_SPAWN;
    submission.arg0 = LLAM_BROKER_TASK_KIND_POPCOUNT_U64;
    submission.arg1 = UINT64_C(0xf0f0f00f);
    submission.arg2 = 768U;
    memset(&submission.token, 0, sizeof(submission.token));
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0) {
        goto done;
    }
    memcpy(&task_token, ring.data + 768U, sizeof(task_token));
    if (llam_runtime_run_handle(broker.runtime) != 0) {
        goto done;
    }
    submission.request_id = 65U;
    submission.op = LLAM_BROKER_RING_OP_TASK_JOIN;
    submission.token = task_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != 16U) {
        goto done;
    }

    submission.request_id = 65U;
    submission.op = LLAM_BROKER_RING_OP_TASK_SPAWN;
    submission.arg0 = LLAM_BROKER_TASK_KIND_SLEEP_NS_RETURN_U64;
    submission.arg1 = 1U;
    submission.arg2 = 896U;
    memset(&submission.token, 0, sizeof(submission.token));
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0) {
        goto done;
    }
    memcpy(&task_token, ring.data + 896U, sizeof(task_token));
    if (llam_runtime_run_handle(broker.runtime) != 0) {
        goto done;
    }
    submission.request_id = 66U;
    submission.op = LLAM_BROKER_RING_OP_TASK_JOIN;
    submission.token = task_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != 1U) {
        goto done;
    }

    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_ring_subject_bound_session(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_capability_token_t task_token;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    const uint64_t subject_id = UINT64_C(0x123456789abc);
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_ring_init(&ring) != 0) {
        goto done;
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 90U;
    submission.op = LLAM_BROKER_RING_OP_TASK_SPAWN;
    submission.arg0 = LLAM_BROKER_TASK_KIND_RETURN_U64;
    submission.arg1 = UINT64_C(0x987654321);
    submission.arg2 = 0U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one_subject(&broker, &ring, subject_id) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status != 0 ||
        completion.result0 != sizeof(task_token)) {
        goto done;
    }
    memcpy(&task_token, ring.data, sizeof(task_token));
    if (task_token.subject_id != subject_id) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_validate_cap(&broker, &task_token, LLAM_CAP_RIGHT_JOIN),
                     EACCES,
                     "ring subject-bound token accepted without ring subject") != 0) {
        goto done;
    }

    /*
     * Once a ring has a broker-side subject, serving it through the legacy
     * subject-0 wrapper or another subject must fail before consuming the
     * pending submission. This keeps shared-memory rings session-bound like
     * local control transports.
     */
    memset(&submission, 0, sizeof(submission));
    submission.request_id = 91U;
    submission.op = LLAM_BROKER_RING_OP_TASK_JOIN;
    submission.token = task_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_serve_one(&broker, &ring),
                     EACCES,
                     "subject-bound broker ring accepted subject-0 serve") != 0) {
        goto done;
    }
    if (llam_broker_ring_serve_one_subject(&broker, &ring, subject_id) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status == 0 ||
        completion.error_code != EAGAIN) {
        goto done;
    }

    if (llam_runtime_run_handle(broker.runtime) != 0) {
        goto done;
    }
    submission.request_id = 92U;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one_subject(&broker, &ring, subject_id) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status != 0 ||
        completion.result0 != UINT64_C(0x987654321)) {
        goto done;
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 93U;
    submission.op = LLAM_BROKER_RING_OP_NOP;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0) {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_serve_one_subject(&broker, &ring, subject_id + 1U),
                     EACCES,
                     "broker ring accepted a different session subject") != 0) {
        goto done;
    }
    if (llam_broker_ring_serve_one_subject(&broker, &ring, subject_id) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status != 0) {
        goto done;
    }

    rc = 0;

done:
    llam_broker_destroy(&broker);
    return rc;
}

#if !LLAM_PLATFORM_WINDOWS
static int test_broker_ring_fd_data_plane(void) {
    static const char inbound[] = "broker fd read";
    static const char outbound[] = "broker fd write";
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_capability_token_t read_token;
    llam_capability_token_t write_token;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    int read_pipe[2] = {-1, -1};
    int write_pipe[2] = {-1, -1};
    char out[sizeof(outbound)];
    ssize_t nread;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_ring_init(&ring) != 0 ||
        pipe(read_pipe) != 0 ||
        pipe(write_pipe) != 0) {
        goto done;
    }
    if (write(read_pipe[1], inbound, sizeof(inbound)) != (ssize_t)sizeof(inbound)) {
        goto done;
    }
    if (llam_broker_register_fd(&broker, read_pipe[0], LLAM_CAP_RIGHT_READ, false, &read_token) != 0 ||
        llam_broker_register_fd(&broker, write_pipe[1], LLAM_CAP_RIGHT_WRITE, false, &write_token) != 0) {
        goto done;
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 20U;
    submission.op = LLAM_BROKER_RING_OP_DESCRIPTOR_READ;
    memset(ring.data + 64U, 0xa5, sizeof(inbound) + 8U);
    submission.arg1 = sizeof(inbound) + 8U;
    submission.arg2 = 64U;
    submission.token = read_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != sizeof(inbound) ||
        memcmp(ring.data + 64U, inbound, sizeof(inbound)) != 0 ||
        !memory_is_byte(ring.data + 64U + sizeof(inbound), 8U, 0U)) {
        goto done;
    }

    memcpy(ring.data + 128U, outbound, sizeof(outbound));
    submission.request_id = 21U;
    submission.op = LLAM_BROKER_RING_OP_DESCRIPTOR_WRITE;
    submission.arg1 = sizeof(outbound);
    submission.arg2 = 128U;
    submission.token = write_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != sizeof(outbound)) {
        goto done;
    }
    nread = read(write_pipe[0], out, sizeof(out));
    if (nread != (ssize_t)sizeof(out) || memcmp(out, outbound, sizeof(out)) != 0) {
        goto done;
    }

    submission.request_id = 22U;
    submission.op = LLAM_BROKER_RING_OP_DESCRIPTOR_WRITE;
    submission.arg1 = 1U;
    submission.arg2 = 0U;
    submission.token = read_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EACCES) {
        goto done;
    }

    (void)llam_broker_revoke_all(&broker);
    submission.request_id = 23U;
    submission.op = LLAM_BROKER_RING_OP_DESCRIPTOR_READ;
    submission.arg1 = 1U;
    submission.arg2 = 0U;
    submission.token = read_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EACCES) {
        goto done;
    }

    rc = 0;

done:
    if (read_pipe[0] >= 0) {
        close(read_pipe[0]);
    }
    if (read_pipe[1] >= 0) {
        close(read_pipe[1]);
    }
    if (write_pipe[0] >= 0) {
        close(write_pipe[0]);
    }
    if (write_pipe[1] >= 0) {
        close(write_pipe[1]);
    }
    llam_broker_destroy(&broker);
    return rc;
}

typedef struct broker_ring_serve_thread_state {
    llam_broker_t *broker;
    llam_broker_ring_t *ring;
    int rc;
    int error_code;
} broker_ring_serve_thread_state_t;

typedef struct broker_ring_session_serve_thread_state {
    llam_broker_t *broker;
    uint64_t session_id;
    uint64_t subject_id;
    int rc;
    int error_code;
} broker_ring_session_serve_thread_state_t;

typedef struct broker_destroy_thread_state {
    llam_broker_t *broker;
} broker_destroy_thread_state_t;

typedef struct broker_buffer_read_thread_state {
    llam_broker_t *broker;
    const llam_capability_token_t *token;
    char *out;
    size_t out_size;
    int rc;
    int error_code;
} broker_buffer_read_thread_state_t;

static void *broker_ring_serve_thread(void *arg) {
    broker_ring_serve_thread_state_t *state = (broker_ring_serve_thread_state_t *)arg;

    state->rc = llam_broker_ring_serve_one(state->broker, state->ring);
    state->error_code = state->rc == 0 ? 0 : errno;
    return NULL;
}

static void *broker_ring_session_serve_thread(void *arg) {
    broker_ring_session_serve_thread_state_t *state = (broker_ring_session_serve_thread_state_t *)arg;

    state->rc = llam_broker_ring_serve_session(state->broker, state->session_id, state->subject_id);
    state->error_code = state->rc == 0 ? 0 : errno;
    return NULL;
}

static void *broker_destroy_thread(void *arg) {
    broker_destroy_thread_state_t *state = (broker_destroy_thread_state_t *)arg;

    llam_broker_destroy(state->broker);
    return NULL;
}

static void *broker_buffer_read_thread(void *arg) {
    broker_buffer_read_thread_state_t *state = (broker_buffer_read_thread_state_t *)arg;

    state->rc = llam_broker_read_buffer(state->broker, state->token, 0U, state->out, state->out_size);
    state->error_code = state->rc == 0 ? 0 : errno;
    return NULL;
}

static uint32_t broker_test_active_ops(llam_broker_t *broker) {
    uint32_t active_ops = 0U;

    if (llam_broker_lock(broker) == 0) {
        active_ops = broker->active_ops;
        llam_broker_unlock(broker);
    }
    return active_ops;
}

static bool broker_test_ring_session_busy(llam_broker_t *broker, uint64_t session_id) {
    bool busy = false;

    if (session_id == 0U || session_id > LLAM_BROKER_RING_SESSIONS) {
        return false;
    }
    if (llam_broker_lock(broker) == 0) {
        const llam_broker_ring_session_t *session = &broker->ring_sessions[(size_t)session_id - 1U];

        busy = session->active && session->busy;
        llam_broker_unlock(broker);
    }
    return busy;
}

static bool broker_test_destroying(llam_broker_t *broker) {
    bool destroying = false;

    if (llam_broker_lock(broker) == 0) {
        destroying = broker->destroying;
        llam_broker_unlock(broker);
    }
    return destroying;
}

static int test_broker_nested_op_survives_destroy_start(void) {
    static const char initial[] = "nested op";
    char out[sizeof(initial)];
    char external_out[sizeof(initial)];
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_token_t token;
    broker_destroy_thread_state_t destroy_state;
    broker_buffer_read_thread_state_t external_read_state;
    pthread_t destroy_thread;
    pthread_t external_read_thread;
    bool broker_initialized = false;
    bool active_op = false;
    bool destroy_started = false;
    bool external_read_started = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;
    if (llam_broker_register_buffer(&broker,
                                    initial,
                                    sizeof(initial),
                                    LLAM_CAP_RIGHT_READ,
                                    &token) != 0) {
        goto done;
    }
    if (llam_broker_begin_op(&broker) != 0) {
        goto done;
    }
    active_op = true;
    destroy_state.broker = &broker;
    if (pthread_create(&destroy_thread, NULL, broker_destroy_thread, &destroy_state) != 0) {
        goto done;
    }
    destroy_started = true;

    /*
     * The outer op models an already accepted transport/ring request. Once
     * destroy starts, that accepted request must still be able to call broker
     * helpers internally; only new external requests should fail closed.
     */
    for (unsigned i = 0U; i < 100000U; ++i) {
        if (broker_test_destroying(&broker)) {
            break;
        }
        sched_yield();
    }
    if (!broker_test_destroying(&broker)) {
        goto done;
    }
    memset(external_out, 0, sizeof(external_out));
    memset(&external_read_state, 0, sizeof(external_read_state));
    external_read_state.broker = &broker;
    external_read_state.token = &token;
    external_read_state.out = external_out;
    external_read_state.out_size = sizeof(external_out);
    if (pthread_create(&external_read_thread, NULL, broker_buffer_read_thread, &external_read_state) != 0) {
        goto done;
    }
    external_read_started = true;
    (void)pthread_join(external_read_thread, NULL);
    external_read_started = false;
    if (external_read_state.rc == 0 || external_read_state.error_code != EINVAL) {
        goto done;
    }
    memset(out, 0, sizeof(out));
    if (llam_broker_read_buffer(&broker, &token, 0U, out, sizeof(out)) != 0 ||
        memcmp(out, initial, sizeof(initial)) != 0) {
        goto done;
    }
    llam_broker_end_op(&broker);
    active_op = false;
    (void)pthread_join(destroy_thread, NULL);
    destroy_started = false;
    broker_initialized = false;
    rc = 0;

done:
    if (active_op) {
        llam_broker_end_op(&broker);
    }
    if (external_read_started) {
        (void)pthread_join(external_read_thread, NULL);
    }
    if (destroy_started) {
        (void)pthread_join(destroy_thread, NULL);
        broker_initialized = false;
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_nested_dispatch_survives_destroy_start(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    broker_destroy_thread_state_t destroy_state;
    pthread_t destroy_thread;
    bool broker_initialized = false;
    bool active_op = false;
    bool destroy_started = false;
    bool should_close = true;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;
    if (llam_broker_begin_op(&broker) != 0) {
        goto done;
    }
    active_op = true;
    destroy_state.broker = &broker;
    if (pthread_create(&destroy_thread, NULL, broker_destroy_thread, &destroy_state) != 0) {
        goto done;
    }
    destroy_started = true;

    /*
     * This models a transport request accepted just before teardown. The outer
     * active-op pin must allow the dispatcher to finish a bounded operation;
     * otherwise destroy can make an already accepted request fail mid-flight.
     */
    for (unsigned i = 0U; i < 100000U; ++i) {
        if (broker_test_destroying(&broker)) {
            break;
        }
        sched_yield();
    }
    if (!broker_test_destroying(&broker)) {
        goto done;
    }
    request_init(&request, LLAM_BROKER_WIRE_OP_PING);
    memset(&response, 0x5a, sizeof(response));
    llam_broker_process_request(&broker, &request, &response, &should_close);
    if (response.status != 0 || response.error_code != 0 || should_close) {
        goto done;
    }
    llam_broker_end_op(&broker);
    active_op = false;
    (void)pthread_join(destroy_thread, NULL);
    destroy_started = false;
    broker_initialized = false;
    rc = 0;

done:
    if (active_op) {
        llam_broker_end_op(&broker);
    }
    if (destroy_started) {
        (void)pthread_join(destroy_thread, NULL);
        broker_initialized = false;
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_nested_ring_create_survives_destroy_start(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    broker_destroy_thread_state_t destroy_state;
    pthread_t destroy_thread;
    llam_handle_t descriptor = LLAM_INVALID_HANDLE;
    uint64_t session_id = 0U;
    bool broker_initialized = false;
    bool active_op = false;
    bool destroy_started = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;
    if (llam_broker_begin_op(&broker) != 0) {
        goto done;
    }
    active_op = true;
    destroy_state.broker = &broker;
    if (pthread_create(&destroy_thread, NULL, broker_destroy_thread, &destroy_state) != 0) {
        goto done;
    }
    destroy_started = true;

    /*
     * CREATE_RING allocates transport authority, then registers a private ring
     * session. It is still a bounded accepted operation and must not fail only
     * because broker destroy started after the transport active-op was claimed.
     */
    for (unsigned i = 0U; i < 100000U; ++i) {
        if (broker_test_destroying(&broker)) {
            break;
        }
        sched_yield();
    }
    if (!broker_test_destroying(&broker)) {
        goto done;
    }
    if (llam_broker_transport_create_ring(&broker, &descriptor, &session_id) != 0 ||
        llam_handle_is_invalid(descriptor) ||
        session_id == 0U) {
        goto done;
    }
    llam_broker_close_handle(descriptor);
    descriptor = LLAM_INVALID_HANDLE;
    llam_broker_end_op(&broker);
    active_op = false;
    (void)pthread_join(destroy_thread, NULL);
    destroy_started = false;
    broker_initialized = false;
    rc = 0;

done:
    if (!llam_handle_is_invalid(descriptor)) {
        llam_broker_close_handle(descriptor);
    }
    if (active_op) {
        llam_broker_end_op(&broker);
    }
    if (destroy_started) {
        (void)pthread_join(destroy_thread, NULL);
        broker_initialized = false;
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_destroy_is_single_owner_under_race(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    broker_destroy_thread_state_t first_state;
    broker_destroy_thread_state_t second_state;
    pthread_t first_thread;
    pthread_t second_thread;
    bool broker_initialized = false;
    bool active_op = false;
    bool first_started = false;
    bool second_started = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;

    /*
     * Hold one operation open so both destroy callers enter the teardown wait.
     * Once released, exactly one caller may own the broker-owned runtime and
     * heap state; the other must wait for teardown to complete and return.
     */
    if (llam_broker_begin_op(&broker) != 0) {
        goto done;
    }
    active_op = true;
    first_state.broker = &broker;
    second_state.broker = &broker;
    if (pthread_create(&first_thread, NULL, broker_destroy_thread, &first_state) != 0) {
        goto done;
    }
    first_started = true;
    if (pthread_create(&second_thread, NULL, broker_destroy_thread, &second_state) != 0) {
        goto done;
    }
    second_started = true;
    for (unsigned i = 0U; i < 100000U; ++i) {
        if (broker_test_destroying(&broker) && broker_test_active_ops(&broker) >= 1U) {
            break;
        }
        sched_yield();
    }
    if (!broker_test_destroying(&broker)) {
        goto done;
    }
    llam_broker_end_op(&broker);
    active_op = false;
    (void)pthread_join(first_thread, NULL);
    first_started = false;
    (void)pthread_join(second_thread, NULL);
    second_started = false;
    broker_initialized = false;
    rc = 0;

done:
    if (active_op) {
        llam_broker_end_op(&broker);
    }
    if (first_started) {
        (void)pthread_join(first_thread, NULL);
    }
    if (second_started) {
        (void)pthread_join(second_thread, NULL);
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_destroy_waits_for_active_ring_io(void) {
    static const char inbound[] = "destroy waits";
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_capability_token_t read_token;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    broker_ring_serve_thread_state_t serve_state;
    broker_destroy_thread_state_t destroy_state;
    pthread_t serve_thread;
    pthread_t destroy_thread;
    int pipe_fds[2] = {-1, -1};
    bool broker_initialized = false;
    bool serve_started = false;
    bool destroy_started = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;
    if (llam_broker_ring_init(&ring) != 0 || pipe(pipe_fds) != 0) {
        goto done;
    }
    if (llam_broker_register_fd(&broker, pipe_fds[0], LLAM_CAP_RIGHT_READ, false, &read_token) != 0) {
        goto done;
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 70U;
    submission.op = LLAM_BROKER_RING_OP_DESCRIPTOR_READ;
    submission.arg1 = sizeof(inbound);
    submission.arg2 = 64U;
    submission.token = read_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0) {
        goto done;
    }
    memset(&serve_state, 0, sizeof(serve_state));
    serve_state.broker = &broker;
    serve_state.ring = &ring;
    serve_state.rc = -1;
    if (pthread_create(&serve_thread, NULL, broker_ring_serve_thread, &serve_state) != 0) {
        goto done;
    }
    serve_started = true;

    /*
     * Wait until the ring serve operation and its nested descriptor read are
     * both pinned. Without the broker active-op guard, destroy could clear the
     * broker lock/table while the serve thread later publishes completion.
     */
    for (unsigned i = 0U; i < 100000U; ++i) {
        if (broker_test_active_ops(&broker) >= 2U) {
            break;
        }
        sched_yield();
    }
    if (broker_test_active_ops(&broker) < 2U) {
        goto done;
    }

    destroy_state.broker = &broker;
    if (pthread_create(&destroy_thread, NULL, broker_destroy_thread, &destroy_state) != 0) {
        goto done;
    }
    destroy_started = true;
    sched_yield();
    if (write(pipe_fds[1], inbound, sizeof(inbound)) != (ssize_t)sizeof(inbound)) {
        goto done;
    }
    (void)pthread_join(serve_thread, NULL);
    serve_started = false;
    (void)pthread_join(destroy_thread, NULL);
    destroy_started = false;
    broker_initialized = false;

    if (serve_state.rc != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status != 0 ||
        completion.result0 != sizeof(inbound) ||
        memcmp(ring.data + 64U, inbound, sizeof(inbound)) != 0) {
        goto done;
    }
    rc = 0;

done:
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
    }
    if (serve_started) {
        (void)pthread_join(serve_thread, NULL);
    }
    if (destroy_started) {
        (void)pthread_join(destroy_thread, NULL);
        broker_initialized = false;
    }
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_ring_publish_cursor_mismatch_fails_closed(void) {
    static const char inbound[] = "publish mismatch";
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_capability_token_t read_token;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    broker_ring_serve_thread_state_t serve_state;
    pthread_t serve_thread;
    int pipe_fds[2] = {-1, -1};
    bool broker_initialized = false;
    bool serve_started = false;
    size_t i;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;
    if (llam_broker_ring_init(&ring) != 0 || pipe(pipe_fds) != 0) {
        goto done;
    }
    if (llam_broker_register_fd(&broker, pipe_fds[0], LLAM_CAP_RIGHT_READ, false, &read_token) != 0) {
        goto done;
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 72U;
    submission.op = LLAM_BROKER_RING_OP_DESCRIPTOR_READ;
    submission.arg1 = sizeof(inbound);
    submission.arg2 = 64U;
    submission.token = read_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0) {
        goto done;
    }
    memset(&serve_state, 0, sizeof(serve_state));
    serve_state.broker = &broker;
    serve_state.ring = &ring;
    serve_state.rc = -1;
    if (pthread_create(&serve_thread, NULL, broker_ring_serve_thread, &serve_state) != 0) {
        goto done;
    }
    serve_started = true;

    /*
     * Wait until the broker has copied the submission and is blocked inside
     * the nested descriptor read. The client-visible ring cursors are shared
     * memory, so a corrupt client can still modify them before the broker
     * publishes completions. That must fail closed at publish time.
     */
    for (unsigned i = 0U; i < 100000U; ++i) {
        if (broker_test_active_ops(&broker) >= 2U) {
            break;
        }
        sched_yield();
    }
    if (broker_test_active_ops(&broker) < 2U) {
        goto done;
    }
    atomic_store_explicit(&ring.submit_head.value, UINT64_C(123), memory_order_release);
    if (write(pipe_fds[1], inbound, sizeof(inbound)) != (ssize_t)sizeof(inbound)) {
        goto done;
    }
    (void)pthread_join(serve_thread, NULL);
    serve_started = false;
    if (serve_state.rc == 0 || serve_state.error_code != EINVAL) {
        fprintf(stderr,
                "[test_security_capability] broker published after cursor corruption rc=%d errno=%d\n",
                serve_state.rc,
                serve_state.error_code);
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_complete_pop(&ring, &completion),
                     EAGAIN,
                     "cursor-corrupted serve published a completion") != 0) {
        goto done;
    }
    for (i = 0U; i < sizeof(inbound); ++i) {
        if (ring.data[64U + i] != 0U) {
            fprintf(stderr, "[test_security_capability] failed serve left output byte %zu visible\n", i);
            goto done;
        }
    }

    /*
     * The corrupted session must be poisoned rather than left active with stale
     * private cursors; otherwise resetting the ring would keep failing or could
     * replay the already executed submission.
     */
    if (llam_broker_ring_init(&ring) != 0) {
        goto done;
    }
    memset(&submission, 0, sizeof(submission));
    submission.request_id = 73U;
    submission.op = LLAM_BROKER_RING_OP_NOP;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status != 0) {
        goto done;
    }
    rc = 0;

done:
    if (pipe_fds[1] >= 0) {
        if (serve_started) {
            ssize_t ignored = write(pipe_fds[1], inbound, sizeof(inbound));

            (void)ignored;
        }
        close(pipe_fds[1]);
    }
    if (serve_started) {
        (void)pthread_join(serve_thread, NULL);
    }
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_ring_session_forget_rejects_busy_serve(void) {
    static const char inbound[] = "session busy";
    const uint64_t subject_id = UINT64_C(0x7b7b7b7b);
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_mapping_t broker_mapping;
    llam_broker_ring_mapping_t client_mapping;
    llam_capability_token_t read_token;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    broker_ring_session_serve_thread_state_t serve_state;
    pthread_t serve_thread;
    int pipe_fds[2] = {-1, -1};
    int client_fd = -1;
    uint64_t session_id = 0U;
    bool broker_initialized = false;
    bool subject_op = false;
    bool serve_started = false;
    int rc = -1;

    memset(&broker_mapping, 0, sizeof(broker_mapping));
    memset(&client_mapping, 0, sizeof(client_mapping));
    broker_mapping.fd = -1;
    client_mapping.fd = -1;
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;
    if (llam_broker_ring_create_private_fd(&broker_mapping) != 0 ||
        pipe(pipe_fds) != 0) {
        goto done;
    }
    client_fd = dup(broker_mapping.fd);
    if (client_fd < 0 ||
        llam_broker_ring_map_fd(client_fd, true, &client_mapping) != 0) {
        goto done;
    }
    client_fd = -1;
    if (llam_broker_ring_register_mapping(&broker, &broker_mapping, subject_id, &session_id) != 0 ||
        session_id == 0U) {
        goto done;
    }

    if (llam_broker_begin_op_subject(&broker, subject_id) != 0) {
        goto done;
    }
    subject_op = true;
    if (llam_broker_register_fd(&broker, pipe_fds[0], LLAM_CAP_RIGHT_READ, false, &read_token) != 0) {
        goto done;
    }
    llam_broker_end_op(&broker);
    subject_op = false;

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 71U;
    submission.op = LLAM_BROKER_RING_OP_DESCRIPTOR_READ;
    submission.arg1 = sizeof(inbound);
    submission.arg2 = 32U;
    submission.token = read_token;
    if (llam_broker_ring_submit_push(client_mapping.ring, &submission) != 0) {
        goto done;
    }
    memset(&serve_state, 0, sizeof(serve_state));
    serve_state.broker = &broker;
    serve_state.session_id = session_id;
    serve_state.subject_id = subject_id;
    serve_state.rc = -1;
    if (pthread_create(&serve_thread, NULL, broker_ring_session_serve_thread, &serve_state) != 0) {
        goto done;
    }
    serve_started = true;

    /*
     * A session-id serve must claim busy before dropping the table lock. If a
     * response-failure cleanup can forget the session while the ring operation
     * is blocked, the broker-owned mapping can be unmapped under the worker.
     */
    for (unsigned i = 0U; i < 100000U; ++i) {
        if (broker_test_ring_session_busy(&broker, session_id)) {
            break;
        }
        sched_yield();
    }
    if (!broker_test_ring_session_busy(&broker, session_id)) {
        goto done;
    }
    if (expect_errno(llam_broker_ring_forget_session(&broker, session_id, subject_id),
                     EBUSY,
                     "forget busy broker ring session") != 0) {
        goto done;
    }
    if (write(pipe_fds[1], inbound, sizeof(inbound)) != (ssize_t)sizeof(inbound)) {
        goto done;
    }
    (void)pthread_join(serve_thread, NULL);
    serve_started = false;
    if (serve_state.rc != 0 ||
        llam_broker_ring_complete_pop(client_mapping.ring, &completion) != 0 ||
        completion.request_id != submission.request_id ||
        completion.status != 0 ||
        completion.result0 != sizeof(inbound) ||
        memcmp(client_mapping.ring->data + 32U, inbound, sizeof(inbound)) != 0) {
        goto done;
    }
    rc = 0;

done:
    if (subject_op) {
        llam_broker_end_op(&broker);
    }
    if (serve_started) {
        if (pipe_fds[1] >= 0) {
            ssize_t ignored = write(pipe_fds[1], inbound, sizeof(inbound));

            (void)ignored;
        }
        (void)pthread_join(serve_thread, NULL);
    }
    if (client_fd >= 0) {
        close(client_fd);
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
    }
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
    }
    llam_broker_ring_unmap(&client_mapping);
    llam_broker_ring_unmap(&broker_mapping);
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_ring_named_session_cleanup_unlinks_mapping(void) {
    const uint64_t subject_id = UINT64_C(0x51515151);
    char forget_name[128];
    char destroy_name[128];
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_mapping_t mapping;
    llam_broker_ring_mapping_t leaked;
    uint64_t session_id = 0U;
    bool broker_initialized = false;
    int rc = -1;

    memset(&mapping, 0, sizeof(mapping));
    memset(&leaked, 0, sizeof(leaked));
    mapping.fd = -1;
    leaked.fd = -1;

    snprintf(forget_name,
             sizeof(forget_name),
             "/llam-fc-%ld",
             (long)getpid());
    snprintf(destroy_name,
             sizeof(destroy_name),
             "/llam-dc-%ld",
             (long)getpid());
    (void)shm_unlink(forget_name);
    (void)shm_unlink(destroy_name);

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;

    if (llam_broker_ring_create_shm(forget_name, &mapping) != 0 ||
        llam_broker_ring_register_mapping(&broker, &mapping, subject_id, &session_id) != 0 ||
        llam_broker_ring_forget_session(&broker, session_id, subject_id) != 0) {
        fprintf(stderr, "[test_security_capability] forget cleanup setup failed errno=%d\n", errno);
        goto done;
    }
    errno = 0;
    if (llam_broker_ring_open_shm(forget_name, &leaked) == 0) {
        fprintf(stderr, "[test_security_capability] forget left named broker ring mapping reachable\n");
        llam_broker_ring_unmap(&leaked);
        (void)shm_unlink(forget_name);
        goto done;
    }
    if (errno != ENOENT) {
        fprintf(stderr,
                "[test_security_capability] forget cleanup reopen errno=%d expected=%d\n",
                errno,
                ENOENT);
        goto done;
    }

    if (llam_broker_ring_create_shm(destroy_name, &mapping) != 0 ||
        llam_broker_ring_register_mapping(&broker, &mapping, subject_id, &session_id) != 0) {
        fprintf(stderr, "[test_security_capability] destroy cleanup setup failed errno=%d\n", errno);
        goto done;
    }
    llam_broker_destroy(&broker);
    broker_initialized = false;
    errno = 0;
    if (llam_broker_ring_open_shm(destroy_name, &leaked) == 0) {
        fprintf(stderr, "[test_security_capability] destroy left named broker ring mapping reachable\n");
        llam_broker_ring_unmap(&leaked);
        (void)shm_unlink(destroy_name);
        goto done;
    }
    if (errno != ENOENT) {
        fprintf(stderr,
                "[test_security_capability] destroy cleanup reopen errno=%d expected=%d\n",
                errno,
                ENOENT);
        goto done;
    }
    rc = 0;

done:
    llam_broker_ring_unmap(&leaked);
    llam_broker_ring_unmap(&mapping);
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    if (rc != 0) {
        (void)shm_unlink(forget_name);
        (void)shm_unlink(destroy_name);
    }
    return rc;
}

static int test_broker_destroy_reclaims_inactive_owned_ring_mapping(void) {
    const uint64_t subject_id = UINT64_C(0x61616161);
    char name[128];
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_mapping_t mapping;
    llam_broker_ring_mapping_t leaked;
    uint64_t session_id = 0U;
    bool broker_initialized = false;
    int rc = -1;

    memset(&mapping, 0, sizeof(mapping));
    memset(&leaked, 0, sizeof(leaked));
    mapping.fd = -1;
    leaked.fd = -1;

    snprintf(name, sizeof(name), "/llam-ic-%ld", (long)getpid());
    (void)shm_unlink(name);

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;

    if (llam_broker_ring_create_shm(name, &mapping) != 0 ||
        llam_broker_ring_register_mapping(&broker, &mapping, subject_id, &session_id) != 0 ||
        session_id == 0U ||
        session_id > LLAM_BROKER_RING_SESSIONS) {
        fprintf(stderr, "[test_security_capability] inactive cleanup setup failed errno=%d\n", errno);
        goto done;
    }

    /*
     * Simulate an interrupted internal lifecycle where the broker still owns a
     * mapping but the session is no longer marked active. Destroy must reclaim
     * authority from owns_mapping, not from the active bit.
     */
    if (llam_broker_lock(&broker) != 0) {
        goto done;
    }
    broker.ring_sessions[(size_t)session_id - 1U].active = false;
    llam_broker_unlock(&broker);

    llam_broker_destroy(&broker);
    broker_initialized = false;
    errno = 0;
    if (llam_broker_ring_open_shm(name, &leaked) == 0) {
        fprintf(stderr, "[test_security_capability] inactive owned ring mapping remained reachable\n");
        llam_broker_ring_unmap(&leaked);
        (void)shm_unlink(name);
        goto done;
    }
    if (errno != ENOENT) {
        fprintf(stderr,
                "[test_security_capability] inactive cleanup reopen errno=%d expected=%d\n",
                errno,
                ENOENT);
        goto done;
    }
    rc = 0;

done:
    llam_broker_ring_unmap(&leaked);
    llam_broker_ring_unmap(&mapping);
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    if (rc != 0) {
        (void)shm_unlink(name);
    }
    return rc;
}

static int test_broker_ring_reuse_reclaims_inactive_owned_mapping(void) {
    const uint64_t first_subject = UINT64_C(0x61616161);
    const uint64_t second_subject = UINT64_C(0x62626262);
    char first_name[128];
    char second_name[128];
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_mapping_t first_mapping;
    llam_broker_ring_mapping_t second_mapping;
    llam_broker_ring_mapping_t leaked;
    uint64_t first_session_id = 0U;
    uint64_t second_session_id = 0U;
    bool broker_initialized = false;
    int rc = -1;

    memset(&first_mapping, 0, sizeof(first_mapping));
    memset(&second_mapping, 0, sizeof(second_mapping));
    memset(&leaked, 0, sizeof(leaked));
    first_mapping.fd = -1;
    second_mapping.fd = -1;
    leaked.fd = -1;

    snprintf(first_name, sizeof(first_name), "/llam-ir1-%ld", (long)getpid());
    snprintf(second_name, sizeof(second_name), "/llam-ir2-%ld", (long)getpid());
    (void)shm_unlink(first_name);
    (void)shm_unlink(second_name);

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;

    if (llam_broker_ring_create_shm(first_name, &first_mapping) != 0 ||
        llam_broker_ring_register_mapping(&broker, &first_mapping, first_subject, &first_session_id) != 0 ||
        first_session_id == 0U ||
        first_session_id > LLAM_BROKER_RING_SESSIONS) {
        fprintf(stderr, "[test_security_capability] ring reuse first setup failed errno=%d\n", errno);
        goto done;
    }

    /*
     * Reproduce an interrupted lifecycle where the slot is available for reuse
     * but still owns a named mapping. Reuse must reclaim that mapping before
     * the new session overwrites the private fd/name authority.
     */
    if (llam_broker_lock(&broker) != 0) {
        goto done;
    }
    broker.ring_sessions[(size_t)first_session_id - 1U].active = false;
    llam_broker_unlock(&broker);

    if (llam_broker_ring_create_shm(second_name, &second_mapping) != 0 ||
        llam_broker_ring_register_mapping(&broker, &second_mapping, second_subject, &second_session_id) != 0 ||
        second_session_id != first_session_id) {
        fprintf(stderr, "[test_security_capability] ring reuse second setup failed errno=%d\n", errno);
        goto done;
    }

    errno = 0;
    if (llam_broker_ring_open_shm(first_name, &leaked) == 0) {
        fprintf(stderr, "[test_security_capability] inactive owned ring mapping was overwritten without unlink\n");
        llam_broker_ring_unmap(&leaked);
        (void)shm_unlink(first_name);
        goto done;
    }
    if (errno != ENOENT) {
        fprintf(stderr,
                "[test_security_capability] ring reuse cleanup reopen errno=%d expected=%d\n",
                errno,
                ENOENT);
        goto done;
    }
    rc = 0;

done:
    llam_broker_ring_unmap(&leaked);
    llam_broker_ring_unmap(&first_mapping);
    llam_broker_ring_unmap(&second_mapping);
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    if (rc != 0) {
        (void)shm_unlink(first_name);
        (void)shm_unlink(second_name);
    }
    return rc;
}

static int test_broker_buffer_reuse_reclaims_inactive_storage(void) {
    static const unsigned char replacement[] = {0x51U, 0x52U, 0x53U, 0x54U};
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_token_t token;
    unsigned char *stale = NULL;
    unsigned char out[sizeof(replacement)];
    uint64_t frees;
    bool broker_initialized = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;
    stale = (unsigned char *)calloc(1U, 32U);
    if (stale == NULL) {
        goto done;
    }

    /*
     * Model a partially invalidated buffer slot: active authority was revoked,
     * but broker-owned heap storage is still attached. Register reuses inactive
     * slots, so it must reclaim that storage before publishing replacement
     * authority or the old allocation is overwritten and leaked.
     */
    broker.buffers[0].data = stale;
    broker.buffers[0].length = 32U;
    broker.buffers[0].id = 7U;
    broker.buffers[0].generation = 3U;
    broker.buffers[0].rights = LLAM_CAP_RIGHT_READ;
    broker.buffers[0].active = false;

    llam_broker_test_buffer_free_count_reset();
    if (llam_broker_register_buffer(&broker,
                                    replacement,
                                    sizeof(replacement),
                                    LLAM_CAP_RIGHT_READ,
                                    &token) != 0) {
        goto done;
    }
    frees = llam_broker_test_buffer_free_count();
    if (frees == 0U) {
        fprintf(stderr,
                "[test_security_capability] inactive buffer storage was overwritten without free\n");
        /*
         * The failing implementation no longer owns this pointer after slot
         * overwrite. Free it here so the reproducer itself stays leak-neutral.
         */
        free(stale);
        stale = NULL;
        goto done;
    }
    stale = NULL;
    if (llam_broker_read_buffer(&broker, &token, 0U, out, sizeof(out)) != 0 ||
        memcmp(out, replacement, sizeof(replacement)) != 0) {
        goto done;
    }
    rc = 0;

done:
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    if (stale != NULL && llam_broker_test_buffer_free_count() == 0U) {
        free(stale);
    }
    return rc;
}

static int test_broker_destroy_reclaims_inactive_owned_descriptor(void) {
    llam_runtime_opts_t opts;
    llam_capability_token_t token;
    llam_broker_t broker;
    int pipe_fds[2] = {-1, -1};
    int registered_fd = -1;
    bool broker_initialized = false;
    bool found_slot = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;
    if (pipe(pipe_fds) != 0) {
        goto done;
    }
    registered_fd = pipe_fds[0];
    if (llam_broker_register_fd(&broker, registered_fd, LLAM_CAP_RIGHT_READ, true, &token) != 0) {
        fprintf(stderr, "[test_security_capability] inactive descriptor setup failed errno=%d\n", errno);
        goto done;
    }
    pipe_fds[0] = -1;

    /*
     * Descriptor ownership is close_on_destroy + a live fd. If an interrupted
     * internal lifecycle clears active before destroy, the broker must still
     * reclaim the fd rather than stranding authority in the process table.
     */
    if (llam_broker_lock(&broker) != 0) {
        goto done;
    }
    for (size_t i = 0U; i < LLAM_BROKER_DESCRIPTOR_SLOTS; ++i) {
        if (broker.descriptors[i].id == token.slot &&
            broker.descriptors[i].generation == token.generation) {
            broker.descriptors[i].active = false;
            found_slot = true;
            break;
        }
    }
    llam_broker_unlock(&broker);
    if (!found_slot) {
        fprintf(stderr, "[test_security_capability] inactive descriptor slot was not found\n");
        goto done;
    }

    llam_broker_destroy(&broker);
    broker_initialized = false;
    if (!broker_fd_is_closed(registered_fd)) {
        fprintf(stderr, "[test_security_capability] inactive owned descriptor remained open\n");
        goto done;
    }
    registered_fd = -1;
    rc = 0;

done:
    if (registered_fd >= 0 && !broker_fd_is_closed(registered_fd)) {
        close(registered_fd);
    }
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_descriptor_reuse_reclaims_inactive_owned_slot(void) {
    llam_runtime_opts_t opts;
    llam_capability_token_t first_token;
    llam_capability_token_t second_token;
    llam_broker_t broker;
    int first_pipe[2] = {-1, -1};
    int second_pipe[2] = {-1, -1};
    int first_registered_fd = -1;
    int second_registered_fd = -1;
    bool broker_initialized = false;
    bool found_slot = false;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;
    if (pipe(first_pipe) != 0 || pipe(second_pipe) != 0) {
        goto done;
    }
    first_registered_fd = first_pipe[0];
    second_registered_fd = second_pipe[0];

    if (llam_broker_register_fd(&broker,
                                first_registered_fd,
                                LLAM_CAP_RIGHT_READ,
                                true,
                                &first_token) != 0) {
        fprintf(stderr, "[test_security_capability] first descriptor reuse setup failed errno=%d\n", errno);
        goto done;
    }
    first_pipe[0] = -1;
    if (llam_broker_lock(&broker) != 0) {
        goto done;
    }
    for (size_t i = 0U; i < LLAM_BROKER_DESCRIPTOR_SLOTS; ++i) {
        if (broker.descriptors[i].id == first_token.slot &&
            broker.descriptors[i].generation == first_token.generation) {
            broker.descriptors[i].active = false;
            found_slot = true;
            break;
        }
    }
    llam_broker_unlock(&broker);
    if (!found_slot) {
        fprintf(stderr, "[test_security_capability] inactive descriptor reuse slot was not found\n");
        goto done;
    }

    /*
     * Registration reuses inactive slots. If a partially invalidated slot still
     * owns a descriptor, reuse must close it before overwriting the fd field.
     */
    if (llam_broker_register_fd(&broker,
                                second_registered_fd,
                                LLAM_CAP_RIGHT_READ,
                                true,
                                &second_token) != 0) {
        fprintf(stderr, "[test_security_capability] second descriptor reuse setup failed errno=%d\n", errno);
        goto done;
    }
    second_pipe[0] = -1;
    second_registered_fd = -1;
    if (!broker_fd_is_closed(first_registered_fd)) {
        fprintf(stderr, "[test_security_capability] inactive owned descriptor was overwritten without close\n");
        goto done;
    }
    first_registered_fd = -1;
    rc = 0;

done:
    if (first_registered_fd >= 0 && !broker_fd_is_closed(first_registered_fd)) {
        close(first_registered_fd);
    }
    if (second_registered_fd >= 0 && !broker_fd_is_closed(second_registered_fd)) {
        close(second_registered_fd);
    }
    if (first_pipe[0] >= 0) {
        close(first_pipe[0]);
    }
    if (first_pipe[1] >= 0) {
        close(first_pipe[1]);
    }
    if (second_pipe[0] >= 0) {
        close(second_pipe[0]);
    }
    if (second_pipe[1] >= 0) {
        close(second_pipe[1]);
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}
#endif

#if LLAM_PLATFORM_WINDOWS
typedef struct broker_ring_windows_flood_state {
    llam_broker_ring_t *ring;
    uint64_t iterations;
    atomic_int stop;
    int rc;
} broker_ring_windows_flood_state_t;

static DWORD WINAPI broker_ring_windows_flood_thread(LPVOID arg) {
    broker_ring_windows_flood_state_t *state = (broker_ring_windows_flood_state_t *)arg;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    uint64_t served = 0U;

    state->rc = -1;
    while (served < state->iterations &&
           atomic_load_explicit(&state->stop, memory_order_acquire) == 0) {
        if (llam_broker_ring_submit_pop(state->ring, &submission) == 0) {
            memset(&completion, 0, sizeof(completion));
            completion.request_id = submission.request_id;
            completion.status = 0;
            completion.result0 = submission.request_id ^ UINT64_C(0xa55aa55aa55aa55a);
            for (;;) {
                if (atomic_load_explicit(&state->stop, memory_order_acquire) != 0) {
                    return 3U;
                }
                if (llam_broker_ring_complete_push(state->ring, &completion) == 0) {
                    break;
                }
                if (errno != EAGAIN) {
                    return 2U;
                }
                Sleep(0);
            }
            ++served;
            continue;
        }
        if (errno != EAGAIN) {
            return 1U;
        }
        Sleep(0);
    }
    state->rc = 0;
    return 0U;
}

static int broker_ring_windows_process_child(const char *name, uint64_t iterations) {
    llam_broker_ring_mapping_t mapping;
    broker_ring_windows_flood_state_t state;
    DWORD thread_rc;

    memset(&mapping, 0, sizeof(mapping));
    mapping.mapping_handle = LLAM_INVALID_HANDLE;
    if (llam_broker_ring_open_shm(name, &mapping) != 0) {
        return 2;
    }

    /*
     * Reuse the same serving loop as the thread-based test, but run it from a
     * separate process mapping. This proves the Windows ring data plane works
     * across process address spaces instead of only across duplicate views in one
     * process.
     */
    memset(&state, 0, sizeof(state));
    state.ring = mapping.ring;
    state.iterations = iterations;
    atomic_init(&state.stop, 0);
    state.rc = -1;
    thread_rc = broker_ring_windows_flood_thread(&state);
    llam_broker_ring_unmap(&mapping);
    return thread_rc == 0U && state.rc == 0 ? 0 : 3;
}

static int broker_ring_windows_session_child(const char *name,
                                             const char *ready_event_name,
                                             uint64_t iterations) {
    enum { PHASE_TIMEOUT_MS = 60000U };
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_mapping_t mapping;
    HANDLE ready_event = NULL;
    ULONGLONG deadline_ms;
    bool broker_initialized = false;
    uint64_t served = 0U;
    int rc = 3;

    memset(&mapping, 0, sizeof(mapping));
    mapping.mapping_handle = LLAM_INVALID_HANDLE;
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 2;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 2;
    }
    broker_initialized = true;
    if (llam_broker_ring_open_shm(name, &mapping) != 0) {
        goto done;
    }
    ready_event = OpenEventA(EVENT_MODIFY_STATE, FALSE, ready_event_name);
    if (ready_event == NULL) {
        goto done;
    }

    while (served < iterations) {
        if (llam_broker_ring_serve_one(&broker, mapping.ring) == 0) {
            ++served;
            continue;
        }
        if (errno != EAGAIN) {
            goto done;
        }
        Sleep(0);
    }
    /*
     * Signal the parent only after this child has left the serve loop and is
     * ready to observe the deliberate public-cursor rewind. Without this
     * handshake, hosted Windows runners can charge the parent process-wait
     * timeout to scheduler starvation before the replay check even starts.
     */
    if (!SetEvent(ready_event)) {
        rc = 6;
        goto done;
    }

    /*
     * The parent rewinds the client-visible counters after all completions have
     * been drained. This separate process must still use the broker-private
     * session cursor and reject the stale replay instead of serving slot 0 again.
     */
    deadline_ms = GetTickCount64() + PHASE_TIMEOUT_MS;
    for (;;) {
        uint64_t shared_head = atomic_load_explicit(&mapping.ring->submit_head.value, memory_order_acquire);
        uint64_t shared_tail = atomic_load_explicit(&mapping.ring->submit_tail.value, memory_order_acquire);

        if (shared_head == 0U && shared_tail == 1U) {
            break;
        }
        if (GetTickCount64() >= deadline_ms) {
            rc = 4;
            goto done;
        }
        Sleep(1U);
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_serve_one(&broker, mapping.ring),
                     EINVAL,
                     "windows cross-process broker session replayed stale cursor") != 0) {
        rc = 5;
        goto done;
    }
    rc = 0;

done:
    if (ready_event != NULL) {
        CloseHandle(ready_event);
    }
    llam_broker_ring_unmap(&mapping);
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int broker_ring_windows_teardown_child(const char *name, uint64_t iterations) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_mapping_t mapping;
    bool broker_initialized = false;
    uint64_t served = 0U;
    int rc = 3;

    memset(&mapping, 0, sizeof(mapping));
    mapping.mapping_handle = LLAM_INVALID_HANDLE;
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 2;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 2;
    }
    broker_initialized = true;
    if (llam_broker_ring_open_shm(name, &mapping) != 0) {
        goto done;
    }

    while (served < iterations) {
        if (llam_broker_ring_serve_one(&broker, mapping.ring) == 0) {
            ++served;
            continue;
        }
        if (errno != EAGAIN) {
            goto done;
        }
        Sleep(0);
    }
    rc = 0;

done:
    llam_broker_ring_unmap(&mapping);
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_ring_windows_cross_process_flood(void) {
    char name[128];
    char exe_path[MAX_PATH];
    char command[1024];
    llam_broker_ring_mapping_t mapping;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    llam_broker_ring_stats_t stats;
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    const uint64_t flood_iters = test_broker_ring_flood_iters();
    DWORD wait_rc;
    DWORD exit_code = 1U;
    uint64_t next_submit = 1U;
    uint64_t next_complete = 1U;
    int rc = -1;

    memset(&mapping, 0, sizeof(mapping));
    mapping.mapping_handle = LLAM_INVALID_HANDLE;
    snprintf(name,
             sizeof(name),
             "Local\\llam-broker-ring-xproc-%lu-%lu",
             (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount());
    if (llam_broker_ring_create_shm(name, &mapping) != 0) {
        goto done;
    }
    if (GetModuleFileNameA(NULL, exe_path, sizeof(exe_path)) == 0U) {
        goto done;
    }
    snprintf(command,
             sizeof(command),
             "\"%s\" --broker-ring-windows-child \"%s\" %llu",
             exe_path,
             name,
             (unsigned long long)flood_iters);

    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessA(NULL,
                        command,
                        NULL,
                        NULL,
                        FALSE,
                        0U,
                        NULL,
                        NULL,
                        &startup,
                        &process)) {
        goto done;
    }

    while (next_complete <= flood_iters) {
        bool progressed = false;

        while (next_submit <= flood_iters) {
            memset(&submission, 0, sizeof(submission));
            submission.request_id = next_submit;
            submission.op = LLAM_BROKER_RING_OP_NOP;
            if (llam_broker_ring_submit_push(mapping.ring, &submission) == 0) {
                ++next_submit;
                progressed = true;
                continue;
            }
            if (errno == EAGAIN) {
                break;
            }
            goto done_process;
        }

        while (next_complete <= flood_iters) {
            if (llam_broker_ring_complete_pop(mapping.ring, &completion) == 0) {
                if (completion.request_id != next_complete ||
                    completion.status != 0 ||
                    completion.result0 != (next_complete ^ UINT64_C(0xa55aa55aa55aa55a))) {
                    goto done_process;
                }
                ++next_complete;
                progressed = true;
                continue;
            }
            if (errno == EAGAIN) {
                break;
            }
            goto done_process;
        }

        if (!progressed) {
            Sleep(0);
        }
    }

    /*
     * Hosted Windows Server 2022 runners can briefly starve the child process
     * under stress. Keep this as an inter-process data-plane check rather than
     * a tight scheduling deadline.
     */
    wait_rc = WaitForSingleObject(process.hProcess, 15000U);
    if (wait_rc != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &exit_code) ||
        exit_code != 0U) {
        goto done_process;
    }
    if (llam_broker_ring_collect_stats(mapping.ring, &stats) != 0 ||
        stats.client_submit_pushes != flood_iters ||
        stats.broker_complete_tail_publishes != flood_iters ||
        stats.client_complete_drain_entries != flood_iters ||
        stats.cursor_write_estimate < flood_iters * 2U) {
        goto done_process;
    }
    rc = 0;

done_process:
    if (rc != 0) {
        (void)TerminateProcess(process.hProcess, 1U);
        (void)WaitForSingleObject(process.hProcess, 1000U);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
done:
    llam_broker_ring_unmap(&mapping);
    return rc;
}

static int test_broker_ring_windows_cross_process_session_replay_guard(void) {
    char name[128];
    char ready_event_name[128];
    char exe_path[MAX_PATH];
    char command[1024];
    llam_broker_ring_mapping_t mapping;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    llam_broker_ring_stats_t stats;
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    HANDLE ready_event = NULL;
    const uint64_t replay_iters = test_broker_ring_replay_iters();
    DWORD wait_rc;
    DWORD exit_code = 1U;
    uint64_t next_submit = 1U;
    uint64_t next_complete = 1U;
    int rc = -1;

    memset(&mapping, 0, sizeof(mapping));
    mapping.mapping_handle = LLAM_INVALID_HANDLE;
    snprintf(name,
             sizeof(name),
             "Local\\llam-broker-ring-session-replay-%lu-%lu",
             (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount());
    snprintf(ready_event_name,
             sizeof(ready_event_name),
             "Local\\llam-broker-ring-session-ready-%lu-%lu",
             (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount());
    ready_event = CreateEventA(NULL, TRUE, FALSE, ready_event_name);
    if (ready_event == NULL) {
        goto done;
    }
    if (llam_broker_ring_create_shm(name, &mapping) != 0) {
        goto done;
    }
    if (GetModuleFileNameA(NULL, exe_path, sizeof(exe_path)) == 0U) {
        goto done;
    }
    snprintf(command,
             sizeof(command),
             "\"%s\" --broker-ring-windows-session-child \"%s\" \"%s\" %llu",
             exe_path,
             name,
             ready_event_name,
             (unsigned long long)replay_iters);

    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessA(NULL,
                        command,
                        NULL,
                        NULL,
                        FALSE,
                        0U,
                        NULL,
                        NULL,
                        &startup,
                        &process)) {
        goto done;
    }

    while (next_complete <= replay_iters) {
        bool progressed = false;

        while (next_submit <= replay_iters) {
            memset(&submission, 0, sizeof(submission));
            submission.request_id = next_submit;
            submission.op = LLAM_BROKER_RING_OP_NOP;
            if (llam_broker_ring_submit_push(mapping.ring, &submission) == 0) {
                ++next_submit;
                progressed = true;
                continue;
            }
            if (errno == EAGAIN) {
                break;
            }
            goto done_process;
        }

        while (next_complete <= replay_iters) {
            if (llam_broker_ring_complete_pop(mapping.ring, &completion) == 0) {
                if (completion.request_id != next_complete || completion.status != 0) {
                    fprintf(stderr,
                            "[test_security_capability] windows session replay completion mismatch "
                            "request=%llu expected=%llu status=%d\n",
                            (unsigned long long)completion.request_id,
                            (unsigned long long)next_complete,
                            completion.status);
                    goto done_process;
                }
                ++next_complete;
                progressed = true;
                continue;
            }
            if (errno == EAGAIN) {
                break;
            }
            goto done_process;
        }

        if (!progressed) {
            Sleep(0);
        }
    }

    wait_rc = WaitForSingleObject(ready_event, 60000U);
    if (wait_rc != WAIT_OBJECT_0) {
        fprintf(stderr,
                "[test_security_capability] windows session replay child not ready "
                "wait=%lu\n",
                (unsigned long)wait_rc);
        goto done_process;
    }
    atomic_store_explicit(&mapping.ring->submit_head.value, 0U, memory_order_relaxed);
    atomic_store_explicit(&mapping.ring->submit_tail.value, 1U, memory_order_release);
    /*
     * The child may be descheduled after the parent rewinds the public cursors,
     * especially on hosted Windows Server 2022. The security check remains
     * strict: the child must still reject the stale replay and exit cleanly, but
     * it gets a CI-tolerant window to run.
     */
    wait_rc = WaitForSingleObject(process.hProcess, 60000U);
    if (wait_rc != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &exit_code) ||
        exit_code != 0U) {
        fprintf(stderr,
                "[test_security_capability] windows session replay child failed "
                "wait=%lu exit=%lu\n",
                (unsigned long)wait_rc,
                (unsigned long)exit_code);
        goto done_process;
    }
    if (llam_broker_ring_collect_stats(mapping.ring, &stats) != 0 ||
        stats.client_submit_pushes != replay_iters ||
        stats.broker_serve_success != replay_iters ||
        stats.broker_complete_tail_publishes != replay_iters ||
        stats.client_complete_drain_entries != replay_iters) {
        fprintf(stderr,
                "[test_security_capability] windows session replay stats mismatch "
                "submit=%llu serve=%llu complete_tail=%llu drained=%llu expected=%llu\n",
                (unsigned long long)stats.client_submit_pushes,
                (unsigned long long)stats.broker_serve_success,
                (unsigned long long)stats.broker_complete_tail_publishes,
                (unsigned long long)stats.client_complete_drain_entries,
                (unsigned long long)replay_iters);
        goto done_process;
    }
    rc = 0;

done_process:
    if (rc != 0) {
        (void)TerminateProcess(process.hProcess, 1U);
        (void)WaitForSingleObject(process.hProcess, 1000U);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
done:
    if (ready_event != NULL) {
        CloseHandle(ready_event);
    }
    llam_broker_ring_unmap(&mapping);
    return rc;
}

static int test_broker_ring_windows_cross_process_teardown_guard(void) {
    enum {
        TEARDOWN_SERVED = 64U,
        TEARDOWN_SUBMITTED = 128U,
        MAX_DRAIN_SPINS = 1000000U
    };
    char name[128];
    char exe_path[MAX_PATH];
    char command[1024];
    llam_broker_ring_mapping_t mapping;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    llam_broker_ring_stats_t stats;
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    DWORD wait_rc;
    DWORD exit_code = 1U;
    uint64_t next_submit;
    uint64_t next_complete = 1U;
    unsigned spins;
    int rc = -1;

    memset(&mapping, 0, sizeof(mapping));
    mapping.mapping_handle = LLAM_INVALID_HANDLE;
    snprintf(name,
             sizeof(name),
             "Local\\llam-broker-ring-teardown-%lu-%lu",
             (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount());
    if (llam_broker_ring_create_shm(name, &mapping) != 0) {
        goto done;
    }
    if (GetModuleFileNameA(NULL, exe_path, sizeof(exe_path)) == 0U) {
        goto done;
    }
    snprintf(command,
             sizeof(command),
             "\"%s\" --broker-ring-windows-teardown-child \"%s\" %u",
             exe_path,
             name,
             (unsigned)TEARDOWN_SERVED);

    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessA(NULL,
                        command,
                        NULL,
                        NULL,
                        FALSE,
                        0U,
                        NULL,
                        NULL,
                        &startup,
                        &process)) {
        goto done;
    }

    /*
     * The child broker serves only a prefix and exits normally. The client side
     * must drain that prefix and tear down the mapping without assuming the
     * remaining submitted slots will ever receive completions.
     */
    for (next_submit = 1U; next_submit <= TEARDOWN_SUBMITTED; ++next_submit) {
        memset(&submission, 0, sizeof(submission));
        submission.request_id = next_submit;
        submission.op = LLAM_BROKER_RING_OP_NOP;
        while (llam_broker_ring_submit_push(mapping.ring, &submission) != 0) {
            if (errno != EAGAIN) {
                goto done_process;
            }
            Sleep(0);
        }
    }

    for (spins = 0U; next_complete <= TEARDOWN_SERVED && spins < MAX_DRAIN_SPINS; ++spins) {
        if (llam_broker_ring_complete_pop(mapping.ring, &completion) == 0) {
            if (completion.request_id != next_complete || completion.status != 0) {
                goto done_process;
            }
            ++next_complete;
            continue;
        }
        if (errno != EAGAIN) {
            goto done_process;
        }
        Sleep(0);
    }
    if (next_complete <= TEARDOWN_SERVED) {
        goto done_process;
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_complete_pop(mapping.ring, &completion),
                     EAGAIN,
                     "windows teardown guard found unexpected completion after broker exit") != 0) {
        goto done_process;
    }

    wait_rc = WaitForSingleObject(process.hProcess, 5000U);
    if (wait_rc != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &exit_code) ||
        exit_code != 0U) {
        goto done_process;
    }
    if (llam_broker_ring_collect_stats(mapping.ring, &stats) != 0 ||
        stats.client_submit_pushes != TEARDOWN_SUBMITTED ||
        stats.broker_serve_success != TEARDOWN_SERVED ||
        stats.client_complete_drain_entries != TEARDOWN_SERVED) {
        goto done_process;
    }
    rc = 0;

done_process:
    if (rc != 0) {
        (void)TerminateProcess(process.hProcess, 1U);
        (void)WaitForSingleObject(process.hProcess, 1000U);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
done:
    llam_broker_ring_unmap(&mapping);
    return rc;
}

static int test_broker_ring_windows_mapping_flood(void) {
    llam_broker_ring_mapping_t broker_mapping;
    llam_broker_ring_mapping_t client_mapping;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    llam_broker_ring_stats_t stats;
    broker_ring_windows_flood_state_t state;
    HANDLE thread = NULL;
    const uint64_t flood_iters = test_broker_ring_flood_iters();
    DWORD wait_rc;
    DWORD thread_rc = 1U;
    uint64_t next_submit = 1U;
    uint64_t next_complete = 1U;
    int rc = -1;

    memset(&broker_mapping, 0, sizeof(broker_mapping));
    memset(&client_mapping, 0, sizeof(client_mapping));
    broker_mapping.mapping_handle = LLAM_INVALID_HANDLE;
    client_mapping.mapping_handle = LLAM_INVALID_HANDLE;
    if (llam_broker_ring_create_private_handle(&broker_mapping) != 0 ||
        llam_broker_ring_map_handle(broker_mapping.mapping_handle, false, &client_mapping) != 0) {
        goto done;
    }

    /*
     * Windows has a distinct mapping/HANDLE backend. Stress it through two
     * mapped views so cursor cache-line layout, ordering, and diagnostic stats
     * are covered without depending on named-pipe control-plane policy.
     */
    memset(&state, 0, sizeof(state));
    state.ring = broker_mapping.ring;
    state.iterations = flood_iters;
    atomic_init(&state.stop, 0);
    state.rc = -1;
    thread = CreateThread(NULL, 0U, broker_ring_windows_flood_thread, &state, 0U, NULL);
    if (thread == NULL) {
        goto done;
    }

    while (next_complete <= flood_iters) {
        bool progressed = false;

        while (next_submit <= flood_iters) {
            memset(&submission, 0, sizeof(submission));
            submission.request_id = next_submit;
            submission.op = LLAM_BROKER_RING_OP_NOP;
            if (llam_broker_ring_submit_push(client_mapping.ring, &submission) == 0) {
                ++next_submit;
                progressed = true;
                continue;
            }
            if (errno == EAGAIN) {
                break;
            }
            goto done;
        }

        while (next_complete <= flood_iters) {
            if (llam_broker_ring_complete_pop(client_mapping.ring, &completion) == 0) {
                if (completion.request_id != next_complete ||
                    completion.status != 0 ||
                    completion.result0 != (next_complete ^ UINT64_C(0xa55aa55aa55aa55a))) {
                    goto done;
                }
                ++next_complete;
                progressed = true;
                continue;
            }
            if (errno == EAGAIN) {
                break;
            }
            goto done;
        }

        if (!progressed) {
            Sleep(0);
        }
    }
    wait_rc = WaitForSingleObject(thread, 5000U);
    if (wait_rc != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &thread_rc) ||
        thread_rc != 0U ||
        state.rc != 0) {
        goto done;
    }
    CloseHandle(thread);
    thread = NULL;

    if (llam_broker_ring_collect_stats(client_mapping.ring, &stats) != 0 ||
        stats.client_submit_pushes != flood_iters ||
        stats.broker_complete_tail_publishes != flood_iters ||
        stats.client_complete_drain_entries != flood_iters ||
        stats.cursor_write_estimate < flood_iters * 2U) {
        goto done;
    }
    rc = 0;

done:
    if (thread != NULL) {
        atomic_store_explicit(&state.stop, 1, memory_order_release);
        (void)WaitForSingleObject(thread, 1000U);
        CloseHandle(thread);
    }
    llam_broker_ring_unmap(&client_mapping);
    llam_broker_ring_unmap(&broker_mapping);
    return rc;
}

static int test_broker_ring_handle_data_plane(void) {
    static const char inbound[] = "broker handle read";
    static const char outbound[] = "broker handle write";
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_ring_t ring;
    llam_capability_token_t read_token;
    llam_capability_token_t write_token;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    HANDLE read_pipe_read = INVALID_HANDLE_VALUE;
    HANDLE read_pipe_write = INVALID_HANDLE_VALUE;
    HANDLE write_pipe_read = INVALID_HANDLE_VALUE;
    HANDLE write_pipe_write = INVALID_HANDLE_VALUE;
    char out[sizeof(outbound)];
    DWORD transferred = 0U;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (llam_broker_ring_init(&ring) != 0 ||
        !CreatePipe(&read_pipe_read, &read_pipe_write, NULL, 0U) ||
        !CreatePipe(&write_pipe_read, &write_pipe_write, NULL, 0U)) {
        goto done;
    }
    if (!WriteFile(read_pipe_write, inbound, (DWORD)sizeof(inbound), &transferred, NULL) ||
        transferred != (DWORD)sizeof(inbound)) {
        goto done;
    }
    /*
     * Force the following oversized pipe read to complete as a successful short
     * read instead of blocking for the whole requested ring output window.
     */
    CloseHandle(read_pipe_write);
    read_pipe_write = INVALID_HANDLE_VALUE;
    if (llam_broker_register_handle(&broker,
                                    (llam_handle_t)read_pipe_read,
                                    LLAM_CAP_RIGHT_READ,
                                    false,
                                    &read_token) != 0 ||
        llam_broker_register_handle(&broker,
                                    (llam_handle_t)write_pipe_write,
                                    LLAM_CAP_RIGHT_WRITE,
                                    false,
                                    &write_token) != 0) {
        goto done;
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 30U;
    submission.op = LLAM_BROKER_RING_OP_DESCRIPTOR_READ;
    memset(ring.data + 64U, 0xa5, sizeof(inbound) + 8U);
    submission.arg1 = sizeof(inbound) + 8U;
    submission.arg2 = 64U;
    submission.token = read_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != sizeof(inbound) ||
        memcmp(ring.data + 64U, inbound, sizeof(inbound)) != 0 ||
        !memory_is_byte(ring.data + 64U + sizeof(inbound), 8U, 0U)) {
        goto done;
    }

    memcpy(ring.data + 128U, outbound, sizeof(outbound));
    submission.request_id = 31U;
    submission.op = LLAM_BROKER_RING_OP_DESCRIPTOR_WRITE;
    submission.arg1 = sizeof(outbound);
    submission.arg2 = 128U;
    submission.token = write_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status != 0 ||
        completion.result0 != sizeof(outbound)) {
        goto done;
    }
    transferred = 0U;
    if (!ReadFile(write_pipe_read, out, (DWORD)sizeof(out), &transferred, NULL) ||
        transferred != (DWORD)sizeof(out) ||
        memcmp(out, outbound, sizeof(out)) != 0) {
        goto done;
    }

    submission.request_id = 32U;
    submission.op = LLAM_BROKER_RING_OP_DESCRIPTOR_WRITE;
    submission.arg1 = 1U;
    submission.arg2 = 0U;
    submission.token = read_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EACCES) {
        goto done;
    }

    (void)llam_broker_revoke_all(&broker);
    submission.request_id = 33U;
    submission.op = LLAM_BROKER_RING_OP_DESCRIPTOR_READ;
    submission.arg1 = 1U;
    submission.arg2 = 0U;
    submission.token = read_token;
    if (llam_broker_ring_submit_push(&ring, &submission) != 0 ||
        llam_broker_ring_serve_one(&broker, &ring) != 0 ||
        llam_broker_ring_complete_pop(&ring, &completion) != 0 ||
        completion.status == 0 ||
        completion.error_code != EACCES) {
        goto done;
    }

    rc = 0;

done:
    if (read_pipe_read != INVALID_HANDLE_VALUE) {
        CloseHandle(read_pipe_read);
    }
    if (read_pipe_write != INVALID_HANDLE_VALUE) {
        CloseHandle(read_pipe_write);
    }
    if (write_pipe_read != INVALID_HANDLE_VALUE) {
        CloseHandle(write_pipe_read);
    }
    if (write_pipe_write != INVALID_HANDLE_VALUE) {
        CloseHandle(write_pipe_write);
    }
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_register_handle_clears_inherit_flag(void) {
    SECURITY_ATTRIBUTES attrs;
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_token_t token;
    HANDLE read_pipe_read = INVALID_HANDLE_VALUE;
    HANDLE read_pipe_write = INVALID_HANDLE_VALUE;
    DWORD flags = 0U;
    bool broker_initialized = false;
    int rc = -1;

    memset(&attrs, 0, sizeof(attrs));
    attrs.nLength = sizeof(attrs);
    attrs.bInheritHandle = TRUE;
    if (!CreatePipe(&read_pipe_read, &read_pipe_write, &attrs, 0U)) {
        return -1;
    }
    if (!GetHandleInformation(read_pipe_read, &flags) ||
        (flags & HANDLE_FLAG_INHERIT) == 0U) {
        goto done;
    }

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        goto done;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        goto done;
    }
    broker_initialized = true;

    /*
     * POSIX descriptor registration clears FD_CLOEXEC.  Windows must clear the
     * equivalent HANDLE_FLAG_INHERIT bit too; otherwise broker-registered
     * HANDLE authority can leak into child processes created with handle
     * inheritance enabled.
     */
    if (llam_broker_register_handle(&broker,
                                    (llam_handle_t)read_pipe_read,
                                    LLAM_CAP_RIGHT_READ,
                                    false,
                                    &token) != 0) {
        goto done;
    }
    flags = HANDLE_FLAG_INHERIT;
    if (!GetHandleInformation(read_pipe_read, &flags) ||
        (flags & HANDLE_FLAG_INHERIT) != 0U) {
        fprintf(stderr, "[test_security_capability] broker registered HANDLE remained inheritable\n");
        goto done;
    }
    rc = 0;

done:
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    if (read_pipe_read != INVALID_HANDLE_VALUE) {
        CloseHandle(read_pipe_read);
    }
    if (read_pipe_write != INVALID_HANDLE_VALUE) {
        CloseHandle(read_pipe_write);
    }
    return rc;
}

typedef struct broker_pipe_server_state {
    llam_broker_t *broker;
    llam_handle_t handle;
    int rc;
    int error_code;
} broker_pipe_server_state_t;

typedef struct broker_pipe_local_server_state {
    llam_broker_t *broker;
    const char *name;
    size_t max_connections;
    int rc;
} broker_pipe_local_server_state_t;

static DWORD WINAPI broker_pipe_server_thread(LPVOID arg) {
    broker_pipe_server_state_t *state = (broker_pipe_server_state_t *)arg;

    state->rc = llam_broker_serve_handle(state->broker, state->handle);
    state->error_code = state->rc == 0 ? 0 : (errno != 0 ? errno : EIO);
    return 0U;
}

static DWORD WINAPI broker_pipe_local_server_thread(LPVOID arg) {
    broker_pipe_local_server_state_t *state = (broker_pipe_local_server_state_t *)arg;

    state->rc = llam_broker_serve_local_n(state->broker, state->name, state->max_connections);
    return 0U;
}

static int broker_pipe_connect_and_close(const char *name) {
    llam_handle_t client = LLAM_INVALID_HANDLE;

    if (llam_broker_connect_pipe(name, &client) != 0) {
        return -1;
    }
    llam_broker_close_handle(client);
    return 0;
}

static int broker_pipe_self_test_after_malformed(const char *name) {
    ULONGLONG deadline = GetTickCount64() + 5000U;
    int last_errno = 0;
    bool retryable;

    /*
     * Windows can retire a just-broken named-pipe instance asynchronously after
     * a client connects and closes before a full broker request.  The broker
     * contract is that a later well-formed session succeeds; tolerate transient
     * transport-level pipe teardown while still failing protocol/authority
     * errors immediately.
     */
    for (;;) {
        if (llam_broker_client_self_test_local(name) == 0) {
            return 0;
        }
        last_errno = errno != 0 ? errno : EIO;
        retryable = last_errno == EPIPE || last_errno == EAGAIN || last_errno == ENOENT;
        if (!retryable || GetTickCount64() >= deadline) {
            errno = last_errno;
            return -1;
        }
        Sleep(25U);
    }
}

static int broker_pipe_write_request_plain(llam_handle_t handle, const llam_broker_wire_request_t *request) {
    const unsigned char *cursor = (const unsigned char *)request;
    size_t done = 0U;

    if (llam_handle_is_invalid(handle) || request == NULL) {
        errno = EINVAL;
        return -1;
    }
    while (done < sizeof(*request)) {
        DWORD chunk = (DWORD)(sizeof(*request) - done);
        DWORD written = 0U;

        if (!WriteFile((HANDLE)handle, cursor + done, chunk, &written, NULL)) {
            errno = llam_broker_windows_pipe_errno(GetLastError());
            return -1;
        }
        if (written == 0U) {
            errno = EPIPE;
            return -1;
        }
        done += (size_t)written;
    }
    return 0;
}

static int test_broker_pipe_transport_handle_grants(void) {
    static const char inbound[] = "pipe transport handle read";
    static const char outbound[] = "pipe transport handle write";
    char name[128];
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    broker_pipe_server_state_t server_state;
    HANDLE server_thread = NULL;
    llam_handle_t server_pipe = LLAM_INVALID_HANDLE;
    llam_handle_t client = LLAM_INVALID_HANDLE;
    HANDLE read_pipe_read = INVALID_HANDLE_VALUE;
    HANDLE read_pipe_write = INVALID_HANDLE_VALUE;
    HANDLE write_pipe_read = INVALID_HANDLE_VALUE;
    HANDLE write_pipe_write = INVALID_HANDLE_VALUE;
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    llam_capability_token_t read_token;
    llam_capability_token_t write_token;
    llam_broker_ring_mapping_t transport_ring;
    llam_broker_ring_mapping_t by_name;
    llam_broker_ring_submission_t ring_submission;
    llam_broker_ring_completion_t ring_completions[3];
    llam_broker_ring_stats_t ring_stats;
    char out[sizeof(outbound)];
    uint64_t ring_session_id = 0U;
    size_t drained_count = 0U;
    size_t ring_i;
    DWORD transferred = 0U;
    int broker_initialized = 0;
    int rc = -1;

    memset(&transport_ring, 0, sizeof(transport_ring));
    memset(&by_name, 0, sizeof(by_name));
    transport_ring.mapping_handle = LLAM_INVALID_HANDLE;
    by_name.mapping_handle = LLAM_INVALID_HANDLE;
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = 1;
    snprintf(name, sizeof(name), "llam-broker-grant-%lu", (unsigned long)GetCurrentProcessId());
    if (llam_broker_listen_pipe(name, &server_pipe) != 0) {
        goto done;
    }
    for (unsigned i = 0U; i < 200U; ++i) {
        if (llam_broker_connect_pipe(name, &client) == 0) {
            break;
        }
        Sleep(10U);
    }
    if (llam_handle_is_invalid(client) ||
        (!ConnectNamedPipe((HANDLE)server_pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) ||
        !CreatePipe(&read_pipe_read, &read_pipe_write, NULL, 0U) ||
        !CreatePipe(&write_pipe_read, &write_pipe_write, NULL, 0U)) {
        goto done;
    }
    memset(&server_state, 0, sizeof(server_state));
    server_state.broker = &broker;
    server_state.handle = server_pipe;
    server_state.rc = -1;
    server_thread = CreateThread(NULL, 0U, broker_pipe_server_thread, &server_state, 0U, NULL);
    if (server_thread == NULL) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_RING);
    if (llam_broker_request_handle(client, &request, &response) != 0 ||
        response.status != 0 ||
        response.result2 == 0U ||
        response.result1 != LLAM_BROKER_RING_CAP ||
        response.result0 == 0U ||
        response.result0 == (uint64_t)(uintptr_t)INVALID_HANDLE_VALUE ||
        llam_broker_ring_map_handle((llam_handle_t)(uintptr_t)response.result0, true, &transport_ring) != 0 ||
        transport_ring.ring == NULL ||
        transport_ring.name[0] != '\0') {
        goto done;
    }
    ring_session_id = response.result2;
    errno = 0;
    if (expect_errno(llam_broker_ring_open_shm(transport_ring.name, &by_name),
                     EINVAL,
                     "transport-created private ring exposed a reusable mapping name") != 0) {
        goto done;
    }
    {
        bool broker_owns_ring = false;
        size_t i;

        if (llam_broker_lock(&broker) != 0) {
            goto done;
        }
        for (i = 0U; i < LLAM_BROKER_RING_SESSIONS; ++i) {
            const llam_broker_ring_session_t *session = &broker.ring_sessions[i];

            if (session->active &&
                session->owns_mapping &&
                session->ring != NULL &&
                session->mapping_bytes >= sizeof(llam_broker_ring_t) &&
                !llam_handle_is_invalid(session->mapping_handle)) {
                broker_owns_ring = true;
                break;
            }
        }
        llam_broker_unlock(&broker);
        if (!broker_owns_ring) {
            goto done;
        }
    }
    memset(&ring_submission, 0, sizeof(ring_submission));
    ring_submission.request_id = 7002U;
    ring_submission.op = LLAM_BROKER_RING_OP_NOP;
    request_init(&request, LLAM_BROKER_WIRE_OP_SERVE_RING);
    request.slot = ring_session_id + 1U;
    if (llam_broker_ring_submit_push(transport_ring.ring, &ring_submission) != 0 ||
        llam_broker_request_handle(client, &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EINVAL) {
        goto done;
    }
    for (ring_i = 1U; ring_i < 3U; ++ring_i) {
        ring_submission.request_id = 7002U + (uint64_t)ring_i;
        ring_submission.op = LLAM_BROKER_RING_OP_NOP;
        if (llam_broker_ring_submit_push(transport_ring.ring, &ring_submission) != 0) {
            goto done;
        }
    }
    request.slot = ring_session_id;
    request.length = 3U;
    if (llam_broker_request_handle(client, &request, &response) != 0 ||
        response.status != 0 ||
        response.result0 != 3U ||
        llam_broker_ring_complete_drain(transport_ring.ring, ring_completions, 3U, &drained_count) != 0 ||
        drained_count != 3U) {
        goto done;
    }
    for (ring_i = 0U; ring_i < drained_count; ++ring_i) {
        if (ring_completions[ring_i].request_id != 7002U + (uint64_t)ring_i ||
            ring_completions[ring_i].status != 0) {
            goto done;
        }
    }
    if (llam_broker_ring_collect_stats(transport_ring.ring, &ring_stats) != 0 ||
        ring_stats.broker_serve_success != 3U ||
        ring_stats.broker_submit_head_publishes != 1U ||
        ring_stats.broker_complete_tail_publishes != 1U) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    request.rights = LLAM_CAP_RIGHT_READ;
    if (llam_broker_request_handle_with_descriptor(client,
                                                   &request,
                                                   (llam_handle_t)read_pipe_read,
                                                   &response) != 0 ||
        response.status != 0 ||
        response.token.family != LLAM_BROKER_CAP_FAMILY_DESCRIPTOR) {
        goto done;
    }
    read_token = response.token;

    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    request.rights = LLAM_CAP_RIGHT_WRITE;
    if (llam_broker_request_handle_with_descriptor(client,
                                                   &request,
                                                   (llam_handle_t)write_pipe_write,
                                                   &response) != 0 ||
        response.status != 0 ||
        response.token.family != LLAM_BROKER_CAP_FAMILY_DESCRIPTOR) {
        goto done;
    }
    write_token = response.token;

    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    request.rights = LLAM_CAP_RIGHT_READ;
    if (llam_broker_request_handle(client, &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EINVAL) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    request.rights = LLAM_CAP_RIGHT_ADMIN;
    if (llam_broker_request_handle_with_descriptor(client,
                                                   &request,
                                                   (llam_handle_t)read_pipe_read,
                                                   &response) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }

    if (!WriteFile(read_pipe_write, inbound, (DWORD)sizeof(inbound), &transferred, NULL) ||
        transferred != (DWORD)sizeof(inbound)) {
        goto done;
    }
    request_init(&request, LLAM_BROKER_WIRE_OP_DESCRIPTOR_READ);
    request.token = read_token;
    request.length = sizeof(inbound);
    if (llam_broker_request_handle(client, &request, &response) != 0 ||
        response.status != 0 ||
        response.result0 != sizeof(inbound) ||
        memcmp(response.data, inbound, sizeof(inbound)) != 0) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_DESCRIPTOR_WRITE);
    request.token = write_token;
    request.length = sizeof(outbound);
    memcpy(request.data, outbound, sizeof(outbound));
    if (llam_broker_request_handle(client, &request, &response) != 0 ||
        response.status != 0 ||
        response.result0 != sizeof(outbound)) {
        goto done;
    }
    transferred = 0U;
    if (!ReadFile(write_pipe_read, out, (DWORD)sizeof(out), &transferred, NULL) ||
        transferred != (DWORD)sizeof(out) ||
        memcmp(out, outbound, sizeof(out)) != 0) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_DESCRIPTOR_WRITE);
    request.token = read_token;
    request.length = 1U;
    request.data[0] = 1U;
    if (llam_broker_request_handle(client, &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }
    request_init(&request, LLAM_BROKER_WIRE_OP_DESCRIPTOR_READ);
    request.token = write_token;
    request.length = 1U;
    if (llam_broker_request_handle(client, &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_STOP);
    if (llam_broker_request_handle(client, &request, &response) != 0 || response.status != 0) {
        goto done;
    }
    rc = 0;

done:
    llam_broker_ring_unmap(&transport_ring);
    llam_broker_ring_unmap(&by_name);
    if (rc != 0 && !llam_handle_is_invalid(client)) {
        llam_broker_close_handle(client);
        client = LLAM_INVALID_HANDLE;
    }
    if (server_thread != NULL) {
        DWORD wait_rc = WaitForSingleObject(server_thread, 5000U);

        CloseHandle(server_thread);
        if (wait_rc != WAIT_OBJECT_0 || server_state.rc != 0) {
            rc = -1;
        }
    }
    if (!llam_handle_is_invalid(client)) {
        llam_broker_close_handle(client);
    }
    if (!llam_handle_is_invalid(server_pipe)) {
        llam_broker_close_handle(server_pipe);
    }
    if (read_pipe_read != INVALID_HANDLE_VALUE) {
        CloseHandle(read_pipe_read);
    }
    if (read_pipe_write != INVALID_HANDLE_VALUE) {
        CloseHandle(read_pipe_write);
    }
    if (write_pipe_read != INVALID_HANDLE_VALUE) {
        CloseHandle(write_pipe_read);
    }
    if (write_pipe_write != INVALID_HANDLE_VALUE) {
        CloseHandle(write_pipe_write);
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_pipe_create_ring_write_failure_closes_remote_handle(void) {
    char name[128];
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    broker_pipe_server_state_t server_state;
    llam_broker_wire_request_t request;
    HANDLE server_thread = NULL;
    llam_handle_t server_pipe = LLAM_INVALID_HANDLE;
    llam_handle_t client = LLAM_INVALID_HANDLE;
    DWORD handles_before = 0U;
    DWORD handles_after = 0U;
    DWORD wait_rc;
    bool leaked_session = false;
    int broker_initialized = 0;
    int rc = -1;
    size_t i;

    if (!GetProcessHandleCount(GetCurrentProcess(), &handles_before)) {
        return -1;
    }
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = 1;
    snprintf(name, sizeof(name), "llam-broker-ring-fail-%lu", (unsigned long)GetCurrentProcessId());
    if (llam_broker_listen_pipe(name, &server_pipe) != 0) {
        goto done;
    }
    for (unsigned attempt = 0U; attempt < 200U; ++attempt) {
        if (llam_broker_connect_pipe(name, &client) == 0) {
            break;
        }
        Sleep(10U);
    }
    if (llam_handle_is_invalid(client) ||
        (!ConnectNamedPipe((HANDLE)server_pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)) {
        goto done;
    }
    memset(&server_state, 0, sizeof(server_state));
    server_state.broker = &broker;
    server_state.handle = server_pipe;
    server_state.rc = -1;
    server_state.error_code = 0;
    server_thread = CreateThread(NULL, 0U, broker_pipe_server_thread, &server_state, 0U, NULL);
    if (server_thread == NULL) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_RING);
    if (broker_pipe_write_request_plain(client, &request) != 0) {
        goto done;
    }
    /*
     * The server will create a broker-owned ring and duplicate a mapping HANDLE
     * into the pipe peer before it tries to write the response. Closing the
     * client pipe here forces the response write to fail; the broker must close
     * that already-duplicated peer HANDLE with DUPLICATE_CLOSE_SOURCE.
     */
    llam_broker_close_handle(client);
    client = LLAM_INVALID_HANDLE;

    wait_rc = WaitForSingleObject(server_thread, 5000U);
    if (wait_rc != WAIT_OBJECT_0 || server_state.rc == 0) {
        fprintf(stderr,
                "[test_security_capability] pipe CREATE_RING write-failure server rc=%d errno=%d wait=%lu\n",
                server_state.rc,
                server_state.error_code,
                (unsigned long)wait_rc);
        goto done;
    }
    CloseHandle(server_thread);
    server_thread = NULL;

    if (llam_broker_lock(&broker) != 0) {
        goto done;
    }
    for (i = 0U; i < LLAM_BROKER_RING_SESSIONS; ++i) {
        if (broker.ring_sessions[i].active || broker.ring_sessions[i].owns_mapping) {
            leaked_session = true;
            break;
        }
    }
    llam_broker_unlock(&broker);
    if (leaked_session) {
        fprintf(stderr, "[test_security_capability] pipe CREATE_RING write failure leaked broker ring session\n");
        goto done;
    }

    if (!llam_handle_is_invalid(server_pipe)) {
        llam_broker_close_handle(server_pipe);
        server_pipe = LLAM_INVALID_HANDLE;
    }
    llam_broker_destroy(&broker);
    broker_initialized = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles_after)) {
        goto done;
    }
    if (handles_after > handles_before) {
        fprintf(stderr,
                "[test_security_capability] pipe CREATE_RING write failure leaked handles before=%lu after=%lu\n",
                (unsigned long)handles_before,
                (unsigned long)handles_after);
        goto done;
    }
    rc = 0;

done:
    if (!llam_handle_is_invalid(client)) {
        llam_broker_close_handle(client);
    }
    if (server_thread != NULL) {
        (void)WaitForSingleObject(server_thread, 5000U);
        CloseHandle(server_thread);
    }
    if (!llam_handle_is_invalid(server_pipe)) {
        llam_broker_close_handle(server_pipe);
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_pipe_serve_local_n_survives_malformed_session(void) {
    char name[128];
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    broker_pipe_local_server_state_t state;
    HANDLE server_thread = NULL;
    int broker_initialized = 0;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = 1;
    snprintf(name,
             sizeof(name),
             "llam-broker-malformed-%lu-%lu-%llu",
             (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetCurrentThreadId(),
             (unsigned long long)GetTickCount64());
    memset(&state, 0, sizeof(state));
    state.broker = &broker;
    state.name = name;
    state.max_connections = 16U;
    state.rc = -1;
    server_thread = CreateThread(NULL, 0U, broker_pipe_local_server_thread, &state, 0U, NULL);
    if (server_thread == NULL) {
        goto done;
    }

    /*
     * Match the POSIX transport hardening: a named-pipe client that connects
     * and closes before sending a complete request must not terminate the
     * long-running broker process or poison the next session's subject id.
     */
    if (broker_pipe_connect_and_close(name) != 0) {
        fprintf(stderr,
                "[test_security_capability] malformed pipe client connect-close failed errno=%d\n",
                errno);
        goto done;
    }
    Sleep(100U);
    if (broker_pipe_self_test_after_malformed(name) != 0) {
        fprintf(stderr,
                "[test_security_capability] malformed pipe follow-up self-test failed errno=%d\n",
                errno);
        goto done;
    }
    rc = 0;

done:
    if (server_thread != NULL) {
        for (unsigned i = 0U; i < state.max_connections; ++i) {
            (void)broker_pipe_connect_and_close(name);
            Sleep(10U);
        }
        {
            DWORD wait_rc = WaitForSingleObject(server_thread, 5000U);

            CloseHandle(server_thread);
            if (wait_rc != WAIT_OBJECT_0 || state.rc != 0) {
                fprintf(stderr,
                        "[test_security_capability] malformed pipe local server rc=%d wait=%lu\n",
                        state.rc,
                        (unsigned long)wait_rc);
                rc = -1;
            }
        }
    }
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}

static int test_broker_ring_mapping_has_restricted_dacl(void) {
    char name[128];
    llam_broker_ring_mapping_t owner;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    PACL dacl = NULL;
    ACL_SIZE_INFORMATION acl_info;
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    PSID system_sid = NULL;
    PSID admins_sid = NULL;
    HANDLE process_token = NULL;
    TOKEN_USER *token_user = NULL;
    DWORD token_user_bytes = 0U;
    bool has_owner = false;
    bool has_system = false;
    bool has_admins = false;
    int rc = -1;

    snprintf(name, sizeof(name), "Local\\llam-broker-ring-dacl-%lu", (unsigned long)GetCurrentProcessId());
    if (llam_broker_ring_create_shm(name, &owner) != 0) {
        return -1;
    }
    if (GetSecurityInfo((HANDLE)owner.mapping_handle,
                        SE_KERNEL_OBJECT,
                        DACL_SECURITY_INFORMATION,
                        NULL,
                        NULL,
                        &dacl,
                        NULL,
                        &descriptor) != ERROR_SUCCESS ||
        dacl == NULL ||
        !GetAclInformation(dacl, &acl_info, sizeof(acl_info), AclSizeInformation) ||
        !AllocateAndInitializeSid(&nt_authority,
                                  1U,
                                  SECURITY_LOCAL_SYSTEM_RID,
                                  0U,
                                  0U,
                                  0U,
                                  0U,
                                  0U,
                                  0U,
                                  0U,
                                  &system_sid) ||
        !AllocateAndInitializeSid(&nt_authority,
                                  2U,
                                  SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS,
                                  0U,
                                  0U,
                                  0U,
                                  0U,
                                  0U,
                                  0U,
                                  &admins_sid) ||
        !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token)) {
        goto done;
    }
    if (GetTokenInformation(process_token, TokenUser, NULL, 0U, &token_user_bytes) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        token_user_bytes == 0U) {
        goto done;
    }
    token_user = (TOKEN_USER *)calloc(1U, token_user_bytes);
    if (token_user == NULL ||
        !GetTokenInformation(process_token, TokenUser, token_user, token_user_bytes, &token_user_bytes)) {
        goto done;
    }
    for (DWORD i = 0U; i < acl_info.AceCount; ++i) {
        void *ace = NULL;
        ACCESS_ALLOWED_ACE *allowed;
        PSID sid;

        if (!GetAce(dacl, i, &ace)) {
            goto done;
        }
        allowed = (ACCESS_ALLOWED_ACE *)ace;
        if (allowed->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) {
            continue;
        }
        sid = (PSID)&allowed->SidStart;
        if (EqualSid(sid, token_user->User.Sid)) {
            has_owner = true;
        } else if (EqualSid(sid, system_sid)) {
            has_system = true;
        } else if (EqualSid(sid, admins_sid)) {
            has_admins = true;
        } else {
            fprintf(stderr, "[test_security_capability] unexpected ring mapping DACL ACE\n");
            goto done;
        }
    }
    if (!has_owner || !has_system || !has_admins) {
        goto done;
    }
    rc = 0;

done:
    if (token_user != NULL) {
        free(token_user);
    }
    if (process_token != NULL) {
        CloseHandle(process_token);
    }
    if (system_sid != NULL) {
        FreeSid(system_sid);
    }
    if (admins_sid != NULL) {
        FreeSid(admins_sid);
    }
    if (descriptor != NULL) {
        LocalFree(descriptor);
    }
    llam_broker_ring_unmap(&owner);
    return rc;
}

static int test_broker_ring_handle_mapping_authority(void) {
    llam_broker_ring_mapping_t owner;
    llam_broker_ring_mapping_t peer;
    llam_broker_ring_mapping_t by_name;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_submission_t popped_submission;
    HANDLE peer_handle = NULL;
    int rc = -1;

    memset(&owner, 0, sizeof(owner));
    memset(&peer, 0, sizeof(peer));
    memset(&by_name, 0, sizeof(by_name));

    if (llam_broker_ring_create_private_handle(&owner) != 0 ||
        owner.ring == NULL ||
        LLAM_HANDLE_IS_INVALID(owner.mapping_handle) ||
        !owner.owner ||
        owner.name[0] != '\0') {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_open_shm(owner.name, &by_name),
                     EINVAL,
                     "handle-backed private ring exposed a reusable mapping name") != 0) {
        goto done;
    }
    if (!DuplicateHandle(GetCurrentProcess(),
                         (HANDLE)owner.mapping_handle,
                         GetCurrentProcess(),
                         &peer_handle,
                         0U,
                         FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        goto done;
    }
    if (llam_broker_ring_map_handle((llam_handle_t)peer_handle, true, &peer) != 0 ||
        peer.ring == NULL ||
        LLAM_HANDLE_IS_INVALID(peer.mapping_handle) ||
        peer.mapping_handle == owner.mapping_handle ||
        peer.owner ||
        peer.name[0] != '\0') {
        goto done;
    }
    peer_handle = NULL;

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 654U;
    submission.op = LLAM_BROKER_RING_OP_CHANNEL_SEND;
    submission.arg1 = 16U;
    submission.arg2 = 64U;
    if (llam_broker_ring_submit_push(owner.ring, &submission) != 0 ||
        llam_broker_ring_submit_pop(peer.ring, &popped_submission) != 0 ||
        popped_submission.request_id != submission.request_id ||
        popped_submission.op != submission.op ||
        popped_submission.arg1 != submission.arg1 ||
        popped_submission.arg2 != submission.arg2) {
        goto done;
    }

    rc = 0;

done:
    if (peer_handle != NULL) {
        CloseHandle(peer_handle);
    }
    llam_broker_ring_unmap(&peer);
    llam_broker_ring_unmap(&by_name);
    llam_broker_ring_unmap(&owner);
    return rc;
}
#endif

static int test_broker_ring_shared_memory_mapping(void) {
    llam_broker_ring_mapping_t owner;
    llam_broker_ring_mapping_t peer;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_submission_t popped_submission;
    llam_broker_ring_completion_t completion;
    llam_broker_ring_completion_t popped_completion;
    int rc = -1;

    memset(&owner, 0, sizeof(owner));
    memset(&peer, 0, sizeof(peer));

    if (llam_broker_ring_create_private_shm(&owner) != 0 ||
        owner.ring == NULL ||
        !owner.owner ||
        owner.name[0] == '\0') {
        goto done;
    }
    if (llam_broker_ring_open_shm(owner.name, &peer) != 0 ||
        peer.ring == NULL ||
        peer.owner) {
        goto done;
    }
#if !LLAM_PLATFORM_WINDOWS
    /*
     * Named POSIX ring mappings are also broker data-plane authority.  The fd
     * must be close-on-exec both for the creator and for later named imports.
     */
    if (owner.fd < 0 ||
        peer.fd < 0 ||
        !broker_fd_has_cloexec(owner.fd) ||
        !broker_fd_has_cloexec(peer.fd)) {
        fprintf(stderr, "[test_security_capability] named ring shm fd is inheritable across exec\n");
        goto done;
    }
#endif

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 123U;
    submission.op = LLAM_BROKER_RING_OP_BUFFER_READ;
    submission.arg0 = 4096U;
    submission.arg1 = 64U;
    if (llam_broker_ring_submit_push(owner.ring, &submission) != 0 ||
        llam_broker_ring_submit_pop(peer.ring, &popped_submission) != 0 ||
        popped_submission.request_id != submission.request_id ||
        popped_submission.op != submission.op ||
        popped_submission.arg0 != submission.arg0 ||
        popped_submission.arg1 != submission.arg1) {
        goto done;
    }

    memset(&completion, 0, sizeof(completion));
    completion.request_id = popped_submission.request_id;
    completion.status = 0;
    completion.result0 = 64U;
    if (llam_broker_ring_complete_push(peer.ring, &completion) != 0 ||
        llam_broker_ring_complete_pop(owner.ring, &popped_completion) != 0 ||
        popped_completion.request_id != completion.request_id ||
        popped_completion.result0 != completion.result0) {
        goto done;
    }

    rc = 0;

done:
    llam_broker_ring_unmap(&peer);
    llam_broker_ring_unmap(&owner);
    return rc;
}

#if !LLAM_PLATFORM_WINDOWS
static int test_broker_ring_fd_mapping_authority(void) {
    llam_broker_ring_mapping_t owner;
    llam_broker_ring_mapping_t peer;
    llam_broker_ring_mapping_t by_name;
    llam_broker_ring_submission_t submission;
    llam_broker_ring_submission_t popped_submission;
    int rc = -1;

    memset(&owner, 0, sizeof(owner));
    memset(&peer, 0, sizeof(peer));
    memset(&by_name, 0, sizeof(by_name));
    owner.fd = -1;
    peer.fd = -1;
    by_name.fd = -1;

    if (llam_broker_ring_create_private_fd(&owner) != 0 ||
        owner.ring == NULL ||
        owner.fd < 0 ||
        !broker_fd_has_cloexec(owner.fd) ||
        !owner.owner ||
        owner.name[0] != '\0') {
        goto done;
    }
    errno = 0;
    if (expect_errno(llam_broker_ring_open_shm(owner.name, &by_name),
                     EINVAL,
                     "fd-backed private ring exposed a reusable shm name") != 0) {
        goto done;
    }
    if (llam_broker_ring_map_fd(owner.fd, false, &peer) != 0 ||
        peer.ring == NULL ||
        peer.fd < 0 ||
        peer.fd == owner.fd ||
        !broker_fd_has_cloexec(peer.fd) ||
        peer.owner ||
        peer.name[0] != '\0') {
        goto done;
    }

    memset(&submission, 0, sizeof(submission));
    submission.request_id = 321U;
    submission.op = LLAM_BROKER_RING_OP_CHANNEL_RECV;
    submission.arg1 = 32U;
    submission.arg2 = 128U;
    if (llam_broker_ring_submit_push(owner.ring, &submission) != 0 ||
        llam_broker_ring_submit_pop(peer.ring, &popped_submission) != 0 ||
        popped_submission.request_id != submission.request_id ||
        popped_submission.op != submission.op ||
        popped_submission.arg1 != submission.arg1 ||
        popped_submission.arg2 != submission.arg2) {
        goto done;
    }

    rc = 0;

done:
    llam_broker_ring_unmap(&peer);
    llam_broker_ring_unmap(&by_name);
    llam_broker_ring_unmap(&owner);
    return rc;
}

typedef struct broker_transport_thread_state {
    llam_broker_t *broker;
    int fd;
    int rc;
} broker_transport_thread_state_t;

typedef struct broker_local_server_thread_state {
    llam_broker_t *broker;
    const char *path;
    size_t max_connections;
    int rc;
} broker_local_server_thread_state_t;

static void *broker_transport_thread(void *arg) {
    broker_transport_thread_state_t *state = (broker_transport_thread_state_t *)arg;

    state->rc = llam_broker_serve_fd(state->broker, state->fd);
    return NULL;
}

static void *broker_local_server_thread(void *arg) {
    broker_local_server_thread_state_t *state = (broker_local_server_thread_state_t *)arg;

    state->rc = llam_broker_serve_local_n(state->broker, state->path, state->max_connections);
    return NULL;
}

static int broker_connect_and_close(const char *path) {
    int fd = -1;

    if (llam_broker_connect_unix(path, &fd) != 0) {
        return -1;
    }
    (void)shutdown(fd, SHUT_RDWR);
    close(fd);
    return 0;
}

static int broker_wait_for_socket_path(const char *path) {
    size_t i;

    for (i = 0U; i < 200U; ++i) {
        struct stat st;

        if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
            return 0;
        }
        usleep(5000);
    }
    errno = ETIMEDOUT;
    return -1;
}

static int broker_client_self_test_unix_retry(const char *path, size_t attempts) {
    int last_errno = 0;
    size_t i;

    if (path == NULL || attempts == 0U) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Hosted macOS can reorder AF_UNIX backlog progress when a connect-close
     * session races a real self-test client. Retry the well-formed session so
     * this test proves broker liveness after malformed input rather than a
     * scheduler-specific accept ordering.
     */
    for (i = 0U; i < attempts; ++i) {
        errno = 0;
        if (llam_broker_client_self_test_unix(path) == 0) {
            return 0;
        }
        last_errno = errno != 0 ? errno : EIO;
        usleep(20000U * (useconds_t)(i + 1U));
    }
    errno = last_errno != 0 ? last_errno : EIO;
    return -1;
}

static int test_broker_socketpair_transport(void) {
    static const unsigned char buffer_payload[] = {'w', 'i', 'r', 'e'};
    static const unsigned char channel_payload[] = {'m', 's', 'g'};
    static const unsigned char descriptor_read_payload[] = {'f', 'd', '-', 'r', 'e', 'a', 'd'};
    static const unsigned char descriptor_write_payload[] = {'f', 'd', '-', 'w', 'r', 'i', 't', 'e'};
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    int fds[2] = {-1, -1};
    int read_pipe[2] = {-1, -1};
    int write_pipe[2] = {-1, -1};
    pthread_t thread;
    broker_transport_thread_state_t state;
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    llam_capability_token_t token;
    llam_capability_token_t channel_token;
    llam_capability_token_t descriptor_read_token;
    llam_capability_token_t descriptor_write_token;
    llam_capability_token_t task_token;
    llam_capability_token_t replacement;
    llam_broker_ring_mapping_t transport_ring;
    llam_broker_ring_mapping_t by_name;
    llam_broker_ring_submission_t ring_submission;
    llam_broker_ring_completion_t ring_completions[3];
    llam_broker_ring_stats_t ring_stats;
    unsigned char descriptor_result[sizeof(descriptor_write_payload)];
    uint64_t ring_session_id = 0U;
    size_t drained_count = 0U;
    size_t ring_i;
    bool thread_started = false;
    int ring_fd = -1;
    int rc = -1;

    memset(&transport_ring, 0, sizeof(transport_ring));
    memset(&by_name, 0, sizeof(by_name));
    transport_ring.fd = -1;
    by_name.fd = -1;
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        llam_broker_destroy(&broker);
        return -1;
    }
    if (pipe(read_pipe) != 0 || pipe(write_pipe) != 0) {
        close(fds[0]);
        close(fds[1]);
        if (read_pipe[0] >= 0) {
            close(read_pipe[0]);
        }
        if (read_pipe[1] >= 0) {
            close(read_pipe[1]);
        }
        if (write_pipe[0] >= 0) {
            close(write_pipe[0]);
        }
        if (write_pipe[1] >= 0) {
            close(write_pipe[1]);
        }
        llam_broker_destroy(&broker);
        return -1;
    }
    state.broker = &broker;
    state.fd = fds[1];
    state.rc = -1;
    if (pthread_create(&thread, NULL, broker_transport_thread, &state) != 0) {
        close(fds[0]);
        close(fds[1]);
        llam_broker_destroy(&broker);
        return -1;
    }
    thread_started = true;

    request_init(&request, LLAM_BROKER_WIRE_OP_PING);
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status != 0 ||
        response.runtime_id == 0U) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_RING);
    if (llam_broker_request_fd_with_response_descriptor(fds[0], &request, &response, &ring_fd) != 0 ||
        response.status != 0 ||
        response.result2 == 0U ||
        response.result1 != LLAM_BROKER_RING_CAP ||
        ring_fd < 0 ||
        llam_broker_ring_map_fd(ring_fd, true, &transport_ring) != 0 ||
        transport_ring.ring == NULL ||
        transport_ring.name[0] != '\0') {
        if (ring_fd >= 0) {
            close(ring_fd);
            ring_fd = -1;
        }
        goto done;
    }
    ring_fd = -1;
    ring_session_id = response.result2;
    errno = 0;
    if (expect_errno(llam_broker_ring_open_shm(transport_ring.name, &by_name),
                     EINVAL,
                     "transport-created private ring exposed a reusable shm name") != 0) {
        goto done;
    }
    {
        bool broker_owns_ring = false;
        size_t i;

        if (llam_broker_lock(&broker) != 0) {
            goto done;
        }
        for (i = 0U; i < LLAM_BROKER_RING_SESSIONS; ++i) {
            const llam_broker_ring_session_t *session = &broker.ring_sessions[i];

            if (session->active &&
                session->owns_mapping &&
                session->ring != NULL &&
                session->mapping_bytes >= sizeof(llam_broker_ring_t) &&
                session->mapping_fd >= 0) {
                broker_owns_ring = true;
                break;
            }
        }
        llam_broker_unlock(&broker);
        if (!broker_owns_ring) {
            goto done;
        }
    }
    memset(&ring_submission, 0, sizeof(ring_submission));
    ring_submission.request_id = 7001U;
    ring_submission.op = LLAM_BROKER_RING_OP_NOP;
    request_init(&request, LLAM_BROKER_WIRE_OP_SERVE_RING);
    request.slot = ring_session_id + 1U;
    if (llam_broker_ring_submit_push(transport_ring.ring, &ring_submission) != 0 ||
        llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EINVAL) {
        goto done;
    }
    for (ring_i = 1U; ring_i < 3U; ++ring_i) {
        ring_submission.request_id = 7001U + (uint64_t)ring_i;
        ring_submission.op = LLAM_BROKER_RING_OP_NOP;
        if (llam_broker_ring_submit_push(transport_ring.ring, &ring_submission) != 0) {
            goto done;
        }
    }
    request.slot = ring_session_id;
    request.length = 3U;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status != 0 ||
        response.result0 != 3U ||
        llam_broker_ring_complete_drain(transport_ring.ring, ring_completions, 3U, &drained_count) != 0 ||
        drained_count != 3U) {
        goto done;
    }
    for (ring_i = 0U; ring_i < drained_count; ++ring_i) {
        if (ring_completions[ring_i].request_id != 7001U + (uint64_t)ring_i ||
            ring_completions[ring_i].status != 0) {
            goto done;
        }
    }
    if (llam_broker_ring_collect_stats(transport_ring.ring, &ring_stats) != 0 ||
        ring_stats.broker_serve_success != 3U ||
        ring_stats.broker_submit_head_publishes != 1U ||
        ring_stats.broker_complete_tail_publishes != 1U) {
        goto done;
    }

    /*
     * Attaching SCM_RIGHTS to a non-register op is malformed authority. The
     * broker must not process this as a successful PING while silently keeping
     * the received fd alive.
     */
    request_init(&request, LLAM_BROKER_WIRE_OP_PING);
    if (llam_broker_request_fd_with_descriptor(fds[0], &request, read_pipe[0], &response) != 0 ||
        response.status == 0 ||
        response.error_code != EINVAL) {
        goto done;
    }
    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    request.magic ^= UINT32_C(0x1);
    if (llam_broker_request_fd_with_descriptor(fds[0], &request, read_pipe[0], &response) != 0 ||
        response.status == 0 ||
        response.error_code != EINVAL) {
        goto done;
    }

    /*
     * Descriptor grants over the control socket must use SCM_RIGHTS. A raw
     * integer in the wire request is never authority, so a remote client cannot
     * forge broker fd ownership by guessing descriptor numbers.
     */
    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    request.rights = LLAM_CAP_RIGHT_READ;
    if (llam_broker_request_fd_with_descriptor(fds[0], &request, read_pipe[0], &response) != 0 ||
        response.status != 0 ||
        response.token.family != LLAM_BROKER_CAP_FAMILY_DESCRIPTOR) {
        goto done;
    }
    descriptor_read_token = response.token;

    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    request.rights = LLAM_CAP_RIGHT_WRITE;
    if (llam_broker_request_fd_with_descriptor(fds[0], &request, write_pipe[1], &response) != 0 ||
        response.status != 0 ||
        response.token.family != LLAM_BROKER_CAP_FAMILY_DESCRIPTOR) {
        goto done;
    }
    descriptor_write_token = response.token;

    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    request.rights = LLAM_CAP_RIGHT_READ;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EINVAL) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR);
    request.rights = LLAM_CAP_RIGHT_ADMIN;
    if (llam_broker_request_fd_with_descriptor(fds[0], &request, read_pipe[0], &response) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_BUFFER);
    request.slot = 128U;
    request.rights = LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_WRITE | LLAM_CAP_RIGHT_DESTROY;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status != 0 ||
        response.token.family != LLAM_BROKER_CAP_FAMILY_BUFFER ||
        response.token.subject_id == 0U) {
        goto done;
    }
    token = response.token;

    request_init(&request, LLAM_BROKER_WIRE_OP_BUFFER_WRITE);
    request.token = token;
    request.offset = 8U;
    request.length = sizeof(buffer_payload);
    memcpy(request.data, buffer_payload, sizeof(buffer_payload));
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status != 0 ||
        response.result0 != sizeof(buffer_payload)) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_BUFFER_READ);
    request.token = token;
    request.offset = 8U;
    request.length = sizeof(buffer_payload);
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status != 0 ||
        response.result0 != sizeof(buffer_payload) ||
        memcmp(response.data, buffer_payload, sizeof(buffer_payload)) != 0) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_ISSUE_CAP);
    request.family = token.family;
    request.slot = token.slot;
    request.generation = token.generation;
    request.rights = LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_WRITE | LLAM_CAP_RIGHT_DESTROY;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_BUFFER);
    request.slot = 16U;
    request.rights = LLAM_CAP_RIGHT_ADMIN;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_BUFFER);
    request.slot = (uint64_t)LLAM_BROKER_BUFFER_MAX_BYTES + 1U;
    request.rights = LLAM_CAP_RIGHT_READ;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EINVAL) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_CHANNEL);
    request.slot = 4U;
    request.rights = LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_RECV | LLAM_CAP_RIGHT_CLOSE;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status != 0 ||
        response.token.family != LLAM_BROKER_CAP_FAMILY_CHANNEL) {
        goto done;
    }
    channel_token = response.token;
    request_init(&request, LLAM_BROKER_WIRE_OP_VALIDATE_CAP);
    request.required_rights = LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_CLOSE;
    request.token = channel_token;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 || response.status != 0) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_CHANNEL_SEND);
    request.token = channel_token;
    request.length = sizeof(channel_payload);
    memcpy(request.data, channel_payload, sizeof(channel_payload));
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status != 0 ||
        response.result0 != sizeof(channel_payload)) {
        goto done;
    }
    request_init(&request, LLAM_BROKER_WIRE_OP_CHANNEL_RECV);
    request.token = channel_token;
    request.length = LLAM_BROKER_WIRE_DATA_BYTES;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status != 0 ||
        response.result0 != sizeof(channel_payload) ||
        memcmp(response.data, channel_payload, sizeof(channel_payload)) != 0) {
        goto done;
    }

    /*
     * The control transport never accepts raw fd numbers as authority. These
     * requests use descriptor tokens that were registered inside the trusted
     * broker address space, proving the IPC path routes through broker-owned
     * descriptor slots and the same right checks as the shared-memory ring path.
     */
    if (write(read_pipe[1], descriptor_read_payload, sizeof(descriptor_read_payload)) !=
        (ssize_t)sizeof(descriptor_read_payload)) {
        goto done;
    }
    request_init(&request, LLAM_BROKER_WIRE_OP_DESCRIPTOR_READ);
    request.token = descriptor_read_token;
    request.length = sizeof(descriptor_read_payload);
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status != 0 ||
        response.result0 != sizeof(descriptor_read_payload) ||
        memcmp(response.data, descriptor_read_payload, sizeof(descriptor_read_payload)) != 0) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_DESCRIPTOR_WRITE);
    request.token = descriptor_write_token;
    request.length = sizeof(descriptor_write_payload);
    memcpy(request.data, descriptor_write_payload, sizeof(descriptor_write_payload));
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status != 0 ||
        response.result0 != sizeof(descriptor_write_payload)) {
        goto done;
    }
    if (read(write_pipe[0], descriptor_result, sizeof(descriptor_result)) !=
        (ssize_t)sizeof(descriptor_result) ||
        memcmp(descriptor_result, descriptor_write_payload, sizeof(descriptor_write_payload)) != 0) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_DESCRIPTOR_WRITE);
    request.token = descriptor_read_token;
    request.length = sizeof(descriptor_write_payload);
    memcpy(request.data, descriptor_write_payload, sizeof(descriptor_write_payload));
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_DESCRIPTOR_READ);
    request.token = descriptor_write_token;
    request.length = sizeof(descriptor_read_payload);
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_DESCRIPTOR_READ);
    request.token = descriptor_read_token;
    request.length = (uint64_t)LLAM_BROKER_WIRE_DATA_BYTES + 1U;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EINVAL) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_TASK_SPAWN);
    request.slot = LLAM_BROKER_TASK_KIND_POPCOUNT_U64;
    request.offset = UINT64_C(0xf0f0f00f);
    request.rights = LLAM_CAP_RIGHT_JOIN | LLAM_CAP_RIGHT_DETACH;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status != 0 ||
        response.token.family != LLAM_BROKER_CAP_FAMILY_TASK) {
        goto done;
    }
    task_token = response.token;
    request_init(&request, LLAM_BROKER_WIRE_OP_TASK_JOIN);
    request.token = task_token;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status != 0 ||
        response.result0 != 16U) {
        goto done;
    }
    request_init(&request, LLAM_BROKER_WIRE_OP_TASK_JOIN);
    request.token = task_token;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_TASK_SPAWN);
    request.slot = LLAM_BROKER_TASK_KIND_RETURN_U64;
    request.offset = 9U;
    request.rights = LLAM_CAP_RIGHT_DETACH;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status != 0 ||
        response.token.family != LLAM_BROKER_CAP_FAMILY_TASK) {
        goto done;
    }
    task_token = response.token;
    request_init(&request, LLAM_BROKER_WIRE_OP_TASK_DETACH);
    request.token = task_token;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status != 0) {
        goto done;
    }
    request_init(&request, LLAM_BROKER_WIRE_OP_TASK_JOIN);
    request.token = task_token;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_TASK_SPAWN);
    request.slot = (uint64_t)UINT32_MAX + 1U;
    request.rights = LLAM_CAP_RIGHT_JOIN;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EINVAL) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_TASK_SPAWN);
    request.slot = LLAM_BROKER_TASK_KIND_RETURN_U64;
    request.rights = LLAM_CAP_RIGHT_ADMIN;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_CHANNEL);
    request.slot = (uint64_t)LLAM_BROKER_CHANNEL_CAPACITY + 1U;
    request.rights = LLAM_CAP_RIGHT_SEND;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EINVAL) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_VALIDATE_CAP);
    request.required_rights = LLAM_CAP_RIGHT_READ;
    request.token = token;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 || response.status != 0) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_REVOKE_CAP);
    request.rights = LLAM_CAP_RIGHT_READ;
    request.token = token;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 || response.status != 0) {
        goto done;
    }
    replacement = response.token;
    request_init(&request, LLAM_BROKER_WIRE_OP_VALIDATE_CAP);
    request.required_rights = LLAM_CAP_RIGHT_READ;
    request.token = token;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }
    token = replacement;

    request_init(&request, LLAM_BROKER_WIRE_OP_ATTENUATE_CAP);
    request.rights = LLAM_CAP_RIGHT_READ;
    request.token = token;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 || response.status != 0) {
        goto done;
    }
    token = response.token;
    request_init(&request, LLAM_BROKER_WIRE_OP_VALIDATE_CAP);
    request.required_rights = LLAM_CAP_RIGHT_WRITE;
    request.token = token;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }

    request.token.rights |= LLAM_CAP_RIGHT_DESTROY;
    request.required_rights = LLAM_CAP_RIGHT_DESTROY;
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 ||
        response.status == 0 ||
        response.error_code != EACCES) {
        goto done;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_STOP);
    if (llam_broker_request_fd(fds[0], &request, &response) != 0 || response.status != 0) {
        goto done;
    }
    rc = 0;

done:
    llam_broker_ring_unmap(&transport_ring);
    llam_broker_ring_unmap(&by_name);
    if (ring_fd >= 0) {
        close(ring_fd);
    }
    if (thread_started && rc != 0 && fds[0] >= 0) {
        (void)shutdown(fds[0], SHUT_RDWR);
    }
    if (thread_started) {
        (void)pthread_join(thread, NULL);
        if (state.rc != 0) {
            rc = -1;
        }
    }
    close(fds[0]);
    close(fds[1]);
    close(read_pipe[0]);
    close(read_pipe[1]);
    close(write_pipe[0]);
    close(write_pipe[1]);
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_create_ring_response_failure_reclaims_session(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_broker_wire_request_t request;
    bool should_close = false;
    bool leaked_session = false;
    int fds[2] = {-1, -1};
    int rc = -1;
    int serve_rc;
    int serve_errno;
    size_t i;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        llam_broker_destroy(&broker);
        return -1;
    }

    request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_RING);
    if (broker_write_request_plain(fds[0], &request) != 0) {
        goto done;
    }
    close(fds[0]);
    fds[0] = -1;

    /*
     * A client can disconnect after causing CREATE_RING to allocate a private
     * broker mapping but before receiving the response fd. The broker must
     * report a transport failure, avoid SIGPIPE termination, and reclaim the
     * just-created session so repeated attempts cannot exhaust ring slots.
     */
    errno = 0;
    serve_rc = llam_broker_serve_one_fd(&broker, fds[1], &should_close);
    serve_errno = errno;
    if (serve_rc == 0 || (serve_errno != EPIPE && serve_errno != ECONNRESET)) {
        fprintf(stderr,
                "[test_security_capability] disconnected CREATE_RING returned rc=%d errno=%d\n",
                serve_rc,
                serve_errno);
        goto done;
    }
    if (llam_broker_lock(&broker) != 0) {
        goto done;
    }
    for (i = 0U; i < LLAM_BROKER_RING_SESSIONS; ++i) {
        if (broker.ring_sessions[i].active || broker.ring_sessions[i].owns_mapping) {
            leaked_session = true;
            break;
        }
    }
    llam_broker_unlock(&broker);
    if (leaked_session) {
        fprintf(stderr, "[test_security_capability] disconnected CREATE_RING leaked a broker ring session\n");
        goto done;
    }
    rc = 0;

done:
    if (fds[0] >= 0) {
        close(fds[0]);
    }
    if (fds[1] >= 0) {
        close(fds[1]);
    }
    llam_broker_destroy(&broker);
    return rc;
}

static int test_broker_serve_local_n_survives_malformed_session(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    broker_local_server_thread_state_t state;
    pthread_t thread;
    char path[128];
    bool thread_started = false;
    bool broker_initialized = false;
    size_t i;
    int rc = -1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    broker_initialized = true;
    snprintf(path, sizeof(path), "/tmp/llam-broker-malformed-%ld.sock", (long)getpid());
    (void)unlink(path);

    state.broker = &broker;
    state.path = path;
    state.max_connections = 8U;
    state.rc = -1;
    if (pthread_create(&thread, NULL, broker_local_server_thread, &state) != 0) {
        goto done;
    }
    thread_started = true;
    if (broker_wait_for_socket_path(path) != 0) {
        goto done;
    }

    /*
     * A malformed client may connect and close before writing a full request.
     * Long-running broker service must isolate that session failure instead of
     * terminating the trusted broker process and dropping later good sessions.
     */
    if (broker_connect_and_close(path) != 0) {
        goto done;
    }
    if (broker_client_self_test_unix_retry(path, 5U) != 0) {
        fprintf(stderr,
                "[test_security_capability] malformed local server follow-up self-test failed errno=%d\n",
                errno);
        goto done;
    }
    rc = 0;

done:
    if (thread_started) {
        /*
         * AF_UNIX accept ordering is not guaranteed when the malformed and
         * well-formed clients race in the listen backlog. Consume the remaining
         * accepted-session budget if the good client happened to run first.
         */
        for (i = 0U; i < state.max_connections; ++i) {
            (void)broker_connect_and_close(path);
            usleep(10000);
        }
    }
    if (thread_started) {
        (void)pthread_join(thread, NULL);
        if (state.rc != 0) {
            rc = -1;
        }
    }
    (void)unlink(path);
    if (broker_initialized) {
        llam_broker_destroy(&broker);
    }
    return rc;
}
#endif

#define LLAM_RUN_SECURITY_TEST(fn)                                               \
    do {                                                                         \
        if (getenv("LLAM_SECURITY_VERBOSE") != NULL) {                           \
            fprintf(stderr, "[test_security_capability] begin %s\n", #fn);       \
            fflush(stderr);                                                      \
        }                                                                        \
        if ((fn)() != 0) {                                                       \
            fprintf(stderr, "[test_security_capability] %s failed\n", #fn);      \
            return 1;                                                            \
        }                                                                        \
        if (getenv("LLAM_SECURITY_VERBOSE") != NULL) {                           \
            fprintf(stderr, "[test_security_capability] ok %s\n", #fn);          \
            fflush(stderr);                                                      \
        }                                                                        \
    } while (0)

int main(int argc, char **argv) {
#if LLAM_PLATFORM_WINDOWS
    if (argc == 5 && strcmp(argv[1], "--broker-ring-windows-session-child") == 0) {
        char *end = NULL;
        unsigned long long iterations;

        errno = 0;
        iterations = strtoull(argv[4], &end, 10);
        if (argv[2][0] == '\0' || argv[3][0] == '\0' || end == argv[4] || *end != '\0') {
            return 2;
        }
        if (errno != 0 || iterations == 0U || (uint64_t)iterations > UINT64_C(1000000)) {
            return 2;
        }
        return broker_ring_windows_session_child(argv[2], argv[3], (uint64_t)iterations);
    }
    if (argc == 4 &&
        (strcmp(argv[1], "--broker-ring-windows-child") == 0 ||
         strcmp(argv[1], "--broker-ring-windows-teardown-child") == 0)) {
        char *end = NULL;
        unsigned long long iterations;

        errno = 0;
        iterations = strtoull(argv[3], &end, 10);
        if (argv[2][0] == '\0' || end == argv[3] || *end != '\0') {
            return 2;
        }
        if (errno != 0 || iterations == 0U || (uint64_t)iterations > UINT64_C(1000000)) {
            return 2;
        }
        if (strcmp(argv[1], "--broker-ring-windows-teardown-child") == 0) {
            return broker_ring_windows_teardown_child(argv[2], (uint64_t)iterations);
        }
        return broker_ring_windows_process_child(argv[2], (uint64_t)iterations);
    }
#else
    (void)argc;
    (void)argv;
#endif

    LLAM_RUN_SECURITY_TEST(test_token_validation_and_rights);
    LLAM_RUN_SECURITY_TEST(test_raw_capability_validate_requires_nonzero_rights);
    LLAM_RUN_SECURITY_TEST(test_token_tamper_rejected);
    LLAM_RUN_SECURITY_TEST(test_token_subject_binding);
    LLAM_RUN_SECURITY_TEST(test_attenuation_cannot_expand_rights);
    LLAM_RUN_SECURITY_TEST(test_wrong_key_rejected);
    LLAM_RUN_SECURITY_TEST(test_capability_key_requires_os_entropy);
    LLAM_RUN_SECURITY_TEST(test_in_process_runtime_does_not_require_broker_entropy);
    LLAM_RUN_SECURITY_TEST(test_capability_issue_requires_os_entropy);
    LLAM_RUN_SECURITY_TEST(test_capability_issue_clears_output_on_invalid_input);
    LLAM_RUN_SECURITY_TEST(test_broker_issue_validate_and_revoke);
    LLAM_RUN_SECURITY_TEST(test_broker_create_paths_clear_output_on_invalid_input);
    LLAM_RUN_SECURITY_TEST(test_broker_transport_grants_require_explicit_rights);
    LLAM_RUN_SECURITY_TEST(test_broker_transport_malformed_requests_fail_closed);
    LLAM_RUN_SECURITY_TEST(test_broker_direct_issue_clears_output_on_invalid_input);
    LLAM_RUN_SECURITY_TEST(test_broker_validate_requires_nonzero_rights);
    LLAM_RUN_SECURITY_TEST(test_broker_destroy_scrubs_authority_state);
    LLAM_RUN_SECURITY_TEST(test_broker_transport_subject_rejects_destroying_broker);
    LLAM_RUN_SECURITY_TEST(test_broker_transport_subject_requires_os_entropy);
    LLAM_RUN_SECURITY_TEST(test_broker_transport_subject_collision_fails_closed);
    LLAM_RUN_SECURITY_TEST(test_broker_attenuate_capability);
    LLAM_RUN_SECURITY_TEST(test_broker_subject_bound_tokens);
    LLAM_RUN_SECURITY_TEST(test_broker_nested_subject_scope_restores_outer);
    LLAM_RUN_SECURITY_TEST(test_broker_nested_subject_conflict_preserves_outer);
    LLAM_RUN_SECURITY_TEST(test_broker_nested_subject_depth_overflow_preserves_scope);
    LLAM_RUN_SECURITY_TEST(test_broker_object_revocation_rotates_generation);
    LLAM_RUN_SECURITY_TEST(test_broker_destroy_drains_unjoined_task_slots);
    LLAM_RUN_SECURITY_TEST(test_broker_destroy_cancels_sleeping_task_slots);
    LLAM_RUN_SECURITY_TEST(test_broker_failed_task_join_consumes_slot);
    LLAM_RUN_SECURITY_TEST(test_broker_rejects_foreign_runtime_token);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_and_buffer_grants);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_doorbell_waits);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_doorbell_flood);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_batch_perf_gate);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_capability_validate_op);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_capability_attenuate_op);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_capability_revoke_op);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_failed_output_windows_are_cleared);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_malformed_submissions_fail_closed);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_reinit_stale_session_fails_closed);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_buffer_data_plane);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_unmap_handles_unterminated_name);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_channel_data_plane);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_task_data_plane);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_subject_bound_session);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_shared_memory_mapping);
    LLAM_RUN_SECURITY_TEST(test_broker_transport_subject_table_fails_closed);
    LLAM_RUN_SECURITY_TEST(test_broker_failure_marker_clears_authority_outputs);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_private_name_clears_short_output);
    LLAM_RUN_SECURITY_TEST(test_broker_control_outputs_clear_on_invalid_input);
#if !LLAM_PLATFORM_WINDOWS
    LLAM_RUN_SECURITY_TEST(test_broker_wire_task_join_sleep_returns_eagain);
    LLAM_RUN_SECURITY_TEST(test_broker_wire_task_join_does_not_drain_peer_sleep);
    LLAM_RUN_SECURITY_TEST(test_broker_detach_reclaims_racing_completed_task_slots);
    LLAM_RUN_SECURITY_TEST(test_broker_concurrent_channel_state);
    LLAM_RUN_SECURITY_TEST(test_broker_validate_revoke_race_guard);
    LLAM_RUN_SECURITY_TEST(test_broker_listen_unix_preserves_existing_file);
    LLAM_RUN_SECURITY_TEST(test_broker_listen_unix_owner_only_mode);
    LLAM_RUN_SECURITY_TEST(test_broker_restrict_owned_socket_rejects_symlink);
    LLAM_RUN_SECURITY_TEST(test_broker_restrict_owned_socket_requires_identity);
    LLAM_RUN_SECURITY_TEST(test_broker_capture_owned_socket_clears_identity_on_failure);
    LLAM_RUN_SECURITY_TEST(test_broker_unlink_owned_socket_uses_path_identity);
    LLAM_RUN_SECURITY_TEST(test_broker_posix_transport_fds_are_cloexec);
    LLAM_RUN_SECURITY_TEST(test_broker_create_response_failure_rolls_back_memory_grants);
    LLAM_RUN_SECURITY_TEST(test_broker_register_descriptor_response_failure_rolls_back);
    LLAM_RUN_SECURITY_TEST(test_broker_task_spawn_response_failure_rolls_back_race);
    LLAM_RUN_SECURITY_TEST(test_broker_failed_wire_reads_clear_outputs);
    LLAM_RUN_SECURITY_TEST(test_broker_request_helpers_clear_response_on_failure);
    LLAM_RUN_SECURITY_TEST(test_broker_endpoint_helpers_clear_outputs_on_failure);
    LLAM_RUN_SECURITY_TEST(test_broker_direct_failed_outputs_are_cleared);
    LLAM_RUN_SECURITY_TEST(test_broker_partial_descriptor_wire_reads_close_received_fd);
    LLAM_RUN_SECURITY_TEST(test_broker_invalid_register_descriptor_closes_received_fd);
    LLAM_RUN_SECURITY_TEST(test_broker_overauthorized_descriptor_array_closes_all_received_fds);
    LLAM_RUN_SECURITY_TEST(test_broker_unclaimed_descriptor_is_rejected_and_closed);
    LLAM_RUN_SECURITY_TEST(test_broker_serve_one_fd_keeps_session_subject);
    LLAM_RUN_SECURITY_TEST(test_broker_transport_rejects_cross_session_replay);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_fd_data_plane);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_fd_mapping_authority);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_multiprocess_flood);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_multiprocess_session_replay_guard);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_multiprocess_teardown_guard);
    LLAM_RUN_SECURITY_TEST(test_broker_nested_op_survives_destroy_start);
    LLAM_RUN_SECURITY_TEST(test_broker_nested_dispatch_survives_destroy_start);
    LLAM_RUN_SECURITY_TEST(test_broker_nested_ring_create_survives_destroy_start);
    LLAM_RUN_SECURITY_TEST(test_broker_destroy_is_single_owner_under_race);
    LLAM_RUN_SECURITY_TEST(test_broker_destroy_waits_for_active_ring_io);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_publish_cursor_mismatch_fails_closed);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_session_forget_rejects_busy_serve);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_named_session_cleanup_unlinks_mapping);
    LLAM_RUN_SECURITY_TEST(test_broker_destroy_reclaims_inactive_owned_ring_mapping);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_reuse_reclaims_inactive_owned_mapping);
    LLAM_RUN_SECURITY_TEST(test_broker_buffer_reuse_reclaims_inactive_storage);
    LLAM_RUN_SECURITY_TEST(test_broker_destroy_reclaims_inactive_owned_descriptor);
    LLAM_RUN_SECURITY_TEST(test_broker_descriptor_reuse_reclaims_inactive_owned_slot);
    LLAM_RUN_SECURITY_TEST(test_broker_socketpair_transport);
    LLAM_RUN_SECURITY_TEST(test_broker_create_ring_response_failure_reclaims_session);
    LLAM_RUN_SECURITY_TEST(test_broker_serve_local_n_survives_malformed_session);
#else
    LLAM_RUN_SECURITY_TEST(test_broker_ring_handle_data_plane);
    LLAM_RUN_SECURITY_TEST(test_broker_register_handle_clears_inherit_flag);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_windows_mapping_flood);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_windows_cross_process_flood);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_windows_cross_process_session_replay_guard);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_windows_cross_process_teardown_guard);
    LLAM_RUN_SECURITY_TEST(test_broker_pipe_transport_handle_grants);
    LLAM_RUN_SECURITY_TEST(test_broker_pipe_create_ring_write_failure_closes_remote_handle);
    LLAM_RUN_SECURITY_TEST(test_broker_pipe_serve_local_n_survives_malformed_session);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_mapping_has_restricted_dacl);
    LLAM_RUN_SECURITY_TEST(test_broker_ring_handle_mapping_authority);
#endif
    puts("test_security_capability ok");
    return 0;
}
