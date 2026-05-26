/**
 * @file src/core/runtime_broker_transport_response.c
 * @brief Broker wire-response validation and failure-output scrubbing.
 *
 * @details
 * Failed broker responses must not carry token, descriptor, result, or data
 * authority. Keep that policy centralized so POSIX and Windows request helpers
 * normalize hostile or malformed peers the same way.
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
#include "runtime_broker.h"

#include <string.h>

void llam_broker_mark_response_failure_clear_outputs(llam_broker_wire_response_t *response, int error_code) {
    if (response == NULL) {
        return;
    }

    response->status = -1;
    response->error_code = error_code != 0 ? error_code : EIO;
    response->result0 = 0U;
    response->result1 = 0U;
    response->result2 = 0U;
    memset(&response->token, 0, sizeof(response->token));
    memset(response->data, 0, sizeof(response->data));
}

void llam_broker_normalize_response_failure_outputs(llam_broker_wire_response_t *response) {
    int error_code;

    if (response == NULL || response->status == 0) {
        return;
    }
    /*
     * A failed response must never carry object authority.  This is a defense
     * against faulty or hostile broker peers that set status != 0 while leaving
     * token/result/data fields populated.
     */
    error_code = response->error_code != 0 ? response->error_code : EIO;
    llam_broker_mark_response_failure_clear_outputs(response, error_code);
}

int llam_broker_validate_response_frame_or_clear(llam_broker_wire_response_t *response) {
    if (LLAM_UNLIKELY(response == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(response->magic != LLAM_BROKER_WIRE_MAGIC ||
                      response->version != LLAM_BROKER_WIRE_VERSION)) {
        /*
         * Malformed response framing means none of the payload can be trusted,
         * even if status claims success. Clear the full response rather than
         * preserving attacker-controlled token/result/data fields.
         */
        return llam_broker_fail_clear_output(response, sizeof(*response), EINVAL);
    }
    llam_broker_normalize_response_failure_outputs(response);
    return 0;
}
