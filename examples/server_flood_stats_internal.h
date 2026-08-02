/**
 * @file examples/server_flood_stats_internal.h
 * @brief Private file-open boundary for server_flood stats parsing.
 *
 * @copyright Copyright 2026 Feralthedogg
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
 * See LICENSES/OLD-LICENSE/Apache-2.0.txt.
 */

#ifndef LLAM_EXAMPLES_SERVER_FLOOD_STATS_INTERNAL_H
#define LLAM_EXAMPLES_SERVER_FLOOD_STATS_INTERNAL_H

#include <stdio.h>

FILE *flood_open_stats_file(const char *stats_path);

#endif
