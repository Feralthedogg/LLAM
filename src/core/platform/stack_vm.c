/**
 * @file src/core/platform/stack_vm.c
 * @brief Guarded fiber-stack virtual-memory operations.
 *
 * @details
 * Stack allocation and cache policy use this boundary instead of issuing
 * platform VM calls directly. Windows keeps a reservation while cached usable
 * pages may be decommitted; POSIX platforms retain the mapping and discard
 * usable pages with madvise. Resident-byte sampling is explicitly optional.
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

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_int
    g_llam_stack_vm_test_errors[LLAM_TEST_STACK_VM_OPERATION_COUNT];
static atomic_uint
    g_llam_stack_vm_test_calls[LLAM_TEST_STACK_VM_OPERATION_COUNT];
static llam_test_stack_vm_observer_fn g_llam_stack_vm_test_observer;
static void *g_llam_stack_vm_test_observer_context;

void llam_runtime_test_reset_stack_vm_hooks(void) {
    for (unsigned operation = 0U;
         operation < LLAM_TEST_STACK_VM_OPERATION_COUNT;
         ++operation) {
        atomic_store_explicit(&g_llam_stack_vm_test_errors[operation],
                              0,
                              memory_order_release);
        atomic_store_explicit(&g_llam_stack_vm_test_calls[operation],
                              0U,
                              memory_order_release);
    }
    g_llam_stack_vm_test_observer = NULL;
    g_llam_stack_vm_test_observer_context = NULL;
}

void llam_runtime_test_set_stack_vm_error(
    llam_test_stack_vm_operation_t operation,
    int error_code) {
    if ((unsigned)operation >= LLAM_TEST_STACK_VM_OPERATION_COUNT) {
        return;
    }
    atomic_store_explicit(&g_llam_stack_vm_test_errors[operation],
                          error_code,
                          memory_order_release);
}

void llam_runtime_test_set_stack_vm_observer(
    llam_test_stack_vm_observer_fn observer,
    void *context) {
    g_llam_stack_vm_test_observer_context = context;
    g_llam_stack_vm_test_observer = observer;
}

unsigned llam_runtime_test_stack_vm_calls(
    llam_test_stack_vm_operation_t operation) {
    if ((unsigned)operation >= LLAM_TEST_STACK_VM_OPERATION_COUNT) {
        return 0U;
    }
    return atomic_load_explicit(&g_llam_stack_vm_test_calls[operation],
                                memory_order_acquire);
}

static bool llam_stack_vm_test_should_fail(
    llam_test_stack_vm_operation_t operation) {
    int error_code;

    atomic_fetch_add_explicit(&g_llam_stack_vm_test_calls[operation],
                              1U,
                              memory_order_relaxed);
    if (g_llam_stack_vm_test_observer != NULL) {
        g_llam_stack_vm_test_observer(
            operation,
            g_llam_stack_vm_test_observer_context);
    }
    error_code =
        atomic_load_explicit(&g_llam_stack_vm_test_errors[operation],
                             memory_order_acquire);
    if (error_code == 0) {
        return false;
    }
    errno = error_code;
    return true;
}
#endif

/**
 * @brief Validate an aligned usable-stack page range.
 */
static int llam_stack_vm_validate_range(void *stack_base, size_t stack_size) {
    long raw_page_size = llam_page_size();
    size_t page_size;

    if (raw_page_size <= 0) {
        errno = EINVAL;
        return -1;
    }
    page_size = (size_t)raw_page_size;
    if (stack_base == NULL || stack_size == 0U ||
        (uintptr_t)stack_base % page_size != 0U ||
        stack_size % page_size != 0U) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int llam_stack_vm_map(size_t stack_size,
                      void **mapping_out,
                      size_t *mapping_size_out,
                      void **stack_base_out) {
    long raw_page_size = llam_page_size();
    size_t page_size;
    size_t mapping_size;
    void *mapping;
    void *stack_base;

    if (mapping_out != NULL) {
        *mapping_out = NULL;
    }
    if (mapping_size_out != NULL) {
        *mapping_size_out = 0U;
    }
    if (stack_base_out != NULL) {
        *stack_base_out = NULL;
    }
    if (mapping_out == NULL || mapping_size_out == NULL ||
        stack_base_out == NULL || raw_page_size <= 0) {
        errno = EINVAL;
        return -1;
    }
    page_size = (size_t)raw_page_size;
    if (stack_size == 0U || stack_size % page_size != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (stack_size > SIZE_MAX - page_size) {
        errno = EOVERFLOW;
        return -1;
    }
    mapping_size = stack_size + page_size;

#if LLAM_RUNTIME_BACKEND_WINDOWS
    mapping = VirtualAlloc(NULL, mapping_size, MEM_RESERVE, PAGE_NOACCESS);
    if (mapping == NULL) {
        errno = llam_windows_system_error_to_errno(GetLastError());
        return -1;
    }
    stack_base = (unsigned char *)mapping + page_size;
    if (VirtualAlloc(stack_base,
                     stack_size,
                     MEM_COMMIT,
                     PAGE_READWRITE) != stack_base) {
        int saved_errno =
            llam_windows_system_error_to_errno(GetLastError());

        llam_stack_mapping_release_or_quarantine(NULL,
                                                 mapping,
                                                 mapping_size);
        errno = saved_errno;
        return -1;
    }
#else
    mapping = mmap(NULL,
                   mapping_size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                   -1,
                   0);
    if (mapping == MAP_FAILED) {
        return -1;
    }
    if (mprotect(mapping, page_size, PROT_NONE) != 0) {
        int saved_errno = errno;

        llam_stack_mapping_release_or_quarantine(NULL,
                                                 mapping,
                                                 mapping_size);
        errno = saved_errno;
        return -1;
    }
    stack_base = (unsigned char *)mapping + page_size;
#endif

    *mapping_out = mapping;
    *mapping_size_out = mapping_size;
    *stack_base_out = stack_base;
    return 0;
}

int llam_stack_vm_release(void *mapping, size_t mapping_size) {
    if (mapping == NULL || mapping == MAP_FAILED || mapping_size == 0U) {
        errno = EINVAL;
        return -1;
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (llam_stack_vm_test_should_fail(LLAM_TEST_STACK_VM_RELEASE)) {
        return -1;
    }
#endif
#if LLAM_RUNTIME_BACKEND_WINDOWS
    if (!VirtualFree(mapping, 0U, MEM_RELEASE)) {
        errno = llam_windows_system_error_to_errno(GetLastError());
        return -1;
    }
    return 0;
#else
    return munmap(mapping, mapping_size);
#endif
}

int llam_stack_vm_discard(void *stack_base, size_t stack_size) {
    if (llam_stack_vm_validate_range(stack_base, stack_size) != 0) {
        return -1;
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (llam_stack_vm_test_should_fail(LLAM_TEST_STACK_VM_DISCARD)) {
        return -1;
    }
#endif
#if LLAM_RUNTIME_BACKEND_WINDOWS
    if (!VirtualFree(stack_base, stack_size, MEM_DECOMMIT)) {
        errno = llam_windows_system_error_to_errno(GetLastError());
        return -1;
    }
    return 0;
#elif defined(MADV_DONTNEED)
    return madvise(stack_base, stack_size, MADV_DONTNEED);
#elif defined(POSIX_MADV_DONTNEED)
    {
        int error_code =
            posix_madvise(stack_base, stack_size, POSIX_MADV_DONTNEED);

        if (error_code != 0) {
            errno = error_code;
            return -1;
        }
        return 0;
    }
#else
    errno = ENOTSUP;
    return -1;
#endif
}

int llam_stack_vm_reactivate(void *stack_base, size_t stack_size) {
    if (llam_stack_vm_validate_range(stack_base, stack_size) != 0) {
        return -1;
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (llam_stack_vm_test_should_fail(LLAM_TEST_STACK_VM_REACTIVATE)) {
        return -1;
    }
#endif
#if LLAM_RUNTIME_BACKEND_WINDOWS
    if (VirtualAlloc(stack_base,
                     stack_size,
                     MEM_COMMIT,
                     PAGE_READWRITE) != stack_base) {
        errno = llam_windows_system_error_to_errno(GetLastError());
        return -1;
    }
#endif
    return 0;
}

int llam_stack_vm_secure_zero(void *stack_base, size_t stack_size) {
    if (llam_stack_vm_validate_range(stack_base, stack_size) != 0) {
        return -1;
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (llam_stack_vm_test_should_fail(LLAM_TEST_STACK_VM_SCRUB)) {
        return -1;
    }
#endif
#if LLAM_RUNTIME_BACKEND_WINDOWS
    SecureZeroMemory(stack_base, stack_size);
#else
    {
        volatile unsigned char *bytes = stack_base;

        for (size_t index = 0U; index < stack_size; ++index) {
            bytes[index] = 0U;
        }
    }
#endif
    return 0;
}

int llam_stack_vm_sample_resident(void *stack_base,
                                  size_t stack_size,
                                  uint64_t *resident_bytes_out,
                                  bool *valid_out) {
    if (resident_bytes_out != NULL) {
        *resident_bytes_out = 0U;
    }
    if (valid_out != NULL) {
        *valid_out = false;
    }
    if (resident_bytes_out == NULL || valid_out == NULL ||
        llam_stack_vm_validate_range(stack_base, stack_size) != 0) {
        errno = EINVAL;
        return -1;
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (llam_stack_vm_test_should_fail(
            LLAM_TEST_STACK_VM_RESIDENT_SAMPLE)) {
        return -1;
    }
#endif
#if LLAM_RUNTIME_BACKEND_WINDOWS
    return 0;
#else
    size_t page_size = (size_t)llam_page_size();
    size_t page_count = stack_size / page_size;
    uint64_t resident_bytes = 0U;

#if LLAM_RUNTIME_BACKEND_LINUX
    {
        unsigned char *residency = calloc(page_count, sizeof(*residency));

        if (residency == NULL) {
            return -1;
        }
        if (mincore(stack_base, stack_size, residency) != 0) {
            free(residency);
            return 0;
        }
        for (size_t index = 0U; index < page_count; ++index) {
            if ((residency[index] & 1U) != 0U) {
                resident_bytes += page_size;
            }
        }
        free(residency);
    }
#else
    {
        char *residency = calloc(page_count, sizeof(*residency));

        if (residency == NULL) {
            return -1;
        }
        if (mincore(stack_base, stack_size, residency) != 0) {
            free(residency);
            return 0;
        }
        for (size_t index = 0U; index < page_count; ++index) {
            if (((unsigned char)residency[index] & 1U) != 0U) {
                resident_bytes += page_size;
            }
        }
        free(residency);
    }
#endif
    *resident_bytes_out = resident_bytes;
    *valid_out = true;
    return 0;
#endif
}
