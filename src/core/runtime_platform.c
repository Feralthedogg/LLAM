/**
 * @file src/core/runtime_platform.c
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

/**
 * @brief Optionally bind the current thread to a CPU.
 *
 * Linux binding is opt-in through @c LLAM_BIND_WORKERS because pinning can hurt
 * short blocking wakeups on some kernels and container schedulers.
 *
 * @param cpu_id CPU id from the runtime's allowed CPU list.
 */
void llam_bind_current_thread_to_cpu(unsigned cpu_id) {
#if defined(__linux__)
    static atomic_int bind_enabled = -1;
    int enabled = atomic_load_explicit(&bind_enabled, memory_order_acquire);
    cpu_set_t set;

    if (enabled < 0) {
        const char *env = llam_env_get("LLAM_BIND_WORKERS");

        /* Linux CPU pinning can stretch short blocking syscall wakeups; keep it opt-in. */
        enabled = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
        atomic_store_explicit(&bind_enabled, enabled, memory_order_release);
    }
    if (enabled == 0) {
        return;
    }
    CPU_ZERO(&set);
    CPU_SET(cpu_id, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)cpu_id;
#endif
}

#if defined(__APPLE__)
/**
 * @brief Check whether Darwin Mach scheduling tuning is enabled.
 *
 * @return @c true unless @c LLAM_DARWIN_MACH_SCHED is explicitly set to 0.
 */
static bool llam_darwin_sched_tuning_enabled(void) {
    const char *value = llam_env_get("LLAM_DARWIN_MACH_SCHED");

    return value == NULL || value[0] == '\0' || strcmp(value, "0") != 0;
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
 * @brief Restore the init thread affinity captured during runtime initialization.
 *
 * @param rt Runtime whose init-thread affinity snapshot should be restored.
 */
void llam_restore_init_thread_affinity(llam_runtime_t *rt) {
#if defined(__linux__)
    if (rt == NULL || !rt->init_thread_affinity_valid) {
        return;
    }
    if (!pthread_equal(pthread_self(), rt->init_thread)) {
        return;
    }
    (void)pthread_setaffinity_np(pthread_self(), sizeof(rt->init_thread_affinity), &rt->init_thread_affinity);
#else
    (void)rt;
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
 * @param rt Runtime storing previous handlers for later restoration.
 *
 * @return 0 on success, or -1 with @c errno set by @c sigaction.
 */
int llam_install_process_signal_handlers(llam_runtime_t *rt) {
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = llam_preempt_signal_handler;
    action.sa_flags = SA_RESTART | SA_ONSTACK;
    if (sigaction(LLAM_PREEMPT_SIGNAL, &action, &rt->previous_preempt_action) != 0) {
        return -1;
    }
    rt->preempt_signal_installed = true;

    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = llam_fault_signal_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGSEGV, &action, &rt->previous_segv_action) != 0) {
        int saved_errno = errno;

        (void)sigaction(LLAM_PREEMPT_SIGNAL, &rt->previous_preempt_action, NULL);
        rt->preempt_signal_installed = false;
        errno = saved_errno;
        return -1;
    }
    rt->segv_signal_installed = true;
    return 0;
}

/**
 * @brief Restore process-wide signal handlers installed by the runtime.
 *
 * @param rt Runtime containing saved handler state.
 */
void llam_restore_process_signal_handlers(llam_runtime_t *rt) {
    if (rt->segv_signal_installed) {
        (void)sigaction(SIGSEGV, &rt->previous_segv_action, NULL);
        rt->segv_signal_installed = false;
    }
    if (rt->preempt_signal_installed) {
        (void)sigaction(LLAM_PREEMPT_SIGNAL, &rt->previous_preempt_action, NULL);
        rt->preempt_signal_installed = false;
    }
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
