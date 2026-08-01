// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "generated/leir_aot_connect_write.h"
#include "leir_aot_connect_bench_support.h"
#include "leir_aot_linux.h"
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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int create_portable_program(
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

static int normalized_alignment(size_t requested, size_t *alignment_out) {
    size_t alignment = requested;

    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    if ((alignment & (alignment - 1U)) != 0U) {
        errno = EINVAL;
        return -1;
    }
    *alignment_out = alignment;
    return 0;
}

static void *allocate_aligned(size_t size, size_t requested_alignment) {
    size_t alignment;
    void *allocation = NULL;
    int result;

    if (normalized_alignment(requested_alignment, &alignment) != 0) {
        return NULL;
    }
    result = posix_memalign(&allocation, alignment, size);
    if (result != 0) {
        errno = result;
        return NULL;
    }
    memset(allocation, 0, size);
    return allocation;
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

static void add_portable_metrics(
    bench_metrics_t *total,
    const leir_phase0_metrics_t *sample) {
    total->activations += sample->activations;
    total->prepared_sqes += sample->backend_submits;
    total->observed_cqes += sample->effect_completions;
    total->task_parks += sample->task_parks;
    total->terminal_wakes += sample->terminal_publications;
    total->hot_allocations += sample->hot_allocations;
}

static void add_native_metrics(
    bench_metrics_t *total,
    const leir_aot_linux_metrics_t *sample) {
    total->activations += sample->activations;
    total->queue_publications += sample->queue_publications;
    total->prepared_sqes += sample->prepared_sqes;
    total->observed_cqes += sample->observed_cqes;
    total->suppressed_success_cqes +=
        sample->suppressed_success_cqes;
    total->task_parks += sample->task_parks;
    total->terminal_wakes += sample->terminal_wakes;
    total->hot_allocations += sample->hot_allocations;
}

static void run_portable_activation(
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

    values[0].fd = client;
    values[1].buffer.data = &state->address;
    values[1].buffer.size = (size_t)state->address_length;
    values[2].u64 = (uint64_t)state->address_length;
    values[3].i64 = -1;
    values[4].buffer.data = worker->payload;
    values[4].buffer.size = state->options.payload;
    values[5].u64 = state->options.payload;
    values[6].i64 = -1;
    memset(&metrics, 0, sizeof(metrics));
    if (leir_phase0_instance_bind(
            &worker->portable_instance,
            values,
            7U,
            &run_options) != 0 ||
        leir_phase0_instance_run(
            &worker->portable_instance,
            values_out,
            7U,
            &metrics) != 0 ||
        values_out[3].i64 != 0 ||
        values_out[6].i64 != (int64_t)state->options.payload) {
        fail_state(state, "portable activation", errno);
        return;
    }
    add_portable_metrics(&worker->metrics, &metrics);
    (void)activation;
}

static void run_native_activation(
    bench_worker_t *worker,
    int client,
    uint64_t activation) {
    bench_state_t *state = worker->state;
    leir_phase0_value_t values[7] = {0};
    leir_phase0_value_t values_out[7] = {0};
    leir_aot_resume_result_v1_t resume;
    leir_aot_linux_metrics_t metrics;

    values[0].fd = client;
    values[1].buffer.data = &state->address;
    values[1].buffer.size = (size_t)state->address_length;
    values[2].u64 = (uint64_t)state->address_length;
    values[3].i64 = -1;
    values[4].buffer.data = worker->payload;
    values[4].buffer.size = state->options.payload;
    values[5].u64 = state->options.payload;
    values[6].i64 = -1;
    memset(&resume, 0, sizeof(resume));
    memset(&metrics, 0, sizeof(metrics));
    if (leir_aot_linux_ticket_bind(
            worker->native_ticket, values, 7U) != 0 ||
        leir_aot_linux_ticket_run(
            worker->native_ticket,
            values_out,
            7U,
            &resume,
            &metrics) != 0 ||
        resume.action != LEIR_AOT_RESUME_RETURN ||
        resume.error_code != 0 ||
        values_out[3].i64 != 0 ||
        values_out[6].i64 != (int64_t)state->options.payload) {
        fail_state(state, "native activation", errno);
        return;
    }
    add_native_metrics(&worker->metrics, &metrics);
    (void)activation;
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
            worker->payload, state->options.payload, activation);
        if (state->options.candidate == BENCH_CANDIDATE_NATIVE) {
            run_native_activation(worker, client, activation);
        } else {
            run_portable_activation(worker, client, activation);
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
    size_t ticket_size = leir_aot_linux_ticket_size();
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
    if (state->options.candidate == BENCH_CANDIDATE_PORTABLE) {
        if (leir_phase0_instance_init(
                &worker->portable_instance,
                sizeof(worker->portable_instance),
                state->program) != 0) {
            return -1;
        }
        worker->portable_initialized = true;
        return 0;
    }

    worker->native_ticket_storage = allocate_aligned(
        ticket_size, leir_aot_linux_ticket_alignment());
    worker->module_storage = allocate_aligned(
        module_size,
        leir_aot_connect_write_module_v1.instance_alignment);
    if (worker->native_ticket_storage == NULL ||
        worker->module_storage == NULL ||
        leir_aot_linux_ticket_init(
            worker->native_ticket_storage,
            ticket_size,
            &leir_aot_connect_write_module_v1,
            worker->module_storage,
            module_size) != 0) {
        return -1;
    }
    worker->native_ticket = worker->native_ticket_storage;
    worker->native_initialized = true;
    return 0;
}

static void worker_destroy(bench_worker_t *worker) {
    if (worker->native_initialized) {
        (void)leir_aot_linux_ticket_destroy(worker->native_ticket);
    }
    free(worker->module_storage);
    free(worker->native_ticket_storage);
    free(worker->payload);
    memset(worker, 0, sizeof(*worker));
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return a < b ? -1 : (a > b ? 1 : 0);
}

static uint64_t p99_latency(uint64_t *latencies, size_t count) {
    size_t rank = (count * 99U + 99U) / 100U;

    qsort(latencies, count, sizeof(latencies[0]), compare_u64);
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
    total->queue_publications += sample->queue_publications;
    total->prepared_sqes += sample->prepared_sqes;
    total->observed_cqes += sample->observed_cqes;
    total->suppressed_success_cqes +=
        sample->suppressed_success_cqes;
    total->task_parks += sample->task_parks;
    total->terminal_wakes += sample->terminal_wakes;
    total->hot_allocations += sample->hot_allocations;
}

static bool native_unavailable(int error_code) {
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
    bench_metrics_t metrics;
    pthread_t peer_thread;
    uint64_t wall_started = 0U;
    uint64_t cpu_started = 0U;
    uint64_t wall_ns = 0U;
    uint64_t cpu_ns = 0U;
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
            "usage: bench_leir_aot_connect --candidate portable|native "
            "[--family tcp|unix] [--concurrency N] [--payload N] "
            "[--activations N]\n",
            stderr);
        return 2;
    }
    if (state.options.candidate == BENCH_CANDIDATE_PORTABLE &&
        create_portable_program(&state.program) != 0) {
        perror("create portable program");
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
        if (native_unavailable(errno)) {
            fprintf(
                stderr,
                "SKIP: runtime backend unavailable: %s\n",
                strerror(errno));
            result = 77;
        } else {
            perror("runtime init");
        }
        goto cleanup;
    }
    runtime_started = true;
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
    if (native_unavailable(state.first_error) &&
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
    correctness =
        atomic_load_explicit(
            &state.failures, memory_order_acquire) == 0U &&
        state.peer.error_code == 0 &&
        state.peer.completed == state.options.activations &&
        metrics.activations == state.options.activations;
    p99_ns = p99_latency(
        state.latencies, (size_t)state.options.activations);
    printf(
        "LEIR_AOT_CONNECT_SAMPLE version=1 candidate=%s family=%s "
        "concurrency=%u payload=%zu activations=%" PRIu64 " "
        "wall_ns=%" PRIu64 " cpu_ns=%" PRIu64 " p99_ns=%" PRIu64 " "
        "correctness=%u queue_publications=%" PRIu64 " "
        "prepared_sqes=%" PRIu64 " observed_cqes=%" PRIu64 " "
        "suppressed_success_cqes=%" PRIu64 " task_parks=%" PRIu64 " "
        "terminal_wakes=%" PRIu64 " hot_allocations=%" PRIu64 " "
        "checksum=%016" PRIx64 "\n",
        candidate_name(state.options.candidate),
        family_name(state.options.family),
        state.options.concurrency,
        state.options.payload,
        state.options.activations,
        wall_ns,
        cpu_ns,
        p99_ns,
        correctness ? 1U : 0U,
        metrics.queue_publications,
        metrics.prepared_sqes,
        metrics.observed_cqes,
        metrics.suppressed_success_cqes,
        metrics.task_parks,
        metrics.terminal_wakes,
        metrics.hot_allocations,
        state.peer.checksum);
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
