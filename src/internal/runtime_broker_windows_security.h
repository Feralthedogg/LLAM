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
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 * Licensed under the LLAM Commercial Reciprocity License 1.0.
 * See the LICENSE file distributed with this Software.
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
