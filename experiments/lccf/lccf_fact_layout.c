/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lccf_fact.h"

size_t lccf_fact_layout_hot_bytes(lccf_fact_layout_t layout) {
    switch (layout) {
    case LCCF_FACT_LAYOUT_SPLIT64_64:
        return sizeof(((lccf_fact_split64_64_layout_t *)0)->hot_words);
    case LCCF_FACT_LAYOUT_SPLIT96_64:
        return sizeof(((lccf_fact_split96_64_layout_t *)0)->hot_words);
    case LCCF_FACT_LAYOUT_UNIFIED128:
        return sizeof(lccf_fact_unified128_layout_t);
    default:
        return 0U;
    }
}

size_t lccf_fact_layout_sidecar_bytes(lccf_fact_layout_t layout) {
    switch (layout) {
    case LCCF_FACT_LAYOUT_SPLIT64_64:
        return sizeof(((lccf_fact_split64_64_layout_t *)0)->sidecar_words);
    case LCCF_FACT_LAYOUT_SPLIT96_64:
        return sizeof(((lccf_fact_split96_64_layout_t *)0)->sidecar_words);
    case LCCF_FACT_LAYOUT_UNIFIED128:
        return 0U;
    default:
        return 0U;
    }
}
