/**
 * @file tests/test_signal_stack_cases.inc
 * @brief Native-thread alternate-signal-stack ownership regression cases.
 *
 * @details
 * These cases are included by the internal shutdown test binary, which
 * supplies fail_errno() and fail_msg().
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
typedef struct signal_stack_pair_state {
    llam_runtime_t runtime;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    unsigned ready;
    bool release;
    void *stack_sp[2];
    int error[2];
} signal_stack_pair_state_t;

typedef struct signal_stack_thread_arg {
    signal_stack_pair_state_t *pair;
    unsigned index;
} signal_stack_thread_arg_t;

static void *signal_stack_thread_main(void *arg) {
    signal_stack_thread_arg_t *thread_arg = arg;
    signal_stack_pair_state_t *pair = thread_arg->pair;
    llam_thread_signal_stack_t scope;
    stack_t observed;

    memset(&scope, 0, sizeof(scope));
    memset(&observed, 0, sizeof(observed));
    if (llam_install_thread_signal_stack(&pair->runtime, &scope) != 0) {
        pair->error[thread_arg->index] = errno != 0 ? errno : EIO;
    } else if (sigaltstack(NULL, &observed) != 0) {
        pair->error[thread_arg->index] = errno != 0 ? errno : EIO;
    } else if (
        !scope.installed ||
        (scope.borrowed && scope.mapping != NULL) ||
        (!scope.borrowed && scope.mapping == NULL) ||
        scope.stack_sp != observed.ss_sp ||
        scope.stack_size != observed.ss_size) {
        fprintf(stderr,
                "signal stack thread diagnostics: borrowed=%d mapping=%p "
                "scope=[%p,%zu] observed=[%p,%zu,%d]\n",
                scope.borrowed ? 1 : 0,
                scope.mapping,
                scope.stack_sp,
                scope.stack_size,
                observed.ss_sp,
                observed.ss_size,
                observed.ss_flags);
        pair->error[thread_arg->index] = EINVAL;
    } else {
        pair->stack_sp[thread_arg->index] = observed.ss_sp;
    }

    pthread_mutex_lock(&pair->lock);
    pair->ready += 1U;
    pthread_cond_broadcast(&pair->cv);
    while (!pair->release) {
        pthread_cond_wait(&pair->cv, &pair->lock);
    }
    pthread_mutex_unlock(&pair->lock);
    llam_uninstall_thread_signal_stack(&scope);
    return NULL;
}

static int exercise_thread_signal_stack_ownership(void) {
    signal_stack_pair_state_t pair;
    signal_stack_thread_arg_t args[2];
    pthread_t threads[2];
    void *host_stack = MAP_FAILED;
    size_t host_stack_size = LLAM_ALTSTACK_BYTES;
    stack_t host;
    stack_t saved;
    stack_t observed;
    llam_thread_signal_stack_t scope;
    bool lock_ready = false;
    bool cv_ready = false;
    unsigned threads_started = 0U;
    int init_rc;
    int rc = 1;

    if (host_stack_size < (size_t)SIGSTKSZ) {
        host_stack_size = (size_t)SIGSTKSZ;
    }
    host_stack = mmap(NULL,
                      host_stack_size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS,
                      -1,
                      0);
    if (host_stack == MAP_FAILED) {
        return fail_errno("host alternate signal stack allocation failed");
    }
    memset(&host, 0, sizeof(host));
    host.ss_sp = host_stack;
    host.ss_size = host_stack_size;
    if (sigaltstack(&host, &saved) != 0) {
        rc = fail_errno("host alternate signal stack install failed");
        goto cleanup;
    }

    memset(&pair, 0, sizeof(pair));
    pair.runtime.fault_signal_installed = true;
    memset(&scope, 0, sizeof(scope));
    if (llam_install_thread_signal_stack(&pair.runtime, &scope) != 0 ||
        !scope.borrowed || scope.mapping != NULL ||
        sigaltstack(NULL, &observed) != 0 ||
        observed.ss_sp != host_stack ||
        observed.ss_size != host_stack_size) {
        rc = fail_msg("valid host alternate signal stack was not borrowed");
        goto restore_host;
    }
    llam_uninstall_thread_signal_stack(&scope);
    if (sigaltstack(NULL, &observed) != 0 ||
        observed.ss_sp != host_stack ||
        observed.ss_size != host_stack_size) {
        rc = fail_msg("borrowed host alternate signal stack was changed");
        goto restore_host;
    }
    if (sigaltstack(&saved, NULL) != 0) {
        rc = fail_errno("host alternate signal stack restore failed");
        goto cleanup;
    }
    memset(&saved, 0, sizeof(saved));
    saved.ss_flags = SS_DISABLE;

    init_rc = pthread_mutex_init(&pair.lock, NULL);
    if (init_rc != 0) {
        errno = init_rc;
        rc = fail_errno("signal stack pair mutex init failed");
        goto cleanup;
    }
    lock_ready = true;
    init_rc = pthread_cond_init(&pair.cv, NULL);
    if (init_rc != 0) {
        errno = init_rc;
        rc = fail_errno("signal stack pair condition init failed");
        goto cleanup;
    }
    cv_ready = true;
    for (unsigned index = 0U; index < 2U; ++index) {
        args[index].pair = &pair;
        args[index].index = index;
        init_rc = pthread_create(
            &threads[index], NULL, signal_stack_thread_main, &args[index]);
        if (init_rc != 0) {
            errno = init_rc;
            rc = fail_errno("signal stack ownership thread create failed");
            goto release_threads;
        }
        threads_started += 1U;
    }
    pthread_mutex_lock(&pair.lock);
    while (pair.ready < 2U) {
        pthread_cond_wait(&pair.cv, &pair.lock);
    }
    if (pair.error[0] != 0 || pair.error[1] != 0 ||
        pair.stack_sp[0] == NULL || pair.stack_sp[1] == NULL ||
        pair.stack_sp[0] == pair.stack_sp[1]) {
        fprintf(stderr,
                "signal stack diagnostics: error=[%d,%d] stack=[%p,%p]\n",
                pair.error[0],
                pair.error[1],
                pair.stack_sp[0],
                pair.stack_sp[1]);
        rc = fail_msg("scheduler threads shared alternate signal-stack state");
    } else {
        rc = 0;
    }
    pair.release = true;
    pthread_cond_broadcast(&pair.cv);
    pthread_mutex_unlock(&pair.lock);
    for (unsigned index = 0U; index < threads_started; ++index) {
        pthread_join(threads[index], NULL);
    }
    threads_started = 0U;
    goto cleanup;

release_threads:
    pthread_mutex_lock(&pair.lock);
    pair.release = true;
    pthread_cond_broadcast(&pair.cv);
    pthread_mutex_unlock(&pair.lock);
    for (unsigned index = 0U; index < threads_started; ++index) {
        pthread_join(threads[index], NULL);
    }
    goto cleanup;

restore_host:
    llam_uninstall_thread_signal_stack(&scope);
    (void)sigaltstack(&saved, NULL);

cleanup:
    if (cv_ready) {
        pthread_cond_destroy(&pair.cv);
    }
    if (lock_ready) {
        pthread_mutex_destroy(&pair.lock);
    }
    if (host_stack != MAP_FAILED) {
        (void)munmap(host_stack, host_stack_size);
    }
    return rc;
}
#else
static int exercise_thread_signal_stack_ownership(void) {
    return 0;
}
#endif
