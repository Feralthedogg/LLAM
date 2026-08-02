// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// See LICENSES/OLD-LICENSE/Apache-2.0.txt.

#include "lcwe_model_internal.h"

#include <stdint.h>

#if defined(LCWE_MODEL_ENABLE_VECTOR_HINTS) && defined(__clang__)
#define LCWE_MODEL_VECTORIZE                                                   \
    _Pragma("clang loop vectorize(enable) interleave(enable)")
#elif defined(LCWE_MODEL_ENABLE_VECTOR_HINTS) && defined(__GNUC__)
#define LCWE_MODEL_VECTORIZE _Pragma("GCC ivdep")
#else
#define LCWE_MODEL_VECTORIZE
#endif

static uint64_t rotl64(uint64_t value, unsigned shift) {
    shift &= 63U;
    return (value << shift) | (value >> ((64U - shift) & 63U));
}

static uint32_t next_site(uint32_t site,
                          uint64_t output,
                          unsigned site_count) {
    return (site + 1U + (unsigned)(output & UINT64_C(1))) % site_count;
}

static void finish_resume(lcwe_model_frame_t *frame,
                          lcwe_model_command_t *command,
                          unsigned site_count,
                          uint64_t output) {
    frame->output = output;
    frame->steps += 1U;
    command->kind = 1U;
    command->output = output;
    command->next_site = next_site(frame->site, output, site_count);
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

static void pointers_io_pipeline(lcwe_model_ticket_t *const *tickets,
                                 unsigned lane_count,
                                 unsigned site_count) {
    unsigned lane;

    for (lane = 0U; lane < lane_count; ++lane) {
        lcwe_model_ticket_t *ticket = tickets[lane];
        lcwe_model_frame_t *frame = ticket->frame;
        const uint64_t x =
            ticket->event.word0 ^
            rotl64(ticket->event.word1 + frame->state0, 13U);
        const uint64_t state0 =
            (x * UINT64_C(0x9E3779B185EBCA87)) ^ frame->state1;
        const uint64_t state1 =
            rotl64(frame->state1 + x +
                       UINT64_C(0xC2B2AE3D27D4EB4F),
                   17U);
        const uint64_t output = state0 ^ rotl64(state1, 7U);

        frame->state0 = state0;
        frame->state1 = state1;
        frame->output = output;
        frame->steps += 1U;
        ticket->command.output = output;
        ticket->command.next_site =
            next_site(frame->site, output, site_count);
        ticket->command.kind = 1U;
    }
}

static void pointers_rpc_state(lcwe_model_ticket_t *const *tickets,
                               unsigned lane_count,
                               unsigned site_count) {
    unsigned lane;

    for (lane = 0U; lane < lane_count; ++lane) {
        lcwe_model_ticket_t *ticket = tickets[lane];
        lcwe_model_frame_t *frame = ticket->frame;
        const uint32_t opcode =
            (uint32_t)(ticket->event.word0 ^ frame->state0) & 3U;
        const uint64_t candidate0 =
            frame->state0 + ticket->event.word1 +
            UINT64_C(0x165667B19E3779F9);
        const uint64_t candidate1 =
            (frame->state0 ^ ticket->event.word1) *
            UINT64_C(0xD6E8FEB86659FD93);
        const uint64_t state0 =
            (opcode & 1U) != 0U ? candidate1 : candidate0;
        const uint64_t state1 =
            rotl64(frame->state1 ^ state0, (opcode + 5U) & 63U);
        const uint64_t output = state0 + state1 + opcode;

        frame->state0 = state0;
        frame->state1 = state1;
        frame->output = output;
        frame->steps += 1U;
        ticket->command.output = output;
        ticket->command.next_site =
            next_site(frame->site, output, site_count);
        ticket->command.kind = 1U;
    }
}

static void pointers_event_fanout(lcwe_model_ticket_t *const *tickets,
                                  unsigned lane_count,
                                  unsigned site_count) {
    unsigned lane;

    for (lane = 0U; lane < lane_count; ++lane) {
        lcwe_model_ticket_t *ticket = tickets[lane];
        lcwe_model_frame_t *frame = ticket->frame;
        const uint64_t state0 =
            frame->state0 * UINT64_C(6364136223846793005) +
            ticket->event.word0 + UINT64_C(1442695040888963407);
        const uint64_t state1 =
            frame->state1 ^
            rotl64(state0 + ticket->event.word1, 23U);
        const uint64_t state2 =
            frame->state2 + (state0 ^ state1);
        const uint64_t output = state2 ^ rotl64(state1, 11U);

        frame->state0 = state0;
        frame->state1 = state1;
        frame->state2 = state2;
        frame->output = output;
        frame->steps += 1U;
        ticket->command.output = output;
        ticket->command.next_site =
            next_site(frame->site, output, site_count);
        ticket->command.kind = 1U;
    }
}

static void capsule_io_pipeline(lcwe_model_capsule_t *capsule,
                                unsigned lane_count,
                                unsigned site_count) {
    unsigned lane;

    LCWE_MODEL_VECTORIZE
    for (lane = 0U; lane < lane_count; ++lane) {
        const uint64_t x =
            capsule->event0[lane] ^
            rotl64(capsule->event1[lane] + capsule->state0[lane], 13U);
        const uint64_t state0 =
            (x * UINT64_C(0x9E3779B185EBCA87)) ^
            capsule->state1[lane];
        const uint64_t state1 =
            rotl64(capsule->state1[lane] + x +
                       UINT64_C(0xC2B2AE3D27D4EB4F),
                   17U);
        const uint64_t output = state0 ^ rotl64(state1, 7U);

        capsule->state0[lane] = state0;
        capsule->state1[lane] = state1;
        capsule->output[lane] = output;
        capsule->steps[lane] += 1U;
        capsule->next_site[lane] =
            next_site(capsule->site[lane], output, site_count);
    }
}

static void capsule_rpc_state(lcwe_model_capsule_t *capsule,
                              unsigned lane_count,
                              unsigned site_count) {
    unsigned lane;

    LCWE_MODEL_VECTORIZE
    for (lane = 0U; lane < lane_count; ++lane) {
        const uint32_t opcode =
            (uint32_t)(capsule->event0[lane] ^
                       capsule->state0[lane]) &
            3U;
        const uint64_t candidate0 =
            capsule->state0[lane] + capsule->event1[lane] +
            UINT64_C(0x165667B19E3779F9);
        const uint64_t candidate1 =
            (capsule->state0[lane] ^ capsule->event1[lane]) *
            UINT64_C(0xD6E8FEB86659FD93);
        const uint64_t state0 =
            (opcode & 1U) != 0U ? candidate1 : candidate0;
        const uint64_t state1 =
            rotl64(capsule->state1[lane] ^ state0,
                   (opcode + 5U) & 63U);
        const uint64_t output = state0 + state1 + opcode;

        capsule->state0[lane] = state0;
        capsule->state1[lane] = state1;
        capsule->output[lane] = output;
        capsule->steps[lane] += 1U;
        capsule->next_site[lane] =
            next_site(capsule->site[lane], output, site_count);
    }
}

static void capsule_event_fanout(lcwe_model_capsule_t *capsule,
                                 unsigned lane_count,
                                 unsigned site_count) {
    unsigned lane;

    LCWE_MODEL_VECTORIZE
    for (lane = 0U; lane < lane_count; ++lane) {
        const uint64_t state0 =
            capsule->state0[lane] *
                UINT64_C(6364136223846793005) +
            capsule->event0[lane] +
            UINT64_C(1442695040888963407);
        const uint64_t state1 =
            capsule->state1[lane] ^
            rotl64(state0 + capsule->event1[lane], 23U);
        const uint64_t state2 =
            capsule->state2[lane] + (state0 ^ state1);
        const uint64_t output = state2 ^ rotl64(state1, 11U);

        capsule->state0[lane] = state0;
        capsule->state1[lane] = state1;
        capsule->state2[lane] = state2;
        capsule->output[lane] = output;
        capsule->steps[lane] += 1U;
        capsule->next_site[lane] =
            next_site(capsule->site[lane], output, site_count);
    }
}

static void aosoa_io_pipeline(lcwe_model_aosoa_t *aosoa,
                              size_t first,
                              unsigned lane_count,
                              unsigned site_count) {
    uint64_t *restrict state0 = aosoa->state0;
    uint64_t *restrict state1 = aosoa->state1;
    uint64_t *restrict event0 = aosoa->event0;
    uint64_t *restrict event1 = aosoa->event1;
    uint64_t *restrict output_array = aosoa->output;
    uint64_t *restrict command_output = aosoa->command_output;
    uint32_t *restrict site = aosoa->site;
    uint32_t *restrict next_site_array = aosoa->next_site;
    uint32_t *restrict steps = aosoa->steps;
    unsigned lane;

    LCWE_MODEL_VECTORIZE
    for (lane = 0U; lane < lane_count; ++lane) {
        const size_t index = first + lane;
        const uint64_t x =
            event0[index] ^ rotl64(event1[index] + state0[index], 13U);
        const uint64_t new_state0 =
            (x * UINT64_C(0x9E3779B185EBCA87)) ^ state1[index];
        const uint64_t new_state1 =
            rotl64(state1[index] + x +
                       UINT64_C(0xC2B2AE3D27D4EB4F),
                   17U);
        const uint64_t output =
            new_state0 ^ rotl64(new_state1, 7U);

        state0[index] = new_state0;
        state1[index] = new_state1;
        output_array[index] = output;
        command_output[index] = output;
        steps[index] += 1U;
        next_site_array[index] =
            next_site(site[index], output, site_count);
    }
}

static void aosoa_rpc_state(lcwe_model_aosoa_t *aosoa,
                            size_t first,
                            unsigned lane_count,
                            unsigned site_count) {
    uint64_t *restrict state0 = aosoa->state0;
    uint64_t *restrict state1 = aosoa->state1;
    uint64_t *restrict event0 = aosoa->event0;
    uint64_t *restrict event1 = aosoa->event1;
    uint64_t *restrict output_array = aosoa->output;
    uint64_t *restrict command_output = aosoa->command_output;
    uint32_t *restrict site = aosoa->site;
    uint32_t *restrict next_site_array = aosoa->next_site;
    uint32_t *restrict steps = aosoa->steps;
    unsigned lane;

    LCWE_MODEL_VECTORIZE
    for (lane = 0U; lane < lane_count; ++lane) {
        const size_t index = first + lane;
        const uint32_t opcode =
            (uint32_t)(event0[index] ^ state0[index]) & 3U;
        const uint64_t candidate0 =
            state0[index] + event1[index] +
            UINT64_C(0x165667B19E3779F9);
        const uint64_t candidate1 =
            (state0[index] ^ event1[index]) *
            UINT64_C(0xD6E8FEB86659FD93);
        const uint64_t new_state0 =
            (opcode & 1U) != 0U ? candidate1 : candidate0;
        const uint64_t new_state1 =
            rotl64(state1[index] ^ new_state0,
                   (opcode + 5U) & 63U);
        const uint64_t output = new_state0 + new_state1 + opcode;

        state0[index] = new_state0;
        state1[index] = new_state1;
        output_array[index] = output;
        command_output[index] = output;
        steps[index] += 1U;
        next_site_array[index] =
            next_site(site[index], output, site_count);
    }
}

static void aosoa_event_fanout(lcwe_model_aosoa_t *aosoa,
                               size_t first,
                               unsigned lane_count,
                               unsigned site_count) {
    uint64_t *restrict state0 = aosoa->state0;
    uint64_t *restrict state1 = aosoa->state1;
    uint64_t *restrict state2 = aosoa->state2;
    uint64_t *restrict event0 = aosoa->event0;
    uint64_t *restrict event1 = aosoa->event1;
    uint64_t *restrict output_array = aosoa->output;
    uint64_t *restrict command_output = aosoa->command_output;
    uint32_t *restrict site = aosoa->site;
    uint32_t *restrict next_site_array = aosoa->next_site;
    uint32_t *restrict steps = aosoa->steps;
    unsigned lane;

    LCWE_MODEL_VECTORIZE
    for (lane = 0U; lane < lane_count; ++lane) {
        const size_t index = first + lane;
        const uint64_t new_state0 =
            state0[index] * UINT64_C(6364136223846793005) +
            event0[index] + UINT64_C(1442695040888963407);
        const uint64_t new_state1 =
            state1[index] ^ rotl64(new_state0 + event1[index], 23U);
        const uint64_t new_state2 =
            state2[index] + (new_state0 ^ new_state1);
        const uint64_t output =
            new_state2 ^ rotl64(new_state1, 11U);

        state0[index] = new_state0;
        state1[index] = new_state1;
        state2[index] = new_state2;
        output_array[index] = output;
        command_output[index] = output;
        steps[index] += 1U;
        next_site_array[index] =
            next_site(site[index], output, site_count);
    }
}

static const lcwe_model_workload_ops_t WORKLOAD_OPS[] = {
    {
        resume_io_pipeline,
        pointers_io_pipeline,
        capsule_io_pipeline,
        aosoa_io_pipeline,
    },
    {
        resume_rpc_state,
        pointers_rpc_state,
        capsule_rpc_state,
        aosoa_rpc_state,
    },
    {
        resume_event_fanout,
        pointers_event_fanout,
        capsule_event_fanout,
        aosoa_event_fanout,
    },
};

const lcwe_model_workload_ops_t *
lcwe_model_get_workload_ops(lcwe_model_workload_t workload) {
    if (workload < LCWE_MODEL_EXEC_IO_PIPELINE ||
        workload > LCWE_MODEL_EXEC_EVENT_FANOUT) {
        return NULL;
    }
    return &WORKLOAD_OPS[(unsigned)workload];
}
