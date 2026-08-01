// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

#include "lcrs_model.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum lcrs_bench_workload {
    LCRS_BENCH_BALANCED = 0,
    LCRS_BENCH_ONE_HOT,
    LCRS_BENCH_ROTATING_HOTSPOT,
    LCRS_BENCH_DRAIN_TAIL,
    LCRS_BENCH_UNEVEN_NUMA,
    LCRS_BENCH_OFFLINE_CHURN,
} lcrs_bench_workload_t;

typedef struct lcrs_bench_options {
    lcrs_bench_workload_t workload;
    lcrs_model_policy_t policy;
    uint32_t shards;
    uint32_t nodes;
    uint32_t iterations;
    uint64_t seed;
} lcrs_bench_options_t;

typedef struct lcrs_bench_totals {
    uint64_t thief_attempts;
    uint64_t selections;
    uint64_t probes;
    uint64_t candidate_tries;
    uint64_t fullscan_fallbacks;
    uint64_t remote_selections;
    uint64_t productive_units;
    uint64_t input_checksum;
    uint64_t decision_checksum;
} lcrs_bench_totals_t;

static uint64_t bench_mix64(uint64_t value) {
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31U;
    return value;
}

static const char *workload_name(lcrs_bench_workload_t workload) {
    switch (workload) {
        case LCRS_BENCH_BALANCED:
            return "balanced";
        case LCRS_BENCH_ONE_HOT:
            return "one_hot";
        case LCRS_BENCH_ROTATING_HOTSPOT:
            return "rotating_hotspot";
        case LCRS_BENCH_DRAIN_TAIL:
            return "drain_tail";
        case LCRS_BENCH_UNEVEN_NUMA:
            return "uneven_numa";
        case LCRS_BENCH_OFFLINE_CHURN:
            return "offline_churn";
        default:
            return NULL;
    }
}

static int parse_workload(const char *text, lcrs_bench_workload_t *out) {
    lcrs_bench_workload_t value;

    if (text == NULL || out == NULL) {
        return EINVAL;
    }
    for (value = LCRS_BENCH_BALANCED;
         value <= LCRS_BENCH_OFFLINE_CHURN;
         value = (lcrs_bench_workload_t)((int)value + 1)) {
        if (strcmp(text, workload_name(value)) == 0) {
            *out = value;
            return 0;
        }
    }
    return EINVAL;
}

static int parse_policy(const char *text, lcrs_model_policy_t *out) {
    lcrs_model_policy_t value;

    if (text == NULL || out == NULL) {
        return EINVAL;
    }
    for (value = LCRS_MODEL_GLOBAL_DEEPEST;
         value <= LCRS_MODEL_CODED_WITH_FULLSCAN;
         value = (lcrs_model_policy_t)((int)value + 1)) {
        if (strcmp(text, lcrs_model_policy_name(value)) == 0) {
            *out = value;
            return 0;
        }
    }
    return EINVAL;
}

static int parse_u64(const char *text, uint64_t maximum, uint64_t *out) {
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || text[0] == '\0' || text[0] == '-' || out == NULL) {
        return EINVAL;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > maximum) {
        return EINVAL;
    }
    *out = (uint64_t)value;
    return 0;
}

static int parse_options(int argc, char **argv, lcrs_bench_options_t *out) {
    lcrs_bench_options_t options = {
        LCRS_BENCH_BALANCED,
        LCRS_MODEL_GLOBAL_DEEPEST,
        32U,
        2U,
        1000U,
        UINT64_C(1),
    };
    int index;

    if (out == NULL) {
        return EINVAL;
    }
    for (index = 1; index < argc; index += 2) {
        uint64_t value;

        if (index + 1 >= argc) {
            return EINVAL;
        }
        if (strcmp(argv[index], "--workload") == 0) {
            if (parse_workload(argv[index + 1], &options.workload) != 0) {
                return EINVAL;
            }
        } else if (strcmp(argv[index], "--policy") == 0) {
            if (parse_policy(argv[index + 1], &options.policy) != 0) {
                return EINVAL;
            }
        } else if (strcmp(argv[index], "--shards") == 0) {
            if (parse_u64(argv[index + 1], 512U, &value) != 0) {
                return EINVAL;
            }
            options.shards = (uint32_t)value;
        } else if (strcmp(argv[index], "--nodes") == 0) {
            if (parse_u64(argv[index + 1], 512U, &value) != 0) {
                return EINVAL;
            }
            options.nodes = (uint32_t)value;
        } else if (strcmp(argv[index], "--iterations") == 0) {
            if (parse_u64(argv[index + 1], UINT32_MAX, &value) != 0) {
                return EINVAL;
            }
            options.iterations = (uint32_t)value;
        } else if (strcmp(argv[index], "--seed") == 0) {
            if (parse_u64(argv[index + 1], UINT64_MAX, &value) != 0) {
                return EINVAL;
            }
            options.seed = value;
        } else {
            return EINVAL;
        }
    }
    if (options.shards < 2U || options.nodes == 0U ||
        options.nodes > options.shards || options.iterations == 0U) {
        return EINVAL;
    }
    *out = options;
    return 0;
}

static uint32_t shard_node(const lcrs_bench_options_t *options,
                           uint32_t shard) {
    if (options->workload != LCRS_BENCH_UNEVEN_NUMA || options->nodes == 1U) {
        return shard % options->nodes;
    }
    if (shard < options->shards / 2U) {
        return 0U;
    }
    return 1U + ((shard - options->shards / 2U) % (options->nodes - 1U));
}

static void fill_workload(const lcrs_bench_options_t *options,
                          uint32_t iteration,
                          lcrs_model_shard_snapshot_t *snapshots,
                          uint64_t *checksum) {
    const uint32_t primary =
        (uint32_t)((options->seed + (uint64_t)iteration) % options->shards);
    uint32_t shard;

    for (shard = 0U; shard < options->shards; ++shard) {
        const uint64_t noise = bench_mix64(
            options->seed ^ ((uint64_t)iteration << 32U) ^ shard);
        uint32_t depth = 0U;
        uint8_t online = 1U;

        switch (options->workload) {
            case LCRS_BENCH_BALANCED:
                depth = 64U + (uint32_t)(noise % 17U);
                break;
            case LCRS_BENCH_ONE_HOT:
                depth = shard == (uint32_t)(options->seed % options->shards)
                            ? 4096U
                            : 0U;
                break;
            case LCRS_BENCH_ROTATING_HOTSPOT:
                if (shard == primary ||
                    shard == (primary + options->shards / 4U + 1U) %
                                 options->shards ||
                    shard == (primary + options->shards / 2U + 1U) %
                                 options->shards ||
                    shard == (primary + (3U * options->shards) / 4U + 1U) %
                                 options->shards) {
                    depth = 1024U + (uint32_t)(noise % 64U);
                }
                break;
            case LCRS_BENCH_DRAIN_TAIL:
                depth = shard == primary ? 64U : 0U;
                break;
            case LCRS_BENCH_UNEVEN_NUMA:
                depth = shard_node(options, shard) == 0U
                            ? 96U + (uint32_t)(noise % 32U)
                            : 24U + (uint32_t)(noise % 8U);
                break;
            case LCRS_BENCH_OFFLINE_CHURN:
                depth = 48U + (uint32_t)(noise % 33U);
                online = (uint8_t)(((shard + iteration) % 8U) != 0U);
                break;
            default:
                break;
        }
        snapshots[shard].shard_id = shard;
        snapshots[shard].depth = depth;
        snapshots[shard].online = online;
        snapshots[shard].paused = 0U;
        snapshots[shard].eligible = 1U;
        snapshots[shard].race_lost = 0U;
        *checksum = bench_mix64(*checksum ^ ((uint64_t)shard << 40U) ^
                                ((uint64_t)depth << 8U) ^ online);
    }
}

static uint32_t quantile_from_histogram(const uint64_t *histogram,
                                        uint32_t maximum,
                                        uint64_t samples,
                                        uint32_t numerator,
                                        uint32_t denominator) {
    const uint64_t target =
        samples == 0U
            ? 0U
            : 1U + ((samples - 1U) * (uint64_t)numerator) / denominator;
    uint64_t cumulative = 0U;
    uint32_t value;

    if (samples == 0U) {
        return 0U;
    }
    for (value = 0U; value <= maximum; ++value) {
        cumulative += histogram[value];
        if (cumulative >= target) {
            return value;
        }
    }
    return maximum;
}

static uint64_t wall_now_ns(void) {
    struct timespec now;

    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static uint64_t cpu_now_ns(void) {
    const clock_t now = clock();
    const uint64_t ticks = now == (clock_t)-1 ? 0U : (uint64_t)now;

    if (now == (clock_t)-1) {
        return 0U;
    }
    return (ticks / (uint64_t)CLOCKS_PER_SEC) * UINT64_C(1000000000) +
           ((ticks % (uint64_t)CLOCKS_PER_SEC) * UINT64_C(1000000000)) /
               (uint64_t)CLOCKS_PER_SEC;
}

static int run_benchmark(const lcrs_bench_options_t *options) {
    lcrs_model_shard_desc_t *descriptors = NULL;
    lcrs_model_shard_snapshot_t *snapshots = NULL;
    lcrs_model_topology_t *topology = NULL;
    uint32_t *fan_in = NULL;
    uint64_t *histogram = NULL;
    lcrs_model_config_t config;
    lcrs_bench_totals_t totals;
    uint64_t fan_in_samples = 0U;
    uint64_t wall_start;
    uint64_t cpu_start;
    uint64_t wall_end;
    uint64_t cpu_end;
    uint64_t wall_elapsed;
    uint64_t cpu_elapsed;
    uint32_t iteration;
    int rc = 1;

    memset(&totals, 0, sizeof(totals));
    totals.input_checksum = UINT64_C(0xcbf29ce484222325);
    totals.decision_checksum = UINT64_C(0x84222325cbf29ce4);
    descriptors = calloc(options->shards, sizeof(*descriptors));
    snapshots = calloc(options->shards, sizeof(*snapshots));
    fan_in = calloc(options->shards, sizeof(*fan_in));
    histogram = calloc((size_t)options->shards + 1U, sizeof(*histogram));
    if (descriptors == NULL || snapshots == NULL || fan_in == NULL ||
        histogram == NULL) {
        fprintf(stderr, "bench_lcrs_model: allocation failed\n");
        goto cleanup;
    }
    for (iteration = 0U; iteration < options->shards; ++iteration) {
        descriptors[iteration].shard_id = iteration;
        descriptors[iteration].node_id = shard_node(options, iteration);
    }
    lcrs_model_config_default(&config);
    config.small_runtime_cutoff = 0U;
    config.runtime_id = options->seed;
    if (lcrs_model_topology_create(descriptors,
                                   options->shards,
                                   &config,
                                   &topology) != 0) {
        fprintf(stderr, "bench_lcrs_model: topology construction failed\n");
        goto cleanup;
    }

    wall_start = wall_now_ns();
    cpu_start = cpu_now_ns();
    for (iteration = 0U; iteration < options->iterations; ++iteration) {
        const uint32_t thief_count = options->shards / 2U;
        uint32_t thief_index;
        uint32_t victim;

        memset(fan_in, 0, (size_t)options->shards * sizeof(*fan_in));
        fill_workload(options,
                      iteration,
                      snapshots,
                      &totals.input_checksum);
        for (thief_index = 0U; thief_index < thief_count; ++thief_index) {
            const uint32_t thief = (thief_index + iteration) % options->shards;
            lcrs_model_select_input_t input;
            lcrs_model_select_result_t result;

            memset(&input, 0, sizeof(input));
            input.policy = options->policy;
            input.thief_shard = thief;
            input.epoch = iteration;
            input.random_seed = options->seed;
            input.shards = snapshots;
            input.shard_count = options->shards;
            if (lcrs_model_select(topology, &input, &result) != 0) {
                fprintf(stderr, "bench_lcrs_model: selector rejected model input\n");
                goto cleanup;
            }
            totals.thief_attempts += 1U;
            totals.probes += result.probes;
            totals.candidate_tries += result.candidate_tries;
            totals.fullscan_fallbacks += result.used_fullscan;
            if (result.found == 0U) {
                continue;
            }
            totals.selections += 1U;
            fan_in[result.victim_shard] += 1U;
            if (descriptors[result.victim_shard].node_id !=
                descriptors[thief].node_id) {
                totals.remote_selections += 1U;
            }
            totals.decision_checksum = bench_mix64(
                totals.decision_checksum ^ ((uint64_t)iteration << 48U) ^
                ((uint64_t)thief << 24U) ^ result.victim_shard);
        }
        for (victim = 0U; victim < options->shards; ++victim) {
            const uint32_t selected = fan_in[victim];
            const uint32_t depth = snapshots[victim].depth;
            uint64_t unit;
            uint64_t possible;

            if (selected == 0U) {
                continue;
            }
            histogram[selected] += 1U;
            fan_in_samples += 1U;
            unit = depth < 4U ? 1U : depth / 4U;
            possible = unit * selected;
            totals.productive_units += possible < depth ? possible : depth;
        }
    }
    wall_end = wall_now_ns();
    cpu_end = cpu_now_ns();
    wall_elapsed = wall_end > wall_start ? wall_end - wall_start : 1U;
    cpu_elapsed = cpu_end > cpu_start ? cpu_end - cpu_start : 1U;
    printf("LCRS_SAMPLE version=1 workload=%s policy=%s shards=%u nodes=%u "
           "iterations=%u thief_attempts=%" PRIu64 " selections=%" PRIu64
           " probes=%" PRIu64 " candidate_tries=%" PRIu64
           " fullscan_fallbacks=%" PRIu64 " remote_selections=%" PRIu64
           " productive_units=%" PRIu64 " p50_fanin=%u p99_fanin=%u "
           "max_fanin=%u wall_ns=%" PRIu64 " cpu_ns=%" PRIu64
           " input_checksum=%016" PRIx64 " decision_checksum=%016" PRIx64
           "\n",
           workload_name(options->workload),
           lcrs_model_policy_name(options->policy),
           options->shards,
           options->nodes,
           options->iterations,
           totals.thief_attempts,
           totals.selections,
           totals.probes,
           totals.candidate_tries,
           totals.fullscan_fallbacks,
           totals.remote_selections,
           totals.productive_units,
           quantile_from_histogram(histogram,
                                   options->shards,
                                   fan_in_samples,
                                   50U,
                                   100U),
           quantile_from_histogram(histogram,
                                   options->shards,
                                   fan_in_samples,
                                   99U,
                                   100U),
           quantile_from_histogram(histogram,
                                   options->shards,
                                   fan_in_samples,
                                   100U,
                                   100U),
           wall_elapsed,
           cpu_elapsed,
           totals.input_checksum,
           totals.decision_checksum);
    rc = 0;

cleanup:
    lcrs_model_topology_destroy(topology);
    free(histogram);
    free(fan_in);
    free(snapshots);
    free(descriptors);
    return rc;
}

int main(int argc, char **argv) {
    lcrs_bench_options_t options;

    if (parse_options(argc, argv, &options) != 0) {
        fprintf(stderr,
                "usage: bench_lcrs_model --workload NAME --policy NAME "
                "--shards N --nodes N --iterations N --seed N\n");
        return 2;
    }
    return run_benchmark(&options);
}
