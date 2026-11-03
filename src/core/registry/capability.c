/**
 * @file src/core/registry/capability.c
 * @brief Broker capability token issue and validation helpers.
 *
 * @details
 * These helpers intentionally do not protect the current process from itself.
 * They become a real capability boundary only when the key remains inside a
 * broker process and untrusted code receives only serialized tokens.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSES/Apache-2.0.txt.
 */

#include "runtime_internal.h"
#include "runtime_capability.h"

#include <string.h>

/*
 * SipHash-2-4 is derived from the authors' CC0 reference algorithm. The
 * fixed-word feeder below is LLAM-specific and preserves capability token v1.
 * See THIRD_PARTY_NOTICES.md for provenance.
 */
typedef struct llam_cap_siphash_state {
    uint64_t v0;
    uint64_t v1;
    uint64_t v2;
    uint64_t v3;
} llam_cap_siphash_state_t;

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_bool g_llam_capability_force_entropy_failure;

static bool llam_cap_entropy_forced_failure(void) {
    return atomic_load_explicit(&g_llam_capability_force_entropy_failure, memory_order_relaxed);
}

void llam_capability_test_force_entropy_failure(bool enabled) {
    atomic_store_explicit(&g_llam_capability_force_entropy_failure, enabled, memory_order_relaxed);
}
#else
static bool llam_cap_entropy_forced_failure(void) {
    return false;
}
#endif

static uint64_t llam_cap_read64_le(const uint8_t *bytes) {
    return ((uint64_t)bytes[0]) |
           ((uint64_t)bytes[1] << 8U) |
           ((uint64_t)bytes[2] << 16U) |
           ((uint64_t)bytes[3] << 24U) |
           ((uint64_t)bytes[4] << 32U) |
           ((uint64_t)bytes[5] << 40U) |
           ((uint64_t)bytes[6] << 48U) |
           ((uint64_t)bytes[7] << 56U);
}

static void llam_cap_write64_le(uint8_t *bytes, uint64_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
    bytes[4] = (uint8_t)(value >> 32U);
    bytes[5] = (uint8_t)(value >> 40U);
    bytes[6] = (uint8_t)(value >> 48U);
    bytes[7] = (uint8_t)(value >> 56U);
}

static void llam_cap_wipe_bytes(void *data, size_t len) {
    volatile uint8_t *bytes = (volatile uint8_t *)data;

    while (len > 0U) {
        *bytes = 0U;
        ++bytes;
        --len;
    }
}

static void llam_cap_wipe_u64(uint64_t *value) {
    volatile uint64_t *slot = (volatile uint64_t *)value;

    *slot = 0U;
}

static uint64_t llam_cap_rotl64(uint64_t value, unsigned bits) {
    return (value << bits) | (value >> (64U - bits));
}

static void llam_cap_siphash_init(llam_cap_siphash_state_t *state,
                                  uint64_t k0,
                                  uint64_t k1) {
    state->v0 = UINT64_C(0x736f6d6570736575) ^ k0;
    state->v1 = UINT64_C(0x646f72616e646f6d) ^ k1;
    state->v2 = UINT64_C(0x6c7967656e657261) ^ k0;
    state->v3 = UINT64_C(0x7465646279746573) ^ k1;
}

static void llam_cap_sipround(llam_cap_siphash_state_t *state) {
    state->v0 += state->v1;
    state->v1 = llam_cap_rotl64(state->v1, 13U);
    state->v1 ^= state->v0;
    state->v0 = llam_cap_rotl64(state->v0, 32U);
    state->v2 += state->v3;
    state->v3 = llam_cap_rotl64(state->v3, 16U);
    state->v3 ^= state->v2;
    state->v0 += state->v3;
    state->v3 = llam_cap_rotl64(state->v3, 21U);
    state->v3 ^= state->v0;
    state->v2 += state->v1;
    state->v1 = llam_cap_rotl64(state->v1, 17U);
    state->v1 ^= state->v2;
    state->v2 = llam_cap_rotl64(state->v2, 32U);
}

static void llam_cap_siphash_compress(llam_cap_siphash_state_t *state, uint64_t word) {
    state->v3 ^= word;
    llam_cap_sipround(state);
    llam_cap_sipround(state);
    state->v0 ^= word;
}

static void llam_cap_siphash_compress_pair(llam_cap_siphash_state_t *first,
                                           llam_cap_siphash_state_t *second,
                                           uint64_t word) {
    llam_cap_siphash_compress(first, word);
    llam_cap_siphash_compress(second, word);
}

static uint64_t llam_cap_siphash_finish(llam_cap_siphash_state_t *state) {
    unsigned round;

    /* Capability token v1 has exactly nine 64-bit canonical words. */
    llam_cap_siphash_compress(state, UINT64_C(72) << 56U);
    state->v2 ^= UINT64_C(0xff);
    for (round = 0U; round < 4U; ++round) {
        llam_cap_sipround(state);
    }
    return state->v0 ^ state->v1 ^ state->v2 ^ state->v3;
}

static void llam_cap_mac(const llam_capability_key_t *key,
                         const llam_capability_token_t *token,
                         uint8_t out_mac[LLAM_CAPABILITY_MAC_BYTES]) {
    llam_cap_siphash_state_t first;
    llam_cap_siphash_state_t second;
    uint64_t h0;
    uint64_t h1;

    llam_cap_siphash_init(&first, key->words[0], key->words[1]);
    llam_cap_siphash_init(&second, key->words[2], key->words[3]);
    llam_cap_siphash_compress_pair(&first,
                                   &second,
                                   ((uint64_t)token->family << 32U) | token->version);
    llam_cap_siphash_compress_pair(&first, &second, token->runtime_id);
    llam_cap_siphash_compress_pair(&first, &second, token->slot);
    llam_cap_siphash_compress_pair(&first, &second, token->generation);
    llam_cap_siphash_compress_pair(&first, &second, token->rights);
    llam_cap_siphash_compress_pair(&first, &second, token->revocation_epoch);
    llam_cap_siphash_compress_pair(&first, &second, token->subject_id);
    llam_cap_siphash_compress_pair(&first, &second, llam_cap_read64_le(token->nonce));
    llam_cap_siphash_compress_pair(&first,
                                   &second,
                                   llam_cap_read64_le(token->nonce + 8U));
    h0 = llam_cap_siphash_finish(&first);
    h1 = llam_cap_siphash_finish(&second);
    llam_cap_write64_le(out_mac, h0);
    llam_cap_write64_le(out_mac + 8U, h1);
    llam_cap_wipe_bytes(&first, sizeof(first));
    llam_cap_wipe_bytes(&second, sizeof(second));
    llam_cap_wipe_u64(&h0);
    llam_cap_wipe_u64(&h1);
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
void llam_capability_test_compute_mac(const llam_capability_key_t *key,
                                      const llam_capability_token_t *token,
                                      uint8_t out_mac[LLAM_CAPABILITY_MAC_BYTES]) {
    llam_cap_mac(key, token, out_mac);
}
#endif

static bool llam_cap_mac_equal(const uint8_t a[LLAM_CAPABILITY_MAC_BYTES],
                               const uint8_t b[LLAM_CAPABILITY_MAC_BYTES]) {
    uint8_t diff = 0U;
    size_t i;

    for (i = 0U; i < LLAM_CAPABILITY_MAC_BYTES; ++i) {
        diff = (uint8_t)(diff | (uint8_t)(a[i] ^ b[i]));
    }
    return diff == 0U;
}

static bool llam_cap_entropy_word(uint64_t *out_word);

static int llam_cap_fill_nonce(llam_capability_token_t *token, const llam_capability_key_t *key) {
    uint64_t n0;
    uint64_t n1;

    if (!llam_cap_entropy_word(&n0) || !llam_cap_entropy_word(&n1)) {
        /*
         * Nonces are MAC-covered token material. A deterministic nonce would
         * not expose the broker key by itself, but it weakens the broker
         * process-boundary story by making freshly issued serialized authority
         * repeatable under entropy failure. Fail closed and let the caller
         * clear any partially initialized token.
         */
        errno = EIO;
        return -1;
    }
    n0 ^= llam_public_slot_fallback_secret(token, key, key->words[0] ^ token->runtime_id);
    n1 ^= llam_public_slot_fallback_secret(key, token, key->words[1] ^ token->slot);
    n0 ^= llam_public_slot_mix64(key->words[2] ^ token->generation);
    n1 ^= llam_public_slot_mix64(key->words[3] ^ token->rights);
    llam_cap_write64_le(token->nonce, llam_public_slot_mix64(n0));
    llam_cap_write64_le(token->nonce + 8U, llam_public_slot_mix64(n1));
    llam_cap_wipe_u64(&n0);
    llam_cap_wipe_u64(&n1);
    return 0;
}

static bool llam_cap_key_valid(const llam_capability_key_t *key) {
    return key != NULL && (key->words[0] | key->words[1] | key->words[2] | key->words[3]) != 0U;
}

static bool llam_cap_entropy_word(uint64_t *out_word) {
    if (LLAM_UNLIKELY(out_word == NULL)) {
        return false;
    }
    if (LLAM_UNLIKELY(llam_cap_entropy_forced_failure())) {
        return false;
    }
    return llam_public_slot_entropy_from_os(out_word) && *out_word != 0U;
}

int llam_capability_key_init(llam_capability_key_t *key, const void *scope, uint64_t seed) {
    uint64_t word;
    size_t i;

    if (LLAM_UNLIKELY(key == NULL)) {
        errno = EINVAL;
        return -1;
    }
    memset(key, 0, sizeof(*key));
    for (i = 0U; i < 4U; ++i) {
        if (!llam_cap_entropy_word(&word)) {
            /*
             * Broker capability keys are process-isolation authority, unlike
             * in-process public handle obfuscation. If secure OS entropy is not
             * available, fail closed instead of minting forgeable deterministic
             * MAC keys from addresses, clocks, or pids.
             */
            llam_capability_key_clear(key);
            errno = EIO;
            return -1;
        }
        word ^= llam_public_slot_fallback_secret(key, scope, seed ^ ((uint64_t)i << 32U));
        key->words[i] = llam_public_slot_mix64(word ^ (UINT64_C(0x9e3779b97f4a7c15) * (uint64_t)(i + 1U)));
    }
    if (!llam_cap_key_valid(key)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

void llam_capability_key_clear(llam_capability_key_t *key) {
    volatile uint64_t *words;
    size_t i;

    if (key == NULL) {
        return;
    }
    words = key->words;
    for (i = 0U; i < 4U; ++i) {
        words[i] = 0U;
    }
}

bool llam_capability_has_rights(const llam_capability_token_t *token, uint64_t required_rights) {
    return token != NULL && (token->rights & required_rights) == required_rights;
}

int llam_capability_issue(const llam_capability_key_t *key,
                          const llam_capability_object_t *object,
                          uint64_t rights,
                          llam_capability_token_t *out_token) {
    if (out_token != NULL) {
        memset(out_token, 0, sizeof(*out_token));
    }
    if (LLAM_UNLIKELY(!llam_cap_key_valid(key) ||
                      object == NULL ||
                      out_token == NULL ||
                      object->runtime_id == 0U ||
                      object->family == 0U ||
                      object->slot == 0U ||
                      object->slot == UINT64_MAX ||
                      object->generation == 0U ||
                      object->revocation_epoch == 0U ||
                      rights == 0U)) {
        errno = EINVAL;
        return -1;
    }
    out_token->version = LLAM_CAPABILITY_VERSION;
    out_token->family = object->family;
    out_token->runtime_id = object->runtime_id;
    out_token->slot = object->slot;
    out_token->generation = object->generation;
    out_token->rights = rights;
    out_token->revocation_epoch = object->revocation_epoch;
    out_token->subject_id = object->subject_id;
    if (llam_cap_fill_nonce(out_token, key) != 0) {
        memset(out_token, 0, sizeof(*out_token));
        return -1;
    }
    llam_cap_mac(key, out_token, out_token->mac);
    return 0;
}

int llam_capability_validate(const llam_capability_key_t *key,
                             const llam_capability_token_t *token,
                             uint64_t required_rights,
                             uint64_t current_revocation_epoch) {
    return llam_capability_validate_subject(key, token, required_rights, current_revocation_epoch, 0U);
}

int llam_capability_validate_subject(const llam_capability_key_t *key,
                                     const llam_capability_token_t *token,
                                     uint64_t required_rights,
                                     uint64_t current_revocation_epoch,
                                     uint64_t expected_subject_id) {
    uint8_t expected[LLAM_CAPABILITY_MAC_BYTES];

    if (LLAM_UNLIKELY(!llam_cap_key_valid(key) || token == NULL || required_rights == 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(token->version != LLAM_CAPABILITY_VERSION ||
                      token->family == 0U ||
                      token->runtime_id == 0U ||
                      token->slot == 0U ||
                      token->slot == UINT64_MAX ||
                      token->generation == 0U ||
                      token->revocation_epoch == 0U ||
                      token->rights == 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(token->revocation_epoch != current_revocation_epoch ||
                      !llam_capability_has_rights(token, required_rights))) {
        errno = EACCES;
        return -1;
    }
    if (LLAM_UNLIKELY(token->subject_id != expected_subject_id)) {
        errno = EACCES;
        return -1;
    }
    llam_cap_mac(key, token, expected);
    if (LLAM_UNLIKELY(!llam_cap_mac_equal(expected, token->mac))) {
        llam_cap_wipe_bytes(expected, sizeof(expected));
        errno = EACCES;
        return -1;
    }
    llam_cap_wipe_bytes(expected, sizeof(expected));
    return 0;
}

int llam_capability_attenuate(const llam_capability_key_t *key,
                              const llam_capability_token_t *token,
                              uint64_t subset_rights,
                              uint64_t current_revocation_epoch,
                              llam_capability_token_t *out_token) {
    llam_capability_object_t object;
    llam_capability_token_t source;
    llam_capability_token_t issued;
    int rc;

    if (LLAM_UNLIKELY(out_token == NULL)) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Support in-place attenuation without preserving broad authority on
     * failure. Copy the source token before clearing the output so callers that
     * pass token == out_token still get fail-closed semantics.
     */
    memset(&source, 0, sizeof(source));
    if (token != NULL) {
        source = *token;
    }
    memset(out_token, 0, sizeof(*out_token));
    if (LLAM_UNLIKELY(subset_rights == 0U)) {
        errno = EINVAL;
        memset(&source, 0, sizeof(source));
        return -1;
    }
    if (llam_capability_validate_subject(key,
                                         token != NULL ? &source : NULL,
                                         subset_rights,
                                         current_revocation_epoch,
                                         token != NULL ? source.subject_id : 0U) != 0) {
        memset(&source, 0, sizeof(source));
        return -1;
    }
    if (LLAM_UNLIKELY((source.rights & subset_rights) != subset_rights)) {
        errno = EACCES;
        memset(&source, 0, sizeof(source));
        return -1;
    }
    object.runtime_id = source.runtime_id;
    object.family = source.family;
    object.reserved0 = 0U;
    object.slot = source.slot;
    object.generation = source.generation;
    object.revocation_epoch = source.revocation_epoch;
    object.subject_id = source.subject_id;
    memset(&issued, 0, sizeof(issued));
    rc = llam_capability_issue(key, &object, subset_rights, &issued);
    if (rc == 0) {
        *out_token = issued;
    }
    memset(&issued, 0, sizeof(issued));
    memset(&source, 0, sizeof(source));
    return rc;
}
