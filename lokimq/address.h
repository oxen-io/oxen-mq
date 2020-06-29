// Copyright (c)      2020, The Loki Project
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
#include <cstdint>
#include <iosfwd>

namespace lokimq {

using namespace std::literals;

/** LokiMQ address abstraction class.  This class uses and extends standard ZMQ addresses allowing
 * extra parameters to be passed in in a relative standard way.
 *
 * External ZMQ addresses generally have two forms that we are concerned with: one for TCP and one
 * for Unix sockets:
 *
 *     tcp://HOST:PORT -- HOST can be a hostname, IPv4 address, or IPv6 address in [...]
 *     ipc://PATH      -- PATH can be absolute (ipc:///path/to/some.sock) or relative (ipc://some.sock)
 *
 * but this doesn't carry enough info: in particular, we can connect with two very different
 * protocols: curve25519-encrypted, or plaintext, but for curve25519-encrypted we require the
 * remote's public key as well to verify the connection.
 *
 * This class, then, handles this by allowing addresses of:
 *
 * Standard ZMQ address: these carry no pubkey and so the connection will be unencrypted:
 *
 *     tcp://HOSTNAME:PORT
 *     ipc://PATH
 *
 * Non-ZMQ address formats that specify that the connection shall be x25519 encrypted:
 *
 *     curve://HOSTNAME:PORT/PUBKEY -- PUBKEY must be specified in hex (64 characters), base32z (52)
 *                                     or base64 (43 or 44 with one '=' trailing padding)
 *     ipc+curve:///path/to/my.sock/PUBKEY -- same requirements on PUBKEY as above.
 *     tcp+curve://(whatever) -- alias for curve://(whatever)
 *
 * We also accept special upper-case TCP-only variants which *only* accept uppercase characters and
 * a few required symbols (:, /, $, ., and -) in the string:
 *
 *     TCP://HOSTNAME:PORT
 *     CURVE://HOSTNAME:PORT/B32ZPUBKEY
 *
 * These versions are explicitly meant to be used with QR codes; the upper-case-only requirement
 * allows a smaller QR code by allowing QR's alphanumeric mode (which allows only [A-Z0-9 $%*+./:-])
 * to be used.  Such a QR-friendly address can be created from the qr_address() method.  To support
 * literal IPv6 addresses we surround the address with $...$ instead of the usual [...].
 *
 * Note that this class does very little validate the host argument at all, and no socket path
 * validation whatsoever.  The only constraint on host is when parsing an encoded address: we check
 * that it contains no : at all, or must be a [bracketed] expression that contains only hex
 * characters, :'s, or .'s.  Otherwise, if you pass broken crap into the hostname, expect broken
 * crap out.
 */
struct address {
    /// Supported address protocols: TCP connections (tcp), or unix sockets (ipc).
    enum class proto {
        tcp,
        tcp_curve,
        ipc,
        ipc_curve
    };
    /// Supported public key encodings (used when regenerating an augmented address).
    enum class encoding {
        hex, ///< hexadecimal encoded
        base32z, ///< base32z encoded
        base64, ///< base64 encoded (*without* trailing = padding)
        BASE32Z ///< upper-case base32z encoding, meant for QR encoding
    };

    /// The protocol: one of the `protocol` enum values for tcp or ipc (unix sockets), with or
    /// without _curve encryption.
    proto protocol = proto::tcp;
    /// The host for tcp connections; can be a hostname or IP address.  If this is an IPv6 it must be surrounded with [ ].
    std::string host;
    /// The port (for tcp connections)
    uint16_t port = 0;
    /// The socket path (for unix socket connections)
    std::string socket;
    /// If a curve connection, this is the required remote public key (in bytes)
    std::string pubkey;

    /// Default constructor; this gives you an unusable address.
    address() = default;

    /**
     * Constructs an address by parsing a string_view containing one of the formats listed in the
     * class description.  This is intentionally implicitly constructible so that you can pass a
     * string_view into anything expecting an `address`.
     *
     * Throw std::invalid_argument if the given address is not parseable.
     */
    address(std::string_view addr);

    /** Constructs an address from a remote string and a separate pubkey.  Typically `remote` is a
     * basic ZMQ connect string, though this is not enforced.  Any pubkey information embedded in
     * the remote string will be discarded and replaced with the given pubkey string.  The result
     * will be curve encrypted if `pubkey` is non-empty, plaintext if `pubkey` is empty.
     *
     * Throws an exception if either addr or pubkey is invalid.
     *
     * Exactly equivalent to `address a{remote}; a.set_pubkey(pubkey);`
     */
    address(std::string_view addr, std::string_view pubkey) : address(addr) { set_pubkey(pubkey); }

    /// Replaces the address's pubkey (if any) with the given pubkey (or no pubkey if empty).  If
    /// changing from pubkey to no-pubkey or no-pubkey to pubkey then the protocol is update to
    /// switch to or from curve encryption.
    ///
    /// pubkey should be the 32-byte binary pubkey, or an empty string to remove an existing pubkey.
    ///
    /// Returns the object itself, so that you can chain it.
    address& set_pubkey(std::string_view pubkey);

    /// Constructs and builds the ZMQ connection address from the stored connection details.  This
    /// does not contain any of the curve-related details; those must be specified separately when
    /// interfacing with ZMQ.
    std::string zmq_address() const;

    /// Returns true if the connection was specified as a curve-encryption-enabled connection, false
    /// otherwise.
    bool curve() const { return protocol == proto::tcp_curve || protocol == proto::ipc_curve; }

    /// True if the protocol is TCP (either with or without curve)
    bool tcp() const { return protocol == proto::tcp || protocol == proto::tcp_curve; }

    /// True if the protocol is unix socket (either with or without curve)
    bool ipc() const { return !tcp(); }

    /// Returns the full "augmented" address string (i.e. that could be passed in to the
    /// constructor).  This will be equivalent (but not necessarily identical) to an augmented
    /// string passed into the constructor.  Takes an optional encoding format for the pubkey (if
    /// any), which defaults to base32z.
    std::string full_address(encoding enc = encoding::base32z) const;

    /// Returns a QR-code friendly address string.  This returns an all-uppercase version of the
    /// address with "TCP://" or "CURVE://" for the protocol string, and uses upper-case base32z
    /// encoding for the pubkey (for curve addresses).  For literal IPv6 addresses we replace the
    /// surround the
    /// address with $ instead of $
    ///
    /// \throws std::logic_error if called on a unix socket address.
    std::string qr_address() const;

    /// Returns `.pubkey` but encoded in the given format
    std::string encode_pubkey(encoding enc) const;

    /// Returns true if two addresses are identical (i.e. same protocol and relevant protocol
    /// arguments).
    ///
    /// Note that it is possible for addresses to connect to the same socket without being
    /// identical: for example, using "foo.sock" and "./foo.sock", or writing IPv6 addresses (or
    /// even IPv4 addresses) in slightly different ways).  Such equivalent but non-equal values will
    /// result in a false return here.
    ///
    /// Note also that we ignore irrelevant arguments: for example, we don't care whether pubkeys
    /// match when comparing two non-curve TCP addresses.
    bool operator==(const address& other) const;
    /// Negation of ==
    bool operator!=(const address& other) const { return !operator==(other); }

    /// Factory function that constructs a TCP address from a host and port.  The connection will be
    /// plaintext.  If the host is an IPv6 address it *must* be surrounded with [ and ].
    static address tcp(std::string host, uint16_t port);

    /// Factory function that constructs a curve-encrypted TCP address from a host, port, and remote
    /// pubkey.  The pubkey must be 32 bytes.  As above, IPv6 addresses must be specified as [addr].
    static address tcp_curve(std::string host, uint16_t, std::string pubkey);

    /// Factory function that constructs a unix socket address from a path.  The connection will be
    /// plaintext (which is usually fine for a socket since unix sockets are local machine).
    static address ipc(std::string path);

    /// Factory function that constructs a unix socket address from a path and remote pubkey.  The
    /// connection will be curve25519 encrypted; the remote pubkey must be 32 bytes.
    static address ipc_curve(std::string path, std::string pubkey);
};

// Outputs address.full_address() when sent to an ostream.
std::ostream& operator<<(std::ostream& o, const address& a);

}
