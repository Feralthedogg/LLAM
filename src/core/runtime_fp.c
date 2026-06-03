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

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <float.h>
#include <immintrin.h>
#include <intrin.h>
#include <malloc.h>
#endif

#if ((LLAM_PLATFORM_LINUX || LLAM_PLATFORM_DARWIN || LLAM_PLATFORM_BSD) && LLAM_ARCH_X86_64) || \
    (LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64)
#define LLAM_XSAVE_PROCESS_GLOBALS 1
#else
#define LLAM_XSAVE_PROCESS_GLOBALS 0
#endif

static pthread_mutex_t g_llam_xsave_global_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_llam_xsave_global_refs;
static bool g_llam_xsave_global_enabled;
static uint32_t g_llam_xsave_global_fp_control_context;
static uint64_t g_llam_xsave_global_mask;
static size_t g_llam_xsave_global_area_size;
static size_t g_llam_xsave_global_area_alloc_size;

/**
 * @brief Clear cached global extended-context capability flags.
 */
void llam_clear_xsave_globals(void) {
    g_llam_xsave_mask_lo = 0U;
    g_llam_xsave_mask_hi = 0U;
    g_llam_fp_control_context = 0U;
}

#if LLAM_XSAVE_PROCESS_GLOBALS
/**
 * @brief Publish cached process-wide FP switch policy into assembly globals.
 */
static void llam_publish_xsave_globals_locked(void) {
    g_llam_fp_control_context = g_llam_xsave_global_fp_control_context;
    g_llam_xsave_mask_lo = (uint32_t)g_llam_xsave_global_mask;
    g_llam_xsave_mask_hi = (uint32_t)(g_llam_xsave_global_mask >> 32U);
}

/**
 * @brief Copy cached process-wide FP switch policy into a runtime.
 */
static void llam_apply_xsave_globals_to_runtime_locked(llam_runtime_t *rt) {
    rt->xsave_enabled = g_llam_xsave_global_enabled;
    rt->xsave_mask = g_llam_xsave_global_mask;
    rt->xsave_area_size = g_llam_xsave_global_area_size;
    rt->xsave_area_alloc_size = g_llam_xsave_global_area_alloc_size;
    llam_publish_xsave_globals_locked();
}
#endif

/**
 * @brief Release one runtime's reference to process-wide FP switch globals.
 */
void llam_release_xsave_globals(llam_runtime_t *rt) {
    if (rt == NULL || !rt->fp_globals_retained) {
        return;
    }

    pthread_mutex_lock(&g_llam_xsave_global_lock);
    if (g_llam_xsave_global_refs > 0U) {
        g_llam_xsave_global_refs -= 1U;
    }
    rt->fp_globals_retained = false;
    if (g_llam_xsave_global_refs == 0U) {
        g_llam_xsave_global_enabled = false;
        g_llam_xsave_global_fp_control_context = 0U;
        g_llam_xsave_global_mask = 0U;
        g_llam_xsave_global_area_size = 0U;
        g_llam_xsave_global_area_alloc_size = 0U;
        llam_clear_xsave_globals();
    }
    pthread_mutex_unlock(&g_llam_xsave_global_lock);
}

#if LLAM_XSAVE_PROCESS_GLOBALS
/** @brief Read the current SSE MXCSR control/status register. */
static uint32_t llam_current_mxcsr(void) {
#if defined(_MSC_VER)
    return (uint32_t)_mm_getcsr();
#else
    uint32_t value;

    __asm__ volatile("stmxcsr %0" : "=m"(value));
    return value;
#endif
}

/** @brief Read the current x87 control word. */
static uint16_t llam_current_x87_cw(void) {
#if defined(_MSC_VER)
    unsigned int current = 0U;

    (void)_controlfp_s(&current, 0U, 0U);
    return (uint16_t)current;
#else
    uint16_t value;

    __asm__ volatile("fnstcw %0" : "=m"(value));
    return value;
#endif
}

#if defined(__linux__)
/** @brief Execute xgetbv for the given extended-control register index. */
static uint64_t llam_xgetbv(uint32_t index) {
    uint32_t eax;
    uint32_t edx;

    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
    return ((uint64_t)edx << 32U) | (uint64_t)eax;
}

#endif

/** @brief Save extended CPU state into an aligned XSAVE area. */
static void llam_save_xsave_area(void *area, uint64_t mask) {
#if defined(_MSC_VER)
    _xsave64(area, mask);
#else
    uint32_t eax = (uint32_t)mask;
    uint32_t edx = (uint32_t)(mask >> 32U);

    __asm__ volatile("xsave64 (%0)" : : "r"(area), "a"(eax), "d"(edx) : "memory");
#endif
}

/**
 * @brief Detect x86-64 XSAVE support and configure runtime FP context policy.
 *
 * @param rt Runtime to update.
 *
 * @return Always 0; unsupported features simply leave XSAVE disabled.
 */
int llam_detect_xsave_support(llam_runtime_t *rt) {
    const char *fp_env;
    int rc = 0;
#if defined(__linux__)
    const char *xsave_env;
    unsigned max_leaf;
    unsigned eax;
    unsigned ebx;
    unsigned ecx;
    unsigned edx;
    uint64_t supported_mask;
    uint64_t xcr0;
#endif

    if (rt == NULL) {
        return 0;
    }

    /*
     * The x86 assembly switch path reads process-wide globals.  Multiple
     * explicit runtimes therefore share one stable FP policy while any runtime
     * is alive; a peer create/destroy must not transiently clear these globals.
     */
    pthread_mutex_lock(&g_llam_xsave_global_lock);
    if (rt->fp_globals_retained) {
        llam_apply_xsave_globals_to_runtime_locked(rt);
        pthread_mutex_unlock(&g_llam_xsave_global_lock);
        return 0;
    }
    if (g_llam_xsave_global_refs > 0U) {
        g_llam_xsave_global_refs += 1U;
        rt->fp_globals_retained = true;
        llam_apply_xsave_globals_to_runtime_locked(rt);
        pthread_mutex_unlock(&g_llam_xsave_global_lock);
        return 0;
    }

    llam_clear_xsave_globals();
    rt->xsave_enabled = false;
    rt->xsave_mask = 0U;
    rt->xsave_area_size = 0U;
    rt->xsave_area_alloc_size = 0U;

    fp_env = llam_env_get("LLAM_FP_CONTROL_CONTEXT");
    g_llam_xsave_global_fp_control_context =
        llam_env_flag_value(fp_env, 1U) != 0U ? 1U : 0U;

#if !defined(__linux__)
    /*
     * Darwin and Windows x86-64 use fast register switches but currently
     * preserve only FP control state. Keep XSAVE disabled until each platform's
     * capability probe and signal/SEH interactions are explicitly validated.
     */
    goto done;
#else
    xsave_env = llam_env_get("LLAM_XSAVE_CONTEXT");
    // XSAVE is opt-in because it increases per-context allocation and switch cost.
    if (llam_env_flag_value(xsave_env, 0U) == 0U) {
        goto done;
    }

    max_leaf = __get_cpuid_max(0, NULL);
    if (max_leaf < 0xDU) {
        goto done;
    }
    if (!__get_cpuid(1U, &eax, &ebx, &ecx, &edx)) {
        goto done;
    }
    if ((ecx & bit_XSAVE) == 0U || (ecx & bit_OSXSAVE) == 0U) {
        goto done;
    }
    if (!__get_cpuid_count(0xDU, 0U, &eax, &ebx, &ecx, &edx)) {
        goto done;
    }

    xcr0 = llam_xgetbv(0U);
    supported_mask = ((uint64_t)edx << 32U) | (uint64_t)eax;
    rt->xsave_mask = xcr0 & supported_mask;
    if ((rt->xsave_mask & 0x3U) != 0x3U) {
        // x87/SSE state must be present before the runtime can rely on XSAVE.
        rt->xsave_mask = 0U;
        goto done;
    }

    rt->xsave_area_size = ebx > ecx ? (size_t)ebx : (size_t)ecx;
    if (rt->xsave_area_size == 0U) {
        rt->xsave_mask = 0U;
        goto done;
    }
    if (llam_align_up_checked(rt->xsave_area_size, 64U, &rt->xsave_area_alloc_size) != 0) {
        rt->xsave_mask = 0U;
        goto done;
    }
    rt->xsave_enabled = true;
#endif

done:
    g_llam_xsave_global_enabled = rt->xsave_enabled;
    g_llam_xsave_global_mask = rt->xsave_mask;
    g_llam_xsave_global_area_size = rt->xsave_area_size;
    g_llam_xsave_global_area_alloc_size = rt->xsave_area_alloc_size;
    g_llam_xsave_global_refs = 1U;
    rt->fp_globals_retained = true;
    llam_publish_xsave_globals_locked();
    pthread_mutex_unlock(&g_llam_xsave_global_lock);
    return rc;
}

/**
 * @brief Initialize floating-point state for a new x86-64 context.
 *
 * @param ctx Context to initialize.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
int llam_ctx_init_fp_state(llam_ctx_t *ctx, llam_runtime_t *rt) {
    void *area = NULL;

    if (ctx == NULL) {
        errno = EINVAL;
        return -1;
    }

#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64
    /*
     * Fresh Windows fibers skip XMM6-XMM15 restore on first entry, but still
     * need to inherit the caller's FP control environment when the scheduler
     * dispatches them without a direct task-to-task handoff.
     */
    ctx->mxcsr = llam_current_mxcsr();
    ctx->x87_cw = llam_current_x87_cw();
    ctx->pad = 0U;
    ctx->xsave_area = NULL;
    ctx->simd_valid = 0U;
    ctx->simd_flags = 0U;
    return 0;
#endif

    /* Fresh task/scheduler contexts inherit the current worker FP environment. */
    ctx->mxcsr = llam_current_mxcsr();
    ctx->x87_cw = llam_current_x87_cw();
    ctx->pad = 0U;
    ctx->xsave_area = NULL;
    if (rt == NULL || !rt->xsave_enabled) {
        return 0;
    }

#if defined(_MSC_VER)
    area = _aligned_malloc(rt->xsave_area_alloc_size, 64U);
    if (area == NULL) {
        errno = ENOMEM;
        return -1;
    }
#else
    if (posix_memalign(&area, 64U, rt->xsave_area_alloc_size) != 0) {
        errno = ENOMEM;
        return -1;
    }
#endif
    memset(area, 0, rt->xsave_area_alloc_size);
    llam_save_xsave_area(area, rt->xsave_mask);
    ctx->xsave_area = area;
    return 0;
}

/**
 * @brief Destroy x86-64 floating-point state owned by a context.
 *
 * @param ctx Context to clean up.
 */
void llam_ctx_destroy_fp_state(llam_ctx_t *ctx) {
    if (ctx == NULL || ctx->xsave_area == NULL) {
        return;
    }

#if defined(_MSC_VER)
    _aligned_free(ctx->xsave_area);
#else
    free(ctx->xsave_area);
#endif
    ctx->xsave_area = NULL;
}
#elif LLAM_ARCH_AARCH64
/** @brief Read AArch64 FPCR. */
static uint64_t llam_current_fpcr(void) {
    uint64_t value;

    __asm__ volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}

/** @brief Read AArch64 FPSR. */
static uint64_t llam_current_fpsr(void) {
    uint64_t value;

    __asm__ volatile("mrs %0, fpsr" : "=r"(value));
    return value;
}

/**
 * @brief Disable XSAVE-style state on AArch64 and clear global flags.
 */
int llam_detect_xsave_support(llam_runtime_t *rt) {
    if (rt != NULL) {
        rt->xsave_enabled = false;
        rt->xsave_mask = 0U;
        rt->xsave_area_size = 0U;
        rt->xsave_area_alloc_size = 0U;
    }
    llam_clear_xsave_globals();
    return 0;
}

/**
 * @brief Initialize AArch64 floating-point control/status state.
 */
int llam_ctx_init_fp_state(llam_ctx_t *ctx, llam_runtime_t *rt) {
    (void)rt;

    if (ctx == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->fpcr = llam_current_fpcr();
    ctx->fpsr = llam_current_fpsr();
    return 0;
}

/**
 * @brief No-op AArch64 FP-state destructor.
 */
void llam_ctx_destroy_fp_state(llam_ctx_t *ctx) {
    (void)ctx;
}

#else
/**
 * @brief Disable XSAVE-style state on portable backends.
 */
int llam_detect_xsave_support(llam_runtime_t *rt) {
    if (rt != NULL) {
        rt->xsave_enabled = false;
        rt->xsave_mask = 0U;
        rt->xsave_area_size = 0U;
        rt->xsave_area_alloc_size = 0U;
    }
    llam_clear_xsave_globals();
    return 0;
}

/**
 * @brief Initialize portable floating-point environment state.
 */
int llam_ctx_init_fp_state(llam_ctx_t *ctx, llam_runtime_t *rt) {
    (void)rt;

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
void llam_ctx_destroy_fp_state(llam_ctx_t *ctx) {
    (void)ctx;
}
#endif
