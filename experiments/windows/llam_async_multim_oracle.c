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

/* Fixed, small native-Windows protocol tests, not a load/soak benchmark.
 * The positive leaf is the unchanged pinned LLVM/LLD R8 LLPM fixture. */
#include "runtime_internal.h"
#include "windows_async_lifetime.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define MM_ITERATIONS UINT64_C(240000000)
typedef unsigned long long (*mm_loop_fn_t)(unsigned long long);
typedef struct mm_state mm_state_t;
typedef struct mm_g {
    mm_state_t *state;
    llam_task_t *compute;
    llam_task_t *peer;
    uintptr_t compute_key;
    uintptr_t peer_key;
    uint64_t actual;
    atomic_uint done;
    unsigned id;
} mm_g_t;
struct mm_state {
    llam_runtime_t *runtime;
    mm_loop_fn_t loop;
    mm_g_t gs[2];
    atomic_uint gate_observed;
    atomic_uint gate_release;
    atomic_uint overlap_observed;
    atomic_uint entry_count;
    bool stop_mixed;
    bool destroy_mixed;
    HANDLE destroy_thread;
    atomic_uint destroy_done;
    bool migration;
    bool three_owners;
    bool delayed;
    bool repeat;
    bool startup_stop;
    unsigned migration_target;
    atomic_uint first_call_active;
    atomic_uint free_gate_observed;
    atomic_uint free_gate_release;
    atomic_uint free_remote_observed;
    atomic_uint migrated;
    atomic_uint post_capture;
    atomic_uint post_release;
    atomic_uint stale_refused;
    atomic_uint delayed_verified;
    atomic_uint source_save;
    atomic_uint source_release;
    atomic_uint scan_hold;
    atomic_uint scan_release;
    atomic_uint owners_verified;
    uintptr_t source_errno_address;
    uintptr_t destination_errno_address;
    DWORD source_native_tid;
    DWORD destination_native_tid;
    atomic_uint repeat_verified;
    uint64_t trace_run;
    uint64_t expected;
    uint64_t iterations;
};

static void mm_fail(const char *where) {
    fprintf(stderr, "llam-win-multim: %s failed errno=%d\n", where, errno);
    ExitProcess(1U);
}

static DWORD WINAPI mm_deadline(void *opaque) {
    if (WaitForSingleObject((HANDLE)opaque, 12000U) == WAIT_TIMEOUT) {
        fputs("llam-win-multim: bounded subprocess deadline\n", stderr);
        ExitProcess(124U);
    }
    return 0U;
}

static uint64_t mm_reference(uint64_t iterations) {
    uint64_t value = 1U;
    for (uint64_t i = 0U; i < iterations; ++i) {
        value = ((value << 7U) ^ (value >> 3U)) + i;
    }
    return value;
}

static bool mm_trace(mm_state_t *state, bool require_coverage);

static void mm_compute(void *opaque) {
    mm_g_t *g = opaque;
    unsigned max_batches = g->state->repeat ? 4U : 1U;
    atomic_fetch_add_explicit(&g->state->entry_count, 1U, memory_order_relaxed);
    if (g_llam_tls_shard->id != g->id) {
        mm_fail("first native M placement");
    }
    /* The repeat gate needs two real chains on every M. A finite call may
     * finish before that coverage exists; keep the same certified computation
     * alive for bounded additional batches without yielding or fabricating
     * events. The independent reference checks every completed batch. */
    for (unsigned batch = 0U; batch < max_batches; ++batch) {
        g->actual = g->state->loop(g->state->iterations);
        if (g->actual != g->state->expected) {
            mm_fail("finite computation result");
        }
        if (!g->state->repeat || mm_trace(g->state, false)) {
            break;
        }
        if (batch + 1U == max_batches) {
            fputs("llam-win-multim: repeat coverage insufficient within finite batch budget\n", stderr);
            ExitProcess(78U);
        }
    }
    llam_windows_async_lifetime_test_note_task(
        g->state->runtime, LLAM_WINDOWS_ASYNC_TEST_RETURN, 0U);
    atomic_store_explicit(&g->done, 1U, memory_order_release);
}

static void mm_peer(void *opaque) {
    mm_g_t *g = opaque;
    size_t seen = 0U;

    while (atomic_load_explicit(&g->done, memory_order_acquire) == 0U) {
        llam_windows_async_test_event_t events[LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY];
        size_t count = llam_windows_async_lifetime_test_trace(
            g->state->runtime, events, LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY);
        uint64_t generation = 0U;
        for (size_t i = seen; i < count; ++i) {
            if (events[i].kind == LLAM_WINDOWS_ASYNC_TEST_CAPTURE &&
                events[i].task_key == g->compute_key) {
                generation = events[i].bank_generation;
            }
        }
        seen = count;
        if (generation != 0U) {
            if (g_llam_tls_shard->id != g->id) {
                mm_fail("local peer migration");
            }
            llam_windows_async_lifetime_test_note_task(
                g->state->runtime, LLAM_WINDOWS_ASYNC_TEST_PEER, generation);
        }
        llam_yield();
    }
}

static llam_task_t *mm_spawn(mm_state_t *state, unsigned id,
                             llam_task_fn function, void *arg,
                             uintptr_t *key) {
    llam_spawn_opts_t opts;
    llam_task_t *handle;
    llam_task_t *raw;

    if (llam_spawn_opts_init(&opts, sizeof(opts)) != 0) {
        mm_fail("spawn opts");
    }
    opts.flags = LLAM_SPAWN_F_PINNED;
    atomic_store_explicit(&state->runtime->next_spawn_shard, id, memory_order_relaxed);
    handle = llam_runtime_spawn_ex(state->runtime, function, arg, &opts, sizeof(opts));
    raw = handle != NULL ? llam_task_resolve_public_handle(handle) : NULL;
    if (raw == NULL || raw->alloc_owner_shard != id) {
        mm_fail("spawn / allocation owner");
    }
    *key = (uintptr_t)raw;
    llam_task_end_public_op(raw);
    return handle;
}

static DWORD WINAPI mm_destroy(void *opaque) {
    mm_state_t *state = opaque;
    llam_runtime_destroy(state->runtime);
    atomic_store_explicit(&state->destroy_done, 1U, memory_order_release);
    return 0U;
}

static bool mm_other_m_progress(const mm_state_t *state) {
    llam_windows_async_test_event_t events[LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY];
    size_t count = llam_windows_async_lifetime_test_trace(
        state->runtime, events, LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY);
    size_t held_capture = SIZE_MAX;

    for (size_t i = 0U; i < count; ++i) {
        if (events[i].kind == LLAM_WINDOWS_ASYNC_TEST_CAPTURE &&
            events[i].task_key == state->gs[0].compute_key && held_capture == SIZE_MAX) {
            held_capture = i;
        }
        if (events[i].kind == LLAM_WINDOWS_ASYNC_TEST_RESTORE &&
            events[i].task_key == state->gs[0].compute_key) {
            mm_fail("held READY continuation unexpectedly restored");
        }
    }
    if (held_capture == SIZE_MAX) return false;
    for (size_t i = held_capture + 1U; i < count; ++i) {
        if (events[i].kind != LLAM_WINDOWS_ASYNC_TEST_CAPTURE ||
            events[i].task_key != state->gs[1].compute_key) continue;
        bool restored = false;
        for (size_t j = i + 1U; j < count; ++j) {
            if (events[j].task_key != events[i].task_key ||
                events[j].bank_key != events[i].bank_key ||
                events[j].bank_generation != events[i].bank_generation) continue;
            if (events[j].kind == LLAM_WINDOWS_ASYNC_TEST_RESTORE) restored = true;
            if (restored && events[j].kind == LLAM_WINDOWS_ASYNC_TEST_WITNESS &&
                events[j].worker_key != events[held_capture].worker_key &&
                events[j].native_thread_id != events[held_capture].native_thread_id &&
                events[j].bank_key != events[held_capture].bank_key) return true;
        }
    }
    return false;
}

static DWORD WINAPI mm_overlap(void *opaque) {
    mm_state_t *state = opaque;
    uint64_t limit = GetTickCount64() + 5000U;

    while (GetTickCount64() < limit) {
        llam_windows_async_stats_t stats = {0};
        if (atomic_load_explicit(&state->gate_observed, memory_order_acquire) != 0U &&
            llam_windows_async_lifetime_stats(state->runtime, &stats) &&
            (!state->stop_mixed || stats.pending_restore_active != 0U)) {
            llam_task_t *raw = (llam_task_t *)state->gs[0].compute_key;
            llam_shard_t *origin = &state->runtime->shards[0];
            llam_windows_async_capsule_t *capsule;
            bool ready = false;

            pthread_mutex_lock(&origin->opaque_lock);
            pthread_mutex_lock(&origin->lock);
            capsule = raw->async_preempt_task;
            if (capsule == NULL || llam_async_preempt_task_origin_shard(raw) != 0U) {
                mm_fail("captured G origin binding");
            }
            for (unsigned bank = 0U; bank < 2U; ++bank) {
                ready |= atomic_load_explicit(&capsule->banks[bank].phase,
                                               memory_order_acquire) ==
                             LLAM_WINDOWS_ASYNC_BANK_READY &&
                         capsule->banks[bank].external_ops == 0U;
            }
            pthread_mutex_unlock(&origin->lock);
            pthread_mutex_unlock(&origin->opaque_lock);
            if (!ready || !mm_other_m_progress(state)) {
                Sleep(1U);
                continue;
            }
            if (stats.remote_transactions_max != 1U) {
                mm_fail("independent READY ownership / serialized suspension");
            }
            printf("llam-win-multim: READY root has zero capture refs; independent M progressed; ops=%" PRIu64
                   " pending_restore=%" PRIu64 " max_suspend=%" PRIu64 "\n",
                   stats.binding_ops, stats.pending_restore_active,
                   stats.remote_transactions_max);
            if (state->destroy_mixed) {
                /* M0 already holds READY on its scheduler. Hold the controller
                 * only AFTER M1's balanced remote restore, then issue public
                 * destroy while READY root + independent restore remain owed. */
                llam_windows_async_lifetime_test_phase_gate(
                    (llam_task_t *)state->gs[1].compute_key,
                    LLAM_WINDOWS_ASYNC_TEST_AFTER_RESTORE,
                    &state->post_capture, &state->post_release);
                while (atomic_load_explicit(&state->post_capture, memory_order_acquire) == 0U) {
                    if (GetTickCount64() >= limit) { mm_fail("mixed destroy restore hold"); }
                    Sleep(1U);
                }
                if (!llam_windows_async_lifetime_stats(state->runtime, &stats) ||
                    stats.binding_ops != 1U || stats.execution_debt < 2U ||
                    stats.pending_restore_active == 0U) {
                    mm_fail("READY plus RESTORING/deferred-DEAD destroy admission");
                }
                state->destroy_thread = CreateThread(NULL, 0U, mm_destroy, state, 0U, NULL);
                if (state->destroy_thread == NULL) { mm_fail("public destroy caller"); }
                while (!atomic_load_explicit(&state->runtime->shutdown_requested, memory_order_acquire)) {
                    if (GetTickCount64() >= limit) { mm_fail("public destroy publication"); }
                    Sleep(1U);
                }
                printf("llam-win-multim: public destroy ops=%" PRIu64
                       " pending_restore=%" PRIu64 " published before release\n",
                       stats.binding_ops, stats.pending_restore_active);
                /* M0's held scheduler guarantees rt is still alive here.
                 * No runtime/task read follows either release below. */
                atomic_store_explicit(&state->overlap_observed, 1U, memory_order_release);
                atomic_store_explicit(&state->gate_release, 1U, memory_order_release);
                atomic_store_explicit(&state->post_release, 1U, memory_order_release);
                return 0U;
            }
            if (state->stop_mixed && llam_runtime_request_stop_rt(state->runtime) != 0) {
                mm_fail("mixed stop");
            }
            atomic_store_explicit(&state->overlap_observed, 1U, memory_order_release);
            atomic_store_explicit(&state->gate_release, 1U, memory_order_release);
            return 0U;
        }
        Sleep(1U);
    }
    mm_fail("READY continuation plus independent M service (coverage insufficient)");
    return 1U;
}

static bool mm_trace(mm_state_t *state, bool require_coverage) {
    llam_windows_async_test_event_t events[LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY];
    size_t count = llam_windows_async_lifetime_test_trace(
        state->runtime, events, LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY);
    DWORD native_ids[2] = {0U, 0U};

    if (llam_windows_async_lifetime_test_trace_overflow(state->runtime)) {
        fprintf(stderr, "llam-win-multim: trace capacity=%u; refusing incomplete lifecycle evidence\n",
                LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY);
        mm_fail("trace overflow");
    }
    for (unsigned id = 0U; id < 2U; ++id) {
        unsigned expected_id = state->migration
            ? (id == 0U ? state->migration_target : 1U) : id;
        unsigned chains = 0U;
        for (size_t c = 0U; c < count; ++c) {
            const llam_windows_async_test_event_t *capture = &events[c];
            size_t peer = count, restore = count, witness = count;
            if (capture->kind != LLAM_WINDOWS_ASYNC_TEST_CAPTURE ||
                capture->task_key != state->gs[id].compute_key ||
                capture->runtime_run != state->trace_run ||
                (state->migration && capture->shard_id != expected_id)) {
                continue;
            }
            if (capture->shard_id != expected_id || capture->worker_key !=
                    (uintptr_t)&state->runtime->shards[expected_id].primary_worker ||
                capture->native_thread_id == 0U || capture->binding_incarnation == 0U ||
                capture->runtime_run == 0U) {
                mm_fail("capture exact M/run identity");
            }
            native_ids[id] = capture->native_thread_id;
            for (size_t k = c + 1U; k < count; ++k) {
                const llam_windows_async_test_event_t *event = &events[k];
                if (event->kind == LLAM_WINDOWS_ASYNC_TEST_PEER &&
                    event->task_key == state->gs[id].peer_key &&
                    event->shard_id == expected_id && event->bank_generation == capture->bank_generation &&
                    peer == count) {
                    peer = k;
                }
                if (event->task_key != capture->task_key ||
                    event->bank_key != capture->bank_key ||
                    event->bank_generation != capture->bank_generation) {
                    continue;
                }
                if (event->worker_key != capture->worker_key ||
                    event->native_thread_id != capture->native_thread_id ||
                    event->binding_incarnation != capture->binding_incarnation ||
                    event->runtime_run != capture->runtime_run || event->shard_id != expected_id ||
                    event->capture_task_run != capture->capture_task_run ||
                    event->capture_worker_run != capture->capture_worker_run) {
                    mm_fail("wrong-M or stale-generation restore/witness");
                }
                if (event->kind == LLAM_WINDOWS_ASYNC_TEST_RESTORE) {
                    restore = k;
                } else if (event->kind == LLAM_WINDOWS_ASYNC_TEST_WITNESS) {
                    witness = k;
                    if (restore == count || event->restore_task_run != events[restore].restore_task_run ||
                        event->restore_worker_run != events[restore].restore_worker_run ||
                        event->restore_task_run <= capture->capture_task_run ||
                        event->restore_worker_run <= capture->capture_worker_run) {
                        mm_fail("exact restored dispatch pair");
                    }
                }
            }
            if (peer < restore && restore < witness && witness < count) {
                ++chains;
                if (require_coverage) printf("llam-win-multim: M%u tid=%lu bank=%" PRIu64
                       " chain=%zu/%zu/%zu/%zu runs=%" PRIu64 "/%" PRIu64
                       " -> %" PRIu64 "/%" PRIu64 "\n", expected_id,
                       (unsigned long)capture->native_thread_id, capture->bank_generation,
                       c, peer, restore, witness, capture->capture_task_run,
                       capture->capture_worker_run, events[restore].restore_task_run,
                       events[restore].restore_worker_run);
            }
        }
        if (chains < 2U && !state->stop_mixed) {
            if (!require_coverage) return false;
            fprintf(stderr, "llam-win-multim: M%u chains=%u coverage insufficient\n", id, chains);
            ExitProcess(78U);
        }
        if (chains == 0U) {
            mm_fail("per-M chain");
        }
    }
    if (native_ids[0] == native_ids[1]) {
        mm_fail("distinct native M threads");
    }
    return true;
}

static void mm_empty_task(void *opaque) {
    mm_state_t *state = opaque;
    atomic_fetch_add_explicit(&state->entry_count, 1U, memory_order_relaxed);
}

/* Reload native-M TLS in a separate non-inlined function on each side of the
 * cooperative safepoint. Do not keep a TLS address in the G's live locals. */
static __attribute__((noinline)) unsigned mm_current_m(void) {
    return g_llam_tls_shard->id;
}

static void *mm_offload_callback(void *opaque) {
    if (g_llam_tls_task != NULL) { mm_fail("foreign callback ran as managed G"); }
    return opaque;
}

static void mm_migrating_compute(void *opaque) {
    mm_state_t *state = opaque;
    llam_task_t *task = g_llam_tls_task;
    llam_windows_async_capsule_t *capsule = task->async_preempt_task;

    atomic_fetch_add_explicit(&state->entry_count, 1U, memory_order_relaxed);
    if (mm_current_m() != 1U || task->alloc_owner_shard != 0U ||
        task->windows_async_debt_owner != 1U) {
        mm_fail("allocation A / first dispatch B");
    }
    errno = 0;
    llam_shard_t *entry_shard = g_llam_tls_shard;
    llam_worker_t *entry_worker = g_llam_tls_worker;
    llam_processor_owner_record_t before_owner = {0}, after_owner = {0};
    pthread_mutex_lock(&entry_shard->opaque_lock);
    pthread_mutex_lock(&entry_shard->lock);
    if (!llam_processor_owner_snapshot_pair_locked(&entry_shard->processor, &before_owner)) {
        mm_fail("pre-refusal P owner snapshot");
    }
    pthread_mutex_unlock(&entry_shard->lock);
    pthread_mutex_unlock(&entry_shard->opaque_lock);
    if (llam_enter_blocking() != -1 || errno != ENOTSUP ||
        atomic_load_explicit(&task->opaque_blocking_depth, memory_order_acquire) != 0U) {
        mm_fail("enabled Windows must refuse helper AND multi-primary redirect detach");
    }
    pthread_mutex_lock(&entry_shard->opaque_lock);
    pthread_mutex_lock(&entry_shard->lock);
    if (!llam_processor_owner_snapshot_pair_locked(&entry_shard->processor, &after_owner) ||
        before_owner.owner_m != after_owner.owner_m || before_owner.state != after_owner.state ||
        before_owner.worker_incarnation != after_owner.worker_incarnation ||
        before_owner.p_generation != after_owner.p_generation ||
        before_owner.lease_projection_epoch != after_owner.lease_projection_epoch ||
        before_owner.lease_state != after_owner.lease_state ||
        llam_worker_load_active_g(entry_worker) != task || task->state != LLAM_TASK_STATE_RUNNING ||
        atomic_load_explicit(&entry_worker->current_p, memory_order_acquire) != &entry_shard->processor ||
        llam_async_preempt_task_origin_shard(task) != 1U ||
        entry_shard->opaque_redirect_active || entry_shard->opaque_helper_thread_started) {
        mm_fail("foreign refusal changed active G / P ticket / service / redirect state");
    }
    pthread_mutex_unlock(&entry_shard->lock);
    pthread_mutex_unlock(&entry_shard->opaque_lock);
    {
        void *result = NULL;
        if (llam_call_blocking_result(mm_offload_callback, state, &result) != 0 ||
            result != state || mm_current_m() != 1U || llam_async_preempt_task_origin_shard(task) != 1U) {
            mm_fail("callback offload remains supported and origin-affine");
        }
    }
    {
        llam_shard_t *shard = g_llam_tls_shard;
        pthread_mutex_lock(&shard->opaque_lock);
        pthread_mutex_lock(&shard->lock);
        task->flags &= ~LLAM_TASK_FLAG_PINNED;
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
    }
    atomic_store_explicit(&state->first_call_active, 1U, memory_order_release);
    if (state->loop(MM_ITERATIONS) != state->expected) { mm_fail("B finite result"); }
    atomic_store_explicit(&state->first_call_active, 0U, memory_order_release);
    if (state->delayed) {
        llam_shard_t *shard = g_llam_tls_shard;
        pthread_mutex_lock(&shard->opaque_lock);
        pthread_mutex_lock(&shard->lock);
        task->flags &= ~LLAM_TASK_FLAG_PINNED;
        task->flags |= LLAM_TASK_FLAG_NO_PREEMPT;
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
        goto request_cooperative_save;
    }
    for (;;) {
        llam_shard_t *shard = g_llam_tls_shard;
        bool free_banks = true;
        pthread_mutex_lock(&shard->opaque_lock);
        pthread_mutex_lock(&shard->lock);
        for (unsigned i = 0U; i < 2U; ++i) {
            free_banks &= atomic_load_explicit(&capsule->banks[i].phase, memory_order_acquire) ==
                              LLAM_WINDOWS_ASYNC_BANK_FREE && capsule->banks[i].external_ops == 0U;
        }
        if (free_banks) {
            if (llam_async_preempt_task_origin_shard(task) != 1U) {
                mm_fail("FREE banks alone must not release B affinity");
            }
            /* Reserve the next explicit cooperative handoff, not another
             * uncertified-PC capture attempt between qualification and yield. */
            task->flags |= LLAM_TASK_FLAG_NO_PREEMPT;
            task->flags &= ~LLAM_TASK_FLAG_PINNED;
        }
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
        if (free_banks) { break; }
        llam_sleep_ns(1000000U); /* Park remains origin-bound; no release request. */
    }
request_cooperative_save:
    llam_yield_internal();
    if (mm_current_m() != 1U || llam_async_preempt_task_origin_shard(task) != 1U ||
        task->windows_async_internal_yield_depth != 0U) {
        mm_fail("runtime-internal yield must retain its native service epoch");
    }
    llam_thread_errno_store(EDOM);
    state->source_errno_address = (uintptr_t)&errno;
    state->source_native_tid = GetCurrentThreadId();
    SetLastError(0x5A17U);
    llam_yield();
    {
    DWORD resumed_last_error = GetLastError();
    int resumed_errno = llam_thread_errno_load();
    state->destination_errno_address = (uintptr_t)&errno;
    state->destination_native_tid = GetCurrentThreadId();
    if (mm_current_m() != state->migration_target || resumed_errno != EDOM || resumed_last_error != 0x5A17U ||
        state->destination_native_tid == state->source_native_tid ||
        state->destination_errno_address == state->source_errno_address ||
        task->windows_async_debt_owner != 1U || task->alloc_owner_shard != 0U ||
        llam_async_preempt_task_origin_shard(task) != state->migration_target) {
        mm_fail("fresh cooperative A claim / logical errno / LastError / debt");
    }
    if (state->delayed) {
        state->gs[0].actual = state->expected;
        atomic_store_explicit(&state->migrated, 1U, memory_order_release);
        atomic_store_explicit(&state->gs[0].done, 1U, memory_order_release);
        return; /* Immediate DEAD while the old controller still owns a scan pin. */
    }
    }
    {
        llam_shard_t *shard = g_llam_tls_shard;
        pthread_mutex_lock(&shard->opaque_lock);
        pthread_mutex_lock(&shard->lock);
        task->flags |= LLAM_TASK_FLAG_PINNED;
        task->flags &= ~LLAM_TASK_FLAG_NO_PREEMPT;
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
    }
    atomic_store_explicit(&state->migrated, 1U, memory_order_release);
    state->gs[0].actual = state->loop(MM_ITERATIONS);
    atomic_store_explicit(&state->gs[0].done, 1U, memory_order_release);
}

static void mm_migration_peer(void *opaque) {
    mm_g_t *g = opaque;
    size_t seen = 0U;
    while (atomic_load_explicit(&g->state->gs[0].done, memory_order_acquire) == 0U) {
        llam_windows_async_test_event_t events[LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY];
        size_t count = llam_windows_async_lifetime_test_trace(
            g->state->runtime, events, LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY);
        uint64_t generation = 0U;
        for (size_t i = seen; i < count; ++i) {
            if (events[i].kind == LLAM_WINDOWS_ASYNC_TEST_CAPTURE && events[i].shard_id == g->id &&
                events[i].task_key == g->state->gs[0].compute_key) {
                generation = events[i].bank_generation;
            }
        }
        seen = count;
        if (generation != 0U) {
            llam_windows_async_lifetime_test_note_task(g->state->runtime,
                LLAM_WINDOWS_ASYNC_TEST_PEER, generation);
        }
        llam_yield();
    }
}

static DWORD WINAPI mm_migration_observer(void *opaque) {
    mm_state_t *state = opaque;
    llam_task_t *task = (llam_task_t *)state->gs[0].compute_key;
    llam_shard_t *source = &state->runtime->shards[1];
    uint64_t deadline = GetTickCount64() + 7000U;

    while (atomic_load_explicit(&state->free_gate_observed, memory_order_acquire) == 0U) {
        if (GetTickCount64() >= deadline) { mm_fail("automatic direct-witness FREE boundary"); }
        Sleep(1U);
    }
    pthread_mutex_lock(&source->opaque_lock);
    pthread_mutex_lock(&source->lock);
    {
        llam_windows_async_capsule_t *capsule = task->async_preempt_task;
        if (atomic_load_explicit(&state->first_call_active, memory_order_acquire) == 0U ||
            atomic_load_explicit(&capsule->context_kind, memory_order_acquire) != 1U ||
            capsule->banks[0].external_ops != 0U || capsule->banks[1].external_ops != 0U ||
            atomic_load_explicit(&capsule->banks[0].phase, memory_order_acquire) != LLAM_WINDOWS_ASYNC_BANK_FREE ||
            atomic_load_explicit(&capsule->banks[1].phase, memory_order_acquire) != LLAM_WINDOWS_ASYNC_BANK_FREE ||
            llam_async_preempt_task_origin_shard(task) != 1U || capsule->origin_enqueued ||
            (task->flags & LLAM_TASK_FLAG_PINNED) != 0U) {
            mm_fail("unpinned running FREE remote continuation origin exclusion (coverage)");
        }
    }
    pthread_mutex_unlock(&source->lock);
    pthread_mutex_unlock(&source->opaque_lock);
    atomic_store_explicit(&state->free_remote_observed, 1U, memory_order_release);
    llam_windows_async_lifetime_test_phase_gate(task, LLAM_WINDOWS_ASYNC_TEST_COOPERATIVE_RELEASED,
                                               &state->gate_observed, &state->gate_release);
    atomic_store_explicit(&state->free_gate_release, 1U, memory_order_release);
    while (atomic_load_explicit(&state->gate_observed, memory_order_acquire) == 0U) {
        if (GetTickCount64() >= deadline) { mm_fail("qualified source handoff deadline"); }
        Sleep(1U);
    }
    /* Test-only forced placement of an ALREADY qualified, off-stack ordinary
     * READY entry. This does not fake a context save or migrate a remote bank. */
    pthread_mutex_lock(&source->opaque_lock);
    pthread_mutex_lock(&source->lock);
    if (llam_async_preempt_task_origin_shard(task) != UINT_MAX ||
        llam_queue_pop_head(&source->processor.inject_q) != task) {
        mm_fail("one qualified migratable queue entry");
    }
    atomic_store_explicit(&source->processor.inject_depth,
                          source->processor.inject_q.depth, memory_order_release);
    pthread_mutex_unlock(&source->lock);
    pthread_mutex_unlock(&source->opaque_lock);
    llam_windows_async_lifetime_test_phase_gate(task, LLAM_WINDOWS_ASYNC_TEST_READY,
                                               &state->post_capture, &state->post_release);
    llam_reinject_task_on_shard(state->runtime, task, state->migration_target,
                               false, LLAM_TRACE_STATE, LLAM_WAIT_NONE);
    while (atomic_load_explicit(&state->migrated, memory_order_acquire) == 0U) {
        if (GetTickCount64() >= deadline) { mm_fail("A fresh service epoch claim"); }
        Sleep(1U);
    }
    atomic_store_explicit(&state->gate_release, 1U, memory_order_release);
    while (atomic_load_explicit(&state->post_capture, memory_order_acquire) == 0U) {
        if (GetTickCount64() >= deadline) { mm_fail("A own native-frame capture"); }
        Sleep(1U);
    }
    {
        llam_shard_t *destination = &state->runtime->shards[state->migration_target];
        llam_windows_async_capsule_t *capsule;
        llam_windows_async_operation_t stale = {0};
        pthread_mutex_lock(&destination->opaque_lock);
        pthread_mutex_lock(&destination->lock);
        capsule = task->async_preempt_task;
        for (unsigned i = 0U; i < 2U; ++i) {
            if (atomic_load_explicit(&capsule->banks[i].phase, memory_order_acquire) == LLAM_WINDOWS_ASYNC_BANK_READY) {
                stale.bank = &capsule->banks[i];
            }
        }
        if (stale.bank == NULL || stale.bank->capture_worker != &destination->primary_worker) {
            mm_fail("new A bank origin");
        }
        stale.runtime = state->runtime;
        stale.shard = source;
        stale.worker = &source->primary_worker;
        stale.task = task;
        stale.binding_incarnation = atomic_load_explicit(&source->primary_worker.binding_incarnation, memory_order_acquire);
        stale.bank_generation = stale.bank->generation;
        stale.runtime_run = capsule->service_run;
        stale.capture_task_run = stale.bank->capture_task_run;
        stale.capture_worker_run = stale.bank->capture_worker_run;
        if (llam_windows_async_lifetime_op_capture_committed(&stale) ||
            llam_windows_async_lifetime_op_restoring(&stale, stale.capture_task_run + 1U,
                                                     stale.capture_worker_run + 1U) ||
            atomic_load_explicit(&stale.bank->phase, memory_order_acquire) != LLAM_WINDOWS_ASYNC_BANK_READY ||
            task->windows_async_debt_owner != 1U ||
            atomic_load_explicit(&source->windows_async_execution_debt, memory_order_acquire) == 0U) {
            mm_fail("stale B cannot mutate A / immutable B debt charge");
        }
        pthread_mutex_unlock(&destination->lock);
        pthread_mutex_unlock(&destination->opaque_lock);
    }
    atomic_store_explicit(&state->stale_refused, 1U, memory_order_release);
    atomic_store_explicit(&state->post_release, 1U, memory_order_release);
    if (state->three_owners) {
        for (;;) {
            llam_windows_async_stats_t stats = {0};
            if (GetTickCount64() >= deadline) { mm_fail("counted terminal finalizer root"); }
            if (atomic_load_explicit(&task->reclaim_ready, memory_order_acquire) != 0U &&
                llam_runtime_live_tasks(state->runtime) == 0U &&
                llam_windows_async_lifetime_stats(state->runtime, &stats) &&
                stats.execution_debt == 0U && stats.finalizer_count == 1U) {
                break;
            }
            Sleep(1U);
        }
        if (task->alloc_owner_shard != 0U || task->windows_async_debt_owner != 1U ||
            task->windows_async_finalizer_owner != 2U ||
            atomic_load_explicit(&state->runtime->shards[2].windows_async_finalizer_count,
                                 memory_order_acquire) != 1U) {
            mm_fail("three distinct allocation / debt / finalizer owners");
        }
        /* An idle M0 and discharged M1 must remain genuine service owners
         * while M2's exclusive finalizer root waits on this one test scan pin. */
        Sleep(3U);
        for (unsigned id = 0U; id < 3U; ++id) {
            llam_worker_t *worker = &state->runtime->shards[id].primary_worker;
            if (!llam_worker_binding_live(worker, atomic_load_explicit(
                    &worker->binding_incarnation, memory_order_acquire))) {
                mm_fail("premature local-empty unbind before global finalizer drain");
            }
        }
        puts("llam-win-multim: allocation=0 immutable_debt=1 terminal_finalizer=2; execution=0 finalizer=1 all-three-Ms-live");
        atomic_store_explicit(&state->owners_verified, 1U, memory_order_release);
        (void)llam_task_scan_ref_release(state->runtime, task);
        /* No raw task/capsule access after the test pin is released. */
    }
    return 0U;
}

static DWORD WINAPI mm_delayed_observer(void *opaque) {
    mm_state_t *state = opaque;
    llam_task_t *task = (llam_task_t *)state->gs[0].compute_key;
    llam_shard_t *source = &state->runtime->shards[1];
    uint64_t limit = GetTickCount64() + 7000U;
    while (atomic_load_explicit(&state->gate_observed, memory_order_acquire) == 0U) {
        if (GetTickCount64() >= limit) { mm_fail("old exact witness / op finish hold"); }
        Sleep(1U);
    }
    llam_windows_async_lifetime_test_phase_gate(task, LLAM_WINDOWS_ASYNC_TEST_COOPERATIVE_SAVED,
                                               &state->source_save, &state->source_release);
    while (atomic_load_explicit(&state->source_save, memory_order_acquire) == 0U) {
        if (GetTickCount64() >= limit) { mm_fail("fresh save held behind old bank ownership"); }
        Sleep(1U);
    }
    pthread_mutex_lock(&source->opaque_lock);
    pthread_mutex_lock(&source->lock);
    {
        llam_windows_async_capsule_t *capsule = task->async_preempt_task;
        bool owned = false;
        for (unsigned i = 0U; i < 2U; ++i) {
            owned |= capsule->banks[i].external_ops == 1U;
        }
        if (!owned || !capsule->source_return_complete || !capsule->origin_enqueued ||
            capsule->yield_save_run != atomic_load_explicit(&task->run_generation, memory_order_acquire) ||
            llam_async_preempt_task_origin_shard(task) != 1U ||
            source->processor.inject_q.depth != 0U) {
            mm_fail("no migratable publication while old bank remains owned");
        }
    }
    pthread_mutex_unlock(&source->lock);
    pthread_mutex_unlock(&source->opaque_lock);
    llam_windows_async_lifetime_test_phase_gate(task, LLAM_WINDOWS_ASYNC_TEST_BEFORE_SCAN_RELEASE,
                                               &state->scan_hold, &state->scan_release);
    atomic_store_explicit(&state->gate_release, 1U, memory_order_release);
    while (atomic_load_explicit(&state->scan_hold, memory_order_acquire) == 0U) {
        if (GetTickCount64() >= limit) { mm_fail("production op_finish delayed epoch release"); }
        Sleep(1U);
    }
    pthread_mutex_lock(&source->opaque_lock);
    pthread_mutex_lock(&source->lock);
    if (llam_async_preempt_task_origin_shard(task) != UINT_MAX ||
        llam_queue_pop_head(&source->processor.inject_q) != task) {
        mm_fail("delayed-release single migratable entry");
    }
    atomic_store_explicit(&source->processor.inject_depth, source->processor.inject_q.depth,
                          memory_order_release);
    pthread_mutex_unlock(&source->lock);
    pthread_mutex_unlock(&source->opaque_lock);
    llam_reinject_task_on_shard(state->runtime, task, 0U, false, LLAM_TRACE_STATE, LLAM_WAIT_NONE);
    while (atomic_load_explicit(&task->reclaim_ready, memory_order_acquire) == 0U) {
        if (GetTickCount64() >= limit) { mm_fail("destination immediate DEAD with old scan held"); }
        Sleep(1U);
    }
    if (task->async_preempt_task != NULL || task->alloc_owner_shard != 0U ||
        task->windows_async_debt_owner != 1U || task->windows_async_finalizer_owner != 0U ||
        atomic_load_explicit(&state->runtime->shards[0].windows_async_finalizer_count,
                             memory_order_acquire) != 1U) {
        mm_fail("old scan protects object, not a freed capsule / terminal finalizer");
    }
    atomic_store_explicit(&state->source_release, 1U, memory_order_release);
    for (;;) {
        llam_windows_async_stats_t stats = {0};
        if (GetTickCount64() >= limit) { mm_fail("old-reference global drain hold"); }
        if (llam_runtime_live_tasks(state->runtime) == 0U &&
            llam_windows_async_lifetime_stats(state->runtime, &stats) &&
            stats.execution_debt == 0U && stats.finalizer_count == 1U && stats.binding_ops == 1U) {
            for (unsigned id = 0U; id < 2U; ++id) {
                llam_worker_t *worker = &state->runtime->shards[id].primary_worker;
                if (!llam_worker_binding_live(worker, atomic_load_explicit(
                        &worker->binding_incarnation, memory_order_acquire))) {
                    mm_fail("false global close through cross-owner finalizer handoff");
                }
            }
            break;
        }
        Sleep(1U);
    }
    puts("llam-win-multim: delayed op_finish released fresh save; destination DEAD/capsule freed before old scan release; execution=0 finalizer=1 old_binding=1 both-Ms-live");
    atomic_store_explicit(&state->delayed_verified, 1U, memory_order_release);
    atomic_store_explicit(&state->scan_release, 1U, memory_order_release);
    /* No raw object access after the only remaining old-operation pin releases. */
    return 0U;
}

static DWORD WINAPI mm_repeat_tail(void *opaque) {
    mm_state_t *state = opaque;
    uint64_t limit = GetTickCount64() + 7000U;
    while (atomic_load_explicit(&state->gate_observed, memory_order_acquire) == 0U) {
        if (GetTickCount64() >= limit) { mm_fail("late old controller sweep coverage"); }
        Sleep(1U);
    }
    while (llam_windows_async_lifetime_has_debt(state->runtime) ||
           llam_runtime_live_tasks(state->runtime) != 0U) {
        if (GetTickCount64() >= limit) { mm_fail("late controller sweep independent native drain"); }
        Sleep(1U);
    }
    Sleep(3U);
    if (!atomic_load_explicit(&state->runtime->exec_started, memory_order_acquire)) {
        mm_fail("completed run exposed while old controller sweep still owns transition gate");
    }
    puts("llam-win-multim: old sweep held after debt=0; run token remains owned until controller transition release");
    atomic_store_explicit(&state->repeat_verified, 1U, memory_order_release);
    atomic_store_explicit(&state->gate_release, 1U, memory_order_release);
    return 0U;
}

static DWORD WINAPI mm_repeat_identity(void *opaque) {
    mm_state_t *state = opaque;
    llam_task_t *task = (llam_task_t *)state->gs[1].compute_key;
    llam_windows_async_operation_t current = {0};
    llam_windows_async_operation_t stale;
    uint64_t limit = GetTickCount64() + 5000U;
    while (atomic_load_explicit(&state->post_capture, memory_order_acquire) == 0U) {
        if (GetTickCount64() >= limit) { mm_fail("second-run current bank coverage"); }
        Sleep(1U);
    }
    if (!llam_windows_async_lifetime_test_copy_operation(state->runtime, 1U, &current) ||
        current.task != task || current.runtime_run != 2U || current.binding_incarnation < 2U ||
        !llam_windows_async_lifetime_test_operation_matches(&current)) {
        mm_fail("second run fresh actual binding / copied live identity");
    }
    stale = current;
    --stale.runtime_run;
    --stale.binding_incarnation;
    if (llam_windows_async_lifetime_test_operation_matches(&stale) ||
        llam_windows_async_lifetime_op_capture_committed(&stale) ||
        llam_windows_async_lifetime_op_restoring(&stale, current.capture_task_run + 1U,
                                                current.capture_worker_run + 1U)) {
        mm_fail("old-run copied identity accepted against new live bank");
    }
    printf("llam-win-multim: second-run generation=%" PRIu64 " incarnation=%" PRIu64
           " old-generation refused before mutation\n", current.runtime_run, current.binding_incarnation);
    atomic_store_explicit(&state->post_release, 1U, memory_order_release);
    return 0U;
}

static DWORD WINAPI mm_startup_stop(void *opaque) {
    mm_state_t *state = opaque;
    uint64_t limit = GetTickCount64() + 5000U;
    while (atomic_load_explicit(&state->gate_observed, memory_order_acquire) == 0U) {
        if (GetTickCount64() >= limit) { mm_fail("staged native worker startup"); }
        Sleep(1U);
    }
    if (llam_runtime_request_stop_rt(state->runtime) != 0) { mm_fail("stop during PREPARING"); }
    atomic_store_explicit(&state->gate_release, 1U, memory_order_release);
    return 0U;
}

int main(int argc, char **argv) {
    mm_state_t state = {0};
    llam_runtime_opts_t options;
    llam_windows_async_preempt_opts_t async_options = {0};
    llam_windows_async_pe_range_set_t ranges = {0};
    llam_windows_async_stats_t stats = {0};
    HMODULE module;
    FARPROC function;
    HANDLE deadline_done, deadline_thread, observer = NULL;
    DWORD_PTR old_mask, system_mask, limited = 0U;
    unsigned workers = 2U, failure = 0U, failure_index = 0U;
    bool overlap;

    if (argc != 3) {
        return 2;
    }
    overlap = strcmp(argv[1], "overlap") == 0 || strcmp(argv[1], "stop") == 0 ||
              strcmp(argv[1], "destroy") == 0;
    state.stop_mixed = strcmp(argv[1], "stop") == 0;
    state.destroy_mixed = strcmp(argv[1], "destroy") == 0;
    state.three_owners = strcmp(argv[1], "owners3") == 0;
    state.delayed = strcmp(argv[1], "delayed") == 0;
    state.repeat = strcmp(argv[1], "repeat") == 0;
    state.startup_stop = strcmp(argv[1], "startup-stop") == 0;
    state.migration = strcmp(argv[1], "migration") == 0 || state.three_owners || state.delayed;
    state.migration_target = state.three_owners ? 2U : 0U;
    if (state.three_owners) { workers = 3U; }
    state.trace_run = 1U;
    if (strcmp(argv[1], "alloc1") == 0) { failure = LLAM_WINDOWS_ASYNC_TEST_FAIL_SLOT_ALLOC; failure_index = 1U; }
    else if (strcmp(argv[1], "create2") == 0) { failure = LLAM_WINDOWS_ASYNC_TEST_FAIL_THREAD_START; failure_index = 2U; workers = 3U; }
    else if (strcmp(argv[1], "dup0") == 0) { failure = LLAM_WINDOWS_ASYNC_TEST_FAIL_DUPLICATE; }
    else if (strcmp(argv[1], "dup2") == 0) { failure = LLAM_WINDOWS_ASYNC_TEST_FAIL_DUPLICATE; failure_index = 2U; workers = 3U; }
    else if (strcmp(argv[1], "probe2") == 0) { failure = LLAM_WINDOWS_ASYNC_TEST_FAIL_QUALIFICATION; failure_index = 2U; workers = 3U; }
    else if (!overlap && !state.migration && !state.repeat && !state.startup_stop &&
             strcmp(argv[1], "positive") != 0) { return 2; }
    if (!GetProcessAffinityMask(GetCurrentProcess(), &old_mask, &system_mask)) {
        return 77;
    }
    {
    unsigned available = 0U, eligible = 0U;
    for (unsigned i = 0U; i < sizeof(DWORD_PTR) * CHAR_BIT; ++i) {
        available += ((old_mask & system_mask) & ((DWORD_PTR)1U << i)) != 0U;
    }
    for (unsigned i = 0U, n = 0U; i < sizeof(DWORD_PTR) * CHAR_BIT && n < workers; ++i) {
        DWORD_PTR bit = (DWORD_PTR)1U << i;
        if ((old_mask & system_mask & bit) != 0U) {
            /* Prefer a hole, when possible, to verify concrete CPU ids too. */
            if (available > workers && eligible++ == 1U) { continue; }
            limited |= bit; ++n;
        }
    }
    if (available < workers) { return 77; }
    }
    if (!SetProcessAffinityMask(GetCurrentProcess(), limited)) {
        return 77;
    }
    atomic_init(&state.entry_count, 0U);
    atomic_init(&state.gate_observed, 0U);
    atomic_init(&state.gate_release, 0U);
    atomic_init(&state.overlap_observed, 0U);
    atomic_init(&state.first_call_active, 0U);
    atomic_init(&state.free_gate_observed, 0U);
    atomic_init(&state.free_gate_release, 0U);
    atomic_init(&state.free_remote_observed, 0U);
    atomic_init(&state.migrated, 0U);
    atomic_init(&state.post_capture, 0U);
    atomic_init(&state.post_release, 0U);
    atomic_init(&state.stale_refused, 0U);
    atomic_init(&state.owners_verified, 0U);
    atomic_init(&state.destroy_done, 0U);
    atomic_init(&state.delayed_verified, 0U);
    atomic_init(&state.source_save, 0U);
    atomic_init(&state.source_release, 0U);
    atomic_init(&state.scan_hold, 0U);
    atomic_init(&state.scan_release, 0U);
    atomic_init(&state.repeat_verified, 0U);
    deadline_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    deadline_thread = deadline_done != NULL
        ? CreateThread(NULL, 0U, mm_deadline, deadline_done, 0U, NULL) : NULL;
    if (deadline_thread == NULL) { mm_fail("deadline thread"); }
    module = LoadLibraryA(argv[2]);
    function = module != NULL ? GetProcAddress(module, "llam_r8_cpu_loop_v1") : NULL;
    if (function == NULL || llam_windows_async_pe_collect_ranges(module, &ranges) != 0) {
        mm_fail("unchanged compiler-certified fixture");
    }
    _Static_assert(sizeof(function) == sizeof(state.loop), "function pointer size");
    memcpy(&state.loop, &function, sizeof(function));
    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) { mm_fail("runtime opts"); }
    options.deterministic = 0U;
    options.preempt_mode = LLAM_PREEMPT_AUTO;
    options.preempt_quantum_ns = 1000000U;
    if (llam_runtime_create_compat(&options, sizeof(options), &state.runtime) != 0 ||
        state.runtime->active_shards != workers) { mm_fail("process-affinity fixed worker count"); }
    for (unsigned i = 0U; i < workers; ++i) {
        if (state.runtime->allowed_cpus[i] >= sizeof(DWORD_PTR) * CHAR_BIT ||
            (limited & ((DWORD_PTR)1U << state.runtime->allowed_cpus[i])) == 0U) {
            mm_fail("concrete allowed CPU id respects sparse process affinity");
        }
    }
    async_options.flags = LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST;
    async_options.ranges = ranges.ranges;
    async_options.range_count = ranges.range_count;
    if (failure == LLAM_WINDOWS_ASYNC_TEST_FAIL_SLOT_ALLOC) {
        llam_windows_async_lifetime_test_fail_worker(failure, failure_index);
        if (llam_runtime_enable_experimental_windows_async_preempt_ex(
                state.runtime, &async_options, sizeof(async_options)) != -1 || errno != ENOMEM ||
            state.runtime->async_preempt_control != NULL ||
            atomic_load_explicit(&state.runtime->async_preempt_enabled, memory_order_acquire) != 0U) {
            mm_fail("nth-slot all-or-nothing enable unwind");
        }
    } else {
        int result = llam_runtime_enable_experimental_windows_async_preempt_ex(
            state.runtime, &async_options, sizeof(async_options));
        if (result != 0 && errno == ENOTSUP) { return 77; }
        if (result != 0) { mm_fail("multi-M public expert enable"); }
        llam_windows_async_lifetime_test_error_sentinels(state.runtime);
        if (failure != 0U || state.startup_stop) {
            uintptr_t ignored = 0U;
            (void)mm_spawn(&state, 0U, mm_empty_task, (mm_g_t *)&state, &ignored);
            if (state.startup_stop) {
                llam_windows_async_lifetime_test_start_gate(state.runtime, 1U,
                    &state.gate_observed, &state.gate_release);
                observer = CreateThread(NULL, 0U, mm_startup_stop, &state, 0U, NULL);
                if (observer == NULL) { mm_fail("startup stop observer"); }
            } else {
                llam_windows_async_lifetime_test_fail_worker(failure, failure_index);
            }
            if (llam_runtime_run_handle(state.runtime) != -1 ||
                atomic_load_explicit(&state.entry_count, memory_order_acquire) != 0U ||
                llam_windows_async_lifetime_has_debt(state.runtime)) {
                mm_fail("partial startup abort before first G dispatch");
            }
            if (state.startup_stop) {
                if (WaitForSingleObject(observer, 1000U) != WAIT_OBJECT_0) { mm_fail("startup stop cleanup"); }
                CloseHandle(observer);
                llam_windows_async_lifetime_test_start_gate(NULL, UINT_MAX, NULL, NULL);
            }
            for (unsigned id = 0U; id < workers; ++id) {
                if (state.runtime->shards[id].thread_started ||
                    atomic_load_explicit(&state.runtime->shards[id].primary_worker.active_g,
                                         memory_order_acquire) != NULL) {
                    mm_fail("partial startup worker cleanup");
                }
            }
        } else {
            for (unsigned round = 0U; round < (state.repeat ? 2U : 1U); ++round) {
            DWORD handles_before = 0U, handles_after = 0U;
            llam_windows_async_stats_t before_stats = {0};
            state.trace_run = (uint64_t)round + 1U;
            /* The second run also holds capture completion for identity
             * inspection. Leave enough finite work for its two full cycles;
             * keep every other workload unchanged and validate both inputs. */
            state.iterations = state.repeat && round == 1U ? 2U * MM_ITERATIONS : MM_ITERATIONS;
            state.expected = mm_reference(state.iterations);
            observer = NULL;
            if (state.migration) {
                llam_task_t *raw;
                state.gs[0].compute = mm_spawn(&state, 0U, mm_migrating_compute,
                    &state, &state.gs[0].compute_key);
                raw = (llam_task_t *)state.gs[0].compute_key;
                pthread_mutex_lock(&state.runtime->shards[0].lock);
                if (llam_queue_pop_head(&state.runtime->shards[0].processor.inject_q) != raw) {
                    mm_fail("pre-run allocation A queue removal");
                }
                atomic_store_explicit(&state.runtime->shards[0].processor.inject_depth,
                    state.runtime->shards[0].processor.inject_q.depth, memory_order_release);
                pthread_mutex_unlock(&state.runtime->shards[0].lock);
                atomic_store_explicit(&raw->home_shard, 1U, memory_order_relaxed);
                llam_reinject_task_on_shard(state.runtime, raw, 1U, false, LLAM_TRACE_STATE, LLAM_WAIT_NONE);
                state.gs[1].compute_key = state.gs[0].compute_key;
                if ((state.three_owners || state.delayed) &&
                    ((state.three_owners && !llam_task_scan_ref_try_acquire(state.runtime, raw)) ||
                     llam_detach(state.gs[0].compute) != 0)) {
                    mm_fail("detached three-owner scan pin");
                }
            }
            for (unsigned id = 0U; id < 2U; ++id) {
                state.gs[id].state = &state;
                unsigned placement = state.migration && id == 0U ? state.migration_target : id;
                state.gs[id].id = placement;
                atomic_init(&state.gs[id].done, 0U);
                if (!state.migration) {
                    state.gs[id].compute = mm_spawn(&state, id, mm_compute, &state.gs[id], &state.gs[id].compute_key);
                }
                state.gs[id].peer = mm_spawn(&state, placement,
                    state.migration ? mm_migration_peer : mm_peer, &state.gs[id], &state.gs[id].peer_key);
            }
            if (!GetProcessHandleCount(GetCurrentProcess(), &handles_before)) { mm_fail("pre-run handle count"); }
            if (!llam_windows_async_lifetime_stats(state.runtime, &before_stats)) { mm_fail("pre-run owned native handles"); }
            if (overlap) {
                llam_windows_async_lifetime_test_phase_gate((llam_task_t *)state.gs[0].compute_key,
                    LLAM_WINDOWS_ASYNC_TEST_READY, &state.gate_observed, &state.gate_release);
                observer = CreateThread(NULL, 0U, mm_overlap, &state, 0U, NULL);
                if (observer == NULL) { mm_fail("overlap observer"); }
            } else if (state.migration) {
                llam_windows_async_lifetime_test_phase_gate((llam_task_t *)state.gs[0].compute_key,
                    state.delayed ? LLAM_WINDOWS_ASYNC_TEST_BEFORE_OP_FINISH
                                  : LLAM_WINDOWS_ASYNC_TEST_AFTER_BANK_FREE,
                    state.delayed ? &state.gate_observed : &state.free_gate_observed,
                    state.delayed ? &state.gate_release : &state.free_gate_release);
                observer = CreateThread(NULL, 0U,
                    state.delayed ? mm_delayed_observer : mm_migration_observer, &state, 0U, NULL);
                if (observer == NULL) { mm_fail("migration observer"); }
            } else if (state.repeat) {
                if (round == 0U) {
                    llam_windows_async_lifetime_test_tail_gate(state.runtime,
                        &state.gate_observed, &state.gate_release);
                    observer = CreateThread(NULL, 0U, mm_repeat_tail, &state, 0U, NULL);
                } else {
                    llam_windows_async_lifetime_test_phase_gate((llam_task_t *)state.gs[1].compute_key,
                        LLAM_WINDOWS_ASYNC_TEST_BEFORE_CAPTURE_HANDOFF, &state.post_capture, &state.post_release);
                    observer = CreateThread(NULL, 0U, mm_repeat_identity, &state, 0U, NULL);
                }
                if (observer == NULL) { mm_fail("repeat-run observer"); }
            }
            if (llam_runtime_run_handle(state.runtime) != 0) { mm_fail("two-M finite run"); }
            if (observer != NULL) {
                if (WaitForSingleObject(observer, 1000U) != WAIT_OBJECT_0 ||
                    (!state.migration && !state.repeat && atomic_load_explicit(&state.overlap_observed, memory_order_acquire) != 1U)) {
                    if (state.repeat) {
                        fprintf(stderr, "repeat observer: round=%u gate=%u released=%u verified=%u\n",
                                round, atomic_load(&state.gate_observed),
                                atomic_load(&state.gate_release), atomic_load(&state.repeat_verified));
                        (void)mm_trace(&state, true);
                    }
                    mm_fail("overlap observer completion");
                }
                CloseHandle(observer);
                llam_windows_async_lifetime_test_phase_gate(NULL, 0U, NULL, NULL);
                llam_windows_async_lifetime_test_tail_gate(NULL, NULL, NULL);
            }
            if (state.destroy_mixed) {
                if (WaitForSingleObject(state.destroy_thread, 1000U) != WAIT_OBJECT_0 ||
                    atomic_load_explicit(&state.destroy_done, memory_order_acquire) != 1U) {
                    mm_fail("public destroy all-M drain completion");
                }
                CloseHandle(state.destroy_thread);
                state.runtime = NULL; /* Public destroy consumed the runtime. */
                for (unsigned id = 0U; id < 2U; ++id) {
                    if (state.gs[id].actual != state.expected ||
                        atomic_load_explicit(&state.gs[id].done, memory_order_acquire) != 1U) {
                        mm_fail("mixed destroy exact finite results");
                    }
                }
                goto runtime_finished;
            }
            if (!llam_windows_async_lifetime_stats(state.runtime, &stats) ||
                stats.binding_ops != 0U || stats.captured_bank_active != 0U ||
                stats.pending_restore_active != 0U || stats.execution_debt != 0U ||
                stats.retire_count != 0U || stats.finalizer_count != 0U ||
                stats.remote_transactions_max != 1U || stats.scheduler_error_checks == 0U ||
                stats.native_handles_created - before_stats.native_handles_created != workers ||
                stats.native_handles_closed - before_stats.native_handles_closed != workers ||
                stats.native_handles_created != stats.native_handles_closed) {
                mm_fail("global all-M retirement drain / native scheduler error isolation");
            }
            if (!state.delayed) { (void)mm_trace(&state, true); }
            if (state.delayed && atomic_load_explicit(&state.delayed_verified, memory_order_acquire) != 1U) {
                mm_fail("delayed release oracle");
            }
            if (state.migration && !state.delayed) {
                if ((!state.three_owners &&
                     (((llam_task_t *)state.gs[0].compute_key)->windows_async_debt_owner != 1U ||
                      ((llam_task_t *)state.gs[0].compute_key)->alloc_owner_shard != 0U ||
                      ((llam_task_t *)state.gs[0].compute_key)->windows_async_finalizer_owner != 0U)) ||
                    (state.three_owners && atomic_load_explicit(&state.owners_verified, memory_order_acquire) != 1U) ||
                    (!state.three_owners && atomic_load_explicit(&state.free_remote_observed, memory_order_acquire) != 1U) ||
                    atomic_load_explicit(&state.stale_refused, memory_order_acquire) != 1U) {
                    mm_fail("migration origin / immutable charge / terminal cleanup identities");
                }
                printf("llam-win-multim: free_remote_observed=%u owners_verified=%u stale_B_refused=%u errno/LastError preserved\n",
                    atomic_load_explicit(&state.free_remote_observed, memory_order_acquire),
                    atomic_load_explicit(&state.owners_verified, memory_order_acquire),
                    atomic_load_explicit(&state.stale_refused, memory_order_acquire));
            }
            for (unsigned id = 0U; id < 2U; ++id) {
                if ((!state.migration || id == 0U) &&
                    (state.gs[id].actual != state.expected ||
                     atomic_load_explicit(&state.gs[id].done, memory_order_acquire) != 1U ||
                     (!state.three_owners && !state.delayed && llam_join(state.gs[id].compute) != 0))) {
                    mm_fail("exact migrated finite result / consuming join");
                }
                if (
                    llam_join(state.gs[id].peer) != 0) {
                    mm_fail("exact finite result / consuming joins");
                }
            }
            if (llam_windows_async_lifetime_charged_bytes(state.runtime) != 0U) {
                mm_fail("capsule quota drain");
            }
            if (!GetProcessHandleCount(GetCurrentProcess(), &handles_after)) {
                mm_fail("post-run process handle diagnostic");
            }
            printf("llam-win-multim: owned-native-created=%" PRIu64 " closed=%" PRIu64
                   " process-handles-before=%lu after=%lu (all-process diagnostic, not owned ledger)\n",
                   stats.native_handles_created - before_stats.native_handles_created,
                   stats.native_handles_closed - before_stats.native_handles_closed,
                   (unsigned long)handles_before, (unsigned long)handles_after);
            }
        }
    }
runtime_finished:
    if (state.repeat && atomic_load_explicit(&state.repeat_verified, memory_order_acquire) != 1U) {
        mm_fail("forced late old sweep / run transition");
    }
    if (state.runtime != NULL) { llam_runtime_destroy(state.runtime); }
    llam_windows_async_pe_release_ranges(&ranges);
    FreeLibrary(module);
    SetEvent(deadline_done);
    if (WaitForSingleObject(deadline_thread, 1000U) != WAIT_OBJECT_0) { mm_fail("deadline completion"); }
    CloseHandle(deadline_thread);
    CloseHandle(deadline_done);
    if (!SetProcessAffinityMask(GetCurrentProcess(), old_mask)) { mm_fail("restore child affinity"); }
    printf("llam-win-multim: %s passed workers=%u user_entries=%u expected=%" PRIu64 "\n",
           argv[1], workers, atomic_load_explicit(&state.entry_count, memory_order_acquire), state.expected);
    return 0;
}
