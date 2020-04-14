#pragma once
#include "auth.h"
#include "string_view.h"

namespace lokimq {

class bt_dict;
struct ConnectionID;

namespace detail {
template <typename... T>
bt_dict build_send(ConnectionID to, string_view cmd, T&&... opts);
}

/// Opaque data structure representing a connection which supports ==, !=, < and std::hash.  For
/// connections to service node this is the service node pubkey (and you can pass a 32-byte string
/// anywhere a ConnectionID is called for).  For non-SN remote connections you need to keep a copy
/// of the ConnectionID returned by connect_remote().
struct ConnectionID {
    ConnectionID(std::string pubkey_) : id{SN_ID}, pk{std::move(pubkey_)} {
        if (pk.size() != 32)
            throw std::runtime_error{"Invalid pubkey: expected 32 bytes"};
    }
    ConnectionID(string_view pubkey_) : ConnectionID(std::string{pubkey_}) {}
    ConnectionID(const ConnectionID&) = default;
    ConnectionID(ConnectionID&&) = default;
    ConnectionID& operator=(const ConnectionID&) = default;
    ConnectionID& operator=(ConnectionID&&) = default;

    // Returns true if this is a ConnectionID (false for a default-constructed, invalid id)
    explicit operator bool() const {
        return id != 0;
    }

    // Two ConnectionIDs are equal if they are both SNs and have matching pubkeys, or they are both
    // not SNs and have matching internal IDs.  (Pubkeys do not have to match for non-SNs, and
    // routes are not considered for equality at all).
    bool operator==(const ConnectionID &o) const {
        if (id == SN_ID && o.id == SN_ID)
            return pk == o.pk;
        return id == o.id;
    }
    bool operator!=(const ConnectionID &o) const { return !(*this == o); }
    bool operator<(const ConnectionID &o) const {
        if (id == SN_ID && o.id == SN_ID)
            return pk < o.pk;
        return id < o.id;
    }
    // Returns true if this ConnectionID represents a SN connection
    bool sn() const { return id == SN_ID; }

    // Returns this connection's pubkey, if any.  (Note that it is possible to have a pubkey and not
    // be a SN when connecting to secure remotes: having a non-empty pubkey does not imply that
    // `sn()` is true).
    const std::string& pubkey() const { return pk; }
    // Default construction; creates a ConnectionID with an invalid internal ID that will not match
    // an actual connection.
    ConnectionID() : ConnectionID(0) {}
private:
    ConnectionID(long long id) : id{id} {}
    ConnectionID(long long id, std::string pubkey, std::string route = "")
        : id{id}, pk{std::move(pubkey)}, route{std::move(route)} {}

    constexpr static long long SN_ID = -1;
    long long id = 0;
    std::string pk;
    std::string route;
    friend class LokiMQ;
    friend struct std::hash<ConnectionID>;
    template <typename... T>
    friend bt_dict detail::build_send(ConnectionID to, string_view cmd, T&&... opts);
    friend std::ostream& operator<<(std::ostream& o, const ConnectionID& conn);
};

} // namespace lokimq
namespace std {
    template <> struct hash<lokimq::ConnectionID> {
        size_t operator()(const lokimq::ConnectionID &c) const {
            return c.sn() ? lokimq::already_hashed{}(c.pk) :
                std::hash<long long>{}(c.id);
        }
    };
} // namespace std

