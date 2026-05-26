/**
 * @file src/core/runtime_broker_transport_dispatch.c
 * @brief Thin broker request-dispatch entry wrappers.
 *
 * @details
 * The descriptor-aware dispatcher owns the real request semantics. These
 * wrappers keep call sites that do not pass descriptor/HANDLE authority on the
 * same normalized path without bloating the operation switch translation unit.
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

void llam_broker_process_request(llam_broker_t *broker,
                                 const llam_broker_wire_request_t *request,
                                 llam_broker_wire_response_t *response,
                                 bool *out_should_close) {
    llam_broker_process_request_with_descriptor(broker, request, response, out_should_close, LLAM_INVALID_HANDLE);
}

void llam_broker_process_request_with_descriptor(llam_broker_t *broker,
                                                 const llam_broker_wire_request_t *request,
                                                 llam_broker_wire_response_t *response,
                                                 bool *out_should_close,
                                                 llam_handle_t descriptor_handle) {
    llam_broker_process_request_with_descriptors(broker,
                                                 request,
                                                 response,
                                                 out_should_close,
                                                 descriptor_handle,
                                                 NULL);
}
