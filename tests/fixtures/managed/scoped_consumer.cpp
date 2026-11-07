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
 * See LICENSE.
 */

#include "llam/future.hpp"
#include "llam/managed.h"
#include <cstdio>
#include <cstdlib>

extern "C" void sdk_scoped_value(void *);
extern "C" void sdk_scoped_peer(void *);
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s errno=%d\n", __FILE__, __LINE__, #expr, errno); std::abort(); } } while (0)

int main() {
    llam_runtime_opts_t options;
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0);
    options.deterministic = 1U;
    options.worker_count = 1U;
    llam_runtime_t *runtime = nullptr;
    CHECK(llam_runtime_create(&options, sizeof(options), &runtime) == 0);
    llam_preempt_info_t info;
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0);
    CHECK(info.requested_policy == LLAM_PREEMPT_POLICY_ASYNC_REQUIRED && !info.compatibility_processors);
    llam_scope_t *scope = nullptr;
    CHECK(llam_runtime_scope_create_ex(runtime, nullptr, 0U, &scope) == 0);
    llam::future<std::uint64_t> value;
    llam::future<llam::unit> peer;
    CHECK(decltype(value)::spawn(scope, sdk_scoped_value, value) == 0);
    CHECK(decltype(peer)::spawn(scope, sdk_scoped_peer, peer) == 0);
    CHECK(llam_runtime_run_handle(runtime) == 0);
    llam_scope_report_t report;
    CHECK(llam_scope_drain_until(scope, 0U, &report, sizeof(report)) == 0);
    CHECK(report.succeeded == 2U && report.pending_tasks == 0U);
    llam_runtime_stats_t stats;
    CHECK(llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) == 0 && stats.preempt_yields > 0U);
    CHECK(llam_scope_destroy(scope) == 0);
    llam_runtime_destroy(runtime);
    llam::outcome<std::uint64_t> result;
    CHECK(value.take(result) == 0);
    std::uint64_t *number = nullptr;
    CHECK(result.success(number) == 0 && *number == 42U);
    CHECK(peer.reset() == 0);
    std::puts("installed SDK: managed C scope, native preemption, C++ typed take after runtime destruction passed");
}
