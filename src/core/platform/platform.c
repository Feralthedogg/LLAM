/**
 * @file src/core/platform/platform.c
 * @brief Portable and platform-specific OS integration for workers, affinity, and environment access.
 *
 * @details
 * Platform code is intentionally centralized here so scheduler and I/O modules
 * do not need to carry OS-specific policy. Linux primarily uses affinity,
 * signal, and sysfs helpers; Darwin additionally applies Mach thread policy and
 * QoS hints.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "runtime_internal.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__APPLE__)
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif

static pthread_mutex_t g_llam_process_signal_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_llam_process_signal_refs;
static bool g_llam_process_preempt_installed;
static bool g_llam_process_segv_installed;
static struct sigaction g_llam_process_previous_preempt_action;
static struct sigaction g_llam_process_previous_segv_action;

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_bool g_llam_affinity_test_enabled;
static atomic_int g_llam_affinity_test_supported = ATOMIC_VAR_INIT(-1);
static atomic_int
    g_llam_affinity_test_errors[LLAM_TEST_AFFINITY_OPERATION_COUNT];
static atomic_uint
    g_llam_affinity_test_calls[LLAM_TEST_AFFINITY_OPERATION_COUNT];

void llam_runtime_test_reset_affinity_hooks(void) {
    unsigned operation;

    atomic_store_explicit(&g_llam_affinity_test_enabled, false, memory_order_release);
    atomic_store_explicit(&g_llam_affinity_test_supported, -1, memory_order_release);
    for (operation = 0U;
         operation < LLAM_TEST_AFFINITY_OPERATION_COUNT;
         ++operation) {
        atomic_store_explicit(&g_llam_affinity_test_errors[operation],
                              0,
                              memory_order_release);
        atomic_store_explicit(&g_llam_affinity_test_calls[operation],
                              0U,
                              memory_order_release);
    }
}

void llam_runtime_test_set_affinity_supported(int supported) {
    atomic_store_explicit(&g_llam_affinity_test_supported,
                          supported != 0 ? 1 : 0,
                          memory_order_release);
    atomic_store_explicit(&g_llam_affinity_test_enabled, true, memory_order_release);
}

void llam_runtime_test_set_affinity_error(llam_test_affinity_operation_t operation,
                                          int error_code) {
    if ((unsigned)operation >= LLAM_TEST_AFFINITY_OPERATION_COUNT) {
        return;
    }
    atomic_store_explicit(&g_llam_affinity_test_errors[operation],
                          error_code,
                          memory_order_release);
    atomic_store_explicit(&g_llam_affinity_test_enabled, true, memory_order_release);
}

unsigned llam_runtime_test_affinity_calls(
    llam_test_affinity_operation_t operation) {
    if ((unsigned)operation >= LLAM_TEST_AFFINITY_OPERATION_COUNT) {
        return 0U;
    }
    return atomic_load_explicit(&g_llam_affinity_test_calls[operation],
                                memory_order_acquire);
}

static bool llam_runtime_test_affinity_result(
    llam_test_affinity_operation_t operation,
    int *error_code) {
    if (!atomic_load_explicit(&g_llam_affinity_test_enabled,
                              memory_order_acquire)) {
        return false;
    }
    atomic_fetch_add_explicit(&g_llam_affinity_test_calls[operation],
                              1U,
                              memory_order_relaxed);
    *error_code =
        atomic_load_explicit(&g_llam_affinity_test_errors[operation],
                             memory_order_acquire);
    return true;
}
#endif

/**
 * @brief Return whether this platform has exact scheduler CPU affinity.
 */
bool llam_runtime_affinity_supported(void) {
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (atomic_load_explicit(&g_llam_affinity_test_enabled,
                             memory_order_acquire)) {
        int supported =
            atomic_load_explicit(&g_llam_affinity_test_supported,
                                 memory_order_acquire);

        if (supported >= 0) {
            return supported != 0;
        }
    }
#endif
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

static void llam_runtime_note_affinity_failure(llam_runtime_t *rt) {
    uint_fast64_t failures;

    failures =
        atomic_load_explicit(&rt->affinity_failures, memory_order_acquire);
    while (failures != UINT_FAST64_MAX &&
           !atomic_compare_exchange_weak_explicit(&rt->affinity_failures,
                                                  &failures,
                                                  failures + 1U,
                                                  memory_order_relaxed,
                                                  memory_order_acquire)) {
    }
}

static int llam_runtime_handle_affinity_failure(llam_runtime_t *rt,
                                                int error_code,
                                                int saved_errno) {
    llam_runtime_note_affinity_failure(rt);
    if (rt->resource_plan.affinity_policy == LLAM_RUNTIME_AFFINITY_REQUIRE) {
        errno = error_code;
        return -1;
    }
    errno = saved_errno;
    return 0;
}

static int llam_runtime_capture_driver_affinity_raw(llam_runtime_t *rt) {
#if defined(LLAM_ENABLE_TEST_HOOKS)
    int rc;

    if (llam_runtime_test_affinity_result(LLAM_TEST_AFFINITY_CAPTURE, &rc)) {
        if (rc == 0) {
            memset(&rt->driver_affinity, 0, sizeof(rt->driver_affinity));
        }
        return rc;
    }
#endif
#if defined(__linux__)
    return pthread_getaffinity_np(pthread_self(),
                                  sizeof(rt->driver_affinity),
                                  &rt->driver_affinity);
#else
    (void)rt;
    return ENOTSUP;
#endif
}

static int llam_runtime_apply_worker_affinity_raw(unsigned cpu_id) {
#if defined(LLAM_ENABLE_TEST_HOOKS)
    int rc;

    if (llam_runtime_test_affinity_result(LLAM_TEST_AFFINITY_APPLY, &rc)) {
        return rc;
    }
#endif
#if defined(__linux__)
    cpu_set_t set;

    if (cpu_id >= CPU_SETSIZE) {
        return EINVAL;
    }
    CPU_ZERO(&set);
    CPU_SET(cpu_id, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)cpu_id;
    return ENOTSUP;
#endif
}

static int llam_runtime_restore_driver_affinity_raw(llam_runtime_t *rt) {
#if defined(LLAM_ENABLE_TEST_HOOKS)
    int rc;

    if (llam_runtime_test_affinity_result(LLAM_TEST_AFFINITY_RESTORE, &rc)) {
        return rc;
    }
#endif
#if defined(__linux__)
    return pthread_setaffinity_np(pthread_self(),
                                  sizeof(rt->driver_affinity),
                                  &rt->driver_affinity);
#else
    (void)rt;
    return ENOTSUP;
#endif
}

int llam_runtime_capture_driver_affinity(llam_runtime_t *rt) {
    unsigned policy;
    int saved_errno = errno;
    int rc;

    if (rt == NULL) {
        errno = EINVAL;
        return -1;
    }
    policy = rt->resource_plan.affinity_policy;
    if (policy == LLAM_RUNTIME_AFFINITY_NONE) {
        return 0;
    }

    rt->driver_thread = pthread_self();
    rt->driver_affinity_capture_attempted = true;
    rt->driver_affinity_valid = false;
    if (!llam_runtime_affinity_supported()) {
        return llam_runtime_handle_affinity_failure(rt, ENOTSUP, saved_errno);
    }
    rc = llam_runtime_capture_driver_affinity_raw(rt);
    if (rc != 0) {
        return llam_runtime_handle_affinity_failure(rt, rc, saved_errno);
    }
    rt->driver_affinity_valid = true;
    errno = saved_errno;
    return 0;
}

int llam_runtime_apply_worker_affinity(llam_runtime_t *rt, unsigned cpu_id) {
    unsigned policy;
    int saved_errno = errno;
    int rc;

    if (rt == NULL) {
        errno = EINVAL;
        return -1;
    }
    policy = rt->resource_plan.affinity_policy;
    if (policy == LLAM_RUNTIME_AFFINITY_NONE) {
        return 0;
    }
    if (rt->driver_affinity_capture_attempted &&
        pthread_equal(pthread_self(), rt->driver_thread) &&
        !rt->driver_affinity_valid) {
        return 0;
    }
    if (!llam_runtime_affinity_supported()) {
        return llam_runtime_handle_affinity_failure(rt, ENOTSUP, saved_errno);
    }
    rc = llam_runtime_apply_worker_affinity_raw(cpu_id);
    if (rc != 0) {
        return llam_runtime_handle_affinity_failure(rt, rc, saved_errno);
    }
    errno = saved_errno;
    return 0;
}

int llam_runtime_restore_driver_affinity(llam_runtime_t *rt) {
    unsigned policy;
    int saved_errno = errno;
    int rc;

    if (rt == NULL) {
        errno = EINVAL;
        return -1;
    }
    policy = rt->resource_plan.affinity_policy;
    if (policy == LLAM_RUNTIME_AFFINITY_NONE ||
        !rt->driver_affinity_valid) {
        return 0;
    }
    if (!pthread_equal(pthread_self(), rt->driver_thread)) {
        return llam_runtime_handle_affinity_failure(rt, EPERM, saved_errno);
    }
    rc = llam_runtime_restore_driver_affinity_raw(rt);
    if (rc != 0) {
        return llam_runtime_handle_affinity_failure(rt, rc, saved_errno);
    }
    rt->driver_affinity_valid = false;
    rt->driver_affinity_capture_attempted = false;
    errno = saved_errno;
    return 0;
}

bool llam_runtime_native_thread_enter(llam_runtime_t *rt,
                                      atomic_uint *counter) {
    unsigned live;

    if (rt == NULL || counter == NULL) {
        errno = EINVAL;
        return false;
    }
    live = atomic_load_explicit(counter, memory_order_acquire);
    while (live != UINT_MAX) {
        if (atomic_compare_exchange_weak_explicit(counter,
                                                  &live,
                                                  live + 1U,
                                                  memory_order_release,
                                                  memory_order_acquire)) {
            return true;
        }
    }
    llam_record_fatal(rt, EOVERFLOW);
    errno = EOVERFLOW;
    return false;
}

void llam_runtime_native_thread_exit(llam_runtime_t *rt,
                                     atomic_uint *counter) {
    unsigned live;

    if (rt == NULL || counter == NULL) {
        return;
    }
    live = atomic_load_explicit(counter, memory_order_acquire);
    while (live != 0U) {
        if (atomic_compare_exchange_weak_explicit(counter,
                                                  &live,
                                                  live - 1U,
                                                  memory_order_release,
                                                  memory_order_acquire)) {
            return;
        }
    }
    llam_record_fatal(rt, EINVAL);
}

#if defined(__APPLE__)
/**
 * @brief Check whether Darwin Mach scheduling tuning is enabled.
 *
 * @return @c true unless @c LLAM_DARWIN_MACH_SCHED is explicitly disabled.
 */
static bool llam_darwin_sched_tuning_enabled(void) {
    const char *value = llam_env_get("LLAM_DARWIN_MACH_SCHED");

    return llam_env_flag_value(value, 1U) != 0U;
}

/**
 * @brief Apply a raw Mach thread policy to the current thread.
 *
 * @param flavor Mach policy flavor.
 * @param policy Policy payload pointer.
 * @param count  Number of policy words.
 */
static void llam_darwin_apply_thread_policy(thread_policy_flavor_t flavor,
                                          thread_policy_t policy,
                                          mach_msg_type_number_t count) {
    thread_port_t thread = mach_thread_self();

    if (thread == MACH_PORT_NULL) {
        return;
    }
    (void)thread_policy_set(thread, flavor, policy, count);
    (void)mach_port_deallocate(mach_task_self(), thread);
}

/** @brief Apply Darwin thread precedence policy. */
static void llam_darwin_apply_thread_precedence(integer_t importance) {
    thread_precedence_policy_data_t policy;

    policy.importance = importance;
    llam_darwin_apply_thread_policy(THREAD_PRECEDENCE_POLICY,
                                  (thread_policy_t)&policy,
                                  THREAD_PRECEDENCE_POLICY_COUNT);
}

/** @brief Apply Darwin thread affinity policy. */
static void llam_darwin_apply_thread_affinity(integer_t affinity_tag) {
    thread_affinity_policy_data_t policy;

    policy.affinity_tag = affinity_tag;
    llam_darwin_apply_thread_policy(THREAD_AFFINITY_POLICY,
                                  (thread_policy_t)&policy,
                                  THREAD_AFFINITY_POLICY_COUNT);
}

/** @brief Apply Darwin timeshare/fixed-priority policy. */
static void llam_darwin_apply_thread_timeshare(boolean_t timeshare) {
    thread_extended_policy_data_t policy;

    policy.timeshare = timeshare;
    llam_darwin_apply_thread_policy(THREAD_EXTENDED_POLICY,
                                  (thread_policy_t)&policy,
                                  THREAD_EXTENDED_POLICY_COUNT);
}

/**
 * @brief Apply Darwin QoS, precedence, timeshare, and optional affinity hints.
 *
 * These are hints, not correctness requirements; failures are deliberately
 * ignored so the runtime can run in restricted sandboxes.
 */
static void llam_darwin_tune_current_thread(qos_class_t qos_class,
                                          int relative_priority,
                                          integer_t precedence,
                                          integer_t affinity_tag) {
    if (!llam_darwin_sched_tuning_enabled()) {
        return;
    }

    if (qos_class != QOS_CLASS_UNSPECIFIED) {
        (void)pthread_set_qos_class_self_np(qos_class, relative_priority);
    }
    llam_darwin_apply_thread_timeshare(TRUE);
    llam_darwin_apply_thread_precedence(precedence);
    if (affinity_tag != THREAD_AFFINITY_TAG_NULL) {
        llam_darwin_apply_thread_affinity(affinity_tag);
    }
}
#endif

/**
 * @brief Apply platform scheduling hints for a scheduler or opaque-helper thread.
 *
 * @param shard         Shard associated with the thread.
 * @param opaque_helper Whether the thread is an opaque-block helper.
 */
void llam_tune_scheduler_thread(llam_shard_t *shard, bool opaque_helper) {
#if defined(__APPLE__)
    integer_t affinity_tag = THREAD_AFFINITY_TAG_NULL;

    if (shard != NULL) {
        affinity_tag = (integer_t)(shard->id + 1U);
    }
    llam_darwin_tune_current_thread(QOS_CLASS_USER_INITIATED,
                                  opaque_helper ? -1 : 0,
                                  opaque_helper ? 1 : 2,
                                  affinity_tag);
#else
    (void)shard;
    (void)opaque_helper;
#endif
}

/**
 * @brief Apply platform scheduling hints for an I/O worker thread.
 *
 * @param node I/O node associated with the worker.
 */
void llam_tune_io_worker_thread(llam_node_t *node) {
#if defined(__APPLE__)
    integer_t affinity_tag = THREAD_AFFINITY_TAG_NULL;

    if (node != NULL) {
        affinity_tag = (integer_t)(0x4000 + node->index + 1U);
    }
    llam_darwin_tune_current_thread(QOS_CLASS_USER_INITIATED, -1, 1, affinity_tag);
#else
    (void)node;
#endif
}

/**
 * @brief Apply platform scheduling hints for a blocking-worker thread.
 */
void llam_tune_block_worker_thread(void) {
#if defined(__APPLE__)
    llam_darwin_tune_current_thread(QOS_CLASS_UTILITY, 0, -2, THREAD_AFFINITY_TAG_NULL);
#endif
}

/**
 * @brief Apply platform scheduling hints for the controller/watchdog thread.
 */
void llam_tune_ctrl_thread(void) {
#if defined(__APPLE__)
    llam_darwin_tune_current_thread(QOS_CLASS_UTILITY, -1, -3, THREAD_AFFINITY_TAG_NULL);
#endif
}

/**
 * @brief Check whether an I/O error means "backend unsupported, try fallback".
 *
 * @param error_code Positive errno value.
 *
 * @return @c true when the caller may fall back to direct/blocking I/O.
 */
bool llam_io_capability_error(int error_code) {
    return error_code == EAGAIN || error_code == EINVAL || error_code == EOPNOTSUPP || error_code == ENOSYS;
}

/**
 * @brief Check whether SQPOLL setup failed for a recoverable capability reason.
 *
 * @param error_code Positive errno value.
 *
 * @return @c true when normal io_uring setup should be attempted.
 */
bool llam_io_sqpoll_setup_error(int error_code) {
    return llam_io_capability_error(error_code) || error_code == EPERM || error_code == EACCES;
}

#if LLAM_ARCH_X86_64 || defined(__i386__) || defined(_M_IX86)
/**
 * @brief Issue a CPU pause hint for spin loops.
 */
void llam_pause_cpu(void) {
#if defined(_MSC_VER)
    YieldProcessor();
#else
    __asm__ volatile("pause" ::: "memory");
#endif
}
#else
/**
 * @brief Compiler barrier fallback for platforms without an explicit pause instruction.
 */
void llam_pause_cpu(void) {
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#else
    __asm__ volatile("" ::: "memory");
#endif
}
#endif

/**
 * @brief Install process-wide preemption and fault signal handlers.
 *
 * @details
 * POSIX signal actions are process-global, while LLAM 2.x allows multiple
 * explicit runtimes to coexist.  The first runtime installs the handlers and
 * saves the previous process actions; later runtimes only take a reference.
 * Restoration happens when the last referencing runtime shuts down.
 *
 * @param rt Runtime taking a process-handler reference.
 *
 * @return 0 on success, or -1 with @c errno set by @c sigaction.
 */
int llam_install_process_signal_handlers(llam_runtime_t *rt) {
    struct sigaction action;
    int saved_errno;

    if (rt == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_llam_process_signal_lock);
    if (g_llam_process_signal_refs > 0U) {
        g_llam_process_signal_refs += 1U;
        rt->previous_preempt_action = g_llam_process_previous_preempt_action;
        rt->previous_segv_action = g_llam_process_previous_segv_action;
        rt->preempt_signal_installed = true;
        rt->segv_signal_installed = true;
        pthread_mutex_unlock(&g_llam_process_signal_lock);
        return 0;
    }

    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = llam_preempt_signal_handler;
    action.sa_flags = SA_RESTART | SA_ONSTACK;
    if (sigaction(LLAM_PREEMPT_SIGNAL, &action, &g_llam_process_previous_preempt_action) != 0) {
        saved_errno = errno;
        pthread_mutex_unlock(&g_llam_process_signal_lock);
        errno = saved_errno;
        return -1;
    }
    g_llam_process_preempt_installed = true;

    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = llam_fault_signal_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGSEGV, &action, &g_llam_process_previous_segv_action) != 0) {
        saved_errno = errno;

        (void)sigaction(LLAM_PREEMPT_SIGNAL, &g_llam_process_previous_preempt_action, NULL);
        g_llam_process_preempt_installed = false;
        pthread_mutex_unlock(&g_llam_process_signal_lock);
        errno = saved_errno;
        return -1;
    }
    g_llam_process_segv_installed = true;
    g_llam_process_signal_refs = 1U;
    rt->previous_preempt_action = g_llam_process_previous_preempt_action;
    rt->previous_segv_action = g_llam_process_previous_segv_action;
    rt->preempt_signal_installed = true;
    rt->segv_signal_installed = true;
    pthread_mutex_unlock(&g_llam_process_signal_lock);
    return 0;
}

/**
 * @brief Release this runtime's process-wide signal handler reference.
 *
 * @details
 * The previous process actions are restored only after the last active runtime
 * releases its reference.  Restoring on every runtime destroy would expose
 * peer runtimes to the default @c SIGUSR1 action while their watchdogs may
 * still request preemption.
 *
 * @param rt Runtime containing local handler-reference state.
 */
void llam_restore_process_signal_handlers(llam_runtime_t *rt) {
    if (rt == NULL || (!rt->segv_signal_installed && !rt->preempt_signal_installed)) {
        return;
    }

    pthread_mutex_lock(&g_llam_process_signal_lock);
    if (g_llam_process_signal_refs > 0U) {
        g_llam_process_signal_refs -= 1U;
    }
    rt->segv_signal_installed = false;
    rt->preempt_signal_installed = false;

    if (g_llam_process_signal_refs == 0U) {
        if (g_llam_process_segv_installed) {
            (void)sigaction(SIGSEGV, &g_llam_process_previous_segv_action, NULL);
            g_llam_process_segv_installed = false;
        }
        if (g_llam_process_preempt_installed) {
            (void)sigaction(LLAM_PREEMPT_SIGNAL, &g_llam_process_previous_preempt_action, NULL);
            g_llam_process_preempt_installed = false;
        }
        memset(&g_llam_process_previous_preempt_action, 0, sizeof(g_llam_process_previous_preempt_action));
        memset(&g_llam_process_previous_segv_action, 0, sizeof(g_llam_process_previous_segv_action));
    }
    pthread_mutex_unlock(&g_llam_process_signal_lock);
}

/**
 * @brief Install a per-thread alternate signal stack for guard-page diagnostics.
 *
 * @param shard Shard whose allocated signal stack should be installed.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
int llam_install_thread_signal_stack(llam_shard_t *shard) {
    stack_t stack;

    if (shard->signal_stack == NULL || shard->signal_stack_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    memset(&stack, 0, sizeof(stack));
    stack.ss_sp = shard->signal_stack;
    stack.ss_size = shard->signal_stack_size;
    stack.ss_flags = 0;
    if (sigaltstack(&stack, &shard->previous_sigaltstack) != 0) {
        return -1;
    }

    shard->sigaltstack_installed = true;
    return 0;
}

/**
 * @brief Disable and restore a thread's previous alternate signal stack.
 *
 * @param shard Shard whose alternate stack was installed.
 */
void llam_uninstall_thread_signal_stack(llam_shard_t *shard) {
    stack_t disabled;

    if (!shard->sigaltstack_installed) {
        return;
    }

    memset(&disabled, 0, sizeof(disabled));
    disabled.ss_flags = SS_DISABLE;
    (void)sigaltstack(&disabled, NULL);
    if ((shard->previous_sigaltstack.ss_flags & SS_DISABLE) == 0) {
        (void)sigaltstack(&shard->previous_sigaltstack, NULL);
    }
    shard->sigaltstack_installed = false;
}

/**
 * @brief Enumerate CPUs available to the current process.
 *
 * @param out_cpus Receives an allocated CPU id array on success.
 *
 * @return Number of CPUs in @p out_cpus, or 0 on failure.
 */
unsigned llam_count_allowed_cpus(unsigned **out_cpus) {
#if defined(__linux__)
    cpu_set_t set;
    unsigned count = 0;
    unsigned *cpus;
    unsigned cpu;

    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        return 0;
    }

    count = (unsigned)CPU_COUNT(&set);
    if (count == 0) {
        return 0;
    }

    cpus = calloc(count, sizeof(*cpus));
    if (cpus == NULL) {
        return 0;
    }

    // Store concrete CPU ids rather than assuming a dense 0..N-1 cpuset.
    count = 0;
    for (cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET((int)cpu, &set)) {
            cpus[count] = cpu;
            count += 1;
        }
    }

    *out_cpus = cpus;
    return count;
#elif LLAM_PLATFORM_WINDOWS
    SYSTEM_INFO system_info;
    unsigned count;
    unsigned *cpus;
    unsigned i;

    if (out_cpus == NULL) {
        return 0;
    }
    memset(&system_info, 0, sizeof(system_info));
    GetNativeSystemInfo(&system_info);
    count = system_info.dwNumberOfProcessors != 0U ? (unsigned)system_info.dwNumberOfProcessors : 1U;
    cpus = calloc(count, sizeof(*cpus));
    if (cpus == NULL) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        cpus[i] = i;
    }
    *out_cpus = cpus;
    return count;
#elif LLAM_PLATFORM_DARWIN
    unsigned count;
    unsigned *cpus;
    unsigned i;
    size_t len;

    if (out_cpus == NULL) {
        return 0;
    }

    count = 0U;
    len = sizeof(count);
    if (sysctlbyname("hw.logicalcpu", &count, &len, NULL, 0) != 0 || count == 0U) {
        return 0;
    }
    if (count == 0U) {
        return 0;
    }

    cpus = calloc(count, sizeof(*cpus));
    if (cpus == NULL) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        cpus[i] = i;
    }
    *out_cpus = cpus;
    return count;
#else
    long online;
    unsigned count;
    unsigned *cpus;
    unsigned i;

    if (out_cpus == NULL) {
        return 0;
    }

#ifdef _SC_NPROCESSORS_ONLN
    online = sysconf(_SC_NPROCESSORS_ONLN);
#else
    online = 1;
#endif
    if (online <= 0) {
        online = 1;
    }
    count = (unsigned)online;
    cpus = calloc(count, sizeof(*cpus));
    if (cpus == NULL) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        cpus[i] = i;
    }
    *out_cpus = cpus;
    return count;
#endif
}

/**
 * @brief Detect the kernel NUMA node for a CPU when available.
 *
 * @param cpu_id CPU id to inspect.
 *
 * @return Kernel NUMA node id, or 0 when detection is unavailable.
 */
unsigned llam_detect_cpu_node(unsigned cpu_id) {
#if defined(__linux__)
    unsigned node_id;
    char path[256];

    for (node_id = 0; node_id < 256U; ++node_id) {
        int written = snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/node%u", cpu_id, node_id);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            continue;
        }
        if (access(path, F_OK) == 0) {
            return node_id;
        }
    }

    return 0;
#else
    (void)cpu_id;
    return 0;
#endif
}

/**
 * @brief Find or append a kernel node id in the runtime-local node table.
 *
 * @param node_ids       Node id array.
 * @param node_count     In/out number of populated entries.
 * @param limit          Maximum entries in @p node_ids.
 * @param kernel_node_id Kernel node id to find or append.
 *
 * @return Existing or newly assigned local node index.
 */
unsigned llam_find_or_add_node_id(unsigned *node_ids,
                                       unsigned *node_count,
                                       unsigned limit,
                                       unsigned kernel_node_id) {
    unsigned i;

    for (i = 0; i < *node_count; ++i) {
        if (node_ids[i] == kernel_node_id) {
            return i;
        }
    }

    if (*node_count < limit) {
        node_ids[*node_count] = kernel_node_id;
        *node_count += 1;
        return *node_count - 1;
    }

    return 0;
}
