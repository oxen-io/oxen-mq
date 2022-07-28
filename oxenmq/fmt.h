#pragma once

#include <fmt/format.h>
#include "connections.h"
#include "auth.h"
#include "address.h"
#include "oxenmq-internal.h"

template <>
struct fmt::formatter<oxenmq::AuthLevel> : fmt::formatter<std::string> {
  auto format(oxenmq::AuthLevel v, format_context& ctx) {
    return formatter<std::string>::format(
        fmt::format("{}", to_string(v)), ctx);
  }
};
template <>
struct fmt::formatter<oxenmq::ConnectionID> : fmt::formatter<std::string> {
  auto format(oxenmq::ConnectionID conn, format_context& ctx) {
    return formatter<std::string>::format(
        fmt::format("{}", conn.to_string()), ctx);
  }
};
template <>
struct fmt::formatter<oxenmq::address> : fmt::formatter<std::string> {
  auto format(oxenmq::address addr, format_context& ctx) {
    return formatter<std::string>::format(
        fmt::format("{}", addr.full_address()), ctx);
  }
};
