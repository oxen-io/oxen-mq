// Copyright (c) 2019-2020, The Loki Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once

// This header is here to provide just the basic bt_value/bt_dict/bt_list definitions without
// needing to include the full bt_serialize.h header.

#include <map>
#include <list>
#include <cstdint>
#include <variant>
#include <string>
#include <string_view>

namespace lokimq {

struct bt_value;

/// The type used to store dictionaries inside bt_value.
using bt_dict = std::map<std::string, bt_value>; // NB: unordered_map doesn't work because it can't be used with a predeclared type
/// The type used to store list items inside bt_value.
using bt_list = std::list<bt_value>;

/// The basic variant that can hold anything (recursively).
using bt_variant = std::variant<
    std::string,
    std::string_view,
    int64_t,
    uint64_t,
    bt_list,
    bt_dict
>;

#ifdef __cpp_lib_remove_cvref // C++20
using std::remove_cvref_t;
#else
template <typename T>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;
#endif

template <typename T, typename Variant>
struct has_alternative;
template <typename T, typename... V>
struct has_alternative<T, std::variant<V...>> : std::bool_constant<(std::is_same_v<T, V> || ...)> {};
template <typename T, typename Variant>
constexpr bool has_alternative_v = has_alternative<T, Variant>::value;

/// Recursive generic type that can fully represent everything valid for a BT serialization.
/// This is basically just an empty wrapper around the std::variant, except we add some integer
/// constructors so that any unsigned value goes to the uint64_t and any signed value goes to the
/// int64_t.
struct bt_value : bt_variant {
    using bt_variant::bt_variant;
    using bt_variant::operator=;

    template <typename T, typename U = std::remove_reference_t<T>, std::enable_if_t<std::is_integral_v<U> && std::is_unsigned_v<U>, int> = 0>
    bt_value(T&& uint) : bt_variant{static_cast<uint64_t>(uint)} {}

    template <typename T, typename U = std::remove_reference_t<T>, std::enable_if_t<std::is_integral_v<U> && std::is_signed_v<U>, int> = 0>
    bt_value(T&& sint) : bt_variant{static_cast<int64_t>(sint)} {}

    // This forward ctor shouldn't be necessary, but clang fails to find the base variant ctors without it.
    template <typename T, typename U = std::remove_reference_t<T>, std::enable_if_t<!std::is_integral_v<U>, int> = 0>
    bt_value(T&& v) : bt_variant{std::forward<T>(v)} {}
};

}
