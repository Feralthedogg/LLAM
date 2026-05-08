/**
 * @file src/core/runtime_windows.c
 * @brief Windows 10/11 detection and IOCP policy selection.
 *
 * @details
 * IOCP is the native Windows completion backend for LLAM. Windows 10 and
 * Windows 11 expose the same required primitives, so this module classifies the
 * OS generation only to choose conservative tuning defaults. Correctness must
 * never depend on the generation branch.
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

#include "runtime_windows.h"

#include "llam/platform.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if LLAM_PLATFORM_WINDOWS
#include <windows.h>

typedef LONG(WINAPI *llam_rtl_get_version_fn)(PRTL_OSVERSIONINFOW);
#endif

/** @brief Classify a Windows NT version into LLAM's supported generation set. */
uint32_t llam_windows_generation_from_version(uint32_t major, uint32_t minor, uint32_t build) {
    if (major != 10U || minor != 0U) {
        return LLAM_WINDOWS_GENERATION_UNSUPPORTED;
    }
    return build >= LLAM_WINDOWS_11_MIN_BUILD ? LLAM_WINDOWS_GENERATION_11 : LLAM_WINDOWS_GENERATION_10;
}

/** @brief Return a stable name for a Windows generation enum value. */
const char *llam_windows_generation_name(uint32_t generation) {
    switch (generation) {
    case LLAM_WINDOWS_GENERATION_10:
        return "windows10";
    case LLAM_WINDOWS_GENERATION_11:
        return "windows11";
    default:
        return "unsupported";
    }
}

/** @brief Return a stable name for a Windows IOCP strategy enum value. */
const char *llam_windows_iocp_strategy_name(uint32_t strategy) {
    switch (strategy) {
    case LLAM_WINDOWS_IOCP_STRATEGY_WIN10_CONSERVATIVE:
        return "win10-conservative";
    case LLAM_WINDOWS_IOCP_STRATEGY_WIN11_BATCHED:
        return "win11-batched";
    default:
        return "unsupported";
    }
}

#if LLAM_PLATFORM_WINDOWS
/** @brief Optionally force a generation for cross-version validation on one host. */
static uint32_t llam_windows_forced_generation(uint32_t detected) {
    const char *value = getenv("LLAM_WINDOWS_FORCE_GENERATION");

    if (value == NULL || value[0] == '\0') {
        return detected;
    }
    if (strcmp(value, "10") == 0 || strcmp(value, "windows10") == 0 || strcmp(value, "win10") == 0) {
        return LLAM_WINDOWS_GENERATION_10;
    }
    if (strcmp(value, "11") == 0 || strcmp(value, "windows11") == 0 || strcmp(value, "win11") == 0) {
        return LLAM_WINDOWS_GENERATION_11;
    }
    return detected;
}
#endif

/** @brief Initialize policy fields shared by all supported Windows generations. */
static void llam_windows_iocp_policy_init_common(uint32_t generation,
                                                 uint32_t strategy,
                                                 uint32_t major,
                                                 uint32_t minor,
                                                 uint32_t build,
                                                 uint32_t processor_count,
                                                 llam_windows_iocp_policy_t *policy) {
    uint32_t cpus = processor_count != 0U ? processor_count : 1U;

    memset(policy, 0, sizeof(*policy));
    policy->generation = generation;
    policy->strategy = strategy;
    policy->major = major;
    policy->minor = minor;
    policy->build = build;
    policy->processor_count = cpus;
    policy->use_gqcs_ex = 1U;
}

/**
 * @brief Fill the conservative Windows 10 IOCP policy.
 *
 * Windows 10 is fully supported by IOCP but defaults are intentionally less
 * aggressive: smaller completion/control batches, fewer preposted accepts, a
 * longer blocking timeout, and no skip-completion mode by default.
 */
void llam_windows_iocp_policy_for_windows10(uint32_t major,
                                            uint32_t minor,
                                            uint32_t build,
                                            uint32_t processor_count,
                                            llam_windows_iocp_policy_t *policy) {
    uint32_t cpus = processor_count != 0U ? processor_count : 1U;

    if (policy == NULL) {
        return;
    }
    llam_windows_iocp_policy_init_common(LLAM_WINDOWS_GENERATION_10,
                                         LLAM_WINDOWS_IOCP_STRATEGY_WIN10_CONSERVATIVE,
                                         major,
                                         minor,
                                         build,
                                         cpus,
                                         policy);
    policy->iocp_concurrency = cpus > 1U ? cpus - 1U : 1U;
    policy->completion_batch = 64U;
    policy->control_batch = 16U;
    policy->accept_prepost = cpus >= 8U ? 2U : 1U;
    policy->recv_prepost = cpus >= 8U ? 16U : 8U;
    policy->idle_spin_iters = cpus >= 8U ? 64U : 32U;
    policy->poll_timeout_ms = 10U;
    policy->timer_granularity_ms = 10U;
    policy->use_skip_completion_on_success = 0U;
}

/**
 * @brief Fill the batched Windows 11 IOCP policy.
 *
 * Windows 11 is still the same IOCP correctness backend, but LLAM assumes newer
 * scheduler/networking behavior and chooses larger batches, more preposted I/O,
 * shorter blocking timeouts, and skip-completion mode for synchronous success.
 */
void llam_windows_iocp_policy_for_windows11(uint32_t major,
                                            uint32_t minor,
                                            uint32_t build,
                                            uint32_t processor_count,
                                            llam_windows_iocp_policy_t *policy) {
    uint32_t cpus = processor_count != 0U ? processor_count : 1U;

    if (policy == NULL) {
        return;
    }
    llam_windows_iocp_policy_init_common(LLAM_WINDOWS_GENERATION_11,
                                         LLAM_WINDOWS_IOCP_STRATEGY_WIN11_BATCHED,
                                         major,
                                         minor,
                                         build,
                                         cpus,
                                         policy);
    policy->iocp_concurrency = cpus;
    policy->completion_batch = cpus >= 16U ? 128U : 64U;
    policy->control_batch = 32U;
    policy->accept_prepost = cpus >= 8U ? 4U : 2U;
    policy->recv_prepost = cpus >= 8U ? 32U : 16U;
    policy->idle_spin_iters = cpus >= 8U ? 128U : 64U;
    policy->poll_timeout_ms = 5U;
    policy->timer_granularity_ms = 1U;
    policy->use_skip_completion_on_success = 1U;
}

/**
 * @brief Fill an IOCP policy from already-known OS and CPU facts.
 *
 * @param generation      Windows generation enum.
 * @param major           NT major version.
 * @param minor           NT minor version.
 * @param build           NT build number.
 * @param processor_count Logical processor count visible to the process.
 * @param policy          Output policy.
 */
void llam_windows_default_iocp_policy(uint32_t generation,
                                      uint32_t major,
                                      uint32_t minor,
                                      uint32_t build,
                                      uint32_t processor_count,
                                      llam_windows_iocp_policy_t *policy) {
    if (policy == NULL) {
        return;
    }

    if (generation == LLAM_WINDOWS_GENERATION_11) {
        llam_windows_iocp_policy_for_windows11(major, minor, build, processor_count, policy);
        return;
    }

    if (generation == LLAM_WINDOWS_GENERATION_10) {
        llam_windows_iocp_policy_for_windows10(major, minor, build, processor_count, policy);
        return;
    }

    memset(policy, 0, sizeof(*policy));
    policy->generation = LLAM_WINDOWS_GENERATION_UNSUPPORTED;
    policy->strategy = LLAM_WINDOWS_IOCP_STRATEGY_UNSUPPORTED;
    policy->major = major;
    policy->minor = minor;
    policy->build = build;
    policy->processor_count = processor_count != 0U ? processor_count : 1U;
    policy->iocp_concurrency = 1U;
    policy->completion_batch = 16U;
    policy->control_batch = 4U;
    policy->accept_prepost = 0U;
    policy->recv_prepost = 0U;
    policy->idle_spin_iters = 0U;
    policy->poll_timeout_ms = 10U;
    policy->timer_granularity_ms = 15U;
    policy->use_gqcs_ex = 0U;
    policy->use_skip_completion_on_success = 0U;
}

/**
 * @brief Detect Windows generation and derive LLAM's IOCP policy.
 *
 * @param policy Receives the detected policy.
 *
 * @return 0 on Windows 10/11, or -1 with @c errno set.
 */
int llam_windows_detect_iocp_policy(llam_windows_iocp_policy_t *policy) {
#if LLAM_PLATFORM_WINDOWS
    HMODULE ntdll;
    llam_rtl_get_version_fn rtl_get_version;
    RTL_OSVERSIONINFOW version;
    SYSTEM_INFO system_info;
    uint32_t generation;
    uint32_t processor_count;

    if (policy == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(&version, 0, sizeof(version));
    version.dwOSVersionInfoSize = sizeof(version);
    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == NULL) {
        errno = ENOSYS;
        return -1;
    }
    {
        FARPROC proc = GetProcAddress(ntdll, "RtlGetVersion");

        memset(&rtl_get_version, 0, sizeof(rtl_get_version));
        memcpy(&rtl_get_version, &proc, sizeof(rtl_get_version) < sizeof(proc) ? sizeof(rtl_get_version) : sizeof(proc));
    }
    if (rtl_get_version == NULL || rtl_get_version(&version) != 0) {
        errno = ENOSYS;
        return -1;
    }
    if (version.dwMajorVersion != 10U || version.dwMinorVersion != 0U) {
        errno = ENOTSUP;
        return -1;
    }

    memset(&system_info, 0, sizeof(system_info));
    GetNativeSystemInfo(&system_info);
    processor_count = system_info.dwNumberOfProcessors != 0U ? system_info.dwNumberOfProcessors : 1U;
    generation = llam_windows_generation_from_version(version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
    if (generation == LLAM_WINDOWS_GENERATION_UNSUPPORTED) {
        errno = ENOTSUP;
        return -1;
    }
    generation = llam_windows_forced_generation(generation);
    llam_windows_default_iocp_policy(generation,
                                     version.dwMajorVersion,
                                     version.dwMinorVersion,
                                     version.dwBuildNumber,
                                     processor_count,
                                     policy);
    return 0;
#else
    if (policy != NULL) {
        memset(policy, 0, sizeof(*policy));
    }
    errno = ENOTSUP;
    return -1;
#endif
}
