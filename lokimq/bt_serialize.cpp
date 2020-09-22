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

#include "bt_serialize.h"
#include <iterator>

namespace lokimq {
namespace detail {

/// Reads digits into an unsigned 64-bit int.  
uint64_t extract_unsigned(std::string_view& s) {
    if (s.empty())
        throw bt_deserialize_invalid{"Expected 0-9 but found end of string"};
    if (s[0] < '0' || s[0] > '9')
        throw bt_deserialize_invalid("Expected 0-9 but found '"s + s[0]);
    uint64_t uval = 0;
    while (!s.empty() && (s[0] >= '0' && s[0] <= '9')) {
        uint64_t bigger = uval * 10 + (s[0] - '0');
        s.remove_prefix(1);
        if (bigger < uval) // overflow
            throw bt_deserialize_invalid("Integer deserialization failed: value is too large for a 64-bit int");
        uval = bigger;
    }
    return uval;
}

void bt_deserialize<std::string_view>::operator()(std::string_view& s, std::string_view& val) {
    if (s.size() < 2) throw bt_deserialize_invalid{"Deserialize failed: given data is not an bt-encoded string"};
    if (s[0] < '0' || s[0] > '9')
        throw bt_deserialize_invalid_type{"Expected 0-9 but found '"s + s[0] + "'"};
    auto len = static_cast<size_t>(extract_unsigned(s));
    if (s.empty() || s[0] != ':')
        throw bt_deserialize_invalid{"Did not find expected ':' during string deserialization"};
    s.remove_prefix(1);

    if (len > s.size())
        throw bt_deserialize_invalid{"String deserialization failed: encoded string length is longer than the serialized data"};

    val = {s.data(), len};
    s.remove_prefix(len);
}

// Check that we are on a 2's complement architecture.  It's highly unlikely that this code ever
// runs on a non-2s-complement architecture (especially since C++20 requires a two's complement
// signed value behaviour), but check at compile time anyway because we rely on these relations
// below.
static_assert(std::numeric_limits<int64_t>::min() + std::numeric_limits<int64_t>::max() == -1 &&
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + uint64_t{1} == (uint64_t{1} << 63),
        "Non 2s-complement architecture not supported!");

std::pair<uint64_t, bool> bt_deserialize_integer(std::string_view& s) {
    // Smallest possible encoded integer is 3 chars: "i0e"
    if (s.size() < 3) throw bt_deserialize_invalid("Deserialization failed: end of string found where integer expected");
    if (s[0] != 'i') throw bt_deserialize_invalid_type("Deserialization failed: expected 'i', found '"s + s[0] + '\'');
    s.remove_prefix(1);
    std::pair<uint64_t, bool> result;
    if (s[0] == '-') {
        result.second = true;
        s.remove_prefix(1);
    }

    result.first = extract_unsigned(s);
    if (s.empty())
        throw bt_deserialize_invalid("Integer deserialization failed: encountered end of string before integer was finished");
    if (s[0] != 'e')
        throw bt_deserialize_invalid("Integer deserialization failed: expected digit or 'e', found '"s + s[0] + '\'');
    s.remove_prefix(1);
    if (result.second /*negative*/ && result.first > (uint64_t{1} << 63))
        throw bt_deserialize_invalid("Deserialization of integer failed: negative integer value is too large for a 64-bit signed int");

    return result;
}

template struct bt_deserialize<int64_t>;
template struct bt_deserialize<uint64_t>;

void bt_deserialize<bt_value, void>::operator()(std::string_view& s, bt_value& val) {
    if (s.size() < 2) throw bt_deserialize_invalid("Deserialization failed: end of string found where bt-encoded value expected");

    switch (s[0]) {
        case 'd': {
            bt_dict dict;
            bt_deserialize<bt_dict>{}(s, dict);
            val = std::move(dict);
            break;
        }
        case 'l': {
            bt_list list;
            bt_deserialize<bt_list>{}(s, list);
            val = std::move(list);
            break;
        }
        case 'i': {
            auto [magnitude, negative] = bt_deserialize_integer(s);
            if (negative) val = -static_cast<int64_t>(magnitude);
            else val = magnitude;
            break;
        }
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': {
            std::string str;
            bt_deserialize<std::string>{}(s, str);
            val = std::move(str);
            break;
        }
        default:
            throw bt_deserialize_invalid("Deserialize failed: encountered invalid value '"s + s[0] + "'; expected one of [0-9idl]");
    }
}

} // namespace detail


bt_list_consumer::bt_list_consumer(std::string_view data_) : data{std::move(data_)} {
    if (data.empty()) throw std::runtime_error{"Cannot create a bt_list_consumer with an empty string_view"};
    if (data[0] != 'l') throw std::runtime_error{"Cannot create a bt_list_consumer with non-list data"};
    data.remove_prefix(1);
}

/// Attempt to parse the next value as a string (and advance just past it).  Throws if the next
/// value is not a string.
std::string_view bt_list_consumer::consume_string_view() {
    if (data.empty())
        throw bt_deserialize_invalid{"expected a string, but reached end of data"};
    else if (!is_string())
        throw bt_deserialize_invalid_type{"expected a string, but found "s + data.front()};
    std::string_view next{data}, result;
    detail::bt_deserialize<std::string_view>{}(next, result);
    data = next;
    return result;
}

std::string bt_list_consumer::consume_string() {
    return std::string{consume_string_view()};
}

/// Consumes a value without returning it.
void bt_list_consumer::skip_value() {
    if (is_string())
        consume_string_view();
    else if (is_integer())
        detail::bt_deserialize_integer(data);
    else if (is_list())
        consume_list_data();
    else if (is_dict())
        consume_dict_data();
    else
        throw bt_deserialize_invalid_type{"next bt value has unknown type"};
}

std::string_view bt_list_consumer::consume_list_data() {
    auto start = data.begin();
    if (data.size() < 2 || !is_list()) throw bt_deserialize_invalid_type{"next bt value is not a list"};
    data.remove_prefix(1); // Descend into the sublist, consume the "l"
    while (!is_finished()) {
        skip_value();
        if (data.empty())
            throw bt_deserialize_invalid{"bt list consumption failed: hit the end of string before the list was done"};
    }
    data.remove_prefix(1); // Back out from the sublist, consume the "e"
    return {start, static_cast<size_t>(std::distance(start, data.begin()))};
}

std::string_view bt_list_consumer::consume_dict_data() {
    auto start = data.begin();
    if (data.size() < 2 || !is_dict()) throw bt_deserialize_invalid_type{"next bt value is not a dict"};
    data.remove_prefix(1); // Descent into the dict, consumer the "d"
    while (!is_finished()) {
        consume_string_view(); // Key is always a string
        if (!data.empty())
            skip_value();
        if (data.empty())
            throw bt_deserialize_invalid{"bt dict consumption failed: hit the end of string before the dict was done"};
    }
    data.remove_prefix(1); // Back out of the dict, consume the "e"
    return {start, static_cast<size_t>(std::distance(start, data.begin()))};
}

bt_dict_consumer::bt_dict_consumer(std::string_view data_) {
    data = std::move(data_);
    if (data.empty()) throw std::runtime_error{"Cannot create a bt_dict_consumer with an empty string_view"};
    if (data.size() < 2 || data[0] != 'd') throw std::runtime_error{"Cannot create a bt_dict_consumer with non-dict data"};
    data.remove_prefix(1);
}

bool bt_dict_consumer::consume_key() {
    if (key_.data())
        return true;
    if (data.empty()) throw bt_deserialize_invalid_type{"expected a key or dict end, found end of string"};
    if (data[0] == 'e') return false;
    key_ = bt_list_consumer::consume_string_view();
    if (data.empty() || data[0] == 'e')
        throw bt_deserialize_invalid{"dict key isn't followed by a value"};
    return true;
}

std::pair<std::string_view, std::string_view> bt_dict_consumer::next_string() {
    if (!is_string())
        throw bt_deserialize_invalid_type{"expected a string, but found "s + data.front()};
    std::pair<std::string_view, std::string_view> ret;
    ret.second = bt_list_consumer::consume_string_view();
    ret.first = flush_key();
    return ret;
}


} // namespace lokimq
