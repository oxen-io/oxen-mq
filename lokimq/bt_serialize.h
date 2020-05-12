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

#include <iostream>

#pragma once

#include <vector>
#include <list>
#include <unordered_map>
#include <algorithm>
#include <functional>
#include <cstring>
#include <ostream>
#include <sstream>
#include <string_view>
#include "mapbox/variant.hpp"
#include <variant>


namespace lokimq {

using namespace std::literals;

/** \file
 * LokiMQ serialization for internal commands is very simple: we support two primitive types,
 * strings and integers, and two container types, lists and dicts with string keys.  On the wire
 * these go in BitTorrent byte encoding as described in BEP-0003
 * (https://www.bittorrent.org/beps/bep_0003.html#bencoding).
 *
 * On the C++ side, on input we allow strings, integral types, STL-like containers of these types,
 * and STL-like containers of pairs with a string first value and any of these types as second
 * value.  We also accept std::variants (if compiled with std::variant support, i.e. in C++17 mode)
 * that contain any of these, and mapbox::util::variants (the internal type used for its recursive
 * support).
 *
 * One minor deviation from BEP-0003 is that we don't support serializing values that don't fit in a
 * 64-bit integer (BEP-0003 specifies arbitrary precision integers).
 *
 * On deserialization we can either deserialize into a mapbox::util::variant that supports everything, or
 * we can fill a container of your given type (though this fails if the container isn't compatible
 * with the deserialized data).
 */

/// Exception throw if deserialization fails
class bt_deserialize_invalid : public std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

/// A more specific subclass that is thown if the serialization type is an initial mismatch: for
/// example, trying deserializing an int but the next thing in input is a list.  This is not,
/// however, thrown if the type initially looks fine but, say, a nested serialization fails.  This
/// error will only be thrown when the input stream has not been advanced (and so can be tried for a
/// different type).
class bt_deserialize_invalid_type : public bt_deserialize_invalid {
    using bt_deserialize_invalid::bt_deserialize_invalid;
};

class bt_list;
class bt_dict;

/// Special type wrapper for storing a uint64_t value that may need to be larger than an int64_t.
/// You *can* shove a uint64_t directly into a bt_value, but it will end up on the wire as its
/// 2s-complement int64_t value; using this wrapper instead allows you to force a 64-bit positive
/// integer onto the wire.
struct bt_u64 { uint64_t val; explicit bt_u64(uint64_t val) : val{val} {} };

/// Recursive generic type that can fully represent everything valid for a BT serialization.
using bt_value = mapbox::util::variant<
    std::string,
    std::string_view,
    int64_t,
    bt_u64,
    mapbox::util::recursive_wrapper<bt_list>,
    mapbox::util::recursive_wrapper<bt_dict>
>;

/// Very thin wrapper around a std::list<bt_value> that holds a list of generic values (though *any*
/// compatible data type can be used).
class bt_list : public std::list<bt_value> {
    using std::list<bt_value>::list;
};
/// Very thin wrapper around a std::unordered_map<bt_value> that holds a list of string -> generic
/// value pairs (though *any* compatible data type can be used).
class bt_dict : public std::unordered_map<std::string, bt_value> {
    using std::unordered_map<std::string, bt_value>::unordered_map;
};

namespace detail {

/// Reads digits into an unsigned 64-bit int.
uint64_t extract_unsigned(std::string_view& s);
// (Provide non-constant lvalue and rvalue ref functions so that we only accept explicit
// string_views but not implicitly converted ones)
inline uint64_t extract_unsigned(std::string_view&& s) { return extract_unsigned(s); }

// Fallback base case; we only get here if none of the partial specializations below work
template <typename T, typename SFINAE = void>
struct bt_serialize { static_assert(!std::is_same<T, T>::value, "Cannot serialize T: unsupported type for bt serialization"); };

template <typename T, typename SFINAE = void>
struct bt_deserialize { static_assert(!std::is_same<T, T>::value, "Cannot deserialize T: unsupported type for bt deserialization"); };

/// Checks that we aren't at the end of a string view and throws if we are.
inline void bt_need_more(const std::string_view &s) {
    if (s.empty())
        throw bt_deserialize_invalid{"Unexpected end of string while deserializing"};
}

union maybe_signed_int64_t { int64_t i64; uint64_t u64; };

/// Deserializes a signed or unsigned 64-bit integer from a string.  Sets the second bool to true
/// iff the value is int64_t because a negative value was read.  Throws an exception if the read
/// value doesn't fit in a int64_t (if negative) or a uint64_t (if positive).  Removes consumed
/// characters from the string_view.
std::pair<maybe_signed_int64_t, bool> bt_deserialize_integer(std::string_view& s);

/// Integer specializations
template <typename T>
struct bt_serialize<T, std::enable_if_t<std::is_integral<T>::value>> {
    static_assert(sizeof(T) <= sizeof(uint64_t), "Serialization of integers larger than uint64_t is not supported");
    void operator()(std::ostream &os, const T &val) {
        // Cast 1-byte types to a larger type to avoid iostream interpreting them as single characters
        using output_type = std::conditional_t<(sizeof(T) > 1), T, std::conditional_t<std::is_signed<T>::value, int, unsigned>>;
        os << 'i' << static_cast<output_type>(val) << 'e';
    }
};

template <typename T>
struct bt_deserialize<T, std::enable_if_t<std::is_integral<T>::value>> {
    void operator()(std::string_view& s, T &val) {
        constexpr uint64_t umax = static_cast<uint64_t>(std::numeric_limits<T>::max());
        constexpr int64_t smin = static_cast<int64_t>(std::numeric_limits<T>::min()),
                          smax = static_cast<int64_t>(std::numeric_limits<T>::max());

        auto read = bt_deserialize_integer(s);
        if (std::is_signed<T>::value) {
            if (!read.second) { // read a positive value
                if (read.first.u64 > umax)
                    throw bt_deserialize_invalid("Integer deserialization failed: found too-large value " + std::to_string(read.first.u64) + " > " + std::to_string(umax));
                val = static_cast<T>(read.first.u64);
            } else {
                bool oob = read.first.i64 < smin || read.first.i64 > smax;
                if (sizeof(T) < sizeof(int64_t) && oob)
                    throw bt_deserialize_invalid("Integer deserialization failed: found out-of-range value " + std::to_string(read.first.i64) + " not in [" + std::to_string(smin) + "," + std::to_string(smax) + "]");
                val = static_cast<T>(read.first.i64);
            }
        } else {
            if (read.second)
                throw bt_deserialize_invalid("Integer deserialization failed: found negative value " + std::to_string(read.first.i64) + " but type is unsigned");
            if (sizeof(T) < sizeof(uint64_t) && read.first.u64 > umax)
                throw bt_deserialize_invalid("Integer deserialization failed: found too-large value " + std::to_string(read.first.u64) + " > " + std::to_string(umax));
            val = static_cast<T>(read.first.u64);
        }
    }
};

extern template struct bt_deserialize<int64_t>;
extern template struct bt_deserialize<uint64_t>;

template<>
struct bt_serialize<bt_u64> { void operator()(std::ostream& os, bt_u64 val) { bt_serialize<uint64_t>{}(os, val.val); } };
template<>
struct bt_deserialize<bt_u64> { void operator()(std::string_view& s, bt_u64& val) { bt_deserialize<uint64_t>{}(s, val.val); } };

template <>
struct bt_serialize<std::string_view> {
    void operator()(std::ostream &os, const std::string_view &val) { os << val.size(); os.put(':'); os.write(val.data(), val.size()); }
};
template <>
struct bt_deserialize<std::string_view> {
    void operator()(std::string_view& s, std::string_view& val);
};

/// String specialization
template <>
struct bt_serialize<std::string> {
    void operator()(std::ostream &os, const std::string &val) { bt_serialize<std::string_view>{}(os, val); }
};
template <>
struct bt_deserialize<std::string> {
    void operator()(std::string_view& s, std::string& val) { std::string_view view; bt_deserialize<std::string_view>{}(s, view); val = {view.data(), view.size()}; }
};

/// char * and string literals -- we allow serialization for convenience, but not deserialization
template <>
struct bt_serialize<char *> {
    void operator()(std::ostream &os, const char *str) { bt_serialize<std::string_view>{}(os, {str, std::strlen(str)}); }
};
template <size_t N>
struct bt_serialize<char[N]> {
    void operator()(std::ostream &os, const char *str) { bt_serialize<std::string_view>{}(os, {str, N-1}); }
};

/// Partial dict validity; we don't check the second type for serializability, that will be handled
/// via the base case static_assert if invalid.
template <typename T, typename = void> struct is_bt_input_dict_container : std::false_type {};
template <typename T>
struct is_bt_input_dict_container<T, std::enable_if_t<
    std::is_same<std::string, std::remove_cv_t<typename T::value_type::first_type>>::value,
    std::void_t<typename T::const_iterator /* is const iterable */,
           typename T::value_type::second_type /* has a second type */>>>
: std::true_type {};

/// Determines whether the type looks like something we can insert into (using `v.insert(v.end(), x)`)
template <typename T, typename = void> struct is_bt_insertable : std::false_type {};
template <typename T>
struct is_bt_insertable<T,
    std::void_t<decltype(std::declval<T>().insert(std::declval<T>().end(), std::declval<typename T::value_type>()))>>
: std::true_type {};

/// Determines whether the given type looks like a compatible map (i.e. has std::string keys) that
/// we can insert into.
template <typename T, typename = void> struct is_bt_output_dict_container : std::false_type {};
template <typename T>
struct is_bt_output_dict_container<T, std::enable_if_t<
    std::is_same<std::string, std::remove_cv_t<typename T::key_type>>::value &&
    is_bt_insertable<T>::value,
    std::void_t<typename T::value_type::second_type /* has a second type */>>>
: std::true_type {};


/// Specialization for a dict-like container (such as an unordered_map).  We accept anything for a
/// dict that is const iterable over something that looks like a pair with std::string for first
/// value type.  The value (i.e. second element of the pair) also must be serializable.
template <typename T>
struct bt_serialize<T, std::enable_if_t<is_bt_input_dict_container<T>::value>> {
    using second_type = typename T::value_type::second_type;
    using ref_pair = std::reference_wrapper<const typename T::value_type>;
    void operator()(std::ostream &os, const T &dict) {
        os << 'd';
        std::vector<ref_pair> pairs;
        pairs.reserve(dict.size());
        for (const auto &pair : dict)
            pairs.emplace(pairs.end(), pair);
        std::sort(pairs.begin(), pairs.end(), [](ref_pair a, ref_pair b) { return a.get().first < b.get().first; });
        for (auto &ref : pairs) {
            bt_serialize<std::string>{}(os, ref.get().first);
            bt_serialize<second_type>{}(os, ref.get().second);
        }
        os << 'e';
    }
};

template <typename T>
struct bt_deserialize<T, std::enable_if_t<is_bt_output_dict_container<T>::value>> {
    using second_type = typename T::value_type::second_type;
    void operator()(std::string_view& s, T& dict) {
        // Smallest dict is 2 bytes "de", for an empty dict.
        if (s.size() < 2) throw bt_deserialize_invalid("Deserialization failed: end of string found where dict expected");
        if (s[0] != 'd') throw bt_deserialize_invalid_type("Deserialization failed: expected 'd', found '"s + s[0] + "'"s);
        s.remove_prefix(1);
        dict.clear();
        bt_deserialize<std::string> key_deserializer;
        bt_deserialize<second_type> val_deserializer;

        while (!s.empty() && s[0] != 'e') {
            std::string key;
            second_type val;
            key_deserializer(s, key);
            val_deserializer(s, val);
            dict.insert(dict.end(), typename T::value_type{std::move(key), std::move(val)});
        }
        if (s.empty())
            throw bt_deserialize_invalid("Deserialization failed: encountered end of string before dict was finished");
        s.remove_prefix(1); // Consume the 'e'
    }
};


/// Accept anything that looks iterable; value serialization validity isn't checked here (it fails
/// via the base case static assert).
template <typename T, typename = void> struct is_bt_input_list_container : std::false_type {};
template <typename T>
struct is_bt_input_list_container<T, std::enable_if_t<
    !std::is_same<T, std::string>::value &&
    !is_bt_input_dict_container<T>::value,
    std::void_t<typename T::const_iterator, typename T::value_type>>>
: std::true_type {};

template <typename T, typename = void> struct is_bt_output_list_container : std::false_type {};
template <typename T>
struct is_bt_output_list_container<T, std::enable_if_t<
    !std::is_same<T, std::string>::value &&
    !is_bt_output_dict_container<T>::value &&
    is_bt_insertable<T>::value>>
: std::true_type {};


/// List specialization
template <typename T>
struct bt_serialize<T, std::enable_if_t<is_bt_input_list_container<T>::value>> {
    void operator()(std::ostream& os, const T& list) {
        os << 'l';
        for (const auto &v : list)
            bt_serialize<std::remove_cv_t<typename T::value_type>>{}(os, v);
        os << 'e';
    }
};
template <typename T>
struct bt_deserialize<T, std::enable_if_t<is_bt_output_list_container<T>::value>> {
    using value_type = typename T::value_type;
    void operator()(std::string_view& s, T& list) {
        // Smallest list is 2 bytes "le", for an empty list.
        if (s.size() < 2) throw bt_deserialize_invalid("Deserialization failed: end of string found where list expected");
        if (s[0] != 'l') throw bt_deserialize_invalid_type("Deserialization failed: expected 'l', found '"s + s[0] + "'"s);
        s.remove_prefix(1);
        list.clear();
        bt_deserialize<value_type> deserializer;
        while (!s.empty() && s[0] != 'e') {
            value_type v;
            deserializer(s, v);
            list.insert(list.end(), std::move(v));
        }
        if (s.empty())
            throw bt_deserialize_invalid("Deserialization failed: encountered end of string before list was finished");
        s.remove_prefix(1); // Consume the 'e'
    }
};

/// variant visitor; serializes whatever is contained
class bt_serialize_visitor {
    std::ostream &os;
public:
    using result_type = void;
    bt_serialize_visitor(std::ostream &os) : os{os} {}
    template <typename T> void operator()(const T &val) const {
        bt_serialize<T>{}(os, val);
    }
};

template <typename T>
using is_bt_deserializable = std::integral_constant<bool,
    std::is_same<T, std::string>::value || std::is_integral<T>::value ||
    is_bt_output_dict_container<T>::value || is_bt_output_list_container<T>::value>;

// General template and base case; this base will only actually be invoked when Ts... is empty,
// which means we reached the end without finding any variant type capable of holding the value.
template <typename SFINAE, typename Variant, typename... Ts>
struct bt_deserialize_try_variant_impl {
    void operator()(std::string_view&, Variant&) {
        throw bt_deserialize_invalid("Deserialization failed: could not deserialize value into any variant type");
    }
};

template <typename... Ts, typename Variant>
void bt_deserialize_try_variant(std::string_view& s, Variant& variant) {
    bt_deserialize_try_variant_impl<void, Variant, Ts...>{}(s, variant);
}


template <typename Variant, typename T, typename... Ts>
struct bt_deserialize_try_variant_impl<std::enable_if_t<is_bt_deserializable<T>::value>, Variant, T, Ts...> {
    void operator()(std::string_view& s, Variant& variant) {
        if (    is_bt_output_list_container<T>::value ? s[0] == 'l' :
                is_bt_output_dict_container<T>::value ? s[0] == 'd' :
                std::is_integral<T>::value            ? s[0] == 'i' :
                std::is_same<T, std::string>::value   ? s[0] >= '0' && s[0] <= '9' :
                false) {
            T val;
            bt_deserialize<T>{}(s, val);
            variant = std::move(val);
        } else {
            bt_deserialize_try_variant<Ts...>(s, variant);
        }
    }
};

template <typename Variant, typename T, typename... Ts>
struct bt_deserialize_try_variant_impl<std::enable_if_t<!is_bt_deserializable<T>::value>, Variant, T, Ts...> {
    void operator()(std::string_view& s, Variant& variant) {
        // Unsupported deserialization type, skip it
        bt_deserialize_try_variant<Ts...>(s, variant);
    }
};

template <>
struct bt_deserialize<bt_value, void> {
    void operator()(std::string_view& s, bt_value& val);
};

template <typename... Ts>
struct bt_serialize<mapbox::util::variant<Ts...>> {
    void operator()(std::ostream& os, const mapbox::util::variant<Ts...>& val) {
        mapbox::util::apply_visitor(bt_serialize_visitor{os}, val);
    }
};

template <typename... Ts>
struct bt_deserialize<mapbox::util::variant<Ts...>> {
    void operator()(std::string_view& s, mapbox::util::variant<Ts...>& val) {
        bt_deserialize_try_variant<Ts...>(s, val);
    }
};


/// C++17 std::variant support
template <typename... Ts>
struct bt_serialize<std::variant<Ts...>> {
    void operator()(std::ostream &os, const std::variant<Ts...>& val) {
        mapbox::util::apply_visitor(bt_serialize_visitor{os}, val);
    }
};

template <typename... Ts>
struct bt_deserialize<std::variant<Ts...>> {
    void operator()(std::string_view& s, std::variant<Ts...>& val) {
        bt_deserialize_try_variant<Ts...>(s, val);
    }
};

template <typename T>
struct bt_stream_serializer {
    const T &val;
    explicit bt_stream_serializer(const T &val) : val{val} {}
    operator std::string() const {
        std::ostringstream oss;
        oss << *this;
        return oss.str();
    }
};
template <typename T>
std::ostream &operator<<(std::ostream &os, const bt_stream_serializer<T> &s) {
    bt_serialize<T>{}(os, s.val);
    return os;
}

} // namespace detail


/// Returns a wrapper around a value reference that can serialize the value directly to an output
/// stream.  This class is intended to be used inline (i.e. without being stored) as in:
///
///     std::list<int> my_list{{1,2,3}};
///     std::cout << bt_serializer(my_list);
///
/// While it is possible to store the returned object and use it, such as:
///
///     auto encoded = bt_serializer(42);
///     std::cout << encoded;
///
/// this approach is not generally recommended: the returned object stores a reference to the
/// passed-in type, which may not survive.  If doing this note that it is the caller's
/// responsibility to ensure the serializer is not used past the end of the lifetime of the value
/// being serialized.
///
/// Also note that serializing directly to an output stream is more efficient as no intermediate
/// string containing the entire serialization has to be constructed.
///
template <typename T>
detail::bt_stream_serializer<T> bt_serializer(const T &val) { return detail::bt_stream_serializer<T>{val}; }

/// Serializes the given value into a std::string.
///
///     int number = 42;
///     std::string encoded = bt_serialize(number);
///     // Equivalent:
///     //auto encoded = (std::string) bt_serialize(number);
///
/// This takes any serializable type: integral types, strings, lists of serializable types, and
/// string->value maps of serializable types.
template <typename T>
std::string bt_serialize(const T &val) { return bt_serializer(val); }

/// Deserializes the given string view directly into `val`.  Usage:
///
///     std::string encoded = "i42e";
///     int value;
///     bt_deserialize(encoded, value); // Sets value to 42
///
template <typename T, std::enable_if_t<!std::is_const<T>::value, int> = 0>
void bt_deserialize(std::string_view s, T& val) {
    return detail::bt_deserialize<T>{}(s, val);
}


/// Deserializes the given string_view into a `T`, which is returned.
///
///     std::string encoded = "li1ei2ei3ee"; // bt-encoded list of ints: [1,2,3]
///     auto mylist = bt_deserialize<std::list<int>>(encoded);
///
template <typename T>
T bt_deserialize(std::string_view s) {
    T val;
    bt_deserialize(s, val);
    return val;
}

/// Deserializes the given value into a generic `bt_value` type (mapbox::util::variant) which is capable
/// of holding all possible BT-encoded values (including recursion).
///
/// Example:
///
///     std::string encoded = "i42e";
///     auto val = bt_get(encoded);
///     int v = get_int<int>(val); // fails unless the encoded value was actually an integer that
///                                // fits into an `int`
///
inline bt_value bt_get(std::string_view s) {
    return bt_deserialize<bt_value>(s);
}

/// Helper functions to extract a value of some integral type from a bt_value which contains an
/// integer.  Does range checking, throwing std::overflow_error if the stored value is outside the
/// range of the target type.
///
/// Example:
///
///     std::string encoded = "i123456789e";
///     auto val = bt_get(encoded);
///     auto v = get_int<uint32_t>(val); // throws if the decoded value doesn't fit in a uint32_t
template <typename IntType, std::enable_if_t<std::is_integral<IntType>::value, int> = 0>
IntType get_int(const bt_value &v) {
    // It's highly unlikely that this code ever runs on a non-2s-complement architecture, but check
    // at compile time if converting to a uint64_t (because while int64_t -> uint64_t is
    // well-defined, uint64_t -> int64_t only does the right thing under 2's complement).
    static_assert(!std::is_unsigned<IntType>::value || sizeof(IntType) != sizeof(int64_t) || -1 == ~0,
            "Non 2s-complement architecture not supported!");
    int64_t value = mapbox::util::get<int64_t>(v);
    if (sizeof(IntType) < sizeof(int64_t)) {
        if (value > static_cast<int64_t>(std::numeric_limits<IntType>::max())
                || value < static_cast<int64_t>(std::numeric_limits<IntType>::min()))
            throw std::overflow_error("Unable to extract integer value: stored value is outside the range of the requested type");
    }
    return static_cast<IntType>(value);
}

/// Class that allows you to walk through a bt-encoded list in memory without copying or allocating
/// memory.  It accesses existing memory directly and so the caller must ensure that the referenced
/// memory stays valid for the lifetime of the bt_list_consumer object.
class bt_list_consumer {
protected:
    std::string_view data;
    bt_list_consumer() = default;
public:
    bt_list_consumer(std::string_view data_);

    /// Copy constructor.  Making a copy copies the current position so can be used for multipass
    /// iteration through a list.
    bt_list_consumer(const bt_list_consumer&) = default;
    bt_list_consumer& operator=(const bt_list_consumer&) = default;

    /// Returns true if the next value indicates the end of the list
    bool is_finished() const { return data.front() == 'e'; }
    /// Returns true if the next element looks like an encoded string
    bool is_string() const { return data.front() >= '0' && data.front() <= '9'; }
    /// Returns true if the next element looks like an encoded integer
    bool is_integer() const { return data.front() == 'i'; }
    /// Returns true if the next element looks like an encoded negative integer
    bool is_negative_integer() const { return is_integer() && data.size() >= 2 && data[1] == '-'; }
    /// Returns true if the next element looks like an encoded list
    bool is_list() const { return data.front() == 'l'; }
    /// Returns true if the next element looks like an encoded dict
    bool is_dict() const { return data.front() == 'd'; }

    /// Attempt to parse the next value as a string (and advance just past it).  Throws if the next
    /// value is not a string.
    std::string consume_string();
    std::string_view consume_string_view();

    /// Attempts to parse the next value as an integer (and advance just past it).  Throws if the
    /// next value is not an integer.
    template <typename IntType>
    IntType consume_integer() {
        if (!is_integer()) throw bt_deserialize_invalid_type{"next value is not an integer"};
        std::string_view next{data};
        IntType ret;
        detail::bt_deserialize<IntType>{}(next, ret);
        data = next;
        return ret;
    }

    /// Consumes a list, return it as a list-like type.  This typically requires dynamic allocation,
    /// but only has to parse the data once.  Compare with consume_list_data() which allows
    /// alloc-free traversal, but requires parsing twice (if the contents are to be used).
    template <typename T = bt_list>
    T consume_list() {
        T list;
        consume_list(list);
        return list;
    }

    /// Same as above, but takes a pre-existing list-like data type.
    template <typename T>
    void consume_list(T& list) {
        if (!is_list()) throw bt_deserialize_invalid_type{"next bt value is not a list"};
        std::string_view n{data};
        detail::bt_deserialize<T>{}(n, list);
        data = n;
    }

    /// Consumes a dict, return it as a dict-like type.  This typically requires dynamic allocation,
    /// but only has to parse the data once.  Compare with consume_dict_data() which allows
    /// alloc-free traversal, but requires parsing twice (if the contents are to be used).
    template <typename T = bt_dict>
    T consume_dict() {
        T dict;
        consume_dict(dict);
        return dict;
    }

    /// Same as above, but takes a pre-existing dict-like data type.
    template <typename T>
    void consume_dict(T& dict) {
        if (!is_dict()) throw bt_deserialize_invalid_type{"next bt value is not a dict"};
        std::string_view n{data};
        detail::bt_deserialize<T>{}(n, dict);
        data = n;
    }

    /// Consumes a value without returning it.
    void skip_value();

    /// Attempts to parse the next value as a list and returns the string_view that contains the
    /// entire thing.  This is recursive into both lists and dicts and likely to be quite
    /// inefficient for large, nested structures (unless the values only need to be skipped but
    /// aren't separately needed).  This, however, does not require dynamic memory allocation.
    std::string_view consume_list_data();

    /// Attempts to parse the next value as a dict and returns the string_view that contains the
    /// entire thing.  This is recursive into both lists and dicts and likely to be quite
    /// inefficient for large, nested structures (unless the values only need to be skipped but
    /// aren't separately needed).  This, however, does not require dynamic memory allocation.
    std::string_view consume_dict_data();
};


/// Class that allows you to walk through key-value pairs of a bt-encoded dict in memory without
/// copying or allocating memory.  It accesses existing memory directly and so the caller must
/// ensure that the referenced memory stays valid for the lifetime of the bt_dict_consumer object.
class bt_dict_consumer : private bt_list_consumer {
    std::string_view key_;

    /// Consume the key if not already consumed and there is a key present (rather than 'e').
    /// Throws exception if what should be a key isn't a string, or if the key consumes the entire
    /// data (i.e. requires that it be followed by something).  Returns true if the key was consumed
    /// (either now or previously and cached).
    bool consume_key();

    /// Clears the cached key and returns it.  Must have already called consume_key directly or
    /// indirectly via one of the `is_{...}` methods.
    std::string_view flush_key() {
        std::string_view k;
        k.swap(key_);
        return k;
    }

public:
    bt_dict_consumer(std::string_view data_);

    /// Copy constructor.  Making a copy copies the current position so can be used for multipass
    /// iteration through a list.
    bt_dict_consumer(const bt_dict_consumer&) = default;
    bt_dict_consumer& operator=(const bt_dict_consumer&) = default;

    /// Returns true if the next value indicates the end of the dict
    bool is_finished() { return !consume_key() && data.front() == 'e'; }
    /// Operator bool is an alias for `!is_finished()`
    operator bool() { return !is_finished(); }
    /// Returns true if the next value looks like an encoded string
    bool is_string() { return consume_key() && data.front() >= '0' && data.front() <= '9'; }
    /// Returns true if the next element looks like an encoded integer
    bool is_integer() { return consume_key() && data.front() == 'i'; }
    /// Returns true if the next element looks like an encoded negative integer
    bool is_negative_integer() { return is_integer() && data.size() >= 2 && data[1] == '-'; }
    /// Returns true if the next element looks like an encoded list
    bool is_list() { return consume_key() && data.front() == 'l'; }
    /// Returns true if the next element looks like an encoded dict
    bool is_dict() { return consume_key() && data.front() == 'd'; }
    /// Returns the key of the next pair.  This does not have to be called; it is also returned by
    /// all of the other consume_* methods.  The value is cached whether called here or by some
    /// other method; accessing it multiple times simple accesses the cache until the next value is
    /// consumed.
    std::string_view key() {
        if (!consume_key())
            throw bt_deserialize_invalid{"Cannot access next key: at the end of the dict"};
        return key_;
    }

    /// Attempt to parse the next value as a string->string pair (and advance just past it).  Throws
    /// if the next value is not a string.
    std::pair<std::string_view, std::string_view> next_string();

    /// Attempts to parse the next value as an string->integer pair (and advance just past it).
    /// Throws if the next value is not an integer.
    template <typename IntType>
    std::pair<std::string_view, IntType> next_integer() {
        if (!is_integer()) throw bt_deserialize_invalid_type{"next bt dict value is not an integer"};
        std::pair<std::string_view, IntType> ret;
        ret.second = bt_list_consumer::consume_integer<IntType>();
        ret.first = flush_key();
        return ret;
    }

    /// Consumes a string->list pair, return it as a list-like type.  This typically requires
    /// dynamic allocation, but only has to parse the data once.  Compare with consume_list_data()
    /// which allows alloc-free traversal, but requires parsing twice (if the contents are to be
    /// used).
    template <typename T = bt_list>
    std::pair<std::string_view, T> next_list() {
        std::pair<std::string_view, T> pair;
        pair.first = consume_list(pair.second);
        return pair;
    }

    /// Same as above, but takes a pre-existing list-like data type.  Returns the key.
    template <typename T>
    std::string_view next_list(T& list) {
        if (!is_list()) throw bt_deserialize_invalid_type{"next bt value is not a list"};
        bt_list_consumer::consume_list(list);
        return flush_key();
    }

    /// Consumes a string->dict pair, return it as a dict-like type.  This typically requires
    /// dynamic allocation, but only has to parse the data once.  Compare with consume_dict_data()
    /// which allows alloc-free traversal, but requires parsing twice (if the contents are to be
    /// used).
    template <typename T = bt_dict>
    std::pair<std::string_view, T> next_dict() {
        std::pair<std::string_view, T> pair;
        pair.first = consume_dict(pair.second);
        return pair;
    }

    /// Same as above, but takes a pre-existing dict-like data type.  Returns the key.
    template <typename T>
    std::string_view next_dict(T& dict) {
        if (!is_dict()) throw bt_deserialize_invalid_type{"next bt value is not a dict"};
        bt_list_consumer::consume_dict(dict);
        return flush_key();
    }

    /// Attempts to parse the next value as a string->list pair and returns the string_view that
    /// contains the entire thing.  This is recursive into both lists and dicts and likely to be
    /// quite inefficient for large, nested structures (unless the values only need to be skipped
    /// but aren't separately needed).  This, however, does not require dynamic memory allocation.
    std::pair<std::string_view, std::string_view> next_list_data() {
        if (data.size() < 2 || !is_list()) throw bt_deserialize_invalid_type{"next bt dict value is not a list"};
        return {flush_key(), bt_list_consumer::consume_list_data()};
    }

    /// Same as next_list_data(), but wraps the value in a bt_list_consumer for convenience
    std::pair<std::string_view, bt_list_consumer> next_list_consumer() { return next_list_data(); }

    /// Attempts to parse the next value as a string->dict pair and returns the string_view that
    /// contains the entire thing.  This is recursive into both lists and dicts and likely to be
    /// quite inefficient for large, nested structures (unless the values only need to be skipped
    /// but aren't separately needed).  This, however, does not require dynamic memory allocation.
    std::pair<std::string_view, std::string_view> next_dict_data() {
        if (data.size() < 2 || !is_dict()) throw bt_deserialize_invalid_type{"next bt dict value is not a dict"};
        return {flush_key(), bt_list_consumer::consume_dict_data()};
    }

    /// Same as next_dict_data(), but wraps the value in a bt_dict_consumer for convenience
    std::pair<std::string_view, bt_dict_consumer> next_dict_consumer() { return next_dict_data(); }

    /// Skips ahead until we find the first key >= the given key or reach the end of the dict.
    /// Returns true if we found an exact match, false if we reached some greater value or the end.
    /// If we didn't hit the end, the next `consumer_*()` call will return the key-value pair we
    /// found (either the exact match or the first key greater than the requested key).
    ///
    /// Two important notes:
    ///
    /// - properly encoded bt dicts must have lexicographically sorted keys, and this method assumes
    ///   that the input is correctly sorted (and thus if we find a greater value then your key does
    ///   not exist).
    /// - this is irreversible; you cannot returned to skipped values without reparsing.  (You *can*
    ///   however, make a copy of the bt_dict_consumer before calling and use the copy to return to
    ///   the pre-skipped position).
    bool skip_until(std::string_view find) {
        while (consume_key() && key_ < find) {
            flush_key();
            skip_value();
        }
        return key_ == find;
    }

    /// The `consume_*` functions are wrappers around next_whatever that discard the returned key.
    ///
    /// Intended for use with skip_until such as:
    ///
    ///     std::string value;
    ///     if (d.skip_until("key"))
    ///         value = d.consume_string();
    ///

    auto consume_string_view() { return next_string().second; }
    auto consume_string() { return std::string{consume_string_view()}; }

    template <typename IntType>
    auto consume_integer() { return next_integer<IntType>().second; }

    template <typename T = bt_list>
    auto consume_list() { return next_list<T>().second; }

    template <typename T>
    void consume_list(T& list) { next_list(list); }

    template <typename T = bt_dict>
    auto consume_dict() { return next_dict<T>().second; }

    template <typename T>
    void consume_dict(T& dict) { next_dict(dict); }

    std::string_view consume_list_data() { return next_list_data().second; }
    std::string_view consume_dict_data() { return next_dict_data().second; }

    bt_list_consumer consume_list_consumer() { return consume_list_data(); }
    bt_dict_consumer consume_dict_consumer() { return consume_dict_data(); }
};


} // namespace lokimq
