/**
 * @file src/core/base/io_udata.c
 * @brief Encoding and decoding helpers for backend I/O user data tags.
 *
 * @details
 * Backend completion APIs carry a single integer user-data value. LLAM stores a
 * pointer plus a small tag in that value; the tag identifies whether completion
 * dispatch should treat the pointer as a request, watch, or control operation.
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

_Static_assert(LLAM_IO_UDATA_CONTROL <= 7U, "LLAM I/O user-data tags must fit in three low bits");
_Static_assert(_Alignof(llam_io_req_t) >= 8U, "LLAM I/O requests must keep three tag bits clear");
_Static_assert(_Alignof(llam_poll_watch_t) >= 8U, "LLAM poll watches must keep three tag bits clear");
_Static_assert(_Alignof(llam_accept_watch_t) >= 8U, "LLAM accept watches must keep three tag bits clear");
_Static_assert(_Alignof(llam_recv_watch_t) >= 8U, "LLAM recv watches must keep three tag bits clear");
_Static_assert(_Alignof(llam_io_control_op_t) >= 8U, "LLAM I/O control ops must keep three tag bits clear");

/**
 * @brief Encode a pointer and low-bit tag for backend completion user data.
 *
 * Runtime-allocated request/watch/control objects are aligned enough that the
 * low three bits are available for tags.
 *
 * @param ptr Pointer to encode.
 * @param tag User-data tag.
 *
 * @return Encoded user-data value.
 */
uint64_t llam_io_udata_encode(void *ptr, unsigned tag) {
    return ((uint64_t)(uintptr_t)ptr) | (uint64_t)(tag & 0x7U);
}

/**
 * @brief Extract the low-bit tag from encoded user data.
 *
 * @param user_data Encoded user-data value.
 *
 * @return Tag value in the range [0, 7].
 */
unsigned llam_io_udata_tag(uint64_t user_data) {
    return (unsigned)(user_data & 0x7U);
}

/**
 * @brief Extract the original pointer from encoded user data.
 *
 * @param user_data Encoded user-data value.
 *
 * @return Decoded pointer with tag bits cleared.
 */
void *llam_io_udata_ptr(uint64_t user_data) {
    return (void *)(uintptr_t)(user_data & ~(uint64_t)0x7U);
}
