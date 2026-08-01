/**
 * @file examples/server_support.h
 * @brief Small helpers shared by the LLAM chat server builds.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#ifndef LLAM_EXAMPLES_SERVER_SUPPORT_H
#define LLAM_EXAMPLES_SERVER_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>

bool chat_env_enabled(const char *value, bool default_value);
int chat_open_append_regular(const char *path);
void chat_peer_name(const struct sockaddr_storage *addr, socklen_t addrlen, char *out, size_t out_size);
uint16_t chat_parse_port(const char *value, uint16_t default_port);
void chat_print_usage(const char *program);

#endif
