// Copyright (c) 2019-2021, The Oxen Project
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
#include "byte_type.h"

namespace oxenmq {

namespace detail {

/// Compile-time generated lookup tables for base32z conversion.  This is case insensitive (though
/// for byte -> b32z conversion we always produce lower case).
struct b32z_table {
    // Store the 0-31 decoded value of every possible char; all the chars that aren't valid are set
    // to 0.  (If you don't trust your data, check it with is_base32z first, which uses these 0's
    // to detect invalid characters -- which is why we want a full 256 element array).
    char from_b32z_lut[256];
    // Store the encoded character of every 0-31 (5 bit) value.
    char to_b32z_lut[32];

    // constexpr constructor that fills out the above (and should do it at compile time for any half
    // decent compiler).
    constexpr b32z_table() noexcept : from_b32z_lut{},
              to_b32z_lut{
                  'y', 'b', 'n', 'd', 'r', 'f', 'g', '8', 'e', 'j', 'k', 'm', 'c', 'p', 'q', 'x',
                  'o', 't', '1', 'u', 'w', 'i', 's', 'z', 'a', '3', '4', '5', 'h', '7', '6', '9'
              }
    {
        for (unsigned char c = 0; c < 32; c++) {
            unsigned char x = to_b32z_lut[c];
            from_b32z_lut[x] = c;
            if (x >= 'a' && x <= 'z')
                from_b32z_lut[x - 'a' + 'A'] = c;
        }
    }
    // Convert a b32z encoded character into a 0-31 value
    constexpr char from_b32z(unsigned char c) const noexcept { return from_b32z_lut[c]; }
    // Convert a 0-31 value into a b32z encoded character
    constexpr char to_b32z(unsigned char b) const noexcept { return to_b32z_lut[b]; }
} constexpr b32z_lut;

// This main point of this static assert is to force the compiler to compile-time build the constexpr tables.
static_assert(b32z_lut.from_b32z('w') == 20 && b32z_lut.from_b32z('T') == 17 && b32z_lut.to_b32z(5) == 'f', "");

} // namespace detail

/// Returns the number of characters required to encode a base32z string from the given number of bytes.
inline constexpr size_t to_base32z_size(size_t byte_size) { return (byte_size*8 + 4) / 5; } // ⌈bits/5⌉ because 5 bits per byte
/// Returns the (maximum) number of bytes required to decode a base32z string of the given size.
inline constexpr size_t from_base32z_size(size_t b32z_size) { return b32z_size*5 / 8; } // ⌊bits/8⌋

/// Converts bytes into a base32z encoded character sequence, writing them starting at `out`.
/// Returns the final value of out (i.e. the iterator positioned just after the last written base32z
/// character).
template <typename InputIt, typename OutputIt>
OutputIt to_base32z(InputIt begin, InputIt end, OutputIt out) {
    static_assert(sizeof(decltype(*begin)) == 1, "to_base32z requires chars/bytes");
    int bits = 0; // Tracks the number of unconsumed bits held in r, will always be in [0, 4]
    std::uint_fast16_t r = 0;
    while (begin != end) {
        r = r << 8 | static_cast<unsigned char>(*begin++);

        // we just added 8 bits, so we can *always* consume 5 to produce one character, so (net) we
        // are adding 3 bits.
        bits += 3;
        *out++ = detail::b32z_lut.to_b32z(r >> bits); // Right-shift off the bits we aren't consuming right now

        // Drop the bits we don't want to keep (because we just consumed them)
        r &= (1 << bits) - 1;

        if (bits >= 5) { // We have enough bits to produce a second character; essentially the same as above
            bits -= 5; // Except now we are just consuming 5 without having added any more
            *out++ = detail::b32z_lut.to_b32z(r >> bits);
            r &= (1 << bits) - 1;
        }
    }

    if (bits > 0) // We hit the end, but still have some unconsumed bits so need one final character to append
        *out++ = detail::b32z_lut.to_b32z(r << (5 - bits));

    return out;
}

/// Creates a base32z string from an iterator pair of a byte sequence.
template <typename It>
std::string to_base32z(It begin, It end) {
    std::string base32z;
    if constexpr (std::is_base_of_v<std::random_access_iterator_tag, typename std::iterator_traits<It>::iterator_category>) {
        using std::distance;
        base32z.reserve(to_base32z_size(distance(begin, end)));
    }
    to_base32z(begin, end, std::back_inserter(base32z));
    return base32z;
}

/// Creates a base32z string from an iterable, std::string-like object
template <typename CharT>
std::string to_base32z(std::basic_string_view<CharT> s) { return to_base32z(s.begin(), s.end()); }
inline std::string to_base32z(std::string_view s) { return to_base32z<>(s); }

/// Returns true if the given [begin, end) range is an acceptable base32z string: specifically every
/// character must be in the base32z alphabet, and the string must be a valid encoding length that
/// could have been produced by to_base32z (i.e. some lengths are impossible).
template <typename It>
constexpr bool is_base32z(It begin, It end) {
    static_assert(sizeof(decltype(*begin)) == 1, "is_base32z requires chars/bytes");
    size_t count = 0;
    constexpr bool random = std::is_base_of_v<std::random_access_iterator_tag, typename std::iterator_traits<It>::iterator_category>;
    if constexpr (random) {
        using std::distance;
        count = distance(begin, end) % 8;
        if (count == 1 || count == 3 || count == 6) // see below
            return false;
    }
    for (; begin != end; ++begin) {
        auto c = static_cast<unsigned char>(*begin);
        if (detail::b32z_lut.from_b32z(c) == 0 && !(c == 'y' || c == 'Y'))
            return false;
        if constexpr (!random)
            count++;
    }
    // Check for a valid length.
    // - 5n + 0 bytes encodes to 8n chars (no padding bits)
    // - 5n + 1 bytes encodes to 8n+2 chars (last 2 bits are padding)
    // - 5n + 2 bytes encodes to 8n+4 chars (last 4 bits are padding)
    // - 5n + 3 bytes encodes to 8n+5 chars (last 1 bit is padding)
    // - 5n + 4 bytes encodes to 8n+7 chars (last 3 bits are padding)
    if constexpr (!random)
        if (count %= 8; count == 1 || count == 3 || count == 6)
            return false;
    return true;
}

/// Returns true if all elements in the string-like value are base32z characters
template <typename CharT>
constexpr bool is_base32z(std::basic_string_view<CharT> s) { return is_base32z(s.begin(), s.end()); }
constexpr bool is_base32z(std::string_view s) { return is_base32z<>(s); }

/// Converts a sequence of base32z digits to bytes.  Undefined behaviour if any characters are not
/// valid base32z alphabet characters.  It is permitted for the input and output ranges to overlap
/// as long as `out` is no later than `begin`.  Note that if you pass in a sequence that could not
/// have been created by a base32z encoding of a byte sequence, we treat the excess bits as if they
/// were not provided.  Returns the final value of out (that is, the iterator positioned just after
/// the last written character).
///
/// For example, "yyy" represents a 15-bit value, but a byte sequence is either 8-bit (requiring 2
/// characters) or 16-bit (requiring 4).  Similarly, "yb" is an impossible encoding because it has
/// its 10th bit set (b = 00001), but a base32z encoded value should have all 0's beyond the 8th (or
/// 16th or 24th or ... bit).  We treat any such bits as if they were not specified (even if they
/// are): which means "yy", "yb", "yyy", "yy9", "yd", etc. all decode to the same 1-byte value "\0".
template <typename InputIt, typename OutputIt>
OutputIt from_base32z(InputIt begin, InputIt end, OutputIt out) {
    static_assert(sizeof(decltype(*begin)) == 1, "from_base32z requires chars/bytes");
    uint_fast16_t curr = 0;
    int bits = 0; // number of bits we've loaded into val; we always keep this < 8.
    while (begin != end) {
        curr = curr << 5 | detail::b32z_lut.from_b32z(static_cast<unsigned char>(*begin++));
        if (bits >= 3) {
            bits -= 3; // Added 5, removing 8
            *out++ = static_cast<detail::byte_type_t<OutputIt>>(
                    static_cast<uint8_t>(curr >> bits));
            curr &= (1 << bits) - 1;
        } else {
            bits += 5;
        }
    }

    // Ignore any trailing bits.  base32z encoding always has at least as many bits as the source
    // bytes, which means we should not be able to get here from a properly encoded b32z value with
    // anything other than 0s: if we have no extra bits (e.g. 5 bytes == 8 b32z chars) then we have
    // a 0-bit value; if we have some extra bits (e.g. 6 bytes requires 10 b32z chars, but that
    // contains 50 bits > 48 bits) then those extra bits will be 0s (and this covers the bits -= 3
    // case above: it'll leave us with 0-4 extra bits, but those extra bits would be 0 if produced
    // from an actual byte sequence).
    //
    // The "bits += 5" case, then, means that we could end with 5-7 bits.  This, however, cannot be
    // produced by a valid encoding:
    // - 0 bytes gives us 0 chars with 0 leftover bits
    // - 1 byte gives us 2 chars with 2 leftover bits
    // - 2 bytes gives us 4 chars with 4 leftover bits
    // - 3 bytes gives us 5 chars with 1 leftover bit
    // - 4 bytes gives us 7 chars with 3 leftover bits
    // - 5 bytes gives us 8 chars with 0 leftover bits (this is where the cycle repeats)
    //
    // So really the only way we can get 5-7 leftover bits is if you took a 0, 2 or 5 char output (or
    // any 8n + {0,2,5} char output) and added a base32z character to the end.  If you do that,
    // well, too bad: you're giving invalid output and so we're just going to pretend that extra
    // character you added isn't there by not doing anything here.

    return out;
}

/// Convert a base32z sequence into a std::string of bytes.  Undefined behaviour if any characters
/// are not valid (case-insensitive) base32z characters.
template <typename It>
std::string from_base32z(It begin, It end) {
    std::string bytes;
    if constexpr (std::is_base_of_v<std::random_access_iterator_tag, typename std::iterator_traits<It>::iterator_category>) {
        using std::distance;
        bytes.reserve(from_base32z_size(distance(begin, end)));
    }
    from_base32z(begin, end, std::back_inserter(bytes));
    return bytes;
}

/// Converts base32z digits from a std::string-like object into a std::string of bytes.  Undefined
/// behaviour if any characters are not valid (case-insensitive) base32z characters.
template <typename CharT>
std::string from_base32z(std::basic_string_view<CharT> s) { return from_base32z(s.begin(), s.end()); }
inline std::string from_base32z(std::string_view s) { return from_base32z<>(s); }

}
