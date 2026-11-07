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

#ifndef LLAM_FUTURE_HPP
#define LLAM_FUTURE_HPP

#include "llam/scope.h"
#include <cerrno>
#include <cstdint>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

// C++17 host bindings. This header does not qualify C++ task code for managed
// execution. Destruction must occur in an allowed owner/host context without
// concurrent operations. Release failure in a destructor terminates, never leaks
// silently; explicit reset() reports errno and preserves ownership on failure.
namespace llam {
struct unit {};

// Specialize with stable uint64_t high/low members for application types. The
// same semantic ID must have the same representation in every participating ABI.
template <class T> struct type_id;
template <> struct type_id<std::uint64_t> {
    static constexpr std::uint64_t high = LLAM_TYPE_SCALAR_ID_HIGH, low = 1U;
};
template <> struct type_id<std::int64_t> {
    static constexpr std::uint64_t high = LLAM_TYPE_SCALAR_ID_HIGH, low = 2U;
};
template <> struct type_id<std::uint32_t> {
    static constexpr std::uint64_t high = LLAM_TYPE_SCALAR_ID_HIGH, low = 3U;
};
template <> struct type_id<std::int32_t> {
    static constexpr std::uint64_t high = LLAM_TYPE_SCALAR_ID_HIGH, low = 4U;
};

template <class T> constexpr llam_type_descriptor_t type_descriptor() noexcept {
    static_assert(std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>);
    if constexpr (std::is_same_v<T, unit>) {
        return LLAM_TYPE_UNIT_INIT;
    } else {
        static_assert(type_id<T>::high != 0U || type_id<T>::low != 0U);
        return {type_id<T>::high, type_id<T>::low, sizeof(T), alignof(T), 0U};
    }
}

template <class T, class E> constexpr llam_result_contract_t result_contract() noexcept {
    return {type_descriptor<T>(), type_descriptor<E>()};
}

namespace detail {
inline int fail(int error) noexcept { errno = error; return -1; }
template <class H, int (*Release)(H *)> struct release_handle {
    void operator()(H *handle) const noexcept {
        const int saved = errno;
        if (Release(handle) != 0) std::terminate();
        errno = saved;
    }
};
template <class H, int (*Release)(H *)>
using owner = std::unique_ptr<H, release_handle<H, Release>>;

template <class H, int (*Release)(H *)>
int reset(owner<H, Release> &handle) noexcept {
    if (!handle) return 0;
    if (Release(handle.get()) != 0) return -1;
    (void)handle.release();
    return 0;
}
} // namespace detail

template <class T, class E> class future;

template <class T> class owned_value {
    detail::owner<llam_value_t, llam_value_release> handle_;
    static void drop(void *data, void *) noexcept { delete static_cast<T *>(data); }
    template <class, class> friend class future;
public:
    owned_value() noexcept = default;
    owned_value(owned_value &&) noexcept = default;
    owned_value &operator=(owned_value &&) noexcept = default;
    owned_value(const owned_value &) = delete;
    owned_value &operator=(const owned_value &) = delete;
    explicit operator bool() const noexcept { return bool(handle_); }
    int reset() noexcept { return detail::reset(handle_); }

    // Destruction of T must satisfy scope.h's callback contract. Its defining
    // module and any referenced context must outlive this value, even after take.
    static int adopt(std::unique_ptr<T> &input, owned_value &out) noexcept {
        static_assert(std::is_nothrow_destructible_v<T>);
        static_assert(!std::is_same_v<T, unit>, "unit uses no value handle");
        if (!input || out) return detail::fail(EINVAL);
        llam_value_spec_t spec{type_descriptor<T>(), input.get(), drop, nullptr};
        llam_value_t *raw = nullptr;
        if (llam_value_create_ex(&spec, sizeof(spec), &raw) != 0) return -1;
        (void)input.release();
        out.handle_.reset(raw);
        return 0;
    }
    int data(T *&out) const noexcept {
        void *raw = nullptr;
        const auto type = type_descriptor<T>();
        if (llam_value_data_ex(handle_.get(), &type, sizeof(type), &raw) != 0) return -1;
        out = static_cast<T *>(raw);
        return 0;
    }
    int complete(std::uint32_t kind = LLAM_OUTCOME_OK) noexcept {
        llam_outcome_t value{kind, 0, handle_.get(), 0U};
        if (llam_task_set_outcome_ex(&value, sizeof(value)) != 0) return -1;
        (void)handle_.release();
        return 0;
    }
};

template <class T, class E = unit> class outcome {
    llam_outcome_t value_{};
    detail::owner<llam_value_t, llam_value_release> payload_;
    friend class future<T, E>;
    template <class V> int data(std::uint32_t kind, V *&out) const noexcept {
        if (value_.kind != kind) return detail::fail(EINVAL);
        if constexpr (std::is_same_v<V, unit>) {
            out = nullptr;
            return 0;
        } else {
            void *raw = nullptr;
            const auto type = type_descriptor<V>();
            if (llam_value_data_ex(payload_.get(), &type, sizeof(type), &raw) != 0) return -1;
            out = static_cast<V *>(raw);
            return 0;
        }
    }
public:
    outcome() noexcept = default;
    outcome(outcome &&other) noexcept
        : value_(std::exchange(other.value_, {})), payload_(std::move(other.payload_)) {}
    outcome &operator=(outcome &&other) noexcept {
        if (this != &other) {
            payload_ = std::move(other.payload_);
            value_ = std::exchange(other.value_, {});
        }
        return *this;
    }
    outcome(const outcome &) = delete;
    outcome &operator=(const outcome &) = delete;
    explicit operator bool() const noexcept { return value_.kind != 0U; }
    std::uint32_t kind() const noexcept { return value_.kind; }
    int runtime_error() const noexcept { return value_.runtime_error; }
    std::uint64_t task_id() const noexcept { return value_.task_id; }
    int success(T *&out) const noexcept { return data(LLAM_OUTCOME_OK, out); }
    int error(E *&out) const noexcept { return data(LLAM_OUTCOME_APP_ERROR, out); }
    int reset() noexcept {
        if (detail::reset(payload_) != 0) return -1;
        value_ = {};
        return 0;
    }
};

template <class T, class E = unit> class future {
    detail::owner<llam_future_t, llam_future_release> handle_;
    static int spawn_raw(llam_scope_t *scope, llam_task_fn entry, llam_value_t *argument,
                         future &out, const llam_spawn_opts_t *options) noexcept {
        if (out) return detail::fail(EBUSY);
        llam_typed_task_spec_t spec{entry, argument, options, options ? sizeof(*options) : 0U,
                                    result_contract<T, E>()};
        llam_future_t *raw = nullptr;
        if (llam_scope_spawn_ex(scope, &spec, sizeof(spec), &raw) != 0) return -1;
        out.handle_.reset(raw);
        return 0;
    }
public:
    future() noexcept = default;
    future(future &&) noexcept = default;
    future &operator=(future &&) noexcept = default;
    future(const future &) = delete;
    future &operator=(const future &) = delete;
    explicit operator bool() const noexcept { return bool(handle_); }
    int reset() noexcept { return detail::reset(handle_); }
    int wait_until(std::uint64_t deadline) const noexcept {
        return llam_future_wait_until(handle_.get(), deadline);
    }
    int take(outcome<T, E> &out) noexcept {
        if (out) return detail::fail(EBUSY);
        const auto contract = result_contract<T, E>();
        llam_outcome_t value{};
        if (llam_future_take_ex(handle_.get(), &contract, sizeof(contract), &value, sizeof(value)) != 0)
            return -1;
        (void)handle_.release();
        out.payload_.reset(value.value);
        out.value_ = value;
        return 0;
    }
    static int spawn(llam_scope_t *scope, llam_task_fn entry, future &out,
                     const llam_spawn_opts_t *options = nullptr) noexcept {
        return spawn_raw(scope, entry, nullptr, out, options);
    }
    template <class A> static int spawn(llam_scope_t *scope, llam_task_fn entry,
                                      owned_value<A> &argument, future &out,
                                      const llam_spawn_opts_t *options = nullptr) noexcept {
        if (spawn_raw(scope, entry, argument.handle_.get(), out, options) != 0) return -1;
        (void)argument.handle_.release();
        return 0;
    }
};
} // namespace llam
#endif
