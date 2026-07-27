#include "leir_native_segment.h"
#include "leir_test_support.h"

#include <errno.h>
#include <limits.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !LLAM_PLATFORM_WINDOWS
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define TEST_FD_SLOT 0U
#define TEST_MUT_BUFFER_SLOT 1U
#define TEST_CONST_BUFFER_SLOT 2U
#define TEST_LENGTH_SLOT 3U
#define TEST_FIRST_RESULT_SLOT 4U
#define TEST_STORAGE_BYTES 8192U
#define INTEGRATION_FD_SLOT 0U
#define INTEGRATION_LENGTH_SLOT 1U
#define INTEGRATION_FIRST_BUFFER_SLOT 2U
#define INTEGRATION_RESULT_SLOT 10U
#define INTEGRATION_SLOT_COUNT 11U
#define INTEGRATION_BYTES 64U
#define INTEGRATION_ACTIVATIONS 3U

typedef union test_instance_storage {
    max_align_t alignment;
    unsigned char bytes[TEST_STORAGE_BYTES];
} test_instance_storage_t;

typedef struct bind_fixture {
    leir_phase0_program_t *program;
    leir_native_plan_t plan;
    test_instance_storage_t storage;
    leir_native_instance_t *instance;
    leir_phase0_value_t values[LEIR_PHASE0_MAX_SLOTS];
    unsigned char mut_buffer[64];
    unsigned char const_buffer[64];
    llam_fd_t pair[2];
    size_t value_count;
    bool signed_length;
} bind_fixture_t;

static leir_phase0_node_desc_t terminal_node(
    leir_phase0_opcode_t opcode,
    uint16_t result_slot) {
    leir_phase0_node_desc_t node;

    memset(&node, 0, sizeof(node));
    node.opcode = (uint16_t)opcode;
    node.fd_slot = LEIR_PHASE0_NODE_NONE;
    node.buffer_slot = LEIR_PHASE0_NODE_NONE;
    node.length_slot = LEIR_PHASE0_NODE_NONE;
    node.result_slot = result_slot;
    node.on_success = LEIR_PHASE0_NODE_NONE;
    node.on_eof = LEIR_PHASE0_NODE_NONE;
    node.on_error = LEIR_PHASE0_NODE_NONE;
    return node;
}

static int create_two_step_program(
    bool signed_length,
    leir_phase0_program_t **out) {
    leir_phase0_node_desc_t nodes[4];
    leir_phase0_slot_kind_t slots[6];
    leir_phase0_program_desc_t desc;
    unsigned i;

    memset(nodes, 0, sizeof(nodes));
    memset(slots, 0, sizeof(slots));
    slots[TEST_FD_SLOT] = LEIR_PHASE0_SLOT_FD;
    slots[TEST_MUT_BUFFER_SLOT] =
        LEIR_PHASE0_SLOT_MUT_BUFFER;
    slots[TEST_CONST_BUFFER_SLOT] =
        LEIR_PHASE0_SLOT_CONST_BUFFER;
    slots[TEST_LENGTH_SLOT] = signed_length
        ? LEIR_PHASE0_SLOT_I64
        : LEIR_PHASE0_SLOT_U64;
    slots[TEST_FIRST_RESULT_SLOT] = LEIR_PHASE0_SLOT_I64;
    slots[TEST_FIRST_RESULT_SLOT + 1U] =
        LEIR_PHASE0_SLOT_I64;

    for (i = 0U; i < 2U; i += 1U) {
        nodes[i].opcode = (uint16_t)(
            i == 0U
                ? LEIR_PHASE0_OP_WRITE_ALL
                : LEIR_PHASE0_OP_READ_EXACT);
        nodes[i].fd_slot = TEST_FD_SLOT;
        nodes[i].buffer_slot = (uint16_t)(
            i == 0U
                ? TEST_CONST_BUFFER_SLOT
                : TEST_MUT_BUFFER_SLOT);
        nodes[i].length_slot = TEST_LENGTH_SLOT;
        nodes[i].result_slot =
            (uint16_t)(TEST_FIRST_RESULT_SLOT + i);
        nodes[i].on_success = (uint16_t)(i + 1U);
        nodes[i].on_eof = 3U;
        nodes[i].on_error = 3U;
    }
    nodes[2] = terminal_node(
        LEIR_PHASE0_OP_RETURN,
        TEST_FIRST_RESULT_SLOT + 1U);
    nodes[3] = terminal_node(
        LEIR_PHASE0_OP_FAIL,
        TEST_FIRST_RESULT_SLOT);

    memset(&desc, 0, sizeof(desc));
    desc.nodes = nodes;
    desc.slot_kinds = slots;
    desc.node_count = 4U;
    desc.slot_count = 6U;
    desc.entry_node = 0U;
    return leir_phase0_program_create(&desc, out);
}

static int bind_fixture_init(
    bind_fixture_t *fixture,
    bool signed_length) {
    int socket_type = SOCK_STREAM;

    memset(fixture, 0, sizeof(*fixture));
    fixture->pair[0] = LLAM_INVALID_FD;
    fixture->pair[1] = LLAM_INVALID_FD;
#if defined(__linux__)
    socket_type = SOCK_SEQPACKET;
#endif
    if (create_two_step_program(
            signed_length, &fixture->program) != 0 ||
        leir_native_plan_compile(
            fixture->program, &fixture->plan) != 0 ||
        leir_native_instance_size() >
            sizeof(fixture->storage.bytes) ||
        leir_native_instance_init(
            fixture->storage.bytes,
            sizeof(fixture->storage.bytes),
            fixture->program,
            &fixture->plan,
            LEIR_NATIVE_MODE_LINK) != 0 ||
        leir_test_socketpair_type(
            socket_type, fixture->pair) != 0) {
        return -1;
    }

    fixture->instance =
        (leir_native_instance_t *)fixture->storage.bytes;
    fixture->value_count = 6U;
    fixture->signed_length = signed_length;
    fixture->values[TEST_FD_SLOT].fd = fixture->pair[0];
    fixture->values[TEST_MUT_BUFFER_SLOT].buffer.data =
        fixture->mut_buffer;
    fixture->values[TEST_MUT_BUFFER_SLOT].buffer.size =
        sizeof(fixture->mut_buffer);
    fixture->values[TEST_CONST_BUFFER_SLOT].buffer.data =
        fixture->const_buffer;
    fixture->values[TEST_CONST_BUFFER_SLOT].buffer.size =
        sizeof(fixture->const_buffer);
    if (signed_length) {
        fixture->values[TEST_LENGTH_SLOT].i64 = 16;
    } else {
        fixture->values[TEST_LENGTH_SLOT].u64 = 16U;
    }
    return 0;
}

static void bind_fixture_destroy(bind_fixture_t *fixture) {
    if (fixture->instance != NULL) {
        (void)leir_native_instance_destroy(fixture->instance);
        fixture->instance = NULL;
    }
    leir_test_close(&fixture->pair[0]);
    leir_test_close(&fixture->pair[1]);
    leir_phase0_program_destroy(fixture->program);
    fixture->program = NULL;
}

static int expect_bind_failure(
    bind_fixture_t *fixture,
    int expected_errno) {
    errno = 0;
    if (leir_native_instance_bind(
            fixture->instance,
            fixture->values,
            fixture->value_count) == 0 ||
        errno != expected_errno) {
        fprintf(
            stderr,
            "bind failure mismatch expected=%d actual=%d\n",
            expected_errno,
            errno);
        return 1;
    }
    return 0;
}

static int test_init_validates_storage(void) {
    leir_phase0_program_t *program = NULL;
    leir_native_plan_t plan;
    test_instance_storage_t storage;
    unsigned char *misaligned = storage.bytes + 1U;
    size_t size;
    int failed = 0;

    if (create_two_step_program(false, &program) != 0 ||
        leir_native_plan_compile(program, &plan) != 0) {
        perror("create init validation program");
        return 1;
    }
    size = leir_native_instance_size();
    if (size == 0U || size > sizeof(storage.bytes)) {
        fprintf(stderr, "invalid native instance size\n");
        failed = 1;
        goto done;
    }
    errno = 0;
    if (leir_native_instance_init(
            NULL,
            size,
            program,
            &plan,
            LEIR_NATIVE_MODE_LINK) == 0 ||
        errno != EINVAL) {
        fprintf(stderr, "null storage was accepted\n");
        failed = 1;
    }
    errno = 0;
    if (leir_native_instance_init(
            storage.bytes,
            size - 1U,
            program,
            &plan,
            LEIR_NATIVE_MODE_LINK) == 0 ||
        errno != EINVAL) {
        fprintf(stderr, "undersized storage was accepted\n");
        failed = 1;
    }
    errno = 0;
    if (leir_native_instance_init(
            misaligned,
            sizeof(storage.bytes) - 1U,
            program,
            &plan,
            LEIR_NATIVE_MODE_LINK) == 0 ||
        errno != EINVAL) {
        fprintf(stderr, "misaligned storage was accepted\n");
        failed = 1;
    }

done:
    leir_phase0_program_destroy(program);
    return failed;
}

static int test_bind_rejects_wrong_value_count(void) {
    bind_fixture_t fixture;
    int failed;

    if (bind_fixture_init(&fixture, false) != 0) {
        perror("wrong-count fixture init");
        return 1;
    }
    fixture.value_count -= 1U;
    failed = expect_bind_failure(&fixture, EINVAL);
    bind_fixture_destroy(&fixture);
    return failed;
}

static int test_bind_rejects_negative_signed_length(void) {
    bind_fixture_t fixture;
    int failed;

    if (bind_fixture_init(&fixture, true) != 0) {
        perror("signed-length fixture init");
        return 1;
    }
    fixture.values[TEST_LENGTH_SLOT].i64 = -1;
    failed = expect_bind_failure(&fixture, EINVAL);
    bind_fixture_destroy(&fixture);
    return failed;
}

static int test_bind_rejects_zero_length(void) {
    bind_fixture_t fixture;
    int failed;

    if (bind_fixture_init(&fixture, false) != 0) {
        perror("zero-length fixture init");
        return 1;
    }
    fixture.values[TEST_LENGTH_SLOT].u64 = 0U;
    failed = expect_bind_failure(&fixture, EINVAL);
    bind_fixture_destroy(&fixture);
    return failed;
}

static int test_bind_rejects_length_beyond_buffer(void) {
    bind_fixture_t fixture;
    int failed;

    if (bind_fixture_init(&fixture, false) != 0) {
        perror("capacity fixture init");
        return 1;
    }
    fixture.values[TEST_LENGTH_SLOT].u64 =
        sizeof(fixture.mut_buffer) + 1U;
    failed = expect_bind_failure(&fixture, EINVAL);
    bind_fixture_destroy(&fixture);
    return failed;
}

static int test_bind_rejects_length_above_uint(void) {
#if SIZE_MAX > UINT_MAX
    bind_fixture_t fixture;
    int failed;

    if (bind_fixture_init(&fixture, false) != 0) {
        perror("overflow fixture init");
        return 1;
    }
    fixture.values[TEST_LENGTH_SLOT].u64 =
        (uint64_t)UINT_MAX + 1U;
    fixture.values[TEST_MUT_BUFFER_SLOT].buffer.size =
        SIZE_MAX;
    fixture.values[TEST_CONST_BUFFER_SLOT].buffer.size =
        SIZE_MAX;
    failed = expect_bind_failure(&fixture, EOVERFLOW);
    bind_fixture_destroy(&fixture);
    return failed;
#else
    return 0;
#endif
}

static int test_bind_rejects_null_nonempty_buffer(void) {
    bind_fixture_t fixture;
    int failed;

    if (bind_fixture_init(&fixture, false) != 0) {
        perror("null-buffer fixture init");
        return 1;
    }
    fixture.values[TEST_MUT_BUFFER_SLOT].buffer.data = NULL;
    failed = expect_bind_failure(&fixture, EINVAL);
    bind_fixture_destroy(&fixture);
    return failed;
}

static int test_bind_rejects_invalid_fd(void) {
    bind_fixture_t fixture;
    int failed;

    if (bind_fixture_init(&fixture, false) != 0) {
        perror("invalid-fd fixture init");
        return 1;
    }
    fixture.values[TEST_FD_SLOT].fd = LLAM_INVALID_FD;
    failed = expect_bind_failure(&fixture, EINVAL);
    bind_fixture_destroy(&fixture);
    return failed;
}

static int test_bind_accepts_repeated_fd(void) {
    bind_fixture_t fixture;
    int failed = 0;

    if (bind_fixture_init(&fixture, false) != 0) {
        perror("repeated-fd fixture init");
        return 1;
    }
    if (leir_native_instance_bind(
            fixture.instance,
            fixture.values,
            fixture.value_count) != 0) {
        perror("repeated fd bind");
        failed = 1;
    }
    bind_fixture_destroy(&fixture);
    return failed;
}

#if defined(__linux__)
static int bind_with_fd(llam_fd_t fd, int expected_errno) {
    bind_fixture_t fixture;
    int failed;

    if (bind_fixture_init(&fixture, false) != 0) {
        perror("Linux fd-boundary fixture init");
        return 1;
    }
    fixture.values[TEST_FD_SLOT].fd = fd;
    failed = expect_bind_failure(&fixture, expected_errno);
    bind_fixture_destroy(&fixture);
    return failed;
}

static int test_bind_rejects_regular_file_with_enotsock(void) {
    int fd = open("/dev/null", O_RDONLY);
    int failed;

    if (fd < 0) {
        perror("open /dev/null");
        return 1;
    }
    failed = bind_with_fd(fd, ENOTSOCK);
    close(fd);
    return failed;
}

static int test_bind_rejects_stream_socket_with_eprototype(void) {
    llam_fd_t pair[2] = {
        LLAM_INVALID_FD,
        LLAM_INVALID_FD,
    };
    int failed;

    if (leir_test_socketpair_type(SOCK_STREAM, pair) != 0) {
        perror("create stream socketpair");
        return 1;
    }
    failed = bind_with_fd(pair[0], EPROTOTYPE);
    leir_test_close(&pair[0]);
    leir_test_close(&pair[1]);
    return failed;
}

static int test_bind_rejects_unconnected_seqpacket(void) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    int failed;

    if (fd < 0) {
        perror("create unconnected seqpacket socket");
        return 1;
    }
    failed = bind_with_fd(fd, ENOTCONN);
    close(fd);
    return failed;
}

static int test_destroy_releases_pinned_fd(void) {
    bind_fixture_t fixture;
    unsigned char byte;
    ssize_t result;
    int failed = 0;

    if (bind_fixture_init(&fixture, false) != 0) {
        perror("pinned-fd fixture init");
        return 1;
    }
    if (leir_native_instance_bind(
            fixture.instance,
            fixture.values,
            fixture.value_count) != 0) {
        perror("pinned-fd bind");
        bind_fixture_destroy(&fixture);
        return 1;
    }

    leir_test_close(&fixture.pair[0]);
    errno = 0;
    result = recv(
        (int)fixture.pair[1],
        &byte,
        sizeof(byte),
        MSG_DONTWAIT);
    if (result != -1 ||
        (errno != EAGAIN && errno != EWOULDBLOCK)) {
        fprintf(stderr, "bound duplicate did not pin socket\n");
        failed = 1;
    }
    if (leir_native_instance_destroy(
            fixture.instance) != 0) {
        perror("destroy pinned-fd instance");
        failed = 1;
    } else {
        fixture.instance = NULL;
        result = recv(
            (int)fixture.pair[1],
            &byte,
            sizeof(byte),
            MSG_DONTWAIT);
        if (result != 0) {
            fprintf(
                stderr,
                "destroy did not release pinned socket\n");
            failed = 1;
        }
    }
    bind_fixture_destroy(&fixture);
    return failed;
}

typedef struct native_integration_state {
    leir_phase0_program_t *program;
    leir_native_plan_t plan;
    test_instance_storage_t storage;
    leir_native_instance_t *instance;
    leir_phase0_value_t values[LEIR_PHASE0_MAX_SLOTS];
    leir_phase0_value_t values_out[LEIR_PHASE0_MAX_SLOTS];
    unsigned char buffers[LEIR_NATIVE_MAX_OPS][INTEGRATION_BYTES];
    llam_fd_t pair[2];
    llam_fd_t replacement_pair[2];
    leir_native_mode_t mode;
    unsigned op_count;
    bool fd_reused;
    atomic_uint failures;
    atomic_uint instance_done;
    int first_errno;
    char first_case[96];
} native_integration_state_t;

static int create_integration_program(
    unsigned op_count,
    leir_phase0_program_t **out) {
    leir_phase0_node_desc_t
        nodes[LEIR_NATIVE_MAX_OPS + 2U];
    leir_phase0_slot_kind_t slots[INTEGRATION_SLOT_COUNT];
    leir_phase0_program_desc_t desc;
    unsigned i;

    memset(nodes, 0, sizeof(nodes));
    memset(slots, 0, sizeof(slots));
    slots[INTEGRATION_FD_SLOT] = LEIR_PHASE0_SLOT_FD;
    slots[INTEGRATION_LENGTH_SLOT] = LEIR_PHASE0_SLOT_U64;
    for (i = 0U; i < LEIR_NATIVE_MAX_OPS; i += 1U) {
        slots[INTEGRATION_FIRST_BUFFER_SLOT + i] =
            LEIR_PHASE0_SLOT_CONST_BUFFER;
    }
    slots[INTEGRATION_RESULT_SLOT] = LEIR_PHASE0_SLOT_I64;

    for (i = 0U; i < op_count; i += 1U) {
        nodes[i].opcode = LEIR_PHASE0_OP_WRITE_ALL;
        nodes[i].fd_slot = INTEGRATION_FD_SLOT;
        nodes[i].buffer_slot =
            (uint16_t)(INTEGRATION_FIRST_BUFFER_SLOT + i);
        nodes[i].length_slot = INTEGRATION_LENGTH_SLOT;
        nodes[i].result_slot = INTEGRATION_RESULT_SLOT;
        nodes[i].on_success = (uint16_t)(i + 1U);
        nodes[i].on_eof = (uint16_t)(op_count + 1U);
        nodes[i].on_error = (uint16_t)(op_count + 1U);
    }
    nodes[op_count] = terminal_node(
        LEIR_PHASE0_OP_RETURN,
        INTEGRATION_RESULT_SLOT);
    nodes[op_count + 1U] = terminal_node(
        LEIR_PHASE0_OP_FAIL,
        INTEGRATION_RESULT_SLOT);

    memset(&desc, 0, sizeof(desc));
    desc.nodes = nodes;
    desc.slot_kinds = slots;
    desc.node_count = op_count + 2U;
    desc.slot_count = INTEGRATION_SLOT_COUNT;
    desc.entry_node = 0U;
    return leir_phase0_program_create(&desc, out);
}

static void native_integration_fail(
    native_integration_state_t *state,
    const char *where,
    int error_code) {
    if (atomic_fetch_add_explicit(
            &state->failures,
            1U,
            memory_order_relaxed) == 0U) {
        state->first_errno = error_code;
        (void)snprintf(
            state->first_case,
            sizeof(state->first_case),
            "%s",
            where);
    }
    if (!LLAM_FD_IS_INVALID(state->pair[0])) {
        (void)shutdown((int)state->pair[0], SHUT_RDWR);
    }
    if (!LLAM_FD_IS_INVALID(state->pair[1])) {
        (void)shutdown((int)state->pair[1], SHUT_RDWR);
    }
    if (!LLAM_FD_IS_INVALID(state->replacement_pair[0])) {
        (void)shutdown(
            (int)state->replacement_pair[0], SHUT_RDWR);
    }
    if (!LLAM_FD_IS_INVALID(state->replacement_pair[1])) {
        (void)shutdown(
            (int)state->replacement_pair[1], SHUT_RDWR);
    }
}

static int native_integration_state_init(
    native_integration_state_t *state,
    unsigned op_count,
    leir_native_mode_t mode) {
    unsigned i;

    memset(state, 0, sizeof(*state));
    state->pair[0] = LLAM_INVALID_FD;
    state->pair[1] = LLAM_INVALID_FD;
    state->replacement_pair[0] = LLAM_INVALID_FD;
    state->replacement_pair[1] = LLAM_INVALID_FD;
    state->op_count = op_count;
    state->mode = mode;
    atomic_init(&state->failures, 0U);
    atomic_init(&state->instance_done, 0U);

    if (create_integration_program(
            op_count, &state->program) != 0 ||
        leir_native_plan_compile(
            state->program, &state->plan) != 0 ||
        leir_native_instance_size() >
            sizeof(state->storage.bytes) ||
        leir_native_instance_init(
            state->storage.bytes,
            sizeof(state->storage.bytes),
            state->program,
            &state->plan,
            mode) != 0 ||
        leir_test_socketpair_type(
            SOCK_SEQPACKET, state->pair) != 0) {
        return -1;
    }
    state->instance =
        (leir_native_instance_t *)state->storage.bytes;
    state->values[INTEGRATION_FD_SLOT].fd =
        state->pair[0];
    state->values[INTEGRATION_LENGTH_SLOT].u64 =
        INTEGRATION_BYTES;
    state->values[INTEGRATION_RESULT_SLOT].i64 = -1;
    for (i = 0U; i < LEIR_NATIVE_MAX_OPS; i += 1U) {
        state->values[
            INTEGRATION_FIRST_BUFFER_SLOT + i].buffer.data =
            state->buffers[i];
        state->values[
            INTEGRATION_FIRST_BUFFER_SLOT + i].buffer.size =
            sizeof(state->buffers[i]);
        leir_test_fill_pattern(
            state->buffers[i],
            sizeof(state->buffers[i]),
            UINT64_C(0x4c4549520000) + i);
    }
    return leir_native_instance_bind(
        state->instance,
        state->values,
        INTEGRATION_SLOT_COUNT);
}

static int native_integration_reuse_bound_fd(
    native_integration_state_t *state) {
    int bound_fd;
    int flags;

    if (state == NULL ||
        LLAM_FD_IS_INVALID(state->pair[0])) {
        errno = EINVAL;
        return -1;
    }
    bound_fd = (int)state->pair[0];
    leir_test_close(&state->pair[0]);
    if (leir_test_socketpair_type(
            SOCK_SEQPACKET,
            state->replacement_pair) != 0) {
        return -1;
    }
    if ((int)state->replacement_pair[1] == bound_fd) {
        llam_fd_t other = state->replacement_pair[0];

        state->replacement_pair[0] =
            state->replacement_pair[1];
        state->replacement_pair[1] = other;
    } else if (
        (int)state->replacement_pair[0] != bound_fd) {
        if (dup2(
                (int)state->replacement_pair[0],
                bound_fd) < 0) {
            return -1;
        }
        leir_test_close(&state->replacement_pair[0]);
        state->replacement_pair[0] = (llam_fd_t)bound_fd;
    }
    flags = fcntl((int)state->pair[1], F_GETFL, 0);
    if (flags < 0 ||
        fcntl(
            (int)state->pair[1],
            F_SETFL,
            flags | O_NONBLOCK) != 0) {
        return -1;
    }
    state->fd_reused = true;
    return 0;
}

static void native_integration_state_destroy(
    native_integration_state_t *state) {
    if (state->instance != NULL) {
        (void)leir_native_instance_destroy(state->instance);
        state->instance = NULL;
    }
    leir_test_close(&state->pair[0]);
    leir_test_close(&state->pair[1]);
    leir_test_close(&state->replacement_pair[0]);
    leir_test_close(&state->replacement_pair[1]);
    leir_phase0_program_destroy(state->program);
    state->program = NULL;
}

static int native_integration_receive(
    native_integration_state_t *state,
    unsigned char received[INTEGRATION_BYTES]) {
    if (!state->fd_reused) {
        return leir_test_read_exact(
            state->pair[1],
            received,
            INTEGRATION_BYTES);
    }

    for (;;) {
        ssize_t result = recv(
            (int)state->pair[1],
            received,
            INTEGRATION_BYTES,
            MSG_DONTWAIT);

        if (result == (ssize_t)INTEGRATION_BYTES) {
            return 0;
        }
        if (result < 0 &&
            (errno == EINTR ||
             errno == EAGAIN ||
             errno == EWOULDBLOCK)) {
            if (atomic_load_explicit(
                    &state->instance_done,
                    memory_order_acquire) != 0U) {
                errno = EPIPE;
                return -1;
            }
            llam_yield();
            continue;
        }
        errno = result == 0 ? EPIPE : EPROTO;
        return -1;
    }
}

static void native_integration_peer_task(void *arg) {
    native_integration_state_t *state = arg;
    unsigned activation;
    unsigned i;

    for (activation = 0U;
         activation < INTEGRATION_ACTIVATIONS;
        activation += 1U) {
        for (i = 0U; i < state->op_count; i += 1U) {
            unsigned char received[INTEGRATION_BYTES];

            memset(received, 0, sizeof(received));
            if (native_integration_receive(
                    state, received) != 0 ||
                memcmp(
                    received,
                    state->buffers[i],
                    sizeof(received)) != 0) {
                native_integration_fail(
                    state,
                    "peer receive",
                    errno != 0 ? errno : EPROTO);
                return;
            }
        }
    }
}

static int native_metrics_are_expected(
    const native_integration_state_t *state,
    const leir_native_metrics_t *metrics,
    unsigned activation) {
    uint64_t activations = (uint64_t)activation + 1U;
    uint64_t logical =
        activations * state->op_count;
    uint64_t expected_cqes =
        state->mode == LEIR_NATIVE_MODE_LINK
            ? logical
            : activations;
    uint64_t expected_suppressed =
        state->mode == LEIR_NATIVE_MODE_LINK
            ? 0U
            : activations * (state->op_count - 1U);

    return metrics->activations == activations &&
           metrics->logical_operations == logical &&
           metrics->queue_publications == activations &&
           metrics->prepared_sqes == logical &&
           metrics->observed_cqes == expected_cqes &&
           metrics->suppressed_success_cqes ==
               expected_suppressed &&
           metrics->task_parks == activations &&
           metrics->terminal_wakes == activations &&
           metrics->hot_allocations == 0U &&
           metrics->first_error_operation == UINT16_MAX;
}

static void native_integration_instance_task(void *arg) {
    native_integration_state_t *state = arg;
    unsigned activation;

    for (activation = 0U;
         activation < INTEGRATION_ACTIVATIONS;
         activation += 1U) {
        leir_native_metrics_t metrics;

        memset(&metrics, 0, sizeof(metrics));
        memset(
            state->values_out,
            0,
            sizeof(state->values_out));
        if (leir_native_instance_run(
                state->instance,
                state->values_out,
                INTEGRATION_SLOT_COUNT,
                &metrics) != 0) {
            native_integration_fail(
                state, "native run", errno);
            goto done;
        }
        if (state->values_out[
                INTEGRATION_RESULT_SLOT].i64 !=
                INTEGRATION_BYTES ||
            !native_metrics_are_expected(
                state, &metrics, activation)) {
            native_integration_fail(
                state,
                "native result or metrics",
                EPROTO);
            goto done;
        }
    }

done:
    atomic_store_explicit(
        &state->instance_done, 1U, memory_order_release);
}

enum {
    NATIVE_INTEGRATION_PASS = 0,
    NATIVE_INTEGRATION_FAIL = 1,
    NATIVE_INTEGRATION_UNAVAILABLE = 77,
};

static int run_native_integration_case(
    unsigned op_count,
    leir_native_mode_t mode,
    bool reuse_bound_fd) {
    native_integration_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *peer = NULL;
    llam_task_t *instance_task = NULL;
    bool runtime_started = false;
    int result = NATIVE_INTEGRATION_FAIL;

    if (native_integration_state_init(
            &state, op_count, mode) != 0) {
        perror("native integration fixture init");
        native_integration_state_destroy(&state);
        return NATIVE_INTEGRATION_FAIL;
    }
    if (reuse_bound_fd &&
        native_integration_reuse_bound_fd(&state) != 0) {
        perror("reuse bound native descriptor");
        native_integration_state_destroy(&state);
        return NATIVE_INTEGRATION_FAIL;
    }
    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.forced_yield_every = 1U;
    opts.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&opts) != 0) {
        perror("native integration runtime init");
        goto cleanup;
    }
    runtime_started = true;
    if (g_llam_runtime.active_nodes == 0U ||
        g_llam_runtime.nodes == NULL ||
        !g_llam_runtime.nodes[0].ring_ready ||
        !g_llam_runtime.nodes[0].supports_recv ||
        !g_llam_runtime.nodes[0].supports_send ||
        (mode == LEIR_NATIVE_MODE_LINK_CQE_SKIP &&
         (g_llam_runtime.nodes[0].linux_ring_features &
          IORING_FEAT_CQE_SKIP) == 0U)) {
        result = NATIVE_INTEGRATION_UNAVAILABLE;
        goto shutdown;
    }

    peer = llam_spawn(
        native_integration_peer_task, &state, NULL);
    if (peer != NULL) {
        instance_task = llam_spawn(
            native_integration_instance_task,
            &state,
            NULL);
    }
    if (peer == NULL ||
        instance_task == NULL ||
        llam_run() != 0 ||
        llam_join(peer) != 0 ||
        llam_join(instance_task) != 0) {
        peer = NULL;
        instance_task = NULL;
        goto shutdown;
    }
    peer = NULL;
    instance_task = NULL;
    if (atomic_load_explicit(
            &state.failures,
            memory_order_acquire) != 0U) {
        fprintf(
            stderr,
            "native integration failed at %s: errno=%d "
            "ops=%u mode=%u\n",
            state.first_case,
            state.first_errno,
            op_count,
            (unsigned)mode);
        goto shutdown;
    }
    if (reuse_bound_fd) {
        unsigned char unexpected;
        ssize_t received;

        errno = 0;
        received = recv(
            (int)state.replacement_pair[1],
            &unexpected,
            sizeof(unexpected),
            MSG_DONTWAIT);
        if (received != -1 ||
            (errno != EAGAIN && errno != EWOULDBLOCK)) {
            fprintf(
                stderr,
                "reused descriptor received native traffic\n");
            goto shutdown;
        }
    }
    result = NATIVE_INTEGRATION_PASS;

shutdown:
    if (runtime_started) {
        llam_runtime_shutdown();
    }
cleanup:
    native_integration_state_destroy(&state);
    return result;
}

static int test_native_runtime_link_and_skip(void) {
    static const unsigned operation_counts[] = {
        1U, 2U, 4U, 8U,
    };
    size_t i;

    for (i = 0U;
         i < sizeof(operation_counts) /
             sizeof(operation_counts[0]);
         i += 1U) {
        int result = run_native_integration_case(
            operation_counts[i],
            LEIR_NATIVE_MODE_LINK,
            false);

        if (result == NATIVE_INTEGRATION_UNAVAILABLE) {
            puts(
                "SKIP: Linux io_uring native segment "
                "integration unavailable");
            return 0;
        }
        if (result != NATIVE_INTEGRATION_PASS) {
            return 1;
        }
    }
    {
        int result = run_native_integration_case(
            4U,
            LEIR_NATIVE_MODE_LINK_CQE_SKIP,
            false);

        if (result == NATIVE_INTEGRATION_UNAVAILABLE) {
            puts(
                "SKIP: Linux CQE_SKIP_SUCCESS "
                "integration unavailable");
            return 0;
        }
        if (result != NATIVE_INTEGRATION_PASS) {
            return 1;
        }
    }
    return 0;
}

static int test_native_runtime_pins_bound_fd(void) {
    int result = run_native_integration_case(
        1U,
        LEIR_NATIVE_MODE_LINK,
        true);

    if (result == NATIVE_INTEGRATION_UNAVAILABLE) {
        puts(
            "SKIP: Linux io_uring native fd pin "
            "integration unavailable");
        return 0;
    }
    return result == NATIVE_INTEGRATION_PASS ? 0 : 1;
}
#endif

int main(void) {
    if (test_init_validates_storage() != 0 ||
        test_bind_rejects_wrong_value_count() != 0 ||
        test_bind_rejects_negative_signed_length() != 0 ||
        test_bind_rejects_zero_length() != 0 ||
        test_bind_rejects_length_beyond_buffer() != 0 ||
        test_bind_rejects_length_above_uint() != 0 ||
        test_bind_rejects_null_nonempty_buffer() != 0 ||
        test_bind_rejects_invalid_fd() != 0 ||
        test_bind_accepts_repeated_fd() != 0) {
        return 1;
    }
#if defined(__linux__)
    if (test_bind_rejects_regular_file_with_enotsock() != 0 ||
        test_bind_rejects_stream_socket_with_eprototype() != 0 ||
        test_bind_rejects_unconnected_seqpacket() != 0 ||
        test_destroy_releases_pinned_fd() != 0 ||
        test_native_runtime_link_and_skip() != 0 ||
        test_native_runtime_pins_bound_fd() != 0) {
        return 1;
    }
#endif
    puts("LEIR native segment binding tests passed");
    return 0;
}
