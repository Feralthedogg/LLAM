// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// See LICENSES/OLD-LICENSE/Apache-2.0.txt.

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "lcwe_model.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define LCWE_BENCH_MAX_INSTANCES UINT64_C(16777216)
#define LCWE_BENCH_MAX_ROUNDS UINT64_C(1000000)
#define LCWE_BENCH_MAX_WARMUP UINT64_C(100000)

enum option_bit {
    OPTION_WORKLOAD = 1U << 0U,
    OPTION_MODE = 1U << 1U,
    OPTION_INSTANCES = 1U << 2U,
    OPTION_SITES = 1U << 3U,
    OPTION_LANES = 1U << 4U,
    OPTION_ROUNDS = 1U << 5U,
    OPTION_WARMUP = 1U << 6U,
    OPTION_SEED = 1U << 7U,
    OPTION_ALL = (1U << 8U) - 1U,
};

typedef struct bench_config {
    lcwe_model_workload_t workload;
    lcwe_model_mode_t mode;
    size_t instances;
    unsigned sites;
    unsigned lanes;
    unsigned rounds;
    unsigned warmup;
    uint64_t seed;
    unsigned seen;
} bench_config_t;

static int option_error(const char *option, const char *reason) {
    fprintf(stderr,
            "bench_lcwe_model: %s: %s\n",
            option == NULL ? "arguments" : option,
            reason);
    return EINVAL;
}

static int parse_u64(const char *text, uint64_t maximum, uint64_t *out) {
    unsigned long long parsed;
    const unsigned char *cursor;
    char *end = NULL;

    if (text == NULL || text[0] == '\0' || out == NULL) {
        return EINVAL;
    }
    for (cursor = (const unsigned char *)text; *cursor != '\0'; ++cursor) {
        if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9') {
            return EINVAL;
        }
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        parsed > maximum) {
        return EINVAL;
    }
    *out = (uint64_t)parsed;
    return 0;
}

static bool lane_width_valid(unsigned lanes) {
    return lanes == 1U || lanes == 2U || lanes == 4U || lanes == 8U ||
           lanes == 16U || lanes == 32U;
}

static int claim_option(bench_config_t *config,
                        unsigned bit,
                        const char *option) {
    if ((config->seen & bit) != 0U) {
        return option_error(option, "duplicate option");
    }
    config->seen |= bit;
    return 0;
}

static int parse_option(bench_config_t *config,
                        const char *option,
                        const char *value) {
    uint64_t parsed;
    unsigned bit;
    int rc;

    if (strcmp(option, "--workload") == 0) {
        bit = OPTION_WORKLOAD;
    } else if (strcmp(option, "--mode") == 0) {
        bit = OPTION_MODE;
    } else if (strcmp(option, "--instances") == 0) {
        bit = OPTION_INSTANCES;
    } else if (strcmp(option, "--sites") == 0) {
        bit = OPTION_SITES;
    } else if (strcmp(option, "--lanes") == 0) {
        bit = OPTION_LANES;
    } else if (strcmp(option, "--rounds") == 0) {
        bit = OPTION_ROUNDS;
    } else if (strcmp(option, "--warmup") == 0) {
        bit = OPTION_WARMUP;
    } else if (strcmp(option, "--seed") == 0) {
        bit = OPTION_SEED;
    } else {
        return option_error(option, "unknown option");
    }
    rc = claim_option(config, bit, option);
    if (rc != 0) {
        return rc;
    }

    switch (bit) {
        case OPTION_WORKLOAD:
            rc = lcwe_model_parse_workload(value, &config->workload);
            break;
        case OPTION_MODE:
            rc = lcwe_model_parse_mode(value, &config->mode);
            break;
        case OPTION_INSTANCES:
            rc = parse_u64(value, LCWE_BENCH_MAX_INSTANCES, &parsed);
            if (rc == 0 && parsed != 0U) {
                config->instances = (size_t)parsed;
            } else {
                rc = EINVAL;
            }
            break;
        case OPTION_SITES:
            rc = parse_u64(value, LCWE_MODEL_MAX_SITES, &parsed);
            if (rc == 0 && parsed != 0U) {
                config->sites = (unsigned)parsed;
            } else {
                rc = EINVAL;
            }
            break;
        case OPTION_LANES:
            rc = parse_u64(value, LCWE_MODEL_MAX_LANES, &parsed);
            if (rc == 0 && lane_width_valid((unsigned)parsed)) {
                config->lanes = (unsigned)parsed;
            } else {
                rc = EINVAL;
            }
            break;
        case OPTION_ROUNDS:
            rc = parse_u64(value, LCWE_BENCH_MAX_ROUNDS, &parsed);
            if (rc == 0 && parsed != 0U) {
                config->rounds = (unsigned)parsed;
            } else {
                rc = EINVAL;
            }
            break;
        case OPTION_WARMUP:
            rc = parse_u64(value, LCWE_BENCH_MAX_WARMUP, &parsed);
            if (rc == 0) {
                config->warmup = (unsigned)parsed;
            }
            break;
        case OPTION_SEED:
            rc = parse_u64(value, UINT64_MAX, &config->seed);
            break;
        default:
            rc = EINVAL;
            break;
    }
    if (rc != 0) {
        return option_error(option, "invalid value");
    }
    return 0;
}

static int parse_arguments(int argc, char **argv, bench_config_t *config) {
    int i;

    memset(config, 0, sizeof(*config));
    if (argc <= 1 || (argc % 2) == 0) {
        return option_error(NULL, "expected option/value pairs");
    }
    for (i = 1; i < argc; i += 2) {
        int rc = parse_option(config, argv[i], argv[i + 1]);

        if (rc != 0) {
            return rc;
        }
    }
    if (config->seen != OPTION_ALL) {
        return option_error(NULL, "missing required option");
    }
    if (config->warmup >= config->rounds) {
        return option_error("--warmup", "must be smaller than --rounds");
    }
    if (config->mode == LCWE_MODEL_WAVE_AOSOA && config->sites != 1U) {
        return option_error("--sites", "wave_aosoa requires one site");
    }
    return 0;
}

static int scaled_ticks_to_ns(uint64_t ticks,
                              uint64_t numerator,
                              uint64_t denominator,
                              uint64_t *out) {
    uint64_t quotient;
    uint64_t remainder;
    uint64_t whole;
    uint64_t fraction;

    if (denominator == 0U || out == NULL) {
        return EOVERFLOW;
    }
    quotient = ticks / denominator;
    remainder = ticks % denominator;
    if ((quotient != 0U && numerator > UINT64_MAX / quotient) ||
        (remainder != 0U && numerator > UINT64_MAX / remainder)) {
        return EOVERFLOW;
    }
    whole = quotient * numerator;
    fraction = (remainder * numerator) / denominator;
    if (fraction > UINT64_MAX - whole) {
        return EOVERFLOW;
    }
    *out = whole + fraction;
    return 0;
}

static int wall_now_ns(uint64_t *out) {
#if defined(__APPLE__)
    static mach_timebase_info_data_t timebase;
    const uint64_t ticks = mach_continuous_time();

    if (timebase.denom == 0U && mach_timebase_info(&timebase) != KERN_SUCCESS) {
        return EIO;
    }
    return scaled_ticks_to_ns(ticks,
                              (uint64_t)timebase.numer,
                              (uint64_t)timebase.denom,
                              out);
#elif defined(_WIN32)
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    if (frequency.QuadPart == 0 &&
        !QueryPerformanceFrequency(&frequency)) {
        return EIO;
    }
    if (!QueryPerformanceCounter(&counter) || counter.QuadPart < 0 ||
        frequency.QuadPart <= 0) {
        return EIO;
    }
    return scaled_ticks_to_ns((uint64_t)counter.QuadPart,
                              UINT64_C(1000000000),
                              (uint64_t)frequency.QuadPart,
                              out);
#else
    struct timespec value;
    uint64_t seconds;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0 ||
        value.tv_sec < 0 || value.tv_nsec < 0 ||
        value.tv_nsec >= 1000000000L) {
        return EIO;
    }
    seconds = (uint64_t)value.tv_sec;
    if (seconds > UINT64_MAX / UINT64_C(1000000000)) {
        return EOVERFLOW;
    }
    *out = seconds * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
    return 0;
#endif
}

static int cpu_elapsed_ns(clock_t start, clock_t end, uint64_t *out) {
    uint64_t ticks;

    if (start == (clock_t)-1 || end == (clock_t)-1 || end < start) {
        return EIO;
    }
    ticks = (uint64_t)(end - start);
    return scaled_ticks_to_ns(ticks,
                              UINT64_C(1000000000),
                              (uint64_t)CLOCKS_PER_SEC,
                              out);
}

static int add_u64(uint64_t *total, uint64_t value) {
    if (value > UINT64_MAX - *total) {
        return EOVERFLOW;
    }
    *total += value;
    return 0;
}

static int multiply_u64(uint64_t lhs, uint64_t rhs, uint64_t *out) {
    if (lhs != 0U && rhs > UINT64_MAX / lhs) {
        return EOVERFLOW;
    }
    *out = lhs * rhs;
    return 0;
}

static int compare_u64(const void *lhs, const void *rhs) {
    const uint64_t left = *(const uint64_t *)lhs;
    const uint64_t right = *(const uint64_t *)rhs;

    return left < right ? -1 : left > right ? 1 : 0;
}

static int run_benchmark(const bench_config_t *config) {
    const unsigned measured_rounds = config->rounds - config->warmup;
    lcwe_model_batch_t *batch = NULL;
    lcwe_model_metrics_t metrics = {0};
    uint64_t *wall_samples = NULL;
    uint64_t *sorted_samples = NULL;
    uint64_t total_wall = 0U;
    uint64_t total_cpu = 0U;
    uint64_t operations = 0U;
    unsigned round;
    int rc;

    wall_samples = calloc(measured_rounds, sizeof(*wall_samples));
    sorted_samples = calloc(measured_rounds, sizeof(*sorted_samples));
    if (wall_samples == NULL || sorted_samples == NULL) {
        rc = ENOMEM;
        goto out;
    }
    rc = lcwe_model_batch_create(config->workload,
                                 config->mode,
                                 config->instances,
                                 config->sites,
                                 config->seed,
                                 &batch);
    if (rc != 0) {
        goto out;
    }

    for (round = 0U; round < config->warmup; ++round) {
        rc = lcwe_model_run_round(batch, config->lanes, &metrics);
        if (rc != 0) {
            goto out;
        }
    }
    memset(&metrics, 0, sizeof(metrics));

    for (round = 0U; round < measured_rounds; ++round) {
        uint64_t wall_start;
        uint64_t wall_end;
        uint64_t cpu_ns;
        clock_t cpu_start;
        clock_t cpu_end;

        rc = wall_now_ns(&wall_start);
        if (rc != 0) {
            goto out;
        }
        cpu_start = clock();
        rc = lcwe_model_run_round(batch, config->lanes, &metrics);
        cpu_end = clock();
        if (rc != 0) {
            goto out;
        }
        rc = wall_now_ns(&wall_end);
        if (rc != 0 || wall_end < wall_start) {
            rc = EIO;
            goto out;
        }
        rc = cpu_elapsed_ns(cpu_start, cpu_end, &cpu_ns);
        if (rc != 0) {
            goto out;
        }
        wall_samples[round] = wall_end - wall_start;
        if (wall_samples[round] == 0U ||
            add_u64(&total_wall, wall_samples[round]) != 0 ||
            add_u64(&total_cpu, cpu_ns) != 0) {
            rc = EOVERFLOW;
            goto out;
        }
    }
    if (total_cpu == 0U) {
        rc = ERANGE;
        goto out;
    }
    rc = multiply_u64((uint64_t)config->instances,
                      (uint64_t)measured_rounds,
                      &operations);
    if (rc != 0 || operations == 0U) {
        rc = EOVERFLOW;
        goto out;
    }

    memcpy(sorted_samples,
           wall_samples,
           measured_rounds * sizeof(*sorted_samples));
    qsort(sorted_samples,
          measured_rounds,
          sizeof(*sorted_samples),
          compare_u64);
    {
        const size_t p50_rank = ((size_t)measured_rounds + 1U) / 2U;
        const size_t p99_rank =
            ((size_t)measured_rounds * 99U + 99U) / 100U;
        const double wall_ns_per_op =
            (double)total_wall / (double)operations;
        const double cpu_ns_per_op =
            (double)total_cpu / (double)operations;
        const double p50_ns_per_op =
            (double)sorted_samples[p50_rank - 1U] /
            (double)config->instances;
        const double p99_ns_per_op =
            (double)sorted_samples[p99_rank - 1U] /
            (double)config->instances;
        const uint64_t checksum = lcwe_model_checksum(batch);

        if (checksum == 0U ||
            printf("[lcwe-model] workload=%s mode=%s instances=%zu "
                   "sites=%u lanes=%u rounds=%u warmup=%u "
                   "ops=%" PRIu64 " wall_ns=%" PRIu64
                   " cpu_ns=%" PRIu64
                   " wall_ns_per_op=%.2f cpu_ns_per_op=%.2f "
                   "p50_ns_per_op=%.2f p99_ns_per_op=%.2f "
                   "checksum=%016" PRIx64 "\n",
                   lcwe_model_workload_name(config->workload),
                   lcwe_model_mode_name(config->mode),
                   config->instances,
                   config->sites,
                   config->lanes,
                   config->rounds,
                   config->warmup,
                   operations,
                   total_wall,
                   total_cpu,
                   wall_ns_per_op,
                   cpu_ns_per_op,
                   p50_ns_per_op,
                   p99_ns_per_op,
                   checksum) < 0) {
            rc = EIO;
            goto out;
        }
    }
    rc = 0;

out:
    if (rc != 0) {
        fprintf(stderr,
                "bench_lcwe_model: benchmark failed: %s\n",
                strerror(rc));
    }
    lcwe_model_batch_destroy(batch);
    free(sorted_samples);
    free(wall_samples);
    return rc;
}

int main(int argc, char **argv) {
    bench_config_t config;
    int rc = parse_arguments(argc, argv, &config);

    if (rc != 0) {
        return 2;
    }
    return run_benchmark(&config) == 0 ? 0 : 1;
}
