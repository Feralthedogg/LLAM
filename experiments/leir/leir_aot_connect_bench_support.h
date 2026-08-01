// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#ifndef LLAM_EXPERIMENTS_LEIR_AOT_CONNECT_BENCH_SUPPORT_H
#define LLAM_EXPERIMENTS_LEIR_AOT_CONNECT_BENCH_SUPPORT_H

#include "leir_aot_linux.h"
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
    BENCH_CANDIDATE_PORTABLE = 0,
    BENCH_CANDIDATE_NATIVE = 1,
} bench_candidate_t;

typedef enum bench_family {
    BENCH_FAMILY_TCP = 0,
    BENCH_FAMILY_UNIX = 1,
} bench_family_t;

typedef struct bench_options {
    bench_candidate_t candidate;
    bench_family_t family;
    unsigned concurrency;
    size_t payload;
    uint64_t activations;
    const char *ring_profile;
    bool candidate_set;
} bench_options_t;

typedef struct bench_metrics {
    uint64_t activations;
    uint64_t queue_publications;
    uint64_t prepared_sqes;
    uint64_t observed_cqes;
    uint64_t suppressed_success_cqes;
    uint64_t task_parks;
    uint64_t terminal_wakes;
    uint64_t hot_allocations;
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
    leir_phase0_instance_t portable_instance;
    leir_aot_linux_ticket_t *native_ticket;
    void *native_ticket_storage;
    void *module_storage;
    bool portable_initialized;
    bool native_initialized;
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
const char *family_name(bench_family_t family);
int parse_options(
    int argc,
    char **argv,
    bench_options_t *options);
bool bench_metrics_timing_is_valid(
    bench_candidate_t candidate,
    const bench_metrics_t *metrics);
uint64_t monotonic_ns(void);
uint64_t process_cpu_ns(void);
void fill_payload(
    unsigned char *payload,
    size_t size,
    uint64_t activation);
void fail_state(
    bench_state_t *state,
    const char *stage,
    int error_code);
int create_listener(bench_state_t *state);
int create_client(const bench_state_t *state);
void *peer_main(void *opaque);

#endif

#endif
