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

#ifndef LLAM_ASYNC_CONTEXT_PHASE_H
#define LLAM_ASYNC_CONTEXT_PHASE_H

/* Shared with native transition assembly. A phase never grants ownership:
 * the backend must also validate its exact generation and reader obligations. */
#define LLAM_ASYNC_BANK_FREE 0
#define LLAM_ASYNC_BANK_CAPTURING 1
#define LLAM_ASYNC_BANK_CAPTURED 2
#define LLAM_ASYNC_BANK_READY 3
#define LLAM_ASYNC_BANK_GATE 4
#define LLAM_ASYNC_BANK_RESTORING 5
#define LLAM_ASYNC_BANK_OBSERVED 6

#endif
