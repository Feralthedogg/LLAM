// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#include "lccf_model.h"
#include "lccf_platform.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum pair_order {
    PAIR_ORDER_ABBA = 0,
    PAIR_ORDER_BAAB = 1,
} pair_order_t;

typedef struct bench_options {
    lccf_model_workload_t workload;
    lccf_model_mode_t candidate;
    lccf_model_mode_t baseline;
    size_t instances;
    size_t frame_bytes;
    size_t cell_bytes;
    unsigned sites;
    unsigned budget;
    unsigned chain;
    unsigned producers;
    uint64_t min_mode_ns;
    uint64_t seed;
    pair_order_t order;
    bool pin_owner;
    unsigned owner_cpu;
} bench_options_t;

typedef struct block_time {
    uint64_t wall_ns;
    uint64_t cpu_ns;
} block_time_t;

typedef enum option_id {
    OPTION_WORKLOAD = 0,
    OPTION_CANDIDATE,
    OPTION_INSTANCES,
    OPTION_FRAME_BYTES,
    OPTION_CELL_BYTES,
    OPTION_SITES,
    OPTION_BUDGET,
    OPTION_CHAIN,
    OPTION_PRODUCERS,
    OPTION_MIN_MODE_MS,
    OPTION_SEED,
    OPTION_ORDER,
    OPTION_OWNER_CPU,
    OPTION_COUNT,
} option_id_t;

static int fail_message(const char *message) {
    fprintf(stderr, "[bench_lccf_model] %s\n", message);
    return 2;
}

static int parse_u64(const char *text, uint64_t *out) {
    uint64_t value = 0U;
    const unsigned char *cursor =
        (const unsigned char *)text;

    if (text == NULL || out == NULL || *text == '\0') {
        return EINVAL;
    }
    while (*cursor != '\0') {
        const unsigned digit =
            (unsigned)(*cursor - (unsigned char)'0');

        if (*cursor < (unsigned char)'0' ||
            *cursor > (unsigned char)'9' ||
            value >
                (UINT64_MAX - (uint64_t)digit) /
                    UINT64_C(10)) {
            return EINVAL;
        }
        value = value * UINT64_C(10) + (uint64_t)digit;
        cursor += 1;
    }
    *out = value;
    return 0;
}

static int option_id_from_name(const char *name,
                               option_id_t *out) {
    static const char *const names[OPTION_COUNT] = {
        "--workload",
        "--candidate",
        "--instances",
        "--frame-bytes",
        "--cell-bytes",
        "--sites",
        "--budget",
        "--chain",
        "--producers",
        "--min-mode-ms",
        "--seed",
        "--order",
        "--owner-cpu",
    };
    unsigned index;

    if (name == NULL || out == NULL) {
        return EINVAL;
    }
    for (index = 0U; index < OPTION_COUNT; ++index) {
        if (strcmp(name, names[index]) == 0) {
            *out = (option_id_t)index;
            return 0;
        }
    }
    return EINVAL;
}

static int assign_numeric_option(bench_options_t *options,
                                 option_id_t id,
                                 const char *value) {
    uint64_t parsed;

    if (parse_u64(value, &parsed) != 0) {
        return EINVAL;
    }
    switch (id) {
        case OPTION_INSTANCES:
            if (parsed == 0U || parsed > SIZE_MAX ||
                parsed > UINT32_MAX) {
                return EINVAL;
            }
            options->instances = (size_t)parsed;
            return 0;
        case OPTION_FRAME_BYTES:
            if (parsed != 64U && parsed != 128U &&
                parsed != 256U) {
                return EINVAL;
            }
            options->frame_bytes = (size_t)parsed;
            return 0;
        case OPTION_CELL_BYTES:
            if (parsed != 64U && parsed != 96U &&
                parsed != 128U) {
                return EINVAL;
            }
            options->cell_bytes = (size_t)parsed;
            return 0;
        case OPTION_SITES:
            if (parsed != 1U &&
                parsed != LCCF_MODEL_MAX_SITES) {
                return EINVAL;
            }
            options->sites = (unsigned)parsed;
            return 0;
        case OPTION_BUDGET:
            if (parsed == 0U ||
                parsed > LCCF_MODEL_MAX_DIRECT_BUDGET) {
                return EINVAL;
            }
            options->budget = (unsigned)parsed;
            return 0;
        case OPTION_CHAIN:
            if (parsed == 0U ||
                parsed > LCCF_MODEL_MAX_CHAIN_LENGTH) {
                return EINVAL;
            }
            options->chain = (unsigned)parsed;
            return 0;
        case OPTION_PRODUCERS:
            if (parsed != 2U) {
                return EINVAL;
            }
            options->producers = (unsigned)parsed;
            return 0;
        case OPTION_MIN_MODE_MS:
            if (parsed == 0U || parsed > UINT64_C(60000)) {
                return EINVAL;
            }
            options->min_mode_ns =
                parsed * UINT64_C(1000000);
            return 0;
        case OPTION_SEED:
            options->seed = parsed;
            return 0;
        case OPTION_OWNER_CPU:
            if (parsed > UINT_MAX) {
                return EINVAL;
            }
            options->pin_owner = true;
            options->owner_cpu = (unsigned)parsed;
            return 0;
        case OPTION_WORKLOAD:
        case OPTION_CANDIDATE:
        case OPTION_ORDER:
        case OPTION_COUNT:
        default:
            return EINVAL;
    }
}

static int assign_option(bench_options_t *options,
                         option_id_t id,
                         const char *value) {
    switch (id) {
        case OPTION_WORKLOAD:
            return lccf_model_parse_workload(
                value, &options->workload);
        case OPTION_CANDIDATE:
            if (lccf_model_parse_mode(
                    value, &options->candidate) != 0) {
                return EINVAL;
            }
            return lccf_model_candidate_baseline(
                options->candidate, &options->baseline);
        case OPTION_ORDER:
            if (strcmp(value, "abba") == 0) {
                options->order = PAIR_ORDER_ABBA;
                return 0;
            }
            if (strcmp(value, "baab") == 0) {
                options->order = PAIR_ORDER_BAAB;
                return 0;
            }
            return EINVAL;
        case OPTION_OWNER_CPU:
            if (strcmp(value, "none") == 0) {
                options->pin_owner = false;
                options->owner_cpu = 0U;
                return 0;
            }
            return assign_numeric_option(options, id, value);
        case OPTION_INSTANCES:
        case OPTION_FRAME_BYTES:
        case OPTION_CELL_BYTES:
        case OPTION_SITES:
        case OPTION_BUDGET:
        case OPTION_CHAIN:
        case OPTION_PRODUCERS:
        case OPTION_MIN_MODE_MS:
        case OPTION_SEED:
            return assign_numeric_option(options, id, value);
        case OPTION_COUNT:
        default:
            return EINVAL;
    }
}

static int parse_options(int argc,
                         char **argv,
                         bench_options_t *out) {
    bool seen[OPTION_COUNT] = {false};
    bench_options_t options;
    int index;

    if (out == NULL || argc != 1 + 2 * OPTION_COUNT) {
        return EINVAL;
    }
    memset(&options, 0, sizeof(options));
    for (index = 1; index < argc; index += 2) {
        option_id_t id;

        if (option_id_from_name(argv[index], &id) != 0 ||
            seen[id] ||
            assign_option(
                &options, id, argv[index + 1]) != 0) {
            return EINVAL;
        }
        seen[id] = true;
    }
    for (index = 0; index < OPTION_COUNT; ++index) {
        if (!seen[index]) {
            return EINVAL;
        }
    }
    *out = options;
    return 0;
}

static int add_duration(uint64_t *target, uint64_t increment) {
    if (target == NULL ||
        increment > UINT64_MAX - *target) {
        return EOVERFLOW;
    }
    *target += increment;
    return 0;
}

static int measure_block(lccf_model_batch_t *batch,
                         uint64_t rounds,
                         lccf_model_metrics_t *metrics,
                         block_time_t *out) {
    const uint64_t wall_start =
        lccf_platform_monotonic_ns();
    const uint64_t cpu_start =
        lccf_platform_process_cpu_ns();
    uint64_t round;
    uint64_t cpu_end;
    uint64_t wall_end;

    if (batch == NULL || rounds == 0U || metrics == NULL ||
        out == NULL || wall_start == 0U || cpu_start == 0U) {
        return EINVAL;
    }
    for (round = 0U; round < rounds; ++round) {
        const int rc =
            lccf_model_run_round(batch, metrics);

        if (rc != 0) {
            return rc;
        }
    }
    cpu_end = lccf_platform_process_cpu_ns();
    wall_end = lccf_platform_monotonic_ns();
    if (cpu_end < cpu_start || wall_end <= wall_start) {
        return ERANGE;
    }
    out->cpu_ns = cpu_end - cpu_start;
    out->wall_ns = wall_end - wall_start;
    return 0;
}

static int batches_match(lccf_model_batch_t *baseline,
                         lccf_model_batch_t *candidate,
                         uint64_t *out_checksum) {
    const uint64_t baseline_checksum =
        lccf_model_checksum(baseline);
    const uint64_t candidate_checksum =
        lccf_model_checksum(candidate);

    if (baseline_checksum == 0U ||
        baseline_checksum != candidate_checksum ||
        !lccf_model_batch_equal(baseline, candidate)) {
        return EPROTO;
    }
    if (out_checksum != NULL) {
        *out_checksum = baseline_checksum;
    }
    return 0;
}

static int calibrate_rounds(
    lccf_model_batch_t *baseline,
    lccf_model_batch_t *candidate,
    uint64_t min_mode_ns,
    uint64_t *out_rounds) {
    const uint64_t minimum_block_ns =
        min_mode_ns / UINT64_C(2) +
        min_mode_ns % UINT64_C(2);
    uint64_t rounds = UINT64_C(1);

    if (baseline == NULL || candidate == NULL ||
        min_mode_ns == 0U || out_rounds == NULL) {
        return EINVAL;
    }
    for (;;) {
        lccf_model_metrics_t baseline_metrics = {0};
        lccf_model_metrics_t candidate_metrics = {0};
        block_time_t baseline_time;
        block_time_t candidate_time;
        int rc;

        rc = lccf_model_batch_reset(baseline);
        if (rc == 0) {
            rc = lccf_model_batch_reset(candidate);
        }
        if (rc == 0) {
            rc = measure_block(
                baseline,
                rounds,
                &baseline_metrics,
                &baseline_time);
        }
        if (rc == 0) {
            rc = measure_block(
                candidate,
                rounds,
                &candidate_metrics,
                &candidate_time);
        }
        if (rc == 0) {
            rc = batches_match(
                baseline, candidate, NULL);
        }
        if (rc != 0) {
            return rc;
        }
        if (baseline_time.wall_ns >= minimum_block_ns &&
            candidate_time.wall_ns >= minimum_block_ns) {
            *out_rounds = rounds;
            return 0;
        }
        if (rounds > UINT64_MAX / UINT64_C(2)) {
            return EOVERFLOW;
        }
        rounds *= UINT64_C(2);
    }
}

static bool mode_uses_causal_cell(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_CAUSAL_CELL_QUEUE ||
           mode == LCCF_MODEL_FUSED_CAUSAL_CELL ||
           mode == LCCF_MODEL_BUDGETED_FUSED_CHAIN ||
           mode == LCCF_MODEL_REMOTE_CAUSAL_CELL;
}

static bool mode_is_remote(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_REMOTE_WAKER_QUEUE ||
           mode == LCCF_MODEL_REMOTE_CAUSAL_CELL;
}

static int verify_metrics(
    const bench_options_t *options,
    lccf_model_mode_t mode,
    uint64_t rounds,
    const lccf_model_metrics_t *metrics) {
    uint64_t completions;
    uint64_t callbacks;
    uint64_t expected_stale;
    uint64_t expected_remote;
    uint64_t expected_fairness;

    if (options == NULL || metrics == NULL ||
        rounds > UINT64_MAX / (uint64_t)options->instances) {
        return EOVERFLOW;
    }
    completions =
        rounds * (uint64_t)options->instances;
    if (completions >
        UINT64_MAX / (uint64_t)options->chain) {
        return EOVERFLOW;
    }
    callbacks =
        completions * (uint64_t)options->chain;
    if (mode_uses_causal_cell(mode) &&
        options->workload ==
            LCCF_MODEL_COMPLETION_TIMER_CANCEL &&
        completions > UINT64_MAX / UINT64_C(2)) {
        return EOVERFLOW;
    }
    expected_stale =
        mode_uses_causal_cell(mode) &&
                options->workload ==
                    LCCF_MODEL_COMPLETION_TIMER_CANCEL ?
            completions * UINT64_C(2) :
            0U;
    expected_remote =
        mode_is_remote(mode) ? completions : 0U;
    expected_fairness =
        options->workload ==
                LCCF_MODEL_COMPLETION_MIXED_FAIRNESS ?
            callbacks / UINT64_C(32) :
            0U;

    if (metrics->completions != completions ||
        metrics->claims != completions ||
        metrics->resume_calls != callbacks ||
        metrics->queue_pushes != metrics->queue_pops ||
        metrics->direct_calls >
            metrics->resume_calls ||
        metrics->queue_pops !=
            metrics->resume_calls - metrics->direct_calls ||
        metrics->stale_tickets != expected_stale ||
        metrics->remote_pushes != expected_remote ||
        metrics->fairness_samples != expected_fairness ||
        metrics->hot_allocations != 0U) {
        return EPROTO;
    }
    if ((expected_fairness == 0U &&
         metrics->fairness_p99_ns != 0U) ||
        (expected_fairness != 0U &&
         metrics->fairness_p99_ns == 0U)) {
        return EPROTO;
    }
    switch (mode) {
        case LCCF_MODEL_FUSED_CAUSAL_CELL:
            if (metrics->queue_pushes != 0U ||
                metrics->direct_calls != callbacks ||
                metrics->forced_escapes != 0U) {
                return EPROTO;
            }
            break;
        case LCCF_MODEL_BUDGETED_FUSED_CHAIN:
            if (metrics->queue_pushes !=
                metrics->forced_escapes) {
                return EPROTO;
            }
            break;
        case LCCF_MODEL_WAKER_QUEUE:
        case LCCF_MODEL_CAUSAL_CELL_QUEUE:
        case LCCF_MODEL_REMOTE_WAKER_QUEUE:
        case LCCF_MODEL_REMOTE_CAUSAL_CELL:
            if (metrics->direct_calls != 0U ||
                metrics->queue_pushes != callbacks ||
                metrics->forced_escapes != 0U) {
                return EPROTO;
            }
            break;
        default:
            return EINVAL;
    }
    return 0;
}

static int run_measured_pair(
    const bench_options_t *options,
    lccf_model_batch_t *baseline,
    lccf_model_batch_t *candidate,
    uint64_t rounds_per_block,
    lccf_model_metrics_t *baseline_metrics,
    lccf_model_metrics_t *candidate_metrics,
    block_time_t *baseline_total,
    block_time_t *candidate_total,
    uint64_t *out_checksum) {
    static const bool ABBA_CANDIDATE[4] = {
        false, true, true, false,
    };
    static const bool BAAB_CANDIDATE[4] = {
        true, false, false, true,
    };
    const bool *sequence =
        options->order == PAIR_ORDER_ABBA ?
            ABBA_CANDIDATE :
            BAAB_CANDIDATE;
    unsigned baseline_blocks = 0U;
    unsigned candidate_blocks = 0U;
    unsigned block;
    int rc;

    memset(baseline_metrics, 0, sizeof(*baseline_metrics));
    memset(candidate_metrics, 0, sizeof(*candidate_metrics));
    memset(baseline_total, 0, sizeof(*baseline_total));
    memset(candidate_total, 0, sizeof(*candidate_total));
    rc = lccf_model_batch_reset(baseline);
    if (rc == 0) {
        rc = lccf_model_batch_reset(candidate);
    }
    if (rc == 0) {
        rc = batches_match(baseline, candidate, NULL);
    }
    if (rc != 0) {
        return rc;
    }

    for (block = 0U; block < 4U; ++block) {
        const bool is_candidate = sequence[block];
        lccf_model_batch_t *batch =
            is_candidate ? candidate : baseline;
        lccf_model_metrics_t *metrics =
            is_candidate ?
                candidate_metrics :
                baseline_metrics;
        block_time_t *total =
            is_candidate ?
                candidate_total :
                baseline_total;
        block_time_t measured;

        rc = measure_block(
            batch, rounds_per_block, metrics, &measured);
        if (rc != 0 ||
            add_duration(
                &total->wall_ns, measured.wall_ns) != 0 ||
            add_duration(
                &total->cpu_ns, measured.cpu_ns) != 0) {
            return rc != 0 ? rc : EOVERFLOW;
        }
        if (is_candidate) {
            candidate_blocks += 1U;
        } else {
            baseline_blocks += 1U;
        }
        if (baseline_blocks == candidate_blocks) {
            rc = batches_match(
                baseline, candidate, out_checksum);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return baseline_blocks == 2U &&
                   candidate_blocks == 2U ?
               0 :
               EPROTO;
}

static int run_benchmark(const bench_options_t *options) {
    lccf_model_config_t baseline_config;
    lccf_model_config_t candidate_config;
    lccf_model_batch_t *baseline = NULL;
    lccf_model_batch_t *candidate = NULL;
    lccf_model_metrics_t baseline_metrics;
    lccf_model_metrics_t candidate_metrics;
    block_time_t baseline_time;
    block_time_t candidate_time;
    uint64_t rounds_per_block;
    uint64_t measured_rounds;
    uint64_t ops_per_mode;
    uint64_t checksum = 0U;
    double wall_speedup;
    double cpu_ratio;
    char affinity[64];
    int rc;
    int result = 2;

    memset(&baseline_config, 0, sizeof(baseline_config));
    baseline_config.workload = options->workload;
    baseline_config.mode = options->baseline;
    baseline_config.instance_count = options->instances;
    baseline_config.frame_bytes = options->frame_bytes;
    baseline_config.cell_bytes = options->cell_bytes;
    baseline_config.site_count = options->sites;
    baseline_config.direct_budget = options->budget;
    baseline_config.chain_length = options->chain;
    baseline_config.remote_producers = options->producers;
    baseline_config.seed = options->seed;
    candidate_config = baseline_config;
    candidate_config.mode = options->candidate;

    rc = lccf_model_batch_create(
        &baseline_config, &baseline);
    if (rc == 0) {
        rc = lccf_model_batch_create(
            &candidate_config, &candidate);
    }
    if (rc != 0) {
        fail_message("batch creation failed");
        goto out;
    }
    if (options->pin_owner) {
        const int affinity_rc =
            lccf_platform_pin_current_thread(
                options->owner_cpu);

        if (affinity_rc == 0) {
            rc = snprintf(
                affinity,
                sizeof(affinity),
                "cpu%u",
                options->owner_cpu);
        } else {
            rc = snprintf(
                affinity,
                sizeof(affinity),
                "failed%u_%d",
                options->owner_cpu,
                affinity_rc);
        }
    } else {
        rc = snprintf(affinity, sizeof(affinity), "none");
    }
    if (rc < 0 || (size_t)rc >= sizeof(affinity)) {
        fail_message("affinity description failed");
        goto out;
    }

    rc = calibrate_rounds(
        baseline,
        candidate,
        options->min_mode_ns,
        &rounds_per_block);
    if (rc != 0) {
        fail_message("paired calibration failed");
        goto out;
    }
    rc = run_measured_pair(
        options,
        baseline,
        candidate,
        rounds_per_block,
        &baseline_metrics,
        &candidate_metrics,
        &baseline_time,
        &candidate_time,
        &checksum);
    if (rc != 0) {
        fail_message("paired measurement failed");
        goto out;
    }
    if (rounds_per_block > UINT64_MAX / UINT64_C(2)) {
        fail_message("measured round count overflow");
        goto out;
    }
    measured_rounds = rounds_per_block * UINT64_C(2);
    if (measured_rounds >
        UINT64_MAX / (uint64_t)options->instances) {
        fail_message("operation count overflow");
        goto out;
    }
    ops_per_mode =
        measured_rounds * (uint64_t)options->instances;
    if (baseline_time.wall_ns < options->min_mode_ns ||
        candidate_time.wall_ns < options->min_mode_ns ||
        baseline_time.cpu_ns == 0U ||
        candidate_time.cpu_ns == 0U ||
        verify_metrics(
            options,
            options->baseline,
            measured_rounds,
            &baseline_metrics) != 0 ||
        verify_metrics(
            options,
            options->candidate,
            measured_rounds,
            &candidate_metrics) != 0 ||
        checksum == 0U) {
        fail_message("measured integrity gate failed");
        goto out;
    }
    wall_speedup =
        (double)baseline_time.wall_ns /
        (double)candidate_time.wall_ns;
    cpu_ratio =
        (double)candidate_time.cpu_ns /
        (double)baseline_time.cpu_ns;
    if (!isfinite(wall_speedup) ||
        !isfinite(cpu_ratio) ||
        wall_speedup <= 0.0 || cpu_ratio <= 0.0) {
        fail_message("non-finite paired ratio");
        goto out;
    }

    printf(
        "LCCF_PAIR version=1 workload=%s candidate=%s baseline=%s "
        "instances=%zu frame_bytes=%zu cell_bytes=%zu sites=%u "
        "budget=%u chain=%u producers=%u min_mode_ns=%" PRIu64 " "
        "rounds_per_block=%" PRIu64 " ops_per_mode=%" PRIu64 " "
        "baseline_wall_ns=%" PRIu64 " candidate_wall_ns=%" PRIu64 " "
        "baseline_cpu_ns=%" PRIu64 " candidate_cpu_ns=%" PRIu64 " "
        "wall_speedup=%.9f cpu_ratio=%.9f "
        "baseline_fair_p99_ns=%" PRIu64 " "
        "candidate_fair_p99_ns=%" PRIu64 " "
        "checksum=%016" PRIx64 " hot_allocations=%" PRIu64 " "
        "forced_escapes=%" PRIu64 " direct_calls=%" PRIu64 " "
        "order=%s affinity=%s\n",
        lccf_model_workload_name(options->workload),
        lccf_model_mode_name(options->candidate),
        lccf_model_mode_name(options->baseline),
        options->instances,
        options->frame_bytes,
        options->cell_bytes,
        options->sites,
        options->budget,
        options->chain,
        options->producers,
        options->min_mode_ns,
        rounds_per_block,
        ops_per_mode,
        baseline_time.wall_ns,
        candidate_time.wall_ns,
        baseline_time.cpu_ns,
        candidate_time.cpu_ns,
        wall_speedup,
        cpu_ratio,
        baseline_metrics.fairness_p99_ns,
        candidate_metrics.fairness_p99_ns,
        checksum,
        baseline_metrics.hot_allocations +
            candidate_metrics.hot_allocations,
        candidate_metrics.forced_escapes,
        candidate_metrics.direct_calls,
        options->order == PAIR_ORDER_ABBA ? "ABBA" : "BAAB",
        affinity);
    result = 0;

out:
    lccf_model_batch_destroy(candidate);
    lccf_model_batch_destroy(baseline);
    return result;
}

int main(int argc, char **argv) {
    bench_options_t options;

    if (parse_options(argc, argv, &options) != 0) {
        return fail_message("invalid command line");
    }
    return run_benchmark(&options);
}
