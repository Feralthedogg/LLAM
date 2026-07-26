// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#include "lccf_model_internal.h"

#include <stdint.h>

static uint64_t rotl64(uint64_t value, unsigned shift) {
    shift &= 63U;
    return (value << shift) | (value >> ((64U - shift) & 63U));
}

static uint32_t next_site(uint32_t site,
                          uint64_t output,
                          unsigned site_count) {
    return (site + 1U + (uint32_t)(output & UINT64_C(1))) %
           site_count;
}

static bool segment_continues(const lccf_model_frame_core_t *frame,
                              const lccf_model_config_t *config) {
    return (frame->steps % config->chain_length) != 0U;
}

static void finish_resume(lccf_model_frame_core_t *frame,
                          lccf_model_command_t *command,
                          const lccf_model_config_t *config,
                          unsigned resume_site,
                          uint64_t output,
                          lccf_model_command_kind_t final_kind) {
    frame->output = output;
    frame->steps += 1U;
    command->output = output;
    command->next_site =
        next_site((uint32_t)resume_site, output, config->site_count);
    command->kind = segment_continues(frame, config) ?
                        LCCF_MODEL_COMMAND_CONTINUE :
                        (uint32_t)final_kind;
}

static void resume_io_pipeline(lccf_model_frame_core_t *frame,
                               const lccf_model_event_t *event,
                               lccf_model_command_t *command,
                               const lccf_model_config_t *config,
                               unsigned resume_site) {
    const uint64_t site_salt =
        (uint64_t)(resume_site + 1U) *
        UINT64_C(0x9E3779B97F4A7C15);
    const uint64_t x =
        event->word0 ^
        rotl64(event->word1 + frame->state0 + site_salt, 13U);

    frame->state0 =
        (x * UINT64_C(0x9E3779B185EBCA87)) ^ frame->state1;
    frame->state1 =
        rotl64(frame->state1 + x + UINT64_C(0xC2B2AE3D27D4EB4F),
               17U);
    finish_resume(frame,
                  command,
                  config,
                  resume_site,
                  frame->state0 ^ rotl64(frame->state1, 7U),
                  LCCF_MODEL_COMMAND_WAIT_IO);
}

static void resume_rpc_state(lccf_model_frame_core_t *frame,
                             const lccf_model_event_t *event,
                             lccf_model_command_t *command,
                             const lccf_model_config_t *config,
                             unsigned resume_site) {
    const uint32_t opcode =
        (uint32_t)(event->word0 ^ frame->state0 ^ resume_site) & 3U;
    const uint64_t success =
        frame->state0 + event->word1 +
        UINT64_C(0x165667B19E3779F9);
    const uint64_t retry =
        (frame->state0 ^ event->word1) *
        UINT64_C(0xD6E8FEB86659FD93);

    frame->state0 = (opcode & 1U) != 0U ? retry : success;
    frame->state1 =
        rotl64(frame->state1 ^ frame->state0,
               (opcode + resume_site + 5U) & 63U);
    frame->state2 +=
        (opcode == 3U ? event->word0 : event->word1) ^
        frame->state1;
    finish_resume(frame,
                  command,
                  config,
                  resume_site,
                  frame->state0 + frame->state1 + frame->state2 +
                      opcode,
                  opcode == 3U ? LCCF_MODEL_COMMAND_YIELD :
                                 LCCF_MODEL_COMMAND_WAIT_IO);
}

static void resume_timer_cancel(lccf_model_frame_core_t *frame,
                                const lccf_model_event_t *event,
                                lccf_model_command_t *command,
                                const lccf_model_config_t *config,
                                unsigned resume_site) {
    const uint64_t winner =
        ((uint64_t)event->kind << 56U) ^ event->word0;
    const uint64_t completed =
        frame->state0 + winner + UINT64_C(0xA0761D6478BD642F);
    const uint64_t cancelled =
        frame->state0 ^ rotl64(event->word1, 29U);

    frame->state0 =
        event->kind == LCCF_MODEL_EVENT_CANCEL ? cancelled : completed;
    frame->state1 =
        rotl64(frame->state1 + frame->state0 + event->word1,
               (event->kind + resume_site + 7U) & 63U);
    frame->state2 ^=
        frame->state0 + (uint64_t)event->kind *
                            UINT64_C(0xE7037ED1A0B428DB);
    finish_resume(
        frame,
        command,
        config,
        resume_site,
        frame->state0 ^ frame->state1 ^ frame->state2,
        event->kind == LCCF_MODEL_EVENT_TIMEOUT ?
            LCCF_MODEL_COMMAND_WAIT_TIMER :
            (event->kind == LCCF_MODEL_EVENT_CANCEL ?
                 LCCF_MODEL_COMMAND_YIELD :
                 LCCF_MODEL_COMMAND_WAIT_IO));
}

static void resume_mixed_fairness(lccf_model_frame_core_t *frame,
                                  const lccf_model_event_t *event,
                                  lccf_model_command_t *command,
                                  const lccf_model_config_t *config,
                                  unsigned resume_site) {
    const uint64_t mixed =
        lccf_model_mix64(event->word0 + frame->state0 +
                         ((uint64_t)resume_site << 32U));

    frame->state0 =
        mixed ^ rotl64(frame->state1 + event->word1, 11U);
    frame->state1 +=
        frame->state0 * UINT64_C(0x94D049BB133111EB);
    frame->state2 ^= rotl64(frame->state0 + frame->state1, 23U);
    finish_resume(frame,
                  command,
                  config,
                  resume_site,
                  frame->state0 ^ frame->state1 ^ frame->state2,
                  (frame->steps & 31U) == 0U ?
                      LCCF_MODEL_COMMAND_WAIT_TIMER :
                      LCCF_MODEL_COMMAND_WAIT_IO);
}

#define LCCF_SITE_TABLE(fn)                                                    \
    {                                                                          \
        (fn), (fn), (fn), (fn), (fn), (fn), (fn), (fn)                       \
    }

static const lccf_model_workload_ops_t WORKLOAD_OPS[] = {
    {LCCF_SITE_TABLE(resume_io_pipeline)},
    {LCCF_SITE_TABLE(resume_rpc_state)},
    {LCCF_SITE_TABLE(resume_timer_cancel)},
    {LCCF_SITE_TABLE(resume_mixed_fairness)},
};

#undef LCCF_SITE_TABLE

const lccf_model_workload_ops_t *
lccf_model_get_workload_ops(lccf_model_workload_t workload) {
    if (workload < LCCF_MODEL_COMPLETION_IO_PIPELINE ||
        workload > LCCF_MODEL_COMPLETION_MIXED_FAIRNESS) {
        return NULL;
    }
    return &WORKLOAD_OPS[(unsigned)workload];
}
