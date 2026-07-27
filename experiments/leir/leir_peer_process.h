#ifndef LLAM_EXPERIMENTS_LEIR_PEER_PROCESS_H
#define LLAM_EXPERIMENTS_LEIR_PEER_PROCESS_H

#include "leir_phase0.h"

#include <stddef.h>
#include <stdint.h>

typedef enum leir_bench_workload {
    LEIR_BENCH_WORKLOAD_SOCKET_RELAY = 0,
    LEIR_BENCH_WORKLOAD_FRAMED_RPC = 1,
    LEIR_BENCH_WORKLOAD_GRAPH_BREAK = 2,
} leir_bench_workload_t;

typedef enum leir_bench_order {
    LEIR_BENCH_ORDER_ABBA = 0,
    LEIR_BENCH_ORDER_BAAB = 1,
} leir_bench_order_t;

typedef enum leir_peer_socket_kind {
    LEIR_PEER_SOCKET_STREAM = 0,
    LEIR_PEER_SOCKET_SEQPACKET = 1,
} leir_peer_socket_kind_t;

typedef struct leir_bench_options {
    leir_bench_workload_t workload;
    unsigned nodes;
    unsigned concurrency;
    size_t payload;
    unsigned inline_budget;
    uint64_t activations;
    uint64_t min_mode_ns;
    leir_bench_order_t order;
} leir_bench_options_t;

int leir_bench_parse_options(
    int argc,
    char *const argv[],
    leir_bench_options_t *out);
uint64_t leir_bench_calibration_target_block_ns(
    uint64_t min_mode_ns);
const char *leir_bench_workload_name(
    leir_bench_workload_t workload);
const char *leir_bench_order_name(leir_bench_order_t order);

typedef struct leir_peer_config {
    leir_bench_workload_t workload;
    unsigned concurrency;
    size_t payload;
    uint64_t activations;
    unsigned transactions_per_activation;
    unsigned warmup_transactions_per_connection;
    leir_peer_socket_kind_t socket_kind;
    unsigned operations_per_activation;
} leir_peer_config_t;

typedef struct leir_peer_result {
    uint64_t checksum;
    uint64_t completed_transactions;
    int status;
} leir_peer_result_t;

typedef struct leir_peer_process leir_peer_process_t;

int leir_peer_process_start(
    const leir_peer_config_t *config,
    leir_peer_process_t **out);
size_t leir_peer_process_connection_count(
    const leir_peer_process_t *peer);
llam_fd_t leir_peer_process_server_fd(
    const leir_peer_process_t *peer,
    size_t index);
int leir_peer_process_signal_start(leir_peer_process_t *peer);
int leir_peer_process_finish(
    leir_peer_process_t *peer,
    leir_peer_result_t *result_out);
void leir_peer_process_abort(leir_peer_process_t *peer);
const char *leir_peer_kind_name(void);
const char *leir_peer_cpu_scope_name(void);

#endif
