/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lrpa_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LRPA_RESULT_BUFFER_SIZE 4096U
#define LRPA_PATH_BUFFER_SIZE 4096U

typedef struct driver_options {
    lrpa_manifest_t manifest;
    const char *manifest_path;
    const char *artifact_dir;
    bool generate_perturbations;
    bool have_seed;
    bool have_lanes;
    bool have_workers;
    bool have_rounds;
    bool have_coupling;
    bool have_queue_capacity;
    bool have_timeout;
    bool have_fault;
    bool have_allowed;
} driver_options_t;

static bool
parse_u64(const char *text, uint64_t *value_out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || value_out == NULL || text[0] == '-') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value_out = (uint64_t)value;
    return true;
}

static bool
parse_u32(const char *text, uint32_t *value_out)
{
    uint64_t value;

    if (!parse_u64(text, &value) || value > UINT32_MAX) {
        return false;
    }
    *value_out = (uint32_t)value;
    return true;
}

static bool
parse_coupling(const char *text, lrpa_coupling_t *coupling_out)
{
    if (strcmp(text, "independent") == 0) {
        *coupling_out = LRPA_COUPLING_INDEPENDENT;
    } else if (strcmp(text, "shared_object") == 0) {
        *coupling_out = LRPA_COUPLING_SHARED_OBJECT;
    } else if (strcmp(text, "ring") == 0) {
        *coupling_out = LRPA_COUPLING_RING;
    } else if (strcmp(text, "colored_graph") == 0) {
        *coupling_out = LRPA_COUPLING_COLORED_GRAPH;
    } else {
        return false;
    }
    return true;
}

static bool
parse_fault(const char *text, lrpa_fault_t *fault_out)
{
    if (strcmp(text, "none") == 0) {
        *fault_out = LRPA_FAULT_NONE;
    } else if (strcmp(text, "select_skip_winner_cas") == 0) {
        *fault_out = LRPA_FAULT_SELECT_SKIP_WINNER_CAS;
    } else if (strcmp(text, "stale_generation_reuse") == 0) {
        *fault_out = LRPA_FAULT_STALE_GENERATION_REUSE;
    } else {
        return false;
    }
    return true;
}

static void
initialize_options(driver_options_t *options)
{
    memset(options, 0, sizeof(*options));
    options->manifest.version = LRPA_MANIFEST_VERSION;
    options->manifest.gadget = LRPA_GADGET_SELECT_COMPLETION;
}

static bool
parse_cli(int argc, char **argv, driver_options_t *options)
{
    int index;

    initialize_options(options);
    for (index = 1; index < argc; ++index) {
        const char *argument = argv[index];

        if (strcmp(argument, "--generate-perturbations") == 0) {
            options->generate_perturbations = true;
            continue;
        }
        if (index + 1 >= argc) {
            return false;
        }
        index += 1;
        if (strcmp(argument, "--manifest") == 0) {
            options->manifest_path = argv[index];
        } else if (strcmp(argument, "--artifact-dir") == 0) {
            options->artifact_dir = argv[index];
        } else if (strcmp(argument, "--seed") == 0) {
            options->have_seed = parse_u64(argv[index],
                                           &options->manifest.seed);
            if (!options->have_seed) return false;
        } else if (strcmp(argument, "--lanes") == 0) {
            options->have_lanes = parse_u32(argv[index],
                                            &options->manifest.lane_count);
            if (!options->have_lanes) return false;
        } else if (strcmp(argument, "--workers") == 0) {
            options->have_workers = parse_u32(
                argv[index], &options->manifest.worker_count);
            if (!options->have_workers) return false;
        } else if (strcmp(argument, "--rounds") == 0) {
            options->have_rounds = parse_u32(argv[index],
                                             &options->manifest.rounds);
            if (!options->have_rounds) return false;
        } else if (strcmp(argument, "--coupling") == 0) {
            options->have_coupling = parse_coupling(
                argv[index], &options->manifest.coupling);
            if (!options->have_coupling) return false;
        } else if (strcmp(argument, "--queue-capacity") == 0) {
            options->have_queue_capacity = parse_u32(
                argv[index], &options->manifest.queue_capacity);
            if (!options->have_queue_capacity) return false;
        } else if (strcmp(argument, "--timeout-ms") == 0) {
            uint64_t timeout_ms;

            options->have_timeout = parse_u64(argv[index], &timeout_ms) &&
                                    timeout_ms <= UINT64_MAX / 1000000U;
            if (!options->have_timeout) return false;
            options->manifest.timeout_ns = timeout_ms * UINT64_C(1000000);
        } else if (strcmp(argument, "--fault") == 0) {
            options->have_fault = parse_fault(argv[index],
                                              &options->manifest.fault_id);
            if (!options->have_fault) return false;
        } else if (strcmp(argument, "--allowed-outcomes") == 0) {
            options->have_allowed = parse_u32(
                argv[index], &options->manifest.allowed_outcomes);
            if (!options->have_allowed) return false;
        } else {
            return false;
        }
    }
    if (options->manifest_path != NULL) {
        return !options->have_seed && !options->have_lanes &&
               !options->have_workers && !options->have_rounds &&
               !options->have_coupling && !options->have_queue_capacity &&
               !options->have_timeout && !options->have_fault &&
               !options->have_allowed;
    }
    return options->have_seed && options->have_lanes &&
           options->have_workers && options->have_rounds &&
           options->have_coupling && options->have_queue_capacity &&
           options->have_timeout && options->have_fault &&
           options->have_allowed;
}

static char *
read_text_file(const char *path)
{
    FILE *file;
    long length;
    char *buffer;

    file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0L, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length < 0L || length > 1048576L || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    buffer = malloc((size_t)length + 1U);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    if (fread(buffer, 1U, (size_t)length, file) != (size_t)length) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    buffer[length] = '\0';
    fclose(file);
    return buffer;
}

static const char *
find_json_value(const char *document, const char *key)
{
    char pattern[128];
    const char *position;

    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) < 0) {
        return NULL;
    }
    position = strstr(document, pattern);
    if (position == NULL) return NULL;
    position = strchr(position + strlen(pattern), ':');
    if (position == NULL) return NULL;
    position += 1;
    while (*position == ' ' || *position == '\t' || *position == '\n' ||
           *position == '\r') {
        position += 1;
    }
    return position;
}

static bool
json_u64(const char *document, const char *key, uint64_t *value_out)
{
    const char *value = find_json_value(document, key);
    char *end = NULL;
    unsigned long long parsed;

    if (value == NULL || *value == '-') return false;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value) return false;
    *value_out = (uint64_t)parsed;
    return true;
}

static bool
json_u32(const char *document, const char *key, uint32_t *value_out)
{
    uint64_t value;

    if (!json_u64(document, key, &value) || value > UINT32_MAX) return false;
    *value_out = (uint32_t)value;
    return true;
}

static bool
json_string(const char *document, const char *key, char *buffer,
            size_t capacity)
{
    const char *value = find_json_value(document, key);
    const char *end;
    size_t length;

    if (value == NULL || *value != '"') return false;
    value += 1;
    end = strchr(value, '"');
    if (end == NULL) return false;
    length = (size_t)(end - value);
    if (length + 1U > capacity) return false;
    memcpy(buffer, value, length);
    buffer[length] = '\0';
    return true;
}

static bool
json_bool(const char *document, const char *key, bool *value_out)
{
    const char *value = find_json_value(document, key);

    if (value == NULL) return false;
    if (strncmp(value, "true", 4U) == 0) {
        *value_out = true;
        return true;
    }
    if (strncmp(value, "false", 5U) == 0) {
        *value_out = false;
        return true;
    }
    return false;
}

static bool
load_manifest(const char *path, driver_options_t *options)
{
    char coupling[64];
    char fault[64];
    char gadget[32];
    char *document = read_text_file(path);
    bool generate = false;
    bool ok;

    if (document == NULL) return false;
    ok = json_u32(document, "version", &options->manifest.version) &&
         json_string(document, "gadget", gadget, sizeof(gadget)) &&
         strcmp(gadget, "select") == 0 &&
         json_string(document, "coupling", coupling, sizeof(coupling)) &&
         parse_coupling(coupling, &options->manifest.coupling) &&
         json_u32(document, "lane_count", &options->manifest.lane_count) &&
         json_u32(document, "worker_count", &options->manifest.worker_count) &&
         json_u32(document, "rounds", &options->manifest.rounds) &&
         json_u32(document, "queue_capacity",
                  &options->manifest.queue_capacity) &&
         json_u64(document, "seed", &options->manifest.seed) &&
         json_u64(document, "timeout_ns", &options->manifest.timeout_ns) &&
         json_string(document, "fault", fault, sizeof(fault)) &&
         parse_fault(fault, &options->manifest.fault_id) &&
         json_u32(document, "allowed_outcomes",
                  &options->manifest.allowed_outcomes) &&
         json_bool(document, "generate_perturbations", &generate);
    free(document);
    options->generate_perturbations = generate;
    return ok;
}

static void
fill_object_ids(lrpa_manifest_t *manifest)
{
    uint32_t lane;

    for (lane = 0U; lane < manifest->lane_count && lane < LRPA_MAX_LANES;
         ++lane) {
        manifest->object_ids[lane] = 1000U + lane;
    }
}

static bool
artifact_path(char *buffer, size_t capacity, const char *directory,
              const char *name)
{
    const int length = snprintf(buffer, capacity, "%s/%s", directory, name);

    return length >= 0 && (size_t)length < capacity;
}

static bool
write_manifest_file(const driver_options_t *options)
{
    char path[LRPA_PATH_BUFFER_SIZE];
    FILE *file;

    if (options->artifact_dir == NULL) return true;
    if (!artifact_path(path, sizeof(path), options->artifact_dir,
                       "manifest.json")) return false;
    file = fopen(path, "wb");
    if (file == NULL) return false;
    fprintf(file,
            "{\"version\":%u,\"gadget\":\"select\","
            "\"coupling\":\"%s\",\"lane_count\":%u,"
            "\"worker_count\":%u,\"rounds\":%u,"
            "\"queue_capacity\":%u,\"seed\":%" PRIu64 ","
            "\"timeout_ns\":%" PRIu64 ",\"fault\":\"%s\","
            "\"allowed_outcomes\":%u,"
            "\"generate_perturbations\":%s,"
            "\"perturbation_count\":%u,"
            "\"perturbation_hash\":\"%016" PRIx64 "\"}\n",
            options->manifest.version,
            lrpa_coupling_name(options->manifest.coupling),
            options->manifest.lane_count, options->manifest.worker_count,
            options->manifest.rounds, options->manifest.queue_capacity,
            options->manifest.seed, options->manifest.timeout_ns,
            lrpa_fault_name(options->manifest.fault_id),
            options->manifest.allowed_outcomes,
            options->generate_perturbations ? "true" : "false",
            options->manifest.perturbation_count,
            options->manifest.perturbation_hash);
    return fclose(file) == 0;
}

static bool
write_trace_files(const char *directory, const lrpa_context_t *context,
                  const lrpa_result_t *result)
{
    char path[LRPA_PATH_BUFFER_SIZE];
    FILE *file;
    size_t index;

    if (directory == NULL) return true;
    if (!artifact_path(path, sizeof(path), directory, "trace.bin")) return false;
    file = fopen(path, "wb");
    if (file == NULL) return false;
    if (result->trace_entries != 0U &&
        fwrite(context->trace.entries, sizeof(context->trace.entries[0]),
               result->trace_entries, file) != result->trace_entries) {
        fclose(file);
        return false;
    }
    if (fclose(file) != 0) return false;

    if (!artifact_path(path, sizeof(path), directory, "trace.txt")) return false;
    file = fopen(path, "wb");
    if (file == NULL) return false;
    for (index = 0U; index < result->trace_entries; ++index) {
        const lrpa_trace_entry_t *entry = &context->trace.entries[index];

        fprintf(file,
                "sequence=%" PRIu64 " lane=%u actor=%u event=%u "
                "object=%u generation=%" PRIu64 " before=%u after=%u "
                "result=%d\n",
                entry->sequence, entry->lane, entry->actor,
                entry->event_kind, entry->object_id, entry->generation,
                entry->state_before, entry->state_after, entry->result);
    }
    if (fclose(file) != 0) return false;

    if (!artifact_path(path, sizeof(path), directory, "runtime_dump.txt"))
        return false;
    file = fopen(path, "wb");
    if (file == NULL) return false;
    fprintf(file, "status=%s cleanup_complete=%s\n",
            lrpa_status_name(result->status),
            result->cleanup_complete ? "true" : "false");
    if (fclose(file) != 0) return false;

    if (!artifact_path(path, sizeof(path), directory, "wait_graph.json"))
        return false;
    file = fopen(path, "wb");
    if (file == NULL) return false;
    fputs("{}\n", file);
    return fclose(file) == 0;
}

static bool
format_result(char *buffer, size_t capacity,
              const driver_options_t *options, const lrpa_result_t *result)
{
    char failure_round[32];
    int length;

    if (result->first_failure_round == UINT32_MAX) {
        strcpy(failure_round, "null");
    } else {
        (void)snprintf(failure_round, sizeof(failure_round), "%u",
                       result->first_failure_round);
    }
    length = snprintf(
        buffer, capacity,
        "{\"schema_version\":%u,\"status\":\"%s\","
        "\"seed\":%" PRIu64 ",\"lanes\":%u,\"workers\":%u,"
        "\"rounds_requested\":%u,\"rounds_completed\":%u,"
        "\"coupling\":\"%s\",\"fault\":\"%s\","
        "\"signature\":\"%016" PRIx64 "\","
        "\"lane_executions\":%" PRIu64 ","
        "\"elapsed_ns\":%" PRIu64 ",\"failures\":%u,"
        "\"first_failure_round\":%s,\"oracle\":%u,"
        "\"armed_total\":%" PRIu64 ","
        "\"winner_total\":%" PRIu64 ","
        "\"cancel_total\":%" PRIu64 ","
        "\"timeout_total\":%" PRIu64 ","
        "\"discard_total\":%" PRIu64 ","
        "\"trace_entries\":%zu,\"trace_truncated\":%s,"
        "\"cleanup_complete\":%s}",
        LRPA_RESULT_VERSION, lrpa_status_name(result->status),
        result->seed, options->manifest.lane_count,
        options->manifest.worker_count, options->manifest.rounds,
        result->rounds_completed,
        lrpa_coupling_name(options->manifest.coupling),
        lrpa_fault_name(options->manifest.fault_id), result->signature,
        result->lane_executions, result->elapsed_ns, result->failures,
        failure_round, (unsigned int)result->first_failure.oracle,
        result->armed_total, result->winner_total, result->cancel_total,
        result->timeout_total, result->discard_total, result->trace_entries,
        result->trace_truncated ? "true" : "false",
        result->cleanup_complete ? "true" : "false");
    return length >= 0 && (size_t)length < capacity;
}

static bool
write_result_file(const char *directory, const char *document)
{
    char path[LRPA_PATH_BUFFER_SIZE];
    FILE *file;

    if (directory == NULL) return true;
    if (!artifact_path(path, sizeof(path), directory, "result.json")) return false;
    file = fopen(path, "wb");
    if (file == NULL) return false;
    fprintf(file, "%s\n", document);
    return fclose(file) == 0;
}

int
main(int argc, char **argv)
{
    driver_options_t options;
    lrpa_run_options_t run_options = {UINT32_MAX, UINT32_MAX};
    lrpa_context_t context;
    lrpa_result_t result;
    lrpa_status_t status;
    char document[LRPA_RESULT_BUFFER_SIZE];
    bool artifacts_ok;

    if (!parse_cli(argc, argv, &options)) {
        fputs("usage: bench_lrpa --seed N --lanes N --workers N --rounds N "
              "--coupling MODE --queue-capacity N --timeout-ms N "
              "--fault ID --allowed-outcomes MASK [--generate-perturbations] "
              "[--artifact-dir DIR]\n"
              "       bench_lrpa --manifest FILE [--artifact-dir DIR]\n",
              stderr);
        return 2;
    }
    if (options.manifest_path != NULL &&
        !load_manifest(options.manifest_path, &options)) {
        fputs("invalid manifest document\n", stderr);
        return 2;
    }
    fill_object_ids(&options.manifest);
    if (options.generate_perturbations &&
        lrpa_manifest_generate_perturbations(&options.manifest) !=
            LRPA_STATUS_OK) {
        fputs("failed to generate perturbations\n", stderr);
        return 2;
    }
    status = lrpa_manifest_validate(&options.manifest);
    if (status != LRPA_STATUS_OK) {
        fprintf(stderr, "invalid manifest: %s\n", lrpa_status_name(status));
        return 2;
    }
    if (!write_manifest_file(&options)) {
        fputs("failed to write manifest artifact\n", stderr);
        return 2;
    }
    status = lrpa_context_init(&context, &options.manifest, &run_options);
    if (status != LRPA_STATUS_OK) {
        fprintf(stderr, "context init failed: %s\n", lrpa_status_name(status));
        return 2;
    }
    status = lrpa_context_run(&context, &result);
    if (!format_result(document, sizeof(document), &options, &result)) {
        lrpa_context_destroy(&context);
        fputs("result document overflow\n", stderr);
        return 2;
    }
    artifacts_ok = write_trace_files(options.artifact_dir, &context, &result) &&
                   write_result_file(options.artifact_dir, document);
    lrpa_context_destroy(&context);
    if (!artifacts_ok) {
        fputs("failed to write result artifacts\n", stderr);
        return 2;
    }
    printf("%s\n", document);
    if (status == LRPA_STATUS_OK) return 0;
    if (status == LRPA_STATUS_ORACLE_FAILURE) return 10;
    return 2;
}
