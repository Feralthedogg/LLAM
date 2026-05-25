/**
 * @file src/core/runtime_broker_transport_selftest.c
 * @brief Broker control-transport self-test request script.
 *
 * @details
 * Kept separate from the platform transport implementation so the transport
 * file stays focused on byte movement. This script exercises the authority
 * contract visible to an untrusted client: raw minting fails, bounded object
 * creation succeeds, over-rights requests fail, and stop closes the session.
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

static void llam_broker_request_init(llam_broker_wire_request_t *request, llam_broker_wire_op_t op) {
    memset(request, 0, sizeof(*request));
    request->magic = LLAM_BROKER_WIRE_MAGIC;
    request->version = LLAM_BROKER_WIRE_VERSION;
    request->op = (uint32_t)op;
}

static int llam_broker_expect_ok(const llam_broker_wire_response_t *response) {
    if (response->magic != LLAM_BROKER_WIRE_MAGIC || response->version != LLAM_BROKER_WIRE_VERSION) {
        errno = EINVAL;
        return -1;
    }
    if (response->status != 0) {
        errno = response->error_code != 0 ? response->error_code : EINVAL;
        return -1;
    }
    return 0;
}

static int llam_broker_expect_error(const llam_broker_wire_response_t *response, int expected) {
    if (response->magic != LLAM_BROKER_WIRE_MAGIC || response->version != LLAM_BROKER_WIRE_VERSION) {
        errno = EINVAL;
        return -1;
    }
    if (response->status == 0 || response->error_code != expected) {
        errno = response->status == 0 ? EACCES : response->error_code;
        return -1;
    }
    return 0;
}

int llam_broker_client_self_test_exchange(llam_broker_wire_request_fn_t request_fn, void *transport) {
    static const unsigned char buffer_payload[] = {'b', 'r', 'o', 'k', 'e', 'r'};
    static const unsigned char channel_payload[] = {'q', 'u', 'e', 'u', 'e'};
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    llam_capability_token_t buffer_token;
    llam_capability_token_t channel_token;
    llam_capability_token_t task_token;

    if (LLAM_UNLIKELY(request_fn == NULL)) {
        errno = EINVAL;
        return -1;
    }

    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_PING);
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_ok(&response) != 0) {
        return -1;
    }

    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_ISSUE_CAP);
    request.family = LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL;
    request.slot = 1U;
    request.generation = 1U;
    request.rights = LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_CLOSE;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_error(&response, EACCES) != 0) {
        return -1;
    }

    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_BUFFER);
    request.slot = 64U;
    request.rights = LLAM_CAP_RIGHT_READ | LLAM_CAP_RIGHT_WRITE;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_ok(&response) != 0) {
        return -1;
    }
    buffer_token = response.token;
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_VALIDATE_CAP);
    request.required_rights = LLAM_CAP_RIGHT_WRITE;
    request.token = buffer_token;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_ok(&response) != 0) {
        return -1;
    }
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_BUFFER_WRITE);
    request.token = buffer_token;
    request.offset = 4U;
    request.length = sizeof(buffer_payload);
    memcpy(request.data, buffer_payload, sizeof(buffer_payload));
    if (request_fn(transport, &request, &response) != 0 ||
        llam_broker_expect_ok(&response) != 0 ||
        response.result0 != sizeof(buffer_payload)) {
        return -1;
    }
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_BUFFER_READ);
    request.token = buffer_token;
    request.offset = 4U;
    request.length = sizeof(buffer_payload);
    if (request_fn(transport, &request, &response) != 0 ||
        llam_broker_expect_ok(&response) != 0 ||
        response.result0 != sizeof(buffer_payload) ||
        memcmp(response.data, buffer_payload, sizeof(buffer_payload)) != 0) {
        return -1;
    }
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_BUFFER_READ);
    request.token = buffer_token;
    request.offset = 0U;
    request.length = (uint64_t)LLAM_BROKER_WIRE_DATA_BYTES + 1U;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_error(&response, EINVAL) != 0) {
        return -1;
    }
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_ATTENUATE_CAP);
    request.token = buffer_token;
    request.rights = LLAM_CAP_RIGHT_READ;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_ok(&response) != 0) {
        return -1;
    }
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_BUFFER_WRITE);
    request.token = response.token;
    request.offset = 0U;
    request.length = 1U;
    request.data[0] = 0x5aU;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_error(&response, EACCES) != 0) {
        return -1;
    }

    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_CHANNEL);
    request.slot = 2U;
    request.rights = LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_RECV | LLAM_CAP_RIGHT_CLOSE;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_ok(&response) != 0) {
        return -1;
    }
    channel_token = response.token;
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_VALIDATE_CAP);
    request.required_rights = LLAM_CAP_RIGHT_SEND | LLAM_CAP_RIGHT_CLOSE;
    request.token = channel_token;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_ok(&response) != 0) {
        return -1;
    }
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_CHANNEL_SEND);
    request.token = channel_token;
    request.length = sizeof(channel_payload);
    memcpy(request.data, channel_payload, sizeof(channel_payload));
    if (request_fn(transport, &request, &response) != 0 ||
        llam_broker_expect_ok(&response) != 0 ||
        response.result0 != sizeof(channel_payload)) {
        return -1;
    }
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_CHANNEL_RECV);
    request.token = channel_token;
    request.length = LLAM_BROKER_WIRE_DATA_BYTES;
    if (request_fn(transport, &request, &response) != 0 ||
        llam_broker_expect_ok(&response) != 0 ||
        response.result0 != sizeof(channel_payload) ||
        memcmp(response.data, channel_payload, sizeof(channel_payload)) != 0) {
        return -1;
    }
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_CHANNEL_CLOSE);
    request.token = channel_token;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_ok(&response) != 0) {
        return -1;
    }
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_CHANNEL_SEND);
    request.token = channel_token;
    request.length = 1U;
    request.data[0] = 1U;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_error(&response, EPIPE) != 0) {
        return -1;
    }

    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_TASK_SPAWN);
    request.slot = LLAM_BROKER_TASK_KIND_INCREMENT_U64;
    request.offset = 41U;
    request.rights = LLAM_CAP_RIGHT_JOIN | LLAM_CAP_RIGHT_DETACH;
    if (request_fn(transport, &request, &response) != 0 ||
        llam_broker_expect_ok(&response) != 0 ||
        response.token.family != LLAM_BROKER_CAP_FAMILY_TASK) {
        return -1;
    }
    task_token = response.token;
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_TASK_JOIN);
    request.token = task_token;
    if (request_fn(transport, &request, &response) != 0 ||
        llam_broker_expect_ok(&response) != 0 ||
        response.result0 != 42U) {
        return -1;
    }
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_TASK_JOIN);
    request.token = task_token;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_error(&response, EACCES) != 0) {
        return -1;
    }
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_TASK_SPAWN);
    request.slot = UINT64_C(0xffffffff);
    request.rights = LLAM_CAP_RIGHT_JOIN;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_error(&response, EINVAL) != 0) {
        return -1;
    }
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_TASK_SPAWN);
    request.slot = LLAM_BROKER_TASK_KIND_RETURN_U64;
    request.offset = 7U;
    request.rights = LLAM_CAP_RIGHT_ADMIN;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_error(&response, EACCES) != 0) {
        return -1;
    }
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_TASK_SPAWN);
    request.slot = LLAM_BROKER_TASK_KIND_RETURN_U64;
    request.offset = 7U;
    request.rights = LLAM_CAP_RIGHT_DETACH;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_ok(&response) != 0) {
        return -1;
    }
    task_token = response.token;
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_TASK_DETACH);
    request.token = task_token;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_ok(&response) != 0) {
        return -1;
    }
    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_TASK_JOIN);
    request.token = task_token;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_error(&response, EACCES) != 0) {
        return -1;
    }

    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_CREATE_BUFFER);
    request.slot = 16U;
    request.rights = LLAM_CAP_RIGHT_ADMIN;
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_error(&response, EACCES) != 0) {
        return -1;
    }

    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_REVOKE_ALL);
    if (request_fn(transport, &request, &response) != 0 || llam_broker_expect_error(&response, EACCES) != 0) {
        return -1;
    }

    llam_broker_request_init(&request, LLAM_BROKER_WIRE_OP_STOP);
    return request_fn(transport, &request, &response) == 0 ? llam_broker_expect_ok(&response) : -1;
}
