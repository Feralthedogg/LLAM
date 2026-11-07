/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */
#include "structured_internal.h"

#if defined(LLAM_ENABLE_TEST_HOOKS)
static void (*g_scope_spawn_hook)(unsigned, void *);
static void *g_scope_spawn_hook_argument;

void llam_scope_test_spawn_hook(void (*hook)(unsigned, void *), void *argument) {
    pthread_mutex_lock(&g_llam_structured_lock);
    g_scope_spawn_hook = hook;
    g_scope_spawn_hook_argument = argument;
    pthread_mutex_unlock(&g_llam_structured_lock);
}

static void llam_scope_pause_test(unsigned point) {
    pthread_mutex_lock(&g_llam_structured_lock);
    void (*hook)(unsigned, void *) = g_scope_spawn_hook;
    void *argument = g_scope_spawn_hook_argument;
    pthread_mutex_unlock(&g_llam_structured_lock);
    if (hook != NULL) hook(point, argument);
}
#else
#define llam_scope_pause_test(point) ((void)0)
#endif

static int llam_scope_validate_spawn(const llam_typed_task_spec_t *input, size_t size,
                                      llam_typed_task_spec_t *spec, llam_future_t **out) {
    if (out == NULL || llam_scope_copy_input(spec, sizeof(*spec), input, size) != 0 ||
        spec->entry == NULL || spec->spawn_opts_size > SIZE_MAX ||
        (spec->spawn_opts != NULL && spec->spawn_opts_size == 0U)) {
        errno = EINVAL;
        return -1;
    }
    return llam_contract_validate(&spec->result);
}

static int llam_scope_reserve_spawn(llam_scope_t *scope, llam_future_t *future, llam_value_t *argument) {
    pthread_mutex_lock(&g_llam_structured_lock);
    int error = 0;
    if (scope->report.state != LLAM_SCOPE_OPEN) error = EPIPE;
    else if (scope->report.admitted_tasks == UINT64_MAX || scope->report.pending_tasks == UINT64_MAX ||
             scope->report.active_spawns == UINT64_MAX || scope->object.refs == SIZE_MAX ||
             future->object.refs == SIZE_MAX) error = EOVERFLOW;
    else if (argument != NULL) {
        future->argument = llam_value_claim_locked(argument);
        if (future->argument == NULL) error = errno;
    }
    if (error == 0) {
        scope->report.admitted_tasks += 1U;
        scope->report.pending_tasks += 1U;
        scope->report.active_spawns += 1U;
        scope->object.refs += 1U;
        future->object.refs += 1U; /* Transferred to the task before publication. */
    }
    pthread_mutex_unlock(&g_llam_structured_lock);
    if (error != 0) { errno = error; return -1; }
    return 0;
}

static void llam_scope_rollback_spawn(llam_scope_t *scope, llam_future_t *future,
                                       llam_value_t *argument) {
    llam_value_rollback_claim(argument);
    pthread_mutex_lock(&g_llam_structured_lock);
    future->argument = NULL;
    scope->report.admitted_tasks -= 1U;
    scope->report.pending_tasks -= 1U;
    if (llam_structured_drop_locked(&scope->object) != NULL ||
        llam_structured_drop_locked(&future->object) != NULL) abort();
    pthread_mutex_unlock(&g_llam_structured_lock);
}

static void llam_scope_finish_spawn(llam_scope_t *scope) {
    pthread_mutex_lock(&g_llam_structured_lock);
    scope->report.active_spawns -= 1U;
    bool notify = llam_scope_finish_closing_locked(scope);
    pthread_mutex_unlock(&g_llam_structured_lock);
    if (notify) llam_structured_notify(scope->object.owner, scope->done);
}

static int llam_scope_publish_task(llam_scope_t *scope, llam_future_t *future,
                                    const llam_typed_task_spec_t *spec, llam_future_t **out) {
    llam_scope_pause_test(1U);
    if (llam_scope_reserve_spawn(scope, future, spec->argument) != 0) return -1;
    llam_scope_pause_test(2U);
    llam_value_t *argument = future->argument;
    void *data = argument != NULL ? argument->spec.data : NULL;
    llam_task_t *task = llam_runtime_spawn_scoped(scope->object.owner, future, spec, data);
    int error = errno;
    if (task != NULL) {
        /* The public argument claim also protects an early-returning child:
         * cleanup cannot finish until this call commits the public reference. */
        llam_value_commit_claim(argument);
        *out = llam_structured_handle(&future->object);
    } else llam_scope_rollback_spawn(scope, future, argument);
    llam_scope_finish_spawn(scope);
    if (task == NULL) { errno = error; return -1; }
    return 0;
}

static int llam_scope_spawn_owned(llam_scope_t *scope, const llam_typed_task_spec_t *spec,
                                  llam_future_t **out) {
    llam_runtime_t *runtime;
    if (llam_runtime_begin_object_creation(scope->object.owner, &runtime) != 0) return -1;
    llam_future_t *future = llam_future_prepare(scope, &spec->result);
    int result = future != NULL ? llam_scope_publish_task(scope, future, spec, out) : -1;
    int error = errno;
    if (future != NULL) {
        if (result != 0) {
            pthread_mutex_lock(&g_llam_structured_lock);
            if (llam_structured_consume_locked(&future->object) != 0) abort();
            pthread_mutex_unlock(&g_llam_structured_lock);
        }
        llam_structured_end(&future->object);
    }
    llam_runtime_end_public_op(runtime);
    if (result != 0) errno = error;
    return result;
}

int llam_scope_spawn_ex(llam_scope_t *handle, const llam_typed_task_spec_t *input,
                         size_t size, llam_future_t **out) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_typed_task_spec_t spec;
    if (llam_scope_validate_spawn(input, size, &spec, out) != 0) return -1;
    llam_structured_object_t *object;
    if (llam_structured_begin(handle, LLAM_PUBLIC_HANDLE_FAMILY_SCOPE, &object) != 0) return -1;
    llam_scope_t *scope = (llam_scope_t *)object;
    pthread_mutex_lock(&g_llam_structured_lock);
    bool open = scope->report.state == LLAM_SCOPE_OPEN;
    pthread_mutex_unlock(&g_llam_structured_lock);
    int result = open ? llam_scope_spawn_owned(scope, &spec, out) : -1;
    int error = open ? errno : EPIPE;
    llam_structured_end(object);
    if (result != 0) errno = error;
    return result;
}
