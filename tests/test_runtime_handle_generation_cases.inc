/**
 * @file tests/test_runtime_handle_generation_cases.inc
 * @brief Explicit-runtime token generation and stale-handle regression checks.
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

static int test_runtime_handle_generation_contract(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *first = NULL;
    llam_runtime_t *first_raw = NULL;
    llam_runtime_t *wrong_raw = NULL;
    llam_runtime_t *second = NULL;
    size_t first_slot = SIZE_MAX;
    size_t second_slot = SIZE_MAX;
    uint32_t first_generation = 0U;
    uint32_t second_generation = 0U;
    uint32_t wrong_family_generation;
    int rc = 1;

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime generation opts init failed");
    }
    if (llam_runtime_create(&opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &first) != 0) {
        return test_fail_errno("first runtime generation create failed");
    }
    if (llam_runtime_begin_public_op(first, &first_raw) != 0) {
        rc = test_fail_errno("first runtime token resolve failed");
        goto cleanup;
    }
    if (first == first_raw ||
        !llam_public_slot_decode_handle(
            (uintptr_t)first,
            LLAM_RUNTIME_PUBLIC_HANDLE_SHIFT,
            &first_slot,
            &first_generation) ||
        (first_generation & LLAM_PUBLIC_HANDLE_FAMILY_MASK) !=
            LLAM_PUBLIC_HANDLE_FAMILY_RUNTIME) {
        llam_runtime_end_public_op(first_raw);
        first_raw = NULL;
        rc = test_fail("explicit runtime handle was not a family-tagged token");
        goto cleanup;
    }
    llam_runtime_end_public_op(first_raw);
    first_raw = NULL;
    wrong_family_generation =
        (first_generation & ~LLAM_PUBLIC_HANDLE_FAMILY_MASK) |
        LLAM_PUBLIC_HANDLE_FAMILY_MUTEX;
    errno = 0;
    if (llam_runtime_begin_public_op(
            (llam_runtime_t *)llam_public_slot_encode_handle(
                first_slot,
                wrong_family_generation,
                LLAM_RUNTIME_PUBLIC_HANDLE_SHIFT),
            &wrong_raw) == 0 ||
        errno != EINVAL) {
        if (wrong_raw != NULL) {
            llam_runtime_end_public_op(wrong_raw);
        }
        wrong_raw = NULL;
        rc = test_fail_errno("runtime resolver accepted a wrong-family token");
        goto cleanup;
    }
    if (llam_runtime_begin_public_op(first, &first_raw) != 0) {
        rc = test_fail_errno("runtime token did not resolve after family rejection");
        goto cleanup;
    }
    llam_runtime_end_public_op(first_raw);
    first_raw = NULL;

    llam_runtime_destroy(first);
    errno = 0;
    if (llam_runtime_begin_public_op(first, &first_raw) == 0 ||
        errno != EINVAL) {
        if (first_raw != NULL) {
            llam_runtime_end_public_op(first_raw);
            first_raw = NULL;
        }
        rc = test_fail_errno("stale runtime token resolved after destroy");
        first = NULL;
        goto cleanup;
    }
    first = NULL;

    if (llam_runtime_create(&opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &second) != 0) {
        return test_fail_errno("second runtime generation create failed");
    }
    if (!llam_public_slot_decode_handle(
            (uintptr_t)second,
            LLAM_RUNTIME_PUBLIC_HANDLE_SHIFT,
            &second_slot,
            &second_generation) ||
        second_slot != first_slot ||
        second_generation == first_generation ||
        second == (llam_runtime_t *)llam_public_slot_encode_handle(
                      first_slot,
                      first_generation,
                      LLAM_RUNTIME_PUBLIC_HANDLE_SHIFT)) {
        rc = test_fail("runtime slot reuse did not advance the public generation");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (wrong_raw != NULL) {
        llam_runtime_end_public_op(wrong_raw);
    }
    if (first_raw != NULL) {
        llam_runtime_end_public_op(first_raw);
    }
    if (second != NULL) {
        llam_runtime_destroy(second);
    }
    if (first != NULL) {
        llam_runtime_destroy(first);
    }
    return rc;
}

static int test_runtime_finalize_preserves_public_owner_refs(void) {
    llam_runtime_t *runtime;
    llam_runtime_t *handle;
    llam_runtime_t *claimed_runtime = NULL;
    uint64_t owner_secret;
    bool heap_allocated = false;

    runtime = llam_aligned_zalloc(
        LLAM_CACHELINE_BYTES, 1U, sizeof(*runtime));
    if (runtime == NULL) {
        return test_fail_errno(
            "runtime finalize fixture allocation failed");
    }
    if (llam_runtime_register_handle(runtime, true) != 0) {
        llam_aligned_free(runtime);
        return test_fail_errno(
            "runtime finalize fixture registration failed");
    }
    handle = llam_runtime_public_handle(runtime);
    owner_secret = llam_runtime_public_owner_secret(runtime);
    if (handle == NULL ||
        owner_secret == 0U ||
        llam_runtime_public_owner_acquire(runtime) != 0 ||
        llam_runtime_public_owner_acquire(runtime) != 0) {
        llam_runtime_unregister_handle(runtime);
        llam_aligned_free(runtime);
        return test_fail_errno(
            "runtime finalize fixture owner pin failed");
    }
    if (llam_runtime_claim_destroy_handle(
            handle, &claimed_runtime, &heap_allocated) != 0 ||
        claimed_runtime != runtime ||
        !heap_allocated) {
        llam_runtime_public_owner_release(runtime);
        llam_runtime_public_owner_release(runtime);
        llam_runtime_unregister_handle(runtime);
        llam_aligned_free(runtime);
        return test_fail_errno(
            "runtime finalize fixture destroy claim failed");
    }

    /*
     * Finalization must publish the raw storage in the retired registry in the
     * same critical section that removes the live token. Otherwise a concurrent
     * public-object release can fall into an unregistered/unretired gap and
     * silently lose its owner-reference decrement.
     */
    llam_runtime_finalize_handle(runtime, true);
    if (runtime->public_owner_refs != 2U ||
        !runtime->retired_storage ||
        llam_runtime_public_owner_secret(runtime) != owner_secret ||
        llam_runtime_check_handle(handle) == 0) {
        llam_runtime_public_owner_release(runtime);
        llam_runtime_public_owner_release(runtime);
        return test_fail(
            "runtime finalization did not preserve the retired owner pins");
    }
    llam_runtime_public_owner_release(runtime);
    if (runtime->public_owner_refs != 1U ||
        !runtime->retired_storage) {
        llam_runtime_public_owner_release(runtime);
        return test_fail(
            "runtime retired owner release lost its reference accounting");
    }
    /*
     * The final release consumes the retired storage. Do not touch runtime
     * after this point.
     */
    llam_runtime_public_owner_release(runtime);
    return 0;
}
