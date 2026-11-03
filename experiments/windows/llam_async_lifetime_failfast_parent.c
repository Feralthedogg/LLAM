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

/* Parent verifies the production fail-fast branch in bounded child processes. */
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LLAM_WINDOWS_STATUS_FAIL_FAST_EXCEPTION UINT32_C(0xC0000602)

static int run_case(const char *program, const char *mode, DWORD expected) {
    STARTUPINFOA start;
    PROCESS_INFORMATION process;
    DWORD wait_result;
    DWORD exit_code = 0U;

    memset(&start, 0, sizeof(start));
    memset(&process, 0, sizeof(process));
    start.cb = sizeof(start);
    if (mode != NULL &&
        !SetEnvironmentVariableA("LLAM_LIFETIME_UNCERTAIN_MODE", mode)) {
        return -1;
    }
    if (mode == NULL) {
        (void)SetEnvironmentVariableA("LLAM_LIFETIME_UNCERTAIN_MODE", NULL);
    }
    if (!CreateProcessA(program, NULL, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &start, &process)) {
        (void)SetEnvironmentVariableA("LLAM_LIFETIME_UNCERTAIN_MODE", NULL);
        return -1;
    }
    (void)SetEnvironmentVariableA("LLAM_LIFETIME_UNCERTAIN_MODE", NULL);
    wait_result = WaitForSingleObject(process.hProcess, 5000U);
    if (wait_result != WAIT_OBJECT_0) {
        /* Timeout is a failure, never evidence for a terminal contract. */
        (void)TerminateProcess(process.hProcess, 1U);
        (void)WaitForSingleObject(process.hProcess, 1000U);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        fprintf(stderr, "llam-async-failfast: child mode=%s timed out\n",
                mode != NULL ? mode : "control");
        return -1;
    }
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return -1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (mode == NULL && exit_code == 77U) {
        return 77;
    }
    if (exit_code != expected) {
        fprintf(stderr,
                "llam-async-failfast: mode=%s exit=0x%08lx expected=0x%08lx\n",
                mode != NULL ? mode : "control",
                (unsigned long)exit_code, (unsigned long)expected);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int control_result;

    if (argc != 2) {
        return 1;
    }
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    control_result = run_case(argv[1], NULL, 0U);
    if (control_result == 77) {
        return 77;
    }
    if (control_result != 0 ||
        run_case(argv[1], "capture",
                 LLAM_WINDOWS_STATUS_FAIL_FAST_EXCEPTION) != 0 ||
        run_case(argv[1], "restore",
                 LLAM_WINDOWS_STATUS_FAIL_FAST_EXCEPTION) != 0) {
        return 1;
    }
    puts("llam-async-failfast: capture/restore terminal, refusal control alive");
    return 0;
}
