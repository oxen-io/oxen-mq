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
#include <string_view>
#include <array>
#include <iterator>
#include <cassert>

namespace lokimq {

namespace detail {

/// Compile-time generated lookup tables hex conversion
struct hex_table {
    char from_hex_lut[256];
    char to_hex_lut[16];
    constexpr hex_table() noexcept : from_hex_lut{}, to_hex_lut{} {
        for (unsigned char c = 0; c < 10; c++) {
            from_hex_lut[(unsigned char)('0' + c)] =  0  + c;
            to_hex_lut[  (unsigned char)( 0  + c)] = '0' + c;
        }
        for (unsigned char c = 0; c < 6; c++) {
            from_hex_lut[(unsigned char)('a' + c)] = 10  + c;
            from_hex_lut[(unsigned char)('A' + c)] = 10  + c;
            to_hex_lut[  (unsigned char)(10  + c)] = 'a' + c;
        }
    }
    constexpr char from_hex(unsigned char c) const noexcept { return from_hex_lut[c]; }
    constexpr char to_hex(unsigned char b) const noexcept { return to_hex_lut[b]; }
} constexpr hex_lut;

// This main point of this static assert is to force the compiler to compile-time build the constexpr tables.
static_assert(hex_lut.from_hex('a') == 10 && hex_lut.from_hex('F') == 15 && hex_lut.to_hex(13) == 'd', "");

} // namespace detail

/// Creates hex digits from a character sequence.
template <typename InputIt, typename OutputIt>
void to_hex(InputIt begin, InputIt end, OutputIt out) {
    static_assert(sizeof(decltype(*begin)) == 1, "to_hex requires chars/bytes");
    for (; begin != end; ++begin) {
        uint8_t c = static_cast<uint8_t>(*begin);
        *out++ = detail::hex_lut.to_hex(c >> 4);
        *out++ = detail::hex_lut.to_hex(c & 0x0f);
    }
}

/// Creates a string of hex digits from a character sequence iterator pair
template <typename It>
std::string to_hex(It begin, It end) {
    std::string hex;
    if constexpr (std::is_base_of_v<std::random_access_iterator_tag, typename std::iterator_traits<It>::iterator_category>)
        hex.reserve(2 * std::distance(begin, end));
    to_hex(begin, end, std::back_inserter(hex));
    return hex;
}

/// Creates a hex string from an iterable, std::string-like object
template <typename CharT>
std::string to_hex(std::basic_string_view<CharT> s) { return to_hex(s.begin(), s.end()); }
inline std::string to_hex(std::string_view s) { return to_hex<>(s); }

/// Returns true if all elements in the range are hex characters
template <typename It>
constexpr bool is_hex(It begin, It end) {
    static_assert(sizeof(decltype(*begin)) == 1, "is_hex requires chars/bytes");
    for (; begin != end; ++begin) {
        if (detail::hex_lut.from_hex(static_cast<unsigned char>(*begin)) == 0 && static_cast<unsigned char>(*begin) != '0')
            return false;
    }
    return true;
}

/// Returns true if all elements in the string-like value are hex characters
template <typename CharT>
constexpr bool is_hex(std::basic_string_view<CharT> s) { return is_hex(s.begin(), s.end()); }
constexpr bool is_hex(std::string_view s) { return is_hex(s.begin(), s.end()); }

/// Convert a hex digit into its numeric (0-15) value
constexpr char from_hex_digit(unsigned char x) noexcept {
    return detail::hex_lut.from_hex(x);
}

/// Constructs a byte value from a pair of hex digits
constexpr char from_hex_pair(unsigned char a, unsigned char b) noexcept { return (from_hex_digit(a) << 4) | from_hex_digit(b); }

/// Converts a sequence of hex digits to bytes.  Undefined behaviour if any characters are not in
/// [0-9a-fA-F] or if the input sequence length is not even.  It is permitted for the input and
/// output ranges to overlap as long as out is no earlier than begin.
template <typename InputIt, typename OutputIt>
void from_hex(InputIt begin, InputIt end, OutputIt out) {
    using std::distance;
    assert(distance(begin, end) % 2 == 0);
    while (begin != end) {
        auto a = *begin++;
        auto b = *begin++;
        *out++ = from_hex_pair(static_cast<unsigned char>(a), static_cast<unsigned char>(b));
    }
}

/// Converts a sequence of hex digits to a string of bytes and returns it.  Undefined behaviour if
/// the input sequence is not an even-length sequence of [0-9a-fA-F] characters.
template <typename It>
std::string from_hex(It begin, It end) {
    std::string bytes;
    if constexpr (std::is_base_of_v<std::random_access_iterator_tag, typename std::iterator_traits<It>::iterator_category>)
        bytes.reserve(std::distance(begin, end) / 2);
    from_hex(begin, end, std::back_inserter(bytes));
    return bytes;
}

/// Converts hex digits from a std::string-like object into a std::string of bytes.  Undefined
/// behaviour if any characters are not in [0-9a-fA-F] or if the input sequence length is not even.
template <typename CharT>
std::string from_hex(std::basic_string_view<CharT> s) { return from_hex(s.begin(), s.end()); }
inline std::string from_hex(std::string_view s) { return from_hex<>(s); }

}
