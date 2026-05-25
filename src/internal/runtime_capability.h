/**
 * @file src/internal/runtime_capability.h
 * @brief Broker-mode capability tokens for address-space isolation.
 *
 * @details
 * Public slot handles are an in-process misuse hardening layer. They cannot be
 * a security boundary against arbitrary same-process memory read/write because
 * a compromised caller can read or patch runtime memory. Broker capability
 * tokens are different: they are designed to be issued and validated in a
 * separate broker process that keeps the MAC key outside the untrusted address
 * space. The structs here are internal until the broker transport is stabilized.
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

#ifndef LLAM_RUNTIME_CAPABILITY_H
#define LLAM_RUNTIME_CAPABILITY_H

#include "llam_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LLAM_CAPABILITY_VERSION 1U
#define LLAM_CAPABILITY_NONCE_BYTES 16U
#define LLAM_CAPABILITY_MAC_BYTES 16U

#define LLAM_CAP_RIGHT_SEND (UINT64_C(1) << 0)
#define LLAM_CAP_RIGHT_RECV (UINT64_C(1) << 1)
#define LLAM_CAP_RIGHT_JOIN (UINT64_C(1) << 2)
#define LLAM_CAP_RIGHT_DETACH (UINT64_C(1) << 3)
#define LLAM_CAP_RIGHT_CLOSE (UINT64_C(1) << 4)
#define LLAM_CAP_RIGHT_DESTROY (UINT64_C(1) << 5)
#define LLAM_CAP_RIGHT_READ (UINT64_C(1) << 6)
#define LLAM_CAP_RIGHT_WRITE (UINT64_C(1) << 7)
#define LLAM_CAP_RIGHT_ADMIN (UINT64_C(1) << 63)

typedef struct llam_capability_key {
    uint64_t words[4];
} llam_capability_key_t;

typedef struct llam_capability_object {
    uint64_t runtime_id;
    uint32_t family;
    uint32_t reserved0;
    uint64_t slot;
    uint64_t generation;
    uint64_t revocation_epoch;
    uint64_t subject_id;
} llam_capability_object_t;

typedef struct llam_capability_token {
    uint32_t version;
    uint32_t family;
    uint64_t runtime_id;
    uint64_t slot;
    uint64_t generation;
    uint64_t rights;
    uint64_t revocation_epoch;
    uint64_t subject_id;
    uint8_t nonce[LLAM_CAPABILITY_NONCE_BYTES];
    uint8_t mac[LLAM_CAPABILITY_MAC_BYTES];
} llam_capability_token_t;

int llam_capability_key_init(llam_capability_key_t *key, const void *scope, uint64_t seed);
void llam_capability_key_clear(llam_capability_key_t *key);
void llam_capability_test_force_entropy_failure(bool enabled);

int llam_capability_issue(const llam_capability_key_t *key,
                          const llam_capability_object_t *object,
                          uint64_t rights,
                          llam_capability_token_t *out_token);

int llam_capability_validate(const llam_capability_key_t *key,
                             const llam_capability_token_t *token,
                             uint64_t required_rights,
                             uint64_t current_revocation_epoch);

int llam_capability_validate_subject(const llam_capability_key_t *key,
                                     const llam_capability_token_t *token,
                                     uint64_t required_rights,
                                     uint64_t current_revocation_epoch,
                                     uint64_t expected_subject_id);

int llam_capability_attenuate(const llam_capability_key_t *key,
                              const llam_capability_token_t *token,
                              uint64_t subset_rights,
                              uint64_t current_revocation_epoch,
                              llam_capability_token_t *out_token);

bool llam_capability_has_rights(const llam_capability_token_t *token, uint64_t required_rights);

#endif
