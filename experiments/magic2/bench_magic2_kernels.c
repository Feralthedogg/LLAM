/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

#include "runtime_internal.h"
#include "runtime_magic2.h"
#include <inttypes.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define BENCH_NEON 1
#else
#define BENCH_NEON 0
#endif

#if defined(_MSC_VER)
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif

typedef struct kernel_environment {
    unsigned rounds;
} kernel_environment_t;

/* Unsigned arithmetic gives every implementation identical modulo-2^32 results. */
static uint32_t mix_step(uint32_t x) {
    x ^= x >> 16U;
    x *= UINT32_C(0x7feb352d);
    x ^= x >> 15U;
    x *= UINT32_C(0x846ca68b);
    return x ^ (x >> 16U);
}

static uint32_t reference_value(uint32_t x, unsigned rounds) {
    for (unsigned r = 0U; r < rounds; ++r) x = mix_step(x);
    return x;
}

/* Ordinary C baseline: compiled with the same optimization flags as all peers. */
static BENCH_NOINLINE int scalar_tile(const void *arg, void *const *ports,
                                      size_t begin, size_t end, uint32_t slot, void *scratch) {
    const kernel_environment_t *env = arg;
    uint32_t *restrict out = ports[0];
    const uint32_t *restrict in = ports[1];
    (void)slot;
    (void)scratch;
    for (size_t i = begin; i < end; ++i) out[i] = reference_value(in[i], env->rounds);
    return 0;
}

/* Independent scalar chains expose instruction-level parallelism and SLP. */
static BENCH_NOINLINE int unroll4_tile(const void *arg, void *const *ports,
                                       size_t begin, size_t end, uint32_t slot, void *scratch) {
    const kernel_environment_t *env = arg;
    uint32_t *restrict out = ports[0];
    const uint32_t *restrict in = ports[1];
    size_t i = begin;
    (void)slot;
    (void)scratch;
    for (; end - i >= 4U; i += 4U) {
        uint32_t a = in[i], b = in[i + 1U], c = in[i + 2U], d = in[i + 3U];
        for (unsigned r = 0U; r < env->rounds; ++r) {
            a = mix_step(a); b = mix_step(b); c = mix_step(c); d = mix_step(d);
        }
        out[i] = a; out[i + 1U] = b; out[i + 2U] = c; out[i + 3U] = d;
    }
    for (; i < end; ++i) out[i] = reference_value(in[i], env->rounds);
    return 0;
}

#if BENCH_NEON
static uint32x4_t vector_step(uint32x4_t x) {
    x = veorq_u32(x, vshrq_n_u32(x, 16));
    x = vmulq_u32(x, vdupq_n_u32(UINT32_C(0x7feb352d)));
    x = veorq_u32(x, vshrq_n_u32(x, 15));
    x = vmulq_u32(x, vdupq_n_u32(UINT32_C(0x846ca68b)));
    return veorq_u32(x, vshrq_n_u32(x, 16));
}

static BENCH_NOINLINE int neon4_tile(const void *arg, void *const *ports,
                                     size_t begin, size_t end, uint32_t slot, void *scratch) {
    const kernel_environment_t *env = arg;
    uint32_t *restrict out = ports[0];
    const uint32_t *restrict in = ports[1];
    size_t i = begin;
    (void)slot;
    (void)scratch;
    for (; end - i >= 4U; i += 4U) {
        uint32x4_t a = vld1q_u32(in + i);
        for (unsigned r = 0U; r < env->rounds; ++r) a = vector_step(a);
        vst1q_u32(out + i, a);
    }
    for (; i < end; ++i) out[i] = reference_value(in[i], env->rounds);
    return 0;
}

static BENCH_NOINLINE int neon16_tile(const void *arg, void *const *ports,
                                      size_t begin, size_t end, uint32_t slot, void *scratch) {
    const kernel_environment_t *env = arg;
    uint32_t *restrict out = ports[0];
    const uint32_t *restrict in = ports[1];
    size_t i = begin;
    (void)slot;
    (void)scratch;
    for (; end - i >= 16U; i += 16U) {
        uint32x4_t a = vld1q_u32(in + i), b = vld1q_u32(in + i + 4U);
        uint32x4_t c = vld1q_u32(in + i + 8U), d = vld1q_u32(in + i + 12U);
        for (unsigned r = 0U; r < env->rounds; ++r) {
            a = vector_step(a); b = vector_step(b);
            c = vector_step(c); d = vector_step(d);
        }
        vst1q_u32(out + i, a); vst1q_u32(out + i + 4U, b);
        vst1q_u32(out + i + 8U, c); vst1q_u32(out + i + 12U, d);
    }
    if (i < end) return neon4_tile(arg, ports, i, end, slot, scratch);
    return 0;
}
#endif

static const struct kernel_variant {
    const char *name;
    magic2_cpu_tile_fn tile;
    uint64_t features;
} variants[] = {
    {"scalar", scalar_tile, 0U},
    {"unroll4", unroll4_tile, 0U},
#if BENCH_NEON
    {"neon4", neon4_tile, MAGIC2_CPU_ARM_NEON},
    {"neon16", neon16_tile, MAGIC2_CPU_ARM_NEON},
#endif
};

#define VARIANT_COUNT (sizeof(variants) / sizeof(variants[0]))
#define GUARD_VALUE UINT32_C(0xa5a5a5a5)

static uint32_t input_value(size_t i, unsigned job) {
    return ((uint32_t)i * UINT32_C(0x9e3779b9)) ^ (UINT32_C(0xdeadbeef) + job);
}

/* This path runs without LLAM fibers, including in the kernel-only ASan build. */
static int kernel_self_test(void) {
    const size_t sizes[] = {0, 1, 3, 4, 5, 7, 8, 15, 16, 17, 63, 64, 65, 257, 4099};
    const size_t grains[] = {1, 3, 16, 64, 4093};
    const unsigned rounds[] = {0, 1, 3, 16};
    unsigned checks = 0U;
    if (reference_value(0U, 1U) != 0U || reference_value(1U, 1U) != UINT32_C(0x688990c0)) return 1;
    for (size_t s = 0U; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        size_t n = sizes[s];
        uint32_t *in = malloc((n + 10U) * sizeof(*in));
        uint32_t *out = malloc((n + 10U) * sizeof(*out));
        if (in == NULL || out == NULL) { free(in); free(out); return 1; }
        for (unsigned offset = 4U; offset <= 5U; ++offset) {
            for (size_t r = 0U; r < sizeof(rounds) / sizeof(rounds[0]); ++r) {
                kernel_environment_t env = {rounds[r]};
                for (size_t k = 0U; k < VARIANT_COUNT; ++k) {
                    for (size_t g = 0U; g < sizeof(grains) / sizeof(grains[0]); ++g) {
                        for (size_t i = 0U; i < n + 10U; ++i) {
                            in[i] = input_value(i, 0U); out[i] = GUARD_VALUE;
                        }
                        void *ports[] = {out + offset, in + offset};
                        for (size_t begin = 0U; begin < n;) {
                            size_t end = n - begin < grains[g] ? n : begin + grains[g];
                            if (variants[k].tile(&env, ports, begin, end, 0U, NULL) != 0) goto failed;
                            begin = end;
                        }
                        for (size_t i = 0U; i < n + 10U; ++i) {
                            uint32_t expected = i >= offset && i - offset < n ?
                                reference_value(input_value(i, 0U), env.rounds) : GUARD_VALUE;
                            if (out[i] != expected || in[i] != input_value(i, 0U)) goto failed;
                        }
                        ++checks;
                    }
                }
            }
        }
        free(in); free(out);
        continue;
failed:
        free(in); free(out);
        fputs("kernel exact-output, tail, or guard check failed\n", stderr);
        return 1;
    }
    printf("{\"self_test\":\"passed\",\"checks\":%u,\"kernels\":%zu}\n", checks, VARIANT_COUNT);
    return 0;
}

typedef struct run_counts {
    uint64_t routes[4];
    uint64_t selected[4];
    uint64_t buckets[3];
    uint64_t observations;
} run_counts_t;

typedef struct bench_state bench_state_t;
typedef struct bench_job {
    bench_state_t *state;
    uint32_t *input_storage, *output_storage, *input, *output;
    void *workspace;
    llam_task_t *task;
    llam_cancel_token_t *token;
    run_counts_t training, measured;
    uint64_t checksum;
    int failed;
} bench_job_t;

struct bench_state {
    kernel_environment_t environment;
    size_t elements, grain;
    unsigned iterations, tasks, warmup;
    int fixed;
    bool automatic, train;
    llam_runtime_t *runtime;
    llam_magic2_cpu_work_t *work;
    magic2_storage workspace;
    llam_channel_t *ready, *start;
    bench_job_t *jobs;
    uint64_t elapsed_ns, warmup_wall_ns;
    int failed;
};

static bool cancelled(void) {
    llam_task_t *g = g_llam_tls_task;
    return atomic_load_explicit(&g->owner_runtime->stop_requested, memory_order_acquire) ||
           atomic_load_explicit(&g->owner_runtime->shutdown_requested, memory_order_acquire) ||
           llam_task_cancel_token_is_cancelled(g);
}

static bool context_ready(void) {
    return g_llam_tls_task != NULL && g_llam_tls_shard != NULL &&
           g_llam_tls_task->owner_runtime == g_llam_tls_shard->runtime &&
           g_llam_tls_processor == &g_llam_tls_shard->processor &&
           atomic_load_explicit(&g_llam_tls_task->opaque_blocking_depth, memory_order_acquire) == 0U &&
           llam_worker_has_safe_managed_g(g_llam_tls_worker, g_llam_tls_processor, g_llam_tls_task);
}

static int fixed_run(bench_job_t *job, size_t kernel) {
    bench_state_t *s = job->state;
    void *ports[] = {job->output, job->input};
    /* Match the bridge's separate entry and pre-admission cancellation checks. */
    if (!context_ready() || cancelled() || cancelled()) return -1;
    for (size_t begin = 0U; begin < s->elements;) {
        if (cancelled()) return -1;
        if (begin != 0U) {
            llam_yield();
            if (!context_ready() || cancelled()) return -1;
        }
        size_t end = s->elements - begin < s->grain ? s->elements : begin + s->grain;
        if (variants[kernel].tile(&s->environment, ports, begin, end, 0U, NULL) != 0) return -1;
        begin = end;
    }
    return cancelled() ? -1 : 0;
}

static int baseline(void *arg, magic2_cpu_run_status *status, int *run_status) {
    int result = fixed_run(arg, 0U);
    (void)status;
    *run_status = result == 0 ? MAGIC2_OK : MAGIC2_ECANCELLED;
    if (result != 0) errno = ECANCELED;
    return result;
}

static int execute(bench_job_t *job, bool training) {
    bench_state_t *s = job->state;
    run_counts_t *counts = training ? &job->training : &job->measured;
    if (!s->automatic || (training && !s->train)) {
        size_t k = s->automatic ? 0U : (size_t)s->fixed;
        int result = fixed_run(job, k);
        if (result == 0) counts->selected[k] += 1U;
        return result;
    }
    magic2_region regions[] = {
        {job->output, s->elements * sizeof(*job->output)},
        {job->input, s->elements * sizeof(*job->input)}
    };
    llam_magic2_cpu_auto_info_t info;
    int status = MAGIC2_ENOTPROCESSED;
    int result = llam_magic2_cpu_auto(s->work, job, regions, 2U, job->workspace,
                                     s->workspace.bytes, NULL, &status, &info);
    if (result != 0 || status != MAGIC2_OK || info.route > LLAM_MAGIC2_CPU_AUTO_NO_CANDIDATE) return -1;
    counts->routes[info.route] += 1U;
    if (info.route == LLAM_MAGIC2_CPU_AUTO_FAMILY) {
        if (info.selected_index >= VARIANT_COUNT || info.bucket >= 3U || info.slot_budget != 1U) return -1;
        counts->selected[info.selected_index] += 1U;
        counts->buckets[info.bucket] += 1U;
        counts->observations += info.observation_valid != 0U;
    }
    return 0;
}

static void worker(void *arg) {
    bench_job_t *job = arg;
    bench_state_t *s = job->state;
    void *message = job;
    for (unsigned i = 0U; i < s->warmup; ++i) {
        if (execute(job, true) != 0) { job->failed = 1; break; }
    }
    if (llam_channel_send(s->ready, job) != 0 || llam_channel_recv_result(s->start, &message) != 0) {
        job->failed = 1;
        return;
    }
    for (unsigned i = 0U; !job->failed && i < s->iterations; ++i) {
        if (execute(job, false) != 0) { job->failed = 1; break; }
        job->checksum += job->output[((size_t)i * 17U) % s->elements];
    }
}

static void controller(void *arg) {
    bench_state_t *s = arg;
    void *message = s;
    s->ready = llam_channel_create(s->tasks);
    s->start = llam_channel_create(s->tasks);
    if (s->ready == NULL || s->start == NULL) { s->failed = 1; return; }
    for (unsigned i = 0U; i < s->tasks; ++i) {
        llam_spawn_opts_t options;
        if (llam_spawn_opts_init(&options, sizeof(options)) != 0 ||
            (s->jobs[i].token = llam_cancel_token_create()) == NULL) goto failed;
        options.cancel_token = s->jobs[i].token;
        s->jobs[i].task = llam_spawn_ex(worker, &s->jobs[i], &options, sizeof(options));
        if (s->jobs[i].task == NULL) goto failed;
    }
    uint64_t begin = llam_now_ns();
    for (unsigned i = 0U; i < s->tasks; ++i) {
        if (llam_channel_recv_result(s->ready, &message) != 0) goto failed;
    }
    s->warmup_wall_ns = llam_now_ns() - begin;
    begin = llam_now_ns();
    for (unsigned i = 0U; i < s->tasks; ++i) {
        if (llam_channel_send(s->start, s) != 0) goto failed;
    }
    for (unsigned i = 0U; i < s->tasks; ++i) {
        if (llam_join(s->jobs[i].task) != 0) goto failed;
        s->jobs[i].task = NULL;
    }
    s->elapsed_ns = llam_now_ns() - begin;
    for (unsigned i = 0U; i < s->tasks; ++i) {
        if (llam_cancel_token_destroy(s->jobs[i].token) != 0) goto failed;
        s->jobs[i].token = NULL;
    }
    if (llam_channel_destroy(s->ready) != 0) goto failed;
    s->ready = NULL;
    if (llam_channel_destroy(s->start) != 0) goto failed;
    s->start = NULL;
    return;
failed:
    s->failed = 1;
    (void)llam_runtime_request_stop();
}

static int prepare_family(bench_state_t *s) {
    magic2_cpu_plan_spec plans[4];
    magic2_port ports[2] = {{0}, {0}};
    for (unsigned i = 0U; i < 2U; ++i) {
        ports[i].bytes = s->elements * sizeof(uint32_t);
        ports[i].stride = sizeof(uint32_t);
        ports[i].alignment = _Alignof(uint32_t);
        ports[i].region = i;
        ports[i].access = i == 0U ? MAGIC2_BUFFER_WRITE : MAGIC2_BUFFER_READ;
        ports[i].present = 1U;
        ports[i].initialization = MAGIC2_PRESERVE;
    }
    for (size_t k = 0U; k < VARIANT_COUNT; ++k) {
        plans[k] = magic2_cpu_plan_spec_initializer();
        plans[k].flags = MAGIC2_CPU_PLAN_ALLOW_PARTIAL_FAILURE;
        plans[k].schedule = MAGIC2_CPU_SCHEDULE_CALLER;
        plans[k].failure_contract = MAGIC2_FAILURE_PARTIAL;
        plans[k].stable_id.low = k + 1U;
        plans[k].shape.operation_id.low = UINT64_C(0x4d324b45524e454c);
        plans[k].shape.semantic_id.low = s->environment.rounds + 1U;
        plans[k].shape.count = s->elements;
        plans[k].shape.port_count = plans[k].shape.region_count = 2U;
        plans[k].shape.ports = ports;
        plans[k].tile = variants[k].tile;
        plans[k].environment = &s->environment;
        plans[k].environment_bytes = sizeof(s->environment);
        plans[k].required_cpu_features = variants[k].features;
        plans[k].grain = s->grain;
    }
    llam_magic2_cpu_work_spec_t descriptor = {0};
    descriptor.kind = LLAM_MAGIC2_CPU_WORK_TRANSFORM;
    descriptor.plans = plans;
    descriptor.plan_count = VARIANT_COUNT;
    descriptor.baseline = baseline;
    /* Operation-count hint only; this is not a measured execution-time claim. */
    descriptor.estimated_cost_ns = (uint64_t)s->elements * s->environment.rounds;
    return llam_magic2_cpu_work_create(&descriptor, &s->work, &s->workspace);
}

static unsigned number(const char *text, unsigned limit) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(text, &end, 10);
    return errno != 0 || *text < '0' || *text > '9' || *end != '\0' || n > limit ? UINT_MAX : (unsigned)n;
}

static void print_counts(const char *name, const run_counts_t *c) {
    printf(",\"%s\":{\"routes\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],"
           "\"selected\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],"
           "\"buckets\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],\"observations\":%" PRIu64 "}",
           name, c->routes[0], c->routes[1], c->routes[2], c->routes[3],
           c->selected[0], c->selected[1], c->selected[2], c->selected[3],
           c->buckets[0], c->buckets[1], c->buckets[2], c->observations);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) return kernel_self_test();
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        printf("{\"kernels\":[");
        for (size_t k = 0U; k < VARIANT_COUNT; ++k) printf("%s\"%s\"", k == 0U ? "" : ",", variants[k].name);
        puts("]}");
        return 0;
    }
    if (argc != 8) {
        fputs("usage: bench_magic2_kernels scalar|unroll4|neon4|neon16|auto-cold|auto-warm elements grain iterations tasks rounds warmup\n", stderr);
        return 2;
    }
    bench_state_t s = {0};
    s.fixed = -1;
    for (size_t k = 0U; k < VARIANT_COUNT; ++k) if (strcmp(argv[1], variants[k].name) == 0) s.fixed = (int)k;
    s.automatic = strcmp(argv[1], "auto-cold") == 0 || strcmp(argv[1], "auto-warm") == 0;
    s.train = strcmp(argv[1], "auto-warm") == 0;
    s.elements = number(argv[2], 1048576U); s.grain = number(argv[3], 1048576U);
    s.iterations = number(argv[4], 1000000U); s.tasks = number(argv[5], 16U);
    s.environment.rounds = number(argv[6], 128U); s.warmup = number(argv[7], 256U);
    if ((!s.automatic && s.fixed < 0) || s.elements == 0U || s.elements == UINT_MAX ||
        s.grain == 0U || s.grain == UINT_MAX || s.iterations == 0U || s.iterations == UINT_MAX ||
        s.tasks == 0U || s.tasks == UINT_MAX || s.environment.rounds == UINT_MAX || s.warmup == UINT_MAX) return 2;
    int result = 1;
    uint64_t begin = llam_now_ns();
    if (s.automatic && prepare_family(&s) != MAGIC2_OK) goto done;
    uint64_t setup_ns = llam_now_ns() - begin;
    s.jobs = calloc(s.tasks, sizeof(*s.jobs));
    if (s.jobs == NULL) goto done;
    for (unsigned j = 0U; j < s.tasks; ++j) {
        bench_job_t *job = &s.jobs[j];
        job->state = &s;
        job->input_storage = malloc((s.elements + 2U) * sizeof(uint32_t));
        job->output_storage = malloc((s.elements + 2U) * sizeof(uint32_t));
        job->workspace = s.automatic ? malloc(s.workspace.bytes) : NULL;
        if (job->input_storage == NULL || job->output_storage == NULL ||
            (s.automatic && (job->workspace == NULL || s.workspace.alignment > _Alignof(max_align_t)))) goto done;
        /* One-element offset exercises unaligned vector starts on every run. */
        job->input = job->input_storage + 1U; job->output = job->output_storage + 1U;
        job->output_storage[0] = job->output_storage[s.elements + 1U] = GUARD_VALUE;
        for (size_t i = 0U; i < s.elements; ++i) { job->input[i] = input_value(i, j); job->output[i] = 0U; }
    }
    llam_runtime_opts_t opts;
    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) goto done;
    opts.deterministic = 1U; opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST; opts.preempt_mode = LLAM_PREEMPT_OFF;
    if (llam_runtime_create(&opts, sizeof(opts), &s.runtime) != 0) goto done;
    llam_task_t *root = llam_runtime_spawn_ex(s.runtime, controller, &s, NULL, 0U);
    llam_runtime_stats_t stats;
    if (root == NULL || llam_runtime_run_handle(s.runtime) != 0 || llam_join(root) != 0 || s.failed ||
        llam_runtime_collect_stats_ex_handle(s.runtime, &stats, sizeof(stats)) != 0 ||
        stats.active_workers != 1U || stats.blocking_calls != 0U) goto done;
    uint64_t checksum = 0U;
    run_counts_t training = {0}, measured = {0};
    for (unsigned j = 0U; j < s.tasks; ++j) {
        bench_job_t *job = &s.jobs[j];
        uint64_t expected_checksum = 0U;
        if (job->failed || job->output_storage[0] != GUARD_VALUE || job->output_storage[s.elements + 1U] != GUARD_VALUE) goto done;
        for (size_t i = 0U; i < s.elements; ++i) {
            if (job->input[i] != input_value(i, j) || job->output[i] != reference_value(input_value(i, j), s.environment.rounds)) goto done;
        }
        for (unsigned i = 0U; i < s.iterations; ++i) expected_checksum += reference_value(input_value(((size_t)i * 17U) % s.elements, j), s.environment.rounds);
        if (job->checksum != expected_checksum) goto done;
        checksum += job->checksum;
        for (unsigned i = 0U; i < 4U; ++i) {
            training.routes[i] += job->training.routes[i]; measured.routes[i] += job->measured.routes[i];
            training.selected[i] += job->training.selected[i]; measured.selected[i] += job->measured.selected[i];
        }
        for (unsigned i = 0U; i < 3U; ++i) { training.buckets[i] += job->training.buckets[i]; measured.buckets[i] += job->measured.buckets[i]; }
        training.observations += job->training.observations; measured.observations += job->measured.observations;
    }
    printf("{\"mode\":\"%s\",\"elements\":%zu,\"grain\":%zu,\"iterations\":%u,\"tasks\":%u,\"rounds\":%u,\"warmup\":%u,"
           "\"elapsed_ns\":%" PRIu64 ",\"plan_setup_ns\":%" PRIu64 ",\"warmup_wall_ns\":%" PRIu64 ","
           "\"workers\":%u,\"blocking_calls\":%" PRIu64 ",\"yields\":%" PRIu64 ",\"checksum\":\"%016" PRIx64 "\"",
           argv[1], s.elements, s.grain, s.iterations, s.tasks, s.environment.rounds, s.warmup,
           s.elapsed_ns, setup_ns, s.warmup_wall_ns, stats.active_workers, stats.blocking_calls, stats.yields, checksum);
    print_counts("training", &training); print_counts("measured", &measured); puts("}");
    result = 0;
done:
    if (result != 0) fprintf(stderr, "bench_magic2_kernels: setup, execution or exact-result check failed (errno=%d)\n", errno);
    if (s.runtime != NULL) llam_runtime_destroy(s.runtime);
    llam_magic2_cpu_work_release(&s.work);
    /* Failed setup/run may leave caller-owned handles after runtime drain. */
    llam_channel_t *channels[] = {s.ready, s.start};
    for (unsigned i = 0U; i < 2U; ++i) {
        if (channels[i] != NULL) {
            void *ignored;
            (void)llam_channel_close(channels[i]);
            while (llam_channel_try_recv_result(channels[i], &ignored) == 0) { }
            if (llam_channel_destroy(channels[i]) != 0) result = 1;
        }
    }
    if (s.jobs != NULL) {
        for (unsigned j = 0U; j < s.tasks; ++j) {
            if (s.jobs[j].token != NULL && llam_cancel_token_destroy(s.jobs[j].token) != 0) result = 1;
            free(s.jobs[j].input_storage); free(s.jobs[j].output_storage); free(s.jobs[j].workspace);
        }
    }
    free(s.jobs);
    return result;
}
