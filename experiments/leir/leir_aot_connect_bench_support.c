// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "leir_aot_connect_bench_support.h"

#if defined(__linux__)

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/un.h>
#include <unistd.h>

const char *candidate_name(bench_candidate_t candidate) {
    return candidate == BENCH_CANDIDATE_NATIVE ? "native" : "portable";
}

const char *family_name(bench_family_t family) {
    return family == BENCH_FAMILY_UNIX ? "unix" : "tcp";
}

static int parse_positive_u64(const char *text, uint64_t *value_out) {
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        errno = EINVAL;
        return -1;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0U) {
        errno = EINVAL;
        return -1;
    }
    *value_out = (uint64_t)value;
    return 0;
}

static const char *canonical_ring_profile(const char *value) {
    if (strcmp(value, "submit_all") == 0) {
        return "submit_all";
    }
    if (strcmp(value, "coop_taskrun") == 0) {
        return "coop_taskrun";
    }
    if (strcmp(value, "defer_taskrun") == 0) {
        return "defer_taskrun";
    }
    return NULL;
}

int parse_options(
    int argc,
    char **argv,
    bench_options_t *options) {
    int i;

    memset(options, 0, sizeof(*options));
    options->family = BENCH_FAMILY_TCP;
    options->concurrency = 4U;
    options->payload = 64U;
    options->activations = 100U;
    options->ring_profile = "submit_all";
    for (i = 1; i < argc; i += 1) {
        const char *name = argv[i];
        const char *value;
        uint64_t parsed;

        if (i + 1 >= argc) {
            errno = EINVAL;
            return -1;
        }
        value = argv[++i];
        if (strcmp(name, "--candidate") == 0) {
            if (strcmp(value, "portable") == 0) {
                options->candidate = BENCH_CANDIDATE_PORTABLE;
            } else if (strcmp(value, "native") == 0) {
                options->candidate = BENCH_CANDIDATE_NATIVE;
            } else {
                errno = EINVAL;
                return -1;
            }
            options->candidate_set = true;
        } else if (strcmp(name, "--family") == 0) {
            if (strcmp(value, "tcp") == 0) {
                options->family = BENCH_FAMILY_TCP;
            } else if (strcmp(value, "unix") == 0) {
                options->family = BENCH_FAMILY_UNIX;
            } else {
                errno = EINVAL;
                return -1;
            }
        } else if (strcmp(name, "--ring-profile") == 0) {
            options->ring_profile = canonical_ring_profile(value);
            if (options->ring_profile == NULL) {
                errno = EINVAL;
                return -1;
            }
        } else if (strcmp(name, "--concurrency") == 0) {
            if (parse_positive_u64(value, &parsed) != 0 ||
                parsed > BENCH_MAX_CONCURRENCY) {
                errno = EINVAL;
                return -1;
            }
            options->concurrency = (unsigned)parsed;
        } else if (strcmp(name, "--payload") == 0) {
            if (parse_positive_u64(value, &parsed) != 0 ||
                parsed < sizeof(uint64_t) ||
                parsed > BENCH_MAX_PAYLOAD) {
                errno = EINVAL;
                return -1;
            }
            options->payload = (size_t)parsed;
        } else if (strcmp(name, "--activations") == 0) {
            if (parse_positive_u64(value, &parsed) != 0) {
                return -1;
            }
            options->activations = parsed;
        } else {
            errno = EINVAL;
            return -1;
        }
    }
    if (!options->candidate_set ||
        options->activations < options->concurrency ||
        options->activations > SIZE_MAX) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

bool bench_metrics_timing_is_valid(
    bench_candidate_t candidate,
    const bench_metrics_t *metrics) {
    uint64_t remaining;

    if (metrics == NULL ||
        metrics->bind_ns == 0U ||
        metrics->execute_ns == 0U) {
        return false;
    }
    if (candidate == BENCH_CANDIDATE_PORTABLE) {
        return metrics->aot_prepare_ns == 0U &&
               metrics->aot_ring_ns == 0U &&
               metrics->aot_resume_ns == 0U;
    }
    if (candidate != BENCH_CANDIDATE_NATIVE ||
        metrics->aot_prepare_ns == 0U ||
        metrics->aot_ring_ns == 0U ||
        metrics->aot_resume_ns == 0U) {
        return false;
    }
    remaining = metrics->execute_ns;
    if (metrics->aot_prepare_ns > remaining) {
        return false;
    }
    remaining -= metrics->aot_prepare_ns;
    if (metrics->aot_ring_ns > remaining) {
        return false;
    }
    remaining -= metrics->aot_ring_ns;
    return metrics->aot_resume_ns <= remaining;
}

uint64_t monotonic_ns(void) {
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0U;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

uint64_t process_cpu_ns(void) {
    struct timespec value;

    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) {
        return 0U;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

static uint64_t rotate_left(uint64_t value, unsigned amount) {
    amount &= 63U;
    return amount == 0U
        ? value
        : (value << amount) | (value >> (64U - amount));
}

static uint64_t payload_hash(const unsigned char *data, size_t size) {
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t i;

    for (i = 0U; i < size; i += 1U) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void fill_payload(
    unsigned char *payload,
    size_t size,
    uint64_t activation) {
    uint64_t state = activation ^ UINT64_C(0x6c6569722d616f74);
    size_t i;

    for (i = 0U; i < sizeof(activation); i += 1U) {
        payload[i] = (unsigned char)(activation >> (i * 8U));
    }
    for (i = sizeof(activation); i < size; i += 1U) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        payload[i] = (unsigned char)(state >> 23U);
    }
}

static uint64_t payload_activation(const unsigned char *payload) {
    uint64_t activation = 0U;
    size_t i;

    for (i = 0U; i < sizeof(activation); i += 1U) {
        activation |= (uint64_t)payload[i] << (i * 8U);
    }
    return activation;
}

void fail_state(
    bench_state_t *state,
    const char *stage,
    int error_code) {
    unsigned expected = 0U;

    if (atomic_compare_exchange_strong_explicit(
            &state->failures,
            &expected,
            1U,
            memory_order_acq_rel,
            memory_order_acquire)) {
        state->first_error = error_code != 0 ? error_code : EIO;
        (void)snprintf(
            state->first_stage, sizeof(state->first_stage), "%s", stage);
    }
}

static int wait_for_fd(int fd, short events) {
    struct pollfd item = {
        .fd = fd,
        .events = events,
    };
    int result;

    do {
        result = poll(&item, 1U, BENCH_TIMEOUT_MS);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    return result < 0 ? -1 : 0;
}

int create_listener(bench_state_t *state) {
    int listener;

    if (state->options.family == BENCH_FAMILY_TCP) {
        struct sockaddr_in address;
        socklen_t length = (socklen_t)sizeof(address);
        int reuse = 1;

        listener = socket(
            AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (listener < 0) {
            return -1;
        }
        (void)setsockopt(
            listener,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            (socklen_t)sizeof(reuse));
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(
                listener,
                (const struct sockaddr *)(const void *)&address,
                (socklen_t)sizeof(address)) != 0 ||
            listen(listener, BENCH_MAX_CONCURRENCY * 2) != 0 ||
            getsockname(
                listener,
                (struct sockaddr *)(void *)&address,
                &length) != 0) {
            int saved_errno = errno;

            (void)close(listener);
            errno = saved_errno;
            return -1;
        }
        memset(&state->address, 0, sizeof(state->address));
        memcpy(&state->address, &address, sizeof(address));
        state->address_length = length;
    } else {
        struct sockaddr_un address;
        char name[sizeof(address.sun_path) - 1U];
        int length;

        listener = socket(
            AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (listener < 0) {
            return -1;
        }
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        length = snprintf(
            name,
            sizeof(name),
            "llam-leir-aot-%ld-%p",
            (long)getpid(),
            (void *)state);
        if (length <= 0 || (size_t)length >= sizeof(name)) {
            (void)close(listener);
            errno = EOVERFLOW;
            return -1;
        }
        memcpy(address.sun_path + 1, name, (size_t)length);
        state->address_length = (socklen_t)(
            offsetof(struct sockaddr_un, sun_path) + 1U +
            (size_t)length);
        if (bind(
                listener,
                (const struct sockaddr *)(const void *)&address,
                state->address_length) != 0 ||
            listen(listener, BENCH_MAX_CONCURRENCY * 2) != 0) {
            int saved_errno = errno;

            (void)close(listener);
            errno = saved_errno;
            return -1;
        }
        memset(&state->address, 0, sizeof(state->address));
        memcpy(&state->address, &address, sizeof(address));
    }
    state->listener = listener;
    return 0;
}

int create_client(const bench_state_t *state) {
    int domain = state->options.family == BENCH_FAMILY_TCP
        ? AF_INET
        : AF_UNIX;

    return socket(
        domain, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
}

static int peer_read_connection(
    bench_peer_t *peer,
    int accepted) {
    bench_state_t *state = peer->state;
    size_t offset = 0U;
    uint64_t activation;

    while (offset < state->options.payload) {
        ssize_t received;

        if (wait_for_fd(accepted, POLLIN) != 0) {
            return -1;
        }
        received = recv(
            accepted,
            peer->observed + offset,
            state->options.payload - offset,
            0);
        if (received <= 0) {
            errno = received == 0 ? ECONNRESET : errno;
            return -1;
        }
        offset += (size_t)received;
    }

    activation = payload_activation(peer->observed);
    if (activation >= state->options.activations ||
        peer->seen[activation] != 0U) {
        errno = EPROTO;
        return -1;
    }
    fill_payload(
        peer->expected, state->options.payload, activation);
    if (memcmp(
            peer->observed,
            peer->expected,
            state->options.payload) != 0) {
        errno = EPROTO;
        return -1;
    }
    peer->seen[activation] = 1U;
    peer->checksum ^= rotate_left(
        payload_hash(peer->observed, state->options.payload),
        (unsigned)(activation & 63U));
    peer->completed += 1U;
    return 0;
}

void *peer_main(void *opaque) {
    bench_peer_t *peer = opaque;
    bench_state_t *state = peer->state;

    while (peer->completed < state->options.activations) {
        int accepted;

        if (wait_for_fd(state->listener, POLLIN) != 0) {
            peer->error_code = errno;
            (void)llam_runtime_request_stop();
            return NULL;
        }
        accepted = accept4(
            state->listener,
            NULL,
            NULL,
            SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (accepted < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            peer->error_code = errno;
            (void)llam_runtime_request_stop();
            return NULL;
        }
        if (peer_read_connection(peer, accepted) != 0) {
            peer->error_code = errno;
            (void)close(accepted);
            (void)llam_runtime_request_stop();
            return NULL;
        }
        (void)close(accepted);
    }
    return NULL;
}

#endif
