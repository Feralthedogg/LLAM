// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "generated/leir_aot_connect_write.h"
#include "leir_aot_connect_bench_support.h"
#include "leir_aot_linux.h"
#include "leir_aot_portable.h"
#include "leir_phase0.h"
#include "leir_phase0_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(__linux__)

int main(void) {
    fputs("SKIP: LEIR AOT CONNECT benchmark requires Linux\n", stderr);
    return 77;
}

#else

#include "io/linux/runtime_io_ring_profile_linux_internal.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int create_oracle_program(
    leir_phase0_program_t **program_out) {
    static const leir_phase0_slot_kind_t slots[] = {
        LEIR_PHASE0_SLOT_FD,
        LEIR_PHASE0_SLOT_CONST_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
        LEIR_PHASE0_SLOT_CONST_BUFFER,
        LEIR_PHASE0_SLOT_U64,
        LEIR_PHASE0_SLOT_I64,
    };
    static const leir_phase0_node_desc_t nodes[] = {
        {
            .opcode = LEIR_PHASE0_OP_CONNECT,
            .fd_slot = 0U,
            .buffer_slot = 1U,
            .length_slot = 2U,
            .result_slot = 3U,
            .on_success = 1U,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = 3U,
        },
        {
            .opcode = LEIR_PHASE0_OP_WRITE,
            .fd_slot = 0U,
            .buffer_slot = 4U,
            .length_slot = 5U,
            .result_slot = 6U,
            .on_success = 2U,
            .on_eof = 3U,
            .on_error = 3U,
        },
        {
            .opcode = LEIR_PHASE0_OP_RETURN,
            .fd_slot = LEIR_PHASE0_NODE_NONE,
            .buffer_slot = LEIR_PHASE0_NODE_NONE,
            .length_slot = LEIR_PHASE0_NODE_NONE,
            .result_slot = 6U,
            .on_success = LEIR_PHASE0_NODE_NONE,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = LEIR_PHASE0_NODE_NONE,
        },
        {
            .opcode = LEIR_PHASE0_OP_FAIL,
            .fd_slot = LEIR_PHASE0_NODE_NONE,
            .buffer_slot = LEIR_PHASE0_NODE_NONE,
            .length_slot = LEIR_PHASE0_NODE_NONE,
            .result_slot = 6U,
            .on_success = LEIR_PHASE0_NODE_NONE,
            .on_eof = LEIR_PHASE0_NODE_NONE,
            .on_error = LEIR_PHASE0_NODE_NONE,
        },
    };
    const leir_phase0_program_desc_t desc = {
        .nodes = nodes,
        .slot_kinds = slots,
        .node_count = sizeof(nodes) / sizeof(nodes[0]),
        .slot_count = sizeof(slots) / sizeof(slots[0]),
        .entry_node = 0U,
    };

    return leir_phase0_program_create(&desc, program_out);
}

static uint64_t activation_count_for_worker(
    const bench_state_t *state,
    unsigned worker) {
    uint64_t count =
        state->options.activations / state->options.concurrency;

    if ((uint64_t)worker <
        state->options.activations % state->options.concurrency) {
        count += 1U;
    }
    return count;
}

static uint64_t activation_begin_for_worker(
    const bench_state_t *state,
    unsigned worker) {
    uint64_t base =
        state->options.activations / state->options.concurrency;
    uint64_t remainder =
        state->options.activations % state->options.concurrency;

    return (uint64_t)worker * base +
           ((uint64_t)worker < remainder
                ? (uint64_t)worker
                : remainder);
}

static void add_oracle_metrics(
    bench_metrics_t *total,
    const leir_phase0_metrics_t *sample) {
    total->activations += sample->activations;
    total->logical_operations += sample->activations * 2U;
    total->interpreter_dispatches +=
        sample->backend_submits + sample->terminal_publications;
    total->parks += sample->task_parks;
    total->wakes += sample->terminal_publications;
    total->hot_allocations += sample->hot_allocations;
}

static void add_portable_metrics(
    bench_metrics_t *total,
    const leir_aot_portable_metrics_t *sample) {
    total->activations += sample->activations;
    total->logical_operations += sample->logical_operations;
    total->interpreter_dispatches += sample->interpreter_dispatches;
    total->normalizations += sample->normalizations;
    total->site_lookups += sample->site_lookups;
    total->hot_allocations += sample->hot_allocations;
}

static void add_linux_metrics(
    bench_metrics_t *total,
    const leir_aot_linux_metrics_t *sample) {
    total->activations += sample->activations;
    total->logical_operations += sample->logical_operations;
    total->interpreter_dispatches += sample->interpreter_dispatches;
    total->normalizations += sample->normalizations;
    total->site_lookups += sample->site_lookups;
    total->queue_publications += sample->queue_publications;
    total->prepared_sqes += sample->prepared_sqes;
    total->observed_cqes += sample->observed_cqes;
    total->suppressed_success_cqes +=
        sample->suppressed_success_cqes;
    total->parks += sample->task_parks;
    total->wakes += sample->terminal_wakes;
    total->hot_allocations += sample->hot_allocations;
    bench_add_timing_value(
        &total->aot_prepare_ns, sample->prepare_ns);
    bench_add_timing_value(&total->aot_ring_ns, sample->ring_ns);
    bench_add_timing_value(&total->aot_resume_ns, sample->resume_ns);
}

static void activation_values(
    bench_worker_t *worker,
    int client,
    leir_phase0_value_t values[7]) {
    bench_state_t *state = worker->state;

    values[0].fd = client;
    values[1].buffer.data = &state->address;
    values[1].buffer.size = (size_t)state->address_length;
    values[2].u64 = (uint64_t)state->address_length;
    values[3].i64 = -1;
    values[4].buffer.data = worker->payload;
    values[4].buffer.size = state->options.payload;
    values[5].u64 = state->options.payload;
    values[6].i64 = -1;
}

static void run_oracle_activation(
    bench_worker_t *worker,
    int client,
    uint64_t activation) {
    bench_state_t *state = worker->state;
    leir_phase0_value_t values[7] = {0};
    leir_phase0_value_t values_out[7] = {0};
    leir_phase0_metrics_t metrics;
    const leir_phase0_run_opts_t run_options = {
        .inline_budget = 8U,
        .force_backend = true,
    };
    uint64_t started_ns;
    int bind_error;
    int bind_result;
    int run_error;
    int run_result;

    activation_values(worker, client, values);
    memset(&metrics, 0, sizeof(metrics));
    started_ns = monotonic_ns();
    bind_result = leir_phase0_instance_bind(
        &worker->oracle_instance,
        values,
        7U,
        &run_options);
    bind_error = errno;
    bench_add_timing(
        &worker->metrics.bind_ns, started_ns, monotonic_ns());
    if (bind_result != 0) {
        fail_state(state, "oracle bind", bind_error);
        return;
    }
    started_ns = monotonic_ns();
    run_result = leir_phase0_instance_run(
        &worker->oracle_instance,
        values_out,
        7U,
        &metrics);
    run_error = errno;
    bench_add_timing(
        &worker->metrics.execute_ns, started_ns, monotonic_ns());
    if (run_result != 0 ||
        values_out[3].i64 != 0 ||
        values_out[6].i64 != (int64_t)state->options.payload) {
        fail_state(state, "oracle activation", run_error);
        return;
    }
    add_oracle_metrics(&worker->metrics, &metrics);
    worker->metrics.result_checksum ^= bench_result_receipt(
        activation, values_out[3].i64, values_out[6].i64);
}

static void run_portable_activation(
    bench_worker_t *worker,
    int client,
    uint64_t activation) {
    bench_state_t *state = worker->state;
    leir_phase0_value_t values[7] = {0};
    leir_phase0_value_t values_out[7] = {0};
    leir_aot_resume_result_v1_t resume;
    leir_aot_portable_metrics_t metrics;
    uint64_t started_ns;
    int bind_error;
    int bind_result;
    int run_error;
    int run_result;

    activation_values(worker, client, values);
    memset(&resume, 0, sizeof(resume));
    memset(&metrics, 0, sizeof(metrics));
    started_ns = monotonic_ns();
    bind_result = leir_aot_portable_ticket_bind(
        worker->portable_ticket, values, 7U);
    bind_error = errno;
    bench_add_timing(
        &worker->metrics.bind_ns, started_ns, monotonic_ns());
    if (bind_result != 0) {
        fail_state(state, "portable bind", bind_error);
        return;
    }
    started_ns = monotonic_ns();
    run_result = leir_aot_portable_ticket_run(
        worker->portable_ticket,
        values_out,
        7U,
        &resume,
        &metrics);
    run_error = errno;
    bench_add_timing(
        &worker->metrics.execute_ns, started_ns, monotonic_ns());
    if (run_result != 0 ||
        resume.action != LEIR_AOT_RESUME_RETURN ||
        resume.error_code != 0 ||
        values_out[3].i64 != 0 ||
        values_out[6].i64 != (int64_t)state->options.payload) {
        fail_state(state, "portable activation", run_error);
        return;
    }
    add_portable_metrics(&worker->metrics, &metrics);
    worker->metrics.result_checksum ^= bench_result_receipt(
        activation, values_out[3].i64, values_out[6].i64);
}

static void run_linux_activation(
    bench_worker_t *worker,
    int client,
    uint64_t activation) {
    bench_state_t *state = worker->state;
    leir_phase0_value_t values[7] = {0};
    leir_phase0_value_t values_out[7] = {0};
    leir_aot_resume_result_v1_t resume;
    leir_aot_linux_metrics_t metrics;
    uint64_t started_ns;
    int bind_error;
    int bind_result;
    int run_error;
    int run_result;

    activation_values(worker, client, values);
    memset(&resume, 0, sizeof(resume));
    memset(&metrics, 0, sizeof(metrics));
    started_ns = monotonic_ns();
    bind_result = leir_aot_linux_ticket_bind(
        worker->linux_ticket, values, 7U);
    bind_error = errno;
    bench_add_timing(
        &worker->metrics.bind_ns, started_ns, monotonic_ns());
    if (bind_result != 0) {
        fail_state(state, "linux bind", bind_error);
        return;
    }
    started_ns = monotonic_ns();
    run_result = leir_aot_linux_ticket_run(
        worker->linux_ticket,
        values_out,
        7U,
        &resume,
        &metrics);
    run_error = errno;
    bench_add_timing(
        &worker->metrics.execute_ns, started_ns, monotonic_ns());
    if (run_result != 0 ||
        resume.action != LEIR_AOT_RESUME_RETURN ||
        resume.error_code != 0 ||
        values_out[3].i64 != 0 ||
        values_out[6].i64 != (int64_t)state->options.payload) {
        fail_state(state, "linux activation", run_error);
        return;
    }
    add_linux_metrics(&worker->metrics, &metrics);
    worker->metrics.result_checksum ^= bench_result_receipt(
        activation, values_out[3].i64, values_out[6].i64);
}

static void candidate_task(void *opaque) {
    bench_worker_t *worker = opaque;
    bench_state_t *state = worker->state;
    uint64_t local;

    for (local = 0U; local < worker->activation_count; local += 1U) {
        uint64_t activation = worker->activation_begin + local;
        uint64_t started_ns = monotonic_ns();
        int client = create_client(state);

        if (client < 0) {
            fail_state(state, "client socket", errno);
            break;
        }
        fill_payload(
            worker->payload,
            state->options.payload,
            activation,
            state->options.seed);
        if (state->options.candidate == BENCH_CANDIDATE_ORACLE) {
            run_oracle_activation(worker, client, activation);
        } else if (state->options.candidate == BENCH_CANDIDATE_PORTABLE) {
            run_portable_activation(worker, client, activation);
        } else {
            run_linux_activation(worker, client, activation);
        }
        (void)close(client);
        state->latencies[activation] = monotonic_ns() - started_ns;
        if (atomic_load_explicit(
                &state->failures, memory_order_acquire) != 0U) {
            (void)llam_runtime_request_stop();
            break;
        }
    }
}

static int worker_init(
    bench_state_t *state,
    bench_worker_t *worker,
    unsigned index) {
    size_t ticket_size;
    size_t ticket_alignment;
    size_t module_size =
        leir_aot_connect_write_module_v1.instance_size;

    memset(worker, 0, sizeof(*worker));
    worker->state = state;
    worker->index = index;
    worker->activation_begin =
        activation_begin_for_worker(state, index);
    worker->activation_count =
        activation_count_for_worker(state, index);
    worker->payload = calloc(state->options.payload, 1U);
    if (worker->payload == NULL) {
        return -1;
    }
    if (state->options.candidate == BENCH_CANDIDATE_ORACLE) {
        if (leir_phase0_instance_init(
                &worker->oracle_instance,
                sizeof(worker->oracle_instance),
                state->program) != 0) {
            return -1;
        }
        worker->oracle_initialized = true;
        return 0;
    }

    if (state->options.candidate == BENCH_CANDIDATE_PORTABLE) {
        ticket_size = leir_aot_portable_ticket_size();
        ticket_alignment = leir_aot_portable_ticket_alignment();
    } else {
        ticket_size = leir_aot_linux_ticket_size();
        ticket_alignment = leir_aot_linux_ticket_alignment();
    }
    worker->ticket_storage = bench_allocate_aligned(
        ticket_size, ticket_alignment);
    worker->module_storage = bench_allocate_aligned(
        module_size,
        leir_aot_connect_write_module_v1.instance_alignment);
    if (worker->ticket_storage == NULL || worker->module_storage == NULL) {
        return -1;
    }
    if (state->options.candidate == BENCH_CANDIDATE_PORTABLE) {
        if (leir_aot_portable_ticket_init(
                worker->ticket_storage,
                ticket_size,
                &leir_aot_connect_write_module_v1,
                worker->module_storage,
                module_size,
                NULL) != 0) {
            return -1;
        }
        worker->portable_ticket = worker->ticket_storage;
        worker->portable_initialized = true;
    } else {
        if (leir_aot_linux_ticket_init(
                worker->ticket_storage,
                ticket_size,
                &leir_aot_connect_write_module_v1,
                worker->module_storage,
                module_size) != 0) {
            return -1;
        }
        worker->linux_ticket = worker->ticket_storage;
        worker->linux_initialized = true;
    }
    return 0;
}

static void worker_destroy(bench_worker_t *worker) {
    if (worker->portable_initialized) {
        (void)leir_aot_portable_ticket_destroy(worker->portable_ticket);
    }
    if (worker->linux_initialized) {
        (void)leir_aot_linux_ticket_destroy(worker->linux_ticket);
    }
    free(worker->module_storage);
    free(worker->ticket_storage);
    free(worker->payload);
    memset(worker, 0, sizeof(*worker));
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return a < b ? -1 : (a > b ? 1 : 0);
}

static uint64_t percentile_at(
    const uint64_t *latencies,
    size_t count,
    size_t percentile) {
    size_t rank = (count * percentile + 99U) / 100U;

    if (rank == 0U) {
        rank = 1U;
    }
    if (rank > count) {
        rank = count;
    }
    return latencies[rank - 1U];
}

static void metrics_add(
    bench_metrics_t *total,
    const bench_metrics_t *sample) {
    total->activations += sample->activations;
    total->logical_operations += sample->logical_operations;
    total->interpreter_dispatches += sample->interpreter_dispatches;
    total->normalizations += sample->normalizations;
    total->site_lookups += sample->site_lookups;
    total->parks += sample->parks;
    total->wakes += sample->wakes;
    total->hot_allocations += sample->hot_allocations;
    total->result_checksum ^= sample->result_checksum;
    total->queue_publications += sample->queue_publications;
    total->prepared_sqes += sample->prepared_sqes;
    total->observed_cqes += sample->observed_cqes;
    total->suppressed_success_cqes +=
        sample->suppressed_success_cqes;
    total->submit_syscalls += sample->submit_syscalls;
    bench_add_timing_value(&total->bind_ns, sample->bind_ns);
    bench_add_timing_value(&total->execute_ns, sample->execute_ns);
    bench_add_timing_value(
        &total->aot_prepare_ns, sample->aot_prepare_ns);
    bench_add_timing_value(&total->aot_ring_ns, sample->aot_ring_ns);
    bench_add_timing_value(&total->aot_resume_ns, sample->aot_resume_ns);
}

static bool backend_unavailable(int error_code) {
    return error_code == ENOTSUP || error_code == EAGAIN ||
           error_code == ENOSYS || error_code == EPERM ||
           error_code == EACCES;
}

static void state_destroy(bench_state_t *state) {
    unsigned i;

    if (state->listener >= 0) {
        (void)close(state->listener);
        state->listener = -1;
    }
    if (state->workers != NULL) {
        for (i = 0U; i < state->options.concurrency; i += 1U) {
            worker_destroy(&state->workers[i]);
        }
    }
    leir_phase0_program_destroy(state->program);
    free(state->peer.seen);
    free(state->peer.expected);
    free(state->peer.observed);
    free(state->latencies);
    free(state->tasks);
    free(state->workers);
}

int main(int argc, char **argv) {
    bench_state_t state;
    llam_runtime_opts_t runtime_options;
    llam_runtime_stats_t runtime_before;
    llam_runtime_stats_t runtime_after;
    bench_metrics_t metrics;
    pthread_t peer_thread;
    uint64_t wall_started = 0U;
    uint64_t cpu_started = 0U;
    uint64_t wall_ns = 0U;
    uint64_t cpu_ns = 0U;
    uint64_t p50_ns = 0U;
    uint64_t p99_ns = 0U;
    bool runtime_started = false;
    bool peer_started = false;
    bool timed = false;
    bool correctness;
    unsigned i;
    int result = 1;

    memset(&state, 0, sizeof(state));
    state.listener = -1;
    atomic_init(&state.failures, 0U);
    if (parse_options(argc, argv, &state.options) != 0) {
        fputs(
            "usage: bench_leir_aot_connect --candidate oracle|portable|linux "
            "--process portable|linux "
            "--ring-profile portable_control|submit_all|coop_taskrun|defer_taskrun "
            "--transport tcp|unix --block N --order 0|1 --seed N "
            "[--concurrency N] [--payload N] [--activations N]\n",
            stderr);
        return 2;
    }
    if (setenv(
            LLAM_LINUX_RESEARCH_RING_PROFILE_ENV,
            strcmp(state.options.ring_profile, "portable_control") == 0
                ? "submit_all"
                : state.options.ring_profile,
            1) != 0) {
        perror("set ring profile");
        goto cleanup;
    }
    if (state.options.candidate == BENCH_CANDIDATE_ORACLE &&
        create_oracle_program(&state.program) != 0) {
        perror("create oracle program");
        goto cleanup;
    }
    state.workers = calloc(
        state.options.concurrency, sizeof(state.workers[0]));
    state.tasks = calloc(
        state.options.concurrency, sizeof(state.tasks[0]));
    state.latencies = calloc(
        (size_t)state.options.activations,
        sizeof(state.latencies[0]));
    state.peer.observed = calloc(state.options.payload, 1U);
    state.peer.expected = calloc(state.options.payload, 1U);
    state.peer.seen = calloc(
        (size_t)state.options.activations, 1U);
    state.peer.state = &state;
    if (state.workers == NULL || state.tasks == NULL ||
        state.latencies == NULL || state.peer.observed == NULL ||
        state.peer.expected == NULL || state.peer.seen == NULL ||
        create_listener(&state) != 0) {
        perror("benchmark setup");
        goto cleanup;
    }
    for (i = 0U; i < state.options.concurrency; i += 1U) {
        if (worker_init(&state, &state.workers[i], i) != 0) {
            perror("worker init");
            goto cleanup;
        }
    }

    memset(&runtime_options, 0, sizeof(runtime_options));
    runtime_options.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    runtime_options.worker_min = 1U;
    runtime_options.worker_count = 1U;
    runtime_options.worker_max = 1U;
    runtime_options.task_prewarm_total = state.options.concurrency;
    runtime_options.stack_prewarm_total = state.options.concurrency;
    if (llam_runtime_init_ex(
            &runtime_options, sizeof(runtime_options)) != 0) {
        if (backend_unavailable(errno)) {
            fprintf(
                stderr,
                "SKIP: ring profile %s unavailable: %s\n",
                state.options.ring_profile,
                strerror(errno));
            result = 77;
        } else {
            perror("runtime init");
        }
        goto cleanup;
    }
    runtime_started = true;
    memset(&runtime_before, 0, sizeof(runtime_before));
    memset(&runtime_after, 0, sizeof(runtime_after));
    if (llam_runtime_collect_stats(&runtime_before) != 0) {
        perror("runtime stats before");
        goto cleanup;
    }
    for (i = 0U; i < state.options.concurrency; i += 1U) {
        state.tasks[i] = llam_spawn(
            candidate_task, &state.workers[i], NULL);
        if (state.tasks[i] == NULL) {
            fail_state(&state, "task spawn", errno);
            (void)llam_runtime_request_stop();
            (void)llam_run();
            goto finish_run;
        }
    }
    {
        int peer_error = pthread_create(
            &peer_thread, NULL, peer_main, &state.peer);

        if (peer_error != 0) {
            fail_state(&state, "peer create", peer_error);
            (void)llam_runtime_request_stop();
            (void)llam_run();
            goto finish_run;
        }
    }
    peer_started = true;
    wall_started = monotonic_ns();
    cpu_started = process_cpu_ns();
    timed = true;
    if (llam_run() != 0) {
        fail_state(&state, "runtime run", errno);
    }

finish_run:
    if (llam_runtime_collect_stats(&runtime_after) != 0) {
        fail_state(&state, "runtime stats after", errno);
    }
    for (i = 0U; i < state.options.concurrency; i += 1U) {
        if (state.tasks[i] != NULL &&
            llam_join(state.tasks[i]) != 0) {
            fail_state(&state, "task join", errno);
        }
        state.tasks[i] = NULL;
    }
    if (atomic_load_explicit(
            &state.failures, memory_order_acquire) != 0U &&
        state.listener >= 0) {
        (void)shutdown(state.listener, SHUT_RDWR);
    }
    if (peer_started) {
        int join_error = pthread_join(peer_thread, NULL);

        peer_started = false;
        if (join_error != 0) {
            fail_state(&state, "peer join", join_error);
        }
    }
    if (timed) {
        uint64_t wall_finished = monotonic_ns();
        uint64_t cpu_finished = process_cpu_ns();

        wall_ns = wall_finished > wall_started
            ? wall_finished - wall_started
            : 1U;
        cpu_ns = cpu_finished > cpu_started
            ? cpu_finished - cpu_started
            : 1U;
    }
    if (state.peer.error_code != 0) {
        fail_state(&state, "peer I/O", state.peer.error_code);
    }
    if (backend_unavailable(state.first_error) &&
        state.peer.completed == 0U) {
        fprintf(
            stderr,
            "SKIP: forced I/O backend unavailable: %s\n",
            strerror(state.first_error));
        result = 77;
        goto cleanup;
    }

    memset(&metrics, 0, sizeof(metrics));
    for (i = 0U; i < state.options.concurrency; i += 1U) {
        metrics_add(&metrics, &state.workers[i].metrics);
    }
    if (bench_metrics_apply_runtime_delta(
            state.options.candidate,
            &metrics,
            &runtime_before,
            &runtime_after) != 0) {
        fail_state(&state, "runtime stats delta", errno);
    }
    correctness =
        atomic_load_explicit(
            &state.failures, memory_order_acquire) == 0U &&
        state.peer.error_code == 0 &&
        state.peer.completed == state.options.activations &&
        metrics.activations == state.options.activations &&
        bench_metrics_timing_is_valid(
            state.options.candidate, &metrics);
    qsort(
        state.latencies,
        (size_t)state.options.activations,
        sizeof(state.latencies[0]),
        compare_u64);
    p50_ns = percentile_at(
        state.latencies, (size_t)state.options.activations, 50U);
    p99_ns = percentile_at(
        state.latencies, (size_t)state.options.activations, 99U);
    bench_print_sample(
        &state,
        &metrics,
        correctness,
        wall_ns,
        cpu_ns,
        p50_ns,
        p99_ns);
    if (!correctness) {
        fprintf(
            stderr,
            "benchmark failed at %s: errno=%d (%s), peer=%" PRIu64
            "/%" PRIu64 ", activations=%" PRIu64 "\n",
            state.first_stage[0] != '\0' ? state.first_stage : "integrity",
            state.first_error,
            state.first_error != 0
                ? strerror(state.first_error)
                : "counter mismatch",
            state.peer.completed,
            state.options.activations,
            metrics.activations);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (runtime_started) {
        (void)llam_runtime_request_stop();
        llam_runtime_shutdown();
    }
    if (peer_started) {
        if (state.listener >= 0) {
            (void)shutdown(state.listener, SHUT_RDWR);
        }
        (void)pthread_join(peer_thread, NULL);
    }
    state_destroy(&state);
    return result;
}

#endif
