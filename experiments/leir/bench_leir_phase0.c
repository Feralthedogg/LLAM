#include "runtime_internal.h"
#include "leir_peer_process.h"
#include "leir_phase0.h"
#include "leir_phase0_internal.h"
#include "leir_test_support.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if LLAM_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define LEIR_BENCH_BLOCK_COUNT 16U
#define LEIR_BENCH_MODE_COUNT 2U
#define LEIR_BENCH_SERVICE_GAP_CAPACITY 65536U
#define LEIR_BENCH_SERVICE_PERIOD_NS UINT64_C(100000)

typedef enum leir_bench_mode {
    LEIR_BENCH_MODE_BASELINE = 0,
    LEIR_BENCH_MODE_CANDIDATE = 1,
} leir_bench_mode_t;

typedef struct leir_bench_task_result {
    leir_phase0_metrics_t metrics;
    uint64_t checksum;
    uint64_t logical_io_submits;
    uint64_t logical_io_completions;
} leir_bench_task_result_t;

typedef struct leir_bench_block_state {
    const leir_bench_options_t *options;
    leir_bench_mode_t mode;
    leir_phase0_program_t *program;
    leir_peer_process_t *peer;
    leir_phase0_instance_t *instances;
    unsigned char *buffers;
    uint64_t *terminal_latencies;
    uint64_t *service_gaps;
    size_t service_gap_count;
    leir_bench_task_result_t *task_results;
    atomic_uint failures;
    atomic_uint_fast64_t remaining_activations;
    int first_error;
    char first_stage[96];
} leir_bench_block_state_t;

typedef struct leir_bench_task_arg {
    leir_bench_block_state_t *state;
    unsigned connection;
    uint64_t activation_begin;
    uint64_t activation_count;
} leir_bench_task_arg_t;

typedef struct leir_bench_block_result {
    uint64_t wall_ns;
    uint64_t cpu_ns;
    uint64_t checksum;
    uint64_t completed_transactions;
    llam_runtime_stats_t runtime;
    leir_phase0_metrics_t metrics;
    uint64_t logical_io_submits;
    uint64_t logical_io_completions;
    uint64_t service_gap_p99_ns;
    uint64_t terminal_p99_ns;
    bool pending_path_valid;
} leir_bench_block_result_t;

typedef struct leir_bench_mode_total {
    uint64_t wall_ns;
    uint64_t cpu_ns;
    uint64_t checksum;
    bool checksum_set;
    llam_runtime_stats_t runtime;
    leir_phase0_metrics_t metrics;
    uint64_t logical_io_submits;
    uint64_t logical_io_completions;
    uint64_t service_gap_p99_ns;
    uint64_t terminal_p99_ns;
    bool pending_path_valid;
    unsigned blocks;
} leir_bench_mode_total_t;

static const unsigned leir_bench_abba_order[LEIR_BENCH_BLOCK_COUNT] = {
    0U, 1U, 1U, 0U,
    0U, 1U, 1U, 0U,
    0U, 1U, 1U, 0U,
    0U, 1U, 1U, 0U,
};

static uint64_t leir_bench_process_cpu_ns(void) {
#if LLAM_PLATFORM_WINDOWS
    FILETIME creation;
    FILETIME exit_time;
    FILETIME kernel;
    FILETIME user;
    ULARGE_INTEGER kernel_ticks;
    ULARGE_INTEGER user_ticks;

    if (!GetProcessTimes(
            GetCurrentProcess(),
            &creation,
            &exit_time,
            &kernel,
            &user)) {
        return 0U;
    }
    kernel_ticks.LowPart = kernel.dwLowDateTime;
    kernel_ticks.HighPart = kernel.dwHighDateTime;
    user_ticks.LowPart = user.dwLowDateTime;
    user_ticks.HighPart = user.dwHighDateTime;
    return (kernel_ticks.QuadPart + user_ticks.QuadPart) * UINT64_C(100);
#else
    struct timespec value;

    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) {
        return 0U;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
#endif
}

static uint64_t leir_bench_rotate_checksum(
    uint64_t value,
    unsigned rotation) {
    rotation &= 63U;
    return rotation == 0U
        ? value
        : (value << rotation) | (value >> (64U - rotation));
}

static uint64_t leir_bench_activations_for_connection(
    uint64_t activations,
    unsigned concurrency,
    unsigned connection) {
    uint64_t count = activations / concurrency;

    if ((uint64_t)connection < activations % concurrency) {
        count += 1U;
    }
    return count;
}

static uint64_t leir_bench_activation_begin(
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

static unsigned leir_bench_transactions_per_activation(
    const leir_bench_options_t *options) {
    if (options->workload == LEIR_BENCH_WORKLOAD_GRAPH_BREAK ||
        options->nodes == 1U) {
        return 1U;
    }
    return options->nodes / 2U;
}

static void leir_bench_metrics_add(
    leir_phase0_metrics_t *total,
    const leir_phase0_metrics_t *sample) {
    total->activations += sample->activations;
    total->effect_completions += sample->effect_completions;
    total->backend_submits += sample->backend_submits;
    total->direct_completions += sample->direct_completions;
    total->task_parks += sample->task_parks;
    total->terminal_publications += sample->terminal_publications;
    total->task_resumes_avoided += sample->task_resumes_avoided;
    total->fairness_resubmits += sample->fairness_resubmits;
    total->stale_completions += sample->stale_completions;
    total->heap_requests += sample->heap_requests;
    total->hot_allocations += sample->hot_allocations;
}

static void leir_bench_stats_add(
    llam_runtime_stats_t *total,
    const llam_runtime_stats_t *sample) {
    total->ctx_switches += sample->ctx_switches;
    total->parks += sample->parks;
    total->wakes += sample->wakes;
    total->io_submits += sample->io_submits;
    total->io_submit_calls += sample->io_submit_calls;
    total->io_submit_syscalls += sample->io_submit_syscalls;
    total->io_completions += sample->io_completions;
}

static void leir_bench_fail(
    leir_bench_block_state_t *state,
    const char *stage,
    int error_code) {
    unsigned expected = 0U;

    if (atomic_compare_exchange_strong_explicit(
            &state->failures,
            &expected,
            1U,
            memory_order_acq_rel,
            memory_order_acquire)) {
        state->first_error = error_code != 0 ? error_code : EIO;
        (void)snprintf(
            state->first_stage,
            sizeof(state->first_stage),
            "%s",
            stage);
    }
}

static void leir_bench_activation_complete(
    leir_bench_block_state_t *state) {
    uint64_t previous = atomic_fetch_sub_explicit(
        &state->remaining_activations,
        1U,
        memory_order_acq_rel);

    if (previous == 0U) {
        leir_bench_fail(
            state, "activation accounting underflow", EPROTO);
    }
}

static bool leir_bench_pending_ops_are_zero(void) {
    unsigned i;

    for (i = 0U; i < g_llam_runtime.active_nodes; i += 1U) {
        if (atomic_load_explicit(
                &g_llam_runtime.nodes[i].pending_ops,
                memory_order_acquire) != 0U) {
            return false;
        }
    }
    return true;
}

static int leir_bench_create_program(
    const leir_bench_options_t *options,
    leir_phase0_program_t **program_out) {
    static const leir_phase0_slot_kind_t slots[] = {
        LEIR_PHASE0_SLOT_FD,
        LEIR_PHASE0_SLOT_MUT_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
        LEIR_PHASE0_SLOT_I64,
    };
    leir_phase0_node_desc_t nodes[LEIR_PHASE0_MAX_NODES];
    leir_phase0_program_desc_t desc;
    unsigned effect_nodes =
        options->workload == LEIR_BENCH_WORKLOAD_GRAPH_BREAK
            ? 1U
            : options->nodes;
    uint16_t return_node = (uint16_t)effect_nodes;
    uint16_t fail_node = (uint16_t)(effect_nodes + 1U);
    unsigned i;

    if (program_out == NULL || effect_nodes == 0U ||
        effect_nodes + 2U > LEIR_PHASE0_MAX_NODES ||
        (effect_nodes > 1U && (effect_nodes & 1U) != 0U)) {
        errno = EINVAL;
        return -1;
    }
    *program_out = NULL;
    memset(nodes, 0, sizeof(nodes));
    for (i = 0U; i < effect_nodes; i += 1U) {
        bool is_read = (i & 1U) == 0U;

        nodes[i] = (leir_phase0_node_desc_t){
            .opcode = is_read
                          ? LEIR_PHASE0_OP_READ_EXACT
                          : LEIR_PHASE0_OP_WRITE_ALL,
            .fd_slot = 0U,
            .buffer_slot = 1U,
            .length_slot = is_read ? 2U : 3U,
            .result_slot = is_read ? 3U : 4U,
            .on_success =
                i + 1U == effect_nodes
                    ? return_node
                    : (uint16_t)(i + 1U),
            .on_eof = fail_node,
            .on_error = fail_node,
        };
    }
    nodes[return_node] = (leir_phase0_node_desc_t){
        .opcode = LEIR_PHASE0_OP_RETURN,
        .fd_slot = LEIR_PHASE0_NODE_NONE,
        .buffer_slot = LEIR_PHASE0_NODE_NONE,
        .length_slot = LEIR_PHASE0_NODE_NONE,
        .result_slot = effect_nodes == 1U ? 3U : 4U,
        .on_success = LEIR_PHASE0_NODE_NONE,
        .on_eof = LEIR_PHASE0_NODE_NONE,
        .on_error = LEIR_PHASE0_NODE_NONE,
    };
    nodes[fail_node] = (leir_phase0_node_desc_t){
        .opcode = LEIR_PHASE0_OP_FAIL,
        .fd_slot = LEIR_PHASE0_NODE_NONE,
        .buffer_slot = LEIR_PHASE0_NODE_NONE,
        .length_slot = LEIR_PHASE0_NODE_NONE,
        .result_slot = effect_nodes == 1U ? 3U : 4U,
        .on_success = LEIR_PHASE0_NODE_NONE,
        .on_eof = LEIR_PHASE0_NODE_NONE,
        .on_error = LEIR_PHASE0_NODE_NONE,
    };
    desc = (leir_phase0_program_desc_t){
        .nodes = nodes,
        .slot_kinds = slots,
        .node_count = effect_nodes + 2U,
        .slot_count = sizeof(slots) / sizeof(slots[0]),
        .entry_node = 0U,
    };
    return leir_phase0_program_create(&desc, program_out);
}

static void leir_bench_record_response(
    leir_bench_task_arg_t *arg,
    uint64_t activation,
    unsigned transaction,
    const unsigned char *buffer) {
    leir_bench_block_state_t *state = arg->state;
    unsigned transactions =
        leir_bench_transactions_per_activation(state->options);
    uint64_t sequence =
        activation * (uint64_t)transactions + transaction;
    uint64_t checksum = leir_test_payload_checksum(
        buffer,
        state->options->payload,
        arg->connection,
        sequence);

    state->task_results[arg->connection].checksum ^=
        leir_bench_rotate_checksum(
            checksum,
            (unsigned)(((uint64_t)arg->connection + sequence) & 63U));
}

static bool leir_bench_request_is_valid(
    const leir_bench_task_arg_t *arg,
    uint64_t activation,
    unsigned transaction,
    const unsigned char *buffer) {
    unsigned transactions =
        leir_bench_transactions_per_activation(arg->state->options);
    uint64_t sequence =
        activation * (uint64_t)transactions + transaction;

    return leir_test_payload_is_valid(
        buffer,
        arg->state->options->payload,
        arg->connection,
        sequence);
}

static void leir_bench_baseline_task(void *opaque) {
    leir_bench_task_arg_t *arg = opaque;
    leir_bench_block_state_t *state = arg->state;
    const leir_bench_options_t *options = state->options;
    unsigned char *buffer =
        state->buffers + (size_t)arg->connection * options->payload;
    llam_fd_t fd =
        leir_peer_process_server_fd(state->peer, arg->connection);
    unsigned transactions =
        leir_bench_transactions_per_activation(options);
    uint64_t local_activation;

    for (local_activation = 0U;
         local_activation < arg->activation_count;
         local_activation += 1U) {
        uint64_t started_ns = llam_now_ns();
        unsigned transaction;

        for (transaction = 0U;
             transaction < transactions;
             transaction += 1U) {
            if (leir_test_read_exact(
                    fd, buffer, options->payload) != 0) {
                leir_bench_fail(state, "baseline read", errno);
                return;
            }
            state->task_results[
                arg->connection].logical_io_submits += 1U;
            state->task_results[
                arg->connection].logical_io_completions += 1U;
            if (!leir_bench_request_is_valid(
                    arg,
                    local_activation,
                    transaction,
                    buffer)) {
                leir_bench_fail(
                    state, "baseline request sequence", EPROTO);
                return;
            }
            if (options->workload ==
                LEIR_BENCH_WORKLOAD_GRAPH_BREAK) {
                leir_test_transform_payload(buffer, options->payload);
            }
            if (leir_test_write_all(
                    fd, buffer, options->payload) != 0) {
                leir_bench_fail(state, "baseline write", errno);
                return;
            }
            state->task_results[
                arg->connection].logical_io_submits += 1U;
            state->task_results[
                arg->connection].logical_io_completions += 1U;
            leir_bench_record_response(
                arg, local_activation, transaction, buffer);
        }
        state->terminal_latencies[
            arg->activation_begin + local_activation] =
            llam_now_ns() - started_ns;
        leir_bench_activation_complete(state);
    }
}

static void leir_bench_candidate_task(void *opaque) {
    leir_bench_task_arg_t *arg = opaque;
    leir_bench_block_state_t *state = arg->state;
    const leir_bench_options_t *options = state->options;
    unsigned char *buffer =
        state->buffers + (size_t)arg->connection * options->payload;
    llam_fd_t fd =
        leir_peer_process_server_fd(state->peer, arg->connection);
    leir_phase0_instance_t *instance =
        &state->instances[arg->connection];
    leir_bench_task_result_t *task_result =
        &state->task_results[arg->connection];
    leir_phase0_run_opts_t run_options = {
        .inline_budget = options->inline_budget,
        .force_backend = true,
    };
    unsigned transactions =
        leir_bench_transactions_per_activation(options);
    uint64_t local_activation;

    for (local_activation = 0U;
         local_activation < arg->activation_count;
         local_activation += 1U) {
        leir_phase0_value_t values[5] = {0};
        leir_phase0_value_t values_out[5] = {0};
        leir_phase0_metrics_t metrics;
        uint64_t started_ns = llam_now_ns();
        unsigned transaction;

        values[0].fd = fd;
        values[1].buffer.data = buffer;
        values[1].buffer.size = options->payload;
        values[2].u64 = options->payload;
        values[3].i64 = -1;
        values[4].i64 = -1;
        memset(&metrics, 0, sizeof(metrics));
        if (leir_phase0_instance_bind(
                instance, values, 5U, &run_options) != 0 ||
            leir_phase0_instance_run(
                instance, values_out, 5U, &metrics) != 0) {
            leir_bench_fail(state, "candidate LEIR run", errno);
            return;
        }
        if (values_out[
                options->nodes == 1U ? 3U : 4U].i64 !=
            (int64_t)options->payload) {
            leir_bench_fail(
                state, "candidate terminal result", EPROTO);
            return;
        }
        task_result->logical_io_submits += 1U;
        task_result->logical_io_completions += 1U;
        if (options->nodes == 1U ||
            options->workload ==
                LEIR_BENCH_WORKLOAD_GRAPH_BREAK) {
            if (!leir_bench_request_is_valid(
                    arg, local_activation, 0U, buffer)) {
                leir_bench_fail(
                    state, "candidate request sequence", EPROTO);
                return;
            }
            if (options->workload ==
                LEIR_BENCH_WORKLOAD_GRAPH_BREAK) {
                leir_test_transform_payload(buffer, options->payload);
            }
            if (leir_test_write_all(
                    fd, buffer, options->payload) != 0) {
                leir_bench_fail(
                    state, "candidate boundary write", errno);
                return;
            }
            task_result->logical_io_submits += 1U;
            task_result->logical_io_completions += 1U;
            leir_bench_record_response(
                arg, local_activation, 0U, buffer);
        } else {
            if (!leir_bench_request_is_valid(
                    arg,
                    local_activation,
                    transactions - 1U,
                    buffer)) {
                leir_bench_fail(
                    state,
                    "candidate terminal sequence",
                    EPROTO);
                return;
            }
            for (transaction = 0U;
                 transaction < transactions;
                 transaction += 1U) {
                /*
                 * Every request in a fused activation is consumed in order.
                 * The peer validates every response; the terminal buffer is
                 * the final transaction and is independently checked here.
                 */
                leir_test_prepare_payload(
                    buffer,
                    options->payload,
                    arg->connection,
                    local_activation * (uint64_t)transactions +
                        transaction);
                leir_bench_record_response(
                    arg,
                    local_activation,
                    transaction,
                    buffer);
            }
        }
        leir_bench_metrics_add(&task_result->metrics, &metrics);
        state->terminal_latencies[
            arg->activation_begin + local_activation] =
            llam_now_ns() - started_ns;
        leir_bench_activation_complete(state);
    }
}

static void leir_bench_service_task(void *opaque) {
    leir_bench_block_state_t *state = opaque;
    uint64_t previous_ns = llam_now_ns();

    while (atomic_load_explicit(
               &state->remaining_activations,
               memory_order_acquire) != 0U &&
           atomic_load_explicit(
               &state->failures,
               memory_order_acquire) == 0U) {
        uint64_t now_ns;

        if (llam_sleep_ns(LEIR_BENCH_SERVICE_PERIOD_NS) != 0) {
            leir_bench_fail(state, "service task sleep", errno);
            return;
        }
        now_ns = llam_now_ns();
        if (state->service_gap_count <
            LEIR_BENCH_SERVICE_GAP_CAPACITY) {
            state->service_gaps[state->service_gap_count++] =
                now_ns - previous_ns;
        }
        previous_ns = now_ns;
    }
}

static void leir_bench_peer_start_task(void *opaque) {
    leir_bench_block_state_t *state = opaque;

    /*
     * Let every server task reach its first read before the peer can publish
     * data. This makes the benchmark exercise a real pending I/O boundary
     * instead of accidentally measuring only pre-filled ready sockets.
     */
    if (llam_sleep_ns(UINT64_C(1000000)) != 0) {
        leir_bench_fail(state, "peer start delay", errno);
        return;
    }
    if (leir_peer_process_signal_start(state->peer) != 0) {
        leir_bench_fail(state, "peer start signal", errno);
    }
}

static int leir_bench_compare_u64(
    const void *left,
    const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return a < b ? -1 : (a > b ? 1 : 0);
}

static uint64_t leir_bench_p99(
    uint64_t *samples,
    size_t count) {
    size_t index;

    if (samples == NULL || count == 0U) {
        return 0U;
    }
    qsort(samples, count, sizeof(samples[0]), leir_bench_compare_u64);
    index = (count * 99U + 99U) / 100U;
    if (index == 0U) {
        index = 1U;
    }
    if (index > count) {
        index = count;
    }
    return samples[index - 1U];
}

static bool leir_bench_block_integrity(
    const leir_bench_options_t *options,
    leir_bench_mode_t mode,
    uint64_t activations,
    const leir_bench_block_result_t *result) {
    uint64_t effect_nodes =
        options->workload == LEIR_BENCH_WORKLOAD_GRAPH_BREAK
            ? 1U
            : options->nodes;
    uint64_t transactions =
        leir_bench_transactions_per_activation(options);

    if (result->checksum == 0U ||
        result->completed_transactions != activations * transactions ||
        result->logical_io_submits == 0U ||
        result->logical_io_submits !=
            result->logical_io_completions ||
        result->runtime.parks <= 1U ||
        !result->pending_path_valid) {
        fprintf(
            stderr,
            "LEIR block counters mode=%s checksum=%016" PRIx64
            " completed=%" PRIu64 "/%" PRIu64
            " logical_io=%" PRIu64 "/%" PRIu64
            " runtime_io=%" PRIu64 "/%" PRIu64
            " ctx=%" PRIu64 " parks=%" PRIu64
            " workers=%u nodes=%u pending=%u\n",
            mode == LEIR_BENCH_MODE_BASELINE
                ? "baseline"
                : "candidate",
            result->checksum,
            result->completed_transactions,
            activations * transactions,
            result->logical_io_submits,
            result->logical_io_completions,
            result->runtime.io_submits,
            result->runtime.io_completions,
            result->runtime.ctx_switches,
            result->runtime.parks,
            result->runtime.active_workers,
            result->runtime.active_nodes,
            result->pending_path_valid ? 1U : 0U);
        return false;
    }
    if (mode == LEIR_BENCH_MODE_BASELINE) {
        return true;
    }
    if (result->metrics.activations != activations ||
        result->metrics.effect_completions !=
            activations * effect_nodes ||
        result->metrics.terminal_publications != activations ||
        result->metrics.task_resumes_avoided !=
            activations * (effect_nodes - 1U) ||
        result->metrics.backend_submits !=
            result->metrics.effect_completions -
                result->metrics.direct_completions ||
        result->metrics.heap_requests != 0U ||
        result->metrics.hot_allocations != 0U ||
        result->metrics.stale_completions != 0U) {
        fprintf(
            stderr,
            "LEIR candidate counters activations=%" PRIu64
            "/%" PRIu64 " effects=%" PRIu64 "/%" PRIu64
            " terminal=%" PRIu64 "/%" PRIu64
            " avoided=%" PRIu64 "/%" PRIu64
            " backend=%" PRIu64 " direct=%" PRIu64
            " heap=%" PRIu64 " hot=%" PRIu64
            " stale=%" PRIu64 "\n",
            result->metrics.activations,
            activations,
            result->metrics.effect_completions,
            activations * effect_nodes,
            result->metrics.terminal_publications,
            activations,
            result->metrics.task_resumes_avoided,
            activations * (effect_nodes - 1U),
            result->metrics.backend_submits,
            result->metrics.direct_completions,
            result->metrics.heap_requests,
            result->metrics.hot_allocations,
            result->metrics.stale_completions);
        return false;
    }
    return true;
}

static int leir_bench_run_block(
    const leir_bench_options_t *options,
    leir_bench_mode_t mode,
    uint64_t activations,
    leir_bench_block_result_t *result_out) {
    leir_peer_config_t peer_config;
    leir_peer_result_t peer_result;
    leir_bench_block_state_t state;
    leir_bench_task_arg_t *task_args = NULL;
    llam_task_t **tasks = NULL;
    llam_task_t *peer_starter = NULL;
    llam_task_t *service_task = NULL;
    llam_runtime_opts_t runtime_options;
    bool runtime_started = false;
    uint64_t wall_started;
    uint64_t cpu_started;
    uint64_t transactions;
    size_t activation_count;
    unsigned i;
    int status = -1;

    if (options == NULL || result_out == NULL ||
        activations == 0U || activations > SIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    activation_count = (size_t)activations;
    transactions = leir_bench_transactions_per_activation(options);
    if (activations > UINT64_MAX / transactions ||
        options->concurrency > SIZE_MAX / options->payload) {
        errno = EOVERFLOW;
        return -1;
    }
    memset(result_out, 0, sizeof(*result_out));
    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.remaining_activations, activations);
    state.options = options;
    state.mode = mode;

    state.buffers = calloc(options->concurrency, options->payload);
    state.task_results = calloc(
        options->concurrency, sizeof(state.task_results[0]));
    state.terminal_latencies = calloc(
        activation_count, sizeof(state.terminal_latencies[0]));
    state.service_gaps = calloc(
        LEIR_BENCH_SERVICE_GAP_CAPACITY,
        sizeof(state.service_gaps[0]));
    task_args = calloc(options->concurrency, sizeof(task_args[0]));
    tasks = calloc(options->concurrency, sizeof(tasks[0]));
    if (state.buffers == NULL || state.task_results == NULL ||
        state.terminal_latencies == NULL ||
        state.service_gaps == NULL ||
        task_args == NULL || tasks == NULL) {
        errno = ENOMEM;
        goto cleanup;
    }

    peer_config = (leir_peer_config_t){
        .workload = options->workload,
        .concurrency = options->concurrency,
        .payload = options->payload,
        .activations = activations,
        .transactions_per_activation = (unsigned)transactions,
    };
    if (leir_peer_process_start(&peer_config, &state.peer) != 0) {
        goto cleanup;
    }
    if (mode == LEIR_BENCH_MODE_CANDIDATE) {
        if (leir_bench_create_program(options, &state.program) != 0) {
            goto cleanup;
        }
        state.instances = calloc(
            options->concurrency, sizeof(state.instances[0]));
        if (state.instances == NULL) {
            errno = ENOMEM;
            goto cleanup;
        }
        for (i = 0U; i < options->concurrency; i += 1U) {
            if (leir_phase0_instance_init(
                    &state.instances[i],
                    sizeof(state.instances[i]),
                    state.program) != 0) {
                goto cleanup;
            }
        }
    }

    memset(&runtime_options, 0, sizeof(runtime_options));
    runtime_options.deterministic = 1U;
    runtime_options.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&runtime_options) != 0) {
        goto cleanup;
    }
    runtime_started = true;
    for (i = 0U; i < options->concurrency; i += 1U) {
        task_args[i] = (leir_bench_task_arg_t){
            .state = &state,
            .connection = i,
            .activation_begin = leir_bench_activation_begin(
                activations, options->concurrency, i),
            .activation_count =
                leir_bench_activations_for_connection(
                    activations, options->concurrency, i),
        };
        tasks[i] = llam_spawn(
            mode == LEIR_BENCH_MODE_BASELINE
                ? leir_bench_baseline_task
                : leir_bench_candidate_task,
            &task_args[i],
            NULL);
        if (tasks[i] == NULL) {
            leir_bench_fail(&state, "task spawn", errno);
            (void)llam_runtime_request_stop();
            goto cleanup;
        }
    }
    service_task = llam_spawn(
        leir_bench_service_task, &state, NULL);
    if (service_task == NULL) {
        leir_bench_fail(&state, "service task spawn", errno);
        (void)llam_runtime_request_stop();
        goto cleanup;
    }
    peer_starter = llam_spawn(
        leir_bench_peer_start_task, &state, NULL);
    if (peer_starter == NULL) {
        leir_bench_fail(&state, "peer starter spawn", errno);
        (void)llam_runtime_request_stop();
        goto cleanup;
    }
    wall_started = llam_now_ns();
    cpu_started = leir_bench_process_cpu_ns();
    if (llam_run() != 0) {
        leir_bench_fail(&state, "runtime run", errno);
    }
    result_out->cpu_ns =
        leir_bench_process_cpu_ns() - cpu_started;
    result_out->wall_ns = llam_now_ns() - wall_started;
    if (llam_runtime_collect_stats(&result_out->runtime) != 0) {
        leir_bench_fail(&state, "runtime stats", errno);
    }
    result_out->pending_path_valid =
        leir_bench_pending_ops_are_zero();
    for (i = 0U; i < options->concurrency; i += 1U) {
        if (tasks[i] != NULL && llam_join(tasks[i]) != 0) {
            leir_bench_fail(&state, "task join", errno);
        }
        tasks[i] = NULL;
        result_out->checksum ^=
            state.task_results[i].checksum;
        result_out->logical_io_submits +=
            state.task_results[i].logical_io_submits;
        result_out->logical_io_completions +=
            state.task_results[i].logical_io_completions;
        leir_bench_metrics_add(
            &result_out->metrics,
            &state.task_results[i].metrics);
    }
    if (peer_starter != NULL && llam_join(peer_starter) != 0) {
        leir_bench_fail(&state, "peer starter join", errno);
    }
    peer_starter = NULL;
    if (service_task != NULL && llam_join(service_task) != 0) {
        leir_bench_fail(&state, "service task join", errno);
    }
    service_task = NULL;
    if (atomic_load_explicit(
            &state.remaining_activations,
            memory_order_acquire) != 0U) {
        leir_bench_fail(
            &state, "activation accounting incomplete", EPROTO);
    }
    if (atomic_load_explicit(
            &state.failures, memory_order_acquire) != 0U) {
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
    result_out->terminal_p99_ns = leir_bench_p99(
        state.terminal_latencies, activation_count);
    result_out->service_gap_p99_ns = leir_bench_p99(
        state.service_gaps, state.service_gap_count);
    if (peer_result.status != 0 ||
        peer_result.checksum != result_out->checksum) {
        leir_bench_fail(&state, "peer checksum", EPROTO);
        errno = EPROTO;
        goto cleanup;
    }
    if (!leir_bench_block_integrity(
            options, mode, activations, result_out)) {
        leir_bench_fail(&state, "block integrity", EPROTO);
        errno = EPROTO;
        goto cleanup;
    }
    status = 0;

cleanup:
    if (status != 0 &&
        atomic_load_explicit(
            &state.failures, memory_order_acquire) != 0U) {
        fprintf(
            stderr,
            "LEIR benchmark block failed mode=%s stage=%s errno=%d\n",
            mode == LEIR_BENCH_MODE_BASELINE
                ? "baseline"
                : "candidate",
            state.first_stage,
            state.first_error);
    }
    if (state.peer != NULL) {
        leir_peer_process_abort(state.peer);
        state.peer = NULL;
    }
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    leir_phase0_program_destroy(state.program);
    free(state.instances);
    free(tasks);
    free(task_args);
    free(state.service_gaps);
    free(state.terminal_latencies);
    free(state.task_results);
    free(state.buffers);
    return status;
}

static void leir_bench_total_add(
    leir_bench_mode_total_t *total,
    const leir_bench_block_result_t *block) {
    total->wall_ns += block->wall_ns;
    total->cpu_ns += block->cpu_ns;
    leir_bench_stats_add(&total->runtime, &block->runtime);
    leir_bench_metrics_add(&total->metrics, &block->metrics);
    total->logical_io_submits += block->logical_io_submits;
    total->logical_io_completions +=
        block->logical_io_completions;
    if (!total->checksum_set) {
        total->checksum = block->checksum;
        total->checksum_set = true;
    }
    if (block->service_gap_p99_ns > total->service_gap_p99_ns) {
        total->service_gap_p99_ns = block->service_gap_p99_ns;
    }
    if (block->terminal_p99_ns > total->terminal_p99_ns) {
        total->terminal_p99_ns = block->terminal_p99_ns;
    }
    total->pending_path_valid =
        total->blocks == 0U
            ? block->pending_path_valid
            : total->pending_path_valid &&
                  block->pending_path_valid;
    total->blocks += 1U;
}

static int leir_bench_calibrate(
    const leir_bench_options_t *options,
    uint64_t *activations_out) {
    uint64_t activations = options->activations;
    unsigned iteration;

    if (activations < options->concurrency) {
        activations = options->concurrency;
    }
    for (iteration = 0U; iteration < 12U; iteration += 1U) {
        leir_bench_block_result_t baseline;
        leir_bench_block_result_t candidate;
        uint64_t slow_ns;
        uint64_t fast_ns;
        uint64_t target_block_ns =
            leir_bench_calibration_target_block_ns(
                options->min_mode_ns);
        uint64_t scale;

        if (leir_bench_run_block(
                options,
                LEIR_BENCH_MODE_BASELINE,
                activations,
                &baseline) != 0 ||
            leir_bench_run_block(
                options,
                LEIR_BENCH_MODE_CANDIDATE,
                activations,
                &candidate) != 0) {
            return -1;
        }
        slow_ns = baseline.wall_ns < candidate.wall_ns
            ? baseline.wall_ns
            : candidate.wall_ns;
        fast_ns = baseline.wall_ns > candidate.wall_ns
            ? baseline.wall_ns
            : candidate.wall_ns;
        if (slow_ns >= target_block_ns &&
            fast_ns >= target_block_ns) {
            *activations_out = activations;
            return 0;
        }
        if (slow_ns == 0U) {
            scale = 2U;
        } else {
            scale =
                (target_block_ns + slow_ns - 1U) / slow_ns;
            if (scale < 2U) {
                scale = 2U;
            }
            if (scale > 16U) {
                scale = 16U;
            }
        }
        if (activations > UINT64_MAX / scale) {
            errno = EOVERFLOW;
            return -1;
        }
        activations *= scale;
    }
    errno = ERANGE;
    return -1;
}

static int leir_bench_run(
    const leir_bench_options_t *options) {
    leir_bench_mode_total_t totals[LEIR_BENCH_MODE_COUNT];
    uint64_t activations;
    unsigned block;

    memset(totals, 0, sizeof(totals));
    if (leir_bench_calibrate(options, &activations) != 0) {
        fprintf(
            stderr,
            "LEIR benchmark calibration failed: errno=%d\n",
            errno);
        return 3;
    }
    for (block = 0U; block < LEIR_BENCH_BLOCK_COUNT; block += 1U) {
        leir_bench_block_result_t result;
        unsigned mode = leir_bench_abba_order[block];

        if (options->order == LEIR_BENCH_ORDER_BAAB) {
            mode ^= 1U;
        }
        if (leir_bench_run_block(
                options,
                (leir_bench_mode_t)mode,
                activations,
                &result) != 0) {
            return 3;
        }
        if (totals[mode].checksum_set &&
            totals[mode].checksum != result.checksum) {
            fputs(
                "LEIR benchmark checksum changed between blocks\n",
                stderr);
            return 3;
        }
        leir_bench_total_add(&totals[mode], &result);
    }
    if (totals[0].blocks != LEIR_BENCH_BLOCK_COUNT / 2U ||
        totals[1].blocks != LEIR_BENCH_BLOCK_COUNT / 2U ||
        totals[0].checksum != totals[1].checksum ||
        totals[0].wall_ns < options->min_mode_ns ||
        totals[1].wall_ns < options->min_mode_ns ||
        !totals[0].pending_path_valid ||
        !totals[1].pending_path_valid ||
        totals[0].wall_ns == 0U ||
        totals[1].wall_ns == 0U ||
        totals[0].cpu_ns == 0U ||
        totals[1].cpu_ns == 0U) {
        fprintf(
            stderr,
            "LEIR benchmark aggregate integrity failed: "
            "blocks=%u/%u checksum=%016" PRIx64 "/%016" PRIx64
            " wall=%" PRIu64 "/%" PRIu64 " min=%" PRIu64
            " cpu=%" PRIu64 "/%" PRIu64 " pending=%u/%u\n",
            totals[0].blocks,
            totals[1].blocks,
            totals[0].checksum,
            totals[1].checksum,
            totals[0].wall_ns,
            totals[1].wall_ns,
            options->min_mode_ns,
            totals[0].cpu_ns,
            totals[1].cpu_ns,
            totals[0].pending_path_valid ? 1U : 0U,
            totals[1].pending_path_valid ? 1U : 0U);
        return 3;
    }

    printf(
        "LEIR_PAIR version=1"
        " workload=%s"
        " nodes=%u"
        " concurrency=%u"
        " payload=%zu"
        " inline_budget=%u"
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
        " baseline_task_io_submits=%" PRIu64
        " candidate_task_io_submits=%" PRIu64
        " baseline_task_io_completions=%" PRIu64
        " candidate_task_io_completions=%" PRIu64
        " candidate_backend_submits=%" PRIu64
        " effect_completions=%" PRIu64
        " direct_completions=%" PRIu64
        " terminal_publications=%" PRIu64
        " resumes_avoided=%" PRIu64
        " fairness_resubmits=%" PRIu64
        " heap_requests=%" PRIu64
        " hot_allocations=%" PRIu64
        " baseline_checksum=%016" PRIx64
        " candidate_checksum=%016" PRIx64
        " pending_path_valid=1"
        " baseline_service_gap_p99_ns=%" PRIu64
        " candidate_service_gap_p99_ns=%" PRIu64
        " baseline_terminal_p99_ns=%" PRIu64
        " candidate_terminal_p99_ns=%" PRIu64
        " peer=%s"
        " cpu_scope=%s"
        " order=%s\n",
        leir_bench_workload_name(options->workload),
        options->nodes,
        options->concurrency,
        options->payload,
        options->inline_budget,
        activations,
        options->min_mode_ns,
        LEIR_BENCH_BLOCK_COUNT,
        totals[0].wall_ns,
        totals[1].wall_ns,
        totals[0].cpu_ns,
        totals[1].cpu_ns,
        (double)totals[0].wall_ns / (double)totals[1].wall_ns,
        (double)totals[1].cpu_ns / (double)totals[0].cpu_ns,
        totals[0].runtime.ctx_switches,
        totals[1].runtime.ctx_switches,
        totals[0].logical_io_submits,
        totals[1].logical_io_submits,
        totals[0].logical_io_completions,
        totals[1].logical_io_completions,
        totals[1].metrics.backend_submits,
        totals[1].metrics.effect_completions,
        totals[1].metrics.direct_completions,
        totals[1].metrics.terminal_publications,
        totals[1].metrics.task_resumes_avoided,
        totals[1].metrics.fairness_resubmits,
        totals[1].metrics.heap_requests,
        totals[1].metrics.hot_allocations,
        totals[0].checksum,
        totals[1].checksum,
        totals[0].service_gap_p99_ns,
        totals[1].service_gap_p99_ns,
        totals[0].terminal_p99_ns,
        totals[1].terminal_p99_ns,
        leir_peer_kind_name(),
        leir_peer_cpu_scope_name(),
        leir_bench_order_name(options->order));
    return 0;
}

int main(int argc, char **argv) {
    leir_bench_options_t options;

    if (leir_bench_parse_options(argc, argv, &options) != 0 ||
        (options.workload == LEIR_BENCH_WORKLOAD_GRAPH_BREAK &&
         options.nodes != 1U)) {
        fputs(
            "usage: bench_leir_phase0"
            " --workload socket_relay|framed_rpc|graph_break"
            " --nodes 1|2|4|8"
            " --concurrency 1..512"
            " --payload 64..16384"
            " --inline-budget 1..32"
            " --activations positive"
            " --min-mode-ms positive"
            " --order ABBA|BAAB\n",
            stderr);
        return 2;
    }
    return leir_bench_run(&options);
}
