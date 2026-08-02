// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#ifndef LLAM_EXPERIMENTS_LEIR_AOT_CONNECT_BENCH_SUPPORT_H
#define LLAM_EXPERIMENTS_LEIR_AOT_CONNECT_BENCH_SUPPORT_H

#include "leir_aot_linux.h"
#include "leir_aot_portable.h"
#include "leir_phase0_internal.h"
#include "llam/runtime.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__linux__)

#include <sys/socket.h>

enum {
    BENCH_TIMEOUT_MS = 15000,
    BENCH_MAX_CONCURRENCY = 256,
    BENCH_MAX_PAYLOAD = 65536,
};

typedef enum bench_candidate {
    BENCH_CANDIDATE_ORACLE = 0,
    BENCH_CANDIDATE_PORTABLE = 1,
    BENCH_CANDIDATE_LINUX = 2,
} bench_candidate_t;

typedef enum bench_process {
    BENCH_PROCESS_PORTABLE = 0,
    BENCH_PROCESS_LINUX = 1,
} bench_process_t;

typedef enum bench_transport {
    BENCH_TRANSPORT_TCP = 0,
    BENCH_TRANSPORT_UNIX = 1,
} bench_transport_t;

typedef struct bench_options {
    bench_candidate_t candidate;
    bench_process_t process;
    bench_transport_t transport;
    unsigned concurrency;
    size_t payload;
    uint64_t activations;
    uint64_t block;
    unsigned order;
    uint64_t seed;
    const char *ring_profile;
} bench_options_t;

typedef struct bench_metrics {
    uint64_t activations;
    uint64_t logical_operations;
    uint64_t interpreter_dispatches;
    uint64_t normalizations;
    uint64_t site_lookups;
    uint64_t parks;
    uint64_t wakes;
    uint64_t hot_allocations;
    uint64_t result_checksum;
    uint64_t queue_publications;
    uint64_t prepared_sqes;
    uint64_t observed_cqes;
    uint64_t suppressed_success_cqes;
    uint64_t submit_syscalls;
    uint64_t bind_ns;
    uint64_t execute_ns;
    uint64_t aot_prepare_ns;
    uint64_t aot_ring_ns;
    uint64_t aot_resume_ns;
} bench_metrics_t;

struct bench_state;

typedef struct bench_worker {
    struct bench_state *state;
    unsigned index;
    uint64_t activation_begin;
    uint64_t activation_count;
    unsigned char *payload;
    leir_phase0_instance_t oracle_instance;
    leir_aot_portable_ticket_t *portable_ticket;
    leir_aot_linux_ticket_t *linux_ticket;
    void *ticket_storage;
    void *module_storage;
    bool oracle_initialized;
    bool portable_initialized;
    bool linux_initialized;
    bench_metrics_t metrics;
} bench_worker_t;

typedef struct bench_peer {
    struct bench_state *state;
    unsigned char *observed;
    unsigned char *expected;
    unsigned char *seen;
    uint64_t completed;
    uint64_t checksum;
    int error_code;
} bench_peer_t;

typedef struct bench_state {
    bench_options_t options;
    leir_phase0_program_t *program;
    struct sockaddr_storage address;
    socklen_t address_length;
    int listener;
    bench_worker_t *workers;
    llam_task_t **tasks;
    uint64_t *latencies;
    bench_peer_t peer;
    atomic_uint failures;
    int first_error;
    char first_stage[80];
} bench_state_t;

const char *candidate_name(bench_candidate_t candidate);
const char *process_name(bench_process_t process);
const char *transport_name(bench_transport_t transport);
int parse_options(
    int argc,
    char **argv,
    bench_options_t *options);
bool bench_metrics_timing_is_valid(
    bench_candidate_t candidate,
    const bench_metrics_t *metrics);
uint64_t monotonic_ns(void);
uint64_t process_cpu_ns(void);
void bench_add_timing(
    uint64_t *total,
    uint64_t start,
    uint64_t finish);
void bench_add_timing_value(uint64_t *total, uint64_t value);
void *bench_allocate_aligned(size_t size, size_t requested_alignment);
void fill_payload(
    unsigned char *payload,
    size_t size,
    uint64_t activation,
    uint64_t seed);
uint64_t bench_result_receipt(
    uint64_t activation,
    int64_t connect_result,
    int64_t write_result);
int bench_metrics_apply_runtime_delta(
    bench_candidate_t candidate,
    bench_metrics_t *metrics,
    const llam_runtime_stats_t *before,
    const llam_runtime_stats_t *after);
void bench_print_sample(
    const bench_state_t *state,
    const bench_metrics_t *metrics,
    bool correctness,
    uint64_t wall_ns,
    uint64_t cpu_ns,
    uint64_t p50_ns,
    uint64_t p99_ns);
void fail_state(
    bench_state_t *state,
    const char *stage,
    int error_code);
int create_listener(bench_state_t *state);
int create_client(const bench_state_t *state);
void *peer_main(void *opaque);

#endif

#endif
