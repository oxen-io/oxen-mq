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

#include <string>

#ifdef __cpp_lib_string_view

#include <string_view>
namespace lokimq { using string_view = std::string_view; }

#else

#include <ostream>

namespace lokimq {

/// Basic implementation of a subset of std::string_view (enough for what we need in lokimq)
class simple_string_view {
    const char *data_;
    size_t size_;
public:
    constexpr simple_string_view() noexcept : data_{nullptr}, size_{0} {}
    constexpr simple_string_view(const simple_string_view&) noexcept = default;
    simple_string_view(const std::string& str) : data_{str.data()}, size_{str.size()} {}
    constexpr simple_string_view(const char* data, size_t size) noexcept : data_{data}, size_{size} {}
    simple_string_view(const char* data) : data_{data}, size_{std::char_traits<char>::length(data)} {}
    simple_string_view& operator=(const simple_string_view&) = default;
    constexpr const char* data() const noexcept { return data_; }
    constexpr size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    operator std::string() const { return {data_, size_}; }
    const char* begin() const noexcept { return data_; }
    const char* end() const noexcept { return data_ + size_; }
    constexpr const char& operator[](size_t pos) const { return data_[pos]; }
    constexpr const char& front() const { return *data_; }
    constexpr const char& back() const { return data_[size_ - 1]; }
    int compare(simple_string_view s) const;
    constexpr void remove_prefix(size_t n) { data_ += n; size_ -= n; }
    constexpr void remove_suffix(size_t n) { size_ -= n; }
    void swap(simple_string_view &s) noexcept { std::swap(data_, s.data_); std::swap(size_, s.size_); }
};
inline bool operator==(simple_string_view lhs, simple_string_view rhs) {
    return lhs.size() == rhs.size() && 0 == std::char_traits<char>::compare(lhs.data(), rhs.data(), lhs.size());
};
inline bool operator!=(simple_string_view lhs, simple_string_view rhs) {
    return !(lhs == rhs);
}
inline int simple_string_view::compare(simple_string_view s) const {
    int cmp = std::char_traits<char>::compare(data_, s.data(), std::min(size_, s.size()));
    if (cmp) return cmp;
    if (size_ < s.size()) return -1;
    else if (size_ > s.size()) return 1;
    return 0;
}
inline bool operator<(simple_string_view lhs, simple_string_view rhs) {
    return lhs.compare(rhs) < 0;
};
inline bool operator<=(simple_string_view lhs, simple_string_view rhs) {
    return lhs.compare(rhs) <= 0;
};
inline bool operator>(simple_string_view lhs, simple_string_view rhs) {
    return lhs.compare(rhs) > 0;
};
inline bool operator>=(simple_string_view lhs, simple_string_view rhs) {
    return lhs.compare(rhs) >= 0;
};
inline std::ostream& operator<<(std::ostream& os, const simple_string_view& s) {
    os.write(s.data(), s.size());
    return os;
}

using string_view = simple_string_view;

}

#endif

