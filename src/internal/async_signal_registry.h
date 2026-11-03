/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSES/Apache-2.0.txt.
 */

#ifndef LLAM_ASYNC_SIGNAL_REGISTRY_H
#define LLAM_ASYNC_SIGNAL_REGISTRY_H

#if (LLAM_PLATFORM_LINUX || LLAM_ASYNC_CONTEXT_SIGNAL) && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
/* Ordinary-context exclusion shared by wait-sets and async handler owners.
 * Claim returns an error number, not -1/errno. The owner must release exactly
 * once after rollback or final disposition teardown. Never call in a handler. */
int llam_async_signal_claim_number(int signal_number);
void llam_async_signal_release_number(int signal_number);
#endif

#endif
