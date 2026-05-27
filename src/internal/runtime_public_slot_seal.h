/**
 * @file src/internal/runtime_public_slot_seal.h
 * @brief Sealed generation-token helpers for public slot handles.
 *
 * @details
 * This header is included only by runtime_public_slot.h after the slot table
 * types are declared. It keeps entropy, nonce, and sealed-token derivation
 * separate from the table reserve/resolve/release operations.
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

#ifndef LLAM_RUNTIME_PUBLIC_SLOT_SEAL_H
#define LLAM_RUNTIME_PUBLIC_SLOT_SEAL_H

#ifndef LLAM_RUNTIME_PUBLIC_SLOT_H
#error "include runtime_public_slot.h instead"
#endif

#if LLAM_PLATFORM_WINDOWS
#include <ntsecapi.h>
#endif

static inline uint64_t llam_public_slot_mix64(uint64_t value) {
    uint64_t x = value;

    x ^= x >> 33U;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33U;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33U;
    return x;
}

static inline bool llam_public_slot_entropy_from_os(uint64_t *out_secret) {
    if (out_secret == NULL) {
        return false;
    }
#if LLAM_PLATFORM_DARWIN || LLAM_PLATFORM_FREEBSD || LLAM_PLATFORM_OPENBSD || LLAM_PLATFORM_NETBSD || LLAM_PLATFORM_DRAGONFLY
    arc4random_buf(out_secret, sizeof(*out_secret));
    return *out_secret != 0U;
#elif defined(__linux__)
#if defined(SYS_getrandom)
    {
        unsigned char *cursor = (unsigned char *)out_secret;
        size_t remaining = sizeof(*out_secret);

        while (remaining > 0U) {
            ssize_t nread = syscall(SYS_getrandom, cursor, remaining, 0U);

            if (nread > 0) {
                cursor += (size_t)nread;
                remaining -= (size_t)nread;
                continue;
            }
            if (nread < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        if (remaining == 0U && *out_secret != 0U) {
            return true;
        }
    }
#endif
#elif LLAM_PLATFORM_WINDOWS
    if (RtlGenRandom(out_secret, (ULONG)sizeof(*out_secret)) && *out_secret != 0U) {
        return true;
    }
#endif
    return false;
}

static inline uint64_t llam_public_slot_fallback_secret(const void *scope,
                                                        const void *object,
                                                        uint64_t owner_secret) {
    static atomic_uint_fast64_t fallback_counter = UINT64_C(0x9e3779b97f4a7c15);
    struct timespec ts;
    uint64_t entropy;

    entropy = owner_secret;
    entropy ^= (uint64_t)(uintptr_t)scope;
    entropy ^= llam_public_slot_mix64((uint64_t)(uintptr_t)object);
    entropy ^= (uint64_t)(uintptr_t)&entropy << 17U;
    entropy ^= atomic_fetch_add_explicit(&fallback_counter,
                                         UINT64_C(0x9e3779b97f4a7c15),
                                         memory_order_relaxed);
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        entropy ^= ((uint64_t)(uint32_t)ts.tv_sec << 32U) ^ (uint64_t)(uint32_t)ts.tv_nsec;
    }
#if LLAM_PLATFORM_WINDOWS
    {
        LARGE_INTEGER counter;

        if (QueryPerformanceCounter(&counter)) {
            entropy ^= (uint64_t)counter.QuadPart;
        }
        entropy ^= (uint64_t)GetCurrentProcessId() << 32U;
        entropy ^= (uint64_t)GetCurrentThreadId();
    }
#else
    entropy ^= (uint64_t)(uint32_t)getpid() << 32U;
#endif
    entropy = llam_public_slot_mix64(entropy);
    return entropy != 0U ? entropy : UINT64_C(0xd6e8feb86659fd93);
}

static inline uint64_t llam_public_slot_table_secret(llam_public_slot_table_t *table,
                                                     const void *object,
                                                     uint64_t owner_secret) {
    uint64_t secret;

    secret = table->handle_secret;
    if (secret != 0U) {
        return secret;
    }

    /*
     * This key hardens opaque handles against trivial same-process guessing; it
     * is deliberately not documented as a cryptographic capability secret.
     * Keep it table-stable after first initialization so each slot can validate
     * its sealed token without exposing the raw internal epoch.
     */
    if (!llam_public_slot_entropy_from_os(&secret)) {
        secret = llam_public_slot_fallback_secret(table, object, owner_secret);
    }
    secret ^= llam_public_slot_fallback_secret(table, object, owner_secret);
    secret = llam_public_slot_mix64(secret);
    if (secret == 0U) {
        secret = UINT64_C(0xa0761d6478bd642f);
    }
    table->handle_secret = secret;
    return secret;
}

static inline uint64_t llam_public_slot_next_nonce(llam_public_slot_table_t *table,
                                                   const void *object,
                                                   uint64_t owner_secret,
                                                   size_t slot,
                                                   uint32_t family,
                                                   uint32_t epoch) {
    uint64_t key;
    uint64_t nonce;

    if (epoch == 0U) {
        epoch = 1U;
    }
    key = llam_public_slot_table_secret(table, object, owner_secret);
    nonce = key ^ llam_public_slot_mix64((uint64_t)(uintptr_t)object);
    nonce ^= ((uint64_t)(slot + 1U) << 32U) ^ ((uint64_t)epoch << 4U) ^ (uint64_t)family;
    nonce ^= llam_public_slot_mix64(owner_secret ^ UINT64_C(0x6a09e667f3bcc909));
    nonce = llam_public_slot_mix64(nonce);
    return nonce != 0U ? nonce : UINT64_C(0xbf58476d1ce4e5b9);
}

static inline uint32_t llam_public_slot_sealed_epoch_token(llam_public_slot_table_t *table,
                                                           size_t slot,
                                                           const void *object,
                                                           uint32_t family,
                                                           uint64_t owner_secret,
                                                           uint32_t epoch,
                                                           uint64_t slot_nonce) {
    uint64_t key;
    uint64_t mac;
    uint32_t token;

    key = llam_public_slot_table_secret(table, object, owner_secret);
    mac = key ^ llam_public_slot_mix64(slot_nonce);
    mac ^= llam_public_slot_mix64(((uint64_t)(slot + 1U) << 32U) ^ (uint64_t)family);
    mac ^= llam_public_slot_mix64(((uint64_t)epoch << 32U) ^ (owner_secret & UINT64_C(0xffffffff)));
    mac = llam_public_slot_mix64(mac);
    token = (uint32_t)(mac & (uint64_t)LLAM_PUBLIC_HANDLE_EPOCH_MASK);
    if (token == 0U) {
        token = 1U;
    }
    return token;
}

static inline uint32_t llam_public_slot_family_generation_for_epoch(llam_public_slot_table_t *table,
                                                                    size_t slot,
                                                                    const void *object,
                                                                    uint32_t family,
                                                                    uint64_t owner_secret,
                                                                    uint32_t epoch,
                                                                    uint64_t slot_nonce) {
    return llam_public_slot_family_generation(
        llam_public_slot_sealed_epoch_token(table, slot, object, family, owner_secret, epoch, slot_nonce),
        family);
}

static inline uint32_t llam_public_slot_initial_generation_for_slot(llam_public_slot_table_t *table,
                                                                    size_t slot,
                                                                    const void *object,
                                                                    uint32_t family,
                                                                    uint64_t owner_secret,
                                                                    uint32_t epoch,
                                                                    uint64_t slot_nonce) {
    if (family == 0U) {
        return 1U;
    }
    return llam_public_slot_family_generation_for_epoch(table,
                                                        slot,
                                                        object,
                                                        family,
                                                        owner_secret,
                                                        epoch,
                                                        slot_nonce);
}

#endif
