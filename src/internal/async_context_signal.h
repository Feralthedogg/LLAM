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

#ifndef LLAM_ASYNC_CONTEXT_SIGNAL_H
#define LLAM_ASYNC_CONTEXT_SIGNAL_H

#if LLAM_ASYNC_CONTEXT_SIGNAL
#include "async_context_signal_native.h"

enum {
    LLAM_ASYNC_SIGNAL_IDLE = 0,
    LLAM_ASYNC_SIGNAL_ARMED = 1,
    LLAM_ASYNC_SIGNAL_CLAIMED = 2,
    LLAM_ASYNC_SIGNAL_RESULT = 3,
    LLAM_ASYNC_SIGNAL_STATE_MASK = 3,
    LLAM_ASYNC_SIGNAL_SEQUENCE_SHIFT = 2
};
enum {
    LLAM_ASYNC_SIGNAL_PROBE = 0,
    LLAM_ASYNC_SIGNAL_CAPTURE,
    LLAM_ASYNC_SIGNAL_RESTORE,
    LLAM_ASYNC_SIGNAL_WITNESS,
    LLAM_ASYNC_SIGNAL_COMMAND_COUNT
};

/* Ordinary shared controller constructs this immutable envelope AFTER pins.
 * The stamped claim protects descriptor reuse; it does not create those pins.
 * control/bank remain opaque to the native signal installation layer. */
typedef struct llam_async_signal_request {
    void *control;
    void *bank;
    uint64_t runtime_run;
    uint64_t binding_incarnation;
    uint64_t bank_generation;
    unsigned kind;
} llam_async_signal_request_t;

typedef struct llam_async_signal_result {
    llam_async_signal_request_t request;
    int error;
} llam_async_signal_result_t;

typedef struct llam_async_signal_binding {
    llam_worker_t *worker;
    llam_async_signal_machine_t *probe;
    pthread_t thread;
    int *errno_location;
    sigset_t original_mask;
    int signal_number;
    uint64_t incarnation;
    uint64_t next_sequence;
    _Atomic uint64_t stamp;
    llam_async_signal_request_t request;
    int result_error;
    /* Written only by the native M before G dispatch. The generation release
     * publishes the pair; handler requires the exact admitted dispatch. */
    _Atomic uint64_t scheduler_run;
    _Atomic uint64_t scheduler_mask[LLAM_ASYNC_SIGNAL_MASK_WORDS];
    atomic_int scheduler_errno;
    bool bound;
    bool owns_signal_stack;
#if defined(LLAM_ENABLE_TEST_HOOKS)
    bool refuse_probe;
    atomic_uint cancellations;
    /* Written before RESULT; the serialized controller consumes it before
     * arming another request. Values only, never a retained native frame. */
    struct {
        uint64_t pc;
        uint64_t sp;
        uint64_t pstate;
        uint32_t first_record[2];
    } refusal;
    _Atomic(atomic_uint *) tail_observed[LLAM_ASYNC_SIGNAL_COMMAND_COUNT];
    _Atomic(atomic_uint *) tail_release[LLAM_ASYNC_SIGNAL_COMMAND_COUNT];
#endif
} llam_async_signal_binding_t;

int llam_async_signal_process_acquire(int *signal_number);
void llam_async_signal_process_release(int signal_number);
void llam_async_signal_prepare(llam_async_signal_binding_t *binding, llam_worker_t *worker,
                               void *probe, int signal_number);
void llam_async_signal_save_scheduler(llam_async_signal_binding_t *binding, uint64_t run,
                                      int error, const sigset_t *mask);
int llam_async_signal_arm(llam_async_signal_binding_t *binding,
                         const llam_async_signal_request_t *request);
int llam_async_signal_send(const llam_async_signal_binding_t *binding);
bool llam_async_signal_idle(const llam_async_signal_binding_t *binding);
bool llam_async_signal_cancel(llam_async_signal_binding_t *binding);
bool llam_async_signal_take_result(llam_async_signal_binding_t *binding,
                                  llam_async_signal_result_t *out);
/* Fixed internal signal entry, implemented with the shared bank definitions.
 * It never calls the ordinary controller, locks, queues or reclamation. */
int llam_async_shared_signal_handle(llam_async_signal_binding_t *binding,
                                   ucontext_t *frame, int entry_errno);
extern const llam_async_native_ops_t g_llam_signal_async_ops;
#if defined(LLAM_ENABLE_TEST_HOOKS)
void llam_async_signal_test_fail_probe(void);
#endif
#endif
#endif
