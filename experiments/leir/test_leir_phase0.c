#include "runtime_internal.h"

#include <stdio.h>
#include <string.h>

static bool test_sink(llam_node_t *node,
                      llam_io_req_t *req,
                      unsigned completion_owner,
                      llam_wait_reason_t *wake_reason,
                      void *context) {
    unsigned *calls = context;

    (void)node;
    if (req == NULL || completion_owner != 7U ||
        wake_reason == NULL || *wake_reason != LLAM_WAIT_IO) {
        return false;
    }
    *calls += 1U;
    return true;
}

static int test_completion_sink_dispatch(void) {
    llam_io_req_t req;
    llam_wait_reason_t wake_reason = LLAM_WAIT_IO;
    unsigned calls = 0U;

    memset(&req, 0, sizeof(req));
    req.completion_sink = test_sink;
    req.completion_sink_context = &calls;
    if (!llam_io_dispatch_completion_sink(
            NULL, &req, 7U, &wake_reason) ||
        calls != 1U) {
        return 1;
    }
    req.completion_sink = NULL;
    return llam_io_dispatch_completion_sink(
               NULL, &req, 7U, &wake_reason)
               ? 1
               : 0;
}

int main(void) {
    if (test_completion_sink_dispatch() != 0) {
        fputs("test_completion_sink_dispatch failed\n", stderr);
        return 1;
    }
    puts("LEIR Phase 0 tests passed");
    return 0;
}
