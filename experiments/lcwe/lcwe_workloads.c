// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#include "lcwe_model_internal.h"

#include <stdint.h>

static uint64_t rotl64(uint64_t value, unsigned shift) {
    shift &= 63U;
    return (value << shift) | (value >> ((64U - shift) & 63U));
}

static void finish_resume(lcwe_model_frame_t *frame,
                          lcwe_model_command_t *command,
                          unsigned site_count,
                          uint64_t output) {
    frame->output = output;
    frame->steps += 1U;
    command->kind = 1U;
    command->output = output;
    command->next_site =
        (frame->site + 1U + (unsigned)(output & UINT64_C(1))) % site_count;
}

static void resume_io_pipeline(lcwe_model_frame_t *frame,
                               const lcwe_model_event_t *event,
                               lcwe_model_command_t *command,
                               unsigned site_count) {
    const uint64_t x =
        event->word0 ^ rotl64(event->word1 + frame->state0, 13U);

    frame->state0 =
        (x * UINT64_C(0x9E3779B185EBCA87)) ^ frame->state1;
    frame->state1 = rotl64(frame->state1 + x +
                               UINT64_C(0xC2B2AE3D27D4EB4F),
                           17U);
    finish_resume(frame,
                  command,
                  site_count,
                  frame->state0 ^ rotl64(frame->state1, 7U));
}

static void resume_rpc_state(lcwe_model_frame_t *frame,
                             const lcwe_model_event_t *event,
                             lcwe_model_command_t *command,
                             unsigned site_count) {
    const uint32_t opcode =
        (uint32_t)(event->word0 ^ frame->state0) & 3U;
    const uint64_t candidate0 =
        frame->state0 + event->word1 + UINT64_C(0x165667B19E3779F9);
    const uint64_t candidate1 =
        (frame->state0 ^ event->word1) *
        UINT64_C(0xD6E8FEB86659FD93);

    frame->state0 = (opcode & 1U) != 0U ? candidate1 : candidate0;
    frame->state1 =
        rotl64(frame->state1 ^ frame->state0, (opcode + 5U) & 63U);
    finish_resume(frame,
                  command,
                  site_count,
                  frame->state0 + frame->state1 + opcode);
}

static void resume_event_fanout(lcwe_model_frame_t *frame,
                                const lcwe_model_event_t *event,
                                lcwe_model_command_t *command,
                                unsigned site_count) {
    frame->state0 =
        frame->state0 * UINT64_C(6364136223846793005) +
        event->word0 + UINT64_C(1442695040888963407);
    frame->state1 ^= rotl64(frame->state0 + event->word1, 23U);
    frame->state2 += frame->state0 ^ frame->state1;
    finish_resume(frame,
                  command,
                  site_count,
                  frame->state2 ^ rotl64(frame->state1, 11U));
}

static const lcwe_model_workload_ops_t WORKLOAD_OPS[] = {
    {resume_io_pipeline},
    {resume_rpc_state},
    {resume_event_fanout},
};

const lcwe_model_workload_ops_t *
lcwe_model_get_workload_ops(lcwe_model_workload_t workload) {
    if (workload < LCWE_MODEL_EXEC_IO_PIPELINE ||
        workload > LCWE_MODEL_EXEC_EVENT_FANOUT) {
        return NULL;
    }
    return &WORKLOAD_OPS[(unsigned)workload];
}
