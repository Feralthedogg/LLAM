// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#include "srem_model_internal.h"

#include <errno.h>
#include <stdint.h>

static uint64_t rotl64(uint64_t value, unsigned shift) {
    shift &= 63U;
    return (value << shift) | (value >> ((64U - shift) & 63U));
}

static uint16_t select_next_site(const srem_model_event_t *event,
                                 const srem_model_config_t *config,
                                 unsigned resume_site) {
    unsigned advance = 1U;

    if (event->divergent != 0U && config->site_count > 1U) {
        advance = 3U;
    }
    return (uint16_t)((resume_site + advance) % config->site_count);
}

static void finish_segment(srem_model_frame_core_t *frame,
                           const srem_model_event_t *event,
                           srem_model_effect_t *effect,
                           const srem_model_config_t *config,
                           unsigned resume_site,
                           uint64_t output,
                           srem_model_effect_kind_t kind) {
    const uint16_t next_site =
        select_next_site(event, config, resume_site);

    frame->steps = (uint16_t)(frame->steps + 1U);
    frame->site = next_site;
    frame->flags =
        ((uint32_t)event->divergent << 8U) | (uint32_t)event->kind;
    effect->argument0 = output;
    effect->argument1 = frame->field[1] ^ frame->field[4];
    effect->argument2 = frame->field[2] + frame->field[5];
    effect->operation = (uint32_t)kind;
    effect->next_site = next_site;
    effect->flags = (uint16_t)frame->flags;
}

static void resume_http_pipeline(srem_model_frame_core_t *frame,
                                 const srem_model_event_t *event,
                                 srem_model_effect_t *effect,
                                 const srem_model_config_t *config,
                                 unsigned resume_site) {
    const uint64_t salt =
        (uint64_t)(resume_site + 1U) *
        UINT64_C(0x9E3779B97F4A7C15);
    const uint64_t a =
        (frame->field[0] + event->word0) ^ salt;
    const uint64_t b =
        rotl64(frame->field[1] ^ event->word1, 13U);
    const uint64_t c =
        (a * UINT64_C(0xD6E8FEB86659FD93)) ^ b;
    const uint64_t d =
        rotl64(frame->field[2] + c, 17U);
    const uint64_t e =
        frame->field[3] + (c ^ d) +
        (uint64_t)(uint32_t)event->result;
    const uint64_t f =
        frame->field[4] ^ rotl64(e + frame->field[5], 11U);

    frame->field[0] = c + rotl64(f, 7U);
    frame->field[1] = b ^ d;
    frame->field[2] = d + e;
    frame->field[3] = e ^ rotl64(a, 23U);
    frame->field[4] = f + c;
    frame->field[5] =
        rotl64(frame->field[5] + f + event->word0, 29U);
    finish_segment(frame,
                   event,
                   effect,
                   config,
                   resume_site,
                   frame->field[0] ^ frame->field[2] ^
                       frame->field[5],
                   (frame->steps & 1U) == 0U ?
                       SREM_MODEL_EFFECT_WRITE :
                       SREM_MODEL_EFFECT_READ);
}

static void resume_rpc_pipeline(srem_model_frame_core_t *frame,
                                const srem_model_event_t *event,
                                srem_model_effect_t *effect,
                                const srem_model_config_t *config,
                                unsigned resume_site) {
    const uint64_t selector =
        (event->word0 ^ frame->field[0] ^ resume_site) & UINT64_C(3);
    const uint64_t mask = UINT64_C(0) - (selector & UINT64_C(1));
    const uint64_t success =
        frame->field[0] + event->word1 +
        UINT64_C(0x165667B19E3779F9);
    const uint64_t retry =
        (frame->field[0] ^ event->word1) *
        UINT64_C(0xA0761D6478BD642F);
    const uint64_t selected =
        (success & ~mask) | (retry & mask);

    frame->field[0] = selected;
    frame->field[1] =
        rotl64(frame->field[1] ^ selected,
               (unsigned)(selector + resume_site + 5U));
    frame->field[2] +=
        (event->word0 & mask) | (event->word1 & ~mask);
    frame->field[3] =
        (frame->field[3] + frame->field[2]) *
        UINT64_C(0x9E3779B185EBCA87);
    frame->field[4] ^=
        rotl64(frame->field[0] + frame->field[3], 19U);
    frame->field[5] +=
        frame->field[1] ^ frame->field[4] ^ selector;
    finish_segment(frame,
                   event,
                   effect,
                   config,
                   resume_site,
                   frame->field[0] + frame->field[3] +
                       frame->field[5],
                   selector == 3U ? SREM_MODEL_EFFECT_YIELD :
                                    SREM_MODEL_EFFECT_WRITE);
}

static void resume_divergent_cancel(
    srem_model_frame_core_t *frame,
    const srem_model_event_t *event,
    srem_model_effect_t *effect,
    const srem_model_config_t *config,
    unsigned resume_site) {
    const uint64_t cancel_mask =
        UINT64_C(0) -
        (uint64_t)(event->kind == SREM_MODEL_EVENT_CANCEL);
    const uint64_t normal =
        frame->field[0] + event->word0 +
        UINT64_C(0xE7037ED1A0B428DB);
    const uint64_t cancelled =
        frame->field[0] ^ rotl64(event->word1, 31U);
    const uint64_t selected =
        (normal & ~cancel_mask) | (cancelled & cancel_mask);

    frame->field[0] = selected;
    frame->field[1] =
        rotl64(frame->field[1] + selected + event->word1, 9U);
    frame->field[2] ^=
        selected * UINT64_C(0x8EBC6AF09C88C6E3);
    frame->field[3] += frame->field[1] ^ frame->field[2];
    frame->field[4] =
        rotl64(frame->field[4] + frame->field[3], 21U);
    frame->field[5] ^=
        frame->field[0] + frame->field[4] +
        (uint64_t)event->kind;
    finish_segment(
        frame,
        event,
        effect,
        config,
        resume_site,
        frame->field[0] ^ frame->field[3] ^ frame->field[5],
        event->kind == SREM_MODEL_EVENT_CANCEL ?
            SREM_MODEL_EFFECT_YIELD :
            (event->kind == SREM_MODEL_EVENT_TIMEOUT ?
                 SREM_MODEL_EFFECT_TIMER :
                 SREM_MODEL_EFFECT_READ));
}

static void resume_mixed_fairness(srem_model_frame_core_t *frame,
                                  const srem_model_event_t *event,
                                  srem_model_effect_t *effect,
                                  const srem_model_config_t *config,
                                  unsigned resume_site) {
    const uint64_t mixed =
        srem_model_mix64(event->word0 + frame->field[0] +
                         ((uint64_t)resume_site << 32U));

    frame->field[0] =
        mixed ^ rotl64(frame->field[1] + event->word1, 11U);
    frame->field[1] +=
        frame->field[0] * UINT64_C(0x94D049BB133111EB);
    frame->field[2] ^= rotl64(frame->field[0] + frame->field[1], 23U);
    frame->field[3] += frame->field[2] ^ event->word0;
    frame->field[4] =
        rotl64(frame->field[4] + frame->field[3], 15U);
    frame->field[5] ^=
        frame->field[0] + frame->field[4] + event->word1;
    finish_segment(frame,
                   event,
                   effect,
                   config,
                   resume_site,
                   frame->field[0] ^ frame->field[2] ^
                       frame->field[5],
                   (frame->steps & 31U) == 0U ?
                       SREM_MODEL_EFFECT_TIMER :
                       SREM_MODEL_EFFECT_WRITE);
}

#define SREM_SITE_TABLE(function_name)                                        \
    {                                                                         \
        (function_name), (function_name), (function_name), (function_name),   \
            (function_name), (function_name), (function_name),                \
            (function_name)                                                   \
    }

static const srem_model_workload_ops_t WORKLOAD_OPS[] = {
    {SREM_SITE_TABLE(resume_http_pipeline)},
    {SREM_SITE_TABLE(resume_rpc_pipeline)},
    {SREM_SITE_TABLE(resume_divergent_cancel)},
    {SREM_SITE_TABLE(resume_mixed_fairness)},
};

#undef SREM_SITE_TABLE

const srem_model_workload_ops_t *
srem_model_get_workload_ops(srem_model_workload_t workload) {
    if (workload < SREM_MODEL_HTTP_PIPELINE ||
        workload > SREM_MODEL_MIXED_FAIRNESS) {
        return NULL;
    }
    return &WORKLOAD_OPS[(unsigned)workload];
}

#if defined(__clang__)
#define SREM_VECTORIZE_LOOP                                                \
    _Pragma("clang loop vectorize(assume_safety) interleave(enable)")
#elif defined(__GNUC__)
#define SREM_VECTORIZE_LOOP _Pragma("GCC ivdep")
#else
#define SREM_VECTORIZE_LOOP
#endif

static uint64_t merge_u64(uint64_t old_value,
                          uint64_t new_value,
                          uint64_t active_mask) {
    return (old_value & ~active_mask) |
           (new_value & active_mask);
}

static uint32_t merge_u32(uint32_t old_value,
                          uint32_t new_value,
                          uint32_t active_mask) {
    return (old_value & ~active_mask) |
           (new_value & active_mask);
}

static uint16_t merge_u16(uint16_t old_value,
                          uint16_t new_value,
                          uint16_t active_mask) {
    return (uint16_t)((old_value & (uint16_t)~active_mask) |
                      (new_value & active_mask));
}

static uint64_t local_mix64(uint64_t value) {
    value ^= value >> 30U;
    value *= UINT64_C(0xBF58476D1CE4E5B9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94D049BB133111EB);
    value ^= value >> 31U;
    return value;
}

static void vector_http_pipeline(
    const srem_model_config_t *config,
    srem_model_tile_view_t *view) {
    const unsigned width = view->width;
    const unsigned resume_site = view->resume_site;
    const uint32_t active_mask = view->active_mask;
    uint64_t *restrict field0 = view->field[0];
    uint64_t *restrict field1 = view->field[1];
    uint64_t *restrict field2 = view->field[2];
    uint64_t *restrict field3 = view->field[3];
    uint64_t *restrict field4 = view->field[4];
    uint64_t *restrict field5 = view->field[5];
    uint16_t *restrict sites = view->site;
    uint16_t *restrict steps = view->steps;
    uint32_t *restrict flags = view->flags;
    const uint64_t *restrict event0 = view->event_word0;
    const uint64_t *restrict event1 = view->event_word1;
    const int32_t *restrict results = view->event_result;
    const uint8_t *restrict event_kind = view->event_kind;
    const uint8_t *restrict divergent = view->event_divergent;
    uint64_t *restrict argument0 = view->effect_argument[0];
    uint64_t *restrict argument1 = view->effect_argument[1];
    uint64_t *restrict argument2 = view->effect_argument[2];
    uint32_t *restrict operation = view->effect_operation;
    uint16_t *restrict next_sites = view->effect_next_site;
    uint16_t *restrict effect_flags = view->effect_flags;
    const uint64_t salt =
        (uint64_t)(resume_site + 1U) *
        UINT64_C(0x9E3779B97F4A7C15);
    const uint16_t normal_site =
        (uint16_t)((resume_site + 1U) %
                   config->site_count);
    const uint16_t divergent_site =
        (uint16_t)((resume_site + 3U) %
                   config->site_count);
    unsigned lane;

    SREM_VECTORIZE_LOOP
    for (lane = 0U; lane < width; ++lane) {
        const uint32_t active =
            (active_mask >> lane) & UINT32_C(1);
        const uint64_t active64 =
            UINT64_C(0) - (uint64_t)active;
        const uint32_t active32 =
            UINT32_C(0) - active;
        const uint16_t active16 =
            (uint16_t)(UINT16_C(0) - (uint16_t)active);
        const uint64_t old0 = field0[lane];
        const uint64_t old1 = field1[lane];
        const uint64_t old2 = field2[lane];
        const uint64_t old3 = field3[lane];
        const uint64_t old4 = field4[lane];
        const uint64_t old5 = field5[lane];
        const uint16_t old_steps = steps[lane];
        const uint64_t a = (old0 + event0[lane]) ^ salt;
        const uint64_t b = rotl64(old1 ^ event1[lane], 13U);
        const uint64_t c =
            (a * UINT64_C(0xD6E8FEB86659FD93)) ^ b;
        const uint64_t d = rotl64(old2 + c, 17U);
        const uint64_t e =
            old3 + (c ^ d) +
            (uint64_t)(uint32_t)results[lane];
        const uint64_t f = old4 ^ rotl64(e + old5, 11U);
        const uint64_t new0 = c + rotl64(f, 7U);
        const uint64_t new1 = b ^ d;
        const uint64_t new2 = d + e;
        const uint64_t new3 = e ^ rotl64(a, 23U);
        const uint64_t new4 = f + c;
        const uint64_t new5 =
            rotl64(old5 + f + event0[lane], 29U);
        const uint16_t next_site =
            divergent[lane] != 0U ? divergent_site :
                                    normal_site;
        const uint32_t new_flags =
            ((uint32_t)divergent[lane] << 8U) |
            (uint32_t)event_kind[lane];
        const uint32_t next_operation =
            (old_steps & 1U) == 0U ?
                SREM_MODEL_EFFECT_WRITE :
                SREM_MODEL_EFFECT_READ;

        field0[lane] = merge_u64(old0, new0, active64);
        field1[lane] = merge_u64(old1, new1, active64);
        field2[lane] = merge_u64(old2, new2, active64);
        field3[lane] = merge_u64(old3, new3, active64);
        field4[lane] = merge_u64(old4, new4, active64);
        field5[lane] = merge_u64(old5, new5, active64);
        sites[lane] =
            merge_u16(sites[lane], next_site, active16);
        steps[lane] =
            merge_u16(old_steps,
                      (uint16_t)(old_steps + 1U),
                      active16);
        flags[lane] =
            merge_u32(flags[lane], new_flags, active32);
        argument0[lane] =
            merge_u64(argument0[lane],
                      new0 ^ new2 ^ new5,
                      active64);
        argument1[lane] =
            merge_u64(argument1[lane],
                      new1 ^ new4,
                      active64);
        argument2[lane] =
            merge_u64(argument2[lane],
                      new2 + new5,
                      active64);
        operation[lane] =
            merge_u32(operation[lane],
                      next_operation,
                      active32);
        next_sites[lane] =
            merge_u16(next_sites[lane], next_site, active16);
        effect_flags[lane] =
            merge_u16(effect_flags[lane],
                      (uint16_t)new_flags,
                      active16);
    }
}

static void vector_rpc_pipeline(const srem_model_config_t *config,
                                srem_model_tile_view_t *view) {
    const unsigned width = view->width;
    const unsigned resume_site = view->resume_site;
    const uint32_t active_mask = view->active_mask;
    uint64_t *restrict field0 = view->field[0];
    uint64_t *restrict field1 = view->field[1];
    uint64_t *restrict field2 = view->field[2];
    uint64_t *restrict field3 = view->field[3];
    uint64_t *restrict field4 = view->field[4];
    uint64_t *restrict field5 = view->field[5];
    uint16_t *restrict sites = view->site;
    uint16_t *restrict steps = view->steps;
    uint32_t *restrict flags = view->flags;
    const uint64_t *restrict event0 = view->event_word0;
    const uint64_t *restrict event1 = view->event_word1;
    const uint8_t *restrict event_kind = view->event_kind;
    const uint8_t *restrict divergent = view->event_divergent;
    uint64_t *restrict argument0 = view->effect_argument[0];
    uint64_t *restrict argument1 = view->effect_argument[1];
    uint64_t *restrict argument2 = view->effect_argument[2];
    uint32_t *restrict operation = view->effect_operation;
    uint16_t *restrict next_sites = view->effect_next_site;
    uint16_t *restrict effect_flags = view->effect_flags;
    const uint16_t normal_site =
        (uint16_t)((resume_site + 1U) %
                   config->site_count);
    const uint16_t divergent_site =
        (uint16_t)((resume_site + 3U) %
                   config->site_count);
    unsigned lane;

    SREM_VECTORIZE_LOOP
    for (lane = 0U; lane < width; ++lane) {
        const uint32_t active =
            (active_mask >> lane) & UINT32_C(1);
        const uint64_t active64 =
            UINT64_C(0) - (uint64_t)active;
        const uint32_t active32 =
            UINT32_C(0) - active;
        const uint16_t active16 =
            (uint16_t)(UINT16_C(0) - (uint16_t)active);
        const uint64_t old0 = field0[lane];
        const uint64_t old1 = field1[lane];
        const uint64_t old2 = field2[lane];
        const uint64_t old3 = field3[lane];
        const uint64_t old4 = field4[lane];
        const uint64_t old5 = field5[lane];
        const uint16_t old_steps = steps[lane];
        const uint64_t selector =
            (event0[lane] ^ old0 ^ resume_site) &
            UINT64_C(3);
        const uint64_t selector_mask =
            UINT64_C(0) - (selector & UINT64_C(1));
        const uint64_t success =
            old0 + event1[lane] +
            UINT64_C(0x165667B19E3779F9);
        const uint64_t retry =
            (old0 ^ event1[lane]) *
            UINT64_C(0xA0761D6478BD642F);
        const uint64_t new0 =
            (success & ~selector_mask) |
            (retry & selector_mask);
        const uint64_t new1 =
            rotl64(old1 ^ new0,
                   (unsigned)(selector +
                              resume_site + 5U));
        const uint64_t new2 =
            old2 + ((event0[lane] & selector_mask) |
                    (event1[lane] & ~selector_mask));
        const uint64_t new3 =
            (old3 + new2) *
            UINT64_C(0x9E3779B185EBCA87);
        const uint64_t new4 =
            old4 ^ rotl64(new0 + new3, 19U);
        const uint64_t new5 =
            old5 + (new1 ^ new4 ^ selector);
        const uint16_t next_site =
            divergent[lane] != 0U ? divergent_site :
                                    normal_site;
        const uint32_t new_flags =
            ((uint32_t)divergent[lane] << 8U) |
            (uint32_t)event_kind[lane];
        const uint32_t next_operation =
            selector == 3U ? SREM_MODEL_EFFECT_YIELD :
                             SREM_MODEL_EFFECT_WRITE;

        field0[lane] = merge_u64(old0, new0, active64);
        field1[lane] = merge_u64(old1, new1, active64);
        field2[lane] = merge_u64(old2, new2, active64);
        field3[lane] = merge_u64(old3, new3, active64);
        field4[lane] = merge_u64(old4, new4, active64);
        field5[lane] = merge_u64(old5, new5, active64);
        sites[lane] =
            merge_u16(sites[lane], next_site, active16);
        steps[lane] =
            merge_u16(old_steps,
                      (uint16_t)(old_steps + 1U),
                      active16);
        flags[lane] =
            merge_u32(flags[lane], new_flags, active32);
        argument0[lane] =
            merge_u64(argument0[lane],
                      new0 + new3 + new5,
                      active64);
        argument1[lane] =
            merge_u64(argument1[lane],
                      new1 ^ new4,
                      active64);
        argument2[lane] =
            merge_u64(argument2[lane],
                      new2 + new5,
                      active64);
        operation[lane] =
            merge_u32(operation[lane],
                      next_operation,
                      active32);
        next_sites[lane] =
            merge_u16(next_sites[lane], next_site, active16);
        effect_flags[lane] =
            merge_u16(effect_flags[lane],
                      (uint16_t)new_flags,
                      active16);
    }
}

static void vector_divergent_cancel(
    const srem_model_config_t *config,
    srem_model_tile_view_t *view) {
    const unsigned width = view->width;
    const unsigned resume_site = view->resume_site;
    const uint32_t active_mask = view->active_mask;
    uint64_t *restrict field0 = view->field[0];
    uint64_t *restrict field1 = view->field[1];
    uint64_t *restrict field2 = view->field[2];
    uint64_t *restrict field3 = view->field[3];
    uint64_t *restrict field4 = view->field[4];
    uint64_t *restrict field5 = view->field[5];
    uint16_t *restrict sites = view->site;
    uint16_t *restrict steps = view->steps;
    uint32_t *restrict flags = view->flags;
    const uint64_t *restrict event0 = view->event_word0;
    const uint64_t *restrict event1 = view->event_word1;
    const uint8_t *restrict event_kind = view->event_kind;
    const uint8_t *restrict divergent = view->event_divergent;
    uint64_t *restrict argument0 = view->effect_argument[0];
    uint64_t *restrict argument1 = view->effect_argument[1];
    uint64_t *restrict argument2 = view->effect_argument[2];
    uint32_t *restrict operation = view->effect_operation;
    uint16_t *restrict next_sites = view->effect_next_site;
    uint16_t *restrict effect_flags = view->effect_flags;
    const uint16_t normal_site =
        (uint16_t)((resume_site + 1U) %
                   config->site_count);
    const uint16_t divergent_site =
        (uint16_t)((resume_site + 3U) %
                   config->site_count);
    unsigned lane;

    SREM_VECTORIZE_LOOP
    for (lane = 0U; lane < width; ++lane) {
        const uint32_t active =
            (active_mask >> lane) & UINT32_C(1);
        const uint64_t active64 =
            UINT64_C(0) - (uint64_t)active;
        const uint32_t active32 =
            UINT32_C(0) - active;
        const uint16_t active16 =
            (uint16_t)(UINT16_C(0) - (uint16_t)active);
        const uint64_t old0 = field0[lane];
        const uint64_t old1 = field1[lane];
        const uint64_t old2 = field2[lane];
        const uint64_t old3 = field3[lane];
        const uint64_t old4 = field4[lane];
        const uint64_t old5 = field5[lane];
        const uint16_t old_steps = steps[lane];
        const uint64_t cancel_mask =
            UINT64_C(0) -
            (uint64_t)(event_kind[lane] ==
                       SREM_MODEL_EVENT_CANCEL);
        const uint64_t normal =
            old0 + event0[lane] +
            UINT64_C(0xE7037ED1A0B428DB);
        const uint64_t cancelled =
            old0 ^ rotl64(event1[lane], 31U);
        const uint64_t new0 =
            (normal & ~cancel_mask) |
            (cancelled & cancel_mask);
        const uint64_t new1 =
            rotl64(old1 + new0 + event1[lane], 9U);
        const uint64_t new2 =
            old2 ^
            (new0 * UINT64_C(0x8EBC6AF09C88C6E3));
        const uint64_t new3 = old3 + (new1 ^ new2);
        const uint64_t new4 = rotl64(old4 + new3, 21U);
        const uint64_t new5 =
            old5 ^ (new0 + new4 + event_kind[lane]);
        const uint16_t next_site =
            divergent[lane] != 0U ? divergent_site :
                                    normal_site;
        const uint32_t new_flags =
            ((uint32_t)divergent[lane] << 8U) |
            (uint32_t)event_kind[lane];
        const uint32_t next_operation =
            event_kind[lane] == SREM_MODEL_EVENT_CANCEL ?
                SREM_MODEL_EFFECT_YIELD :
                (event_kind[lane] == SREM_MODEL_EVENT_TIMEOUT ?
                     SREM_MODEL_EFFECT_TIMER :
                     SREM_MODEL_EFFECT_READ);

        field0[lane] = merge_u64(old0, new0, active64);
        field1[lane] = merge_u64(old1, new1, active64);
        field2[lane] = merge_u64(old2, new2, active64);
        field3[lane] = merge_u64(old3, new3, active64);
        field4[lane] = merge_u64(old4, new4, active64);
        field5[lane] = merge_u64(old5, new5, active64);
        sites[lane] =
            merge_u16(sites[lane], next_site, active16);
        steps[lane] =
            merge_u16(old_steps,
                      (uint16_t)(old_steps + 1U),
                      active16);
        flags[lane] =
            merge_u32(flags[lane], new_flags, active32);
        argument0[lane] =
            merge_u64(argument0[lane],
                      new0 ^ new3 ^ new5,
                      active64);
        argument1[lane] =
            merge_u64(argument1[lane],
                      new1 ^ new4,
                      active64);
        argument2[lane] =
            merge_u64(argument2[lane],
                      new2 + new5,
                      active64);
        operation[lane] =
            merge_u32(operation[lane],
                      next_operation,
                      active32);
        next_sites[lane] =
            merge_u16(next_sites[lane], next_site, active16);
        effect_flags[lane] =
            merge_u16(effect_flags[lane],
                      (uint16_t)new_flags,
                      active16);
    }
}

static void vector_mixed_fairness(
    const srem_model_config_t *config,
    srem_model_tile_view_t *view) {
    const unsigned width = view->width;
    const unsigned resume_site = view->resume_site;
    const uint32_t active_mask = view->active_mask;
    uint64_t *restrict field0 = view->field[0];
    uint64_t *restrict field1 = view->field[1];
    uint64_t *restrict field2 = view->field[2];
    uint64_t *restrict field3 = view->field[3];
    uint64_t *restrict field4 = view->field[4];
    uint64_t *restrict field5 = view->field[5];
    uint16_t *restrict sites = view->site;
    uint16_t *restrict steps = view->steps;
    uint32_t *restrict flags = view->flags;
    const uint64_t *restrict event0 = view->event_word0;
    const uint64_t *restrict event1 = view->event_word1;
    const uint8_t *restrict event_kind = view->event_kind;
    const uint8_t *restrict divergent = view->event_divergent;
    uint64_t *restrict argument0 = view->effect_argument[0];
    uint64_t *restrict argument1 = view->effect_argument[1];
    uint64_t *restrict argument2 = view->effect_argument[2];
    uint32_t *restrict operation = view->effect_operation;
    uint16_t *restrict next_sites = view->effect_next_site;
    uint16_t *restrict effect_flags = view->effect_flags;
    const uint16_t normal_site =
        (uint16_t)((resume_site + 1U) %
                   config->site_count);
    const uint16_t divergent_site =
        (uint16_t)((resume_site + 3U) %
                   config->site_count);
    unsigned lane;

    SREM_VECTORIZE_LOOP
    for (lane = 0U; lane < width; ++lane) {
        const uint32_t active =
            (active_mask >> lane) & UINT32_C(1);
        const uint64_t active64 =
            UINT64_C(0) - (uint64_t)active;
        const uint32_t active32 =
            UINT32_C(0) - active;
        const uint16_t active16 =
            (uint16_t)(UINT16_C(0) - (uint16_t)active);
        const uint64_t old0 = field0[lane];
        const uint64_t old1 = field1[lane];
        const uint64_t old2 = field2[lane];
        const uint64_t old3 = field3[lane];
        const uint64_t old4 = field4[lane];
        const uint64_t old5 = field5[lane];
        const uint16_t old_steps = steps[lane];
        const uint64_t mixed =
            local_mix64(event0[lane] + old0 +
                        ((uint64_t)resume_site << 32U));
        const uint64_t new0 =
            mixed ^ rotl64(old1 + event1[lane], 11U);
        const uint64_t new1 =
            old1 +
            new0 * UINT64_C(0x94D049BB133111EB);
        const uint64_t new2 =
            old2 ^ rotl64(new0 + new1, 23U);
        const uint64_t new3 = old3 + (new2 ^ event0[lane]);
        const uint64_t new4 = rotl64(old4 + new3, 15U);
        const uint64_t new5 =
            old5 ^ (new0 + new4 + event1[lane]);
        const uint16_t next_site =
            divergent[lane] != 0U ? divergent_site :
                                    normal_site;
        const uint32_t new_flags =
            ((uint32_t)divergent[lane] << 8U) |
            (uint32_t)event_kind[lane];
        const uint32_t next_operation =
            (old_steps & 31U) == 0U ?
                SREM_MODEL_EFFECT_TIMER :
                SREM_MODEL_EFFECT_WRITE;

        field0[lane] = merge_u64(old0, new0, active64);
        field1[lane] = merge_u64(old1, new1, active64);
        field2[lane] = merge_u64(old2, new2, active64);
        field3[lane] = merge_u64(old3, new3, active64);
        field4[lane] = merge_u64(old4, new4, active64);
        field5[lane] = merge_u64(old5, new5, active64);
        sites[lane] =
            merge_u16(sites[lane], next_site, active16);
        steps[lane] =
            merge_u16(old_steps,
                      (uint16_t)(old_steps + 1U),
                      active16);
        flags[lane] =
            merge_u32(flags[lane], new_flags, active32);
        argument0[lane] =
            merge_u64(argument0[lane],
                      new0 ^ new2 ^ new5,
                      active64);
        argument1[lane] =
            merge_u64(argument1[lane],
                      new1 ^ new4,
                      active64);
        argument2[lane] =
            merge_u64(argument2[lane],
                      new2 + new5,
                      active64);
        operation[lane] =
            merge_u32(operation[lane],
                      next_operation,
                      active32);
        next_sites[lane] =
            merge_u16(next_sites[lane], next_site, active16);
        effect_flags[lane] =
            merge_u16(effect_flags[lane],
                      (uint16_t)new_flags,
                      active16);
    }
}

int srem_model_run_vector_superblock(
    srem_model_workload_t workload,
    const srem_model_config_t *config,
    srem_model_tile_view_t *view) {
    if (config == NULL || view == NULL ||
        workload < SREM_MODEL_HTTP_PIPELINE ||
        workload > SREM_MODEL_MIXED_FAIRNESS ||
        (view->width != 8U && view->width != 16U &&
         view->width != 32U) ||
        view->resume_site >= config->site_count ||
        (view->width < 32U &&
         (view->active_mask >> view->width) != 0U)) {
        return EINVAL;
    }
    switch (workload) {
    case SREM_MODEL_HTTP_PIPELINE:
        vector_http_pipeline(config, view);
        break;
    case SREM_MODEL_RPC_PIPELINE:
        vector_rpc_pipeline(config, view);
        break;
    case SREM_MODEL_DIVERGENT_CANCEL:
        vector_divergent_cancel(config, view);
        break;
    case SREM_MODEL_MIXED_FAIRNESS:
        vector_mixed_fairness(config, view);
        break;
    }
    return 0;
}

#undef SREM_VECTORIZE_LOOP
