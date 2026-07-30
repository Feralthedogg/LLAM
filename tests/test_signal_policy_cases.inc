/**
 * @file tests/test_signal_policy_cases.inc
 * @brief Process-signal embedding policy regression cases.
 *
 * @details
 * These cases are included by the explicit-runtime regression binary, which
 * supplies init_runtime_opts() and the test_fail*() helpers.
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
#if LLAM_PLATFORM_POSIX
static void signal_policy_host_noop_handler(int signo) {
    (void)signo;
}

static void signal_policy_host_replacement_handler(int signo) {
    (void)signo;
}

static void signal_policy_host_siginfo_exit(int signo,
                                            siginfo_t *info,
                                            void *ucontext) {
    (void)signo;
    (void)info;
    (void)ucontext;
    _exit(42);
}

static void signal_policy_host_classic_exit(int signo) {
    (void)signo;
    _exit(43);
}

static bool signal_policy_classic_action_is(
    const struct sigaction *action,
    void (*handler)(int)) {
    return action != NULL &&
           (action->sa_flags & SA_SIGINFO) == 0 &&
           action->sa_handler == handler;
}

static int signal_policy_wait_for_exit(pid_t pid,
                                       int expected,
                                       const char *message) {
    int status = 0;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return test_fail_errno("host fault-chain waitpid failed");
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected) {
        return test_fail(message);
    }
    return 0;
}
#endif

static int signal_policy_test_full_opt_out(void) {
#if LLAM_PLATFORM_POSIX
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    struct sigaction host;
    struct sigaction saved_preempt;
    struct sigaction saved_fault;
    struct sigaction observed;
    int rc = 1;

    memset(&host, 0, sizeof(host));
    sigemptyset(&host.sa_mask);
    host.sa_handler = signal_policy_host_noop_handler;
    if (sigaction(SIGUSR1, &host, &saved_preempt) != 0 ||
        sigaction(SIGSEGV, &host, &saved_fault) != 0) {
        return test_fail_errno("signal opt-out host action setup failed");
    }
    if (init_runtime_opts(&opts) != 0) {
        rc = test_fail_errno("signal opt-out opts init failed");
        goto cleanup;
    }
    opts.signal_flags = 0U;
    if (llam_runtime_create(
            &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
        rc = test_fail_errno("signal opt-out runtime create failed");
        goto cleanup;
    }
    if (sigaction(SIGUSR1, NULL, &observed) != 0 ||
        !signal_policy_classic_action_is(
            &observed, signal_policy_host_noop_handler) ||
        sigaction(SIGSEGV, NULL, &observed) != 0 ||
        !signal_policy_classic_action_is(
            &observed, signal_policy_host_noop_handler)) {
        rc = test_fail("signal opt-out replaced a host action");
        goto cleanup;
    }
    llam_runtime_destroy(runtime);
    runtime = NULL;
    if (sigaction(SIGSEGV, NULL, &observed) != 0 ||
        !signal_policy_classic_action_is(
            &observed, signal_policy_host_noop_handler)) {
        rc = test_fail("signal opt-out teardown changed a host action");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime);
    (void)sigaction(SIGSEGV, &saved_fault, NULL);
    (void)sigaction(SIGUSR1, &saved_preempt, NULL);
    return rc;
#else
    return 0;
#endif
}

static int signal_policy_test_host_fault_chaining(void) {
#if LLAM_PLATFORM_POSIX
    static const int fault_signals[] = {
        SIGSEGV,
#if LLAM_PLATFORM_DARWIN || LLAM_PLATFORM_BSD
        SIGBUS,
#endif
    };

    for (size_t signal_index = 0U;
         signal_index < sizeof(fault_signals) / sizeof(fault_signals[0]);
         ++signal_index) {
        for (unsigned use_siginfo = 0U; use_siginfo < 2U; ++use_siginfo) {
            pid_t pid = fork();
            int expected = use_siginfo != 0U ? 42 : 43;

            if (pid < 0) {
                return test_fail_errno("host fault-chain fork failed");
            }
            if (pid == 0) {
                llam_runtime_opts_t opts;
                llam_runtime_t *runtime = NULL;
                struct sigaction host;

                (void)alarm(3U);
                memset(&host, 0, sizeof(host));
                sigemptyset(&host.sa_mask);
                if (use_siginfo != 0U) {
                    host.sa_sigaction = signal_policy_host_siginfo_exit;
                    host.sa_flags = SA_SIGINFO;
                } else {
                    host.sa_handler = signal_policy_host_classic_exit;
                }
                if (sigaction(fault_signals[signal_index], &host, NULL) != 0 ||
                    init_runtime_opts(&opts) != 0) {
                    _exit(10);
                }
                opts.signal_flags = LLAM_RUNTIME_SIGNAL_F_GUARD_FAULT;
                if (llam_runtime_create(
                        &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
                    _exit(11);
                }
                (void)raise(fault_signals[signal_index]);
                _exit(12);
            }
            if (signal_policy_wait_for_exit(
                    pid,
                    expected,
                    "non-guard fault did not chain the host action") != 0) {
                return 1;
            }
        }
    }
#endif
    return 0;
}

static int signal_policy_test_host_replacement(void) {
#if LLAM_PLATFORM_POSIX
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    struct sigaction initial;
    struct sigaction replacement;
    struct sigaction saved;
    struct sigaction observed;
    int rc = 1;

    memset(&initial, 0, sizeof(initial));
    sigemptyset(&initial.sa_mask);
    initial.sa_handler = signal_policy_host_noop_handler;
    replacement = initial;
    replacement.sa_handler = signal_policy_host_replacement_handler;
    if (sigaction(SIGSEGV, &initial, &saved) != 0) {
        return test_fail_errno("host replacement setup failed");
    }
    if (init_runtime_opts(&opts) != 0 ||
        llam_runtime_create(
            &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0 ||
        sigaction(SIGSEGV, &replacement, NULL) != 0) {
        rc = test_fail_errno("host replacement runtime setup failed");
        goto cleanup;
    }
    llam_runtime_destroy(runtime);
    runtime = NULL;
    if (sigaction(SIGSEGV, NULL, &observed) != 0 ||
        !signal_policy_classic_action_is(
            &observed, signal_policy_host_replacement_handler)) {
        rc = test_fail("runtime teardown overwrote the host replacement");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime);
    (void)sigaction(SIGSEGV, &saved, NULL);
    return rc;
#else
    return 0;
#endif
}

static int signal_policy_test_configuration_contract(void) {
#if LLAM_PLATFORM_POSIX
    llam_runtime_opts_t opts_a;
    llam_runtime_opts_t opts_b;
    llam_runtime_t *runtime_a = NULL;
    llam_runtime_t *runtime_b = NULL;
    int rc = 1;

    if (init_runtime_opts(&opts_a) != 0 ||
        init_runtime_opts(&opts_b) != 0 ||
        opts_a.signal_flags != LLAM_RUNTIME_SIGNAL_DEFAULT_FLAGS ||
        opts_a.preempt_signal != 0) {
        return test_fail("signal configuration defaults were not initialized");
    }
    opts_a.signal_flags = LLAM_RUNTIME_SIGNAL_F_PREEMPT;
    opts_a.preempt_signal = SIGUSR1;
    opts_b.signal_flags = LLAM_RUNTIME_SIGNAL_F_PREEMPT;
    opts_b.preempt_signal = SIGUSR2;
    if (llam_runtime_create(
            &opts_a, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_a) != 0) {
        rc = test_fail_errno("first signal configuration create failed");
        goto cleanup;
    }
    errno = 0;
    if (llam_runtime_create(
            &opts_b, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_b) != -1 ||
        errno != EBUSY || runtime_b != NULL) {
        rc = test_fail("incompatible signal configuration was accepted");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime_b);
    llam_runtime_destroy(runtime_a);
    return rc;
#else
    return 0;
#endif
}

static int signal_policy_test_validation(void) {
#if LLAM_PLATFORM_POSIX
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("signal validation opts init failed");
    }
    opts.signal_flags = UINT32_MAX;
    errno = 0;
    if (llam_runtime_create(
            &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != -1 ||
        errno != EINVAL || runtime != NULL) {
        return test_fail("unknown signal flags were accepted");
    }
    opts.signal_flags = LLAM_RUNTIME_SIGNAL_F_PREEMPT;
    opts.preempt_signal = SIGSEGV;
    errno = 0;
    if (llam_runtime_create(
            &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != -1 ||
        errno != EINVAL || runtime != NULL) {
        return test_fail("SIGSEGV was accepted for preemption");
    }
    opts.preempt_signal = SIGKILL;
    errno = 0;
    if (llam_runtime_create(
            &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != -1 ||
        errno != EINVAL || runtime != NULL) {
        return test_fail("uncatchable preemption signal was accepted");
    }
#if LLAM_PLATFORM_DARWIN || LLAM_PLATFORM_BSD
    opts.preempt_signal = SIGBUS;
    errno = 0;
    if (llam_runtime_create(
            &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != -1 ||
        errno != EINVAL || runtime != NULL) {
        return test_fail("guard-fault signal was accepted for preemption");
    }
#endif
    opts.signal_flags = LLAM_RUNTIME_SIGNAL_F_GUARD_FAULT;
    if (llam_runtime_create(
            &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
        return test_fail_errno("irrelevant preemption signal was not ignored");
    }
    llam_runtime_destroy(runtime);
#endif
    return 0;
}

static int signal_policy_test_custom_preemption(void) {
#if LLAM_PLATFORM_POSIX
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    struct sigaction host;
    struct sigaction saved;
    struct sigaction observed;
    int rc = 1;

    memset(&host, 0, sizeof(host));
    sigemptyset(&host.sa_mask);
    host.sa_handler = signal_policy_host_noop_handler;
    if (sigaction(SIGUSR2, &host, &saved) != 0) {
        return test_fail_errno("custom preemption host setup failed");
    }
    if (init_runtime_opts(&opts) != 0) {
        goto cleanup;
    }
    opts.signal_flags = LLAM_RUNTIME_SIGNAL_F_PREEMPT;
    opts.preempt_signal = SIGUSR2;
    if (llam_runtime_create(
            &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0 ||
        sigaction(SIGUSR2, NULL, &observed) != 0 ||
        observed.sa_handler != llam_preempt_signal_handler ||
        raise(SIGUSR2) != 0) {
        rc = test_fail_errno("custom preemption signal was not installed");
        goto cleanup;
    }
    llam_runtime_destroy(runtime);
    runtime = NULL;
    if (sigaction(SIGUSR2, NULL, &observed) != 0 ||
        !signal_policy_classic_action_is(
            &observed, signal_policy_host_noop_handler)) {
        rc = test_fail("custom preemption host action was not restored");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime);
    (void)sigaction(SIGUSR2, &saved, NULL);
    return rc;
#else
    return 0;
#endif
}

static int test_signal_policy_embedding_contract(void) {
    return signal_policy_test_full_opt_out() != 0 ||
           signal_policy_test_host_fault_chaining() != 0 ||
           signal_policy_test_host_replacement() != 0 ||
           signal_policy_test_configuration_contract() != 0 ||
           signal_policy_test_validation() != 0 ||
           signal_policy_test_custom_preemption() != 0;
}
#include "test_host_process_cases.inc"
