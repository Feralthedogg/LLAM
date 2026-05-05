/**
 * @file src/core/runtime_fp.c
 * @brief Floating-point context preservation and optional FP control policy.
 *
 * @details
 * Context switches must preserve the floating-point control environment used by
 * user code. x86-64 can optionally allocate XSAVE areas for extended CPU state;
 * AArch64 stores FPCR/FPSR directly in the runtime context.
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

/**
 * @brief Clear cached global extended-context capability flags.
 */
void nm_clear_xsave_globals(void) {
    g_nm_xsave_mask_lo = 0U;
    g_nm_xsave_mask_hi = 0U;
    g_nm_fp_control_context = 0U;
}

#if defined(__linux__) && defined(__x86_64__)
/** @brief Read the current SSE MXCSR control/status register. */
static uint32_t nm_current_mxcsr(void) {
    uint32_t value;

    __asm__ volatile("stmxcsr %0" : "=m"(value));
    return value;
}

/** @brief Read the current x87 control word. */
static uint16_t nm_current_x87_cw(void) {
    uint16_t value;

    __asm__ volatile("fnstcw %0" : "=m"(value));
    return value;
}

/** @brief Execute xgetbv for the given extended-control register index. */
static uint64_t nm_xgetbv(uint32_t index) {
    uint32_t eax;
    uint32_t edx;

    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
    return ((uint64_t)edx << 32U) | (uint64_t)eax;
}

/** @brief Save extended CPU state into an aligned XSAVE area. */
static void nm_save_xsave_area(void *area, uint64_t mask) {
    uint32_t eax = (uint32_t)mask;
    uint32_t edx = (uint32_t)(mask >> 32U);

    __asm__ volatile("xsave64 (%0)" : : "r"(area), "a"(eax), "d"(edx) : "memory");
}

/**
 * @brief Detect x86-64 XSAVE support and configure runtime FP context policy.
 *
 * @param rt Runtime to update.
 *
 * @return Always 0; unsupported features simply leave XSAVE disabled.
 */
int nm_detect_xsave_support(nm_runtime_t *rt) {
    const char *xsave_env;
    const char *fp_env;
    unsigned max_leaf;
    unsigned eax;
    unsigned ebx;
    unsigned ecx;
    unsigned edx;
    uint64_t supported_mask;
    uint64_t xcr0;

    nm_clear_xsave_globals();
    if (rt == NULL) {
        return 0;
    }

    fp_env = nm_env_get("LLAM_FP_CONTROL_CONTEXT");
    g_nm_fp_control_context =
        (fp_env == NULL || fp_env[0] == '\0' || strcmp(fp_env, "0") != 0) ? 1U : 0U;

    xsave_env = nm_env_get("LLAM_XSAVE_CONTEXT");
    // XSAVE is opt-in because it increases per-context allocation and switch cost.
    if (xsave_env == NULL || xsave_env[0] == '\0' || strcmp(xsave_env, "0") == 0) {
        rt->xsave_enabled = false;
        rt->xsave_mask = 0U;
        rt->xsave_area_size = 0U;
        rt->xsave_area_alloc_size = 0U;
        return 0;
    }

    max_leaf = __get_cpuid_max(0, NULL);
    if (max_leaf < 0xDU) {
        return 0;
    }
    if (!__get_cpuid(1U, &eax, &ebx, &ecx, &edx)) {
        return 0;
    }
    if ((ecx & bit_XSAVE) == 0U || (ecx & bit_OSXSAVE) == 0U) {
        return 0;
    }
    if (!__get_cpuid_count(0xDU, 0U, &eax, &ebx, &ecx, &edx)) {
        return 0;
    }

    xcr0 = nm_xgetbv(0U);
    supported_mask = ((uint64_t)edx << 32U) | (uint64_t)eax;
    rt->xsave_mask = xcr0 & supported_mask;
    if ((rt->xsave_mask & 0x3U) != 0x3U) {
        // x87/SSE state must be present before the runtime can rely on XSAVE.
        rt->xsave_mask = 0U;
        return 0;
    }

    rt->xsave_area_size = ebx > ecx ? (size_t)ebx : (size_t)ecx;
    if (rt->xsave_area_size == 0U) {
        rt->xsave_mask = 0U;
        return 0;
    }
    rt->xsave_area_alloc_size = nm_align_up(rt->xsave_area_size, 64U);
    rt->xsave_enabled = true;
    g_nm_xsave_mask_lo = (uint32_t)rt->xsave_mask;
    g_nm_xsave_mask_hi = (uint32_t)(rt->xsave_mask >> 32U);
    return 0;
}

/**
 * @brief Initialize floating-point state for a new x86-64 context.
 *
 * @param ctx Context to initialize.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
int nm_ctx_init_fp_state(nm_ctx_t *ctx) {
    void *area = NULL;

    if (ctx == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* Fresh task/scheduler contexts inherit the current worker FP environment. */
    ctx->mxcsr = nm_current_mxcsr();
    ctx->x87_cw = nm_current_x87_cw();
    ctx->pad = 0U;
    ctx->xsave_area = NULL;
    if (!g_nm_runtime.xsave_enabled) {
        return 0;
    }

    if (posix_memalign(&area, 64U, g_nm_runtime.xsave_area_alloc_size) != 0) {
        errno = ENOMEM;
        return -1;
    }
    memset(area, 0, g_nm_runtime.xsave_area_alloc_size);
    nm_save_xsave_area(area, g_nm_runtime.xsave_mask);
    ctx->xsave_area = area;
    return 0;
}

/**
 * @brief Destroy x86-64 floating-point state owned by a context.
 *
 * @param ctx Context to clean up.
 */
void nm_ctx_destroy_fp_state(nm_ctx_t *ctx) {
    if (ctx == NULL || ctx->xsave_area == NULL) {
        return;
    }

    free(ctx->xsave_area);
    ctx->xsave_area = NULL;
}
#elif defined(__aarch64__)
/** @brief Read AArch64 FPCR. */
static uint64_t nm_current_fpcr(void) {
    uint64_t value;

    __asm__ volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}

/** @brief Read AArch64 FPSR. */
static uint64_t nm_current_fpsr(void) {
    uint64_t value;

    __asm__ volatile("mrs %0, fpsr" : "=r"(value));
    return value;
}

/**
 * @brief Disable XSAVE-style state on AArch64 and clear global flags.
 */
int nm_detect_xsave_support(nm_runtime_t *rt) {
    if (rt != NULL) {
        rt->xsave_enabled = false;
        rt->xsave_mask = 0U;
        rt->xsave_area_size = 0U;
        rt->xsave_area_alloc_size = 0U;
    }
    nm_clear_xsave_globals();
    return 0;
}

/**
 * @brief Initialize AArch64 floating-point control/status state.
 */
int nm_ctx_init_fp_state(nm_ctx_t *ctx) {
    if (ctx == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->fpcr = nm_current_fpcr();
    ctx->fpsr = nm_current_fpsr();
    return 0;
}

/**
 * @brief No-op AArch64 FP-state destructor.
 */
void nm_ctx_destroy_fp_state(nm_ctx_t *ctx) {
    (void)ctx;
}

#else
/**
 * @brief Disable XSAVE-style state on portable backends.
 */
int nm_detect_xsave_support(nm_runtime_t *rt) {
    if (rt != NULL) {
        rt->xsave_enabled = false;
        rt->xsave_mask = 0U;
        rt->xsave_area_size = 0U;
        rt->xsave_area_alloc_size = 0U;
    }
    nm_clear_xsave_globals();
    return 0;
}

/**
 * @brief Initialize portable floating-point environment state.
 */
int nm_ctx_init_fp_state(nm_ctx_t *ctx) {
    if (ctx == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    if (fegetenv(&ctx->fenv) == 0) {
        ctx->fenv_valid = 1;
    }
    return 0;
}

/**
 * @brief No-op portable FP-state destructor.
 */
void nm_ctx_destroy_fp_state(nm_ctx_t *ctx) {
    (void)ctx;
}
#endif
