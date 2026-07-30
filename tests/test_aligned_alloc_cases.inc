// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

static int test_aligned_zalloc_contract(void) {
    unsigned char *bytes;
    void *allocation;
    size_t i;

    errno = 0;
    allocation = llam_aligned_zalloc(64U, 3U, 37U);
    if (allocation == NULL || errno != 0) {
        return fail_errno("aligned zero allocation valid case failed");
    }
    if (((uintptr_t)allocation & 63U) != 0U) {
        llam_aligned_free(allocation);
        return fail_msg("aligned zero allocation returned a misaligned pointer");
    }
    bytes = allocation;
    for (i = 0U; i < 3U * 37U; ++i) {
        if (bytes[i] != 0U) {
            llam_aligned_free(allocation);
            return fail_msg("aligned zero allocation returned nonzero storage");
        }
    }
    memset(allocation, 0xA5, 3U * 37U);
    llam_aligned_free(allocation);
    llam_aligned_free(NULL);

    errno = 0;
    allocation = llam_aligned_zalloc(24U, 1U, 64U);
    if (allocation != NULL || errno != EINVAL) {
        llam_aligned_free(allocation);
        return fail_errno("aligned zero allocation accepted non-power-of-two alignment");
    }

    errno = 0;
    allocation = llam_aligned_zalloc(64U, SIZE_MAX, 2U);
    if (allocation != NULL || errno != ENOMEM) {
        llam_aligned_free(allocation);
        return fail_errno("aligned zero allocation size overflow did not fail closed");
    }
    return 0;
}
