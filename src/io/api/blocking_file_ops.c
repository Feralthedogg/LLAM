/**
 * @file src/io/api/blocking_file_ops.c
 * @brief Blocking fallback callbacks for generic handles and positional file I/O.
 *
 * @details
 * These callbacks are isolated from socket accept/connect/poll fallbacks because
 * file HANDLE and positional descriptor operations have simpler retry semantics.
 * Keeping them separate prevents the general blocking fallback module from
 * accumulating unrelated platform policy.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "io/runtime_io_api_internal.h"

/**
 * @brief Blocking-worker fallback for positional descriptor reads.
 */
void *llam_blocking_pread_impl(void *arg) {
    llam_io_req_t *req = arg;

    if (req == NULL) {
        return NULL;
    }
    if (llam_blocking_req_cancelled(req)) {
        llam_blocking_req_set_cancelled(req);
        return req;
    }
    do {
        req->result = llam_platform_pread_fd(req->fd, req->buf, req->count, req->offset);
    } while (req->result < 0 && errno == EINTR);
    return req;
}

/**
 * @brief Blocking-worker fallback for positional descriptor writes.
 */
void *llam_blocking_pwrite_impl(void *arg) {
    llam_io_req_t *req = arg;

    if (req == NULL) {
        return NULL;
    }
    if (llam_blocking_req_cancelled(req)) {
        llam_blocking_req_set_cancelled(req);
        return req;
    }
    do {
        req->result = llam_platform_pwrite_fd(req->fd, req->buf, req->count, req->offset);
    } while (req->result < 0 && errno == EINTR);
    return req;
}

/**
 * @brief Blocking-worker fallback for generic handle reads.
 */
void *llam_blocking_handle_read_impl(void *arg) {
    llam_io_req_t *req = arg;

    if (req == NULL) {
        return NULL;
    }
    do {
        req->result = llam_platform_read_handle(req->handle, req->buf, req->count);
    } while (req->result < 0 && errno == EINTR);
    return req;
}

/**
 * @brief Blocking-worker fallback for generic handle writes.
 */
void *llam_blocking_handle_write_impl(void *arg) {
    llam_io_req_t *req = arg;

    if (req == NULL) {
        return NULL;
    }
    do {
        req->result = llam_platform_write_handle(req->handle, req->buf, req->count);
    } while (req->result < 0 && errno == EINTR);
    return req;
}

/**
 * @brief Blocking-worker fallback for positional generic handle reads.
 */
void *llam_blocking_handle_pread_impl(void *arg) {
    llam_io_req_t *req = arg;

    if (req == NULL) {
        return NULL;
    }
    if (llam_blocking_req_cancelled(req)) {
        llam_blocking_req_set_cancelled(req);
        return req;
    }
    do {
        req->result = llam_platform_pread_handle(req->handle, req->buf, req->count, req->offset);
    } while (req->result < 0 && errno == EINTR);
    return req;
}

/**
 * @brief Blocking-worker fallback for positional generic handle writes.
 */
void *llam_blocking_handle_pwrite_impl(void *arg) {
    llam_io_req_t *req = arg;

    if (req == NULL) {
        return NULL;
    }
    if (llam_blocking_req_cancelled(req)) {
        llam_blocking_req_set_cancelled(req);
        return req;
    }
    do {
        req->result = llam_platform_pwrite_handle(req->handle, req->buf, req->count, req->offset);
    } while (req->result < 0 && errno == EINTR);
    return req;
}

