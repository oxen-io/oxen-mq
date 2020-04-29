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
namespace lokimq {
using string_view = std::string_view;
using ustring_view = std::basic_string_view<unsigned char>;
}

#else

#include <ostream>
#include <limits>

namespace lokimq {

/// Basic implementation of std::string_view (except for std::hash support).
template <typename CharT>
class simple_string_view {
    const CharT *data_;
    size_t size_;
public:
    using traits_type = std::char_traits<CharT>;
    using value_type = CharT;
    using pointer = CharT*;
    using const_pointer = const CharT*;
    using reference = CharT&;
    using const_reference = const CharT&;
    using const_iterator = const_pointer;
    using iterator = const_iterator;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;
    using reverse_iterator = const_reverse_iterator;
    using size_type = std::size_t;
    using different_type = std::ptrdiff_t;

    static constexpr auto& npos = std::string::npos;

    constexpr simple_string_view() noexcept : data_{nullptr}, size_{0} {}
    constexpr simple_string_view(const simple_string_view&) noexcept = default;
    simple_string_view(const std::basic_string<CharT>& str) : data_{str.data()}, size_{str.size()} {}
    constexpr simple_string_view(const CharT* data, size_t size) noexcept : data_{data}, size_{size} {}
    simple_string_view(const CharT* data) : data_{data}, size_{traits_type::length(data)} {}
    simple_string_view& operator=(const simple_string_view&) = default;
    constexpr const CharT* data() const noexcept { return data_; }
    constexpr size_t size() const noexcept { return size_; }
    constexpr size_t length() const noexcept { return size_; }
    constexpr size_t max_size() const noexcept { return std::numeric_limits<size_t>::max(); }
    constexpr bool empty() const noexcept { return size_ == 0; }
    explicit operator std::basic_string<CharT>() const { return {data_, size_}; }
    constexpr const CharT* begin() const noexcept { return data_; }
    constexpr const CharT* cbegin() const noexcept { return data_; }
    constexpr const CharT* end() const noexcept { return data_ + size_; }
    constexpr const CharT* cend() const noexcept { return data_ + size_; }
    reverse_iterator rbegin() const { return reverse_iterator{end()}; }
    reverse_iterator crbegin() const { return reverse_iterator{end()}; }
    reverse_iterator rend() const { return reverse_iterator{begin()}; }
    reverse_iterator crend() const { return reverse_iterator{begin()}; }
    constexpr const CharT& operator[](size_t pos) const { return data_[pos]; }
    constexpr const CharT& front() const { return *data_; }
    constexpr const CharT& back() const { return data_[size_ - 1]; }
    int compare(simple_string_view s) const;
    constexpr void remove_prefix(size_t n) { data_ += n; size_ -= n; }
    constexpr void remove_suffix(size_t n) { size_ -= n; }
    void swap(simple_string_view &s) noexcept { std::swap(data_, s.data_); std::swap(size_, s.size_); }

#if defined(__clang__) || !defined(__GNUG__) || __GNUC__ >= 6
    constexpr // GCC 5.x is buggy wrt constexpr throwing
#endif
    const CharT& at(size_t pos) const {
        if (pos >= size())
            throw std::out_of_range{"invalid string_view index"};
        return data_[pos];
    }; 

    size_t copy(CharT* dest, size_t count, size_t pos = 0) const {
        if (pos > size()) throw std::out_of_range{"invalid copy pos"};
        size_t rcount = std::min(count, size_ - pos);
        traits_type::copy(dest, data_ + pos, rcount);
        return rcount;
    }

#if defined(__clang__) || !defined(__GNUG__) || __GNUC__ >= 6
    constexpr // GCC 5.x is buggy wrt constexpr throwing
#endif
    simple_string_view substr(size_t pos = 0, size_t count = npos) const {
        if (pos > size()) throw std::out_of_range{"invalid substr range"};
        simple_string_view result = *this;
        if (pos > 0) result.remove_prefix(pos);
        if (count < result.size()) result.remove_suffix(result.size() - count);
        return result;
    }

    size_t find(simple_string_view v, size_t pos = 0) const {
        if (pos > size_ || v.size_ > size_) return npos;
        for (const size_t max_pos = size_ - v.size_; pos <= max_pos; ++pos) {
            if (0 == traits_type::compare(v.data_, data_ + pos, v.size_))
                return pos;
        }
        return npos;
    }
    size_t find(CharT c, size_t pos = 0) const { return find({&c, 1}, pos); }
    size_t find(const CharT* c, size_t pos, size_t count) const { return find({c, count}, pos); }
    size_t find(const CharT* c, size_t pos = 0) const { return find(simple_string_view(c), pos); }

    size_t rfind(simple_string_view v, size_t pos = npos) const {
        if (v.size_ > size_) return npos;
        const size_t max_pos = size_ - v.size_;
        for (pos = std::min(pos, max_pos); pos <= max_pos; --pos) {
            if (0 == traits_type::compare(v.data_, data_ + pos, v.size_))
                return pos;
        }
        return npos;
    }
    size_t rfind(CharT c, size_t pos = npos) const { return rfind({&c, 1}, pos); }
    size_t rfind(const CharT* c, size_t pos, size_t count) const { return rfind({c, count}, pos); }
    size_t rfind(const CharT* c, size_t pos = npos) const { return rfind(simple_string_view(c), pos); }

    constexpr size_t find_first_of(simple_string_view v, size_t pos = 0) const noexcept {
        for (; pos < size_; ++pos)
            for (CharT c : v)
                if (data_[pos] == c)
                    return pos;
        return npos;
    }
    constexpr size_t find_first_of(CharT c, size_t pos = 0) const noexcept { return find_first_of({&c, 1}, pos); }
    constexpr size_t find_first_of(const CharT* c, size_t pos, size_t count) const { return find_first_of({c, count}, pos); }
    size_t find_first_of(const CharT* c, size_t pos = 0) const { return find_first_of(simple_string_view(c), pos); }

    constexpr size_t find_last_of(simple_string_view v, const size_t pos = npos) const noexcept {
        if (size_ == 0) return npos;
        const size_t last_pos = std::min(pos, size_-1);
        for (size_t i = last_pos; i <= last_pos; --i)
            for (CharT c : v)
                if (data_[i] == c)
                    return i;
        return npos;
    }
    constexpr size_t find_last_of(CharT c, size_t pos = npos) const noexcept { return find_last_of({&c, 1}, pos); }
    constexpr size_t find_last_of(const CharT* c, size_t pos, size_t count) const { return find_last_of({c, count}, pos); }
    size_t find_last_of(const CharT* c, size_t pos = npos) const { return find_last_of(simple_string_view(c), pos); }

    constexpr size_t find_first_not_of(simple_string_view v, size_t pos = 0) const noexcept {
        for (; pos < size_; ++pos) {
            bool none = true;
            for (CharT c : v) {
                if (data_[pos] == c) {
                    none = false;
                    break;
                }
            }
            if (none) return pos;
        }
        return npos;
    }
    constexpr size_t find_first_not_of(CharT c, size_t pos = 0) const noexcept { return find_first_not_of({&c, 1}, pos); }
    constexpr size_t find_first_not_of(const CharT* c, size_t pos, size_t count) const { return find_first_not_of({c, count}, pos); }
    size_t find_first_not_of(const CharT* c, size_t pos = 0) const { return find_first_not_of(simple_string_view(c), pos); }

    constexpr size_t find_last_not_of(simple_string_view v, const size_t pos = npos) const noexcept {
        if (size_ == 0) return npos;
        const size_t last_pos = std::min(pos, size_-1);
        for (size_t i = last_pos; i <= last_pos; --i) {
            bool none = true;
            for (CharT c : v) {
                if (data_[i] == c) {
                    none = false;
                    break;
                }
            }
            if (none) return i;
        }
        return npos;
    }
    constexpr size_t find_last_not_of(CharT c, size_t pos = npos) const noexcept { return find_last_not_of({&c, 1}, pos); }
    constexpr size_t find_last_not_of(const CharT* c, size_t pos, size_t count) const { return find_last_not_of({c, count}, pos); }
    size_t find_last_not_of(const CharT* c, size_t pos = npos) const { return find_last_not_of(simple_string_view(c), pos); }
};
/// We have three of each of these: one with two string views, one with RHS argument deduction, and
/// one with LHS argument deduction, so that you can do (sv == sv), (sv == "foo"), and ("foo" == sv)
template <typename CharT>
inline bool operator==(simple_string_view<CharT> lhs, simple_string_view<CharT> rhs) {
    return lhs.size() == rhs.size() && 0 == std::char_traits<CharT>::compare(lhs.data(), rhs.data(), lhs.size());
};
template <typename CharT>
inline bool operator==(simple_string_view<CharT> lhs, std::common_type_t<simple_string_view<CharT>> rhs) {
    return lhs.size() == rhs.size() && 0 == std::char_traits<CharT>::compare(lhs.data(), rhs.data(), lhs.size());
};
template <typename CharT>
inline bool operator==(std::common_type_t<simple_string_view<CharT>> lhs, simple_string_view<CharT> rhs) {
    return lhs.size() == rhs.size() && 0 == std::char_traits<CharT>::compare(lhs.data(), rhs.data(), lhs.size());
};
template <typename CharT>
inline bool operator!=(simple_string_view<CharT> lhs, simple_string_view<CharT> rhs) {
    return !(lhs == rhs);
}
template <typename CharT>
inline bool operator!=(simple_string_view<CharT> lhs, std::common_type_t<simple_string_view<CharT>> rhs) {
    return !(lhs == rhs);
}
template <typename CharT>
inline bool operator!=(std::common_type_t<simple_string_view<CharT>> lhs, simple_string_view<CharT> rhs) {
    return !(lhs == rhs);
}
template <typename CharT>
inline int simple_string_view<CharT>::compare(simple_string_view s) const {
    int cmp = std::char_traits<CharT>::compare(data_, s.data(), std::min(size_, s.size()));
    if (cmp) return cmp;
    if (size_ < s.size()) return -1;
    else if (size_ > s.size()) return 1;
    return 0;
}
template <typename CharT>
inline bool operator<(simple_string_view<CharT> lhs, simple_string_view<CharT> rhs) {
    return lhs.compare(rhs) < 0;
};
template <typename CharT>
inline bool operator<(simple_string_view<CharT> lhs, std::common_type_t<simple_string_view<CharT>> rhs) {
    return lhs.compare(rhs) < 0;
};
template <typename CharT>
inline bool operator<(std::common_type_t<simple_string_view<CharT>> lhs, simple_string_view<CharT> rhs) {
    return lhs.compare(rhs) < 0;
};
template <typename CharT>
inline bool operator<=(simple_string_view<CharT> lhs, simple_string_view<CharT> rhs) {
    return lhs.compare(rhs) <= 0;
};
template <typename CharT>
inline bool operator<=(simple_string_view<CharT> lhs, std::common_type_t<simple_string_view<CharT>> rhs) {
    return lhs.compare(rhs) <= 0;
};
template <typename CharT>
inline bool operator<=(std::common_type_t<simple_string_view<CharT>> lhs, simple_string_view<CharT> rhs) {
    return lhs.compare(rhs) <= 0;
};
template <typename CharT>
inline bool operator>(simple_string_view<CharT> lhs, simple_string_view<CharT> rhs) {
    return lhs.compare(rhs) > 0;
};
template <typename CharT>
inline bool operator>(simple_string_view<CharT> lhs, std::common_type_t<simple_string_view<CharT>> rhs) {
    return lhs.compare(rhs) > 0;
};
template <typename CharT>
inline bool operator>(std::common_type_t<simple_string_view<CharT>> lhs, simple_string_view<CharT> rhs) {
    return lhs.compare(rhs) > 0;
};
template <typename CharT>
inline bool operator>=(simple_string_view<CharT> lhs, simple_string_view<CharT> rhs) {
    return lhs.compare(rhs) >= 0;
};
template <typename CharT>
inline bool operator>=(simple_string_view<CharT> lhs, std::common_type_t<simple_string_view<CharT>> rhs) {
    return lhs.compare(rhs) >= 0;
};
template <typename CharT>
inline bool operator>=(std::common_type_t<simple_string_view<CharT>> lhs, simple_string_view<CharT> rhs) {
    return lhs.compare(rhs) >= 0;
};
template <typename CharT>
inline std::basic_ostream<CharT>& operator<<(std::basic_ostream<CharT>& os, const simple_string_view<CharT>& s) {
    os.write(s.data(), s.size());
    return os;
}

using string_view = simple_string_view<char>;
using ustring_view = simple_string_view<unsigned char>;

}

#endif

// Add a "foo"_sv literal that works exactly like the C++17 "foo"sv literal, but works with our
// implementation in pre-C++17.
namespace lokimq {
inline namespace literals {
    inline constexpr string_view operator""_sv(const char* str, size_t len) { return {str, len}; }
}
}
