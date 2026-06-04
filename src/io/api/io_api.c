/**
 * @file src/io/api/io_api.c
 * @brief Runtime I/O API coordination and common request lifecycle handling.
 *
 * @details
 * The concrete public I/O entry points are split across focused files:
 *  - @c public.c for read/write/accept/poll wrappers,
 *  - @c issue.c for request parking and backend submission,
 *  - @c direct.c for immediate non-blocking fast paths,
 *  - @c blocking_ops.c for blocking-worker fallbacks.
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

#include "io/runtime_io_api_internal.h"
