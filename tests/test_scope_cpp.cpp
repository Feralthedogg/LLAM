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
#include <atomic>
#include <cstdio>
#include <cstdlib>

static std::atomic<unsigned> drops{0U};
struct tracked {
    std::uint64_t value;
    ~tracked() noexcept { ++drops; }
};
namespace llam {
template <> struct type_id<tracked> {
    static constexpr std::uint64_t high = 0x8145a8b46dc23f12ULL, low = 1U;
};
}
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s errno=%d\n", __FILE__, __LINE__, #expr, errno); std::abort(); } } while (0)
static_assert(!std::is_copy_constructible_v<llam::future<tracked>>);
static_assert(!std::is_copy_constructible_v<llam::outcome<tracked>>);
static_assert(std::is_nothrow_move_constructible_v<llam::owned_value<tracked>>);

static void producer(void *data) {
    auto input = static_cast<tracked *>(data);
    std::unique_ptr<tracked> result(new tracked{input->value * 2U});
    llam::owned_value<tracked> value;
    CHECK(llam::owned_value<tracked>::adopt(result, value) == 0 && !result);
    CHECK(value.complete() == 0 && !value);
}
static void app_error(void *) {
    auto error = std::make_unique<std::int32_t>(17);
    llam::owned_value<std::int32_t> value;
    CHECK(llam::owned_value<std::int32_t>::adopt(error, value) == 0);
    CHECK(value.complete(LLAM_OUTCOME_APP_ERROR) == 0);
}
static llam_runtime_t *runtime() {
    llam_runtime_opts_t options;
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0);
    options.deterministic = 1U;
    llam_runtime_t *result = nullptr;
    CHECK(llam_runtime_create_compat(&options, sizeof(options), &result) == 0);
    return result;
}
static llam_scope_t *scope(llam_runtime_t *rt) {
    llam_scope_t *result = nullptr;
    CHECK(llam_runtime_scope_create_ex(rt, nullptr, 0U, &result) == 0);
    return result;
}
static void result_lifetime() {
    auto rt = runtime();
    auto owner = scope(rt);
    std::unique_ptr<tracked> input(new tracked{21U});
    llam::owned_value<tracked> argument;
    CHECK(llam::owned_value<tracked>::adopt(input, argument) == 0 && !input);
    llam::future<tracked> first;
    CHECK(llam::future<tracked>::spawn(owner, producer, argument, first) == 0 && !argument);
    llam::outcome<tracked> result;
    CHECK(first.take(result) == -1 && errno == EAGAIN && first && !result);
    auto future = std::move(first);
    CHECK(!first && future);
    CHECK(llam_runtime_run_handle(rt) == 0);
    CHECK(llam_scope_drain_until(owner, 0U, nullptr, 0U) == 0);
    CHECK(drops == 1U && llam_scope_destroy(owner) == 0);
    llam_runtime_destroy(rt);
    CHECK(future.take(result) == 0 && !future);
    tracked *payload = nullptr;
    CHECK(result.success(payload) == 0 && payload->value == 42U);
    auto moved = std::move(result);
    CHECK(!result && moved.kind() == LLAM_OUTCOME_OK && moved.task_id() != 0U);
    CHECK(moved.reset() == 0 && drops == 2U);
}
static void failure_and_errors() {
    auto rt = runtime();
    auto owner = scope(rt);
    llam::future<llam::unit, std::int32_t> future;
    CHECK((decltype(future)::spawn(owner, app_error, future) == 0));
    CHECK(llam_runtime_run_handle(rt) == 0);
    CHECK(llam_scope_drain_until(owner, 0U, nullptr, 0U) == 0);
    llam::outcome<llam::unit, std::int32_t> result;
    CHECK(future.take(result) == 0 && result.kind() == LLAM_OUTCOME_APP_ERROR);
    std::int32_t *error = nullptr;
    CHECK(result.error(error) == 0 && *error == 17);
    std::unique_ptr<tracked> input(new tracked{1U});
    llam::owned_value<tracked> argument;
    CHECK(llam::owned_value<tracked>::adopt(input, argument) == 0);
    llam::future<tracked> rejected;
    CHECK(llam::future<tracked>::spawn(owner, producer, argument, rejected) == -1);
    CHECK(errno == EPIPE && argument && !rejected);
    CHECK(argument.reset() == 0 && drops == 3U);
    CHECK(llam_scope_destroy(owner) == 0);
    llam_runtime_destroy(rt);
}
int main() {
    result_lifetime();
    failure_and_errors();
    std::puts("typed C++ host: move-only results, application errors, rollback and independent lifetime passed");
}
