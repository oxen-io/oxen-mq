#pragma once

#include <string_view>

namespace lokimq {

// Deprecated type alias for std::string_view
using string_view = std::string_view;

// Deprecated "foo"_sv literal; you should use "foo"sv (from <string_view>) instead.
inline namespace literals {
    inline constexpr std::string_view operator""_sv(const char* str, size_t len) { return {str, len}; }
}

}
