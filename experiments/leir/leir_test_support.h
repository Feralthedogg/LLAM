// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

#ifndef LLAM_EXPERIMENTS_LEIR_TEST_SUPPORT_H
#define LLAM_EXPERIMENTS_LEIR_TEST_SUPPORT_H

#include "leir_phase0.h"

#include <stddef.h>
#include <stdint.h>

int leir_test_socketpair(llam_fd_t pair_out[2]);
int leir_test_socketpair_type(
    int socket_type,
    llam_fd_t pair_out[2]);
void leir_test_close(llam_fd_t *fd);
int leir_test_set_socket_buffers(llam_fd_t fd, int size);
int leir_test_shutdown_write(llam_fd_t fd);
int leir_test_read_exact(llam_fd_t fd, void *data, size_t size);
int leir_test_write_all(llam_fd_t fd, const void *data, size_t size);
void leir_test_fill_pattern(
    unsigned char *data,
    size_t size,
    uint64_t seed);
void leir_test_prepare_payload(
    unsigned char *data,
    size_t size,
    uint64_t connection,
    uint64_t sequence);
bool leir_test_payload_is_valid(
    const unsigned char *data,
    size_t size,
    uint64_t connection,
    uint64_t sequence);
void leir_test_transform_payload(
    unsigned char *data,
    size_t size);
uint64_t leir_test_payload_checksum(
    const unsigned char *data,
    size_t size,
    uint64_t connection,
    uint64_t sequence);

#endif
