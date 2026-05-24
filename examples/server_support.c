/**
 * @file examples/server_support.c
 * @brief CLI and socket-address helpers for the LLAM chat server.
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

#include "server_support.h"
#include "diagnostic_output.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool chat_env_enabled(const char *value, bool default_value) {
    if (value == NULL || value[0] == '\0') {
        return default_value;
    }
    return strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 &&
           strcmp(value, "FALSE") != 0 &&
           strcmp(value, "off") != 0 &&
           strcmp(value, "OFF") != 0;
}

int chat_open_append_regular(const char *path) {
    return llam_example_open_append_regular(path);
}

void chat_peer_name(const struct sockaddr_storage *addr, socklen_t addrlen, char *out, size_t out_size) {
    const void *src = NULL;
    unsigned port = 0U;
    char ip[INET6_ADDRSTRLEN];

    (void)addrlen;
    if (out_size == 0U) {
        return;
    }
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)(const void *)addr;

        src = &in->sin_addr;
        port = ntohs(in->sin_port);
    } else if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)(const void *)addr;

        src = &in6->sin6_addr;
        port = ntohs(in6->sin6_port);
    }
    if (src == NULL || inet_ntop(addr->ss_family, src, ip, sizeof(ip)) == NULL) {
        (void)snprintf(out, out_size, "unknown");
        return;
    }
    (void)snprintf(out, out_size, "%s:%u", ip, port);
}

uint16_t chat_parse_port(const char *value, uint16_t default_port) {
    char *end = NULL;
    unsigned long port;

    if (value == NULL || value[0] == '\0') {
        return default_port;
    }
    errno = 0;
    port = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || port == 0UL || port > 65535UL) {
        fprintf(stderr, "invalid port: %s\n", value);
        return 0U;
    }
    return (uint16_t)port;
}

void chat_print_usage(const char *program) {
    fprintf(stderr,
            "usage: %s [--public] [--lossless|--best-effort] [port]\n"
            "  --lossless     block producers when a client outbox is full\n"
            "  --best-effort  drop per-client deliveries when an outbox is full\n",
            program != NULL ? program : "server");
}
