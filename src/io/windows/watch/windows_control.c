/**
 * @file src/io/windows/watch/windows_control.c
 * @brief Windows IOCP control-packet handling.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "io/windows/runtime_io_watch_windows_internal.h"

/**
 * @brief Detach all queued control operations from an IOCP node.
 *
 * @param node Node whose control queue should be drained.
 *
 * @return Singly linked list of detached control operations.
 */
static llam_io_control_op_t *llam_take_node_controls(llam_node_t *node) {
    llam_io_control_op_t *head;

    pthread_mutex_lock(&node->watch_lock);
    head = node->control_head;
    node->control_head = NULL;
    node->control_tail = NULL;
    pthread_mutex_unlock(&node->watch_lock);
    return head;
}

/**
 * @brief Resolve one IOCP control operation.
 *
 * Currently the only native control packet is request cancellation. Fallback
 * watch controls are intentionally kept out of the IOCP worker path.
 *
 * @param op Control operation to process and free.
 */
static void llam_windows_process_control(llam_node_t *node, llam_io_control_op_t *op) {
    llam_io_req_t *req;
    llam_windows_io_op_t *io_op;

    if (op == NULL) {
        return;
    }
    if (op->kind != LLAM_IO_CONTROL_REQ_CANCEL) {
        llam_io_control_op_destroy(node, op);
        return;
    }

    req = op->target;
    if (req != NULL && atomic_load_explicit(&req->wait_mode, memory_order_acquire) == LLAM_IO_WAIT_MODE_INFLIGHT) {
        io_op = req->platform_data;
        if (io_op != NULL && io_op->magic == LLAM_WINDOWS_IO_OP_MAGIC) {
            DWORD error_code;
            bool already_closing;

            atomic_fetch_add_explicit(&io_op->node->windows_cancel_controls, 1U, memory_order_relaxed);
            HANDLE cancel_handle = (req->kind == LLAM_IO_KIND_HANDLE_READ ||
                                    req->kind == LLAM_IO_KIND_HANDLE_WRITE ||
                                    req->kind == LLAM_IO_KIND_HANDLE_PREAD ||
                                    req->kind == LLAM_IO_KIND_HANDLE_PWRITE) ?
                                       (HANDLE)req->handle :
                                       (HANDLE)(uintptr_t)req->fd;

            llam_fd_watch_lifecycle_lock();
            already_closing = io_op->association == NULL || io_op->association->closing;
            if (already_closing) {
                atomic_fetch_add_explicit(&io_op->node->windows_cancel_not_found, 1U, memory_order_relaxed);
            } else if (CancelIoEx(cancel_handle, &io_op->overlapped)) {
                atomic_fetch_add_explicit(&io_op->node->windows_cancel_success, 1U, memory_order_relaxed);
            } else {
                error_code = GetLastError();
                atomic_fetch_add_explicit(&io_op->node->windows_cancel_failures, 1U, memory_order_relaxed);
                if (error_code == ERROR_NOT_FOUND) {
                    atomic_fetch_add_explicit(&io_op->node->windows_cancel_not_found, 1U, memory_order_relaxed);
                }
            }
            llam_fd_watch_lifecycle_unlock();
        }
    }
    llam_io_control_op_destroy(node, op);
}

/**
 * @brief Drain and process all queued IOCP control operations for a node.
 *
 * @param node Node whose control queue should be processed.
 */
void llam_windows_process_controls(llam_node_t *node) {
    llam_io_control_op_t *controls = llam_take_node_controls(node);

    while (controls != NULL) {
        llam_io_control_op_t *next = controls->next;

        controls->next = NULL;
        llam_windows_process_control(node, controls);
        controls = next;
    }
}
