/**
 * @file src/internal/runtime_broker_windows_security.h
 * @brief Shared Windows broker security descriptor helpers.
 *
 * @details
 * Broker Windows objects such as named pipes and file mappings must fail
 * closed unless they can be created with explicit current-user, LocalSystem,
 * and Administrators access control. This internal helper keeps that policy
 * identical across transport and ring backends.
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

#ifndef LLAM_RUNTIME_BROKER_WINDOWS_SECURITY_H
#define LLAM_RUNTIME_BROKER_WINDOWS_SECURITY_H

#if LLAM_PLATFORM_WINDOWS

#include "runtime_windows_compat.h"

typedef struct llam_broker_windows_security {
    SECURITY_ATTRIBUTES attrs;
    SECURITY_DESCRIPTOR descriptor;
    ACL *dacl;
    TOKEN_USER *token_user;
} llam_broker_windows_security_t;

int llam_broker_windows_security_init(llam_broker_windows_security_t *security,
                                      DWORD access_mask);
void llam_broker_windows_security_cleanup(llam_broker_windows_security_t *security);

#endif

#endif
