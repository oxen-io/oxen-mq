#pragma once
#include <iostream>

namespace lokimq {

/// Authentication levels for command categories and connections
enum class AuthLevel {
    denied, ///< Not actually an auth level, but can be returned by the AllowFunc to deny an incoming connection.
    none, ///< No authentication at all; any random incoming ZMQ connection can invoke this command.
    basic, ///< Basic authentication commands require a login, or a node that is specifically configured to be a public node (e.g. for public RPC).
    admin, ///< Advanced authentication commands require an admin user, either via explicit login or by implicit login from localhost.  This typically protects administrative commands like shutting down, starting mining, or access sensitive data.
};

std::ostream& operator<<(std::ostream& os, AuthLevel a);

/// The access level for a command category
struct Access {
    /// Minimum access level required
    AuthLevel auth = AuthLevel::none;
    /// If true only remote SNs may call the category commands
    bool remote_sn = false;
    /// If true the category requires that the local node is a SN
    bool local_sn = false;
};

/// Return type of the AllowFunc: this determines whether we allow the connection at all, and if so,
/// sets the initial authentication level and tells LokiMQ whether the other end is an active SN.
struct Allow {
    AuthLevel auth = AuthLevel::none;
    bool remote_sn = false;
};

}
