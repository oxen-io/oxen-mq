#pragma once

#include <fmt/format.h>

#include "address.h"
#include "auth.h"
#include "connections.h"

template <>
struct fmt::formatter<oxenmq::AuthLevel> : fmt::formatter<std::string_view> {
    auto format(oxenmq::AuthLevel v, format_context& ctx) const {
        return formatter<std::string_view>::format(to_string(v), ctx);
    }
};
template <>
struct fmt::formatter<oxenmq::ConnectionID> : fmt::formatter<std::string> {
    auto format(const oxenmq::ConnectionID& conn, format_context& ctx) const {
        return formatter<std::string>::format(conn.to_string(), ctx);
    }
};
template <>
struct fmt::formatter<oxenmq::address> : fmt::formatter<std::string> {
    auto format(const oxenmq::address& addr, format_context& ctx) const {
        return formatter<std::string>::format(addr.full_address(), ctx);
    }
};
