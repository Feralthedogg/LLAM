/**
 * @file src/core/broker/broker_windows_security.c
 * @brief Shared Windows broker security descriptor implementation.
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
#include "runtime_broker_windows_security.h"

#if LLAM_PLATFORM_WINDOWS

#include <stdlib.h>
#include <string.h>

void llam_broker_windows_security_cleanup(llam_broker_windows_security_t *security) {
    if (security == NULL) {
        return;
    }
    free(security->dacl);
    free(security->token_user);
    memset(security, 0, sizeof(*security));
}

int llam_broker_windows_security_init(llam_broker_windows_security_t *security,
                                      DWORD access_mask) {
    HANDLE token = NULL;
    BYTE system_sid[SECURITY_MAX_SID_SIZE];
    BYTE admins_sid[SECURITY_MAX_SID_SIZE];
    DWORD token_user_bytes = 0U;
    DWORD system_sid_bytes = sizeof(system_sid);
    DWORD admins_sid_bytes = sizeof(admins_sid);
    DWORD dacl_bytes;
    int rc = -1;

    if (LLAM_UNLIKELY(security == NULL || access_mask == 0U)) {
        errno = EINVAL;
        return -1;
    }
    memset(security, 0, sizeof(*security));
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        errno = llam_windows_system_error_to_errno(GetLastError());
        return -1;
    }
    if (GetTokenInformation(token, TokenUser, NULL, 0U, &token_user_bytes) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        token_user_bytes == 0U) {
        errno = llam_windows_system_error_to_errno(GetLastError());
        goto done;
    }
    security->token_user = (TOKEN_USER *)calloc(1U, token_user_bytes);
    if (security->token_user == NULL) {
        errno = ENOMEM;
        goto done;
    }
    if (!GetTokenInformation(token, TokenUser, security->token_user, token_user_bytes, &token_user_bytes) ||
        !CreateWellKnownSid(WinLocalSystemSid, NULL, system_sid, &system_sid_bytes) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, admins_sid, &admins_sid_bytes)) {
        errno = llam_windows_system_error_to_errno(GetLastError());
        goto done;
    }
    dacl_bytes = (DWORD)sizeof(ACL) +
                 (DWORD)((sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) * 3U) +
                 GetLengthSid(security->token_user->User.Sid) +
                 GetLengthSid(system_sid) +
                 GetLengthSid(admins_sid);
    security->dacl = (ACL *)calloc(1U, dacl_bytes);
    if (security->dacl == NULL) {
        errno = ENOMEM;
        goto done;
    }
    if (!InitializeAcl(security->dacl, dacl_bytes, ACL_REVISION) ||
        !AddAccessAllowedAce(security->dacl, ACL_REVISION, access_mask, security->token_user->User.Sid) ||
        !AddAccessAllowedAce(security->dacl, ACL_REVISION, access_mask, system_sid) ||
        !AddAccessAllowedAce(security->dacl, ACL_REVISION, access_mask, admins_sid) ||
        !InitializeSecurityDescriptor(&security->descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&security->descriptor, TRUE, security->dacl, FALSE)) {
        errno = llam_windows_system_error_to_errno(GetLastError());
        goto done;
    }
    security->attrs.nLength = sizeof(security->attrs);
    security->attrs.lpSecurityDescriptor = &security->descriptor;
    security->attrs.bInheritHandle = FALSE;
    rc = 0;

done:
    if (token != NULL) {
        CloseHandle(token);
    }
    if (rc != 0) {
        llam_broker_windows_security_cleanup(security);
    }
    return rc;
}

#endif
