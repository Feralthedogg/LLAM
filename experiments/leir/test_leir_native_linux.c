#include <stdio.h>

#if !defined(__linux__)

int main(void) {
    puts("SKIP: LEIR native Linux tests require Linux");
    return 0;
}

#else

#include "io/linux/runtime_io_segment_linux_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef int (*test_fn)(void);

typedef struct test_case {
    const char *name;
    test_fn run;
} test_case_t;

typedef struct queue_fixture {
    llam_runtime_t runtime;
    llam_runtime_t foreign_runtime;
    llam_shard_t shard;
    llam_node_t node;
    llam_io_req_t req;
    llam_linux_native_segment_t segment;
    llam_linux_native_batch_t batch;
    llam_linux_native_op_t
        ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char
        buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    bool shard_lock_ready;
    bool submit_lock_ready;
} queue_fixture_t;

static void fill_operations(
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS],
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32],
    unsigned count) {
    unsigned i;

    memset(
        ops,
        0,
        LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS * sizeof(ops[0]));
    memset(
        buffers,
        0,
        LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS * sizeof(buffers[0]));
    for (i = 0U; i < count; i += 1U) {
        ops[i].kind = LLAM_LINUX_NATIVE_OP_SEND;
        ops[i].result_slot = (uint16_t)(4U + i);
        ops[i].fd = (llam_fd_t)(10 + (int)i);
        ops[i].buffer = buffers[i];
        ops[i].length = (uint32_t)(16U + i);
    }
}

static int configure_segment(
    llam_linux_native_segment_t *segment,
    llam_linux_native_segment_mode_t mode,
    unsigned count,
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS],
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32]) {
    unsigned i;

    fill_operations(ops, buffers, count);
    if (llam_linux_native_segment_configure(
            segment, ops, count, mode) != 0) {
        return -1;
    }
    segment->generation = UINT64_C(42);
    for (i = 0U; i < count; i += 1U) {
        segment->tokens[i].generation = UINT64_C(42);
    }
    atomic_store_explicit(
        &segment->state,
        LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT,
        memory_order_release);
    return 0;
}

static bool consume_test_completion(
    llam_node_t *node,
    llam_io_req_t *req,
    unsigned completion_owner,
    llam_wait_reason_t *wake_reason,
    void *context) {
    unsigned *completion_count = context;

    (void)node;
    (void)req;
    (void)completion_owner;
    (void)wake_reason;
    if (completion_count != NULL) {
        *completion_count += 1U;
    }
    return true;
}

static int queue_fixture_init(
    queue_fixture_t *fixture,
    llam_linux_native_segment_mode_t mode,
    unsigned op_count,
    unsigned *completion_count) {
    int rc;

    memset(fixture, 0, sizeof(*fixture));
    atomic_init(&fixture->runtime.fatal_errno, 0);
    fixture->runtime.active_shards = 1U;
    fixture->runtime.shards = &fixture->shard;
    fixture->runtime.active_nodes = 1U;
    fixture->runtime.nodes = &fixture->node;
    fixture->shard.runtime = &fixture->runtime;
    fixture->shard.id = 0U;
    atomic_init(&fixture->shard.inflight_io_waiters, 0U);
    rc = pthread_mutex_init(&fixture->shard.lock, NULL);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    fixture->shard_lock_ready = true;

    fixture->node.runtime = &fixture->runtime;
    fixture->node.index = 0U;
    fixture->node.event_fd = -1;
    fixture->node.linux_ring_features = IORING_FEAT_CQE_SKIP;
    atomic_init(&fixture->node.event_pending, 0U);
    atomic_init(&fixture->node.pending_ops, 0U);
    rc = pthread_mutex_init(&fixture->node.submit_lock, NULL);
    if (rc != 0) {
        errno = rc;
        pthread_mutex_destroy(&fixture->shard.lock);
        fixture->shard_lock_ready = false;
        return -1;
    }
    fixture->submit_lock_ready = true;

    llam_io_req_reset(
        &fixture->req, &fixture->runtime, 0U, UINT_MAX);
    atomic_store_explicit(
        &fixture->req.wait_mode,
        LLAM_IO_WAIT_MODE_SUBMIT_QUEUE,
        memory_order_release);
    fixture->req.completion_sink = consume_test_completion;
    fixture->req.completion_sink_context = completion_count;

    fill_operations(fixture->ops, fixture->buffers, op_count);
    if (llam_linux_native_segment_configure(
            &fixture->segment,
            fixture->ops,
            op_count,
            mode) != 0) {
        pthread_mutex_destroy(&fixture->node.submit_lock);
        pthread_mutex_destroy(&fixture->shard.lock);
        fixture->submit_lock_ready = false;
        fixture->shard_lock_ready = false;
        return -1;
    }
    fixture->segment.owner_runtime = &fixture->runtime;
    fixture->segment.generation = UINT64_C(1);
    fixture->batch.owner_runtime = &fixture->runtime;
    fixture->batch.segments[0] = &fixture->segment;
    fixture->batch.segment_count = 1U;
    atomic_init(
        &fixture->batch.state,
        LLAM_LINUX_NATIVE_BATCH_IDLE);
    atomic_init(&fixture->batch.terminal_claimed, 0U);
    atomic_init(
        &fixture->batch.cancel_state,
        LLAM_LINUX_NATIVE_CANCEL_NONE);
    atomic_init(&fixture->batch.cancel_requested, 0U);
    return 0;
}

static void queue_fixture_destroy(queue_fixture_t *fixture) {
    if (fixture->submit_lock_ready) {
        pthread_mutex_destroy(&fixture->node.submit_lock);
        fixture->submit_lock_ready = false;
    }
    if (fixture->shard_lock_ready) {
        pthread_mutex_destroy(&fixture->shard.lock);
        fixture->shard_lock_ready = false;
    }
}

typedef struct resource_update_fixture {
    unsigned file_calls;
    unsigned buffer_calls;
    unsigned fail_file_call;
    unsigned fail_buffer_call;
    uint64_t live_files;
    uint64_t live_buffers;
} resource_update_fixture_t;

static int resource_files_update(
    llam_node_t *node,
    unsigned offset,
    const int *files,
    unsigned count,
    void *arg) {
    resource_update_fixture_t *fixture = arg;
    uint64_t bit;

    (void)node;
    if (files == NULL || count != 1U || offset >= 64U) {
        return -EINVAL;
    }
    fixture->file_calls += 1U;
    if (fixture->fail_file_call != 0U &&
        fixture->file_calls == fixture->fail_file_call) {
        return -EIO;
    }
    bit = UINT64_C(1) << offset;
    if (files[0] < 0) {
        fixture->live_files &= ~bit;
    } else {
        fixture->live_files |= bit;
    }
    return 1;
}

static int resource_buffers_update(
    llam_node_t *node,
    unsigned offset,
    const struct iovec *buffers,
    unsigned count,
    void *arg) {
    resource_update_fixture_t *fixture = arg;
    uint64_t bit;

    (void)node;
    if (buffers == NULL ||
        count != 1U ||
        offset >= 64U) {
        return -EINVAL;
    }
    fixture->buffer_calls += 1U;
    if (fixture->fail_buffer_call != 0U &&
        fixture->buffer_calls ==
            fixture->fail_buffer_call) {
        return -EIO;
    }
    bit = UINT64_C(1) << offset;
    if (buffers[0].iov_base == NULL &&
        buffers[0].iov_len == 0U) {
        fixture->live_buffers &= ~bit;
    } else {
        fixture->live_buffers |= bit;
    }
    return 1;
}

static int init_resource_node(
    llam_node_t *node,
    resource_update_fixture_t *updates) {
    int rc;

    memset(node, 0, sizeof(*node));
    memset(updates, 0, sizeof(*updates));
    node->supports_native_fixed_files = true;
    node->supports_native_fixed_buffers = true;
    node->native_files_update_override =
        resource_files_update;
    node->native_buffers_update_override =
        resource_buffers_update;
    node->native_resource_update_override_arg =
        updates;
    rc = pthread_mutex_init(
        &node->native_resource_lock, NULL);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    node->native_resource_lock_initialized = true;
    return 0;
}

static void destroy_resource_node(llam_node_t *node) {
    if (node->native_resource_lock_initialized) {
        pthread_mutex_destroy(
            &node->native_resource_lock);
        node->native_resource_lock_initialized = false;
    }
}

static int test_resource_slots_attach_and_detach(void) {
    llam_node_t node;
    resource_update_fixture_t updates;
    llam_linux_native_resource_lease_t lease;
    int fds[2] = {10, 11};
    unsigned char storage[2][32];
    struct iovec buffers[2] = {
        {storage[0], sizeof(storage[0])},
        {storage[1], sizeof(storage[1])},
    };

    memset(&lease, 0, sizeof(lease));
    if (init_resource_node(&node, &updates) != 0) {
        perror("resource node init");
        return 1;
    }
    node.index = 3U;
    if (llam_linux_native_resources_attach(
            &node,
            fds,
            2U,
            buffers,
            2U,
            &lease) != 0 ||
        !lease.attached ||
        lease.node_index != 3U ||
        lease.file_count != 2U ||
        lease.buffer_count != 2U ||
        __builtin_popcountll(
            node.native_fixed_file_bitmap) != 2 ||
        __builtin_popcountll(
            node.native_fixed_buffer_bitmap) != 2 ||
        __builtin_popcountll(updates.live_files) != 2 ||
        __builtin_popcountll(updates.live_buffers) != 2 ||
        llam_linux_native_resources_detach(
            &node, &lease) != 0 ||
        lease.attached ||
        lease.file_count != 0U ||
        lease.buffer_count != 0U ||
        node.native_fixed_file_bitmap != 0U ||
        node.native_fixed_buffer_bitmap != 0U ||
        updates.live_files != 0U ||
        updates.live_buffers != 0U) {
        fprintf(stderr, "native resource lease mismatch\n");
        destroy_resource_node(&node);
        return 1;
    }
    destroy_resource_node(&node);
    return 0;
}

static int run_resource_rollback_case(
    unsigned fail_file_call,
    unsigned fail_buffer_call) {
    llam_node_t node;
    resource_update_fixture_t updates;
    llam_linux_native_resource_lease_t lease;
    int fds[2] = {10, 11};
    unsigned char storage[2][32];
    struct iovec buffers[2] = {
        {storage[0], sizeof(storage[0])},
        {storage[1], sizeof(storage[1])},
    };

    memset(&lease, 0, sizeof(lease));
    if (init_resource_node(&node, &updates) != 0) {
        return 1;
    }
    updates.fail_file_call = fail_file_call;
    updates.fail_buffer_call = fail_buffer_call;
    errno = 0;
    if (llam_linux_native_resources_attach(
            &node,
            fds,
            2U,
            buffers,
            2U,
            &lease) == 0 ||
        errno != EIO ||
        lease.attached ||
        lease.file_count != 0U ||
        lease.buffer_count != 0U ||
        node.native_fixed_file_bitmap != 0U ||
        node.native_fixed_buffer_bitmap != 0U ||
        updates.live_files != 0U ||
        updates.live_buffers != 0U) {
        fprintf(
            stderr,
            "native resource rollback mismatch files=%u "
            "buffers=%u\n",
            fail_file_call,
            fail_buffer_call);
        destroy_resource_node(&node);
        return 1;
    }
    destroy_resource_node(&node);
    return 0;
}

static int test_resource_attach_rolls_back_updates(void) {
    return run_resource_rollback_case(2U, 0U) != 0 ||
           run_resource_rollback_case(0U, 2U) != 0;
}

static int test_resource_attach_reports_capacity(void) {
    llam_node_t node;
    resource_update_fixture_t updates;
    llam_linux_native_resource_lease_t lease;
    int fd = 10;
    unsigned char storage[32];
    struct iovec buffer = {
        storage,
        sizeof(storage),
    };

    memset(&lease, 0, sizeof(lease));
    if (init_resource_node(&node, &updates) != 0) {
        return 1;
    }
    node.native_fixed_file_bitmap = UINT64_MAX;
    node.native_fixed_buffer_bitmap = UINT64_MAX;
    errno = 0;
    if (llam_linux_native_resources_attach(
            &node,
            &fd,
            1U,
            &buffer,
            1U,
            &lease) == 0 ||
        errno != ENOSPC ||
        updates.file_calls != 0U ||
        updates.buffer_calls != 0U ||
        lease.attached) {
        fprintf(stderr, "native resource capacity mismatch\n");
        destroy_resource_node(&node);
        return 1;
    }
    destroy_resource_node(&node);
    return 0;
}

static int test_enqueue_publishes_once(void) {
    queue_fixture_t fixture;
    unsigned completions = 0U;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            4U,
            &completions) != 0) {
        perror("queue fixture init");
        return 1;
    }
    if (!llam_linux_native_batch_enqueue(
            &fixture.node,
            &fixture.batch,
            &fixture.req) ||
        fixture.node.native_batch_head != &fixture.batch ||
        fixture.node.native_batch_tail != &fixture.batch ||
        fixture.batch.next != NULL ||
        fixture.batch.owner_node != &fixture.node ||
        fixture.batch.req != &fixture.req ||
        fixture.segment.owner_node != &fixture.node ||
        fixture.segment.req != &fixture.req ||
        fixture.segment.queue_publications != 1U ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.segment.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_QUEUED ||
        atomic_load_explicit(
            &fixture.req.attached_node_index,
            memory_order_acquire) != 0U) {
        fprintf(stderr, "native enqueue ownership mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_enqueue_rejects_foreign_runtime(void) {
    queue_fixture_t fixture;
    unsigned completions = 0U;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            2U,
            &completions) != 0) {
        perror("foreign queue fixture init");
        return 1;
    }
    fixture.req.owner_runtime = &fixture.foreign_runtime;
    errno = 0;
    if (llam_linux_native_batch_enqueue(
            &fixture.node,
            &fixture.batch,
            &fixture.req) ||
        errno != EXDEV ||
        fixture.node.native_batch_head != NULL ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.segment.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_IDLE) {
        fprintf(stderr, "foreign runtime enqueue was not rejected\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_enqueue_rejects_non_idle_segment(void) {
    queue_fixture_t fixture;
    unsigned completions = 0U;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            2U,
            &completions) != 0) {
        perror("busy queue fixture init");
        return 1;
    }
    atomic_store_explicit(
        &fixture.segment.state,
        LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT,
        memory_order_release);
    errno = 0;
    if (llam_linux_native_batch_enqueue(
            &fixture.node,
            &fixture.batch,
            &fixture.req) ||
        errno != EBUSY ||
        fixture.node.native_batch_head != NULL ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U) {
        fprintf(stderr, "non-idle native segment was enqueued\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_take_all_marks_request_inflight_once(void) {
    queue_fixture_t fixture;
    llam_linux_native_batch_t *taken;
    unsigned completions = 0U;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            4U,
            &completions) != 0) {
        perror("take queue fixture init");
        return 1;
    }
    if (!llam_linux_native_batch_enqueue(
            &fixture.node,
            &fixture.batch,
            &fixture.req)) {
        perror("enqueue before take");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    taken = llam_linux_native_batch_take_all(&fixture.node);
    if (taken != &fixture.batch ||
        fixture.node.native_batch_head != NULL ||
        fixture.node.native_batch_tail != NULL ||
        atomic_load_explicit(
            &fixture.req.wait_mode,
            memory_order_acquire) !=
            LLAM_IO_WAIT_MODE_INFLIGHT ||
        atomic_load_explicit(
            &fixture.req.inflight_owner_shard,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 1U ||
        llam_linux_native_batch_take_all(&fixture.node) != NULL ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 1U) {
        fprintf(stderr, "native queue detach ownership mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int configure_batch_peer(
    queue_fixture_t *fixture,
    llam_linux_native_segment_t *segment,
    llam_linux_native_op_t
        ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS],
    unsigned char
        buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32],
    llam_linux_native_segment_mode_t mode) {
    fill_operations(ops, buffers, 2U);
    if (llam_linux_native_segment_configure(
            segment, ops, 2U, mode) != 0) {
        return -1;
    }
    segment->owner_runtime = &fixture->runtime;
    segment->generation = UINT64_C(2);
    return 0;
}

static void initialize_batch(
    llam_linux_native_batch_t *batch,
    llam_runtime_t *runtime,
    llam_linux_native_segment_t *first,
    llam_linux_native_segment_t *second,
    unsigned count) {
    memset(batch, 0, sizeof(*batch));
    batch->owner_runtime = runtime;
    batch->segments[0] = first;
    batch->segments[1] = second;
    batch->segment_count = count;
    atomic_init(&batch->state, LLAM_LINUX_NATIVE_BATCH_IDLE);
    atomic_init(&batch->terminal_claimed, 0U);
    atomic_init(
        &batch->cancel_state,
        LLAM_LINUX_NATIVE_CANCEL_NONE);
    atomic_init(&batch->cancel_requested, 0U);
}

static int test_batch_enqueue_publishes_width_two_once(void) {
    queue_fixture_t fixture;
    llam_linux_native_segment_t second;
    llam_linux_native_op_t
        second_ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char
        second_buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    llam_linux_native_batch_t batch;
    llam_linux_native_batch_t *taken;
    unsigned completions = 0U;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            2U,
            &completions) != 0 ||
        configure_batch_peer(
            &fixture,
            &second,
            second_ops,
            second_buffers,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) != 0) {
        perror("width-two batch fixture init");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    initialize_batch(
        &batch,
        &fixture.runtime,
        &fixture.segment,
        &second,
        2U);
    if (!llam_linux_native_batch_enqueue(
            &fixture.node, &batch, &fixture.req) ||
        fixture.node.native_batch_head != &batch ||
        fixture.node.native_batch_tail != &batch ||
        batch.next != NULL ||
        batch.owner_node != &fixture.node ||
        batch.req != &fixture.req ||
        fixture.segment.batch != &batch ||
        second.batch != &batch ||
        fixture.segment.queue_publications +
                second.queue_publications !=
            1U ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.segment.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_QUEUED ||
        atomic_load_explicit(
            &second.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_QUEUED) {
        fprintf(stderr, "width-two batch publication mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    taken = llam_linux_native_batch_take_all(&fixture.node);
    if (taken != &batch ||
        fixture.node.native_batch_head != NULL ||
        fixture.node.native_batch_tail != NULL ||
        atomic_load_explicit(
            &fixture.req.wait_mode,
            memory_order_acquire) !=
            LLAM_IO_WAIT_MODE_INFLIGHT ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 1U ||
        llam_linux_native_batch_take_all(
            &fixture.node) != NULL ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 1U) {
        fprintf(stderr, "width-two batch detach mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_batch_enqueue_rejects_invalid_groups(void) {
    queue_fixture_t fixture;
    llam_linux_native_segment_t second;
    llam_linux_native_op_t
        second_ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char
        second_buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    llam_linux_native_batch_t batch;
    unsigned completions = 0U;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            2U,
            &completions) != 0 ||
        configure_batch_peer(
            &fixture,
            &second,
            second_ops,
            second_buffers,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) != 0) {
        perror("invalid batch fixture init");
        queue_fixture_destroy(&fixture);
        return 1;
    }

    initialize_batch(
        &batch,
        &fixture.runtime,
        &fixture.segment,
        &second,
        0U);
    errno = 0;
    if (llam_linux_native_batch_enqueue(
            &fixture.node, &batch, &fixture.req) ||
        errno != EINVAL) {
        fprintf(stderr, "empty native batch was accepted\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    batch.segment_count =
        LLAM_LINUX_NATIVE_BATCH_MAX_SEGMENTS + 1U;
    errno = 0;
    if (llam_linux_native_batch_enqueue(
            &fixture.node, &batch, &fixture.req) ||
        errno != EINVAL) {
        fprintf(stderr, "oversized native batch was accepted\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    batch.segment_count = 2U;
    batch.segments[1] = &fixture.segment;
    errno = 0;
    if (llam_linux_native_batch_enqueue(
            &fixture.node, &batch, &fixture.req) ||
        errno != EINVAL) {
        fprintf(stderr, "duplicate native segment was accepted\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    batch.segments[1] = &second;
    second.owner_runtime = &fixture.foreign_runtime;
    errno = 0;
    if (llam_linux_native_batch_enqueue(
            &fixture.node, &batch, &fixture.req) ||
        errno != EXDEV) {
        fprintf(stderr, "mixed-runtime native batch was accepted\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    second.owner_runtime = &fixture.runtime;
    second.mode = LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP;
    fixture.node.linux_ring_features = 0U;
    errno = 0;
    if (llam_linux_native_batch_enqueue(
            &fixture.node, &batch, &fixture.req) ||
        errno != ENOTSUP ||
        fixture.node.native_batch_head != NULL ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.segment.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_IDLE ||
        atomic_load_explicit(
            &second.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_IDLE ||
        atomic_load_explicit(
            &batch.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_BATCH_IDLE) {
        fprintf(stderr, "invalid native batch mutated ownership\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_batch_abort_queued_detaches_exact_ticket(void) {
    queue_fixture_t fixture;
    llam_linux_native_segment_t middle_first;
    llam_linux_native_segment_t middle_second;
    llam_linux_native_segment_t last;
    llam_linux_native_op_t
        middle_first_ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_linux_native_op_t
        middle_second_ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_linux_native_op_t
        last_ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char
        middle_first_buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    unsigned char
        middle_second_buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    unsigned char
        last_buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    llam_linux_native_batch_t middle_batch;
    llam_linux_native_batch_t last_batch;
    llam_io_req_t middle_req;
    llam_io_req_t last_req;
    unsigned completions = 0U;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            2U,
            &completions) != 0 ||
        configure_batch_peer(
            &fixture,
            &middle_first,
            middle_first_ops,
            middle_first_buffers,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) != 0 ||
        configure_batch_peer(
            &fixture,
            &middle_second,
            middle_second_ops,
            middle_second_buffers,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) != 0 ||
        configure_batch_peer(
            &fixture,
            &last,
            last_ops,
            last_buffers,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) != 0) {
        perror("queued abort fixture init");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    middle_first.generation = UINT64_C(2);
    middle_second.generation = UINT64_C(3);
    last.generation = UINT64_C(4);
    llam_io_req_reset(
        &middle_req, &fixture.runtime, 0U, UINT_MAX);
    llam_io_req_reset(
        &last_req, &fixture.runtime, 0U, UINT_MAX);
    atomic_store_explicit(
        &middle_req.wait_mode,
        LLAM_IO_WAIT_MODE_SUBMIT_QUEUE,
        memory_order_release);
    atomic_store_explicit(
        &last_req.wait_mode,
        LLAM_IO_WAIT_MODE_SUBMIT_QUEUE,
        memory_order_release);
    initialize_batch(
        &middle_batch,
        &fixture.runtime,
        &middle_first,
        &middle_second,
        2U);
    initialize_batch(
        &last_batch,
        &fixture.runtime,
        &last,
        NULL,
        1U);
    if (!llam_linux_native_batch_enqueue(
            &fixture.node,
            &fixture.batch,
            &fixture.req) ||
        !llam_linux_native_batch_enqueue(
            &fixture.node,
            &middle_batch,
            &middle_req) ||
        !llam_linux_native_batch_enqueue(
            &fixture.node,
            &last_batch,
            &last_req) ||
        !llam_linux_native_batch_abort_queued(
            &fixture.node,
            &middle_batch,
            &middle_req) ||
        fixture.node.native_batch_head != &fixture.batch ||
        fixture.batch.next != &last_batch ||
        fixture.node.native_batch_tail != &last_batch ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 2U ||
        atomic_load_explicit(
            &middle_req.wait_mode,
            memory_order_acquire) !=
            LLAM_IO_WAIT_MODE_NONE ||
        atomic_load_explicit(
            &middle_req.linux_native_batch,
            memory_order_acquire) != NULL ||
        atomic_load_explicit(
            &middle_batch.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_BATCH_RETIRED ||
        atomic_load_explicit(
            &middle_first.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_RETIRED ||
        atomic_load_explicit(
            &middle_second.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_RETIRED) {
        fprintf(stderr, "queued native batch detach mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int keep_linux_ring_full(
    llam_node_t *node,
    unsigned expected,
    void *arg) {
    unsigned *calls = arg;

    (void)node;
    (void)expected;
    *calls += 1U;
    return 0;
}

static int fail_linux_ring_permanently(
    llam_node_t *node,
    unsigned expected,
    void *arg) {
    unsigned *calls = arg;

    (void)node;
    (void)expected;
    *calls += 1U;
    return -EIO;
}

static void init_userspace_sq_fixture(
    struct io_uring *ring,
    struct io_uring_sqe *sqes,
    unsigned *head,
    unsigned entries) {
    memset(ring, 0, sizeof(*ring));
    memset(sqes, 0, entries * sizeof(sqes[0]));
    *head = 0U;
    ring->ring_fd = -1;
    ring->sq.khead = head;
    ring->sq.sqes = sqes;
    ring->sq.ring_entries = entries;
    ring->sq.ring_mask = entries - 1U;
}

static int prepare_dispatch_fixture(
    queue_fixture_t *fixture,
    llam_linux_native_segment_mode_t mode,
    unsigned op_count,
    unsigned *completion_count,
    struct io_uring_sqe *sqes,
    unsigned sqe_count,
    unsigned *sq_head) {
    llam_linux_native_batch_t *taken;

    if (queue_fixture_init(
            fixture, mode, op_count, completion_count) != 0) {
        return -1;
    }
    init_userspace_sq_fixture(
        &fixture->node.ring, sqes, sq_head, sqe_count);
    fixture->node.ring_ready = true;
    if (!llam_linux_native_batch_enqueue(
            &fixture->node,
            &fixture->batch,
            &fixture->req)) {
        queue_fixture_destroy(fixture);
        return -1;
    }
    taken = llam_linux_native_batch_take_all(&fixture->node);
    if (taken != &fixture->batch ||
        llam_linux_native_batch_submit_one(
            &fixture->node, taken) != op_count) {
        queue_fixture_destroy(fixture);
        errno = EIO;
        return -1;
    }
    return 0;
}

typedef enum cancel_event_kind {
    CANCEL_EVENT_TARGET = 0,
    CANCEL_EVENT_CONTROL = 1,
} cancel_event_kind_t;

typedef struct cancel_event {
    cancel_event_kind_t kind;
    unsigned operation_index;
    int result;
} cancel_event_t;

static int prepare_cancel_fixture(
    queue_fixture_t *fixture,
    unsigned *completions,
    struct io_uring_sqe *sqes,
    unsigned *sq_head) {
    llam_linux_native_batch_t *cancelled;

    if (prepare_dispatch_fixture(
            fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            2U,
            completions,
            sqes,
            8U,
            sq_head) != 0) {
        return -1;
    }
    atomic_store_explicit(
        &fixture->req.abort_reason,
        LLAM_IO_ABORT_CANCEL,
        memory_order_release);
    if (!llam_linux_native_batch_request_cancel(
            &fixture->node,
            &fixture->batch,
            &fixture->req)) {
        queue_fixture_destroy(fixture);
        return -1;
    }
    cancelled = llam_linux_native_cancel_take_all(
        &fixture->node);
    if (cancelled != &fixture->batch ||
        llam_linux_native_batch_submit_cancel(
            &fixture->node, cancelled) != 2U) {
        queue_fixture_destroy(fixture);
        errno = EIO;
        return -1;
    }
    return 0;
}

static int run_cancel_order_case(
    const cancel_event_t *events,
    size_t event_count,
    ssize_t expected_result,
    int expected_error) {
    queue_fixture_t fixture;
    struct io_uring_sqe sqes[8];
    unsigned completions = 0U;
    unsigned sq_head;
    size_t i;

    if (prepare_cancel_fixture(
            &fixture,
            &completions,
            sqes,
            &sq_head) != 0) {
        perror("cancel order fixture init");
        return 1;
    }
    for (i = 0U; i < event_count; i += 1U) {
        const cancel_event_t *event = &events[i];

        if (event->kind == CANCEL_EVENT_CONTROL) {
            llam_linux_native_cancel_handle_cqe(
                &fixture.node,
                &fixture.segment.cancel_tokens[
                    event->operation_index],
                event->result);
        } else {
            llam_linux_native_segment_handle_cqe(
                &fixture.node,
                &fixture.segment.tokens[
                    event->operation_index],
                event->result);
        }
        if (i + 1U < event_count &&
            (completions != 0U ||
             atomic_load_explicit(
                 &fixture.node.pending_ops,
                 memory_order_acquire) != 1U ||
             atomic_load_explicit(
                 &fixture.shard.inflight_io_waiters,
                 memory_order_acquire) != 1U)) {
            fprintf(stderr, "native cancel completed on prefix %zu\n", i);
            queue_fixture_destroy(&fixture);
            return 1;
        }
    }
    if (completions != 1U ||
        fixture.req.result != expected_result ||
        fixture.req.error_code != expected_error ||
        fixture.batch.cancel_sqes_prepared != 2U ||
        fixture.batch.cancel_cqes_observed != 2U ||
        fixture.segment.observed_cancel_mask != UINT64_C(0x03) ||
        atomic_load_explicit(
            &fixture.batch.cancel_state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_CANCEL_RETIRED ||
        atomic_load_explicit(
            &fixture.batch.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_BATCH_RETIRED ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 0U) {
        fprintf(stderr, "native cancel retirement mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_cancel_and_target_orderings_retire_once(void) {
    static const cancel_event_t cancel_before_target[] = {
        {CANCEL_EVENT_CONTROL, 0U, 0},
        {CANCEL_EVENT_TARGET, 0U, -ECANCELED},
        {CANCEL_EVENT_CONTROL, 1U, -ENOENT},
        {CANCEL_EVENT_TARGET, 1U, -ECANCELED},
    };
    static const cancel_event_t target_before_cancel[] = {
        {CANCEL_EVENT_TARGET, 0U, 16},
        {CANCEL_EVENT_TARGET, 1U, 17},
        {CANCEL_EVENT_CONTROL, 0U, -ENOENT},
        {CANCEL_EVENT_CONTROL, 1U, -EALREADY},
    };

    if (run_cancel_order_case(
            cancel_before_target,
            sizeof(cancel_before_target) /
                sizeof(cancel_before_target[0]),
            -1,
            ECANCELED) != 0 ||
        run_cancel_order_case(
            target_before_cancel,
            sizeof(target_before_cancel) /
                sizeof(target_before_cancel[0]),
            17,
            0) != 0) {
        return 1;
    }
    return 0;
}

static int test_cancel_sqes_target_operation_tokens(void) {
    queue_fixture_t fixture;
    struct io_uring_sqe sqes[8];
    unsigned completions = 0U;
    unsigned sq_head;
    unsigned i;

    if (prepare_cancel_fixture(
            &fixture,
            &completions,
            sqes,
            &sq_head) != 0) {
        perror("cancel SQE fixture init");
        return 1;
    }
    for (i = 0U; i < 2U; i += 1U) {
        struct io_uring_sqe *sqe = &sqes[2U + i];

        if (sqe->opcode != IORING_OP_ASYNC_CANCEL ||
            sqe->addr != llam_io_udata_encode(
                &fixture.segment.tokens[i],
                LLAM_IO_UDATA_NATIVE_SEGMENT) ||
            llam_io_udata_tag(sqe->user_data) !=
                LLAM_IO_UDATA_NATIVE_CANCEL ||
            llam_io_udata_ptr(sqe->user_data) !=
                &fixture.segment.cancel_tokens[i]) {
            fprintf(stderr, "native cancel SQE token mismatch\n");
            queue_fixture_destroy(&fixture);
            return 1;
        }
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_cancel_rejects_stale_and_duplicate_tokens(void) {
    queue_fixture_t stale_fixture;
    queue_fixture_t duplicate_fixture;
    struct io_uring_sqe stale_sqes[8];
    struct io_uring_sqe duplicate_sqes[8];
    llam_linux_native_cancel_token_t stale;
    unsigned stale_completions = 0U;
    unsigned duplicate_completions = 0U;
    unsigned stale_sq_head;
    unsigned duplicate_sq_head;

    if (prepare_cancel_fixture(
            &stale_fixture,
            &stale_completions,
            stale_sqes,
            &stale_sq_head) != 0) {
        perror("stale cancel fixture init");
        return 1;
    }
    stale = stale_fixture.segment.cancel_tokens[0];
    stale.generation += 1U;
    llam_linux_native_cancel_handle_cqe(
        &stale_fixture.node, &stale, 0);
    if (stale_completions != 0U ||
        stale_fixture.batch.cancel_cqes_observed != 0U ||
        stale_fixture.segment.observed_cancel_mask != 0U ||
        atomic_load_explicit(
            &stale_fixture.runtime.fatal_errno,
            memory_order_acquire) != EPROTO) {
        fprintf(stderr, "stale cancel token was not rejected\n");
        queue_fixture_destroy(&stale_fixture);
        return 1;
    }
    queue_fixture_destroy(&stale_fixture);

    if (prepare_cancel_fixture(
            &duplicate_fixture,
            &duplicate_completions,
            duplicate_sqes,
            &duplicate_sq_head) != 0) {
        perror("duplicate cancel fixture init");
        return 1;
    }
    llam_linux_native_cancel_handle_cqe(
        &duplicate_fixture.node,
        &duplicate_fixture.segment.cancel_tokens[0],
        -ENOENT);
    llam_linux_native_cancel_handle_cqe(
        &duplicate_fixture.node,
        &duplicate_fixture.segment.cancel_tokens[0],
        -ENOENT);
    if (duplicate_completions != 0U ||
        duplicate_fixture.batch.cancel_cqes_observed != 1U ||
        duplicate_fixture.segment.observed_cancel_mask !=
            UINT64_C(0x01) ||
        atomic_load_explicit(
            &duplicate_fixture.runtime.fatal_errno,
            memory_order_acquire) != EPROTO) {
        fprintf(stderr, "duplicate cancel token was not rejected\n");
        queue_fixture_destroy(&duplicate_fixture);
        return 1;
    }
    queue_fixture_destroy(&duplicate_fixture);
    return 0;
}

static int test_cancel_local_retirement_ignores_prior_statistics(void) {
    queue_fixture_t fixture;
    llam_linux_native_batch_t *taken;
    llam_linux_native_batch_t *cancelled;
    unsigned completions = 0U;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            2U,
            &completions) != 0) {
        perror("local cancel fixture init");
        return 1;
    }
    fixture.segment.prepared_sqes = 17U;
    if (!llam_linux_native_batch_enqueue(
            &fixture.node,
            &fixture.batch,
            &fixture.req)) {
        perror("local cancel enqueue");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    taken = llam_linux_native_batch_take_all(&fixture.node);
    atomic_store_explicit(
        &fixture.req.abort_reason,
        LLAM_IO_ABORT_CANCEL,
        memory_order_release);
    if (taken != &fixture.batch ||
        !llam_linux_native_batch_request_cancel(
            &fixture.node,
            &fixture.batch,
            &fixture.req) ||
        llam_linux_native_batch_submit_one(
            &fixture.node, taken) != 0U ||
        completions != 0U) {
        fprintf(stderr, "local cancel setup mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    cancelled = llam_linux_native_cancel_take_all(
        &fixture.node);
    if (cancelled != &fixture.batch ||
        llam_linux_native_batch_submit_cancel(
            &fixture.node, cancelled) != 0U ||
        completions != 1U ||
        fixture.req.error_code != EAGAIN ||
        fixture.batch.cancel_sqes_prepared != 0U ||
        fixture.batch.cancel_cqes_observed != 0U ||
        atomic_load_explicit(
            &fixture.batch.cancel_state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_CANCEL_RETIRED ||
        atomic_load_explicit(
            &fixture.batch.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_BATCH_RETIRED ||
        atomic_load_explicit(
            &fixture.runtime.fatal_errno,
            memory_order_acquire) != 0 ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 0U) {
        fprintf(stderr, "local cancel retirement mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_terminal_cancel_submit_is_not_requeued(void) {
    queue_fixture_t fixture;
    llam_linux_native_batch_t *cancelled;
    struct io_uring_sqe sqes[2];
    unsigned completions = 0U;
    unsigned submit_calls = 0U;
    unsigned sq_head;
    unsigned i;

    if (prepare_dispatch_fixture(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            2U,
            &completions,
            sqes,
            2U,
            &sq_head) != 0) {
        perror("terminal cancel fixture init");
        return 1;
    }
    fixture.node.linux_submit_override =
        fail_linux_ring_permanently;
    fixture.node.linux_submit_override_arg = &submit_calls;
    atomic_store_explicit(
        &fixture.req.abort_reason,
        LLAM_IO_ABORT_CANCEL,
        memory_order_release);
    if (!llam_linux_native_batch_request_cancel(
            &fixture.node,
            &fixture.batch,
            &fixture.req)) {
        fprintf(stderr, "terminal cancel request mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    cancelled = llam_linux_native_cancel_take_all(
        &fixture.node);
    if (cancelled != &fixture.batch ||
        llam_linux_native_batch_submit_cancel(
            &fixture.node, cancelled) != 0U ||
        submit_calls != 1U ||
        !fixture.node.linux_submit_terminal ||
        fixture.node.native_cancel_head != NULL ||
        fixture.node.native_cancel_tail != NULL ||
        atomic_load_explicit(
            &fixture.batch.cancel_state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_CANCEL_RETIRED ||
        fixture.batch.cancel_sqes_prepared != 0U ||
        fixture.batch.cancel_cqes_observed != 0U ||
        completions != 0U) {
        fprintf(stderr, "terminal cancel was requeued\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    for (i = 0U; i < fixture.segment.op_count; i += 1U) {
        llam_linux_native_segment_handle_cqe(
            &fixture.node,
            &fixture.segment.tokens[i],
            (int)fixture.ops[i].length);
    }
    if (completions != 1U ||
        atomic_load_explicit(
            &fixture.batch.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_BATCH_RETIRED ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 0U) {
        fprintf(stderr, "terminal cancel retirement mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_chain_capacity_failure_consumes_no_sqe(void) {
    queue_fixture_t fixture;
    llam_linux_native_batch_t *taken;
    struct io_uring_sqe sqes[4];
    unsigned completions = 0U;
    unsigned submit_calls = 0U;
    unsigned sq_head;
    unsigned tail_before;
    unsigned i;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            4U,
            &completions) != 0) {
        perror("capacity queue fixture init");
        return 1;
    }
    init_userspace_sq_fixture(
        &fixture.node.ring, sqes, &sq_head, 4U);
    fixture.node.ring_ready = true;
    for (i = 0U; i < 2U; i += 1U) {
        struct io_uring_sqe *sqe =
            io_uring_get_sqe(&fixture.node.ring);

        if (sqe == NULL) {
            fprintf(stderr, "failed to fill native capacity fixture\n");
            queue_fixture_destroy(&fixture);
            return 1;
        }
        io_uring_prep_nop(sqe);
    }
    tail_before = fixture.node.ring.sq.sqe_tail;
    fixture.node.linux_submit_override = keep_linux_ring_full;
    fixture.node.linux_submit_override_arg = &submit_calls;

    if (!llam_linux_native_batch_enqueue(
            &fixture.node,
            &fixture.batch,
            &fixture.req)) {
        perror("enqueue before capacity failure");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    taken = llam_linux_native_batch_take_all(&fixture.node);
    if (taken != &fixture.batch ||
        llam_linux_native_batch_submit_one(
            &fixture.node, taken) != 0U ||
        fixture.node.ring.sq.sqe_tail != tail_before ||
        fixture.segment.prepared_sqes != 0U ||
        fixture.req.error_code != EAGAIN ||
        completions != 1U ||
        submit_calls != 1U ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.segment.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_RETIRED) {
        fprintf(stderr, "native capacity failure was not atomic\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_chain_prepares_all_sqes(void) {
    queue_fixture_t fixture;
    llam_linux_native_batch_t *taken;
    struct io_uring_sqe sqes[8];
    unsigned completions = 0U;
    unsigned sq_head;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            4U,
            &completions) != 0) {
        perror("prepare queue fixture init");
        return 1;
    }
    init_userspace_sq_fixture(
        &fixture.node.ring, sqes, &sq_head, 8U);
    fixture.node.ring_ready = true;
    if (!llam_linux_native_batch_enqueue(
            &fixture.node,
            &fixture.batch,
            &fixture.req)) {
        perror("enqueue before chain preparation");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    taken = llam_linux_native_batch_take_all(&fixture.node);
    if (taken != &fixture.batch ||
        llam_linux_native_batch_submit_one(
            &fixture.node, taken) != 4U ||
        io_uring_sq_ready(&fixture.node.ring) != 4U ||
        fixture.segment.prepared_sqes != 4U ||
        completions != 0U ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.segment.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT) {
        fprintf(stderr, "native chain was not prepared atomically\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_link_dispatch_wakes_only_after_final_cqe(void) {
    queue_fixture_t fixture;
    struct io_uring_sqe sqes[8];
    unsigned completions = 0U;
    unsigned sq_head;
    unsigned i;

    if (prepare_dispatch_fixture(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            4U,
            &completions,
            sqes,
            8U,
            &sq_head) != 0) {
        perror("link dispatch fixture init");
        return 1;
    }
    for (i = 0U; i < 3U; i += 1U) {
        llam_linux_native_segment_handle_cqe(
            &fixture.node,
            &fixture.segment.tokens[i],
            (int)fixture.ops[i].length);
        if (completions != 0U ||
            fixture.segment.terminal_wakes != 0U ||
            atomic_load_explicit(
                &fixture.node.pending_ops,
                memory_order_acquire) != 1U ||
            atomic_load_explicit(
                &fixture.shard.inflight_io_waiters,
                memory_order_acquire) != 1U) {
            fprintf(stderr, "link dispatch woke before final CQE\n");
            queue_fixture_destroy(&fixture);
            return 1;
        }
    }
    llam_linux_native_segment_handle_cqe(
        &fixture.node,
        &fixture.segment.tokens[3],
        (int)fixture.ops[3].length);
    if (completions != 1U ||
        fixture.segment.terminal_wakes != 1U ||
        fixture.req.result != (int)fixture.ops[3].length ||
        fixture.req.error_code != 0 ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 0U) {
        fprintf(stderr, "link final dispatch ownership mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_skip_dispatch_wakes_once_on_final_success(void) {
    queue_fixture_t fixture;
    struct io_uring_sqe sqes[8];
    unsigned completions = 0U;
    unsigned sq_head;

    if (prepare_dispatch_fixture(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            4U,
            &completions,
            sqes,
            8U,
            &sq_head) != 0) {
        perror("skip success dispatch fixture init");
        return 1;
    }
    llam_linux_native_segment_handle_cqe(
        &fixture.node,
        &fixture.segment.tokens[3],
        (int)fixture.ops[3].length);
    if (completions != 1U ||
        fixture.segment.terminal_wakes != 1U ||
        fixture.segment.observed_cqes != 1U ||
        fixture.segment.suppressed_success_cqes != 3U ||
        fixture.req.result != (int)fixture.ops[3].length ||
        fixture.req.error_code != 0 ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 0U) {
        fprintf(stderr, "skip final dispatch ownership mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_skip_dispatch_waits_for_target_retirement(void) {
    queue_fixture_t fixture;
    struct io_uring_sqe sqes[8];
    unsigned completions = 0U;
    unsigned sq_head;

    if (prepare_dispatch_fixture(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            4U,
            &completions,
            sqes,
            8U,
            &sq_head) != 0) {
        perror("skip error dispatch fixture init");
        return 1;
    }
    llam_linux_native_segment_handle_cqe(
        &fixture.node,
        &fixture.segment.tokens[1],
        -ECONNRESET);
    if (completions != 0U ||
        fixture.segment.terminal_wakes != 0U ||
        fixture.segment.first_error_index != 1U ||
        fixture.segment.observed_cqes != 1U ||
        fixture.segment.suppressed_success_cqes != 1U ||
        atomic_load_explicit(
            &fixture.segment.semantic_claimed,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.segment.target_retired,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.segment.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_RETIRING ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 1U) {
        fprintf(stderr, "skip first error released ownership early\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    llam_linux_native_segment_handle_cqe(
        &fixture.node,
        &fixture.segment.tokens[2],
        -ECANCELED);
    if (completions != 0U ||
        fixture.segment.terminal_wakes != 0U) {
        fprintf(stderr, "skip cancellation released ownership early\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    llam_linux_native_segment_handle_cqe(
        &fixture.node,
        &fixture.segment.tokens[3],
        -ECANCELED);
    if (completions != 1U ||
        fixture.segment.terminal_wakes != 1U ||
        fixture.segment.observed_cqes != 3U ||
        fixture.req.result != -1 ||
        fixture.req.error_code != ECONNRESET ||
        atomic_load_explicit(
            &fixture.segment.target_retired,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.segment.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_RETIRED ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 0U) {
        fprintf(stderr, "skip target retirement ownership mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_batch_dispatch_wakes_after_all_segment_tails(void) {
    queue_fixture_t fixture;
    llam_linux_native_segment_t second;
    llam_linux_native_op_t
        second_ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char
        second_buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    struct io_uring_sqe sqes[8];
    llam_linux_native_batch_t *taken;
    unsigned completions = 0U;
    unsigned sq_head;
    unsigned i;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            2U,
            &completions) != 0 ||
        configure_batch_peer(
            &fixture,
            &second,
            second_ops,
            second_buffers,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) != 0) {
        perror("batch dispatch fixture init");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    initialize_batch(
        &fixture.batch,
        &fixture.runtime,
        &fixture.segment,
        &second,
        2U);
    init_userspace_sq_fixture(
        &fixture.node.ring, sqes, &sq_head, 8U);
    fixture.node.ring_ready = true;
    if (!llam_linux_native_batch_enqueue(
            &fixture.node,
            &fixture.batch,
            &fixture.req)) {
        perror("batch dispatch enqueue");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    taken = llam_linux_native_batch_take_all(
        &fixture.node);
    if (taken != &fixture.batch ||
        llam_linux_native_batch_submit_one(
            &fixture.node, taken) != 4U) {
        fprintf(stderr, "batch dispatch submit mismatch\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    for (i = 0U; i < fixture.segment.op_count; i += 1U) {
        llam_linux_native_segment_handle_cqe(
            &fixture.node,
            &fixture.segment.tokens[i],
            (int)fixture.segment.ops[i].length);
    }
    if (completions != 0U ||
        fixture.batch.retired_segments != 1U ||
        fixture.segment.batch != &fixture.batch ||
        second.batch != &fixture.batch ||
        atomic_load_explicit(
            &fixture.batch.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_BATCH_RETIRING ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 1U) {
        fprintf(stderr, "batch woke after first segment tail\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    for (i = 0U; i < second.op_count; i += 1U) {
        llam_linux_native_segment_handle_cqe(
            &fixture.node,
            &second.tokens[i],
            (int)second.ops[i].length);
    }
    if (completions != 1U ||
        fixture.batch.retired_segments != 2U ||
        fixture.segment.batch != NULL ||
        second.batch != NULL ||
        fixture.segment.terminal_wakes +
                second.terminal_wakes !=
            1U ||
        atomic_load_explicit(
            &fixture.batch.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_BATCH_RETIRED ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 0U) {
        fprintf(stderr, "batch did not wake once after all tails\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_dispatch_rejects_duplicate_terminal_wake(void) {
    queue_fixture_t fixture;
    struct io_uring_sqe sqes[2];
    unsigned completions = 0U;
    unsigned sq_head;

    if (prepare_dispatch_fixture(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            1U,
            &completions,
            sqes,
            2U,
            &sq_head) != 0) {
        perror("duplicate dispatch fixture init");
        return 1;
    }
    llam_linux_native_segment_handle_cqe(
        &fixture.node,
        &fixture.segment.tokens[0],
        (int)fixture.ops[0].length);
    llam_linux_native_segment_handle_cqe(
        &fixture.node,
        &fixture.segment.tokens[0],
        (int)fixture.ops[0].length);
    if (completions != 1U ||
        fixture.segment.terminal_wakes != 1U ||
        atomic_load_explicit(
            &fixture.runtime.fatal_errno,
            memory_order_acquire) != EPROTO ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 0U) {
        fprintf(stderr, "duplicate terminal dispatch was not failed closed\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_dispatch_records_fatal_for_stale_token(void) {
    queue_fixture_t fixture;
    llam_linux_native_token_t stale;
    struct io_uring_sqe sqes[2];
    unsigned completions = 0U;
    unsigned sq_head;

    if (prepare_dispatch_fixture(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            1U,
            &completions,
            sqes,
            2U,
            &sq_head) != 0) {
        perror("stale dispatch fixture init");
        return 1;
    }
    stale = fixture.segment.tokens[0];
    stale.generation += 1U;
    llam_linux_native_segment_handle_cqe(
        &fixture.node,
        &stale,
        (int)fixture.ops[0].length);
    if (completions != 0U ||
        fixture.segment.terminal_wakes != 0U ||
        atomic_load_explicit(
            &fixture.runtime.fatal_errno,
            memory_order_acquire) != EPROTO ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.shard.inflight_io_waiters,
            memory_order_acquire) != 1U) {
        fprintf(stderr, "stale dispatch token was not failed closed\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_skip_mode_rejects_missing_feature(void) {
    queue_fixture_t fixture;
    unsigned completions = 0U;

    if (queue_fixture_init(
            &fixture,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            4U,
            &completions) != 0) {
        perror("feature queue fixture init");
        return 1;
    }
    fixture.node.linux_ring_features = 0U;
    errno = 0;
    if (llam_linux_native_batch_enqueue(
            &fixture.node,
            &fixture.batch,
            &fixture.req) ||
        errno != ENOTSUP ||
        fixture.node.native_batch_head != NULL ||
        atomic_load_explicit(
            &fixture.node.pending_ops,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.segment.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_IDLE) {
        fprintf(stderr, "missing CQE skip feature was not rejected\n");
        queue_fixture_destroy(&fixture);
        return 1;
    }
    queue_fixture_destroy(&fixture);
    return 0;
}

static int test_validates_configuration(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    unsigned valid_counts[] = {1U, 2U, 4U, 8U};
    size_t i;

    for (i = 0U;
         i < sizeof(valid_counts) / sizeof(valid_counts[0]);
         i += 1U) {
        fill_operations(ops, buffers, valid_counts[i]);
        if (llam_linux_native_segment_configure(
                &segment,
                ops,
                valid_counts[i],
                LLAM_LINUX_NATIVE_SEGMENT_LINK) != 0) {
            fprintf(
                stderr,
                "valid operation count %u was rejected\n",
                valid_counts[i]);
            return 1;
        }
    }

    fill_operations(ops, buffers, 4U);
    errno = 0;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            3U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) == 0 ||
        errno != EINVAL) {
        fprintf(stderr, "invalid operation count was accepted\n");
        return 1;
    }
    ops[0].kind = LLAM_LINUX_NATIVE_OP_RECV;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            4U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) == 0) {
        fprintf(stderr, "nonterminal receive was accepted\n");
        return 1;
    }
    fill_operations(ops, buffers, 4U);
    ops[0].fd = -1;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            4U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) == 0) {
        fprintf(stderr, "invalid descriptor was accepted\n");
        return 1;
    }
    fill_operations(ops, buffers, 4U);
    ops[0].buffer = NULL;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            4U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) == 0) {
        fprintf(stderr, "null buffer was accepted\n");
        return 1;
    }
    fill_operations(ops, buffers, 4U);
    ops[0].length = 0U;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            4U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) == 0) {
        fprintf(stderr, "zero length was accepted\n");
        return 1;
    }
    fill_operations(ops, buffers, 4U);
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            4U,
            (llam_linux_native_segment_mode_t)2) == 0) {
        fprintf(stderr, "invalid mode was accepted\n");
        return 1;
    }
    return 0;
}

static int test_encodes_recv_and_send_fields(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    struct io_uring_sqe send_sqe;
    struct io_uring_sqe recv_sqe;

    fill_operations(ops, buffers, 2U);
    ops[1].kind = LLAM_LINUX_NATIVE_OP_RECV;
    if (llam_linux_native_segment_configure(
            &segment,
            ops,
            2U,
            LLAM_LINUX_NATIVE_SEGMENT_LINK) != 0) {
        perror("configure encoding segment");
        return 1;
    }
    memset(&send_sqe, 0, sizeof(send_sqe));
    memset(&recv_sqe, 0, sizeof(recv_sqe));
    llam_linux_native_segment_prepare_sqe(
        &segment, 0U, &send_sqe);
    llam_linux_native_segment_prepare_sqe(
        &segment, 1U, &recv_sqe);

    if (send_sqe.opcode != IORING_OP_SEND ||
        send_sqe.fd != ops[0].fd ||
        send_sqe.addr != (uint64_t)(uintptr_t)ops[0].buffer ||
        send_sqe.len != ops[0].length ||
        send_sqe.msg_flags != (uint32_t)MSG_NOSIGNAL ||
        recv_sqe.opcode != IORING_OP_RECV ||
        recv_sqe.fd != ops[1].fd ||
        recv_sqe.addr != (uint64_t)(uintptr_t)ops[1].buffer ||
        recv_sqe.len != ops[1].length ||
        recv_sqe.msg_flags != 0U) {
        fprintf(stderr, "native SQE operation fields mismatch\n");
        return 1;
    }
    return 0;
}

static int test_encodes_link_and_skip_flags(void) {
    llam_linux_native_segment_t link;
    llam_linux_native_segment_t skip;
    llam_linux_native_op_t link_ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    llam_linux_native_op_t skip_ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char link_buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    unsigned char skip_buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    struct io_uring_sqe sqe;
    unsigned i;

    if (configure_segment(
            &link,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            4U,
            link_ops,
            link_buffers) != 0 ||
        configure_segment(
            &skip,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            4U,
            skip_ops,
            skip_buffers) != 0) {
        perror("configure flag segment");
        return 1;
    }

    for (i = 0U; i < 4U; i += 1U) {
        unsigned expected_link =
            i == 3U ? 0U : IOSQE_IO_LINK;
        unsigned expected_skip =
            i == 3U
                ? 0U
                : IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;

        memset(&sqe, 0, sizeof(sqe));
        llam_linux_native_segment_prepare_sqe(
            &link, i, &sqe);
        if (sqe.flags != expected_link) {
            fprintf(
                stderr,
                "link flags mismatch index=%u actual=%u\n",
                i,
                (unsigned)sqe.flags);
            return 1;
        }
        memset(&sqe, 0, sizeof(sqe));
        llam_linux_native_segment_prepare_sqe(
            &skip, i, &sqe);
        if (sqe.flags != expected_skip) {
            fprintf(
                stderr,
                "skip flags mismatch index=%u actual=%u\n",
                i,
                (unsigned)sqe.flags);
            return 1;
        }
    }
    return 0;
}

static int test_encodes_aligned_generation_token(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    struct io_uring_sqe sqe;
    llam_linux_native_token_t *decoded;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            1U,
            ops,
            buffers) != 0) {
        perror("configure token segment");
        return 1;
    }
    memset(&sqe, 0, sizeof(sqe));
    llam_linux_native_segment_prepare_sqe(
        &segment, 0U, &sqe);
    decoded = llam_io_udata_ptr(sqe.user_data);

    if (llam_io_udata_tag(sqe.user_data) !=
            LLAM_IO_UDATA_NATIVE_SEGMENT ||
        decoded != &segment.tokens[0] ||
        decoded->owner != &segment ||
        decoded->generation != UINT64_C(42) ||
        decoded->operation_index != 0U ||
        ((uintptr_t)decoded & (uintptr_t)7U) != 0U) {
        fprintf(stderr, "native SQE token mismatch\n");
        return 1;
    }
    return 0;
}

static int test_link_waits_for_final_cqe(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    int terminal_result = INT_MIN;
    unsigned i;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            4U,
            ops,
            buffers) != 0) {
        perror("configure link completion segment");
        return 1;
    }
    for (i = 0U; i < 3U; i += 1U) {
        if (llam_linux_native_segment_apply_cqe(
                &segment,
                &segment.tokens[i],
                (int)ops[i].length,
                &terminal_result) !=
                LLAM_LINUX_NATIVE_CQE_CONTINUE) {
            fprintf(stderr, "link completed before final CQE\n");
            return 1;
        }
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[3],
            (int)ops[3].length,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_RETIRED_OK ||
        terminal_result != (int)ops[3].length ||
        segment.observed_cqes != 4U ||
        atomic_load_explicit(
            &segment.state, memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_RETIRED ||
        atomic_load_explicit(
            &segment.semantic_claimed,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &segment.target_retired,
            memory_order_acquire) != 1U) {
        fprintf(stderr, "link final completion mismatch\n");
        return 1;
    }
    return 0;
}

static int test_link_preserves_first_non_cancel_error(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    static const int results[4] = {
        16,
        -EPIPE,
        -ECANCELED,
        -ECANCELED,
    };
    int terminal_result = INT_MIN;
    unsigned i;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            4U,
            ops,
            buffers) != 0) {
        perror("configure link error segment");
        return 1;
    }
    for (i = 0U; i < 4U; i += 1U) {
        llam_linux_native_cqe_action_t action =
            llam_linux_native_segment_apply_cqe(
                &segment,
                &segment.tokens[i],
                results[i],
                &terminal_result);

        if (i == 0U &&
            action != LLAM_LINUX_NATIVE_CQE_CONTINUE) {
            fprintf(stderr, "link error completed out of order\n");
            return 1;
        }
        if (i == 1U &&
            action != LLAM_LINUX_NATIVE_CQE_SEMANTIC) {
            fprintf(stderr, "link error did not publish semantics\n");
            return 1;
        }
        if (i == 2U &&
            action != LLAM_LINUX_NATIVE_CQE_CONTINUE) {
            fprintf(stderr, "link cancellation completed early\n");
            return 1;
        }
        if (i == 3U &&
            action != LLAM_LINUX_NATIVE_CQE_RETIRED_ERROR) {
            fprintf(stderr, "link error did not complete at tail\n");
            return 1;
        }
    }
    if (terminal_result != -EPIPE ||
        segment.first_error_index != 1U ||
        segment.first_error != EPIPE) {
        fprintf(stderr, "link lost first non-cancel error\n");
        return 1;
    }
    return 0;
}

static int test_skip_success_completes_on_final_cqe(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    int terminal_result = INT_MIN;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            4U,
            ops,
            buffers) != 0) {
        perror("configure skip success segment");
        return 1;
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[3],
            (int)ops[3].length,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_RETIRED_OK ||
        terminal_result != (int)ops[3].length ||
        segment.observed_cqes != 1U ||
        segment.suppressed_success_cqes != 3U ||
        atomic_load_explicit(
            &segment.target_retired,
            memory_order_acquire) != 1U) {
        fprintf(stderr, "skip success accounting mismatch\n");
        return 1;
    }
    return 0;
}

static int test_skip_intermediate_failure_waits_for_retirement(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    int terminal_result = INT_MIN;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            4U,
            ops,
            buffers) != 0) {
        perror("configure skip failure segment");
        return 1;
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[1],
            -ECONNRESET,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_SEMANTIC ||
        terminal_result != -ECONNRESET ||
        segment.first_error_index != 1U ||
        segment.suppressed_success_cqes != 1U ||
        segment.observed_cqes != 1U ||
        atomic_load_explicit(
            &segment.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_RETIRING ||
        atomic_load_explicit(
            &segment.semantic_claimed,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &segment.target_retired,
            memory_order_acquire) != 0U) {
        fprintf(stderr, "skip intermediate failure mismatch\n");
        return 1;
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[2],
            -ECANCELED,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_CONTINUE ||
        segment.first_error_index != 1U ||
        segment.suppressed_success_cqes != 1U ||
        segment.observed_cqes != 2U) {
        fprintf(stderr, "late skip cancellation was not retained\n");
        return 1;
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[3],
            -ECANCELED,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_RETIRED_ERROR ||
        terminal_result != -ECONNRESET ||
        segment.first_error_index != 1U ||
        segment.observed_cqes != 3U ||
        segment.observed_operation_mask != UINT64_C(0x0e) ||
        atomic_load_explicit(
            &segment.target_retired,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &segment.state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_SEGMENT_RETIRED) {
        fprintf(stderr, "skip target did not retire at tail\n");
        return 1;
    }
    return 0;
}

static int test_skip_rejects_duplicate_operation_cqe(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    int terminal_result = INT_MIN;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            4U,
            ops,
            buffers) != 0) {
        perror("configure duplicate operation segment");
        return 1;
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[1],
            -ECONNRESET,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_SEMANTIC ||
        llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[1],
            -ECONNRESET,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_FATAL ||
        segment.observed_cqes != 1U ||
        segment.observed_operation_mask != UINT64_C(0x02)) {
        fprintf(stderr, "duplicate operation CQE was not rejected\n");
        return 1;
    }
    return 0;
}

static int test_skip_out_of_order_errors_choose_lowest_index(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    int terminal_result = INT_MIN;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            4U,
            ops,
            buffers) != 0) {
        perror("configure out-of-order segment");
        return 1;
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[2],
            -EPIPE,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_SEMANTIC ||
        llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[1],
            -ECONNRESET,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_CONTINUE ||
        llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[3],
            -ECANCELED,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_RETIRED_ERROR ||
        terminal_result != -ECONNRESET ||
        segment.first_error_index != 1U ||
        segment.first_error != ECONNRESET ||
        segment.observed_cqes != 3U ||
        segment.suppressed_success_cqes != 1U) {
        fprintf(stderr, "out-of-order errors lost lowest index\n");
        return 1;
    }
    return 0;
}

static int test_final_short_success_becomes_emsgsize(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    int terminal_result = INT_MIN;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            4U,
            ops,
            buffers) != 0) {
        perror("configure short success segment");
        return 1;
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[3],
            (int)ops[3].length - 1,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_RETIRED_ERROR ||
        terminal_result != -EMSGSIZE ||
        segment.first_error_index != 3U ||
        segment.first_error != EMSGSIZE ||
        segment.suppressed_success_cqes != 3U) {
        fprintf(stderr, "short exact result was not rejected\n");
        return 1;
    }
    return 0;
}

static int test_stale_generation_is_fatal(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    llam_linux_native_token_t stale;
    int terminal_result = INT_MIN;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            1U,
            ops,
            buffers) != 0) {
        perror("configure stale token segment");
        return 1;
    }
    stale = segment.tokens[0];
    stale.generation = UINT64_C(41);
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &stale,
            (int)ops[0].length,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_FATAL ||
        segment.observed_cqes != 0U ||
        atomic_load_explicit(
            &segment.terminal_claimed,
            memory_order_acquire) != 0U) {
        fprintf(stderr, "stale generation was not failed closed\n");
        return 1;
    }
    return 0;
}

static int test_foreign_owner_is_fatal(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_segment_t foreign;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    llam_linux_native_token_t token;
    int terminal_result = INT_MIN;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK,
            1U,
            ops,
            buffers) != 0) {
        perror("configure foreign token segment");
        return 1;
    }
    memset(&foreign, 0, sizeof(foreign));
    token = segment.tokens[0];
    token.owner = &foreign;
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &token,
            (int)ops[0].length,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_FATAL ||
        segment.observed_cqes != 0U) {
        fprintf(stderr, "foreign token owner was not rejected\n");
        return 1;
    }
    return 0;
}

static int test_duplicate_terminal_is_fatal(void) {
    llam_linux_native_segment_t segment;
    llam_linux_native_op_t ops[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned char buffers[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS][32];
    int terminal_result = INT_MIN;

    if (configure_segment(
            &segment,
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP,
            1U,
            ops,
            buffers) != 0) {
        perror("configure duplicate terminal segment");
        return 1;
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[0],
            (int)ops[0].length,
            &terminal_result) !=
        LLAM_LINUX_NATIVE_CQE_RETIRED_OK) {
        fprintf(stderr, "first terminal CQE did not complete\n");
        return 1;
    }
    if (llam_linux_native_segment_apply_cqe(
            &segment,
            &segment.tokens[0],
            (int)ops[0].length,
            &terminal_result) !=
            LLAM_LINUX_NATIVE_CQE_FATAL ||
        segment.observed_cqes != 1U) {
        fprintf(stderr, "duplicate terminal CQE was not rejected\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    static const test_case_t unit_tests[] = {
        {"resource slots attach and detach",
         test_resource_slots_attach_and_detach},
        {"resource attach rolls back updates",
         test_resource_attach_rolls_back_updates},
        {"resource attach reports capacity",
         test_resource_attach_reports_capacity},
        {"validate configuration",
         test_validates_configuration},
        {"encode recv and send fields",
         test_encodes_recv_and_send_fields},
        {"encode link and skip flags",
         test_encodes_link_and_skip_flags},
        {"encode aligned generation token",
         test_encodes_aligned_generation_token},
        {"link waits for final CQE",
         test_link_waits_for_final_cqe},
        {"link preserves first non-cancel error",
         test_link_preserves_first_non_cancel_error},
        {"skip success completes on final CQE",
         test_skip_success_completes_on_final_cqe},
        {"skip intermediate failure waits for retirement",
         test_skip_intermediate_failure_waits_for_retirement},
        {"skip rejects duplicate operation CQE",
         test_skip_rejects_duplicate_operation_cqe},
        {"skip out-of-order errors choose lowest index",
         test_skip_out_of_order_errors_choose_lowest_index},
        {"final short success becomes EMSGSIZE",
         test_final_short_success_becomes_emsgsize},
        {"stale generation is fatal",
         test_stale_generation_is_fatal},
        {"foreign owner is fatal",
         test_foreign_owner_is_fatal},
        {"duplicate terminal is fatal",
         test_duplicate_terminal_is_fatal},
    };
    static const test_case_t queue_tests[] = {
        {"enqueue publishes once",
         test_enqueue_publishes_once},
        {"enqueue rejects foreign runtime",
         test_enqueue_rejects_foreign_runtime},
        {"enqueue rejects non-idle segment",
         test_enqueue_rejects_non_idle_segment},
        {"take all marks request inflight once",
         test_take_all_marks_request_inflight_once},
        {"batch enqueue publishes width two once",
         test_batch_enqueue_publishes_width_two_once},
        {"batch enqueue rejects invalid groups",
         test_batch_enqueue_rejects_invalid_groups},
        {"batch abort queued detaches exact ticket",
         test_batch_abort_queued_detaches_exact_ticket},
        {"chain capacity failure consumes no SQE",
         test_chain_capacity_failure_consumes_no_sqe},
        {"chain prepares all SQEs",
         test_chain_prepares_all_sqes},
        {"skip mode rejects missing feature",
         test_skip_mode_rejects_missing_feature},
    };
    static const test_case_t dispatch_tests[] = {
        {"link dispatch wakes only after final CQE",
         test_link_dispatch_wakes_only_after_final_cqe},
        {"skip dispatch wakes once on final success",
         test_skip_dispatch_wakes_once_on_final_success},
        {"skip dispatch waits for target retirement",
         test_skip_dispatch_waits_for_target_retirement},
        {"batch dispatch wakes after all segment tails",
         test_batch_dispatch_wakes_after_all_segment_tails},
        {"cancel and target orderings retire once",
         test_cancel_and_target_orderings_retire_once},
        {"cancel SQEs target operation tokens",
         test_cancel_sqes_target_operation_tokens},
        {"cancel rejects stale and duplicate tokens",
         test_cancel_rejects_stale_and_duplicate_tokens},
        {"local cancel retirement ignores prior statistics",
         test_cancel_local_retirement_ignores_prior_statistics},
        {"terminal cancel submit is not requeued",
         test_terminal_cancel_submit_is_not_requeued},
        {"dispatch rejects duplicate terminal wake",
         test_dispatch_rejects_duplicate_terminal_wake},
        {"dispatch records fatal for stale token",
         test_dispatch_records_fatal_for_stale_token},
    };
    bool run_unit = true;
    bool run_queue = true;
    bool run_dispatch = true;
    size_t i;

    if (argc == 2 && strcmp(argv[1], "--unit-only") == 0) {
        run_queue = false;
        run_dispatch = false;
    } else if (argc == 2 && strcmp(argv[1], "--queue") == 0) {
        run_unit = false;
        run_dispatch = false;
    } else if (argc == 2 && strcmp(argv[1], "--dispatch") == 0) {
        run_unit = false;
        run_queue = false;
    } else if (argc != 1) {
        fputs(
            "usage: test_leir_native_linux "
            "[--unit-only|--queue|--dispatch]\n",
            stderr);
        return 2;
    }
    if (run_unit) {
        for (i = 0U;
             i < sizeof(unit_tests) / sizeof(unit_tests[0]);
             i += 1U) {
            if (unit_tests[i].run() != 0) {
                fprintf(stderr, "FAIL: %s\n", unit_tests[i].name);
                return 1;
            }
        }
    }
    if (run_queue) {
        for (i = 0U;
             i < sizeof(queue_tests) / sizeof(queue_tests[0]);
             i += 1U) {
            if (queue_tests[i].run() != 0) {
                fprintf(stderr, "FAIL: %s\n", queue_tests[i].name);
                return 1;
            }
        }
    }
    if (run_dispatch) {
        for (i = 0U;
             i < sizeof(dispatch_tests) / sizeof(dispatch_tests[0]);
             i += 1U) {
            if (dispatch_tests[i].run() != 0) {
                fprintf(
                    stderr,
                    "FAIL: %s\n",
                    dispatch_tests[i].name);
                return 1;
            }
        }
    }
    puts("LEIR native Linux tests passed");
    return 0;
}

#endif
