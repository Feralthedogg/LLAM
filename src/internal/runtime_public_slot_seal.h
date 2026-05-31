/**
 * @file src/internal/runtime_public_slot_seal.h
 * @brief Sealed generation-token helpers for public slot handles.
 *
 * @details
 * This header is included only by runtime_public_slot.h after the slot table
 * types are declared. It keeps entropy, affine sealing, and generation-token
 * derivation separate from the table reserve/resolve/release operations.
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

static inline bool llam_public_slot_affine_multiplier_valid(uint32_t multiplier) {
    /*
     * The public token domain is 2^28 - 1. These are its prime factors:
     * 3 * 5 * 29 * 43 * 113 * 127. A multiplier coprime with that modulus
     * makes (epoch * multiplier + addend) a permutation over all nonzero
     * epoch tokens.
     */
    return multiplier != 0U &&
           (multiplier % 3U) != 0U &&
           (multiplier % 5U) != 0U &&
           (multiplier % 29U) != 0U &&
           (multiplier % 43U) != 0U &&
           (multiplier % 113U) != 0U &&
           (multiplier % 127U) != 0U;
}

static inline uint32_t llam_public_slot_mersenne28_reduce(uint64_t value) {
    uint64_t reduced;

    /*
     * LLAM_PUBLIC_HANDLE_EPOCH_MASK is 2^28 - 1. Use the Mersenne folding
     * identity instead of a hardware division so seal setup and test oracles
     * stay cheap. Hot-path reactivation advances the token by addition below.
     */
    reduced = (value & (uint64_t)LLAM_PUBLIC_HANDLE_EPOCH_MASK) + (value >> 28U);
    reduced = (reduced & (uint64_t)LLAM_PUBLIC_HANDLE_EPOCH_MASK) + (reduced >> 28U);
    if (reduced >= (uint64_t)LLAM_PUBLIC_HANDLE_EPOCH_MASK) {
        reduced -= (uint64_t)LLAM_PUBLIC_HANDLE_EPOCH_MASK;
    }
    return (uint32_t)reduced;
}

static inline uint32_t llam_public_slot_choose_affine_multiplier(uint64_t seed) {
    uint32_t multiplier = llam_public_slot_mersenne28_reduce(seed);

    if (multiplier == 0U) {
        multiplier = 1U;
    }
    while (!llam_public_slot_affine_multiplier_valid(multiplier)) {
        multiplier += 1U;
        if (multiplier == 0U || multiplier > LLAM_PUBLIC_HANDLE_EPOCH_MASK) {
            multiplier = 1U;
        }
    }
    return multiplier;
}

static inline void llam_public_slot_prepare_affine_seal(llam_public_slot_table_t *table,
                                                        llam_public_slot_t *entry,
                                                        size_t slot,
                                                        const void *object,
                                                        uint64_t owner_secret) {
    uint64_t key;
    uint64_t seed;

    if (entry == NULL || entry->seal_multiplier != 0U) {
        return;
    }
    key = llam_public_slot_table_secret(table, object, owner_secret);
    seed = key ^ llam_public_slot_mix64(((uint64_t)(slot + 1U) << 32U) ^
                                        (uint64_t)(uintptr_t)table);
    entry->seal_multiplier = llam_public_slot_choose_affine_multiplier(seed);
    entry->seal_addend =
        (uint32_t)(llam_public_slot_mix64(seed ^ UINT64_C(0xa0761d6478bd642f)) %
                   (uint64_t)LLAM_PUBLIC_HANDLE_EPOCH_MASK);
}

static inline uint32_t llam_public_slot_affine_epoch_token(uint32_t epoch,
                                                           uint32_t multiplier,
                                                           uint32_t addend) {
    uint64_t index;

    if (epoch == 0U) {
        epoch = 1U;
    }
    index = (uint64_t)(epoch - 1U);
    return llam_public_slot_mersenne28_reduce(index * (uint64_t)multiplier + (uint64_t)addend) + 1U;
}

static inline uint32_t llam_public_slot_next_affine_token(uint32_t previous_generation,
                                                          uint32_t family,
                                                          uint32_t seal_multiplier,
                                                          uint32_t seal_addend) {
    uint32_t token;
    uint32_t zero_based;

    if (previous_generation != 0U &&
        (previous_generation & LLAM_PUBLIC_HANDLE_FAMILY_MASK) == family) {
        token = previous_generation >> LLAM_PUBLIC_HANDLE_FAMILY_BITS;
        zero_based = token != 0U ? token - 1U : 0U;
        zero_based += seal_multiplier;
        if (zero_based >= LLAM_PUBLIC_HANDLE_EPOCH_MASK) {
            zero_based -= LLAM_PUBLIC_HANDLE_EPOCH_MASK;
        }
        return zero_based + 1U;
    }
    return seal_addend + 1U;
}

static inline uint32_t llam_public_slot_next_affine_generation(uint32_t previous_generation,
                                                               uint32_t family,
                                                               uint32_t seal_multiplier,
                                                               uint32_t seal_addend) {
    return llam_public_slot_family_generation(
        llam_public_slot_next_affine_token(previous_generation, family, seal_multiplier, seal_addend),
        family);
}

static inline uint32_t llam_public_slot_family_generation_for_epoch(llam_public_slot_table_t *table,
                                                                    size_t slot,
                                                                    const void *object,
                                                                    uint32_t family,
                                                                    uint64_t owner_secret,
                                                                    uint32_t epoch,
                                                                    uint32_t seal_multiplier,
                                                                    uint32_t seal_addend) {
    (void)table;
    (void)slot;
    (void)object;
    (void)owner_secret;
    return llam_public_slot_family_generation(
        llam_public_slot_affine_epoch_token(epoch, seal_multiplier, seal_addend),
        family);
}

static inline uint32_t llam_public_slot_initial_generation_for_slot(llam_public_slot_table_t *table,
                                                                    size_t slot,
                                                                    const void *object,
                                                                    uint32_t family,
                                                                    uint64_t owner_secret,
                                                                    uint32_t epoch,
                                                                    uint32_t seal_multiplier,
                                                                    uint32_t seal_addend) {
    if (family == 0U) {
        return 1U;
    }
    return llam_public_slot_family_generation_for_epoch(table,
                                                        slot,
                                                        object,
                                                        family,
                                                        owner_secret,
                                                        epoch,
                                                        seal_multiplier,
                                                        seal_addend);
}

#endif
