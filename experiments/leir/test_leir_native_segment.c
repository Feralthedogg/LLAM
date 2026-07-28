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
#include <sys/wait.h>
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

static int bind_fixture_init_mode(
    bind_fixture_t *fixture,
    bool signed_length,
    leir_native_mode_t mode) {
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
            mode) != 0 ||
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

static int bind_fixture_init(
    bind_fixture_t *fixture,
    bool signed_length) {
    return bind_fixture_init_mode(
        fixture,
        signed_length,
        LEIR_NATIVE_MODE_LINK);
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

static int bind_fixture_bind(bind_fixture_t *fixture) {
    return leir_native_instance_bind(
        fixture->instance,
        fixture->values,
        fixture->value_count);
}

static int expect_batch_failure(
    leir_native_instance_t *const *instances,
    leir_phase0_value_t *const *values_out,
    const size_t *value_counts,
    leir_native_metrics_t *metrics_out,
    size_t instance_count,
    int expected_errno) {
    leir_native_batch_metrics_t batch_metrics;

    memset(&batch_metrics, 0xa5, sizeof(batch_metrics));
    errno = 0;
    if (leir_native_batch_run(
            instances,
            values_out,
            value_counts,
            metrics_out,
            instance_count,
            &batch_metrics) == 0 ||
        errno != expected_errno) {
        fprintf(
            stderr,
            "batch failure mismatch expected=%d actual=%d\n",
            expected_errno,
            errno);
        return 1;
    }
    return 0;
}

static int test_batch_validates_width_and_members(void) {
    bind_fixture_t first;
    bind_fixture_t second;
    leir_native_instance_t *instances[9];
    leir_phase0_value_t first_out[LEIR_PHASE0_MAX_SLOTS];
    leir_phase0_value_t second_out[LEIR_PHASE0_MAX_SLOTS];
    leir_phase0_value_t *values_out[9];
    size_t value_counts[9];
    leir_native_metrics_t metrics[9];
    size_t i;
    int failed = 0;

    if (bind_fixture_init(&first, false) != 0 ||
        bind_fixture_init_mode(
            &second,
            false,
            LEIR_NATIVE_MODE_LINK_CQE_SKIP) != 0 ||
        bind_fixture_bind(&first) != 0 ||
        bind_fixture_bind(&second) != 0) {
        perror("batch validation fixture init");
        bind_fixture_destroy(&first);
        bind_fixture_destroy(&second);
        return 1;
    }
    memset(first_out, 0, sizeof(first_out));
    memset(second_out, 0, sizeof(second_out));
    memset(metrics, 0, sizeof(metrics));
    for (i = 0U; i < 9U; i += 1U) {
        instances[i] = first.instance;
        values_out[i] = first_out;
        value_counts[i] = first.value_count;
    }

    if (expect_batch_failure(
            instances,
            values_out,
            value_counts,
            metrics,
            0U,
            EINVAL) != 0 ||
        expect_batch_failure(
            instances,
            values_out,
            value_counts,
            metrics,
            9U,
            EINVAL) != 0 ||
        expect_batch_failure(
            instances,
            values_out,
            value_counts,
            metrics,
            2U,
            EINVAL) != 0) {
        failed = 1;
        goto done;
    }

    instances[1] = second.instance;
    values_out[1] = second_out;
    value_counts[1] = second.value_count;
    if (expect_batch_failure(
            instances,
            values_out,
            value_counts,
            metrics,
            2U,
            EINVAL) != 0) {
        failed = 1;
        goto done;
    }

    if (leir_native_instance_bind(
            second.instance,
            second.values,
            second.value_count) != 0) {
        perror("batch rejection left second instance busy");
        failed = 1;
    }

done:
    bind_fixture_destroy(&first);
    bind_fixture_destroy(&second);
    return failed;
}

static int test_batch_rejects_unbound_member_and_rolls_back(void) {
    bind_fixture_t first;
    bind_fixture_t second;
    leir_native_instance_t *instances[2];
    leir_phase0_value_t outputs[2][LEIR_PHASE0_MAX_SLOTS];
    leir_phase0_value_t *values_out[2] = {
        outputs[0],
        outputs[1],
    };
    size_t value_counts[2];
    leir_native_metrics_t metrics[2];
    int failed = 0;

    if (bind_fixture_init(&first, false) != 0 ||
        bind_fixture_init(&second, false) != 0 ||
        bind_fixture_bind(&first) != 0) {
        perror("unbound batch fixture init");
        bind_fixture_destroy(&first);
        bind_fixture_destroy(&second);
        return 1;
    }
    instances[0] = first.instance;
    instances[1] = second.instance;
    value_counts[0] = first.value_count;
    value_counts[1] = second.value_count;
    memset(outputs, 0, sizeof(outputs));
    memset(metrics, 0, sizeof(metrics));

    if (expect_batch_failure(
            instances,
            values_out,
            value_counts,
            metrics,
            2U,
            EINVAL) != 0 ||
        leir_native_instance_bind(
            first.instance,
            first.values,
            first.value_count) != 0) {
        fprintf(
            stderr,
            "unbound batch rejection did not restore instances\n");
        failed = 1;
    }
    bind_fixture_destroy(&first);
    bind_fixture_destroy(&second);
    return failed;
}

static int test_valid_batch_without_runtime_releases_instances(void) {
    bind_fixture_t first;
    bind_fixture_t second;
    leir_native_instance_t *instances[2];
    leir_phase0_value_t outputs[2][LEIR_PHASE0_MAX_SLOTS];
    leir_phase0_value_t *values_out[2] = {
        outputs[0],
        outputs[1],
    };
    size_t value_counts[2];
    leir_native_metrics_t metrics[2];
    int expected_errno;
    int failed = 0;

    if (bind_fixture_init(&first, false) != 0 ||
        bind_fixture_init(&second, false) != 0 ||
        bind_fixture_bind(&first) != 0 ||
        bind_fixture_bind(&second) != 0) {
        perror("valid batch fixture init");
        bind_fixture_destroy(&first);
        bind_fixture_destroy(&second);
        return 1;
    }
    instances[0] = first.instance;
    instances[1] = second.instance;
    value_counts[0] = first.value_count;
    value_counts[1] = second.value_count;
    memset(outputs, 0, sizeof(outputs));
    memset(metrics, 0, sizeof(metrics));
#if defined(__linux__)
    expected_errno = EINVAL;
#else
    expected_errno = ENOTSUP;
#endif
    if (expect_batch_failure(
            instances,
            values_out,
            value_counts,
            metrics,
            2U,
            expected_errno) != 0 ||
        leir_native_instance_bind(
            first.instance,
            first.values,
            first.value_count) != 0 ||
        leir_native_instance_bind(
            second.instance,
            second.values,
            second.value_count) != 0) {
        fprintf(
            stderr,
            "valid batch failure did not release instances\n");
        failed = 1;
    }
    bind_fixture_destroy(&first);
    bind_fixture_destroy(&second);
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

#if !LLAM_PLATFORM_WINDOWS
typedef struct native_destroy_bind_race {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_cond_t release;
    bind_fixture_t *fixture;
    bool entered;
    bool resume;
    int bind_result;
    int bind_errno;
} native_destroy_bind_race_t;

static void native_destroy_bind_hook(void *context) {
    native_destroy_bind_race_t *race = context;

    pthread_mutex_lock(&race->lock);
    race->entered = true;
    pthread_cond_signal(&race->ready);
    while (!race->resume) {
        pthread_cond_wait(&race->release, &race->lock);
    }
    pthread_mutex_unlock(&race->lock);
}

static void *native_destroy_bind_thread(void *context) {
    native_destroy_bind_race_t *race = context;

    errno = 0;
    race->bind_result = leir_native_instance_bind(
        race->fixture->instance,
        race->fixture->values,
        race->fixture->value_count);
    race->bind_errno = errno;
    return NULL;
}

static int run_native_destroy_bind_race_child(void) {
    bind_fixture_t fixture;
    native_destroy_bind_race_t race;
    pthread_t thread;
    int destroy_result;
    int destroy_errno;
    int failed = 1;

    memset(&race, 0, sizeof(race));
    if (bind_fixture_init(&fixture, false) != 0 ||
        pthread_mutex_init(&race.lock, NULL) != 0 ||
        pthread_cond_init(&race.ready, NULL) != 0 ||
        pthread_cond_init(&race.release, NULL) != 0) {
        return 1;
    }
    race.fixture = &fixture;
    leir_native_test_set_bind_before_claim_hook(
        native_destroy_bind_hook, &race);
    if (pthread_create(
            &thread, NULL, native_destroy_bind_thread, &race) != 0) {
        leir_native_test_set_bind_before_claim_hook(NULL, NULL);
        goto cleanup;
    }
    pthread_mutex_lock(&race.lock);
    while (!race.entered) {
        pthread_cond_wait(&race.ready, &race.lock);
    }
    pthread_mutex_unlock(&race.lock);

    errno = 0;
    destroy_result =
        leir_native_instance_destroy(fixture.instance);
    destroy_errno = errno;
    if (destroy_result == 0) {
        fixture.instance = NULL;
    }
    pthread_mutex_lock(&race.lock);
    race.resume = true;
    pthread_cond_signal(&race.release);
    pthread_mutex_unlock(&race.lock);
    if (pthread_join(thread, NULL) != 0) {
        leir_native_test_set_bind_before_claim_hook(NULL, NULL);
        goto cleanup;
    }
    leir_native_test_set_bind_before_claim_hook(NULL, NULL);
    if (destroy_result == 0 &&
        destroy_errno == 0 &&
        race.bind_result == -1 &&
        race.bind_errno == EINVAL) {
        failed = 0;
    } else {
        fprintf(
            stderr,
            "destroy/bind ABA: destroy=%d/%d bind=%d/%d\n",
            destroy_result,
            destroy_errno,
            race.bind_result,
            race.bind_errno);
    }

cleanup:
    leir_native_test_set_bind_before_claim_hook(NULL, NULL);
    pthread_cond_destroy(&race.release);
    pthread_cond_destroy(&race.ready);
    pthread_mutex_destroy(&race.lock);
    bind_fixture_destroy(&fixture);
    return failed;
}

static int test_destroyed_instance_cannot_reenter_bind(void) {
    unsigned iteration;

    for (iteration = 0U; iteration < 32U; iteration += 1U) {
        pid_t child = fork();
        int status;

        if (child < 0) {
            return 1;
        }
        if (child == 0) {
            _Exit(run_native_destroy_bind_race_child());
        }
        if (waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            fputs(
                "destroyed native instance reentered bind\n",
                stderr);
            return 1;
        }
    }
    return 0;
}
#endif

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

typedef struct native_batch_integration_state {
    native_integration_state_t lanes[2];
    atomic_uint batch_done;
} native_batch_integration_state_t;

static bool native_batch_lane_metrics_are_expected(
    const native_integration_state_t *lane,
    const leir_native_metrics_t *metrics,
    unsigned activation,
    bool ticket_owner) {
    uint64_t activations = (uint64_t)activation + 1U;
    uint64_t ticket_events =
        ticket_owner ? activations : 0U;

    return metrics->activations == activations &&
           metrics->logical_operations ==
               activations * lane->op_count &&
           metrics->queue_publications == ticket_events &&
           metrics->prepared_sqes ==
               activations * lane->op_count &&
           metrics->observed_cqes ==
               activations * lane->op_count &&
           metrics->suppressed_success_cqes == 0U &&
           metrics->task_parks == ticket_events &&
           metrics->terminal_wakes == ticket_events &&
           metrics->hot_allocations == 0U &&
           metrics->first_error_operation == UINT16_MAX;
}

static void native_batch_integration_task(void *arg) {
    native_batch_integration_state_t *state = arg;
    leir_native_instance_t *instances[2] = {
        state->lanes[0].instance,
        state->lanes[1].instance,
    };
    leir_phase0_value_t *values_out[2] = {
        state->lanes[0].values_out,
        state->lanes[1].values_out,
    };
    size_t value_counts[2] = {
        INTEGRATION_SLOT_COUNT,
        INTEGRATION_SLOT_COUNT,
    };
    unsigned activation;

    for (activation = 0U;
         activation < INTEGRATION_ACTIVATIONS;
         activation += 1U) {
        leir_native_metrics_t metrics[2];
        leir_native_batch_metrics_t batch_metrics;
        unsigned i;

        memset(metrics, 0, sizeof(metrics));
        memset(&batch_metrics, 0, sizeof(batch_metrics));
        for (i = 0U; i < 2U; i += 1U) {
            memset(
                state->lanes[i].values_out,
                0,
                sizeof(state->lanes[i].values_out));
        }
        if (leir_native_batch_run(
                instances,
                values_out,
                value_counts,
                metrics,
                2U,
                &batch_metrics) != 0) {
            native_integration_fail(
                &state->lanes[0],
                "native batch run",
                errno);
            goto done;
        }
        if (batch_metrics.activations != 1U ||
            batch_metrics.segments != 2U ||
            batch_metrics.queue_publications != 1U ||
            batch_metrics.task_parks != 1U ||
            batch_metrics.terminal_wakes != 1U ||
            batch_metrics.operation_sqes != 4U ||
            batch_metrics.operation_cqes != 4U ||
            batch_metrics.cancel_sqes != 0U ||
            batch_metrics.cancel_cqes != 0U ||
            batch_metrics.hot_allocations != 0U) {
            native_integration_fail(
                &state->lanes[0],
                "native batch metrics",
                EPROTO);
            goto done;
        }
        for (i = 0U; i < 2U; i += 1U) {
            if (state->lanes[i].values_out[
                    INTEGRATION_RESULT_SLOT].i64 !=
                    INTEGRATION_BYTES ||
                !native_batch_lane_metrics_are_expected(
                    &state->lanes[i],
                    &metrics[i],
                    activation,
                    i == 0U)) {
                native_integration_fail(
                    &state->lanes[i],
                    "native batch lane result",
                    EPROTO);
                goto done;
            }
        }
    }

done:
    atomic_store_explicit(
        &state->batch_done, 1U, memory_order_release);
    atomic_store_explicit(
        &state->lanes[0].instance_done,
        1U,
        memory_order_release);
    atomic_store_explicit(
        &state->lanes[1].instance_done,
        1U,
        memory_order_release);
}

static int test_native_runtime_batches_width_two(void) {
    native_batch_integration_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *peers[2] = {NULL, NULL};
    llam_task_t *batch_task = NULL;
    bool runtime_started = false;
    int result = NATIVE_INTEGRATION_FAIL;
    unsigned i;

    memset(&state, 0, sizeof(state));
    for (i = 0U; i < 2U; i += 1U) {
        state.lanes[i].pair[0] = LLAM_INVALID_FD;
        state.lanes[i].pair[1] = LLAM_INVALID_FD;
        state.lanes[i].replacement_pair[0] =
            LLAM_INVALID_FD;
        state.lanes[i].replacement_pair[1] =
            LLAM_INVALID_FD;
    }
    atomic_init(&state.batch_done, 0U);
    if (native_integration_state_init(
            &state.lanes[0],
            2U,
            LEIR_NATIVE_MODE_LINK) != 0 ||
        native_integration_state_init(
            &state.lanes[1],
            2U,
            LEIR_NATIVE_MODE_LINK) != 0) {
        perror("native batch integration fixture init");
        goto cleanup;
    }
    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.forced_yield_every = 1U;
    opts.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&opts) != 0) {
        perror("native batch integration runtime init");
        goto cleanup;
    }
    runtime_started = true;
    if (g_llam_runtime.active_nodes == 0U ||
        g_llam_runtime.nodes == NULL ||
        !g_llam_runtime.nodes[0].ring_ready ||
        !g_llam_runtime.nodes[0].supports_send) {
        result = NATIVE_INTEGRATION_UNAVAILABLE;
        goto shutdown;
    }
    for (i = 0U; i < 2U; i += 1U) {
        peers[i] = llam_spawn(
            native_integration_peer_task,
            &state.lanes[i],
            NULL);
    }
    batch_task = llam_spawn(
        native_batch_integration_task,
        &state,
        NULL);
    if (peers[0] == NULL ||
        peers[1] == NULL ||
        batch_task == NULL ||
        llam_run() != 0) {
        goto shutdown;
    }
    for (i = 0U; i < 2U; i += 1U) {
        if (llam_join(peers[i]) != 0) {
            goto shutdown;
        }
        peers[i] = NULL;
    }
    if (llam_join(batch_task) != 0) {
        goto shutdown;
    }
    batch_task = NULL;
    if (atomic_load_explicit(
            &state.lanes[0].failures,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &state.lanes[1].failures,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &state.batch_done,
            memory_order_acquire) == 0U) {
        fprintf(
            stderr,
            "native width-two batch integration failed\n");
        goto shutdown;
    }
    result = NATIVE_INTEGRATION_PASS;

shutdown:
    if (runtime_started) {
        llam_runtime_shutdown();
    }
cleanup:
    native_integration_state_destroy(&state.lanes[0]);
    native_integration_state_destroy(&state.lanes[1]);
    if (result == NATIVE_INTEGRATION_UNAVAILABLE) {
        puts(
            "SKIP: Linux io_uring native batch "
            "integration unavailable");
        return 0;
    }
    return result == NATIVE_INTEGRATION_PASS ? 0 : 1;
}

enum {
    FIXED_PIPELINE_FD_SLOT = 0U,
    FIXED_PIPELINE_LENGTH_SLOT = 1U,
    FIXED_PIPELINE_BUFFER_SLOT = 2U,
    FIXED_PIPELINE_RECV_RESULT_SLOT = 3U,
    FIXED_PIPELINE_SEND_RESULT_SLOT = 4U,
    FIXED_PIPELINE_SLOT_COUNT = 5U,
    FIXED_PIPELINE_BYTES = 64U,
};

typedef struct fixed_pipeline_state {
    leir_phase0_program_t *program;
    leir_native_plan_t plan;
    leir_native_mode_t mode;
    test_instance_storage_t storage;
    leir_native_instance_t *instance;
    leir_phase0_value_t values[LEIR_PHASE0_MAX_SLOTS];
    leir_phase0_value_t values_out[LEIR_PHASE0_MAX_SLOTS];
    unsigned char external_buffer[FIXED_PIPELINE_BYTES];
    unsigned char payload[FIXED_PIPELINE_BYTES];
    unsigned char echoed[FIXED_PIPELINE_BYTES];
    llam_fd_t pair[2];
    atomic_uint failures;
    int first_errno;
} fixed_pipeline_state_t;

static int create_fixed_pipeline_program(
    leir_phase0_program_t **out) {
    leir_phase0_node_desc_t nodes[4];
    leir_phase0_slot_kind_t
        slots[FIXED_PIPELINE_SLOT_COUNT];
    leir_phase0_program_desc_t desc;

    memset(nodes, 0, sizeof(nodes));
    memset(slots, 0, sizeof(slots));
    slots[FIXED_PIPELINE_FD_SLOT] = LEIR_PHASE0_SLOT_FD;
    slots[FIXED_PIPELINE_LENGTH_SLOT] =
        LEIR_PHASE0_SLOT_U64;
    slots[FIXED_PIPELINE_BUFFER_SLOT] =
        LEIR_PHASE0_SLOT_MUT_BUFFER;
    slots[FIXED_PIPELINE_RECV_RESULT_SLOT] =
        LEIR_PHASE0_SLOT_I64;
    slots[FIXED_PIPELINE_SEND_RESULT_SLOT] =
        LEIR_PHASE0_SLOT_I64;

    nodes[0].opcode = LEIR_PHASE0_OP_READ_EXACT;
    nodes[0].fd_slot = FIXED_PIPELINE_FD_SLOT;
    nodes[0].buffer_slot = FIXED_PIPELINE_BUFFER_SLOT;
    nodes[0].length_slot = FIXED_PIPELINE_LENGTH_SLOT;
    nodes[0].result_slot =
        FIXED_PIPELINE_RECV_RESULT_SLOT;
    nodes[0].on_success = 1U;
    nodes[0].on_eof = 3U;
    nodes[0].on_error = 3U;

    nodes[1].opcode = LEIR_PHASE0_OP_WRITE_ALL;
    nodes[1].fd_slot = FIXED_PIPELINE_FD_SLOT;
    nodes[1].buffer_slot = FIXED_PIPELINE_BUFFER_SLOT;
    nodes[1].length_slot = FIXED_PIPELINE_LENGTH_SLOT;
    nodes[1].result_slot =
        FIXED_PIPELINE_SEND_RESULT_SLOT;
    nodes[1].on_success = 2U;
    nodes[1].on_eof = 3U;
    nodes[1].on_error = 3U;
    nodes[2] = terminal_node(
        LEIR_PHASE0_OP_RETURN,
        FIXED_PIPELINE_SEND_RESULT_SLOT);
    nodes[3] = terminal_node(
        LEIR_PHASE0_OP_FAIL,
        FIXED_PIPELINE_RECV_RESULT_SLOT);

    memset(&desc, 0, sizeof(desc));
    desc.nodes = nodes;
    desc.slot_kinds = slots;
    desc.node_count = 4U;
    desc.slot_count = FIXED_PIPELINE_SLOT_COUNT;
    desc.entry_node = 0U;
    return leir_phase0_program_create(&desc, out);
}

static int fixed_pipeline_state_init(
    fixed_pipeline_state_t *state,
    leir_native_mode_t mode) {
    memset(state, 0, sizeof(*state));
    state->mode = mode;
    state->pair[0] = LLAM_INVALID_FD;
    state->pair[1] = LLAM_INVALID_FD;
    atomic_init(&state->failures, 0U);
    if (create_fixed_pipeline_program(
            &state->program) != 0 ||
        leir_native_plan_compile(
            state->program, &state->plan) != 0 ||
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
    state->values[FIXED_PIPELINE_FD_SLOT].fd =
        state->pair[0];
    state->values[FIXED_PIPELINE_LENGTH_SLOT].u64 =
        FIXED_PIPELINE_BYTES;
    state->values[
        FIXED_PIPELINE_BUFFER_SLOT].buffer.data =
        state->external_buffer;
    state->values[
        FIXED_PIPELINE_BUFFER_SLOT].buffer.size =
        sizeof(state->external_buffer);
    leir_test_fill_pattern(
        state->payload,
        sizeof(state->payload),
        UINT64_C(0x46495845444c4549));
    return leir_native_instance_bind(
        state->instance,
        state->values,
        FIXED_PIPELINE_SLOT_COUNT);
}

static void fixed_pipeline_fail(
    fixed_pipeline_state_t *state,
    int error) {
    if (atomic_fetch_add_explicit(
            &state->failures,
            1U,
            memory_order_relaxed) == 0U) {
        state->first_errno = error;
    }
    if (!LLAM_FD_IS_INVALID(state->pair[0])) {
        (void)shutdown((int)state->pair[0], SHUT_RDWR);
    }
    if (!LLAM_FD_IS_INVALID(state->pair[1])) {
        (void)shutdown((int)state->pair[1], SHUT_RDWR);
    }
}

static void fixed_pipeline_peer_task(void *arg) {
    fixed_pipeline_state_t *state = arg;

    if (leir_test_write_all(
            state->pair[1],
            state->payload,
            sizeof(state->payload)) != 0 ||
        leir_test_read_exact(
            state->pair[1],
            state->echoed,
            sizeof(state->echoed)) != 0 ||
        memcmp(
            state->echoed,
            state->payload,
            sizeof(state->payload)) != 0) {
        fixed_pipeline_fail(
            state, errno != 0 ? errno : EPROTO);
    }
}

static void fixed_pipeline_instance_task(void *arg) {
    fixed_pipeline_state_t *state = arg;
    leir_native_metrics_t metrics;
    uint64_t expected_cqes =
        state->mode ==
                LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP
            ? 1U
            : 2U;
    uint64_t expected_suppressed =
        state->mode ==
                LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP
            ? 1U
            : 0U;

    memset(&metrics, 0, sizeof(metrics));
    if (leir_native_instance_run(
            state->instance,
            state->values_out,
            FIXED_PIPELINE_SLOT_COUNT,
            &metrics) != 0 ||
        state->values_out[
            FIXED_PIPELINE_RECV_RESULT_SLOT].i64 !=
            FIXED_PIPELINE_BYTES ||
        state->values_out[
            FIXED_PIPELINE_SEND_RESULT_SLOT].i64 !=
            FIXED_PIPELINE_BYTES ||
        memcmp(
            state->external_buffer,
            state->payload,
            sizeof(state->payload)) != 0 ||
        metrics.activations != 1U ||
        metrics.logical_operations != 2U ||
        metrics.prepared_sqes != 2U ||
        metrics.observed_cqes != expected_cqes ||
        metrics.suppressed_success_cqes !=
            expected_suppressed ||
        metrics.task_parks != 1U ||
        metrics.terminal_wakes != 1U ||
        metrics.hot_allocations != 0U) {
        fixed_pipeline_fail(
            state, errno != 0 ? errno : EPROTO);
    }
}

static int run_fixed_recv_send_pipeline(
    leir_native_mode_t mode,
    bool destroy_after_shutdown) {
    fixed_pipeline_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *peer = NULL;
    llam_task_t *instance_task = NULL;
    bool runtime_started = false;
    bool fixed_available = false;
    int failed = 1;

    if (fixed_pipeline_state_init(&state, mode) != 0) {
        perror("fixed pipeline fixture init");
        return 1;
    }
    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.forced_yield_every = 1U;
    if (llam_runtime_init(&opts) != 0) {
        perror("fixed pipeline runtime init");
        goto cleanup;
    }
    runtime_started = true;
    fixed_available =
        g_llam_runtime.active_nodes != 0U &&
        g_llam_runtime.nodes != NULL &&
        g_llam_runtime.nodes[0].ring_ready &&
        g_llam_runtime.nodes[0]
            .supports_native_fixed_files &&
        g_llam_runtime.nodes[0]
            .supports_native_fixed_buffers &&
        (mode != LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP ||
         (g_llam_runtime.nodes[0].linux_ring_features &
          IORING_FEAT_CQE_SKIP) != 0U);
    if (!fixed_available) {
        failed = 0;
        goto destroy_instance;
    }

    peer = llam_spawn(
        fixed_pipeline_peer_task, &state, NULL);
    instance_task = llam_spawn(
        fixed_pipeline_instance_task, &state, NULL);
    if (peer == NULL ||
        instance_task == NULL ||
        llam_run() != 0 ||
        llam_join(peer) != 0 ||
        llam_join(instance_task) != 0 ||
        atomic_load_explicit(
            &state.failures,
            memory_order_acquire) != 0U ||
        __builtin_popcountll(
            g_llam_runtime.nodes[0].
                native_fixed_file_bitmap) != 1 ||
        __builtin_popcountll(
            g_llam_runtime.nodes[0].
                native_fixed_buffer_bitmap) != 1) {
        fprintf(
            stderr,
            "fixed pipeline failed errno=%d\n",
            state.first_errno);
        goto destroy_instance;
    }
    failed = 0;

destroy_instance:
    if (!destroy_after_shutdown &&
        state.instance != NULL) {
        if (leir_native_instance_destroy(
                state.instance) != 0) {
            perror("destroy fixed pipeline instance");
            failed = 1;
        } else {
            state.instance = NULL;
        }
    }
    if (fixed_available &&
        !destroy_after_shutdown &&
        (g_llam_runtime.nodes[0].
             native_fixed_file_bitmap != 0U ||
         g_llam_runtime.nodes[0].
             native_fixed_buffer_bitmap != 0U)) {
        fprintf(stderr, "fixed pipeline lease was not detached\n");
        failed = 1;
    }
    if (!fixed_available) {
        puts(
            "SKIP: Linux fixed-file/buffer "
            "registration unavailable");
    }

cleanup:
    if (runtime_started) {
        llam_runtime_shutdown();
        runtime_started = false;
    }
    if (state.instance != NULL) {
        if (leir_native_instance_destroy(
                state.instance) != 0) {
            perror(
                "destroy fixed pipeline instance "
                "after runtime shutdown");
            failed = 1;
        } else {
            state.instance = NULL;
        }
    }
    leir_test_close(&state.pair[0]);
    leir_test_close(&state.pair[1]);
    leir_phase0_program_destroy(state.program);
    return failed;
}

static int test_fixed_recv_send_pipeline(void) {
    static const struct {
        leir_native_mode_t mode;
        bool destroy_after_shutdown;
    } cases[] = {
        {LEIR_NATIVE_MODE_FIXED_LINK, false},
        {LEIR_NATIVE_MODE_FIXED_LINK, true},
        {LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP, true},
    };
    size_t i;

    for (i = 0U;
         i < sizeof(cases) / sizeof(cases[0]);
         i += 1U) {
        if (run_fixed_recv_send_pipeline(
                cases[i].mode,
                cases[i].destroy_after_shutdown) != 0) {
            return 1;
        }
    }
    return 0;
}

enum {
    FIXED_STALE_FD_SLOT = 0U,
    FIXED_STALE_SHORT_LENGTH_SLOT = 1U,
    FIXED_STALE_LONG_LENGTH_SLOT = 2U,
    FIXED_STALE_BUFFER_SLOT = 3U,
    FIXED_STALE_FIRST_RESULT_SLOT = 4U,
    FIXED_STALE_SECOND_RESULT_SLOT = 5U,
    FIXED_STALE_SLOT_COUNT = 6U,
    FIXED_STALE_SHORT_BYTES = 4U,
    FIXED_STALE_LONG_BYTES = 64U,
};

typedef struct fixed_stale_state {
    leir_phase0_program_t *program;
    leir_native_plan_t plan;
    test_instance_storage_t storage;
    leir_native_instance_t *instance;
    leir_phase0_value_t values[LEIR_PHASE0_MAX_SLOTS];
    leir_phase0_value_t values_out[LEIR_PHASE0_MAX_SLOTS];
    unsigned char external_buffer[FIXED_STALE_LONG_BYTES];
    unsigned char prior_payload[FIXED_STALE_LONG_BYTES];
    unsigned char short_payloads[3][FIXED_STALE_SHORT_BYTES];
    llam_fd_t pair[2];
    atomic_uint failures;
    int first_errno;
} fixed_stale_state_t;

static int create_fixed_stale_program(
    leir_phase0_program_t **out) {
    leir_phase0_node_desc_t nodes[4];
    leir_phase0_slot_kind_t slots[FIXED_STALE_SLOT_COUNT];
    leir_phase0_program_desc_t desc;

    memset(nodes, 0, sizeof(nodes));
    memset(slots, 0, sizeof(slots));
    slots[FIXED_STALE_FD_SLOT] = LEIR_PHASE0_SLOT_FD;
    slots[FIXED_STALE_SHORT_LENGTH_SLOT] =
        LEIR_PHASE0_SLOT_U64;
    slots[FIXED_STALE_LONG_LENGTH_SLOT] =
        LEIR_PHASE0_SLOT_U64;
    slots[FIXED_STALE_BUFFER_SLOT] =
        LEIR_PHASE0_SLOT_MUT_BUFFER;
    slots[FIXED_STALE_FIRST_RESULT_SLOT] =
        LEIR_PHASE0_SLOT_I64;
    slots[FIXED_STALE_SECOND_RESULT_SLOT] =
        LEIR_PHASE0_SLOT_I64;

    nodes[0].opcode = LEIR_PHASE0_OP_READ_EXACT;
    nodes[0].fd_slot = FIXED_STALE_FD_SLOT;
    nodes[0].buffer_slot = FIXED_STALE_BUFFER_SLOT;
    nodes[0].length_slot = FIXED_STALE_SHORT_LENGTH_SLOT;
    nodes[0].result_slot = FIXED_STALE_FIRST_RESULT_SLOT;
    nodes[0].on_success = 1U;
    nodes[0].on_eof = 3U;
    nodes[0].on_error = 3U;

    nodes[1].opcode = LEIR_PHASE0_OP_READ_EXACT;
    nodes[1].fd_slot = FIXED_STALE_FD_SLOT;
    nodes[1].buffer_slot = FIXED_STALE_BUFFER_SLOT;
    nodes[1].length_slot = FIXED_STALE_LONG_LENGTH_SLOT;
    nodes[1].result_slot = FIXED_STALE_SECOND_RESULT_SLOT;
    nodes[1].on_success = 2U;
    nodes[1].on_eof = 3U;
    nodes[1].on_error = 3U;
    nodes[2] = terminal_node(
        LEIR_PHASE0_OP_RETURN,
        FIXED_STALE_SECOND_RESULT_SLOT);
    nodes[3] = terminal_node(
        LEIR_PHASE0_OP_FAIL,
        FIXED_STALE_SECOND_RESULT_SLOT);

    memset(&desc, 0, sizeof(desc));
    desc.nodes = nodes;
    desc.slot_kinds = slots;
    desc.node_count = 4U;
    desc.slot_count = FIXED_STALE_SLOT_COUNT;
    desc.entry_node = 0U;
    return leir_phase0_program_create(&desc, out);
}

static int fixed_stale_state_init(fixed_stale_state_t *state) {
    unsigned i;

    memset(state, 0, sizeof(*state));
    state->pair[0] = LLAM_INVALID_FD;
    state->pair[1] = LLAM_INVALID_FD;
    atomic_init(&state->failures, 0U);
    if (create_fixed_stale_program(&state->program) != 0 ||
        leir_native_plan_compile(
            state->program, &state->plan) != 0 ||
        leir_native_instance_init(
            state->storage.bytes,
            sizeof(state->storage.bytes),
            state->program,
            &state->plan,
            LEIR_NATIVE_MODE_FIXED_LINK) != 0 ||
        leir_test_socketpair_type(
            SOCK_SEQPACKET, state->pair) != 0) {
        return -1;
    }
    state->instance =
        (leir_native_instance_t *)state->storage.bytes;
    state->values[FIXED_STALE_FD_SLOT].fd =
        state->pair[0];
    state->values[FIXED_STALE_SHORT_LENGTH_SLOT].u64 =
        FIXED_STALE_SHORT_BYTES;
    state->values[FIXED_STALE_LONG_LENGTH_SLOT].u64 =
        FIXED_STALE_LONG_BYTES;
    state->values[FIXED_STALE_BUFFER_SLOT].buffer.data =
        state->external_buffer;
    state->values[FIXED_STALE_BUFFER_SLOT].buffer.size =
        sizeof(state->external_buffer);
    leir_test_fill_pattern(
        state->prior_payload,
        sizeof(state->prior_payload),
        UINT64_C(0x5052494f52534543));
    for (i = 0U; i < 3U; i += 1U) {
        leir_test_fill_pattern(
            state->short_payloads[i],
            sizeof(state->short_payloads[i]),
            UINT64_C(0x53484f5254000000) + i);
    }
    return leir_native_instance_bind(
        state->instance,
        state->values,
        FIXED_STALE_SLOT_COUNT);
}

static void fixed_stale_fail(
    fixed_stale_state_t *state,
    int error) {
    if (atomic_fetch_add_explicit(
            &state->failures,
            1U,
            memory_order_relaxed) == 0U) {
        state->first_errno = error;
    }
    if (!LLAM_FD_IS_INVALID(state->pair[0])) {
        (void)shutdown((int)state->pair[0], SHUT_RDWR);
    }
    if (!LLAM_FD_IS_INVALID(state->pair[1])) {
        (void)shutdown((int)state->pair[1], SHUT_RDWR);
    }
}

static void fixed_stale_peer_task(void *arg) {
    fixed_stale_state_t *state = arg;

    if (leir_test_write_all(
            state->pair[1],
            state->short_payloads[0],
            FIXED_STALE_SHORT_BYTES) != 0 ||
        leir_test_write_all(
            state->pair[1],
            state->prior_payload,
            FIXED_STALE_LONG_BYTES) != 0 ||
        leir_test_write_all(
            state->pair[1],
            state->short_payloads[1],
            FIXED_STALE_SHORT_BYTES) != 0 ||
        leir_test_write_all(
            state->pair[1],
            state->short_payloads[2],
            FIXED_STALE_SHORT_BYTES) != 0) {
        fixed_stale_fail(
            state, errno != 0 ? errno : EPROTO);
    }
}

static void fixed_stale_instance_task(void *arg) {
    fixed_stale_state_t *state = arg;
    leir_native_metrics_t metrics;
    size_t i;
    int run_result;
    int run_errno;

    memset(&metrics, 0, sizeof(metrics));
    if (leir_native_instance_run(
            state->instance,
            state->values_out,
            FIXED_STALE_SLOT_COUNT,
            &metrics) != 0 ||
        memcmp(
            state->external_buffer,
            state->prior_payload,
            sizeof(state->prior_payload)) != 0) {
        fixed_stale_fail(
            state, errno != 0 ? errno : EPROTO);
        return;
    }

    memset(
        state->external_buffer,
        0,
        sizeof(state->external_buffer));
    memset(&metrics, 0, sizeof(metrics));
    errno = 0;
    run_result = leir_native_instance_run(
        state->instance,
        state->values_out,
        FIXED_STALE_SLOT_COUNT,
        &metrics);
    run_errno = errno;
    if (run_result != -1 ||
        run_errno != EMSGSIZE ||
        metrics.first_error_operation != 1U ||
        state->values_out[
            FIXED_STALE_FIRST_RESULT_SLOT].i64 !=
            FIXED_STALE_SHORT_BYTES ||
        state->values_out[
            FIXED_STALE_SECOND_RESULT_SLOT].i64 != -1) {
        fixed_stale_fail(
            state, run_errno != 0 ? run_errno : EPROTO);
        return;
    }
    for (i = FIXED_STALE_SHORT_BYTES;
         i < sizeof(state->external_buffer);
         i += 1U) {
        if (state->external_buffer[i] != 0U) {
            fixed_stale_fail(state, EPROTO);
            return;
        }
    }
}

static int test_fixed_short_read_does_not_copy_prior_activation(void) {
    fixed_stale_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *peer = NULL;
    llam_task_t *instance_task = NULL;
    bool runtime_started = false;
    bool fixed_available = false;
    int failed = 1;

    if (fixed_stale_state_init(&state) != 0) {
        perror("fixed stale fixture init");
        return 1;
    }
    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.forced_yield_every = 1U;
    if (llam_runtime_init(&opts) != 0) {
        perror("fixed stale runtime init");
        goto cleanup;
    }
    runtime_started = true;
    fixed_available =
        g_llam_runtime.active_nodes != 0U &&
        g_llam_runtime.nodes != NULL &&
        g_llam_runtime.nodes[0].ring_ready &&
        g_llam_runtime.nodes[0].linux_submit_all &&
        g_llam_runtime.nodes[0].supports_native_fixed_files &&
        g_llam_runtime.nodes[0].supports_native_fixed_buffers;
    if (!fixed_available) {
        failed = 0;
        goto cleanup;
    }

    peer = llam_spawn(fixed_stale_peer_task, &state, NULL);
    instance_task = llam_spawn(
        fixed_stale_instance_task, &state, NULL);
    if (peer == NULL ||
        instance_task == NULL ||
        llam_run() != 0 ||
        llam_join(peer) != 0 ||
        llam_join(instance_task) != 0 ||
        atomic_load_explicit(
            &state.failures,
            memory_order_acquire) != 0U) {
        fprintf(
            stderr,
            "fixed stale copy failed errno=%d\n",
            state.first_errno);
        goto cleanup;
    }
    failed = 0;

cleanup:
    if (state.instance != NULL) {
        if (leir_native_instance_destroy(
                state.instance) != 0) {
            perror("destroy fixed stale instance");
            failed = 1;
        } else {
            state.instance = NULL;
        }
    }
    if (!fixed_available && runtime_started) {
        puts(
            "SKIP: Linux fixed short-read "
            "integration unavailable");
    }
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    leir_test_close(&state.pair[0]);
    leir_test_close(&state.pair[1]);
    leir_phase0_program_destroy(state.program);
    return failed;
}

typedef struct native_cancel_batch_state
    native_cancel_batch_state_t;

typedef struct native_cancel_peer_context {
    native_cancel_batch_state_t *state;
    unsigned index;
} native_cancel_peer_context_t;

struct native_cancel_batch_state {
    fixed_pipeline_state_t
        lanes[LEIR_NATIVE_MAX_BATCH_SEGMENTS];
    leir_native_instance_t
        *instances[LEIR_NATIVE_MAX_BATCH_SEGMENTS];
    leir_phase0_value_t
        *values_out[LEIR_NATIVE_MAX_BATCH_SEGMENTS];
    size_t value_counts[LEIR_NATIVE_MAX_BATCH_SEGMENTS];
    leir_native_metrics_t
        metrics[LEIR_NATIVE_MAX_BATCH_SEGMENTS];
    native_cancel_peer_context_t
        peer_contexts[LEIR_NATIVE_MAX_BATCH_SEGMENTS];
    leir_native_batch_metrics_t batch_metrics;
    llam_cancel_token_t *token;
    unsigned width;
    unsigned initialized_lanes;
    bool completion_race;
    bool runtime_stop;
    atomic_uint runner_started;
    atomic_uint release_peers;
    atomic_uint runner_done;
    atomic_uint failures;
    int first_errno;
    int run_result;
    int run_errno;
};

static void native_cancel_batch_fail(
    native_cancel_batch_state_t *state,
    int error) {
    unsigned i;

    if (atomic_fetch_add_explicit(
            &state->failures,
            1U,
            memory_order_relaxed) == 0U) {
        state->first_errno = error;
    }
    atomic_store_explicit(
        &state->release_peers, 1U, memory_order_release);
    for (i = 0U; i < state->initialized_lanes; i += 1U) {
        if (!LLAM_FD_IS_INVALID(state->lanes[i].pair[0])) {
            (void)shutdown(
                (int)state->lanes[i].pair[0],
                SHUT_RDWR);
        }
        if (!LLAM_FD_IS_INVALID(state->lanes[i].pair[1])) {
            (void)shutdown(
                (int)state->lanes[i].pair[1],
                SHUT_RDWR);
        }
    }
}

static int native_cancel_batch_state_init(
    native_cancel_batch_state_t *state,
    unsigned width,
    leir_native_mode_t mode,
    bool completion_race,
    bool runtime_stop) {
    unsigned i;

    memset(state, 0, sizeof(*state));
    state->width = width;
    state->completion_race = completion_race;
    state->runtime_stop = runtime_stop;
    atomic_init(&state->runner_started, 0U);
    atomic_init(&state->release_peers, 0U);
    atomic_init(&state->runner_done, 0U);
    atomic_init(&state->failures, 0U);
    for (i = 0U; i < width; i += 1U) {
        state->initialized_lanes = i + 1U;
        if (fixed_pipeline_state_init(
                &state->lanes[i], mode) != 0) {
            return -1;
        }
        leir_test_fill_pattern(
            state->lanes[i].payload,
            sizeof(state->lanes[i].payload),
            UINT64_C(0x43414e43454c0000) + i);
        state->instances[i] = state->lanes[i].instance;
        state->values_out[i] =
            state->lanes[i].values_out;
        state->value_counts[i] =
            FIXED_PIPELINE_SLOT_COUNT;
        state->peer_contexts[i].state = state;
        state->peer_contexts[i].index = i;
    }
    state->token = llam_cancel_token_create();
    return state->token != NULL ? 0 : -1;
}

static int native_cancel_batch_state_destroy(
    native_cancel_batch_state_t *state) {
    int result = 0;
    unsigned i;

    for (i = 0U; i < state->initialized_lanes; i += 1U) {
        if (state->lanes[i].instance != NULL) {
            if (leir_native_instance_destroy(
                    state->lanes[i].instance) != 0) {
                result = -1;
            } else {
                state->lanes[i].instance = NULL;
            }
        }
        leir_test_close(&state->lanes[i].pair[0]);
        leir_test_close(&state->lanes[i].pair[1]);
        leir_phase0_program_destroy(
            state->lanes[i].program);
        state->lanes[i].program = NULL;
    }
    state->initialized_lanes = 0U;
    if (state->token != NULL) {
        if (llam_cancel_token_destroy(state->token) != 0) {
            result = -1;
        } else {
            state->token = NULL;
        }
    }
    return result;
}

static bool native_cancel_batch_is_inflight(void) {
    llam_node_t *node;
    bool queue_empty;

    if (g_llam_runtime.nodes == NULL ||
        g_llam_runtime.active_nodes == 0U) {
        return false;
    }
    node = &g_llam_runtime.nodes[0];
    pthread_mutex_lock(&node->submit_lock);
    queue_empty = node->native_batch_head == NULL;
    pthread_mutex_unlock(&node->submit_lock);
    return queue_empty &&
           atomic_load_explicit(
               &node->pending_ops,
               memory_order_acquire) != 0U &&
           atomic_load_explicit(
               &g_llam_runtime.active_io_waiters,
               memory_order_acquire) != 0U;
}

static bool native_cancel_batch_queues_are_empty(void) {
    llam_node_t *node;
    bool empty;

    if (g_llam_runtime.nodes == NULL ||
        g_llam_runtime.active_nodes == 0U) {
        return false;
    }
    node = &g_llam_runtime.nodes[0];
    pthread_mutex_lock(&node->submit_lock);
    empty = node->native_batch_head == NULL &&
            node->native_batch_tail == NULL &&
            node->native_cancel_head == NULL &&
            node->native_cancel_tail == NULL;
    pthread_mutex_unlock(&node->submit_lock);
    return empty;
}

static void native_cancel_batch_runner_task(void *arg) {
    native_cancel_batch_state_t *state = arg;

    atomic_store_explicit(
        &state->runner_started, 1U, memory_order_release);
    errno = 0;
    state->run_result = leir_native_batch_run(
        state->instances,
        state->values_out,
        state->value_counts,
        state->metrics,
        state->width,
        &state->batch_metrics);
    state->run_errno = errno;
    atomic_store_explicit(
        &state->runner_done, 1U, memory_order_release);
}

static void native_cancel_batch_controller_task(void *arg) {
    native_cancel_batch_state_t *state = arg;
    unsigned spins;

    for (spins = 0U; spins < 100000U; spins += 1U) {
        if (atomic_load_explicit(
                &state->runner_started,
                memory_order_acquire) != 0U &&
            native_cancel_batch_is_inflight()) {
            break;
        }
        if (atomic_load_explicit(
                &state->runner_done,
                memory_order_acquire) != 0U) {
            break;
        }
        llam_yield();
    }
    if (!native_cancel_batch_is_inflight()) {
        native_cancel_batch_fail(state, ETIMEDOUT);
    }
    if (state->completion_race) {
        atomic_store_explicit(
            &state->release_peers,
            1U,
            memory_order_release);
        llam_yield();
    }
    if (state->runtime_stop) {
        if (llam_runtime_request_stop() != 0) {
            native_cancel_batch_fail(
                state, errno != 0 ? errno : EIO);
        }
    } else if (
        llam_cancel_token_cancel(state->token) != 0) {
        native_cancel_batch_fail(
            state, errno != 0 ? errno : EIO);
    }
}

static void native_cancel_batch_peer_task(void *arg) {
    native_cancel_peer_context_t *context = arg;
    native_cancel_batch_state_t *state = context->state;
    fixed_pipeline_state_t *lane =
        &state->lanes[context->index];
    size_t offset = 0U;

    while (atomic_load_explicit(
               &state->release_peers,
               memory_order_acquire) == 0U) {
        llam_yield();
    }
    if (leir_test_write_all(
            lane->pair[1],
            lane->payload,
            sizeof(lane->payload)) != 0) {
        native_cancel_batch_fail(
            state, errno != 0 ? errno : EIO);
        return;
    }

    while (offset < sizeof(lane->echoed)) {
        ssize_t received = recv(
            (int)lane->pair[1],
            lane->echoed + offset,
            sizeof(lane->echoed) - offset,
            MSG_DONTWAIT);

        if (received > 0) {
            offset += (size_t)received;
            continue;
        }
        if (received == 0) {
            native_cancel_batch_fail(state, ECONNRESET);
            return;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK &&
            errno != EINTR) {
            native_cancel_batch_fail(state, errno);
            return;
        }
        if (atomic_load_explicit(
                &state->runner_done,
                memory_order_acquire) != 0U &&
            state->run_result != 0) {
            return;
        }
        llam_yield();
    }
    if (memcmp(
            lane->echoed,
            lane->payload,
            sizeof(lane->payload)) != 0) {
        native_cancel_batch_fail(state, EPROTO);
    }
}

static bool native_cancel_mode_is_available(
    leir_native_mode_t mode) {
    llam_node_t *node;
    bool fixed =
        mode == LEIR_NATIVE_MODE_FIXED_LINK ||
        mode == LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP;
    bool skip =
        mode == LEIR_NATIVE_MODE_LINK_CQE_SKIP ||
        mode == LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP;

    if (g_llam_runtime.nodes == NULL ||
        g_llam_runtime.active_nodes == 0U) {
        return false;
    }
    node = &g_llam_runtime.nodes[0];
    return node->ring_ready &&
           node->supports_recv &&
           node->supports_send &&
           (!fixed ||
            (node->supports_native_fixed_files &&
             node->supports_native_fixed_buffers)) &&
           (!skip ||
            (node->linux_ring_features &
             IORING_FEAT_CQE_SKIP) != 0U);
}

static int run_native_cancel_batch_case(
    unsigned width,
    leir_native_mode_t mode,
    bool completion_race,
    bool runtime_stop) {
    native_cancel_batch_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_spawn_opts_t spawn_opts;
    llam_task_t *runner = NULL;
    llam_task_t *controller = NULL;
    llam_task_t
        *peers[LEIR_NATIVE_MAX_BATCH_SEGMENTS];
    bool runtime_started = false;
    bool unavailable = false;
    bool fixed_mode =
        mode == LEIR_NATIVE_MODE_FIXED_LINK ||
        mode == LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP;
    bool skip_mode =
        mode == LEIR_NATIVE_MODE_LINK_CQE_SKIP ||
        mode == LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP;
    int failed = 1;
    unsigned i;

    memset(peers, 0, sizeof(peers));
    if (native_cancel_batch_state_init(
            &state,
            width,
            mode,
            completion_race,
            runtime_stop) != 0) {
        perror("native cancel batch fixture init");
        (void)native_cancel_batch_state_destroy(&state);
        return 1;
    }
    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.forced_yield_every = 1U;
    if (llam_runtime_init(&runtime_opts) != 0) {
        perror("native cancel batch runtime init");
        goto cleanup;
    }
    runtime_started = true;
    if (!native_cancel_mode_is_available(mode)) {
        unavailable = true;
        failed = 0;
        goto shutdown;
    }
    if (llam_spawn_opts_init(
            &spawn_opts,
            LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        perror("native cancel batch spawn opts");
        goto shutdown;
    }
    spawn_opts.cancel_token = state.token;
    runner = llam_spawn(
        native_cancel_batch_runner_task,
        &state,
        &spawn_opts);
    controller = llam_spawn(
        native_cancel_batch_controller_task,
        &state,
        NULL);
    if (completion_race) {
        for (i = 0U; i < width; i += 1U) {
            peers[i] = llam_spawn(
                native_cancel_batch_peer_task,
                &state.peer_contexts[i],
                NULL);
        }
    }
    if (runner == NULL || controller == NULL) {
        native_cancel_batch_fail(
            &state, errno != 0 ? errno : EIO);
        goto shutdown;
    }
    for (i = 0U; i < width && completion_race; i += 1U) {
        if (peers[i] == NULL) {
            native_cancel_batch_fail(
                &state, errno != 0 ? errno : EIO);
            goto shutdown;
        }
    }
    if (llam_run() != 0 ||
        llam_join(runner) != 0 ||
        llam_join(controller) != 0) {
        perror("native cancel batch run");
        goto shutdown;
    }
    runner = NULL;
    controller = NULL;
    for (i = 0U; i < width && completion_race; i += 1U) {
        if (llam_join(peers[i]) != 0) {
            perror("native cancel batch peer join");
            goto shutdown;
        }
        peers[i] = NULL;
    }
    if (atomic_load_explicit(
            &state.failures,
            memory_order_acquire) != 0U ||
        ((!completion_race || runtime_stop) &&
         (state.run_result != -1 ||
          state.run_errno != ECANCELED)) ||
        (completion_race && !runtime_stop &&
         state.run_result != 0 &&
         (state.run_result != -1 ||
          state.run_errno != ECANCELED)) ||
        state.batch_metrics.activations != 1U ||
        state.batch_metrics.segments != width ||
        state.batch_metrics.queue_publications != 1U ||
        state.batch_metrics.task_parks != 1U ||
        state.batch_metrics.terminal_wakes != 1U ||
        state.batch_metrics.operation_sqes !=
            (uint64_t)width * 2U ||
        state.batch_metrics.operation_cqes !=
            (skip_mode
                 ? (uint64_t)width
                 : (uint64_t)width * 2U) ||
        state.batch_metrics.hot_allocations != 0U ||
        atomic_load_explicit(
            &g_llam_runtime.nodes[0].pending_ops,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &g_llam_runtime.active_io_waiters,
            memory_order_acquire) != 0U ||
        !native_cancel_batch_queues_are_empty()) {
        fprintf(
            stderr,
            "native cancel batch mismatch width=%u "
            "mode=%u race=%u result=%d errno=%d "
            "failure=%d\n",
            width,
            (unsigned)mode,
            completion_race ? 1U : 0U,
            state.run_result,
            state.run_errno,
            state.first_errno);
        goto shutdown;
    }
    if ((fixed_mode &&
         (__builtin_popcountll(
              g_llam_runtime.nodes[0].
                  native_fixed_file_bitmap) !=
              (int)width ||
          __builtin_popcountll(
              g_llam_runtime.nodes[0].
                  native_fixed_buffer_bitmap) !=
              (int)width)) ||
        (!fixed_mode &&
         (g_llam_runtime.nodes[0].
              native_fixed_file_bitmap != 0U ||
          g_llam_runtime.nodes[0].
              native_fixed_buffer_bitmap != 0U))) {
        fprintf(
            stderr,
            "native cancel fixed attachment mismatch "
            "width=%u mode=%u\n",
            width,
            (unsigned)mode);
        goto shutdown;
    }
    if (state.run_result == 0) {
        for (i = 0U; i < width; i += 1U) {
            if (state.lanes[i].values_out[
                    FIXED_PIPELINE_RECV_RESULT_SLOT].i64 !=
                    FIXED_PIPELINE_BYTES ||
                state.lanes[i].values_out[
                    FIXED_PIPELINE_SEND_RESULT_SLOT].i64 !=
                    FIXED_PIPELINE_BYTES ||
                memcmp(
                    state.lanes[i].external_buffer,
                    state.lanes[i].payload,
                    FIXED_PIPELINE_BYTES) != 0) {
                fprintf(
                    stderr,
                    "native cancel race success bytes "
                    "mismatch lane=%u\n",
                    i);
                goto shutdown;
            }
        }
    } else if (
        state.batch_metrics.cancel_sqes !=
            (uint64_t)width * 2U ||
        state.batch_metrics.cancel_cqes !=
            state.batch_metrics.cancel_sqes) {
        fprintf(
            stderr,
            "native cancel control retirement mismatch "
            "width=%u sqes=%llu cqes=%llu\n",
            width,
            (unsigned long long)
                state.batch_metrics.cancel_sqes,
            (unsigned long long)
                state.batch_metrics.cancel_cqes);
        goto shutdown;
    }
    failed = 0;

shutdown:
    atomic_store_explicit(
        &state.release_peers, 1U, memory_order_release);
    if (state.token != NULL &&
        atomic_load_explicit(
            &state.runner_done,
            memory_order_acquire) == 0U) {
        (void)llam_cancel_token_cancel(state.token);
    }
    if (runtime_started && failed == 0) {
        if (native_cancel_batch_state_destroy(&state) != 0) {
            perror("native cancel batch destroy");
            failed = 1;
        }
        if (g_llam_runtime.nodes[0].
                native_fixed_file_bitmap != 0U ||
            g_llam_runtime.nodes[0].
                native_fixed_buffer_bitmap != 0U) {
            fprintf(
                stderr,
                "native cancel batch leaked fixed slots\n");
            failed = 1;
        }
    }
    if (runtime_started) {
        llam_runtime_shutdown();
        runtime_started = false;
    }
cleanup:
    if (state.token != NULL ||
        state.initialized_lanes != 0U) {
        if (native_cancel_batch_state_destroy(
                &state) != 0) {
            failed = 1;
        }
    }
    if (unavailable) {
        puts(
            "SKIP: native cancel batch mode unavailable");
    }
    return failed;
}

static int test_native_cancel_batch_ownership(void) {
    static const struct {
        unsigned width;
        leir_native_mode_t mode;
        bool completion_race;
        bool runtime_stop;
    } cases[] = {
        {2U, LEIR_NATIVE_MODE_LINK, false, false},
        {2U, LEIR_NATIVE_MODE_LINK_CQE_SKIP, false, false},
        {8U, LEIR_NATIVE_MODE_LINK_CQE_SKIP, false, false},
        {2U, LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP, true, false},
        {8U, LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP, true, false},
        {8U, LEIR_NATIVE_MODE_FIXED_LINK_CQE_SKIP, false, true},
    };
    size_t i;

    for (i = 0U;
         i < sizeof(cases) / sizeof(cases[0]);
         i += 1U) {
        if (run_native_cancel_batch_case(
                cases[i].width,
                cases[i].mode,
                cases[i].completion_race,
                cases[i].runtime_stop) != 0) {
            return 1;
        }
    }
    return 0;
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
        test_bind_accepts_repeated_fd() != 0 ||
        test_batch_validates_width_and_members() != 0 ||
        test_batch_rejects_unbound_member_and_rolls_back() != 0 ||
        test_valid_batch_without_runtime_releases_instances() != 0) {
        return 1;
    }
#if defined(__linux__)
    if (test_bind_rejects_regular_file_with_enotsock() != 0 ||
        test_bind_rejects_stream_socket_with_eprototype() != 0 ||
        test_bind_rejects_unconnected_seqpacket() != 0 ||
        test_destroy_releases_pinned_fd() != 0 ||
        test_native_runtime_link_and_skip() != 0 ||
        test_native_runtime_pins_bound_fd() != 0 ||
        test_native_runtime_batches_width_two() != 0 ||
        test_fixed_recv_send_pipeline() != 0 ||
        test_fixed_short_read_does_not_copy_prior_activation() != 0 ||
        test_native_cancel_batch_ownership() != 0) {
        return 1;
    }
#endif
#if defined(__linux__)
    if (test_destroyed_instance_cannot_reenter_bind() != 0) {
        return 1;
    }
#endif
    puts("LEIR native segment binding tests passed");
    return 0;
}
