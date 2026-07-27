#include "runtime_internal.h"
#include "leir_native_plan.h"
#include "leir_native_segment.h"
#include "leir_peer_process.h"
#include "leir_phase0_internal.h"
#include "leir_test_support.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PIPELINE_BLOCK_COUNT 16U
#define PIPELINE_BLOCKS_PER_MODE (PIPELINE_BLOCK_COUNT / 2U)
#define PIPELINE_WARMUP_TRANSACTIONS 1U
#define PIPELINE_MAX_ACTIVATIONS UINT64_C(10000000)
#define PIPELINE_MAX_MIN_MODE_MS UINT64_C(60000)

enum {
    PIPELINE_FD_SLOT = 0U,
    PIPELINE_LENGTH_SLOT = 1U,
    PIPELINE_BUFFER_SLOT = 2U,
    PIPELINE_RECV_RESULT_SLOT = 3U,
    PIPELINE_SEND_RESULT_SLOT = 4U,
    PIPELINE_SLOT_COUNT = 5U,
};

typedef enum pipeline_candidate {
    PIPELINE_CANDIDATE_LINK = 0,
    PIPELINE_CANDIDATE_LINK_SKIP = 1,
    PIPELINE_CANDIDATE_FIXED_LINK_SKIP = 2,
} pipeline_candidate_t;

typedef enum pipeline_mode {
    PIPELINE_MODE_BASELINE = 0,
    PIPELINE_MODE_CANDIDATE = 1,
} pipeline_mode_t;

typedef struct pipeline_options {
    pipeline_candidate_t candidate;
    unsigned batch_width;
    unsigned concurrency;
    size_t payload;
    uint64_t activations;
    uint64_t min_mode_ns;
    leir_bench_order_t order;
} pipeline_options_t;

typedef struct pipeline_block_state pipeline_block_state_t;

typedef struct pipeline_task_arg {
    pipeline_block_state_t *state;
    unsigned connection_begin;
    unsigned connection_count;
    uint64_t rounds;
    size_t latency_begin;
    uint64_t checksum;
    uint64_t segments;
    uint64_t logical_operations;
    leir_native_metrics_t native_metrics;
    leir_native_batch_metrics_t batch_metrics;
    leir_native_metrics_t
        previous_metrics[LEIR_NATIVE_MAX_BATCH_SEGMENTS];
} pipeline_task_arg_t;

typedef struct pipeline_block_result {
    uint64_t wall_ns;
    uint64_t cpu_ns;
    uint64_t p99_ns;
    uint64_t checksum;
    uint64_t completed_transactions;
    uint64_t segments;
    uint64_t logical_operations;
    uint64_t fixed_file_attachments;
    uint64_t fixed_buffer_attachments;
    llam_runtime_stats_t runtime;
    leir_native_metrics_t native_metrics;
    leir_native_batch_metrics_t batch_metrics;
    bool ownership_valid;
} pipeline_block_result_t;

typedef struct pipeline_mode_total {
    uint64_t wall_ns;
    uint64_t cpu_ns;
    uint64_t p99_ns;
    uint64_t checksum;
    uint64_t completed_transactions;
    uint64_t segments;
    uint64_t logical_operations;
    uint64_t fixed_file_attachments;
    uint64_t fixed_buffer_attachments;
    llam_runtime_stats_t runtime;
    leir_native_metrics_t native_metrics;
    leir_native_batch_metrics_t batch_metrics;
    bool checksum_set;
    bool ownership_valid;
    unsigned blocks;
} pipeline_mode_total_t;

struct pipeline_block_state {
    const pipeline_options_t *options;
    pipeline_mode_t mode;
    leir_peer_process_t *peer;
    leir_phase0_program_t *program;
    leir_native_plan_t plan;
    unsigned char *instance_storage;
    size_t instance_stride;
    bool *instance_live;
    unsigned initialized_instances;
    unsigned char *buffers;
    uint64_t *latencies;
    size_t latency_count;
    unsigned group_count;
    unsigned effective_width;
    pipeline_task_arg_t *task_args;
    atomic_uint failures;
    atomic_uint ready_groups;
    atomic_uint measurement_started;
    atomic_uint measurement_finished;
    atomic_uint_fast64_t remaining_segments;
    uint64_t wall_started_ns;
    uint64_t wall_finished_ns;
    uint64_t cpu_started_ns;
    uint64_t cpu_finished_ns;
    llam_runtime_stats_t runtime_before;
    int first_error;
    unsigned first_connection;
    uint64_t first_round;
    char first_stage[96];
};

static const unsigned pipeline_abba_order[
    PIPELINE_BLOCK_COUNT] = {
    0U, 1U, 1U, 0U,
    0U, 1U, 1U, 0U,
    0U, 1U, 1U, 0U,
    0U, 1U, 1U, 0U,
};

enum {
    PIPELINE_OK = 0,
    PIPELINE_ERROR = -1,
    PIPELINE_UNAVAILABLE = 77,
};

static int fail_with_errno(int error) {
    errno = error;
    return -1;
}

static bool width_is_supported(unsigned value) {
    return value == 1U || value == 2U ||
           value == 4U || value == 8U;
}

static bool concurrency_is_supported(unsigned value) {
    return value == 1U || value == 4U || value == 16U;
}

static bool payload_is_supported(size_t value) {
    return value == 64U || value == 512U || value == 4096U;
}

static int parse_u64(const char *text, uint64_t *out) {
    const unsigned char *cursor =
        (const unsigned char *)text;
    uint64_t value = 0U;

    if (text == NULL || out == NULL || *text == '\0') {
        return -1;
    }
    while (*cursor != '\0') {
        unsigned digit;

        if (*cursor < (unsigned char)'0' ||
            *cursor > (unsigned char)'9') {
            return -1;
        }
        digit = (unsigned)(*cursor - (unsigned char)'0');
        if (value >
            (UINT64_MAX - (uint64_t)digit) /
                UINT64_C(10)) {
            return -1;
        }
        value = value * UINT64_C(10) + digit;
        cursor += 1;
    }
    *out = value;
    return 0;
}

typedef enum pipeline_option_id {
    PIPELINE_OPTION_CANDIDATE = 0,
    PIPELINE_OPTION_BATCH_WIDTH,
    PIPELINE_OPTION_CONCURRENCY,
    PIPELINE_OPTION_PAYLOAD,
    PIPELINE_OPTION_ACTIVATIONS,
    PIPELINE_OPTION_MIN_MODE_MS,
    PIPELINE_OPTION_ORDER,
    PIPELINE_OPTION_COUNT,
} pipeline_option_id_t;

static int option_id_from_name(
    const char *name,
    pipeline_option_id_t *out) {
    static const char *const names[PIPELINE_OPTION_COUNT] = {
        "--candidate",
        "--batch-width",
        "--concurrency",
        "--payload",
        "--activations",
        "--min-mode-ms",
        "--order",
    };
    unsigned i;

    if (name == NULL || out == NULL) {
        return -1;
    }
    for (i = 0U; i < PIPELINE_OPTION_COUNT; i += 1U) {
        if (strcmp(name, names[i]) == 0) {
            *out = (pipeline_option_id_t)i;
            return 0;
        }
    }
    return -1;
}

static int assign_option(
    pipeline_options_t *options,
    pipeline_option_id_t id,
    const char *text) {
    uint64_t value;

    if (options == NULL || text == NULL) {
        return -1;
    }
    if (id == PIPELINE_OPTION_CANDIDATE) {
        if (strcmp(text, "link") == 0) {
            options->candidate = PIPELINE_CANDIDATE_LINK;
            return 0;
        }
        if (strcmp(text, "link_skip") == 0) {
            options->candidate =
                PIPELINE_CANDIDATE_LINK_SKIP;
            return 0;
        }
        if (strcmp(text, "fixed_link_skip") == 0) {
            options->candidate =
                PIPELINE_CANDIDATE_FIXED_LINK_SKIP;
            return 0;
        }
        return -1;
    }
    if (id == PIPELINE_OPTION_ORDER) {
        if (strcmp(text, "ABBA") == 0) {
            options->order = LEIR_BENCH_ORDER_ABBA;
            return 0;
        }
        if (strcmp(text, "BAAB") == 0) {
            options->order = LEIR_BENCH_ORDER_BAAB;
            return 0;
        }
        return -1;
    }
    if (parse_u64(text, &value) != 0) {
        return -1;
    }
    switch (id) {
        case PIPELINE_OPTION_BATCH_WIDTH:
            if (value > UINT_MAX ||
                !width_is_supported((unsigned)value)) {
                return -1;
            }
            options->batch_width = (unsigned)value;
            return 0;
        case PIPELINE_OPTION_CONCURRENCY:
            if (value > UINT_MAX ||
                !concurrency_is_supported(
                    (unsigned)value)) {
                return -1;
            }
            options->concurrency = (unsigned)value;
            return 0;
        case PIPELINE_OPTION_PAYLOAD:
            if (value > SIZE_MAX ||
                !payload_is_supported((size_t)value)) {
                return -1;
            }
            options->payload = (size_t)value;
            return 0;
        case PIPELINE_OPTION_ACTIVATIONS:
            if (value == 0U ||
                value > PIPELINE_MAX_ACTIVATIONS) {
                return -1;
            }
            options->activations = value;
            return 0;
        case PIPELINE_OPTION_MIN_MODE_MS:
            if (value == 0U ||
                value > PIPELINE_MAX_MIN_MODE_MS ||
                value >
                    UINT64_MAX / UINT64_C(1000000)) {
                return -1;
            }
            options->min_mode_ns =
                value * UINT64_C(1000000);
            return 0;
        case PIPELINE_OPTION_CANDIDATE:
        case PIPELINE_OPTION_ORDER:
        case PIPELINE_OPTION_COUNT:
        default:
            return -1;
    }
}

static int parse_options(
    int argc,
    char *const argv[],
    pipeline_options_t *out) {
    bool seen[PIPELINE_OPTION_COUNT] = {false};
    pipeline_options_t options;
    int index;

    if (argv == NULL || out == NULL) {
        return -1;
    }
    memset(&options, 0, sizeof(options));
    if (argc == 1) {
        options.candidate = PIPELINE_CANDIDATE_LINK;
        options.batch_width = 1U;
        options.concurrency = 1U;
        options.payload = 64U;
        options.activations = 8U;
        options.min_mode_ns = UINT64_C(1000000);
        options.order = LEIR_BENCH_ORDER_ABBA;
        *out = options;
        return 0;
    }
    if (argc != 1 + 2 * PIPELINE_OPTION_COUNT) {
        return -1;
    }
    for (index = 1; index < argc; index += 2) {
        pipeline_option_id_t id;

        if (argv[index] == NULL ||
            argv[index + 1] == NULL ||
            option_id_from_name(argv[index], &id) != 0 ||
            seen[id] ||
            assign_option(
                &options, id, argv[index + 1]) != 0) {
            return -1;
        }
        seen[id] = true;
    }
    for (index = 0;
         index < (int)PIPELINE_OPTION_COUNT;
         index += 1) {
        if (!seen[index]) {
            return -1;
        }
    }
    *out = options;
    return 0;
}

static const char *candidate_name(
    pipeline_candidate_t candidate) {
    switch (candidate) {
        case PIPELINE_CANDIDATE_LINK:
            return "link";
        case PIPELINE_CANDIDATE_LINK_SKIP:
            return "link_skip";
        case PIPELINE_CANDIDATE_FIXED_LINK_SKIP:
            return "fixed_link_skip";
        default:
            return "unknown";
    }
}

static leir_native_mode_t candidate_mode(
    pipeline_candidate_t candidate) {
    switch (candidate) {
        case PIPELINE_CANDIDATE_LINK:
            return LEIR_NATIVE_MODE_LINK;
        case PIPELINE_CANDIDATE_LINK_SKIP:
            return LEIR_NATIVE_MODE_LINK_CQE_SKIP;
        case PIPELINE_CANDIDATE_FIXED_LINK_SKIP:
            return LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP;
        default:
            return LEIR_NATIVE_MODE_LINK;
    }
}

static bool candidate_uses_skip(
    pipeline_candidate_t candidate) {
    return candidate != PIPELINE_CANDIDATE_LINK;
}

static bool candidate_uses_fixed(
    pipeline_candidate_t candidate) {
    return candidate ==
           PIPELINE_CANDIDATE_FIXED_LINK_SKIP;
}

static uint64_t process_cpu_ns(void) {
    struct timespec value;

    if (clock_gettime(
            CLOCK_PROCESS_CPUTIME_ID, &value) != 0) {
        return 0U;
    }
    return (uint64_t)value.tv_sec *
               UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

static uint64_t rotate_checksum(
    uint64_t value,
    unsigned rotation) {
    rotation &= 63U;
    return rotation == 0U
        ? value
        : (value << rotation) |
              (value >> (64U - rotation));
}

static void native_metrics_init(
    leir_native_metrics_t *metrics) {
    memset(metrics, 0, sizeof(*metrics));
    metrics->first_error_operation = UINT16_MAX;
}

static void native_metrics_add(
    leir_native_metrics_t *total,
    const leir_native_metrics_t *sample) {
    total->activations += sample->activations;
    total->logical_operations +=
        sample->logical_operations;
    total->queue_publications +=
        sample->queue_publications;
    total->prepared_sqes += sample->prepared_sqes;
    total->observed_cqes += sample->observed_cqes;
    total->suppressed_success_cqes +=
        sample->suppressed_success_cqes;
    total->task_parks += sample->task_parks;
    total->terminal_wakes += sample->terminal_wakes;
    total->hot_allocations += sample->hot_allocations;
    if (sample->first_error_operation != UINT16_MAX) {
        total->first_error_operation =
            sample->first_error_operation;
    }
}

static int native_metrics_delta(
    const leir_native_metrics_t *after,
    const leir_native_metrics_t *before,
    leir_native_metrics_t *out) {
#define PIPELINE_DELTA(field)                                      \
    do {                                                           \
        if (after->field < before->field) {                        \
            return fail_with_errno(EPROTO);                        \
        }                                                          \
        out->field = after->field - before->field;                 \
    } while (0)

    native_metrics_init(out);
    PIPELINE_DELTA(activations);
    PIPELINE_DELTA(logical_operations);
    PIPELINE_DELTA(queue_publications);
    PIPELINE_DELTA(prepared_sqes);
    PIPELINE_DELTA(observed_cqes);
    PIPELINE_DELTA(suppressed_success_cqes);
    PIPELINE_DELTA(task_parks);
    PIPELINE_DELTA(terminal_wakes);
    PIPELINE_DELTA(hot_allocations);
    out->first_error_operation =
        after->first_error_operation;
#undef PIPELINE_DELTA
    return 0;
}

static void batch_metrics_add(
    leir_native_batch_metrics_t *total,
    const leir_native_batch_metrics_t *sample) {
    total->activations += sample->activations;
    total->segments += sample->segments;
    total->queue_publications +=
        sample->queue_publications;
    total->task_parks += sample->task_parks;
    total->terminal_wakes += sample->terminal_wakes;
    total->operation_sqes += sample->operation_sqes;
    total->operation_cqes += sample->operation_cqes;
    total->cancel_sqes += sample->cancel_sqes;
    total->cancel_cqes += sample->cancel_cqes;
    total->hot_allocations += sample->hot_allocations;
}

static int runtime_stats_delta(
    const llam_runtime_stats_t *after,
    const llam_runtime_stats_t *before,
    llam_runtime_stats_t *out) {
#define PIPELINE_RUNTIME_DELTA(field)                              \
    do {                                                           \
        if (after->field < before->field) {                        \
            return fail_with_errno(EPROTO);                        \
        }                                                          \
        out->field = after->field - before->field;                 \
    } while (0)

    memset(out, 0, sizeof(*out));
    PIPELINE_RUNTIME_DELTA(ctx_switches);
    PIPELINE_RUNTIME_DELTA(parks);
    PIPELINE_RUNTIME_DELTA(wakes);
    PIPELINE_RUNTIME_DELTA(io_submits);
    PIPELINE_RUNTIME_DELTA(io_submit_calls);
    PIPELINE_RUNTIME_DELTA(io_submit_syscalls);
    PIPELINE_RUNTIME_DELTA(io_completions);
#undef PIPELINE_RUNTIME_DELTA
    return 0;
}

static void runtime_stats_add(
    llam_runtime_stats_t *total,
    const llam_runtime_stats_t *sample) {
    total->ctx_switches += sample->ctx_switches;
    total->parks += sample->parks;
    total->wakes += sample->wakes;
    total->io_submits += sample->io_submits;
    total->io_submit_calls += sample->io_submit_calls;
    total->io_submit_syscalls +=
        sample->io_submit_syscalls;
    total->io_completions += sample->io_completions;
}

static void block_fail_at(
    pipeline_block_state_t *state,
    const char *stage,
    int error,
    unsigned connection,
    uint64_t round) {
    unsigned expected = 0U;

    if (atomic_compare_exchange_strong_explicit(
            &state->failures,
            &expected,
            1U,
            memory_order_acq_rel,
            memory_order_acquire)) {
        state->first_error = error != 0 ? error : EIO;
        state->first_connection = connection;
        state->first_round = round;
        (void)snprintf(
            state->first_stage,
            sizeof(state->first_stage),
            "%s",
            stage);
    }
    if (g_llam_tls_task != NULL) {
        (void)llam_runtime_request_stop();
    }
}

static void block_fail(
    pipeline_block_state_t *state,
    const char *stage,
    int error) {
    block_fail_at(
        state, stage, error, UINT_MAX, UINT64_MAX);
}

static leir_phase0_node_desc_t terminal_node(
    leir_phase0_opcode_t opcode,
    uint16_t result_slot) {
    leir_phase0_node_desc_t node;

    memset(&node, 0, sizeof(node));
    node.opcode = (uint16_t)opcode;
    node.fd_slot = LEIR_PHASE0_NODE_NONE;
    node.buffer_slot = LEIR_PHASE0_NODE_NONE;
    node.length_slot = LEIR_PHASE0_NODE_NONE;
    node.result_slot = result_slot;
    node.on_success = LEIR_PHASE0_NODE_NONE;
    node.on_eof = LEIR_PHASE0_NODE_NONE;
    node.on_error = LEIR_PHASE0_NODE_NONE;
    return node;
}

static int create_pipeline_program(
    leir_phase0_program_t **out) {
    leir_phase0_slot_kind_t slots[PIPELINE_SLOT_COUNT];
    leir_phase0_node_desc_t nodes[4];
    leir_phase0_program_desc_t desc;

    if (out == NULL) {
        return fail_with_errno(EINVAL);
    }
    memset(slots, 0, sizeof(slots));
    memset(nodes, 0, sizeof(nodes));
    slots[PIPELINE_FD_SLOT] = LEIR_PHASE0_SLOT_FD;
    slots[PIPELINE_LENGTH_SLOT] = LEIR_PHASE0_SLOT_U64;
    slots[PIPELINE_BUFFER_SLOT] =
        LEIR_PHASE0_SLOT_MUT_BUFFER;
    slots[PIPELINE_RECV_RESULT_SLOT] =
        LEIR_PHASE0_SLOT_I64;
    slots[PIPELINE_SEND_RESULT_SLOT] =
        LEIR_PHASE0_SLOT_I64;

    nodes[0].opcode = LEIR_PHASE0_OP_READ_EXACT;
    nodes[0].fd_slot = PIPELINE_FD_SLOT;
    nodes[0].buffer_slot = PIPELINE_BUFFER_SLOT;
    nodes[0].length_slot = PIPELINE_LENGTH_SLOT;
    nodes[0].result_slot = PIPELINE_RECV_RESULT_SLOT;
    nodes[0].on_success = 1U;
    nodes[0].on_eof = 3U;
    nodes[0].on_error = 3U;

    nodes[1].opcode = LEIR_PHASE0_OP_WRITE_ALL;
    nodes[1].fd_slot = PIPELINE_FD_SLOT;
    nodes[1].buffer_slot = PIPELINE_BUFFER_SLOT;
    nodes[1].length_slot = PIPELINE_LENGTH_SLOT;
    nodes[1].result_slot = PIPELINE_SEND_RESULT_SLOT;
    nodes[1].on_success = 2U;
    nodes[1].on_eof = 3U;
    nodes[1].on_error = 3U;
    nodes[2] = terminal_node(
        LEIR_PHASE0_OP_RETURN,
        PIPELINE_SEND_RESULT_SLOT);
    nodes[3] = terminal_node(
        LEIR_PHASE0_OP_FAIL,
        PIPELINE_RECV_RESULT_SLOT);

    memset(&desc, 0, sizeof(desc));
    desc.nodes = nodes;
    desc.slot_kinds = slots;
    desc.node_count = 4U;
    desc.slot_count = PIPELINE_SLOT_COUNT;
    desc.entry_node = 0U;
    return leir_phase0_program_create(&desc, out);
}

static leir_native_instance_t *instance_at(
    pipeline_block_state_t *state,
    unsigned connection) {
    return (leir_native_instance_t *)(
        state->instance_storage +
        (size_t)connection * state->instance_stride);
}

static unsigned char *buffer_at(
    pipeline_block_state_t *state,
    unsigned connection) {
    return state->buffers +
           (size_t)connection * state->options->payload;
}

static uint64_t payload_checksum(
    pipeline_block_state_t *state,
    unsigned connection,
    uint64_t sequence) {
    uint64_t checksum = leir_test_payload_checksum(
        buffer_at(state, connection),
        state->options->payload,
        connection,
        sequence);

    return rotate_checksum(
        checksum,
        (unsigned)(((uint64_t)connection + sequence) & 63U));
}

static int initialize_candidate_instances(
    pipeline_block_state_t *state) {
    const pipeline_options_t *options = state->options;
    size_t storage_size;
    unsigned i;

    if (create_pipeline_program(&state->program) != 0 ||
        leir_native_plan_compile(
            state->program, &state->plan) != 0) {
        return -1;
    }
    state->instance_stride =
        leir_native_instance_size();
    if (state->instance_stride == 0U ||
        state->instance_stride >
            SIZE_MAX / options->concurrency) {
        return fail_with_errno(EOVERFLOW);
    }
    storage_size =
        state->instance_stride * options->concurrency;
    state->instance_storage = calloc(1U, storage_size);
    state->instance_live = calloc(
        options->concurrency,
        sizeof(state->instance_live[0]));
    if (state->instance_storage == NULL ||
        state->instance_live == NULL) {
        return -1;
    }
    for (i = 0U; i < options->concurrency; i += 1U) {
        leir_phase0_value_t values[PIPELINE_SLOT_COUNT];
        leir_native_instance_t *instance =
            instance_at(state, i);

        memset(values, 0, sizeof(values));
        values[PIPELINE_FD_SLOT].fd =
            leir_peer_process_server_fd(state->peer, i);
        values[PIPELINE_LENGTH_SLOT].u64 =
            options->payload;
        values[PIPELINE_BUFFER_SLOT].buffer.data =
            buffer_at(state, i);
        values[PIPELINE_BUFFER_SLOT].buffer.size =
            options->payload;
        values[PIPELINE_RECV_RESULT_SLOT].i64 = -1;
        values[PIPELINE_SEND_RESULT_SLOT].i64 = -1;
        if (leir_native_instance_init(
                instance,
                state->instance_stride,
                state->program,
                &state->plan,
                candidate_mode(options->candidate)) != 0) {
            return -1;
        }
        state->instance_live[i] = true;
        state->initialized_instances = i + 1U;
        if (leir_native_instance_bind(
                instance,
                values,
                PIPELINE_SLOT_COUNT) != 0) {
            return -1;
        }
    }
    return 0;
}

static int destroy_candidate_instances(
    pipeline_block_state_t *state) {
    int first_error = 0;
    unsigned i;

    for (i = 0U;
         i < state->initialized_instances;
         i += 1U) {
        if (!state->instance_live[i]) {
            continue;
        }
        if (leir_native_instance_destroy(
                instance_at(state, i)) != 0) {
            if (first_error == 0) {
                first_error = errno != 0 ? errno : EIO;
            }
        } else {
            state->instance_live[i] = false;
        }
    }
    if (first_error != 0) {
        return fail_with_errno(first_error);
    }
    state->initialized_instances = 0U;
    return 0;
}

static bool native_backend_is_available(
    pipeline_candidate_t candidate) {
#if LLAM_RUNTIME_BACKEND_LINUX
    unsigned i;

    if (g_llam_runtime.active_nodes == 0U ||
        g_llam_runtime.nodes == NULL) {
        return false;
    }
    for (i = 0U; i < g_llam_runtime.active_nodes; i += 1U) {
        llam_node_t *node = &g_llam_runtime.nodes[i];

        if (!node->ring_ready ||
            !node->supports_recv ||
            !node->supports_send ||
            (candidate_uses_skip(candidate) &&
             (node->linux_ring_features &
              IORING_FEAT_CQE_SKIP) == 0U) ||
            (candidate_uses_fixed(candidate) &&
             (!node->supports_native_fixed_files ||
              !node->supports_native_fixed_buffers))) {
            return false;
        }
    }
    return true;
#else
    (void)candidate;
    return false;
#endif
}

static bool runtime_ownership_is_clear(void) {
    unsigned i;

    if (atomic_load_explicit(
            &g_llam_runtime.active_io_waiters,
            memory_order_acquire) != 0U) {
        return false;
    }
    for (i = 0U; i < g_llam_runtime.active_nodes; i += 1U) {
        llam_node_t *node = &g_llam_runtime.nodes[i];

        if (atomic_load_explicit(
                &node->pending_ops,
                memory_order_acquire) != 0U) {
            return false;
        }
#if LLAM_RUNTIME_BACKEND_LINUX
        {
            bool queues_empty;

            pthread_mutex_lock(&node->submit_lock);
            queues_empty =
                node->native_batch_head == NULL &&
                node->native_batch_tail == NULL &&
                node->native_cancel_head == NULL &&
                node->native_cancel_tail == NULL;
            pthread_mutex_unlock(&node->submit_lock);
            if (!queues_empty) {
                return false;
            }
        }
#endif
    }
    return true;
}

static void fixed_attachment_counts(
    uint64_t *files_out,
    uint64_t *buffers_out) {
    uint64_t files = 0U;
    uint64_t buffers = 0U;
#if LLAM_RUNTIME_BACKEND_LINUX
    unsigned i;

    for (i = 0U; i < g_llam_runtime.active_nodes; i += 1U) {
        files += (uint64_t)__builtin_popcountll(
            g_llam_runtime.nodes[i].
                native_fixed_file_bitmap);
        buffers += (uint64_t)__builtin_popcountll(
            g_llam_runtime.nodes[i].
                native_fixed_buffer_bitmap);
    }
#endif
    *files_out = files;
    *buffers_out = buffers;
}

static int run_baseline_round(
    pipeline_task_arg_t *arg,
    uint64_t round,
    bool measured) {
    pipeline_block_state_t *state = arg->state;
    unsigned lane;

    for (lane = 0U; lane < arg->connection_count; lane += 1U) {
        unsigned connection =
            arg->connection_begin + lane;
        llam_fd_t fd = leir_peer_process_server_fd(
            state->peer, connection);
        unsigned char *buffer =
            buffer_at(state, connection);
        uint64_t sequence = measured
            ? PIPELINE_WARMUP_TRANSACTIONS + round
            : 0U;

        if (leir_test_read_exact(
                fd,
                buffer,
                state->options->payload) != 0 ||
            !leir_test_payload_is_valid(
                buffer,
                state->options->payload,
                connection,
                sequence) ||
            leir_test_write_all(
                fd,
                buffer,
                state->options->payload) != 0) {
            return -1;
        }
        if (measured) {
            arg->checksum ^= payload_checksum(
                state, connection, sequence);
            arg->segments += 1U;
            arg->logical_operations += 2U;
        }
    }
    return 0;
}

static int run_candidate_round(
    pipeline_task_arg_t *arg,
    uint64_t round,
    bool measured) {
    pipeline_block_state_t *state = arg->state;
    leir_native_instance_t
        *instances[LEIR_NATIVE_MAX_BATCH_SEGMENTS];
    leir_phase0_value_t
        values[LEIR_NATIVE_MAX_BATCH_SEGMENTS]
              [PIPELINE_SLOT_COUNT];
    leir_phase0_value_t
        *values_out[LEIR_NATIVE_MAX_BATCH_SEGMENTS];
    size_t value_counts[LEIR_NATIVE_MAX_BATCH_SEGMENTS];
    leir_native_metrics_t
        metrics[LEIR_NATIVE_MAX_BATCH_SEGMENTS];
    leir_native_batch_metrics_t batch_metrics;
    unsigned lane;

    memset(values, 0, sizeof(values));
    memset(metrics, 0, sizeof(metrics));
    memset(&batch_metrics, 0, sizeof(batch_metrics));
    for (lane = 0U; lane < arg->connection_count; lane += 1U) {
        unsigned connection =
            arg->connection_begin + lane;

        instances[lane] = instance_at(state, connection);
        values_out[lane] = values[lane];
        value_counts[lane] = PIPELINE_SLOT_COUNT;
    }
    if (leir_native_batch_run(
            instances,
            values_out,
            value_counts,
            metrics,
            arg->connection_count,
            &batch_metrics) != 0) {
        return -1;
    }
    for (lane = 0U; lane < arg->connection_count; lane += 1U) {
        unsigned connection =
            arg->connection_begin + lane;
        uint64_t sequence = measured
            ? PIPELINE_WARMUP_TRANSACTIONS + round
            : 0U;
        leir_native_metrics_t delta;

        if (values[lane][
                PIPELINE_RECV_RESULT_SLOT].i64 !=
                (int64_t)state->options->payload ||
            values[lane][
                PIPELINE_SEND_RESULT_SLOT].i64 !=
                (int64_t)state->options->payload ||
            !leir_test_payload_is_valid(
                buffer_at(state, connection),
                state->options->payload,
                connection,
                sequence)) {
            return fail_with_errno(EPROTO);
        }
        if (!measured) {
            arg->previous_metrics[lane] = metrics[lane];
            continue;
        }
        if (native_metrics_delta(
                &metrics[lane],
                &arg->previous_metrics[lane],
                &delta) != 0) {
            return -1;
        }
        arg->previous_metrics[lane] = metrics[lane];
        native_metrics_add(
            &arg->native_metrics, &delta);
        arg->checksum ^= payload_checksum(
            state, connection, sequence);
        arg->segments += 1U;
        arg->logical_operations += 2U;
    }
    if (measured) {
        batch_metrics_add(
            &arg->batch_metrics, &batch_metrics);
    }
    return 0;
}

static void complete_segments(
    pipeline_block_state_t *state,
    unsigned count) {
    uint64_t previous = atomic_fetch_sub_explicit(
        &state->remaining_segments,
        count,
        memory_order_acq_rel);

    if (previous < count) {
        block_fail(
            state, "segment accounting underflow", EPROTO);
        return;
    }
    if (previous == count) {
        state->cpu_finished_ns = process_cpu_ns();
        state->wall_finished_ns = llam_now_ns();
        atomic_store_explicit(
            &state->measurement_finished,
            1U,
            memory_order_release);
    }
}

static void pipeline_worker_task(void *opaque) {
    pipeline_task_arg_t *arg = opaque;
    pipeline_block_state_t *state = arg->state;
    uint64_t round;

    if ((state->mode == PIPELINE_MODE_BASELINE
             ? run_baseline_round(arg, 0U, false)
             : run_candidate_round(arg, 0U, false)) != 0) {
        block_fail_at(
            state,
            "warmup",
            errno,
            arg->connection_begin,
            0U);
        return;
    }
    atomic_fetch_add_explicit(
        &state->ready_groups, 1U, memory_order_acq_rel);
    while (atomic_load_explicit(
               &state->measurement_started,
               memory_order_acquire) == 0U) {
        if (atomic_load_explicit(
                &state->failures,
                memory_order_acquire) != 0U) {
            return;
        }
        llam_yield();
    }

    for (round = 0U; round < arg->rounds; round += 1U) {
        uint64_t started_ns = llam_now_ns();
        int result = state->mode == PIPELINE_MODE_BASELINE
            ? run_baseline_round(arg, round, true)
            : run_candidate_round(arg, round, true);

        if (result != 0) {
            block_fail_at(
                state,
                state->mode == PIPELINE_MODE_BASELINE
                    ? "baseline pipeline"
                    : "candidate pipeline",
                errno,
                arg->connection_begin,
                round);
            return;
        }
        state->latencies[
            arg->latency_begin + (size_t)round] =
            llam_now_ns() - started_ns;
        complete_segments(state, arg->connection_count);
    }
}

static void peer_start_task(void *opaque) {
    pipeline_block_state_t *state = opaque;

    if (leir_peer_process_signal_start(state->peer) != 0) {
        block_fail(state, "peer start signal", errno);
    }
}

static void measurement_start_task(void *opaque) {
    pipeline_block_state_t *state = opaque;

    while (atomic_load_explicit(
               &state->ready_groups,
               memory_order_acquire) !=
           state->group_count) {
        if (atomic_load_explicit(
                &state->failures,
                memory_order_acquire) != 0U) {
            return;
        }
        llam_yield();
    }
    if (llam_runtime_collect_stats(
            &state->runtime_before) != 0) {
        block_fail(state, "runtime stats snapshot", errno);
        return;
    }
    state->wall_started_ns = llam_now_ns();
    state->cpu_started_ns = process_cpu_ns();
    if (state->wall_started_ns == 0U ||
        state->cpu_started_ns == 0U) {
        block_fail(state, "measurement clock", EIO);
        return;
    }
    atomic_store_explicit(
        &state->measurement_started,
        1U,
        memory_order_release);
}

static int compare_u64(
    const void *left,
    const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return a < b ? -1 : (a > b ? 1 : 0);
}

static uint64_t p99(uint64_t *samples, size_t count) {
    size_t index;

    if (samples == NULL || count == 0U) {
        return 0U;
    }
    qsort(
        samples,
        count,
        sizeof(samples[0]),
        compare_u64);
    index = (count * 99U + 99U) / 100U;
    if (index == 0U) {
        index = 1U;
    }
    if (index > count) {
        index = count;
    }
    return samples[index - 1U];
}

static bool block_integrity(
    const pipeline_options_t *options,
    pipeline_mode_t mode,
    uint64_t activations,
    uint64_t batch_activations,
    const pipeline_block_result_t *result) {
    uint64_t logical;

    if (activations > UINT64_MAX / 2U) {
        return false;
    }
    logical = activations * 2U;
    if (!result->ownership_valid ||
        result->wall_ns == 0U ||
        result->cpu_ns == 0U ||
        result->p99_ns == 0U ||
        result->completed_transactions != activations ||
        result->segments != activations ||
        result->logical_operations != logical) {
        return false;
    }
    if (mode == PIPELINE_MODE_BASELINE) {
        return result->fixed_file_attachments == 0U &&
               result->fixed_buffer_attachments == 0U;
    }
    return result->native_metrics.activations ==
               activations &&
           result->native_metrics.logical_operations ==
               logical &&
           result->native_metrics.queue_publications ==
               batch_activations &&
           result->native_metrics.prepared_sqes == logical &&
           result->native_metrics.observed_cqes ==
               (candidate_uses_skip(options->candidate)
                    ? activations
                    : logical) &&
           result->native_metrics.suppressed_success_cqes ==
               (candidate_uses_skip(options->candidate)
                    ? activations
                    : 0U) &&
           result->native_metrics.task_parks ==
               batch_activations &&
           result->native_metrics.terminal_wakes ==
               batch_activations &&
           result->native_metrics.hot_allocations == 0U &&
           result->native_metrics.first_error_operation ==
               UINT16_MAX &&
           result->batch_metrics.activations ==
               batch_activations &&
           result->batch_metrics.segments == activations &&
           result->batch_metrics.queue_publications ==
               batch_activations &&
           result->batch_metrics.task_parks ==
               batch_activations &&
           result->batch_metrics.terminal_wakes ==
               batch_activations &&
           result->batch_metrics.operation_sqes == logical &&
           result->batch_metrics.operation_cqes ==
               (candidate_uses_skip(options->candidate)
                    ? activations
                    : logical) &&
           result->batch_metrics.cancel_sqes == 0U &&
           result->batch_metrics.cancel_cqes == 0U &&
           result->batch_metrics.hot_allocations == 0U &&
           result->fixed_file_attachments ==
               (candidate_uses_fixed(options->candidate)
                    ? options->concurrency
                    : 0U) &&
           result->fixed_buffer_attachments ==
               (candidate_uses_fixed(options->candidate)
                    ? options->concurrency
                    : 0U);
}

static int run_block(
    const pipeline_options_t *options,
    pipeline_mode_t mode,
    uint64_t activations,
    pipeline_block_result_t *out) {
    pipeline_block_state_t state;
    leir_peer_config_t peer_config;
    leir_peer_result_t peer_result;
    llam_runtime_opts_t runtime_options;
    llam_runtime_stats_t runtime_after;
    llam_task_t *workers[16];
    llam_task_t *starter = NULL;
    llam_task_t *measurement = NULL;
    uint64_t rounds;
    uint64_t batch_activations;
    bool runtime_started = false;
    bool instances_destroyed = false;
    unsigned i;
    int status = PIPELINE_ERROR;

    if (options == NULL || out == NULL ||
        activations == 0U ||
        activations > PIPELINE_MAX_ACTIVATIONS ||
        activations % options->concurrency != 0U) {
        return fail_with_errno(EINVAL);
    }
    memset(&state, 0, sizeof(state));
    memset(out, 0, sizeof(*out));
    memset(workers, 0, sizeof(workers));
    native_metrics_init(&out->native_metrics);
    state.options = options;
    state.mode = mode;
    state.effective_width =
        options->batch_width < options->concurrency
            ? options->batch_width
            : options->concurrency;
    state.group_count =
        (options->concurrency + state.effective_width - 1U) /
        state.effective_width;
    rounds = activations / options->concurrency;
    if (rounds >
        SIZE_MAX / state.group_count) {
        return fail_with_errno(EOVERFLOW);
    }
    state.latency_count =
        (size_t)rounds * state.group_count;
    batch_activations = state.latency_count;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ready_groups, 0U);
    atomic_init(&state.measurement_started, 0U);
    atomic_init(&state.measurement_finished, 0U);
    atomic_init(&state.remaining_segments, activations);

    if (options->concurrency >
            SIZE_MAX / options->payload) {
        return fail_with_errno(EOVERFLOW);
    }
    state.buffers = calloc(
        options->concurrency, options->payload);
    state.latencies = calloc(
        state.latency_count,
        sizeof(state.latencies[0]));
    state.task_args = calloc(
        state.group_count,
        sizeof(state.task_args[0]));
    if (state.buffers == NULL ||
        state.latencies == NULL ||
        state.task_args == NULL) {
        errno = ENOMEM;
        goto cleanup;
    }
    memset(&peer_config, 0, sizeof(peer_config));
    peer_config.workload =
        LEIR_BENCH_WORKLOAD_SOCKET_RELAY;
    peer_config.concurrency = options->concurrency;
    peer_config.payload = options->payload;
    peer_config.activations = activations;
    peer_config.transactions_per_activation = 1U;
    peer_config.warmup_transactions_per_connection =
        PIPELINE_WARMUP_TRANSACTIONS;
    peer_config.socket_kind = LEIR_PEER_SOCKET_SEQPACKET;
    peer_config.operations_per_activation = 0U;
    if (leir_peer_process_start(
            &peer_config, &state.peer) != 0) {
        goto cleanup;
    }
    if (mode == PIPELINE_MODE_CANDIDATE &&
        initialize_candidate_instances(&state) != 0) {
        goto cleanup;
    }
    memset(&runtime_options, 0, sizeof(runtime_options));
    runtime_options.deterministic = 1U;
    runtime_options.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&runtime_options) != 0) {
        goto cleanup;
    }
    runtime_started = true;
    if (mode == PIPELINE_MODE_CANDIDATE &&
        !native_backend_is_available(options->candidate)) {
        status = PIPELINE_UNAVAILABLE;
        goto cleanup;
    }

    for (i = 0U; i < state.group_count; i += 1U) {
        pipeline_task_arg_t *arg = &state.task_args[i];
        unsigned begin = i * state.effective_width;
        unsigned remaining = options->concurrency - begin;

        arg->state = &state;
        arg->connection_begin = begin;
        arg->connection_count =
            remaining < state.effective_width
                ? remaining
                : state.effective_width;
        arg->rounds = rounds;
        arg->latency_begin = (size_t)i * (size_t)rounds;
        native_metrics_init(&arg->native_metrics);
        workers[i] = llam_spawn(
            pipeline_worker_task, arg, NULL);
        if (workers[i] == NULL) {
            block_fail(&state, "worker spawn", errno);
            goto cleanup;
        }
    }
    starter = llam_spawn(peer_start_task, &state, NULL);
    measurement = llam_spawn(
        measurement_start_task, &state, NULL);
    if (starter == NULL || measurement == NULL) {
        block_fail(&state, "support task spawn", errno);
        goto cleanup;
    }
    if (llam_run() != 0 &&
        atomic_load_explicit(
            &state.failures,
            memory_order_acquire) == 0U) {
        block_fail(&state, "runtime run", errno);
    }
    for (i = 0U; i < state.group_count; i += 1U) {
        if (workers[i] != NULL &&
            llam_join(workers[i]) != 0) {
            block_fail(&state, "worker join", errno);
        }
        workers[i] = NULL;
    }
    if (starter != NULL && llam_join(starter) != 0) {
        block_fail(&state, "peer starter join", errno);
    }
    starter = NULL;
    if (measurement != NULL &&
        llam_join(measurement) != 0) {
        block_fail(&state, "measurement join", errno);
    }
    measurement = NULL;
    if (atomic_load_explicit(
            &state.failures,
            memory_order_acquire) != 0U) {
        errno = state.first_error;
        goto cleanup;
    }
    if (atomic_load_explicit(
            &state.measurement_finished,
            memory_order_acquire) == 0U ||
        state.wall_finished_ns <= state.wall_started_ns ||
        state.cpu_finished_ns <= state.cpu_started_ns ||
        llam_runtime_collect_stats(&runtime_after) != 0 ||
        runtime_stats_delta(
            &runtime_after,
            &state.runtime_before,
            &out->runtime) != 0) {
        block_fail(&state, "measurement completion", EPROTO);
        errno = EPROTO;
        goto cleanup;
    }
    out->wall_ns =
        state.wall_finished_ns - state.wall_started_ns;
    out->cpu_ns =
        state.cpu_finished_ns - state.cpu_started_ns;
    out->p99_ns = p99(
        state.latencies, state.latency_count);
    out->ownership_valid = runtime_ownership_is_clear();
    fixed_attachment_counts(
        &out->fixed_file_attachments,
        &out->fixed_buffer_attachments);
    for (i = 0U; i < state.group_count; i += 1U) {
        pipeline_task_arg_t *arg = &state.task_args[i];

        out->checksum ^= arg->checksum;
        out->segments += arg->segments;
        out->logical_operations +=
            arg->logical_operations;
        native_metrics_add(
            &out->native_metrics,
            &arg->native_metrics);
        batch_metrics_add(
            &out->batch_metrics,
            &arg->batch_metrics);
    }
    if (leir_peer_process_finish(
            state.peer, &peer_result) != 0) {
        state.peer = NULL;
        goto cleanup;
    }
    state.peer = NULL;
    out->completed_transactions =
        peer_result.completed_transactions;
    if (peer_result.status != 0 ||
        peer_result.checksum != out->checksum ||
        !block_integrity(
            options,
            mode,
            activations,
            batch_activations,
            out)) {
        block_fail(&state, "block integrity", EPROTO);
        errno = EPROTO;
        goto cleanup;
    }
    status = PIPELINE_OK;

cleanup:
    if (status == PIPELINE_ERROR &&
        atomic_load_explicit(
            &state.failures,
            memory_order_acquire) != 0U) {
        fprintf(
            stderr,
            "LEIR pipeline block failed mode=%s stage=%s "
            "errno=%d connection=%u round=%" PRIu64 "\n",
            mode == PIPELINE_MODE_BASELINE
                ? "baseline"
                : "candidate",
            state.first_stage,
            state.first_error,
            state.first_connection,
            state.first_round);
    }
    if (state.peer != NULL) {
        leir_peer_process_abort(state.peer);
        state.peer = NULL;
    }
    if (runtime_started) {
        if (destroy_candidate_instances(&state) == 0) {
            instances_destroyed = true;
            if (status == PIPELINE_OK) {
                uint64_t files;
                uint64_t buffers;

                fixed_attachment_counts(&files, &buffers);
                if (files != 0U || buffers != 0U) {
                    status = PIPELINE_ERROR;
                    errno = EPROTO;
                }
            }
        } else if (status == PIPELINE_OK) {
            status = PIPELINE_ERROR;
        }
        llam_runtime_shutdown();
        runtime_started = false;
    }
    if (!instances_destroyed &&
        destroy_candidate_instances(&state) != 0 &&
        status == PIPELINE_OK) {
        status = PIPELINE_ERROR;
    }
    leir_phase0_program_destroy(state.program);
    free(state.instance_live);
    free(state.instance_storage);
    free(state.task_args);
    free(state.latencies);
    free(state.buffers);
    return status;
}

static void total_init(pipeline_mode_total_t *total) {
    memset(total, 0, sizeof(*total));
    native_metrics_init(&total->native_metrics);
}

static int add_u64(uint64_t *target, uint64_t value) {
    if (*target > UINT64_MAX - value) {
        return fail_with_errno(EOVERFLOW);
    }
    *target += value;
    return 0;
}

static int total_add(
    pipeline_mode_total_t *total,
    const pipeline_block_result_t *block) {
    if ((total->checksum_set &&
         total->checksum != block->checksum) ||
        add_u64(&total->wall_ns, block->wall_ns) != 0 ||
        add_u64(&total->cpu_ns, block->cpu_ns) != 0 ||
        add_u64(
            &total->completed_transactions,
            block->completed_transactions) != 0 ||
        add_u64(&total->segments, block->segments) != 0 ||
        add_u64(
            &total->logical_operations,
            block->logical_operations) != 0 ||
        add_u64(
            &total->fixed_file_attachments,
            block->fixed_file_attachments) != 0 ||
        add_u64(
            &total->fixed_buffer_attachments,
            block->fixed_buffer_attachments) != 0) {
        return -1;
    }
    total->checksum = block->checksum;
    total->checksum_set = true;
    runtime_stats_add(&total->runtime, &block->runtime);
    native_metrics_add(
        &total->native_metrics,
        &block->native_metrics);
    batch_metrics_add(
        &total->batch_metrics,
        &block->batch_metrics);
    if (block->p99_ns > total->p99_ns) {
        total->p99_ns = block->p99_ns;
    }
    total->ownership_valid =
        total->blocks == 0U
            ? block->ownership_valid
            : total->ownership_valid &&
                  block->ownership_valid;
    total->blocks += 1U;
    return 0;
}

static int normalize_activations(
    const pipeline_options_t *options,
    uint64_t value,
    uint64_t *out) {
    uint64_t remainder;

    if (value < options->concurrency) {
        value = options->concurrency;
    }
    remainder = value % options->concurrency;
    if (remainder != 0U) {
        uint64_t addition =
            options->concurrency - remainder;

        if (value > PIPELINE_MAX_ACTIVATIONS - addition) {
            return fail_with_errno(EOVERFLOW);
        }
        value += addition;
    }
    if (value > PIPELINE_MAX_ACTIVATIONS) {
        return fail_with_errno(ERANGE);
    }
    *out = value;
    return 0;
}

static int calibrate(
    const pipeline_options_t *options,
    uint64_t *activations_out) {
    uint64_t activations;
    uint64_t target_ns =
        options->min_mode_ns / 2U +
        (options->min_mode_ns % 2U != 0U ? 1U : 0U);
    unsigned iteration;

    if (normalize_activations(
            options,
            options->activations,
            &activations) != 0) {
        return PIPELINE_ERROR;
    }
    for (iteration = 0U; iteration < 12U; iteration += 1U) {
        pipeline_block_result_t baseline;
        pipeline_block_result_t candidate;
        uint64_t faster;
        uint64_t scale;
        uint64_t next;
        int result;

        result = run_block(
            options,
            PIPELINE_MODE_BASELINE,
            activations,
            &baseline);
        if (result != PIPELINE_OK) {
            return result;
        }
        result = run_block(
            options,
            PIPELINE_MODE_CANDIDATE,
            activations,
            &candidate);
        if (result != PIPELINE_OK) {
            return result;
        }
        faster = baseline.wall_ns < candidate.wall_ns
            ? baseline.wall_ns
            : candidate.wall_ns;
        if (faster >= target_ns) {
            *activations_out = activations;
            return PIPELINE_OK;
        }
        scale = faster == 0U
            ? 2U
            : (target_ns + faster - 1U) / faster;
        if (scale < 2U) {
            scale = 2U;
        }
        if (scale > 16U) {
            scale = 16U;
        }
        if (activations > PIPELINE_MAX_ACTIVATIONS / scale) {
            return fail_with_errno(ERANGE);
        }
        next = activations * scale;
        if (normalize_activations(
                options, next, &activations) != 0) {
            return PIPELINE_ERROR;
        }
    }
    return fail_with_errno(ERANGE);
}

static int run_benchmark(
    const pipeline_options_t *options) {
    pipeline_mode_total_t totals[2];
    uint64_t activations_per_block;
    uint64_t total_activations;
    uint64_t expected_batch_activations;
    uint64_t expected_logical;
    uint64_t expected_operation_cqes;
    unsigned effective_width =
        options->batch_width < options->concurrency
            ? options->batch_width
            : options->concurrency;
    unsigned block;
    int result;

    total_init(&totals[0]);
    total_init(&totals[1]);
    result = calibrate(options, &activations_per_block);
    if (result != PIPELINE_OK) {
        return result;
    }
    for (block = 0U; block < PIPELINE_BLOCK_COUNT; block += 1U) {
        pipeline_block_result_t block_result;
        unsigned mode = pipeline_abba_order[block];

        if (options->order == LEIR_BENCH_ORDER_BAAB) {
            mode ^= 1U;
        }
        result = run_block(
            options,
            (pipeline_mode_t)mode,
            activations_per_block,
            &block_result);
        if (result != PIPELINE_OK) {
            return result;
        }
        if (total_add(&totals[mode], &block_result) != 0) {
            return PIPELINE_ERROR;
        }
    }
    if (activations_per_block >
            UINT64_MAX / PIPELINE_BLOCKS_PER_MODE) {
        return fail_with_errno(EOVERFLOW);
    }
    total_activations =
        activations_per_block * PIPELINE_BLOCKS_PER_MODE;
    if (total_activations > UINT64_MAX / 2U) {
        return fail_with_errno(EOVERFLOW);
    }
    expected_logical = total_activations * 2U;
    expected_batch_activations =
        total_activations / effective_width;
    expected_operation_cqes =
        candidate_uses_skip(options->candidate)
            ? total_activations
            : expected_logical;

    if (totals[0].blocks != PIPELINE_BLOCKS_PER_MODE ||
        totals[1].blocks != PIPELINE_BLOCKS_PER_MODE ||
        !totals[0].checksum_set ||
        !totals[1].checksum_set ||
        totals[0].checksum != totals[1].checksum ||
        totals[0].wall_ns < options->min_mode_ns ||
        totals[1].wall_ns < options->min_mode_ns ||
        totals[0].cpu_ns == 0U ||
        totals[1].cpu_ns == 0U ||
        totals[0].p99_ns == 0U ||
        totals[1].p99_ns == 0U ||
        !totals[0].ownership_valid ||
        !totals[1].ownership_valid ||
        totals[0].segments != total_activations ||
        totals[1].segments != total_activations ||
        totals[0].logical_operations != expected_logical ||
        totals[1].logical_operations != expected_logical ||
        totals[1].batch_metrics.activations !=
            expected_batch_activations ||
        totals[1].batch_metrics.segments !=
            total_activations ||
        totals[1].batch_metrics.queue_publications !=
            expected_batch_activations ||
        totals[1].batch_metrics.task_parks !=
            expected_batch_activations ||
        totals[1].batch_metrics.terminal_wakes !=
            expected_batch_activations ||
        totals[1].batch_metrics.operation_sqes !=
            expected_logical ||
        totals[1].batch_metrics.operation_cqes !=
            expected_operation_cqes ||
        totals[1].batch_metrics.cancel_sqes != 0U ||
        totals[1].batch_metrics.cancel_cqes != 0U ||
        totals[1].batch_metrics.hot_allocations != 0U ||
        totals[1].native_metrics.suppressed_success_cqes !=
            (candidate_uses_skip(options->candidate)
                 ? total_activations
                 : 0U) ||
        totals[1].fixed_file_attachments !=
            (candidate_uses_fixed(options->candidate)
                 ? (uint64_t)options->concurrency *
                       PIPELINE_BLOCKS_PER_MODE
                 : 0U) ||
        totals[1].fixed_buffer_attachments !=
            totals[1].fixed_file_attachments) {
        return fail_with_errno(EPROTO);
    }

    printf(
        "RESULT"
        " baseline_wall_ns=%" PRIu64
        " candidate_wall_ns=%" PRIu64
        " baseline_cpu_ns=%" PRIu64
        " candidate_cpu_ns=%" PRIu64
        " baseline_p99_ns=%" PRIu64
        " candidate_p99_ns=%" PRIu64
        " activations=%" PRIu64
        " segments=%" PRIu64
        " logical_ops=%" PRIu64
        " batch_width=%u"
        " queue_publications=%" PRIu64
        " task_parks=%" PRIu64
        " terminal_wakes=%" PRIu64
        " operation_sqes=%" PRIu64
        " operation_cqes=%" PRIu64
        " suppressed_success_cqes=%" PRIu64
        " cancel_sqes=%" PRIu64
        " cancel_cqes=%" PRIu64
        " fixed_file_attachments=%" PRIu64
        " fixed_buffer_attachments=%" PRIu64
        " submit_calls=%" PRIu64
        " submit_syscalls=%" PRIu64
        " checksum=%" PRIu64
        " status=OK\n",
        totals[0].wall_ns,
        totals[1].wall_ns,
        totals[0].cpu_ns,
        totals[1].cpu_ns,
        totals[0].p99_ns,
        totals[1].p99_ns,
        total_activations,
        totals[1].batch_metrics.segments,
        totals[1].logical_operations,
        options->batch_width,
        totals[1].batch_metrics.queue_publications,
        totals[1].batch_metrics.task_parks,
        totals[1].batch_metrics.terminal_wakes,
        totals[1].batch_metrics.operation_sqes,
        totals[1].batch_metrics.operation_cqes,
        totals[1].native_metrics.suppressed_success_cqes,
        totals[1].batch_metrics.cancel_sqes,
        totals[1].batch_metrics.cancel_cqes,
        totals[1].fixed_file_attachments,
        totals[1].fixed_buffer_attachments,
        totals[1].runtime.io_submit_calls,
        totals[1].runtime.io_submit_syscalls,
        totals[1].checksum);
    return PIPELINE_OK;
}

int main(int argc, char **argv) {
    pipeline_options_t options;

    if (parse_options(argc, argv, &options) != 0) {
        fputs(
            "usage: bench_leir_native_pipeline "
            "--candidate link|link_skip|fixed_link_skip "
            "--batch-width 1|2|4|8 "
            "--concurrency 1|4|16 "
            "--payload 64|512|4096 "
            "--activations N "
            "--min-mode-ms N "
            "--order ABBA|BAAB\n",
            stderr);
        return 2;
    }
#if !LLAM_RUNTIME_BACKEND_LINUX
    (void)run_benchmark;
    fprintf(
        stderr,
        "LEIR native pipeline benchmark candidate=%s "
        "requires Linux io_uring\n",
        candidate_name(options.candidate));
    return PIPELINE_UNAVAILABLE;
#else
    {
        int result = run_benchmark(&options);

        if (result == PIPELINE_UNAVAILABLE) {
            fprintf(
                stderr,
                "LEIR_PIPELINE_SKIP candidate=%s "
                "reason=backend_unavailable\n",
                candidate_name(options.candidate));
            return PIPELINE_UNAVAILABLE;
        }
        if (result != PIPELINE_OK) {
            fprintf(
                stderr,
                "LEIR native pipeline benchmark failed: errno=%d\n",
                errno);
            return 3;
        }
    }
    return 0;
#endif
}
