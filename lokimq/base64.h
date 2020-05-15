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

/// Compile-time generated lookup tables for base64 conversion.
struct b64_table {
    // Store the 0-63 decoded value of every possible char; all the chars that aren't valid are set
    // to 0.  (If you don't trust your data, check it with is_base64 first, which uses these 0's
    // to detect invalid characters -- which is why we want a full 256 element array).
    char from_b64_lut[256];
    // Store the encoded character of every 0-63 (6 bit) value.
    char to_b64_lut[64];

    // constexpr constructor that fills out the above (and should do it at compile time for any half
    // decent compiler).
    constexpr b64_table() noexcept : from_b64_lut{}, to_b64_lut{} {
        for (unsigned char c = 0; c < 26; c++) {
            from_b64_lut[(unsigned char)('A' + c)] =  0  + c;
            to_b64_lut[  (unsigned char)( 0  + c)] = 'A' + c;
        }
        for (unsigned char c = 0; c < 26; c++) {
            from_b64_lut[(unsigned char)('a' + c)] = 26  + c;
            to_b64_lut[  (unsigned char)(26  + c)] = 'a' + c;
        }
        for (unsigned char c = 0; c < 10; c++) {
            from_b64_lut[(unsigned char)('0' + c)] = 52  + c;
            to_b64_lut[  (unsigned char)(52  + c)] = '0' + c;
        }
        to_b64_lut[62] = '+'; from_b64_lut[(unsigned char) '+'] = 62;
        to_b64_lut[63] = '/'; from_b64_lut[(unsigned char) '/'] = 63;
    }
    // Convert a b64 encoded character into a 0-63 value
    constexpr char from_b64(unsigned char c) const noexcept { return from_b64_lut[c]; }
    // Convert a 0-31 value into a b64 encoded character
    constexpr char to_b64(unsigned char b) const noexcept { return to_b64_lut[b]; }
} constexpr b64_lut;

// This main point of this static assert is to force the compiler to compile-time build the constexpr tables.
static_assert(b64_lut.from_b64('/') == 63 && b64_lut.from_b64('7') == 59 && b64_lut.to_b64(38) == 'm', "");

} // namespace detail

/// Converts bytes into a base64 encoded character sequence.
template <typename InputIt, typename OutputIt>
void to_base64(InputIt begin, InputIt end, OutputIt out) {
    static_assert(sizeof(decltype(*begin)) == 1, "to_base64 requires chars/bytes");
    int bits = 0; // Tracks the number of unconsumed bits held in r, will always be in {0, 2, 4}
    std::uint_fast16_t r = 0;
    while (begin != end) {
        r = r << 8 | static_cast<unsigned char>(*begin++);

        // we just added 8 bits, so we can *always* consume 6 to produce one character, so (net) we
        // are adding 2 bits.
        bits += 2;
        *out++ = detail::b64_lut.to_b64(r >> bits); // Right-shift off the bits we aren't consuming right now

        // Drop the bits we don't want to keep (because we just consumed them)
        r &= (1 << bits) - 1;

        if (bits == 6) { // We have enough bits to produce a second character (which means we had 4 before and added 8)
            bits = 0;
            *out++ = detail::b64_lut.to_b64(r);
            r = 0;
        }
    }

    // If bits == 0 then we ended our 6-bit outputs coinciding with 8-bit values, i.e. at a multiple
    // of 24 bits: this means we don't have anything else to output and don't need any padding.
    if (bits == 2) {
        // We finished with 2 unconsumed bits, which means we ended 1 byte past a 24-bit group (e.g.
        // 1 byte, 4 bytes, 301 bytes, etc.); since we need to always be a multiple of 4 output
        // characters that means we've produced 1: so we right-fill 0s to get the next char, then
        // add two padding ='s.
        *out++ = detail::b64_lut.to_b64(r << 4);
        *out++ = '=';
        *out++ = '=';
    } else if (bits == 4) {
        // 4 bits left means we produced 2 6-bit values from the first 2 bytes of a 3-byte group.
        // Fill 0s to get the last one, plus one padding output.
        *out++ = detail::b64_lut.to_b64(r << 2);
        *out++ = '=';
    }
}

/// Creates and returns a base64 string from an iterator pair of a character sequence
template <typename It>
std::string to_base64(It begin, It end) {
    std::string base64;
    if constexpr (std::is_base_of_v<std::random_access_iterator_tag, typename std::iterator_traits<It>::iterator_category>)
        base64.reserve((std::distance(begin, end) + 2) / 3 * 4); // bytes*4/3, rounded up to the next multiple of 4
    to_base64(begin, end, std::back_inserter(base64));
    return base64;
}

/// Creates a base64 string from an iterable, std::string-like object
template <typename CharT>
std::string to_base64(std::basic_string_view<CharT> s) { return to_base64(s.begin(), s.end()); }
inline std::string to_base64(std::string_view s) { return to_base64<>(s); }

/// Returns true if the range is a base64 encoded value; we allow (but do not require) '=' padding,
/// but only at the end, only 1 or 2, and only if it pads out the total to a multiple of 4.
template <typename It>
constexpr bool is_base64(It begin, It end) {
    static_assert(sizeof(decltype(*begin)) == 1, "is_base64 requires chars/bytes");
    using std::distance;
    using std::prev;

    // Allow 1 or 2 padding chars *if* they pad it to a multiple of 4.
    if (begin != end && distance(begin, end) % 4 == 0) {
        auto last = prev(end);
        if (static_cast<unsigned char>(*last) == '=')
            end = last--;
        if (static_cast<unsigned char>(*last) == '=')
            end = last;
    }

    for (; begin != end; ++begin) {
        auto c = static_cast<unsigned char>(*begin);
        if (detail::b64_lut.from_b64(c) == 0 && c != 'A')
            return false;
    }
    return true;
}

/// Returns true if the string-like value is a base64 encoded value
template <typename CharT>
constexpr bool is_base64(std::basic_string_view<CharT> s) { return is_base64(s.begin(), s.end()); }
constexpr bool is_base64(std::string_view s) { return is_base64(s.begin(), s.end()); }

/// Converts a sequence of base64 digits to bytes.  Undefined behaviour if any characters are not
/// valid base64 alphabet characters.  It is permitted for the input and output ranges to overlap as
/// long as `out` is no earlier than `begin`.  Trailing padding characters are permitted but not
/// required.
///
/// It is possible to provide "impossible" base64 encoded values; for example "YWJja" which has 30
/// bits of data even though a base64 encoded byte string should have 24 (4 chars) or 36 (6 chars)
/// bits for a 3- and 4-byte input, respectively.  We ignore any such "impossible" bits, and
/// similarly ignore impossible bits in the bit "overhang"; that means "YWJjZA==" (the proper
/// encoding of "abcd") and "YWJjZB", "YWJjZC", ..., "YWJjZP" all decode to the same "abcd" value:
/// the last 4 bits of the last character are essentially considered padding.
template <typename InputIt, typename OutputIt>
void from_base64(InputIt begin, InputIt end, OutputIt out) {
    static_assert(sizeof(decltype(*begin)) == 1, "from_base64 requires chars/bytes");
    uint_fast16_t curr = 0;
    int bits = 0; // number of bits we've loaded into val; we always keep this < 8.
    while (begin != end) {
        auto c = static_cast<unsigned char>(*begin++);

        // padding; don't bother checking if we're at the end because is_base64 is a precondition
        // and we're allowed UB if it isn't satisfied.
        if (c == '=') continue;

        curr = curr << 6 | detail::b64_lut.from_b64(c);
        if (bits == 0)
            bits = 6;
        else {
            bits -= 2; // Added 6, removing 8
            *out++ = static_cast<uint8_t>(curr >> bits);
            curr &= (1 << bits) - 1;
        }
    }
    // Don't worry about leftover bits because either they have to be 0, or they can't happen at
    // all.  See base32z.h for why: the reasoning is exactly the same (except using 6 bits per
    // character here instead of 5).
}

/// Converts base64 digits from a iterator pair of characters into a std::string of bytes.
/// Undefined behaviour if any characters are not valid base64 characters.
template <typename It>
std::string from_base64(It begin, It end) {
    std::string bytes;
    if constexpr (std::is_base_of_v<std::random_access_iterator_tag, typename std::iterator_traits<It>::iterator_category>)
        bytes.reserve(std::distance(begin, end)*6 / 8); // each digit carries 6 bits; this may overallocate by 1-2 bytes due to padding
    from_base64(begin, end, std::back_inserter(bytes));
    return bytes;
}

/// Converts base64 digits from a std::string-like object into a std::string of bytes.  Undefined
/// behaviour if any characters are not valid base64 characters.
template <typename CharT>
std::string from_base64(std::basic_string_view<CharT> s) { return from_base64(s.begin(), s.end()); }
inline std::string from_base64(std::string_view s) { return from_base64<>(s); }

}
