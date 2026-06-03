/**
 * @file examples/broker.c
 * @brief Minimal LLAM secure-broker command-line hook.
 *
 * @details
 * This binary is intentionally a control-plane skeleton, not the final IPC
 * broker. It verifies that broker-owned runtime state can issue, validate, and
 * revoke capability tokens without exposing broker keys to client code.
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

#include "runtime_broker.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void broker_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --self-test\n"
            "       %s --serve <local-transport-name>\n"
            "       %s --serve-once <local-transport-name>\n"
            "       %s --serve-n <local-transport-name> <connections>\n"
            "       %s --client-self-test <local-transport-name>\n"
            "       %s --help\n",
            argv0,
            argv0,
            argv0,
            argv0,
            argv0,
            argv0);
}

static int broker_self_test(void) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    llam_capability_token_t token;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        perror("llam_runtime_opts_init");
        return 1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        perror("llam_broker_init");
        return 1;
    }
    if (llam_broker_create_channel(&broker,
                                   2U,
                                   LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_CLOSE,
                                   &token) != 0) {
        perror("llam_broker_create_channel");
        llam_broker_destroy(&broker);
        return 1;
    }
    if (llam_broker_validate_cap(&broker, &token, LLAM_CAP_RIGHT_SEND) != 0) {
        perror("llam_broker_validate_cap");
        llam_broker_destroy(&broker);
        return 1;
    }
    (void)llam_broker_revoke_all(&broker);
    errno = 0;
    if (llam_broker_validate_cap(&broker, &token, LLAM_CAP_RIGHT_SEND) == 0 || errno != EACCES) {
        fprintf(stderr, "revoked broker token unexpectedly validated errno=%d\n", errno);
        llam_broker_destroy(&broker);
        return 1;
    }
    llam_broker_destroy(&broker);
    puts("llam_broker self-test ok");
    return 0;
}

static int broker_serve_n(const char *path, size_t max_connections) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    int rc;

    if (max_connections == 0U) {
        errno = EINVAL;
        perror("connection count");
        return 1;
    }
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        perror("llam_runtime_opts_init");
        return 1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        perror("llam_broker_init");
        return 1;
    }
    rc = llam_broker_serve_local_n(&broker, path, max_connections);
    if (rc != 0) {
        perror("llam_broker_serve_local_n");
        llam_broker_destroy(&broker);
        return 1;
    }
    llam_broker_destroy(&broker);
    return 0;
}

static int broker_serve_once(const char *path) {
    return broker_serve_n(path, 1U);
}

static int broker_serve_forever(const char *path) {
    llam_runtime_opts_t opts;
    llam_broker_t broker;
    int rc;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        perror("llam_runtime_opts_init");
        return 1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_broker_init(&broker, &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        perror("llam_broker_init");
        return 1;
    }
    rc = llam_broker_serve_local(&broker, path);
    if (rc != 0) {
        perror("llam_broker_serve_local");
        llam_broker_destroy(&broker);
        return 1;
    }
    llam_broker_destroy(&broker);
    return 0;
}

static int broker_parse_count(const char *text, size_t *out_count) {
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || out_count == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (text[0] == ' ' || text[0] == '\t' || text[0] == '\n' ||
        text[0] == '\r' || text[0] == '\f' || text[0] == '\v' ||
        text[0] == '-' || text[0] == '+') {
        errno = EINVAL;
        return -1;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0ULL || value > (unsigned long long)SIZE_MAX) {
        errno = EINVAL;
        return -1;
    }
    *out_count = (size_t)value;
    return 0;
}

static int broker_client_self_test(const char *path) {
    if (llam_broker_client_self_test_local(path) != 0) {
        perror("llam_broker_client_self_test_local");
        return 1;
    }
    puts("llam_broker client self-test ok");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
        return broker_self_test();
    }
    if (argc == 3 && strcmp(argv[1], "--serve") == 0) {
        return broker_serve_forever(argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "--serve-once") == 0) {
        return broker_serve_once(argv[2]);
    }
    if (argc == 4 && strcmp(argv[1], "--serve-n") == 0) {
        size_t count = 0U;

        if (broker_parse_count(argv[3], &count) != 0) {
            perror("connection count");
            return 2;
        }
        return broker_serve_n(argv[2], count);
    }
    if (argc == 3 && strcmp(argv[1], "--client-self-test") == 0) {
        return broker_client_self_test(argv[2]);
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        broker_usage(argv[0]);
        return 0;
    }
    broker_usage(argc > 0 ? argv[0] : "llam_broker");
    return 2;
}
