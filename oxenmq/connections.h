#pragma once
#include "auth.h"
#include "bt_value.h"
#include <string_view>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace oxenmq {

struct ConnectionID;

namespace detail {
template <typename... T>
bt_dict build_send(ConnectionID to, std::string_view cmd, T&&... opts);
}

/// Opaque data structure representing a connection which supports ==, !=, < and std::hash.  For
/// connections to service node this is the service node pubkey (and you can pass a 32-byte string
/// anywhere a ConnectionID is called for).  For non-SN remote connections you need to keep a copy
/// of the ConnectionID returned by connect_remote().
struct ConnectionID {
    // Default construction; creates a ConnectionID with an invalid internal ID that will not match
    // an actual connection.
    ConnectionID() : ConnectionID(0) {}
    // Construction from a service node pubkey
    ConnectionID(std::string pubkey_) : id{SN_ID}, pk{std::move(pubkey_)} {
        if (pk.size() != 32)
            throw std::runtime_error{"Invalid pubkey: expected 32 bytes"};
    }
    // Construction from a service node pubkey
    ConnectionID(std::string_view pubkey_) : ConnectionID(std::string{pubkey_}) {}

    ConnectionID(const ConnectionID&) = default;
    ConnectionID(ConnectionID&&) = default;
    ConnectionID& operator=(const ConnectionID&) = default;
    ConnectionID& operator=(ConnectionID&&) = default;

    // Returns true if this is a ConnectionID (false for a default-constructed, invalid id)
    explicit operator bool() const {
        return id != 0;
    }

    // Two ConnectionIDs are equal if they are both SNs and have matching pubkeys, or they are both
    // not SNs and have matching internal IDs and routes.  (Pubkeys do not have to match for
    // non-SNs).
    bool operator==(const ConnectionID &o) const {
        if (sn() && o.sn())
            return pk == o.pk;
        return id == o.id && route == o.route;
    }
    bool operator!=(const ConnectionID &o) const { return !(*this == o); }
    bool operator<(const ConnectionID &o) const {
        if (sn() && o.sn())
            return pk < o.pk;
        return id < o.id || (id == o.id && route < o.route);
    }

    // Returns true if this ConnectionID represents a SN connection
    bool sn() const { return id == SN_ID; }

    // Returns this connection's pubkey, if any.  (Note that all curve connections have pubkeys, not
    // only SNs).
    const std::string& pubkey() const { return pk; }

    // Returns a copy of the ConnectionID with the route set to empty.
    ConnectionID unrouted() { return ConnectionID{id, pk, ""}; }

private:
    ConnectionID(int64_t id) : id{id} {}
    ConnectionID(int64_t id, std::string pubkey, std::string route = "")
        : id{id}, pk{std::move(pubkey)}, route{std::move(route)} {}

    constexpr static int64_t SN_ID = -1;
    int64_t id = 0;
    std::string pk;
    std::string route;
    friend class OxenMQ;
    friend struct std::hash<ConnectionID>;
    template <typename... T>
    friend bt_dict detail::build_send(ConnectionID to, std::string_view cmd, T&&... opts);
    friend std::ostream& operator<<(std::ostream& o, const ConnectionID& conn);
};

} // namespace oxenmq
namespace std {
    template <> struct hash<oxenmq::ConnectionID> {
        size_t operator()(const oxenmq::ConnectionID &c) const {
            return c.sn() ? oxenmq::already_hashed{}(c.pk) :
                std::hash<int64_t>{}(c.id) + std::hash<std::string>{}(c.route);
        }
    };
} // namespace std

