/**
 * @file src/io/windows/watch/windows_state.c
 * @brief Windows IOCP submission/control queue state helpers.
 *
 * @details
 * This file owns backend-neutral queue mutation for the Windows IOCP watcher:
 * submit queue insertion/removal, control queue insertion/removal, and
 * in-flight owner accounting used by dynamic rehome logic.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 * Licensed under the LLAM Commercial Reciprocity License 1.0.
 * See the LICENSE file distributed with this Software.
 */

#include "io/runtime_io_api_internal.h"

int llam_node_queue_control(llam_node_t *node, llam_io_control_kind_t kind, void *target) {
    int rc;

    if (node == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&node->watch_lock);
    rc = llam_node_queue_control_locked(node, kind, target);
    pthread_mutex_unlock(&node->watch_lock);
    if (rc == 0) {
        llam_kick_node(node);
    }
    return rc;
}

bool llam_drop_node_control_locked(llam_node_t *node, llam_io_control_kind_t kind, const void *target) {
    llam_io_control_op_t *prev = NULL;
    llam_io_control_op_t *cur;

    if (node == NULL) {
        return false;
    }
    cur = node->control_head;
    while (cur != NULL) {
        if (cur->kind == kind && cur->target == target) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                node->control_head = cur->next;
            }
            if (node->control_tail == cur) {
                node->control_tail = prev;
            }
            llam_io_control_op_destroy(node, cur);
            return true;
        }
        prev = cur;
        cur = cur->next;
    }
    return false;
}
