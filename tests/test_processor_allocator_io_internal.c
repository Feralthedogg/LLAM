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
 * See LICENSES/Apache-2.0.txt.
 */

#include "runtime_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__SANITIZE_ADDRESS__)
#define LLAM_TEST_ASAN_STACK 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LLAM_TEST_ASAN_STACK 1
#endif
#endif
#if defined(LLAM_TEST_ASAN_STACK)
#include <sanitizer/asan_interface.h>
#endif

static int fail(const char *message) {
    fprintf(stderr, "test_processor_allocator_io_internal: %s\n", message);
    return -1;
}

static int create_runtime(uint64_t experimental_flags, llam_runtime_t **out) {
    llam_runtime_opts_t opts;

    *out = NULL;
    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    opts.experimental_flags = experimental_flags;
    return llam_runtime_create(&opts, sizeof(opts), out);
}

static int test_io_request_uses_processor_route_and_allocator(uint64_t flags) {
    llam_runtime_t *runtime = NULL;
    llam_shard_t *source;
    llam_shard_t *remote;
    llam_io_req_t *req;
    llam_io_req_t *reused;
    unsigned source_id = 0U;
    unsigned remote_id;
    int expected_node;
    bool remote_bound = false;
    int rc = -1;

    llam_scheduler_clear_identity();
    if (create_runtime(flags, &runtime) != 0 || runtime == NULL ||
        runtime->shards == NULL || runtime->active_shards == 0U ||
        runtime->active_nodes == 0U) {
        llam_runtime_destroy(runtime);
        return fail("could not create I/O-route runtime");
    }
    source = &runtime->shards[source_id];
    remote_id = runtime->active_shards > 1U ? 1U : source_id;
    remote = &runtime->shards[remote_id];
    expected_node = (int)llam_processor_io_node_index(&source->processor);
    req = llam_io_req_alloc(source);
    if (req == NULL) {
        llam_runtime_destroy(runtime);
        return fail("processor allocator could not allocate an I/O request");
    }
    if (req->alloc_owner_shard != source_id ||
        atomic_load_explicit(&req->owner_shard, memory_order_acquire) != source_id ||
        expected_node < 0 || (unsigned)expected_node >= runtime->active_nodes ||
        llam_io_req_node_index(req) != expected_node ||
        ((flags & LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS) != 0U &&
         (unsigned)expected_node != source_id)) {
        llam_io_req_free(source, req);
        llam_allocator_quiescent(source);
        llam_runtime_destroy(runtime);
        return fail("I/O request route diverged from its processor's configured node");
    }

    if (remote_id != source_id) {
        llam_scheduler_bind_identity(remote,
                                     &remote->primary_worker,
                                     &remote->scheduler_ctx);
        remote_bound = true;
    }
    llam_io_req_free(remote, req);
    if (atomic_load_explicit(&source->processor.allocator.io_req_remote_free,
                             memory_order_acquire) != req ||
        atomic_load_explicit(&remote->processor.allocator.remote_free_pending,
                             memory_order_acquire) != 0U) {
        if (remote_bound) {
            llam_scheduler_unbind_identity(remote, &remote->primary_worker);
            llam_scheduler_clear_identity();
        }
        llam_runtime_destroy(runtime);
        return fail("foreign I/O-request free did not return to the allocation P");
    }
    llam_allocator_quiescent(source);
    if (source->processor.allocator.io_req_free != req ||
        source->processor.allocator.io_req_remote_drains == 0U ||
        atomic_load_explicit(&source->processor.allocator.remote_free_pending,
                             memory_order_acquire) != 0U) {
        if (remote_bound) {
            llam_scheduler_unbind_identity(remote, &remote->primary_worker);
            llam_scheduler_clear_identity();
        }
        llam_runtime_destroy(runtime);
        return fail("owner P did not drain the remote I/O-request free list");
    }
    if (remote_bound) {
        llam_scheduler_unbind_identity(remote, &remote->primary_worker);
        llam_scheduler_clear_identity();
        remote_bound = false;
    }

    reused = llam_io_req_alloc(source);
    if (reused != req) {
        if (reused != NULL) {
            llam_io_req_free(source, reused);
            llam_allocator_quiescent(source);
        }
        llam_runtime_destroy(runtime);
        return fail("processor-local I/O-request cache did not reuse its drained object");
    }
    llam_io_req_free(source, reused);
    llam_allocator_quiescent(source);
    rc = 0;
    llam_runtime_destroy(runtime);
    return rc;
}

static int test_io_route_tracks_logical_owner_and_attached_node(uint64_t flags) {
    llam_runtime_t *runtime = NULL;
    llam_shard_t *source;
    llam_shard_t *target;
    llam_io_req_t *req;
    unsigned target_id;
    unsigned attached_route;
    int source_route;
    int target_route;

    llam_scheduler_clear_identity();
    if (create_runtime(flags, &runtime) != 0 || runtime == NULL ||
        runtime->shards == NULL || runtime->active_shards == 0U ||
        runtime->active_nodes == 0U) {
        llam_runtime_destroy(runtime);
        return fail("could not create logical I/O-route runtime");
    }
    source = &runtime->shards[0];
    target_id = runtime->active_shards > 1U ? 1U : 0U;
    target = &runtime->shards[target_id];
    req = llam_io_req_alloc(source);
    if (req == NULL) {
        llam_runtime_destroy(runtime);
        return fail("could not allocate request for route migration checks");
    }

    source_route = llam_io_req_node_index(req);
    target_route = (int)llam_processor_io_node_index(&target->processor);
    if (source_route < 0 || target_route < 0 ||
        (unsigned)target_route >= runtime->active_nodes) {
        llam_io_req_free(source, req);
        llam_allocator_quiescent(source);
        llam_runtime_destroy(runtime);
        return fail("processor I/O-route accessor returned an invalid node");
    }
    attached_route = (unsigned)target_route;
    if (runtime->active_nodes > 1U) {
        attached_route = ((unsigned)target_route + 1U) % runtime->active_nodes;
    }

    /* Model the owner publication performed by a completed request rehome. */
    atomic_store_explicit(&req->owner_shard, target_id, memory_order_release);
    if (llam_io_req_node_index(req) != target_route) {
        llam_io_req_free(source, req);
        llam_allocator_quiescent(source);
        llam_runtime_destroy(runtime);
        return fail("unattached request did not follow its rehomed logical P");
    }

    /* While attached, the backend node remains authoritative for completion. */
    atomic_store_explicit(&req->attached_node_index,
                          attached_route,
                          memory_order_release);
    if (llam_io_req_node_index(req) != (int)attached_route) {
        llam_io_req_free(source, req);
        llam_allocator_quiescent(source);
        llam_runtime_destroy(runtime);
        return fail("in-flight completion route ignored the attached node");
    }
    atomic_store_explicit(&req->attached_node_index, UINT_MAX, memory_order_release);
    if (llam_io_req_node_index(req) != target_route) {
        llam_io_req_free(source, req);
        llam_allocator_quiescent(source);
        llam_runtime_destroy(runtime);
        return fail("detached request did not resume routing through its logical P");
    }

    /* The logical owner changed, but allocation ownership did not. */
    llam_io_req_free(target, req);
    llam_allocator_quiescent(source);
    if (source->processor.allocator.io_req_free != req) {
        llam_runtime_destroy(runtime);
        return fail("logical rehome changed the request's allocation owner");
    }
    llam_runtime_destroy(runtime);
    return 0;
}

static int test_same_id_cross_runtime_remote_free(void) {
    llam_runtime_t *owner_runtime = NULL;
    llam_runtime_t *foreign_runtime = NULL;
    llam_shard_t *owner = NULL;
    llam_shard_t *foreign = NULL;
    llam_io_req_t *req = NULL;
    llam_io_req_t *allocated_req = NULL;
    bool foreign_bound = false;
    int result = -1;

    llam_scheduler_clear_identity();
    if (create_runtime(0U, &owner_runtime) != 0 || owner_runtime == NULL ||
        create_runtime(0U, &foreign_runtime) != 0 || foreign_runtime == NULL ||
        owner_runtime->shards == NULL || foreign_runtime->shards == NULL ||
        owner_runtime->active_shards == 0U || foreign_runtime->active_shards == 0U) {
        fail("could not create runtimes for same-ID ownership test");
        goto cleanup;
    }
    owner = &owner_runtime->shards[0];
    foreign = &foreign_runtime->shards[0];
    allocated_req = llam_io_req_alloc(owner);
    if (allocated_req == NULL) {
        fail("could not allocate cross-runtime I/O request");
        goto cleanup;
    }
    req = allocated_req;
    llam_scheduler_bind_identity(foreign,
                                 &foreign->primary_worker,
                                 &foreign->scheduler_ctx);
    foreign_bound = true;
    llam_io_req_free(foreign, req);
    req = NULL;

    if (atomic_load_explicit(&owner->processor.allocator.io_req_remote_free,
                             memory_order_acquire) != allocated_req ||
        atomic_load_explicit(&owner->processor.allocator.remote_free_pending,
                             memory_order_acquire) == 0U ||
        atomic_load_explicit(&foreign->processor.allocator.remote_free_pending,
                             memory_order_acquire) != 0U) {
        fail("same numeric shard ID returned a request to the wrong runtime");
        goto cleanup;
    }

    llam_scheduler_unbind_identity(foreign, &foreign->primary_worker);
    llam_scheduler_clear_identity();
    foreign_bound = false;
    llam_allocator_quiescent(owner);
    if (owner->processor.allocator.io_req_free != allocated_req ||
        owner->processor.allocator.io_req_remote_drains == 0U ||
        atomic_load_explicit(&owner->processor.allocator.remote_free_pending,
                             memory_order_acquire) != 0U) {
        fail("allocation runtime did not drain its processor remote-free list");
        goto cleanup;
    }

    req = llam_io_req_alloc(owner);
    if (req != allocated_req) {
        fail("cross-runtime remote free was not reusable by its allocation P");
        goto cleanup;
    }
    llam_io_req_free(foreign, req);
    req = NULL;
    llam_allocator_quiescent(owner);
    result = 0;

cleanup:
    if (foreign_bound) {
        llam_scheduler_unbind_identity(foreign, &foreign->primary_worker);
        llam_scheduler_clear_identity();
    }
    if (req != NULL && owner_runtime != NULL && owner_runtime->shards != NULL) {
        llam_io_req_free(owner != NULL ? owner : &owner_runtime->shards[0], req);
        llam_allocator_quiescent(owner != NULL ? owner : &owner_runtime->shards[0]);
    }
    if (owner_runtime != NULL) {
        llam_runtime_destroy(owner_runtime);
    }
    if (foreign_runtime != NULL) {
        llam_runtime_destroy(foreign_runtime);
    }
    return result;
}

static int test_partial_init_drains_p_stack_caches(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *failed_runtime = NULL;
    llam_runtime_t *recovery_runtime = NULL;
    int rc;

    llam_scheduler_clear_identity();
    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return fail("could not initialize partial-init options");
    }
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    llam_runtime_test_fail_after_stack_cache_prewarm();
    errno = 0;
    rc = llam_runtime_create(&opts, sizeof(opts), &failed_runtime);
    if (rc == 0 || failed_runtime != NULL || errno != ENOMEM) {
        llam_runtime_destroy(failed_runtime);
        return fail("test hook did not fail after processor stack-cache prewarm");
    }

    if (create_runtime(0U, &recovery_runtime) != 0 || recovery_runtime == NULL) {
        llam_runtime_destroy(recovery_runtime);
        return fail("runtime creation failed after partial-init cleanup");
    }
    llam_runtime_destroy(recovery_runtime);
    return 0;
}

static int test_stack_cache_tracks_home_processor(void) {
    llam_runtime_t *runtime = NULL;
    llam_shard_t *source;
    llam_shard_t *target;
    llam_task_t *task = NULL;
    unsigned source_large_after_alloc;
    unsigned source_large_after_release;
    unsigned target_large_before;
    unsigned target_large_after;

    llam_scheduler_clear_identity();
    if (create_runtime(0U, &runtime) != 0 || runtime == NULL ||
        runtime->shards == NULL || runtime->active_shards == 0U) {
        llam_runtime_destroy(runtime);
        return fail("could not create processor stack-cache runtime");
    }
    if (runtime->active_shards < 2U) {
        fprintf(stderr,
                "test_processor_allocator_io_internal: SKIP stack cache rehome (one processor)\n");
        llam_runtime_destroy(runtime);
        return 77;
    }
    source = &runtime->shards[0];
    target = &runtime->shards[1];
    task = llam_task_alloc(source);
    if (task == NULL || llam_alloc_task_stack(task, LLAM_STACK_CLASS_LARGE) != 0) {
        if (task != NULL) {
            llam_task_allocator_free(task);
            llam_allocator_quiescent(source);
        }
        llam_runtime_destroy(runtime);
        return fail("could not allocate task/stack for P cache rehome");
    }

    pthread_mutex_lock(&source->processor.stack_cache_lock);
    source_large_after_alloc = source->processor.stack_cache_large_count;
    pthread_mutex_unlock(&source->processor.stack_cache_lock);
    pthread_mutex_lock(&target->processor.stack_cache_lock);
    target_large_before = target->processor.stack_cache_large_count;
    pthread_mutex_unlock(&target->processor.stack_cache_lock);

    atomic_store_explicit(&task->home_shard, target->id, memory_order_release);
    llam_task_release_stack(task);

    pthread_mutex_lock(&target->processor.stack_cache_lock);
    target_large_after = target->processor.stack_cache_large_count;
    pthread_mutex_unlock(&target->processor.stack_cache_lock);
    pthread_mutex_lock(&source->processor.stack_cache_lock);
    source_large_after_release = source->processor.stack_cache_large_count;
    pthread_mutex_unlock(&source->processor.stack_cache_lock);
    if (task->stack_mapping != NULL || target_large_after != target_large_before + 1U ||
        source_large_after_release != source_large_after_alloc) {
        llam_task_allocator_free(task);
        llam_allocator_quiescent(source);
        llam_runtime_destroy(runtime);
        return fail("stack release did not follow atomic home-P cache ownership");
    }
    llam_task_allocator_free(task);
    llam_allocator_quiescent(source);
    llam_runtime_destroy(runtime);
    return 0;
}

static int test_buffer_slab_stride_and_remote_reuse(void) {
    llam_runtime_t *runtime = NULL;
    llam_io_buffer_t *buffers[LLAM_IO_BUFFER_SLAB_COUNT * 2U + 1U] = {0};
    unsigned count = (unsigned)(sizeof(buffers) / sizeof(buffers[0]));
    llam_shard_t *owner;
    int result = -1;

    llam_scheduler_clear_identity();
    if (create_runtime(0U, &runtime) != 0) {
        return fail("could not create buffer slab runtime");
    }
    owner = &runtime->shards[0];
    for (unsigned round = 0U; round < 2U; ++round) {
        for (unsigned i = 0U; i < count; ++i) {
            size_t capacity = i == count - 1U ? 8192U : 64U;

            buffers[i] = llam_io_buffer_allocator_alloc(owner, capacity);
            if (buffers[i] == NULL || buffers[i]->capacity < capacity ||
                buffers[i]->inline_capacity != LLAM_IO_BUFFER_INLINE_BYTES) {
                fail("buffer slab did not retain its inline backing capacity");
                goto cleanup;
            }
            for (size_t j = 0U; j < buffers[i]->capacity; ++j) {
                if (buffers[i]->data[j] != 0U) {
                    fail("recycled slab buffer exposed the previous payload");
                    goto cleanup;
                }
            }
            memset(buffers[i]->data, (int)i + 1, buffers[i]->capacity);
        }
        for (unsigned i = 0U; i < count; ++i) {
            for (size_t j = 0U; j < buffers[i]->capacity; ++j) {
                if (buffers[i]->data[j] != (unsigned char)(i + 1U)) {
                    fail("variable-sized slab entries overlap");
                    goto cleanup;
                }
            }
            llam_io_buffer_allocator_free(buffers[i]);
            buffers[i] = NULL;
        }
        llam_allocator_quiescent(owner);
    }
    result = 0;
cleanup:
    for (unsigned i = 0U; i < count; ++i) {
        if (buffers[i] != NULL) {
            llam_io_buffer_allocator_free(buffers[i]);
        }
    }
    llam_allocator_quiescent(owner);
    llam_runtime_destroy(runtime);
    return result;
}

static int test_cold_stack_cache_reuse(void) {
    const llam_stack_class_t classes[] = {LLAM_STACK_CLASS_DEFAULT,
                                         LLAM_STACK_CLASS_LARGE,
                                         LLAM_STACK_CLASS_HUGE};
    const unsigned counts[] = {864U, 96U, 24U};
    llam_task_t *tasks[864] = {0};
    void *mappings[864] = {0};
    llam_runtime_t *runtime = NULL;
    int result = -1;

    llam_scheduler_clear_identity();
    if (create_runtime(0U, &runtime) != 0) {
        return fail("could not create cold stack-cache runtime");
    }
    for (unsigned c = 0U; c < sizeof(classes) / sizeof(classes[0]); ++c) {
        unsigned count = counts[c];
        size_t stack_size = llam_stack_bytes(classes[c]);

        for (unsigned i = 0U; i < count; ++i) {
            tasks[i] = llam_task_alloc(&runtime->shards[0]);
            if (tasks[i] == NULL || llam_alloc_task_stack(tasks[i], classes[c]) != 0) {
                fail("could not allocate stack-cache burst");
                goto cleanup;
            }
            mappings[i] = tasks[i]->stack_mapping;
            memset(tasks[i]->stack_base, 0x5a, tasks[i]->stack_size);
        }
        for (unsigned i = 0U; i < count; ++i) {
            llam_task_release_stack(tasks[i]);
        }
        llam_processor_t *processor = &runtime->shards[0].processor;
        unsigned warm_count = classes[c] == LLAM_STACK_CLASS_DEFAULT
                                  ? processor->stack_cache_default_count
                                  : (classes[c] == LLAM_STACK_CLASS_LARGE
                                         ? processor->stack_cache_large_count
                                         : processor->stack_cache_huge_count);
        unsigned cold_count = classes[c] == LLAM_STACK_CLASS_DEFAULT
                                  ? runtime->stack_cache_default_count
                                  : (classes[c] == LLAM_STACK_CLASS_LARGE
                                         ? runtime->stack_cache_large_count
                                         : runtime->stack_cache_huge_count);

        unsigned warm_total = warm_count + runtime->stack_cache_warm_count[c];

        size_t warm_bytes = c == 0U ? 16U * 1024U * 1024U : 8U * 1024U * 1024U;

        if ((size_t)warm_count * stack_size > warm_bytes ||
            cold_count == 0U || runtime->stack_cache_cold[c] == NULL) {
            fail("stack burst did not leave a bounded warm pool and cold mappings");
            goto cleanup;
        }
        for (unsigned i = 0U; i < count; ++i) {
            bool found = false;

            if (llam_alloc_task_stack(tasks[i], classes[c]) != 0) {
                fail("cold cached mapping could not be reactivated");
                goto cleanup;
            }
            for (unsigned j = 0U; j < count; ++j) {
                if (tasks[i]->stack_mapping == mappings[j]) {
                    found = true;
                    mappings[j] = NULL;
                    break;
                }
            }
            if (!found || tasks[i]->mapping_size != stack_size + (size_t)llam_page_size()) {
                fail("cached mapping or guard extent was lost during reuse");
                goto cleanup;
            }
            if (i < warm_total && *(unsigned char *)tasks[i]->stack_base != 0x5aU) {
                fail("cold cache entries were reused ahead of available warm stacks");
                goto cleanup;
            }
#if LLAM_PLATFORM_LINUX
            if (i >= warm_total && *(unsigned char *)tasks[i]->stack_base != 0U) {
                fail("cold Linux stack payload pages were not discarded");
                goto cleanup;
            }
#endif
            /* Writes after reactivation must fault pages back in safely. */
            memset(tasks[i]->stack_base, 0xa5, tasks[i]->stack_size);
        }
        for (unsigned i = 0U; i < count; ++i) {
            llam_task_release_stack(tasks[i]);
            llam_task_allocator_free(tasks[i]);
            tasks[i] = NULL;
        }
    }
    result = 0;
cleanup:
    for (unsigned i = 0U; i < sizeof(tasks) / sizeof(tasks[0]); ++i) {
        if (tasks[i] != NULL) {
            llam_task_release_stack(tasks[i]);
            llam_task_allocator_free(tasks[i]);
        }
    }
    llam_runtime_destroy(runtime);
    return result;
}

typedef struct select_frame_state {
    llam_channel_t *channels[9];
    unsigned width;
    unsigned mode;
    atomic_uint failures;
} select_frame_state_t;

static bool select_node_in_frame(const llam_task_t *task, const llam_wait_node_t *node) {
    uintptr_t base = (uintptr_t)task->stack_base;
    uintptr_t end = base + task->stack_size;
    uintptr_t address = (uintptr_t)node;

    if (address >= base && address <= end - sizeof(*node)) {
        return true;
    }
#if defined(LLAM_TEST_ASAN_STACK)
    /* ASan may move automatic objects into its fake stack. The driver and
     * waiter are pinned to the same M, so its GC interface maps them back to
     * their real frame without mistaking allocator nodes for stack storage. */
    address = (uintptr_t)__asan_addr_is_in_fake_stack(__asan_get_current_fake_stack(),
                                                     (void *)node, NULL, NULL);
    return address >= base && address < end;
#else
    return false;
#endif
}

static void select_frame_waiter(void *arg) {
    select_frame_state_t *state = arg;
    llam_select_op_t ops[9] = {0};
    void *values[9] = {0};
    size_t selected = SIZE_MAX;
    uint64_t deadline = state->mode == 3U ? llam_now_ns() + 1000000000ULL : UINT64_MAX;
    int rc;
    int error;

    for (unsigned i = 0U; i < state->width; ++i) {
        ops[i].kind = LLAM_SELECT_OP_RECV;
        ops[i].channel = state->channels[i];
        ops[i].recv_out = &values[i];
    }
    rc = llam_channel_select(ops, state->width, deadline, &selected);
    error = errno;
    if ((state->mode == 0U &&
         (rc != 0 || selected != 0U || values[0] != (void *)(uintptr_t)123U)) ||
        (state->mode == 1U &&
         (rc != 0 || selected != state->width - 1U || ops[selected].result_errno != EPIPE)) ||
        (state->mode >= 2U &&
         (rc != -1 || error != (state->mode == 2U ? ECANCELED : ETIMEDOUT)))) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
    /* Exercise stack reuse after select has relinquished all of its waiters. */
    {
        volatile unsigned char scratch[4096];
        unsigned sum = 0U;

        for (unsigned i = 0U; i < sizeof(scratch); ++i) {
            scratch[i] = 0xa5U;
            sum += scratch[i];
        }
        if (sum != sizeof(scratch) * 0xa5U) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        }
    }
}

static void select_frame_driver(void *arg) {
    select_frame_state_t *state = arg;
    const unsigned widths[] = {1U, 4U, 5U, 9U};

    for (unsigned w = 0U; w < sizeof(widths) / sizeof(widths[0]); ++w) {
        for (unsigned mode = 0U; mode < 4U; ++mode) {
            llam_cancel_token_t *token = NULL;
            llam_task_t *handle = NULL;
            llam_task_t *raw = NULL;
            uint64_t until = llam_now_ns() + 2000000000ULL;
            bool inspected = false;
            bool failed = false;

            state->width = widths[w];
            state->mode = mode;
            for (unsigned i = 0U; i < state->width; ++i) {
                state->channels[i] = llam_channel_create(1U);
                if (state->channels[i] == NULL) {
                    failed = true;
                    goto cleanup;
                }
            }
            token = llam_cancel_token_create();
            if (token == NULL) {
                failed = true;
                goto cleanup;
            }
            handle = llam_spawn_ex(select_frame_waiter, state,
                                    &(llam_spawn_opts_t){
                                        .flags = LLAM_SPAWN_F_PINNED,
                                        .cancel_token = token,
                                    }, sizeof(llam_spawn_opts_t));
            raw = llam_task_resolve_public_handle(handle);
            if (raw == NULL) {
                failed = true;
                goto cleanup;
            }
            /* The parent owns the unconsumed join handle. A resolver claim
             * additionally keeps the published select frame from returning. */
            while (!inspected && llam_now_ns() < until) {
                if (llam_task_wait_resolver_try_begin(raw)) {
                    llam_channel_select_state_t *select =
                        atomic_load_explicit(&raw->active_select_state, memory_order_acquire);

                    if (select != NULL && atomic_load_explicit(&raw->state, memory_order_acquire) ==
                                              LLAM_TASK_STATE_PARKED) {
                        inspected = true;
                        for (unsigned i = 0U; i < state->width; ++i) {
                            bool on_stack = select_node_in_frame(raw, select->nodes[i]);

                            if (on_stack != (i < 4U) || select->nodes[i]->task != raw ||
                                select->nodes[i]->select_state != select) {
                                failed = true;
                            }
                        }
                    }
                    llam_task_wait_resolver_end(raw);
                }
                if (!inspected) {
                    llam_yield();
                }
            }
            if (!inspected) {
                failed = true;
            }
            if (mode == 0U && llam_channel_send(state->channels[0], (void *)(uintptr_t)123U) != 0) {
                failed = true;
            } else if (mode == 1U && llam_channel_close(state->channels[state->width - 1U]) != 0) {
                failed = true;
            } else if (mode == 2U && llam_cancel_token_cancel(token) != 0) {
                failed = true;
            }
cleanup:
            /* Raw handle resolution pins the task. Join's final reclaim must
             * not wait on a pin retained by the joining inspector itself. */
            if (raw != NULL) {
                llam_task_end_public_op(raw);
                raw = NULL;
            }
            if (failed) {
                for (unsigned i = 0U; i < state->width; ++i) {
                    if (state->channels[i] != NULL) {
                        (void)llam_channel_close(state->channels[i]);
                    }
                }
            }
            if (handle != NULL && llam_join(handle) != 0) {
                failed = true;
            }
            if (token != NULL && llam_cancel_token_destroy(token) != 0) {
                failed = true;
            }
            for (unsigned i = 0U; i < state->width; ++i) {
                if (state->channels[i] != NULL && llam_channel_destroy(state->channels[i]) != 0) {
                    failed = true;
                }
                state->channels[i] = NULL;
            }
            if (failed) {
                atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
                return;
            }
        }
    }
}

static int test_select_frame_storage_and_lifetime(void) {
    llam_runtime_t *runtime = NULL;
    select_frame_state_t state = {0};
    llam_task_t *driver;
    int result = -1;

    atomic_init(&state.failures, 0U);
    if (create_runtime(0U, &runtime) != 0) {
        return fail("could not create select frame runtime");
    }
    driver = llam_runtime_spawn_ex(runtime, select_frame_driver, &state,
                                    &(llam_spawn_opts_t){.flags = LLAM_SPAWN_F_PINNED},
                                    sizeof(llam_spawn_opts_t));
    if (driver != NULL && llam_runtime_run_handle(runtime) == 0 &&
        llam_join(driver) == 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        result = 0;
    } else {
        fail("select scratch storage/lifetime check failed");
    }
    llam_runtime_destroy(runtime);
    return result;
}

static void release_host_task(llam_task_t *task) {
    if (task != NULL) {
        llam_task_invalidate_public_handle(task);
        llam_task_allocator_free(task);
    }
}

static unsigned external_span_count(const llam_allocator_t *allocator) {
    unsigned count = 0U;
    for (const llam_alloc_chunk_t *span = allocator->chunks; span != NULL; span = span->next) {
        if (span->external_task_span) count += 1U;
    }
    return count;
}

static int test_external_span_bound_and_packing(void) {
    llam_runtime_t *runtime = NULL;
    llam_task_t *tasks[1024] = {0};
    llam_alloc_chunk_t *spans[1024] = {0};
    unsigned span_count = 0U;
    llam_shard_t *owner;
    int result = -1;

    llam_scheduler_clear_identity();
    if (create_runtime(0U, &runtime) != 0) return fail("external span runtime create failed");
    owner = &runtime->shards[0];
    for (unsigned i = 0U; i < 1024U; ++i) {
        tasks[i] = llam_task_alloc(owner);
        if (tasks[i] == NULL || tasks[i]->alloc_span == NULL ||
            !tasks[i]->alloc_span->external_task_span || tasks[i]->alloc_span->item_count > 16U) {
            fail("external span allocation/growth failed");
            goto cleanup;
        }
        bool found = false;
        for (unsigned j = 0U; j < span_count; ++j) {
            if (spans[j] == tasks[i]->alloc_span) found = true;
        }
        if (!found) spans[span_count++] = tasks[i]->alloc_span;
    }
    /* Keep one live object per span. Empty-slot availability must not trigger
     * more backing allocations, and live spans must never be reclaimed. */
    for (unsigned j = 0U; j < span_count; ++j) {
        bool keep = true;
        for (unsigned i = 0U; i < 1024U; ++i) {
            if (tasks[i] != NULL && tasks[i]->alloc_span == spans[j]) {
                if (keep) keep = false;
                else { release_host_task(tasks[i]); tasks[i] = NULL; }
            }
        }
    }
    {
        unsigned before = external_span_count(&owner->processor.allocator);
        llam_task_t *extra = llam_task_alloc(owner);
        bool reused_partial = false;
        if (extra == NULL) goto cleanup;
        for (unsigned j = 0U; j < span_count; ++j) {
            if (extra->alloc_span == spans[j]) reused_partial = true;
        }
        bool valid = reused_partial && external_span_count(&owner->processor.allocator) == before;
        release_host_task(extra);
        if (!valid) { fail("allocator did not prefer partial span capacity"); goto cleanup; }
    }
    for (unsigned i = 0U; i < 1024U; ++i) {
        release_host_task(tasks[i]); tasks[i] = NULL;
    }
    if (owner->processor.allocator.task_external_empty_count > 2U ||
        external_span_count(&owner->processor.allocator) > 2U ||
        owner->processor.allocator.task_external_retired != NULL) {
        fail("historical host burst remained in the allocator");
        goto cleanup;
    }
    result = 0;
cleanup:
    for (unsigned i = 0U; i < 1024U; ++i) release_host_task(tasks[i]);
    llam_runtime_destroy(runtime);
    return result;
}

static int test_external_span_pin_quarantine(void) {
    llam_runtime_t *runtime = NULL;
    llam_task_t *tasks[96] = {0};
    llam_task_t *pin = NULL;
    llam_task_t *extra = NULL;
    llam_alloc_chunk_t *pinned_span = NULL;
    llam_shard_t *owner;
    int result = -1;

    llam_scheduler_clear_identity();
    if (create_runtime(0U, &runtime) != 0) return fail("span pin runtime create failed");
    owner = &runtime->shards[0];
    for (unsigned i = 0U; i < 96U; ++i) {
        tasks[i] = llam_task_alloc(owner);
        if (tasks[i] == NULL) goto cleanup;
    }
    pin = llam_task_resolve_public_handle(llam_task_public_handle(tasks[0]));
    if (pin == NULL) goto cleanup;
    pinned_span = pin->alloc_span;
    for (unsigned i = 0U; i < 96U; ++i) {
        release_host_task(tasks[i]); tasks[i] = NULL;
    }
    if (owner->processor.allocator.task_external_retired != pinned_span ||
        llam_public_active_op_count(&pin->active_ops) != 1U) {
        fail("busy span was not retained in quarantine");
        goto cleanup;
    }
    extra = llam_task_alloc(owner);
    if (extra == NULL || extra->alloc_span == pinned_span) {
        fail("busy span storage was reused while pinned");
        goto cleanup;
    }
    release_host_task(extra); extra = NULL;
    llam_task_end_public_op(pin); pin = NULL;
    llam_allocator_quiescent(owner);
    extra = llam_task_alloc(owner);
    if (extra == NULL || owner->processor.allocator.task_external_retired != NULL) {
        fail("unpinned retired span did not finish collection");
        goto cleanup;
    }
    release_host_task(extra); extra = NULL;
    result = 0;
cleanup:
    if (pin != NULL) llam_task_end_public_op(pin);
    release_host_task(extra);
    for (unsigned i = 0U; i < 96U; ++i) release_host_task(tasks[i]);
    llam_runtime_destroy(runtime);
    return result;
}

static int test_external_span_allocation_failures(void) {
    for (unsigned stage = 1U; stage <= 3U; ++stage) {
        llam_runtime_t *runtime = NULL;
        llam_task_t *task;
        if (create_runtime(0U, &runtime) != 0) return -1;
        llam_shard_t *owner = &runtime->shards[0];
        llam_alloc_chunk_t *before = owner->processor.allocator.chunks;
        owner->processor.allocator.task_external_next_count = 16U;
        llam_task_span_test_fail_next(stage);
        errno = 0;
        task = llam_task_alloc(owner);
        int error = errno;
        bool clean = task == NULL && error == (stage == 3U ? EAGAIN : ENOMEM) &&
                     owner->processor.allocator.chunks == before &&
                     owner->processor.allocator.task_external_free == NULL;
        release_host_task(task);
        llam_task_span_test_fail_next(0U);
        task = llam_task_alloc(owner);
        if (task == NULL) clean = false;
        release_host_task(task);
        llam_runtime_destroy(runtime);
        if (!clean) return fail("span construction failure left partial storage/registration");
    }
    {
        llam_runtime_t *runtime = NULL;
        llam_task_t *first;
        llam_task_t *fallback;
        if (create_runtime(0U, &runtime) != 0) return -1;
        llam_shard_t *owner = &runtime->shards[0];
        first = llam_task_alloc(owner);
        if (first == NULL) { llam_runtime_destroy(runtime); return -1; }
        owner->processor.allocator.task_external_next_count = owner->processor.allocator.task_external_max_count;
        llam_task_span_test_fail_next(4U);
        fallback = llam_task_alloc(owner);
        bool valid = fallback != NULL && !fallback->alloc_span->mmapped;
        llam_task_span_test_fail_next(0U);
        release_host_task(first); release_host_task(fallback);
        llam_runtime_destroy(runtime);
        if (!valid) return fail("span mapping failure did not use tracked heap fallback");
    }
    return 0;
}

typedef struct span_thread_state {
    llam_shard_t *owner;
    atomic_uint failures;
} span_thread_state_t;

static void *span_host_thread(void *arg) {
    span_thread_state_t *state = arg;
    for (unsigned round = 0U; round < 128U; ++round) {
        llam_task_t *tasks[32] = {0};
        for (unsigned i = 0U; i < 32U; ++i) {
            tasks[i] = llam_task_alloc(state->owner);
            if (tasks[i] == NULL) {
                atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
                break;
            }
        }
        for (unsigned i = 0U; i < 32U; ++i) release_host_task(tasks[i]);
    }
    return NULL;
}

static int test_external_span_concurrent_hosts(void) {
    llam_runtime_t *runtime = NULL;
    span_thread_state_t state = {0};
    pthread_t threads[4];
    unsigned started = 0U;
    int result = -1;
    llam_scheduler_clear_identity();
    if (create_runtime(0U, &runtime) != 0) return -1;
    state.owner = &runtime->shards[0];
    atomic_init(&state.failures, 0U);
    for (unsigned i = 0U; i < 4U; ++i) {
        if (pthread_create(&threads[i], NULL, span_host_thread, &state) != 0) break;
        started += 1U;
    }
    for (unsigned i = 0U; i < started; ++i) pthread_join(threads[i], NULL);
    /* Last registry contention can leave a retired span. A fresh operation
     * drains one quarantined span at a time after all host readers have ended. */
    for (unsigned i = 0U; i < 1024U && state.owner->processor.allocator.task_external_retired != NULL; ++i) {
        llam_allocator_quiescent(state.owner);
    }
    if (started == 4U && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U &&
        state.owner->processor.allocator.task_external_empty_count <= 2U &&
        state.owner->processor.allocator.task_external_retired == NULL) {
        result = 0;
    } else fail("concurrent host span recycle/collection failed");
    llam_runtime_destroy(runtime);
    return result;
}

int main(void) {
    int failures = 0;
    int rc;

    rc = test_io_request_uses_processor_route_and_allocator(0U);
    if (rc != 0 && rc != 77) {
        failures += 1;
    }
    rc = test_io_request_uses_processor_route_and_allocator(
        LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS);
    if (rc != 0 && rc != 77) {
        failures += 1;
    }
    rc = test_io_route_tracks_logical_owner_and_attached_node(0U);
    if (rc != 0 && rc != 77) {
        failures += 1;
    }
    rc = test_io_route_tracks_logical_owner_and_attached_node(
        LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS);
    if (rc != 0 && rc != 77) {
        failures += 1;
    }
    if (test_same_id_cross_runtime_remote_free() != 0) {
        failures += 1;
    }
    if (test_partial_init_drains_p_stack_caches() != 0) {
        failures += 1;
    }
    if (test_buffer_slab_stride_and_remote_reuse() != 0 ||
        test_cold_stack_cache_reuse() != 0 ||
        test_select_frame_storage_and_lifetime() != 0) {
        failures += 1;
    }
    rc = test_stack_cache_tracks_home_processor();
    if (rc != 0 && rc != 77) {
        failures += 1;
    }
    if (test_external_span_bound_and_packing() != 0 ||
        test_external_span_pin_quarantine() != 0 ||
        test_external_span_allocation_failures() != 0 ||
        test_external_span_concurrent_hosts() != 0) {
        failures += 1;
    }
    if (failures != 0) {
        return 1;
    }
    puts("test_processor_allocator_io_internal: ownership routing checks passed");
    return 0;
}
