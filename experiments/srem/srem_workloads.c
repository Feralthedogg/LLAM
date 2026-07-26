// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#include "srem_model_internal.h"

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
