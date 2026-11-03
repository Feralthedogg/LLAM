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

/* Fault the native completion boundary, not LLAM's scheduler. Production
 * pthread_join must never convert an unobserved thread exit into success. */
#include <winsock2.h>
#include <windows.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

static DWORD wait_result;
static BOOL close_result;
static unsigned wait_calls;
static unsigned close_calls;
static HANDLE expected_handle;

static DWORD WINAPI join_test_wait(HANDLE handle, DWORD timeout) {
    if (handle != expected_handle || timeout != INFINITE) return WAIT_FAILED;
    ++wait_calls;
    return wait_result;
}
static BOOL WINAPI join_test_close(HANDLE handle) {
    if (handle != expected_handle) return FALSE;
    ++close_calls;
    return close_result;
}
#define WaitForSingleObject join_test_wait
#define CloseHandle join_test_close
#include "runtime_internal.h"
#undef CloseHandle
#undef WaitForSingleObject

int main(void) {
    const DWORD failures[] = {WAIT_FAILED, WAIT_TIMEOUT, WAIT_ABANDONED};
    expected_handle = (HANDLE)(uintptr_t)0x12340U;
    for (size_t i = 0U; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        wait_calls = close_calls = 0U;
        wait_result = failures[i];
        close_result = TRUE;
        void *result = expected_handle;
        if (pthread_join((pthread_t)(uintptr_t)expected_handle, &result) != EIO ||
            result != NULL || wait_calls != 1U || close_calls != 0U) return 1;
    }
    wait_calls = close_calls = 0U;
    if (pthread_join((pthread_t)0U, NULL) != EINVAL || wait_calls != 0U || close_calls != 0U) return 1;
    wait_result = WAIT_OBJECT_0;
    close_result = FALSE;
    if (pthread_join((pthread_t)(uintptr_t)expected_handle, NULL) != EIO || close_calls != 1U) return 1;
    close_result = TRUE;
    if (pthread_join((pthread_t)(uintptr_t)expected_handle, NULL) != 0 || close_calls != 2U) return 1;
    puts("Windows join: wait failures retain handle; only observed thread exit and owned-handle close succeed");
    return 0;
}
