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

#include "runtime_internal.h"
#include <stdio.h>
#include <inttypes.h>

#if defined(LLAM_ASYNC_SHARED_TEST_LINUX)
#define REAL_TEST_NAME "test_linux_arm64_async_scheduler_real"
#define REAL_TEST_PLATFORM LLAM_ASYNC_CONTEXT_SIGNAL
#else
#define REAL_TEST_NAME "test_darwin_arm64_async_scheduler_real"
#define REAL_TEST_PLATFORM LLAM_PLATFORM_DARWIN
#endif

/* Same ownership/register oracle, separate native-platform test targets. */
#if !LLAM_ASYNC_CONTEXT_SHARED || !REAL_TEST_PLATFORM || !LLAM_ARCH_AARCH64
int main(void) {
    puts(REAL_TEST_NAME ": skipped (new native adapter disabled)");
    return 77;
}
#else
#if LLAM_ASYNC_CONTEXT_SIGNAL
#include "async_context_signal.h"
#else
#include "async_context_darwin.h"
#endif
#include "fixtures/async_darwin_leaf.h"

#define REAL_ITERATIONS UINT64_C(300000000)

enum real_mode {
    REAL_MIXED = 0,
    REAL_CAPTURE_LATE,
    REAL_RESTORE_EARLY_EXIT,
    REAL_READY_DRAIN,
    REAL_TASK_READER,
    REAL_WITNESS_LATE,
    REAL_OFFLOAD,
    REAL_PENDING_SIGNAL,
    REAL_OBSERVED_COOPERATIVE,
    REAL_SIGNAL_CAPTURE_HANDLER,
    REAL_SIGNAL_RESTORE_HANDLER,
    REAL_SIGNAL_WITNESS_ARMED,
    REAL_SIGNAL_CAPTURE_ARMED,
    REAL_READY_DESTROY,
    REAL_WITNESS_READ_REFUSAL
};

static atomic_uint real_signal_count = ATOMIC_VAR_INIT(0U);

static void real_signal_handler(int signal_number) {
    int saved = errno;
    (void)signal_number;
    atomic_fetch_add_explicit(&real_signal_count, 1U, memory_order_relaxed);
    errno = saved;
}

typedef struct real_state real_state_t;
typedef struct real_item {
    real_state_t *state;
    unsigned index;
    uint64_t task_id;
    uint64_t result;
    llam_async_darwin_snapshot_t snapshot;
} real_item_t;

struct real_state {
    llam_runtime_t *runtime;
    bool enabled;
    enum real_mode mode;
    atomic_uint started;
    atomic_uint finished;
    atomic_uint errors;
    atomic_uint tail_observed;
    atomic_uint tail_release;
    atomic_uint peer_release;
    atomic_uint bank_released;
    atomic_uint scan_release;
    atomic_uint cooperative_returns;
    atomic_uint destroy_done;
    pthread_t destroy_host;
    bool destroy_started;
    uint64_t initial_charge;
    real_item_t items[2];
};

static void real_check(real_state_t *state, bool valid) {
    if (!valid) atomic_fetch_add_explicit(&state->errors, 1U, memory_order_relaxed);
}

static void *real_blocking(void *opaque) {
    real_item_t *item = opaque;
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
    real_check(item->state, g_llam_tls_task == NULL);
    (void)nanosleep(&delay, NULL);
    return item;
}

static void real_task(void *opaque) {
    real_item_t *item = opaque;
    real_state_t *state = item->state;
    llam_task_t *task = g_llam_tls_task;
    uintptr_t low = (uintptr_t)task->stack_base;
    uintptr_t high = low + task->stack_size;
    sigset_t mask;
    int error_value = item->index == 0U ? 41 : 92;
    int expected_mask = state->enabled && item->index == 0U ? 1 : 0;

    item->task_id = task->id;
    /* Every NEW task inherits scheduler M's unblocked sentinel, not G1's mask. */
    real_check(state, pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0 &&
                      sigismember(&mask, SIGUSR2) == 0);
    if (expected_mask) sigaddset(&mask, SIGUSR2);
    else sigdelset(&mask, SIGUSR2);
    real_check(state, pthread_sigmask(SIG_SETMASK, &mask, NULL) == 0);
    if (state->mode == REAL_PENDING_SIGNAL && item->index == 0U) {
        /* Native-thread pending signal, not a synthetic per-G pending queue.
         * It is delivered when the return seam installs M's unblocked mask. */
        real_check(state, pthread_kill(pthread_self(), SIGUSR2) == 0);
    }
    atomic_fetch_add_explicit(&state->started, 1U, memory_order_release);
    if ((state->mode == REAL_READY_DRAIN || state->mode == REAL_READY_DESTROY) && item->index == 1U) {
        /* Only this drain adversary pauses an uncertified peer before its
         * leaf. The mixed liveness oracle above has two no-yield leaves. */
        while (atomic_load_explicit(&state->peer_release, memory_order_acquire) == 0U) {
            atomic_signal_fence(memory_order_seq_cst);
        }
    }
    errno = error_value;
    item->result = llam_async_darwin_leaf(REAL_ITERATIONS, &item->snapshot);
    real_check(state, errno == error_value);
    real_check(state, pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0 &&
                      sigismember(&mask, SIGUSR2) == expected_mask);
    real_check(state, item->result == REAL_ITERATIONS * (REAL_ITERATIONS + 1U) / 2U);
    real_check(state, llam_async_darwin_snapshot_valid(&item->snapshot, item->result, low, high));
    if (state->mode == REAL_OFFLOAD) {
        void *out = NULL;
        real_check(state, llam_call_blocking_result(real_blocking, item, &out) == 0 && out == item);
        real_check(state, pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0 &&
                          sigismember(&mask, SIGUSR2) == expected_mask);
    }
    if (state->mode == REAL_OBSERVED_COOPERATIVE || state->mode == REAL_SIGNAL_WITNESS_ARMED) {
        for (unsigned i = 0U; i < 3U; ++i) {
            llam_yield();
            atomic_fetch_add_explicit(&state->cooperative_returns, 1U, memory_order_release);
        }
    }
    atomic_fetch_add_explicit(&state->finished, 1U, memory_order_release);
}

#if LLAM_ASYNC_CONTEXT_SIGNAL
static bool real_signal_pstate_trace(llam_runtime_t *runtime, bool require_capture) {
    const uint64_t nzcv = UINT64_C(0xf0000000);
    const uint64_t ssbs = UINT64_C(0x1000);
    llam_async_shared_event_t events[512];
    size_t count = llam_async_shared_test_events(runtime, events, 512U);
    unsigned captures = 0U, restores = 0U, capture_ssbs = 0U, gate_ssbs = 0U;
    for (size_t i = 0U; i < count; ++i) {
        const llam_async_shared_event_t *event = &events[i];
        if (event->kind != LLAM_ASYNC_SHARED_TEST_CAPTURE &&
            event->kind != LLAM_ASYNC_SHARED_TEST_RESTORE) continue;
        if ((event->pstate_before & ~(nzcv | ssbs)) != 0U ||
            (event->pstate_after & ~(nzcv | ssbs)) != 0U) return false;
        if (event->kind == LLAM_ASYNC_SHARED_TEST_CAPTURE) {
            if (event->pstate_after != (event->pstate_before & ~nzcv)) return false;
            ++captures;
            if ((event->pstate_before & ssbs) != 0U) ++capture_ssbs;
        } else {
            bool matched = false;
            for (size_t j = 0U; j < i; ++j) {
                if (events[j].kind == LLAM_ASYNC_SHARED_TEST_CAPTURE &&
                    events[j].task_id == event->task_id &&
                    events[j].bank_generation == event->bank_generation &&
                    events[j].runtime_run == event->runtime_run) {
                    if (event->pstate_after != events[j].pstate_before) return false;
                    matched = true;
                    break;
                }
            }
            if (!matched) return false;
            ++restores;
            if ((event->pstate_before & ssbs) != 0U) ++gate_ssbs;
        }
    }
    printf("native PSTATE trace prefix: captures=%u restores=%u capture_ssbs1=%u gate_ssbs1=%u exact_writes=pass\n",
           captures, restores, capture_ssbs, gate_ssbs);
    return !require_capture || (captures != 0U && restores != 0U);
}
#endif

static bool real_mixed_trace(const real_state_t *state) {
    llam_async_shared_event_t events[512];
    size_t count = llam_async_shared_test_events(state->runtime, events, 512U);
    size_t first_capture = SIZE_MAX, first_ready = SIZE_MAX;
    size_t first_handoff = SIZE_MAX, second_capture = SIZE_MAX, first_restore = SIZE_MAX;
    unsigned captures[2] = {0U, 0U};
    uint64_t first = state->items[0].task_id, second = state->items[1].task_id;
    for (size_t i = 0U; i < count; ++i) {
        if (events[i].runtime_run != events[0].runtime_run) return false;
        if (events[i].task_id == first) {
            if (events[i].kind == LLAM_ASYNC_SHARED_TEST_CAPTURE) ++captures[0];
            if (events[i].kind == LLAM_ASYNC_SHARED_TEST_CAPTURE && first_capture == SIZE_MAX) first_capture = i;
            if (events[i].kind == LLAM_ASYNC_SHARED_TEST_READY && first_ready == SIZE_MAX) first_ready = i;
            if (events[i].kind == LLAM_ASYNC_SHARED_TEST_CAPTURE_END && first_handoff == SIZE_MAX) first_handoff = i;
            if (events[i].kind == LLAM_ASYNC_SHARED_TEST_RESTORE && first_restore == SIZE_MAX) first_restore = i;
        } else if (events[i].task_id == second &&
                   events[i].kind == LLAM_ASYNC_SHARED_TEST_CAPTURE && second_capture == SIZE_MAX) {
            second_capture = i;
        }
        if (events[i].task_id == second && events[i].kind == LLAM_ASYNC_SHARED_TEST_CAPTURE) ++captures[1];
    }
    if (!(first_capture < first_ready && first_ready < first_handoff &&
          first_handoff < second_capture && second_capture < first_restore &&
          captures[0] >= 2U && captures[1] >= 2U)) {
        fprintf(stderr, "real mixed trace: count=%zu capture=%zu ready=%zu handoff=%zu second=%zu restore=%zu\n",
                count, first_capture, first_ready, first_handoff, second_capture, first_restore);
        return false;
    }
    return true;
}

static void *real_destroy_host(void *opaque) {
    real_state_t *state = opaque;
    llam_runtime_destroy(state->runtime); /* Consuming public operation. */
    atomic_store_explicit(&state->destroy_done, 1U, memory_order_release);
    return NULL;
}

static void *real_monitor(void *opaque) {
    real_state_t *state = opaque;
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    bool matched = false;
#if LLAM_ASYNC_CONTEXT_SIGNAL
    bool late_doorbell = state->mode == REAL_SIGNAL_WITNESS_ARMED ||
                         state->mode == REAL_SIGNAL_CAPTURE_ARMED;
    pthread_t native_target = (pthread_t)0;
    int private_signal = 0;
#endif

    while (llam_now_ns() < deadline) {
        llam_async_shared_checkpoint_t checkpoint;
        llam_async_shared_stats_t stats;
        if (atomic_load_explicit(&state->tail_observed, memory_order_acquire) != 0U &&
            llam_async_shared_test_checkpoint(state->runtime, &checkpoint) &&
            llam_async_shared_stats(state->runtime, &stats)) {
            if (state->mode == REAL_CAPTURE_LATE) {
                matched = checkpoint.capture_ops == 1U && checkpoint.restore_ops == 0U &&
                          checkpoint.pending_phase == LLAM_ASYNC_BANK_GATE;
            } else if (state->mode == REAL_RESTORE_EARLY_EXIT || state->mode == REAL_TASK_READER ||
                       state->mode == REAL_WITNESS_LATE) {
                matched = stats.retire_depth != 0U && stats.binding_ops != 0U &&
                          stats.execution_debt != 0U && stats.charged_bytes != 0U &&
                          stats.bindings_closed == 0U &&
                          atomic_load_explicit(&state->finished, memory_order_acquire) != 0U;
                if (state->mode == REAL_WITNESS_LATE) {
                    matched = matched && checkpoint.pending_phase == LLAM_ASYNC_BANK_OBSERVED;
                }
            } else if (state->mode == REAL_READY_DRAIN || state->mode == REAL_READY_DESTROY) {
                matched = checkpoint.capture_ops == 0U && checkpoint.restore_ops == 0U &&
                          checkpoint.pending_phase == 0U && checkpoint.ready_depth == 1U &&
                          stats.execution_debt == 2U &&
                          atomic_load_explicit(&state->started, memory_order_acquire) == 2U;
                if (matched && state->mode == REAL_READY_DRAIN) {
                    real_check(state, llam_runtime_request_stop_rt(state->runtime) == 0);
                } else if (matched) {
                    state->destroy_started = pthread_create(&state->destroy_host, NULL,
                                                           real_destroy_host, state) == 0;
                    real_check(state, state->destroy_started);
                    /* Both Gs are held by the actual READY/peer roots, so
                     * destroy cannot complete/free RT before this release. */
                    while (state->destroy_started && llam_now_ns() < deadline &&
                           !atomic_load_explicit(&state->runtime->shutdown_requested, memory_order_acquire)) {
                        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
                        (void)nanosleep(&delay, NULL);
                    }
                    real_check(state, state->destroy_started &&
                        atomic_load_explicit(&state->runtime->shutdown_requested, memory_order_acquire) &&
                        atomic_load_explicit(&state->destroy_done, memory_order_acquire) == 0U);
                }
            } else if (state->mode == REAL_OBSERVED_COOPERATIVE) {
                matched = checkpoint.pending_phase == LLAM_ASYNC_BANK_OBSERVED &&
                    atomic_load_explicit(&state->cooperative_returns, memory_order_acquire) >= 2U;
            }
#if LLAM_ASYNC_CONTEXT_SIGNAL
            else if (state->mode == REAL_SIGNAL_CAPTURE_HANDLER ||
                     state->mode == REAL_SIGNAL_RESTORE_HANDLER) {
                matched = stats.execution_debt == 1U && stats.binding_ops == 1U &&
                    stats.bindings_closed + 1U == stats.bindings_created &&
                    state->runtime->shards[0].primary_worker.sigaltstack_installed &&
                    atomic_load_explicit(&state->finished, memory_order_acquire) == 0U &&
                    stats.witnesses == 0U;
                if (state->mode == REAL_SIGNAL_CAPTURE_HANDLER) {
                    matched = matched && checkpoint.capture_ops == 1U &&
                        checkpoint.capture_phase == LLAM_ASYNC_BANK_CAPTURED &&
                        checkpoint.ready_depth == 0U && checkpoint.pending_phase == 0U;
                } else {
                    matched = matched && checkpoint.restore_ops == 1U &&
                        checkpoint.pending_phase == LLAM_ASYNC_BANK_RESTORING;
                }
                if (matched) real_check(state, llam_runtime_request_stop_rt(state->runtime) == 0);
            } else if (late_doorbell) {
                matched = checkpoint.mailbox_state == LLAM_ASYNC_SIGNAL_ARMED &&
                    stats.execution_debt == 1U && stats.binding_ops == 1U && stats.retire_depth != 0U &&
                    stats.bindings_closed + 1U == stats.bindings_created &&
                    atomic_load_explicit(&state->finished, memory_order_acquire) == 1U;
                if (state->mode == REAL_SIGNAL_WITNESS_ARMED) {
                    matched = matched && checkpoint.mailbox_kind == LLAM_ASYNC_SIGNAL_WITNESS &&
                        checkpoint.pending_phase == LLAM_ASYNC_BANK_OBSERVED &&
                        atomic_load_explicit(&state->cooperative_returns, memory_order_acquire) >= 2U;
                } else {
                    matched = matched && checkpoint.mailbox_kind == LLAM_ASYNC_SIGNAL_CAPTURE &&
                        checkpoint.capture_phase == LLAM_ASYNC_BANK_CAPTURING && stats.captures == 0U;
                }
                if (matched) {
                    native_target = state->runtime->shards[0].primary_worker.native_thread;
                    private_signal = state->runtime->async_preempt_signal;
                    real_check(state, llam_runtime_request_stop_rt(state->runtime) == 0);
                }
            }
#endif
            if (matched) break;
        }
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    if (matched && state->mode == REAL_TASK_READER) {
        atomic_store_explicit(&state->tail_release, 1U, memory_order_release);
        matched = false;
        while (llam_now_ns() < deadline) {
            llam_async_shared_stats_t stats;
            if (atomic_load_explicit(&state->bank_released, memory_order_acquire) != 0U &&
                llam_async_shared_stats(state->runtime, &stats) && stats.finalizer_depth != 0U &&
                stats.charged_bytes < state->initial_charge && stats.execution_debt == 1U &&
                stats.binding_ops != 0U && stats.bindings_closed == 0U) {
                matched = true;
                break;
            }
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
            (void)nanosleep(&delay, NULL);
        }
    }
    if (!matched) fprintf(stderr, "real adversary checkpoint timed out: mode=%d\n", state->mode);
    real_check(state, matched);
    atomic_store_explicit(&state->peer_release, 1U, memory_order_release);
    atomic_store_explicit(&state->tail_release, 1U, memory_order_release);
    atomic_store_explicit(&state->scan_release, 1U, memory_order_release);
#if LLAM_ASYNC_CONTEXT_SIGNAL
    if (matched && late_doorbell) {
        bool closed = false;
        while (llam_now_ns() < deadline) {
            llam_async_shared_stats_t stats;
            if (llam_async_shared_stats(state->runtime, &stats) && stats.binding_ops == 0U &&
                stats.execution_debt == 0U && stats.bindings_closed == stats.bindings_created) {
                closed = true;
                break;
            }
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
            (void)nanosleep(&delay, NULL);
        }
        /* Borrowed caller M is still alive until this monitor joins. Its TLS,
         * private altstack and old bank are now detached. A genuine late OS
         * doorbell must not resurrect the canceled request/old G authority. */
        real_check(state, closed && pthread_kill(native_target, private_signal) == 0);
    }
#endif
    return NULL;
}

static int real_run(bool enabled, unsigned repetitions, enum real_mode mode) {
    llam_runtime_opts_t options;
    llam_runtime_t *runtime = NULL;
    sigset_t original, scheduler_mask, after;
    struct sigaction original_action;
    bool action_installed = false;
    llam_async_shared_stats_t stats = {0};
    int result = 1;

    if (pthread_sigmask(SIG_SETMASK, NULL, &original) != 0) return 1;
    scheduler_mask = original;
    sigdelset(&scheduler_mask, SIGUSR2);
    if (pthread_sigmask(SIG_SETMASK, &scheduler_mask, NULL) != 0) return 1;
    if (mode == REAL_PENDING_SIGNAL) {
        struct sigaction action = {0};
        action.sa_handler = real_signal_handler;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGUSR2, &action, &original_action) != 0) goto done;
        action_installed = true;
        atomic_store_explicit(&real_signal_count, 0U, memory_order_relaxed);
    }
    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) goto done;
    options.deterministic = 1U;
    options.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    /* These one-G adversaries need a real request even without peer pressure.
     * AUTO correctly leaves a lone finite G alone; STRICT supplies the event
     * being held, without synthesizing a capture or weakening its predicates. */
    options.preempt_mode = mode == REAL_OBSERVED_COOPERATIVE ||
        (mode >= REAL_SIGNAL_CAPTURE_HANDLER && mode <= REAL_SIGNAL_CAPTURE_ARMED)
            ? LLAM_PREEMPT_STRICT : LLAM_PREEMPT_AUTO;
    options.preempt_quantum_ns = UINT64_C(1000000);
    if (llam_runtime_create_compat(&options, sizeof(options), &runtime) != 0) goto done;
    if (enabled) {
        const llam_async_preempt_range_t range = {
            (uintptr_t)llam_async_darwin_leaf_begin, (uintptr_t)llam_async_darwin_leaf_end
        };
        if (llam_async_shared_enable(runtime, &range, 1U, true) != 0) {
            fprintf(stderr, "real async enable failed: %d\n", errno);
            goto done;
        }
    }
    for (unsigned run = 0U; run < repetitions; ++run) {
        real_state_t state = {.runtime = runtime, .enabled = enabled, .mode = mode};
        llam_task_t *tasks[2] = {NULL, NULL};
        pthread_t monitor;
        int run_result;
        bool held = (mode >= REAL_CAPTURE_LATE && mode <= REAL_WITNESS_LATE) ||
                    mode == REAL_OBSERVED_COOPERATIVE ||
                    (mode >= REAL_SIGNAL_CAPTURE_HANDLER && mode <= REAL_READY_DESTROY);
#if !LLAM_ASYNC_CONTEXT_SIGNAL
        if (mode == REAL_WITNESS_READ_REFUSAL)
            llam_darwin_async_test_fail(LLAM_DARWIN_ASYNC_TEST_FAIL_OBSERVE);
#endif
        unsigned task_count = mode == REAL_OBSERVED_COOPERATIVE ||
            (mode >= REAL_SIGNAL_CAPTURE_HANDLER && mode <= REAL_SIGNAL_CAPTURE_ARMED) ? 1U : 2U;
        for (unsigned i = 0U; i < task_count; ++i) {
            state.items[i].state = &state;
            state.items[i].index = i;
            tasks[i] = llam_runtime_spawn_ex(runtime, real_task, &state.items[i], NULL, 0U);
            if (tasks[i] == NULL) goto done;
        }
        if (mode == REAL_RESTORE_EARLY_EXIT || mode == REAL_TASK_READER || mode == REAL_WITNESS_LATE) {
            if (llam_detach(tasks[0]) != 0) goto done;
            tasks[0] = NULL;
        }
        if (held) {
#if LLAM_ASYNC_CONTEXT_SIGNAL
            if (mode == REAL_SIGNAL_CAPTURE_HANDLER || mode == REAL_SIGNAL_RESTORE_HANDLER) {
                llam_async_shared_test_signal_hold(runtime,
                    mode == REAL_SIGNAL_CAPTURE_HANDLER ? LLAM_ASYNC_SIGNAL_CAPTURE : LLAM_ASYNC_SIGNAL_RESTORE,
                    &state.tail_observed, &state.tail_release);
            } else
#endif
            {
                unsigned point = mode == REAL_CAPTURE_LATE ? LLAM_ASYNC_SHARED_TEST_CAPTURE_RETURN :
                    (mode == REAL_READY_DRAIN || mode == REAL_READY_DESTROY) ? LLAM_ASYNC_SHARED_TEST_READY_HANDOFF :
                    mode == REAL_SIGNAL_CAPTURE_ARMED ? LLAM_ASYNC_SHARED_TEST_SIGNAL_CAPTURE_ARMED :
                    mode == REAL_SIGNAL_WITNESS_ARMED ? LLAM_ASYNC_SHARED_TEST_SIGNAL_WITNESS_ARMED :
                    (mode == REAL_WITNESS_LATE || mode == REAL_OBSERVED_COOPERATIVE)
                        ? LLAM_ASYNC_SHARED_TEST_WITNESS_RETURN : LLAM_ASYNC_SHARED_TEST_RESTORE_RETURN;
                llam_async_shared_test_hold(runtime, point, &state.tail_observed, &state.tail_release);
            }
            if (mode == REAL_TASK_READER) {
                if (!llam_async_shared_stats(runtime, &stats)) goto done;
                state.initial_charge = stats.charged_bytes;
                llam_async_shared_test_hold(runtime, LLAM_ASYNC_SHARED_TEST_TASK_READER,
                    &state.bank_released, &state.scan_release);
            }
            if (pthread_create(&monitor, NULL, real_monitor, &state) != 0) goto done;
        }
        run_result = llam_runtime_run_handle(runtime);
        if (held && pthread_join(monitor, NULL) != 0) goto done;
        if (mode == REAL_READY_DESTROY && state.destroy_started) {
            int join_result = pthread_join(state.destroy_host, NULL);
            runtime = NULL; /* Never inspect consumed RT or task handles. */
            if (join_result != 0 || run_result != 0 || atomic_load(&state.destroy_done) != 1U ||
                atomic_load(&state.started) != 2U || atomic_load(&state.finished) != 2U ||
                atomic_load(&state.errors) != 0U) goto done;
            result = 0;
            goto done;
        }
        if (run_result != 0) {
            fprintf(stderr, "real runtime run failed: %d\n", errno);
            goto done;
        }
        if (enabled && !held && !real_mixed_trace(&state)) goto done;
        if (atomic_load(&state.started) != task_count || atomic_load(&state.finished) != task_count ||
            atomic_load(&state.errors) != 0U) {
            fprintf(stderr, "real task state: started=%u finished=%u errors=%u\n",
                atomic_load(&state.started), atomic_load(&state.finished), atomic_load(&state.errors));
            goto done;
        }
        for (unsigned i = 0U; i < task_count; ++i) {
            if (tasks[i] != NULL && llam_join(tasks[i]) != 0) goto done;
        }
        if (enabled) {
#if !LLAM_ASYNC_CONTEXT_SIGNAL
            if (mode == REAL_WITNESS_READ_REFUSAL && llam_darwin_async_test_pending_failure() != 0U)
                goto done;
#endif
            if (!llam_async_shared_stats(runtime, &stats) ||
                (mode == REAL_SIGNAL_CAPTURE_ARMED ? stats.captures != 0U :
                                                    stats.captures < (held ? 1U : 2U)) ||
                stats.restores != stats.captures || stats.witnesses != stats.restores ||
                stats.execution_debt != 0U || stats.binding_ops != 0U ||
                stats.retire_depth != 0U || stats.finalizer_depth != 0U ||
                stats.charged_bytes != 0U || stats.bindings_created != run + 1U ||
                stats.bindings_closed != run + 1U ||
                stats.scheduler_error_checks < (mode == REAL_SIGNAL_CAPTURE_ARMED ? 1U : 2U)) {
                fprintf(stderr, "real async drain: capture=%" PRIu64 " restore=%" PRIu64
                    " witness=%" PRIu64 " debt=%" PRIu64 " ops=%" PRIu64
                    " retire=%" PRIu64 " final=%" PRIu64 " bytes=%" PRIu64 "\n",
                    stats.captures, stats.restores, stats.witnesses, stats.execution_debt,
                    stats.binding_ops, stats.retire_depth, stats.finalizer_depth, stats.charged_bytes);
                goto done;
            }
#if LLAM_ASYNC_CONTEXT_SIGNAL
            if (!real_signal_pstate_trace(runtime, stats.captures != 0U)) goto done;
            if (mode == REAL_SIGNAL_CAPTURE_ARMED || mode == REAL_SIGNAL_WITNESS_ARMED) {
                llam_async_shared_checkpoint_t checkpoint;
                if (!llam_async_shared_test_checkpoint(runtime, &checkpoint) ||
                    checkpoint.mailbox_state != LLAM_ASYNC_SIGNAL_IDLE ||
                    checkpoint.mailbox_cancellations < run + 1U) goto done;
            }
#endif
        }
        if (pthread_sigmask(SIG_SETMASK, NULL, &after) != 0 ||
            sigismember(&after, SIGUSR2) != 0) goto done;
    }
    if (mode == REAL_PENDING_SIGNAL && atomic_load(&real_signal_count) != 1U) goto done;
    result = 0;
done:
    if (runtime != NULL) llam_runtime_destroy(runtime);
    if (pthread_sigmask(SIG_SETMASK, &original, NULL) != 0) result = 1;
    if (action_installed && sigaction(SIGUSR2, &original_action, NULL) != 0) result = 1;
    if (result == 0) {
        if (mode == REAL_READY_DESTROY) {
            puts(REAL_TEST_NAME ": public destroy drained G-owned READY with idle controller; exact finite results; no consumed-handle reads");
            return 0;
        }
        printf(REAL_TEST_NAME ": %s mode=%u runs=%u capture=%" PRIu64
               " restore=%" PRIu64 " witness=%" PRIu64
               "; exact finite results/GPR/NEON/errno/mask; zero debt/refs/quota; bindings balanced\n",
               enabled ? "automatic native preemption" : "default-OFF baseline", mode, repetitions,
               stats.captures, stats.restores, stats.witnesses);
    }
    return result;
}

static int real_startup_abort(void) {
    llam_runtime_opts_t options;
    llam_runtime_t *runtime = NULL;
    llam_async_shared_stats_t stats;
    real_state_t state = {0};
    const llam_async_preempt_range_t range = {
        (uintptr_t)llam_async_darwin_leaf_begin, (uintptr_t)llam_async_darwin_leaf_end
    };
    int result = 1;

    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) return 1;
    options.deterministic = 1U;
    options.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    options.preempt_mode = LLAM_PREEMPT_AUTO;
    if (llam_runtime_create_compat(&options, sizeof(options), &runtime) != 0) return 1;
    if (llam_async_shared_enable(runtime, &range, 1U, true) != 0) goto done;
    state.runtime = runtime;
    state.items[0].state = &state;
    if (llam_runtime_spawn_ex(runtime, real_task, &state.items[0], NULL, 0U) == NULL) goto done;
#if LLAM_ASYNC_CONTEXT_SIGNAL
    llam_async_signal_test_fail_probe();
#else
    llam_darwin_async_test_fail(LLAM_DARWIN_ASYNC_TEST_FAIL_READ);
#endif
    if (llam_runtime_run_handle(runtime) != -1 || atomic_load(&state.started) != 0U ||
        !llam_async_shared_stats(runtime, &stats) || stats.execution_debt != 0U ||
        stats.binding_ops != 0U ||
        stats.bindings_created != (LLAM_ASYNC_CONTEXT_SIGNAL ? 0U : 1U) ||
        stats.bindings_closed != stats.bindings_created ||
        stats.captures != 0U || stats.restores != 0U || stats.retire_depth != 0U ||
        stats.finalizer_depth != 0U) goto done;
    result = 0;
done:
    llam_runtime_destroy(runtime); /* Also verifies never-started capsule cleanup. */
    if (result == 0) puts("real async: failed startup admitted no G; owned binding/never-started storage cleaned");
    return result;
}

#if !LLAM_ASYNC_CONTEXT_SIGNAL
typedef struct real_probe_state {
    llam_runtime_t *runtime;
    atomic_uint entered;
    int run_result;
} real_probe_state_t;

static void real_probe_task(void *argument) {
    real_probe_state_t *state = argument;
    atomic_fetch_add_explicit(&state->entered, 1U, memory_order_release);
}

static void *real_probe_host(void *argument) {
    real_probe_state_t *state = argument;
    state->run_result = llam_runtime_run_handle(state->runtime);
    return NULL;
}

static int real_probe_publication(void) {
    llam_runtime_opts_t options;
    real_probe_state_t state = {0};
    llam_async_shared_checkpoint_t checkpoint;
    llam_async_shared_stats_t stats;
    atomic_uint observed = ATOMIC_VAR_INIT(0U), release = ATOMIC_VAR_INIT(0U);
    pthread_t host;
    bool host_started = false;
    const llam_async_preempt_range_t range = {
        (uintptr_t)llam_async_darwin_leaf_begin, (uintptr_t)llam_async_darwin_leaf_end
    };
    int result = 1;

    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) return 1;
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_AUTO;
    if (llam_runtime_create_compat(&options, sizeof(options), &state.runtime) != 0) return 1;
    if (llam_async_shared_enable(state.runtime, &range, 1U, true) != 0) goto done;
    llam_task_t *task = llam_runtime_spawn_ex(state.runtime, real_probe_task, &state, NULL, 0U);
    if (task == NULL) goto done;
    llam_async_shared_test_hold(state.runtime, LLAM_ASYNC_SHARED_TEST_BEFORE_PROBE, &observed, &release);
    llam_darwin_async_test_fail(LLAM_DARWIN_ASYNC_TEST_FAIL_READ);
    if (pthread_create(&host, NULL, real_probe_host, &state) != 0) goto done;
    host_started = true;
    uint64_t deadline = llam_now_ns() + UINT64_C(2000000000);
    while (atomic_load_explicit(&observed, memory_order_acquire) == 0U && llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    if (atomic_load_explicit(&observed, memory_order_acquire) == 0U ||
        !llam_async_shared_test_checkpoint(state.runtime, &checkpoint) || checkpoint.probe_phase != 0U)
        goto done;
    /* Native initialization can call into the OS. Keep it ineligible while
     * the controller runs; only the subsequent syscall-free wait may arm. */
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000L};
    (void)nanosleep(&delay, NULL);
    if (llam_darwin_async_test_pending_failure() != LLAM_DARWIN_ASYNC_TEST_FAIL_READ ||
        atomic_load_explicit(&state.entered, memory_order_acquire) != 0U) goto done;
    llam_darwin_async_test_fail(0U);
    atomic_store_explicit(&release, 1U, memory_order_release);
    if (pthread_join(host, NULL) != 0) goto done;
    host_started = false;
    if (state.run_result != 0 || llam_join(task) != 0 ||
        atomic_load_explicit(&state.entered, memory_order_acquire) != 1U ||
        !llam_async_shared_stats(state.runtime, &stats) || stats.bindings_created != 1U ||
        stats.bindings_closed != 1U || stats.binding_ops != 0U || stats.execution_debt != 0U ||
        stats.charged_bytes != 0U) goto done;
    result = 0;
done:
    llam_darwin_async_test_fail(0U);
    atomic_store_explicit(&release, 1U, memory_order_release);
    if (host_started && pthread_join(host, NULL) != 0) result = 1;
    if (state.runtime != NULL) llam_runtime_destroy(state.runtime);
    if (result == 0) puts("real async: native probe arms only after initialization; no premature reads or G dispatch");
    return result;
}
#endif

static int real_configuration(void) {
    llam_runtime_opts_t options;
    llam_runtime_t *runtime = NULL;
    const llam_async_preempt_range_t range = {
        (uintptr_t)llam_async_darwin_leaf_begin, (uintptr_t)llam_async_darwin_leaf_end
    };
    const llam_async_preempt_range_t invalid = {range.begin, range.begin};
    int result = 1;

    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) return 1;
    options.deterministic = 1U;
    options.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    options.preempt_mode = LLAM_PREEMPT_AUTO;
    if (llam_runtime_create_compat(&options, sizeof(options), &runtime) != 0) return 1;
    if (llam_async_shared_enable(runtime, &range, 1U, false) != -1 || errno != EINVAL ||
        llam_async_shared_enable(runtime, &invalid, 1U, true) != -1 || errno != EINVAL) goto done;
    runtime->shards[0].scheduler_ctx.simd_flags = LLAM_CTX_SIMD_F_SKIP_RESTORE;
    if (llam_async_shared_enable(runtime, &range, 1U, true) != -1 || errno != ENOTSUP) goto done;
    runtime->shards[0].scheduler_ctx.simd_flags = 0U;
    if (llam_async_shared_enable(runtime, &range, 1U, true) != 0 ||
        llam_async_shared_enable(runtime, &range, 1U, true) != -1 || errno != ENOTSUP) goto done;
    result = 0;
done:
    runtime->shards[0].scheduler_ctx.simd_flags = 0U;
    llam_runtime_destroy(runtime);
    if (result == 0) puts("real async: unsafe profile, malformed intervals and missing acknowledgement refused");
    return result;
}

static void *real_isolation_host(void *opaque) {
    enum real_mode mode = *(const enum real_mode *)opaque;
    return (void *)(intptr_t)real_run(true, 1U, mode);
}

static int real_isolation(void) {
    const enum real_mode modes[2] = {REAL_READY_DRAIN, REAL_MIXED};
    pthread_t hosts[2];
    void *results[2] = {NULL, NULL};
    unsigned started = 0U;
    int result = 0;

    for (; started < 2U; ++started) {
        if (pthread_create(&hosts[started], NULL, real_isolation_host, (void *)&modes[started]) != 0) break;
    }
    if (started != 2U) result = 1;
    for (unsigned i = 0U; i < started; ++i) {
        if (pthread_join(hosts[i], &results[i]) != 0 || (intptr_t)results[i] != 0) result = 1;
    }
    if (result == 0) puts("real async: concurrent independent runtimes, one stopping and one completing mixed leaves");
    return result;
}


#if LLAM_ASYNC_CONTEXT_SIGNAL
typedef struct real_signal_conflict {
    real_state_t state;
    llam_signal_set_t *set;
    int occupied;
    bool reserve_waiter;
} real_signal_conflict_t;

static void real_signal_conflict_task(void *opaque) {
    real_signal_conflict_t *conflict = opaque;
    if (conflict->reserve_waiter) {
        real_check(&conflict->state,
            llam_signal_set_create_ex(&conflict->occupied, 1U, NULL, 0U, &conflict->set) == 0);
    } else {
        llam_signal_set_t *unexpected = NULL;
        int reserved = conflict->state.runtime->async_preempt_signal;
        sigset_t mask;
        errno = 0;
        int rc = llam_signal_set_create_ex(&reserved, 1U, NULL, 0U, &unexpected);
        int error = errno;
        real_check(&conflict->state, rc == -1 && error == EBUSY && unexpected == NULL);
        if (unexpected != NULL) (void)llam_signal_set_destroy(unexpected);
        real_check(&conflict->state, pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0 &&
            sigismember(&mask, reserved) == 0 && sigismember(&mask, conflict->occupied) == 1);
        real_task(&conflict->state.items[0]);
    }
}

static int real_signal_namespace(void) {
    llam_runtime_opts_t options;
    llam_runtime_t *waiter_runtime = NULL, *runtime = NULL;
    llam_task_t *task;
    real_signal_conflict_t conflict = {.occupied = SIGRTMIN + 2, .reserve_waiter = true};
    llam_async_shared_stats_t stats;
    const llam_async_preempt_range_t range = {
        (uintptr_t)llam_async_darwin_leaf_begin, (uintptr_t)llam_async_darwin_leaf_end
    };
    sigset_t original;
    int result = 1;

    if (pthread_sigmask(SIG_SETMASK, NULL, &original) != 0 ||
        llam_runtime_opts_init(&options, sizeof(options)) != 0) return 1;
    options.deterministic = 1U;
    options.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    options.preempt_mode = LLAM_PREEMPT_STRICT;
    if (llam_runtime_create_compat(&options, sizeof(options), &waiter_runtime) != 0) goto done;
    task = llam_runtime_spawn_ex(waiter_runtime, real_signal_conflict_task, &conflict, NULL, 0U);
    if (task == NULL || llam_runtime_run_handle(waiter_runtime) != 0 || llam_join(task) != 0 ||
        atomic_load(&conflict.state.errors) != 0U || conflict.set == NULL) goto done;
    if (llam_runtime_create_compat(&options, sizeof(options), &runtime) != 0 ||
        llam_async_shared_enable(runtime, &range, 1U, true) != 0 ||
        runtime->async_preempt_signal == conflict.occupied) goto done;
    conflict.reserve_waiter = false;
    conflict.state.runtime = runtime;
    conflict.state.enabled = true;
    conflict.state.mode = REAL_OBSERVED_COOPERATIVE;
    conflict.state.items[0].state = &conflict.state;
    task = llam_runtime_spawn_ex(runtime, real_signal_conflict_task, &conflict, NULL, 0U);
    if (task == NULL || llam_runtime_run_handle(runtime) != 0 || llam_join(task) != 0 ||
        atomic_load(&conflict.state.errors) != 0U || !llam_async_shared_stats(runtime, &stats) ||
        stats.captures < 2U || stats.captures != stats.restores || stats.witnesses != stats.restores ||
        stats.execution_debt != 0U || stats.binding_ops != 0U || stats.charged_bytes != 0U) goto done;
    result = 0;
done:
    if (runtime != NULL) llam_runtime_destroy(runtime);
    if (conflict.set != NULL && llam_signal_set_destroy(conflict.set) != 0) result = 1;
    if (waiter_runtime != NULL) llam_runtime_destroy(waiter_runtime);
    if (pthread_sigmask(SIG_SETMASK, &original, NULL) != 0) result = 1;
    if (result == 0) puts(REAL_TEST_NAME ": existing wait-set not stolen; async-owned number refuses waiter; native preemption remains live");
    return result;
}

static int real_signal_blocked_start(void) {
    llam_runtime_opts_t options;
    llam_runtime_t *runtime = NULL;
    real_state_t state = {.enabled = true};
    llam_async_shared_stats_t stats;
    const llam_async_preempt_range_t range = {
        (uintptr_t)llam_async_darwin_leaf_begin, (uintptr_t)llam_async_darwin_leaf_end
    };
    sigset_t original, reserved, after;
    int result = 1;
    bool mask_saved = false;

    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) return 1;
    options.deterministic = 1U;
    options.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    options.preempt_mode = LLAM_PREEMPT_STRICT;
    if (llam_runtime_create_compat(&options, sizeof(options), &runtime) != 0 ||
        llam_async_shared_enable(runtime, &range, 1U, true) != 0) goto done;
    state.runtime = runtime;
    state.items[0].state = &state;
    if (llam_runtime_spawn_ex(runtime, real_task, &state.items[0], NULL, 0U) == NULL) goto done;
    sigemptyset(&reserved);
    sigaddset(&reserved, runtime->async_preempt_signal);
    if (pthread_sigmask(SIG_BLOCK, &reserved, &original) != 0) goto done;
    mask_saved = true;
    if (llam_runtime_run_handle(runtime) != -1 || errno != EBUSY ||
        atomic_load(&state.started) != 0U ||
        pthread_sigmask(SIG_SETMASK, NULL, &after) != 0 ||
        sigismember(&after, runtime->async_preempt_signal) != 1 ||
        !llam_async_shared_stats(runtime, &stats) || stats.bindings_created != 0U ||
        stats.execution_debt != 0U || stats.binding_ops != 0U) goto done;
    result = 0;
done:
    if (runtime != NULL) llam_runtime_destroy(runtime);
    if (mask_saved && pthread_sigmask(SIG_SETMASK, &original, NULL) != 0) result = 1;
    if (result == 0) puts(REAL_TEST_NAME ": blocked reserved signal rejects startup without admitting G or altering caller mask");
    return result;
}

typedef struct real_signal_owners {
    real_state_t state;
    llam_task_t *task;
    atomic_uint bound;
    atomic_uint release;
    atomic_uint done;
    int peer_result;
    int run_result;
    int run_error;
    int join_error;
} real_signal_owners_t;

static void real_signal_owner_task(void *opaque) {
    real_signal_owners_t *owners = opaque;
    atomic_store_explicit(&owners->bound, 1U, memory_order_release);
    while (atomic_load_explicit(&owners->release, memory_order_acquire) == 0U) llam_yield();
    /* The peer runtime has been publicly destroyed BEFORE this qualified leaf.
     * All of these native captures therefore require the retained handler. */
    real_task(&owners->state.items[0]);
}

static void *real_signal_owner_host(void *opaque) {
    real_signal_owners_t *owners = opaque;
    owners->run_result = llam_runtime_run_handle(owners->state.runtime);
    owners->run_error = owners->run_result != 0 ? errno : 0;
    if (owners->run_result == 0 && llam_join(owners->task) != 0) {
        owners->join_error = errno;
        owners->run_result = -1;
    }
    atomic_store_explicit(&owners->done, 1U, memory_order_release);
    return NULL;
}

static void *real_signal_peer_destroy(void *opaque) {
    real_signal_owners_t *owners = opaque;
    owners->peer_result = real_run(true, 1U, REAL_OBSERVED_COOPERATIVE);
    atomic_store_explicit(&owners->release, 1U, memory_order_release);
    return NULL;
}

static int real_signal_process_owners(void) {
    real_signal_owners_t owners = {0};
    llam_runtime_opts_t options;
    llam_runtime_t *runtime = NULL;
    llam_async_shared_stats_t stats;
    struct sigaction retained, after_peer, after_all;
    pthread_t host, peer;
    bool host_started = false, peer_started = false;
    const llam_async_preempt_range_t range = {
        (uintptr_t)llam_async_darwin_leaf_begin, (uintptr_t)llam_async_darwin_leaf_end
    };
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    int signal_number = 0;
    int result = 1;

    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) return 1;
    options.deterministic = 1U;
    options.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    options.preempt_mode = LLAM_PREEMPT_STRICT;
    if (llam_runtime_create_compat(&options, sizeof(options), &runtime) != 0 ||
        llam_async_shared_enable(runtime, &range, 1U, true) != 0) goto done;
    signal_number = runtime->async_preempt_signal;
    if (sigaction(signal_number, NULL, &retained) != 0) goto done;
    owners.state.runtime = runtime;
    owners.state.enabled = true;
    owners.state.mode = REAL_OBSERVED_COOPERATIVE;
    owners.state.items[0].state = &owners.state;
    owners.task = llam_runtime_spawn_ex(runtime, real_signal_owner_task, &owners, NULL, 0U);
    if (owners.task == NULL || pthread_create(&host, NULL, real_signal_owner_host, &owners) != 0) goto done;
    host_started = true;
    while (atomic_load_explicit(&owners.bound, memory_order_acquire) == 0U &&
           atomic_load_explicit(&owners.done, memory_order_acquire) == 0U && llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    if (atomic_load(&owners.bound) == 0U ||
        pthread_create(&peer, NULL, real_signal_peer_destroy, &owners) != 0) goto done;
    peer_started = true;
    if (pthread_join(peer, NULL) != 0) goto done;
    peer_started = false;
    if (sigaction(signal_number, NULL, &after_peer) != 0 ||
        retained.sa_sigaction != after_peer.sa_sigaction) goto done;
    if (pthread_join(host, NULL) != 0) goto done;
    host_started = false;
    if (owners.peer_result != 0 || owners.run_result != 0 || atomic_load(&owners.state.errors) != 0U ||
        !llam_async_shared_stats(runtime, &stats) || stats.captures < 2U ||
        stats.captures != stats.restores || stats.witnesses != stats.restores ||
        stats.execution_debt != 0U || stats.binding_ops != 0U || stats.charged_bytes != 0U ||
        stats.bindings_created != stats.bindings_closed) {
        if (llam_async_shared_stats(runtime, &stats)) {
            fprintf(stderr, "owner isolation: peer=%d run=%d run_errno=%d join_errno=%d bound=%u started=%u finished=%u errors=%u captures=%llu restores=%llu witnesses=%llu debt=%llu refs=%llu charged=%llu bindings=%llu/%llu\n",
                    owners.peer_result, owners.run_result, owners.run_error, owners.join_error,
                    atomic_load(&owners.bound), atomic_load(&owners.state.started),
                    atomic_load(&owners.state.finished), atomic_load(&owners.state.errors),
                    (unsigned long long)stats.captures, (unsigned long long)stats.restores,
                    (unsigned long long)stats.witnesses, (unsigned long long)stats.execution_debt,
                    (unsigned long long)stats.binding_ops,
                    (unsigned long long)stats.charged_bytes,
                    (unsigned long long)stats.bindings_created, (unsigned long long)stats.bindings_closed);
        }
        goto done;
    }
    if (!real_signal_pstate_trace(runtime, true)) goto done;
    result = 0;
done:
    atomic_store_explicit(&owners.release, 1U, memory_order_release);
    if (peer_started && pthread_join(peer, NULL) != 0) result = 1;
    if (host_started && pthread_join(host, NULL) != 0) result = 1;
    if (runtime != NULL) llam_runtime_destroy(runtime);
    if (signal_number != 0 && (sigaction(signal_number, NULL, &after_all) != 0 ||
                              after_all.sa_handler != SIG_DFL)) result = 1;
    if (result == 0) puts(REAL_TEST_NAME ": peer destroy retained process handler; remaining runtime then completed native captures; last owner restored disposition");
    return result;
}
#endif
int main(int argc, char **argv) {
    if (argc == 1) return real_run(true, 1U, REAL_MIXED);
    if (argc == 2 && strcmp(argv[1], "--disabled") == 0) return real_run(false, 1U, REAL_MIXED);
    if (argc == 2 && strcmp(argv[1], "--repeat") == 0) return real_run(true, 2U, REAL_MIXED);
    if (argc == 2 && strcmp(argv[1], "--capture-late") == 0) return real_run(true, 1U, REAL_CAPTURE_LATE);
    if (argc == 2 && strcmp(argv[1], "--restore-early-exit") == 0) return real_run(true, 1U, REAL_RESTORE_EARLY_EXIT);
    if (argc == 2 && strcmp(argv[1], "--ready-drain") == 0) return real_run(true, 1U, REAL_READY_DRAIN);
    if (argc == 2 && strcmp(argv[1], "--ready-destroy") == 0) return real_run(true, 1U, REAL_READY_DESTROY);
    if (argc == 2 && strcmp(argv[1], "--offload") == 0) return real_run(true, 1U, REAL_OFFLOAD);
    if (argc == 2 && strcmp(argv[1], "--startup-abort") == 0) return real_startup_abort();
    if (argc == 2 && strcmp(argv[1], "--configuration") == 0) return real_configuration();
    if (argc == 2 && strcmp(argv[1], "--isolation") == 0) return real_isolation();
    if (argc == 2 && strcmp(argv[1], "--task-reader") == 0) return real_run(true, 1U, REAL_TASK_READER);
    if (argc == 2 && strcmp(argv[1], "--witness-late") == 0) return real_run(true, 1U, REAL_WITNESS_LATE);
    if (argc == 2 && strcmp(argv[1], "--pending-signal") == 0) return real_run(true, 1U, REAL_PENDING_SIGNAL);
    if (argc == 2 && strcmp(argv[1], "--observed-cooperative") == 0) return real_run(true, 1U, REAL_OBSERVED_COOPERATIVE);
#if !LLAM_ASYNC_CONTEXT_SIGNAL
    if (argc == 2 && strcmp(argv[1], "--probe-publication") == 0) return real_probe_publication();
    if (argc == 2 && strcmp(argv[1], "--witness-read-refusal") == 0)
        return real_run(true, 1U, REAL_WITNESS_READ_REFUSAL);
#endif
#if LLAM_ASYNC_CONTEXT_SIGNAL
    if (argc == 2 && strcmp(argv[1], "--signal-namespace") == 0) return real_signal_namespace();
    if (argc == 2 && strcmp(argv[1], "--signal-blocked-start") == 0) return real_signal_blocked_start();
    if (argc == 2 && strcmp(argv[1], "--signal-owner-isolation") == 0) return real_signal_process_owners();
    if (argc == 2 && strcmp(argv[1], "--capture-handler-tail") == 0) return real_run(true, 1U, REAL_SIGNAL_CAPTURE_HANDLER);
    if (argc == 2 && strcmp(argv[1], "--restore-handler-tail") == 0) return real_run(true, 1U, REAL_SIGNAL_RESTORE_HANDLER);
    if (argc == 2 && strcmp(argv[1], "--witness-armed-return") == 0) return real_run(true, 1U, REAL_SIGNAL_WITNESS_ARMED);
    if (argc == 2 && strcmp(argv[1], "--capture-armed-cancel") == 0) return real_run(true, 2U, REAL_SIGNAL_CAPTURE_ARMED);
#endif
    return 2;
}
#endif
