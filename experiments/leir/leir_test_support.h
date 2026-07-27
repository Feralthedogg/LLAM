#ifndef LLAM_EXPERIMENTS_LEIR_TEST_SUPPORT_H
#define LLAM_EXPERIMENTS_LEIR_TEST_SUPPORT_H

#include "leir_phase0.h"

#include <stddef.h>
#include <stdint.h>

int leir_test_socketpair(llam_fd_t pair_out[2]);
void leir_test_close(llam_fd_t *fd);
int leir_test_set_socket_buffers(llam_fd_t fd, int size);
int leir_test_shutdown_write(llam_fd_t fd);
int leir_test_read_exact(llam_fd_t fd, void *data, size_t size);
int leir_test_write_all(llam_fd_t fd, const void *data, size_t size);
void leir_test_fill_pattern(
    unsigned char *data,
    size_t size,
    uint64_t seed);

#endif
