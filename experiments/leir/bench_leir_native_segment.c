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

#define LEIR_NATIVE_BENCH_BLOCK_COUNT 16U
#define LEIR_NATIVE_BENCH_BLOCKS_PER_MODE \
    (LEIR_NATIVE_BENCH_BLOCK_COUNT / 2U)
#define LEIR_NATIVE_BENCH_SERVICE_GAP_CAPACITY 65536U
#define LEIR_NATIVE_BENCH_SERVICE_PERIOD_NS UINT64_C(100000)

typedef enum native_candidate {
    NATIVE_CANDIDATE_LINK = 0,
    NATIVE_CANDIDATE_LINK_SKIP = 1,
} native_candidate_t;

typedef enum native_bench_mode {
    NATIVE_BENCH_BASELINE = 0,
    NATIVE_BENCH_CANDIDATE = 1,
} native_bench_mode_t;

typedef struct native_bench_options {
    native_candidate_t candidate;
    unsigned operations;
    unsigned concurrency;
    size_t payload;
    uint64_t activations;
    uint64_t min_mode_ns;
    leir_bench_order_t order;
} native_bench_options_t;

typedef struct native_task_result {
    leir_native_metrics_t metrics;
    uint64_t checksum;
    uint64_t logical_operations;
} native_task_result_t;

typedef struct native_block_state {
    const native_bench_options_t *options;
    native_bench_mode_t mode;
    leir_peer_process_t *peer;
    leir_phase0_program_t *program;
    leir_native_plan_t plan;
    unsigned char *instance_storage;
    size_t instance_stride;
    unsigned initialized_instances;
    unsigned char *buffers;
    uint64_t *terminal_latencies;
    uint64_t *service_gaps;
    size_t service_gap_count;
    native_task_result_t *task_results;
    atomic_uint failures;
    atomic_uint_fast64_t remaining_activations;
    int first_error;
    unsigned first_connection;
    uint64_t first_activation;
    char first_stage[96];
} native_block_state_t;

typedef struct native_task_arg {
    native_block_state_t *state;
    unsigned connection;
    uint64_t activation_begin;
    uint64_t activation_count;
} native_task_arg_t;

typedef struct native_block_result {
    uint64_t wall_ns;
    uint64_t cpu_ns;
    uint64_t checksum;
    uint64_t completed_transactions;
    uint64_t logical_operations;
    llam_runtime_stats_t runtime;
    leir_native_metrics_t metrics;
    uint64_t service_gap_p99_ns;
    uint64_t terminal_p99_ns;
    bool pending_path_valid;
} native_block_result_t;

typedef struct native_mode_total {
    uint64_t wall_ns;
    uint64_t cpu_ns;
    uint64_t checksum;
    uint64_t completed_transactions;
    uint64_t logical_operations;
    llam_runtime_stats_t runtime;
    leir_native_metrics_t metrics;
    uint64_t service_gap_p99_ns;
    uint64_t terminal_p99_ns;
    bool checksum_set;
    bool pending_path_valid;
    unsigned blocks;
} native_mode_total_t;

static const unsigned native_abba_order[
    LEIR_NATIVE_BENCH_BLOCK_COUNT] = {
    0U, 1U, 1U, 0U,
    0U, 1U, 1U, 0U,
    0U, 1U, 1U, 0U,
    0U, 1U, 1U, 0U,
};

static int fail_with_errno(int error_code) {
    errno = error_code;
    return -1;
}

static bool operation_count_is_supported(unsigned count) {
    return count == 1U || count == 2U ||
           count == 4U || count == 8U;
}

static int parse_u64(
    const char *text,
    uint64_t *value_out) {
    const unsigned char *cursor =
        (const unsigned char *)text;
    uint64_t value = 0U;

    if (text == NULL || value_out == NULL ||
        *text == '\0') {
        return -1;
    }
    while (*cursor != '\0') {
        unsigned digit;

        if (*cursor < (unsigned char)'0' ||
            *cursor > (unsigned char)'9') {
            return -1;
        }
        digit = (unsigned)(
            *cursor - (unsigned char)'0');
        if (value >
            (UINT64_MAX - digit) / UINT64_C(10)) {
            return -1;
        }
        value = value * UINT64_C(10) + digit;
        cursor += 1;
    }
    *value_out = value;
    return 0;
}

typedef enum native_option_id {
    NATIVE_OPTION_CANDIDATE = 0,
    NATIVE_OPTION_OPERATIONS,
    NATIVE_OPTION_CONCURRENCY,
    NATIVE_OPTION_PAYLOAD,
    NATIVE_OPTION_ACTIVATIONS,
    NATIVE_OPTION_MIN_MODE_MS,
    NATIVE_OPTION_ORDER,
    NATIVE_OPTION_COUNT,
} native_option_id_t;

static int option_id_from_name(
    const char *name,
    native_option_id_t *id_out) {
    static const char *const names[NATIVE_OPTION_COUNT] = {
        "--candidate",
        "--ops",
        "--concurrency",
        "--payload",
        "--activations",
        "--min-mode-ms",
        "--order",
    };
    unsigned i;

    if (name == NULL || id_out == NULL) {
        return -1;
    }
    for (i = 0U; i < NATIVE_OPTION_COUNT; i += 1U) {
        if (strcmp(name, names[i]) == 0) {
            *id_out = (native_option_id_t)i;
            return 0;
        }
    }
    return -1;
}

static int assign_option(
    native_bench_options_t *options,
    native_option_id_t id,
    const char *text) {
    uint64_t value;

    if (options == NULL || text == NULL) {
        return -1;
    }
    if (id == NATIVE_OPTION_CANDIDATE) {
        if (strcmp(text, "link") == 0) {
            options->candidate = NATIVE_CANDIDATE_LINK;
            return 0;
        }
        if (strcmp(text, "link_skip") == 0) {
            options->candidate =
                NATIVE_CANDIDATE_LINK_SKIP;
            return 0;
        }
        return -1;
    }
    if (id == NATIVE_OPTION_ORDER) {
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
        case NATIVE_OPTION_OPERATIONS:
            if (value > UINT_MAX ||
                !operation_count_is_supported(
                    (unsigned)value)) {
                return -1;
            }
            options->operations = (unsigned)value;
            return 0;
        case NATIVE_OPTION_CONCURRENCY:
            if (value == 0U || value > 512U) {
                return -1;
            }
            options->concurrency = (unsigned)value;
            return 0;
        case NATIVE_OPTION_PAYLOAD:
            if (value < 64U ||
                value > 16384U ||
                value > SIZE_MAX) {
                return -1;
            }
            options->payload = (size_t)value;
            return 0;
        case NATIVE_OPTION_ACTIVATIONS:
            if (value == 0U) {
                return -1;
            }
            options->activations = value;
            return 0;
        case NATIVE_OPTION_MIN_MODE_MS:
            if (value == 0U ||
                value >
                    UINT64_MAX / UINT64_C(1000000)) {
                return -1;
            }
            options->min_mode_ns =
                value * UINT64_C(1000000);
            return 0;
        case NATIVE_OPTION_CANDIDATE:
        case NATIVE_OPTION_ORDER:
        case NATIVE_OPTION_COUNT:
        default:
            return -1;
    }
}

static int parse_options(
    int argc,
    char *const argv[],
    native_bench_options_t *options_out) {
    bool seen[NATIVE_OPTION_COUNT] = {false};
    native_bench_options_t options;
    int index;

    if (argv == NULL || options_out == NULL ||
        argc != 1 + 2 * NATIVE_OPTION_COUNT) {
        return -1;
    }
    memset(&options, 0, sizeof(options));
    for (index = 1; index < argc; index += 2) {
        native_option_id_t id;

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
         index < (int)NATIVE_OPTION_COUNT;
         index += 1) {
        if (!seen[index]) {
            return -1;
        }
    }
    *options_out = options;
    return 0;
}

static const char *candidate_name(
    native_candidate_t candidate) {
    return candidate == NATIVE_CANDIDATE_LINK_SKIP
        ? "link_skip"
        : "link";
}

static leir_native_mode_t candidate_mode(
    native_candidate_t candidate) {
    return candidate == NATIVE_CANDIDATE_LINK_SKIP
        ? LEIR_NATIVE_MODE_LINK_CQE_SKIP
        : LEIR_NATIVE_MODE_LINK;
}

static unsigned transactions_per_activation(
    const native_bench_options_t *options) {
    return options->operations;
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

static uint64_t activations_for_connection(
    uint64_t activations,
    unsigned concurrency,
    unsigned connection) {
    uint64_t count = activations / concurrency;

    if ((uint64_t)connection <
        activations % concurrency) {
        count += 1U;
    }
    return count;
}

static uint64_t activation_begin(
    uint64_t activations,
    unsigned concurrency,
    unsigned connection) {
    uint64_t base = activations / concurrency;
    uint64_t remainder = activations % concurrency;

    return (uint64_t)connection * base +
           ((uint64_t)connection < remainder
                ? (uint64_t)connection
                : remainder);
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
    native_block_state_t *state,
    const char *stage,
    int error_code,
    unsigned connection,
    uint64_t activation) {
    unsigned expected = 0U;

    if (atomic_compare_exchange_strong_explicit(
            &state->failures,
            &expected,
            1U,
            memory_order_acq_rel,
            memory_order_acquire)) {
        state->first_error =
            error_code != 0 ? error_code : EIO;
        state->first_connection = connection;
        state->first_activation = activation;
        (void)snprintf(
            state->first_stage,
            sizeof(state->first_stage),
            "%s",
            stage);
    }
}

static void block_fail(
    native_block_state_t *state,
    const char *stage,
    int error_code) {
    block_fail_at(
        state,
        stage,
        error_code,
        UINT_MAX,
        UINT64_MAX);
}

static void activation_complete(
    native_block_state_t *state) {
    uint64_t previous = atomic_fetch_sub_explicit(
        &state->remaining_activations,
        1U,
        memory_order_acq_rel);

    if (previous == 0U) {
        block_fail(
            state,
            "activation accounting underflow",
            EPROTO);
    }
}

static bool pending_ops_are_zero(void) {
    unsigned i;

    for (i = 0U;
         i < g_llam_runtime.active_nodes;
         i += 1U) {
        if (atomic_load_explicit(
                &g_llam_runtime.nodes[i].pending_ops,
                memory_order_acquire) != 0U) {
            return false;
        }
    }
    return true;
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

static int create_program(
    unsigned operations,
    leir_phase0_program_t **program_out) {
    leir_phase0_slot_kind_t slots[LEIR_PHASE0_MAX_SLOTS];
    leir_phase0_node_desc_t
        nodes[LEIR_NATIVE_MAX_OPS + 2U];
    leir_phase0_program_desc_t desc;
    unsigned first_buffer_slot = 2U;
    unsigned first_result_slot =
        first_buffer_slot + operations;
    unsigned i;

    if (program_out == NULL ||
        !operation_count_is_supported(operations) ||
        first_result_slot >= LEIR_PHASE0_MAX_SLOTS) {
        return fail_with_errno(EINVAL);
    }
    *program_out = NULL;
    memset(nodes, 0, sizeof(nodes));
    memset(slots, 0, sizeof(slots));
    slots[0] = LEIR_PHASE0_SLOT_FD;
    slots[1] = LEIR_PHASE0_SLOT_U64;
    slots[first_result_slot] = LEIR_PHASE0_SLOT_I64;
    for (i = 0U; i < operations; i += 1U) {
        slots[first_buffer_slot + i] =
            LEIR_PHASE0_SLOT_CONST_BUFFER;
        nodes[i].opcode = LEIR_PHASE0_OP_WRITE_ALL;
        nodes[i].fd_slot = 0U;
        nodes[i].buffer_slot =
            (uint16_t)(first_buffer_slot + i);
        nodes[i].length_slot = 1U;
        nodes[i].result_slot = (uint16_t)first_result_slot;
        nodes[i].on_success = (uint16_t)(i + 1U);
        nodes[i].on_eof = (uint16_t)(operations + 1U);
        nodes[i].on_error = (uint16_t)(operations + 1U);
    }
    nodes[operations] = terminal_node(
        LEIR_PHASE0_OP_RETURN, (uint16_t)first_result_slot);
    nodes[operations + 1U] = terminal_node(
        LEIR_PHASE0_OP_FAIL,
        (uint16_t)first_result_slot);
    memset(&desc, 0, sizeof(desc));
    desc.nodes = nodes;
    desc.slot_kinds = slots;
    desc.node_count = operations + 2U;
    desc.slot_count = first_result_slot + 1U;
    desc.entry_node = 0U;
    return leir_phase0_program_create(
        &desc, program_out);
}

static leir_native_instance_t *instance_at(
    native_block_state_t *state,
    unsigned connection) {
    return (leir_native_instance_t *)(
        state->instance_storage +
        (size_t)connection * state->instance_stride);
}

static void record_checksum(
    native_task_arg_t *arg,
    uint64_t sequence,
    const unsigned char *buffer) {
    native_block_state_t *state = arg->state;
    uint64_t checksum = leir_test_payload_checksum(
        buffer,
        state->options->payload,
        arg->connection,
        sequence);

    state->task_results[arg->connection].checksum ^=
        rotate_checksum(
            checksum,
            (unsigned)(
                ((uint64_t)arg->connection + sequence) &
                63U));
}

static unsigned char *operation_buffer(
    native_block_state_t *state,
    unsigned connection,
    unsigned operation) {
    size_t index =
        (size_t)connection * state->options->operations +
        operation;

    return state->buffers +
           index * state->options->payload;
}

static void baseline_task(void *opaque) {
    native_task_arg_t *arg = opaque;
    native_block_state_t *state = arg->state;
    const native_bench_options_t *options =
        state->options;
    llam_fd_t fd = leir_peer_process_server_fd(
        state->peer, arg->connection);
    uint64_t activation;

    for (activation = 0U;
         activation < arg->activation_count;
         activation += 1U) {
        uint64_t started_ns = llam_now_ns();
        unsigned operation;

        for (operation = 0U;
             operation < options->operations;
             operation += 1U) {
            uint64_t sequence =
                activation * options->operations +
                operation;
            unsigned char *buffer = operation_buffer(
                state, arg->connection, operation);

            leir_test_prepare_payload(
                buffer,
                options->payload,
                arg->connection,
                sequence);
            if (leir_test_write_all(
                    fd, buffer, options->payload) != 0) {
                block_fail_at(
                    state,
                    "baseline write",
                    errno,
                    arg->connection,
                    activation);
                return;
            }
            state->task_results[
                arg->connection].logical_operations += 1U;
            record_checksum(arg, sequence, buffer);
        }
        state->terminal_latencies[
            arg->activation_begin + activation] =
            llam_now_ns() - started_ns;
        activation_complete(state);
    }
}

static void candidate_task(void *opaque) {
    native_task_arg_t *arg = opaque;
    native_block_state_t *state = arg->state;
    const native_bench_options_t *options =
        state->options;
    native_task_result_t *task_result =
        &state->task_results[arg->connection];
    leir_native_instance_t *instance =
        instance_at(state, arg->connection);
    uint64_t activation;

    for (activation = 0U;
         activation < arg->activation_count;
         activation += 1U) {
        leir_phase0_value_t
            values_out[LEIR_PHASE0_MAX_SLOTS];
        leir_native_metrics_t metrics;
        uint64_t started_ns = llam_now_ns();
        unsigned operation;

        memset(values_out, 0, sizeof(values_out));
        native_metrics_init(&metrics);
        for (operation = 0U;
             operation < options->operations;
             operation += 1U) {
            uint64_t sequence =
                activation * options->operations +
                operation;
            unsigned char *buffer = operation_buffer(
                state, arg->connection, operation);

            leir_test_prepare_payload(
                buffer,
                options->payload,
                arg->connection,
                sequence);
            record_checksum(arg, sequence, buffer);
        }
        if (leir_native_instance_run(
                instance,
                values_out,
                state->program->slot_count,
                &metrics) != 0) {
            block_fail_at(
                state,
                "candidate native run",
                errno,
                arg->connection,
                activation);
            return;
        }
        if (values_out[
                state->plan.result_slot].i64 !=
            (int64_t)options->payload) {
            block_fail_at(
                state,
                "candidate terminal result",
                EPROTO,
                arg->connection,
                activation);
            return;
        }

        task_result->metrics = metrics;
        task_result->logical_operations +=
            options->operations;
        state->terminal_latencies[
            arg->activation_begin + activation] =
            llam_now_ns() - started_ns;
        activation_complete(state);
    }
}

static void service_task(void *opaque) {
    native_block_state_t *state = opaque;
    uint64_t previous_ns = llam_now_ns();

    while (atomic_load_explicit(
               &state->remaining_activations,
               memory_order_acquire) != 0U &&
           atomic_load_explicit(
               &state->failures,
               memory_order_acquire) == 0U) {
        uint64_t now_ns;

        if (llam_sleep_ns(
                LEIR_NATIVE_BENCH_SERVICE_PERIOD_NS) != 0) {
            block_fail(
                state, "service task sleep", errno);
            return;
        }
        now_ns = llam_now_ns();
        if (state->service_gap_count <
            LEIR_NATIVE_BENCH_SERVICE_GAP_CAPACITY) {
            state->service_gaps[
                state->service_gap_count++] =
                now_ns - previous_ns;
        }
        previous_ns = now_ns;
    }
}

static void peer_start_task(void *opaque) {
    native_block_state_t *state = opaque;

    if (llam_sleep_ns(UINT64_C(1000000)) != 0) {
        block_fail(
            state, "peer start delay", errno);
        return;
    }
    if (leir_peer_process_signal_start(
            state->peer) != 0) {
        block_fail(
            state, "peer start signal", errno);
    }
}

static int compare_u64(
    const void *left,
    const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return a < b ? -1 : (a > b ? 1 : 0);
}

static uint64_t p99(
    uint64_t *samples,
    size_t count) {
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

static bool native_backend_is_available(
    native_candidate_t candidate) {
#if LLAM_RUNTIME_BACKEND_LINUX
    llam_node_t *node;

    if (g_llam_runtime.active_nodes == 0U ||
        g_llam_runtime.nodes == NULL) {
        return false;
    }
    node = &g_llam_runtime.nodes[0];
    if (!node->ring_ready ||
        !node->supports_recv ||
        !node->supports_send) {
        return false;
    }
    if (candidate == NATIVE_CANDIDATE_LINK_SKIP &&
        (node->linux_ring_features &
         IORING_FEAT_CQE_SKIP) == 0U) {
        return false;
    }
    return true;
#else
    (void)candidate;
    return false;
#endif
}

static bool block_integrity(
    const native_bench_options_t *options,
    native_bench_mode_t mode,
    uint64_t activations,
    const native_block_result_t *result) {
    uint64_t logical;
    uint64_t transactions;

    if (activations >
            UINT64_MAX / options->operations ||
        activations >
            UINT64_MAX /
                transactions_per_activation(options)) {
        return false;
    }
    logical = activations * options->operations;
    transactions =
        activations *
        transactions_per_activation(options);
    if (result->completed_transactions != transactions ||
        result->logical_operations != logical ||
        !result->pending_path_valid ||
        result->wall_ns == 0U ||
        result->cpu_ns == 0U) {
        return false;
    }
    if (mode == NATIVE_BENCH_BASELINE) {
        return true;
    }
    return result->metrics.activations == activations &&
           result->metrics.logical_operations == logical &&
           result->metrics.queue_publications == activations &&
           result->metrics.prepared_sqes == logical &&
           result->metrics.observed_cqes ==
               (options->candidate ==
                        NATIVE_CANDIDATE_LINK
                    ? logical
                    : activations) &&
           result->metrics.suppressed_success_cqes ==
               (options->candidate ==
                        NATIVE_CANDIDATE_LINK
                    ? 0U
                    : logical - activations) &&
           result->metrics.task_parks == activations &&
           result->metrics.terminal_wakes == activations &&
           result->metrics.hot_allocations == 0U &&
           result->metrics.first_error_operation ==
               UINT16_MAX;
}

static int initialize_candidate_instances(
    native_block_state_t *state) {
    const native_bench_options_t *options =
        state->options;
    size_t storage_size;
    unsigned i;

    if (create_program(
            options->operations,
            &state->program) != 0 ||
        leir_native_plan_compile(
            state->program,
            &state->plan) != 0) {
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
    state->instance_storage =
        calloc(1U, storage_size);
    if (state->instance_storage == NULL) {
        return -1;
    }

    for (i = 0U; i < options->concurrency; i += 1U) {
        leir_phase0_value_t
            values[LEIR_PHASE0_MAX_SLOTS];
        leir_native_instance_t *instance =
            instance_at(state, i);
        unsigned operation;

        memset(values, 0, sizeof(values));
        values[0].fd = leir_peer_process_server_fd(
            state->peer, i);
        values[1].u64 = options->payload;
        for (operation = 0U;
             operation < options->operations;
             operation += 1U) {
            values[2U + operation].buffer.data =
                operation_buffer(state, i, operation);
            values[2U + operation].buffer.size =
                options->payload;
        }
        values[2U + options->operations].i64 = -1;
        if (leir_native_instance_init(
                instance,
                state->instance_stride,
                state->program,
                &state->plan,
                candidate_mode(
                    options->candidate)) != 0) {
            return -1;
        }
        state->initialized_instances += 1U;
        if (leir_native_instance_bind(
                instance,
                values,
                state->program->slot_count) != 0) {
            return -1;
        }
    }
    return 0;
}

static int destroy_candidate_instances(
    native_block_state_t *state) {
    int first_error = 0;
    unsigned i;

    for (i = 0U;
         i < state->initialized_instances;
         i += 1U) {
        if (leir_native_instance_destroy(
                instance_at(state, i)) != 0 &&
            first_error == 0) {
            first_error = errno != 0 ? errno : EIO;
        }
    }
    state->initialized_instances = 0U;
    if (first_error != 0) {
        return fail_with_errno(first_error);
    }
    return 0;
}

enum {
    NATIVE_BLOCK_OK = 0,
    NATIVE_BLOCK_ERROR = -1,
    NATIVE_BLOCK_UNAVAILABLE = 77,
};

static int run_block(
    const native_bench_options_t *options,
    native_bench_mode_t mode,
    uint64_t activations,
    native_block_result_t *result_out) {
    leir_peer_config_t peer_config;
    leir_peer_result_t peer_result;
    native_block_state_t state;
    native_task_arg_t *task_args = NULL;
    llam_task_t **tasks = NULL;
    llam_task_t *starter = NULL;
    llam_task_t *service = NULL;
    llam_runtime_opts_t runtime_options;
    size_t activation_count;
    bool runtime_started = false;
    uint64_t wall_started = 0U;
    uint64_t cpu_started = 0U;
    unsigned i;
    int status = NATIVE_BLOCK_ERROR;

    if (options == NULL ||
        result_out == NULL ||
        activations == 0U ||
        activations > SIZE_MAX ||
        activations >
            UINT64_MAX / options->operations ||
        options->operations >
            SIZE_MAX / options->payload ||
        options->concurrency >
            SIZE_MAX /
                (options->operations * options->payload)) {
        return fail_with_errno(EOVERFLOW);
    }
    activation_count = (size_t)activations;
    memset(result_out, 0, sizeof(*result_out));
    native_metrics_init(&result_out->metrics);
    memset(&state, 0, sizeof(state));
    state.options = options;
    state.mode = mode;
    atomic_init(&state.failures, 0U);
    atomic_init(
        &state.remaining_activations,
        activations);

    state.buffers = calloc(
        (size_t)options->concurrency *
            options->operations,
        options->payload);
    state.task_results = calloc(
        options->concurrency,
        sizeof(state.task_results[0]));
    state.terminal_latencies = calloc(
        activation_count,
        sizeof(state.terminal_latencies[0]));
    state.service_gaps = calloc(
        LEIR_NATIVE_BENCH_SERVICE_GAP_CAPACITY,
        sizeof(state.service_gaps[0]));
    task_args = calloc(
        options->concurrency,
        sizeof(task_args[0]));
    tasks = calloc(
        options->concurrency,
        sizeof(tasks[0]));
    if (state.buffers == NULL ||
        state.task_results == NULL ||
        state.terminal_latencies == NULL ||
        state.service_gaps == NULL ||
        task_args == NULL ||
        tasks == NULL) {
        errno = ENOMEM;
        goto cleanup;
    }

    memset(&peer_config, 0, sizeof(peer_config));
    peer_config.workload =
        LEIR_BENCH_WORKLOAD_SOCKET_RELAY;
    peer_config.concurrency = options->concurrency;
    peer_config.payload = options->payload;
    peer_config.activations = activations;
    peer_config.transactions_per_activation =
        transactions_per_activation(options);
    peer_config.socket_kind =
        LEIR_PEER_SOCKET_SEQPACKET;
    peer_config.operations_per_activation =
        options->operations;
    if (leir_peer_process_start(
            &peer_config, &state.peer) != 0) {
        goto cleanup;
    }
    if (mode == NATIVE_BENCH_CANDIDATE &&
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
    if (mode == NATIVE_BENCH_CANDIDATE &&
        !native_backend_is_available(
            options->candidate)) {
        status = NATIVE_BLOCK_UNAVAILABLE;
        goto cleanup;
    }

    for (i = 0U; i < options->concurrency; i += 1U) {
        task_args[i].state = &state;
        task_args[i].connection = i;
        task_args[i].activation_begin =
            activation_begin(
                activations,
                options->concurrency,
                i);
        task_args[i].activation_count =
            activations_for_connection(
                activations,
                options->concurrency,
                i);
        tasks[i] = llam_spawn(
            mode == NATIVE_BENCH_BASELINE
                ? baseline_task
                : candidate_task,
            &task_args[i],
            NULL);
        if (tasks[i] == NULL) {
            block_fail(
                &state, "task spawn", errno);
            (void)llam_runtime_request_stop();
            goto cleanup;
        }
    }
    service = llam_spawn(
        service_task, &state, NULL);
    starter = llam_spawn(
        peer_start_task, &state, NULL);
    if (service == NULL || starter == NULL) {
        block_fail(
            &state, "support task spawn", errno);
        (void)llam_runtime_request_stop();
        goto cleanup;
    }

    wall_started = llam_now_ns();
    cpu_started = process_cpu_ns();
    if (llam_run() != 0) {
        block_fail(&state, "runtime run", errno);
    }
    result_out->cpu_ns =
        process_cpu_ns() - cpu_started;
    result_out->wall_ns =
        llam_now_ns() - wall_started;
    if (llam_runtime_collect_stats(
            &result_out->runtime) != 0) {
        block_fail(
            &state, "runtime stats", errno);
    }
    result_out->pending_path_valid =
        pending_ops_are_zero();

    for (i = 0U; i < options->concurrency; i += 1U) {
        if (tasks[i] != NULL &&
            llam_join(tasks[i]) != 0) {
            block_fail(&state, "task join", errno);
        }
        tasks[i] = NULL;
        result_out->checksum ^=
            state.task_results[i].checksum;
        result_out->logical_operations +=
            state.task_results[i].logical_operations;
        if (mode == NATIVE_BENCH_CANDIDATE) {
            native_metrics_add(
                &result_out->metrics,
                &state.task_results[i].metrics);
        }
    }
    if (starter != NULL && llam_join(starter) != 0) {
        block_fail(
            &state, "peer starter join", errno);
    }
    starter = NULL;
    if (service != NULL && llam_join(service) != 0) {
        block_fail(
            &state, "service task join", errno);
    }
    service = NULL;
    if (atomic_load_explicit(
            &state.remaining_activations,
            memory_order_acquire) != 0U) {
        block_fail(
            &state,
            "activation accounting incomplete",
            EPROTO);
    }
    if (atomic_load_explicit(
            &state.failures,
            memory_order_acquire) != 0U) {
        errno = state.first_error;
        goto cleanup;
    }
    if (leir_peer_process_finish(
            state.peer, &peer_result) != 0) {
        state.peer = NULL;
        goto cleanup;
    }
    state.peer = NULL;
    result_out->completed_transactions =
        peer_result.completed_transactions;
    result_out->terminal_p99_ns = p99(
        state.terminal_latencies,
        activation_count);
    result_out->service_gap_p99_ns = p99(
        state.service_gaps,
        state.service_gap_count);
    if (peer_result.status != 0 ||
        peer_result.checksum != result_out->checksum ||
        !block_integrity(
            options,
            mode,
            activations,
            result_out)) {
        block_fail(
            &state, "block integrity", EPROTO);
        errno = EPROTO;
        goto cleanup;
    }
    status = NATIVE_BLOCK_OK;

cleanup:
    if (status == NATIVE_BLOCK_ERROR &&
        atomic_load_explicit(
            &state.failures,
            memory_order_acquire) != 0U) {
        fprintf(
            stderr,
            "LEIR native block failed mode=%s stage=%s "
            "errno=%d connection=%u activation=%" PRIu64
            "\n",
            mode == NATIVE_BENCH_BASELINE
                ? "baseline"
                : "candidate",
            state.first_stage,
            state.first_error,
            state.first_connection,
            state.first_activation);
    }
    if (state.peer != NULL) {
        leir_peer_process_abort(state.peer);
        state.peer = NULL;
    }
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    if (destroy_candidate_instances(&state) != 0 &&
        status == NATIVE_BLOCK_OK) {
        status = NATIVE_BLOCK_ERROR;
    }
    leir_phase0_program_destroy(state.program);
    free(state.instance_storage);
    free(tasks);
    free(task_args);
    free(state.service_gaps);
    free(state.terminal_latencies);
    free(state.task_results);
    free(state.buffers);
    return status;
}

static void total_init(native_mode_total_t *total) {
    memset(total, 0, sizeof(*total));
    native_metrics_init(&total->metrics);
}

static int total_add(
    native_mode_total_t *total,
    const native_block_result_t *block) {
    if (total->wall_ns > UINT64_MAX - block->wall_ns ||
        total->cpu_ns > UINT64_MAX - block->cpu_ns ||
        total->completed_transactions >
            UINT64_MAX -
                block->completed_transactions ||
        total->logical_operations >
            UINT64_MAX -
                block->logical_operations) {
        return fail_with_errno(EOVERFLOW);
    }
    if (total->checksum_set &&
        total->checksum != block->checksum) {
        return fail_with_errno(EPROTO);
    }
    total->wall_ns += block->wall_ns;
    total->cpu_ns += block->cpu_ns;
    total->completed_transactions +=
        block->completed_transactions;
    total->logical_operations +=
        block->logical_operations;
    total->checksum = block->checksum;
    total->checksum_set = true;
    runtime_stats_add(
        &total->runtime, &block->runtime);
    native_metrics_add(
        &total->metrics, &block->metrics);
    if (block->service_gap_p99_ns >
        total->service_gap_p99_ns) {
        total->service_gap_p99_ns =
            block->service_gap_p99_ns;
    }
    if (block->terminal_p99_ns >
        total->terminal_p99_ns) {
        total->terminal_p99_ns =
            block->terminal_p99_ns;
    }
    total->pending_path_valid =
        total->blocks == 0U
            ? block->pending_path_valid
            : total->pending_path_valid &&
                  block->pending_path_valid;
    total->blocks += 1U;
    return 0;
}

static int calibrate(
    const native_bench_options_t *options,
    uint64_t *activations_out) {
    uint64_t activations = options->activations;
    unsigned iteration;

    if (activations < options->concurrency) {
        activations = options->concurrency;
    }
    for (iteration = 0U; iteration < 12U; iteration += 1U) {
        native_block_result_t baseline;
        native_block_result_t candidate;
        uint64_t faster_ns;
        uint64_t target_ns =
            leir_bench_calibration_target_block_ns(
                options->min_mode_ns);
        uint64_t scale;
        int result;

        result = run_block(
            options,
            NATIVE_BENCH_BASELINE,
            activations,
            &baseline);
        if (result != NATIVE_BLOCK_OK) {
            return result;
        }
        result = run_block(
            options,
            NATIVE_BENCH_CANDIDATE,
            activations,
            &candidate);
        if (result != NATIVE_BLOCK_OK) {
            return result;
        }
        faster_ns =
            baseline.wall_ns < candidate.wall_ns
                ? baseline.wall_ns
                : candidate.wall_ns;
        if (faster_ns >= target_ns) {
            *activations_out = activations;
            return NATIVE_BLOCK_OK;
        }
        if (faster_ns == 0U) {
            scale = 2U;
        } else {
            scale =
                (target_ns + faster_ns - 1U) /
                faster_ns;
            if (scale < 2U) {
                scale = 2U;
            }
            if (scale > 16U) {
                scale = 16U;
            }
        }
        if (activations > UINT64_MAX / scale) {
            return fail_with_errno(EOVERFLOW);
        }
        activations *= scale;
    }
    return fail_with_errno(ERANGE);
}

static int run_benchmark(
    const native_bench_options_t *options) {
    native_mode_total_t totals[2];
    uint64_t activations_per_block;
    uint64_t total_activations;
    uint64_t expected_cqes;
    uint64_t resumes_avoided;
    unsigned block;
    int result;

    total_init(&totals[0]);
    total_init(&totals[1]);
    result = calibrate(
        options, &activations_per_block);
    if (result != NATIVE_BLOCK_OK) {
        return result;
    }
    for (block = 0U;
         block < LEIR_NATIVE_BENCH_BLOCK_COUNT;
         block += 1U) {
        native_block_result_t block_result;
        unsigned mode = native_abba_order[block];

        if (options->order == LEIR_BENCH_ORDER_BAAB) {
            mode ^= 1U;
        }
        result = run_block(
            options,
            (native_bench_mode_t)mode,
            activations_per_block,
            &block_result);
        if (result != NATIVE_BLOCK_OK) {
            return result;
        }
        if (total_add(
                &totals[mode],
                &block_result) != 0) {
            return NATIVE_BLOCK_ERROR;
        }
    }
    if (activations_per_block >
        UINT64_MAX /
            LEIR_NATIVE_BENCH_BLOCKS_PER_MODE) {
        return fail_with_errno(EOVERFLOW);
    }
    total_activations =
        activations_per_block *
        LEIR_NATIVE_BENCH_BLOCKS_PER_MODE;
    if (total_activations >
        UINT64_MAX / options->operations) {
        return fail_with_errno(EOVERFLOW);
    }
    expected_cqes =
        options->candidate == NATIVE_CANDIDATE_LINK
            ? totals[1].logical_operations
            : total_activations;
    resumes_avoided =
        totals[1].logical_operations - total_activations;

    if (totals[0].blocks !=
            LEIR_NATIVE_BENCH_BLOCKS_PER_MODE ||
        totals[1].blocks !=
            LEIR_NATIVE_BENCH_BLOCKS_PER_MODE ||
        !totals[0].checksum_set ||
        !totals[1].checksum_set ||
        totals[0].checksum != totals[1].checksum ||
        totals[0].wall_ns < options->min_mode_ns ||
        totals[1].wall_ns < options->min_mode_ns ||
        totals[0].wall_ns == 0U ||
        totals[1].wall_ns == 0U ||
        totals[0].cpu_ns == 0U ||
        totals[1].cpu_ns == 0U ||
        !totals[0].pending_path_valid ||
        !totals[1].pending_path_valid ||
        totals[0].logical_operations !=
            total_activations * options->operations ||
        totals[1].logical_operations !=
            total_activations * options->operations ||
        totals[1].metrics.activations !=
            total_activations ||
        totals[1].metrics.logical_operations !=
            totals[1].logical_operations ||
        totals[1].metrics.queue_publications !=
            total_activations ||
        totals[1].metrics.prepared_sqes !=
            totals[1].logical_operations ||
        totals[1].metrics.observed_cqes !=
            expected_cqes ||
        totals[1].metrics.task_parks !=
            total_activations ||
        totals[1].metrics.terminal_wakes !=
            total_activations ||
        totals[1].metrics.hot_allocations != 0U) {
        return fail_with_errno(EPROTO);
    }

    printf(
        "LEIR_NATIVE_PAIR version=1"
        " candidate=%s"
        " ops=%u"
        " concurrency=%u"
        " payload=%zu"
        " activations=%" PRIu64
        " min_mode_ns=%" PRIu64
        " blocks_per_mode=%u"
        " baseline_wall_ns=%" PRIu64
        " candidate_wall_ns=%" PRIu64
        " baseline_cpu_ns=%" PRIu64
        " candidate_cpu_ns=%" PRIu64
        " wall_speedup=%.9f"
        " cpu_ratio=%.9f"
        " baseline_ctx_switches=%" PRIu64
        " candidate_ctx_switches=%" PRIu64
        " logical_operations=%" PRIu64
        " queue_publications=%" PRIu64
        " prepared_sqes=%" PRIu64
        " ring_submit_calls=%" PRIu64
        " ring_submit_syscalls=%" PRIu64
        " expected_cqes=%" PRIu64
        " observed_cqes=%" PRIu64
        " suppressed_success_cqes=%" PRIu64
        " baseline_task_parks=%" PRIu64
        " candidate_task_parks=%" PRIu64
        " terminal_wakes=%" PRIu64
        " resumes_avoided=%" PRIu64
        " hot_allocations=%" PRIu64
        " baseline_checksum=%016" PRIx64
        " candidate_checksum=%016" PRIx64
        " pending_path_valid=1"
        " baseline_service_gap_p99_ns=%" PRIu64
        " candidate_service_gap_p99_ns=%" PRIu64
        " baseline_terminal_p99_ns=%" PRIu64
        " candidate_terminal_p99_ns=%" PRIu64
        " peer=process"
        " cpu_scope=server"
        " platform=linux_io_uring"
        " order=%s\n",
        candidate_name(options->candidate),
        options->operations,
        options->concurrency,
        options->payload,
        total_activations,
        options->min_mode_ns,
        LEIR_NATIVE_BENCH_BLOCKS_PER_MODE,
        totals[0].wall_ns,
        totals[1].wall_ns,
        totals[0].cpu_ns,
        totals[1].cpu_ns,
        (double)totals[0].wall_ns /
            (double)totals[1].wall_ns,
        (double)totals[1].cpu_ns /
            (double)totals[0].cpu_ns,
        totals[0].runtime.ctx_switches,
        totals[1].runtime.ctx_switches,
        totals[1].logical_operations,
        totals[1].metrics.queue_publications,
        totals[1].metrics.prepared_sqes,
        totals[1].runtime.io_submit_calls,
        totals[1].runtime.io_submit_syscalls,
        expected_cqes,
        totals[1].metrics.observed_cqes,
        totals[1].metrics.suppressed_success_cqes,
        totals[0].runtime.io_submits,
        totals[1].metrics.task_parks,
        totals[1].metrics.terminal_wakes,
        resumes_avoided,
        totals[1].metrics.hot_allocations,
        totals[0].checksum,
        totals[1].checksum,
        totals[0].service_gap_p99_ns,
        totals[1].service_gap_p99_ns,
        totals[0].terminal_p99_ns,
        totals[1].terminal_p99_ns,
        leir_bench_order_name(options->order));
    return NATIVE_BLOCK_OK;
}

int main(int argc, char **argv) {
    native_bench_options_t options;

    if (parse_options(argc, argv, &options) != 0) {
        fputs(
            "usage: bench_leir_native_segment "
            "--candidate link|link_skip "
            "--ops 1|2|4|8 "
            "--concurrency 1..512 "
            "--payload 64..16384 "
            "--activations N "
            "--min-mode-ms N "
            "--order ABBA|BAAB\n",
            stderr);
        return 2;
    }
#if !LLAM_RUNTIME_BACKEND_LINUX
    (void)run_benchmark;
    fputs(
        "LEIR native benchmark requires Linux io_uring\n",
        stderr);
    return NATIVE_BLOCK_UNAVAILABLE;
#else
    int result;

    result = run_benchmark(&options);
    if (result == NATIVE_BLOCK_UNAVAILABLE) {
        fprintf(
            stderr,
            "LEIR_NATIVE_SKIP candidate=%s "
            "reason=backend_unavailable\n",
            candidate_name(options.candidate));
        return NATIVE_BLOCK_UNAVAILABLE;
    }
    if (result != NATIVE_BLOCK_OK) {
        fprintf(
            stderr,
            "LEIR native benchmark failed: errno=%d\n",
            errno);
        return 3;
    }
    return 0;
#endif
}
