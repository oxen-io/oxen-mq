#include "address.h"
#include <tuple>
#include <limits>
#include <cstddef>
#include <utility>
#include <stdexcept>
#include <ostream>
#include "hex.h"
#include "base32z.h"
#include "base64.h"

namespace lokimq {

constexpr size_t enc_length(address::encoding enc) {
    return enc == address::encoding::hex ? 64 :
        enc == address::encoding::base64 ? 43 : // this can be 44 with a padding byte, but we don't need it
        52 /*base32z*/;
};

// Parses an encoding pubkey from the given string_view.  Advanced the string_view beyond the
// consumed pubkey data, and returns the pubkey (as a 32-byte string).  Throws if no valid pubkey
// was found at the beginning of addr.  We look for hex, base32z, or base64 pubkeys *unless* qr is
// given: for QR-friendly we only accept hex or base32z (since QR cannot handle base64's alphabet).
std::string decode_pubkey(std::string_view& in, bool qr) {
    std::string pubkey;
    if (in.size() >= 64 && lokimq::is_hex(in.substr(0, 64))) {
        pubkey = from_hex(in.substr(0, 64));
        in.remove_prefix(64);
    } else if (in.size() >= 52 && lokimq::is_base32z(in.substr(0, 52))) {
        pubkey = from_base32z(in.substr(0, 52));
        in.remove_prefix(52);
    } else if (!qr && in.size() >= 43 && lokimq::is_base64(in.substr(0, 43))) {
        pubkey = from_base64(in.substr(0, 43));
        in.remove_prefix(43);
        if (!in.empty() && in.front() == '=')
            in.remove_prefix(1); // allow (and eat) a padding byte at the end
    } else {
        throw std::invalid_argument{"No pubkey found"};
    }
    return pubkey;
}

// Parse the host, port, and optionally pubkey from a string view, mutating it to remove the parsed
// sections.  qr should be true if we should accept $IPv6$ as a QR-encoding-friendly alternative to
// [IPv6] (the returned host will have the $ replaced, i.e. [IPv6]).
std::tuple<std::string, uint16_t, std::string> parse_tcp(std::string_view& addr, bool qr, bool expect_pubkey) {
    std::tuple<std::string, uint16_t, std::string> result;
    auto& host = std::get<0>(result);
    if (addr.front() == '[' || (qr && addr.front() == '$')) { // IPv6 addr (though this is far from complete validation)
        auto pos = addr.find_first_not_of(":.1234567890abcdefABCDEF", 1);
        if (pos == std::string_view::npos)
            throw std::invalid_argument("Could not find terminating ] while parsing an IPv6 address");
        if (!(addr[pos] == ']' || (qr && addr[pos] == '$')))
            throw std::invalid_argument{"Expected " + (qr ? "$"s : "]"s) + " to close IPv6 address but found " + std::string(1, addr[pos])};
        host = std::string{addr.substr(0, pos+1)};
        if (qr) {
            if (host.front() == '$')
                host.front() = '[';
            if (host.back() == '$')
                host.back() = ']';
        }
        addr.remove_prefix(pos+1);
    } else {
        auto port_pos = addr.find(':');
        if (port_pos == std::string_view::npos)
            throw std::invalid_argument{"Could not determine host (no following ':port' found)"};
        if (port_pos == 0)
            throw std::invalid_argument{"Host cannot be empty"};
        host = std::string{addr.substr(0, port_pos)};
        addr.remove_prefix(port_pos);
    }

    if (qr)
        // Lower-case the host because upper case hostnames are ugly
        for (char& c : host)
            if (c >= 'A' && c <= 'Z')
                c = c - 'A' + 'a';

    if (addr.size() < 2 || addr[0] != ':')
        throw std::invalid_argument{"Could not find :port in address string"};
    addr.remove_prefix(1);
    auto pos = addr.find_first_not_of("1234567890");
    if (pos == 0)
        throw std::invalid_argument{"Could not find numeric port in address string"};
    if (pos == std::string_view::npos)
        pos = addr.size();
    size_t processed;
    int port_int = std::stoi(std::string{addr.substr(0, pos)}, &processed);
    if (port_int == 0 || processed != pos)
        throw std::invalid_argument{"Could not parse numeric port in address string"};
    if (port_int < 0 || port_int > std::numeric_limits<uint16_t>::max())
        throw std::invalid_argument{"Invalid port: port must be in range 1-65535"};
    std::get<1>(result) = static_cast<uint16_t>(port_int);
    addr.remove_prefix(pos);

    if (expect_pubkey) {
        if (addr.size() < 1 + enc_length(qr ? address::encoding::base32z : address::encoding::base64)
                || addr.front() != '/')
            throw std::invalid_argument{"Invalid address: expected /PUBKEY after port"};
        addr.remove_prefix(1);

        std::get<2>(result) = decode_pubkey(addr, qr);
        if (!addr.empty())
            throw std::invalid_argument{"Invalid address: found unexpected trailing data after pubkey"};
    } else if (!addr.empty()) {
        throw std::invalid_argument{"Invalid address: found unexpected trailing data after port"};
    }

    return result;
}

// Parse the socket path and (possibly) pubkey, mutating it to remove the parsed sections.
// Currently the /pubkey *must* be at the end of the string, but this might not always be the case
// (e.g. we could in the future support query string-like arguments).
std::pair<std::string, std::string> parse_unix(std::string_view& addr, bool expect_pubkey) {
    std::pair<std::string, std::string> result;
    if (expect_pubkey) {
        size_t b64_len = addr.size() > 0 && addr.back() == '=' ? 44 : 43;
        if (addr.size() > 64 && addr[addr.size() - 65] == '/' && is_hex(addr.substr(addr.size() - 64))) {
            result.first = std::string{addr.substr(0, addr.size() - 65)};
            result.second = from_hex(addr.substr(addr.size() - 64));
        } else if (addr.size() > 52 && addr[addr.size() - 53] == '/' && is_base32z(addr.substr(addr.size() - 52))) {
            result.first = std::string{addr.substr(0, addr.size() - 53)};
            result.second = from_base32z(addr.substr(addr.size() - 52));
        } else if (addr.size() > b64_len && addr[addr.size() - b64_len - 1] == '/' && is_base64(addr.substr(addr.size() - b64_len))) {
            result.first = std::string{addr.substr(0, addr.size() - b64_len - 1)};
            result.second = from_base64(addr.substr(addr.size() - b64_len));
        } else {
            throw std::invalid_argument{"icp+curve:// requires a trailing /PUBKEY value, got: " + std::string{addr}};
        }
    } else {
        // Anything goes
        result.first = std::string{addr};
    }

    // Any path above consumes everything:
    addr.remove_prefix(addr.size());

    return result;
}

address::address(std::string_view addr) {
    auto protoend = addr.find("://"sv);
    if (protoend == std::string_view::npos || protoend == 0)
        throw std::invalid_argument("Invalid address: no protocol found");
    auto pro = addr.substr(0, protoend);
    addr.remove_prefix(protoend + 3);
    if (addr.empty())
        throw std::invalid_argument("Invalid address: no value specified after protocol");
    bool qr = false;
    if (pro == "tcp") protocol = proto::tcp;
    else if (pro == "tcp+curve" || pro == "curve") protocol = proto::tcp_curve;
    else if (pro == "ipc") protocol = proto::ipc;
    else if (pro == "ipc+curve") protocol = proto::ipc_curve;
    else if (pro == "TCP") {
        protocol = proto::tcp;
        qr = true;
    } else if (pro == "CURVE") {
        protocol = proto::tcp_curve;
        qr = true;
    } else {
        throw std::invalid_argument("Invalid protocol '" + std::string{pro} + "'");
    }

    if (qr) {
        // The QR variations only allow QR-alphanumeric characters (upper-case letters, numbers, and
        // a few symbols):
        for (char c : addr) {
            // QR alphanumeric also allows space, %, *, +, but we don't need or allow any of those here.
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '$' || c == ':' || c == '/' || c == '.' || c == '-'))
                throw std::invalid_argument("Found non-QR-alphanumeric value in QR TCP:// or CURVE:// address");
        }
    }

    if (tcp())
        std::tie(host, port, pubkey) = parse_tcp(addr, qr, curve());
    else
        std::tie(socket, pubkey) = parse_unix(addr, curve());

    if (!addr.empty())
        throw std::invalid_argument{"Invalid trailing garbage '" + std::string{addr} + "' in address"};
}

address& address::set_pubkey(std::string_view pk) {
    if (pk.size() == 0) {
        if (protocol == proto::tcp_curve) protocol = proto::tcp;
        else if (protocol == proto::ipc_curve) protocol = proto::ipc;
    } else if (pk.size() == 32) {
        if (protocol == proto::tcp) protocol = proto::tcp_curve;
        else if (protocol == proto::ipc) protocol = proto::ipc_curve;
    } else {
        throw std::invalid_argument{"Invalid pubkey passed to set_pubkey(): require 0- or 32-byte pubkey"};
    }
    pubkey = pk;
    return *this;
}

std::string address::encode_pubkey(encoding enc) const {
    std::string pk;
    if (enc == encoding::hex)
        pk = to_hex(pubkey);
    else if (enc == encoding::base32z)
        pk = to_base32z(pubkey);
    else if (enc == encoding::BASE32Z) {
        pk = to_base32z(pubkey);
        for (char& c : pk)
            if (c >= 'a' && c <= 'z')
                c = c - 'a' + 'A';
    } else if (enc == encoding::base64) {
        pk = to_base64(pubkey);
        if (pk.size() == 44 && pk.back() == '=')
            pk.resize(43);
    } else {
        throw std::logic_error{"Invalid encoding"};
    }
    return pk;
}

std::string address::full_address(encoding enc) const {
    std::string result;
    std::string pk;
    if (curve())
        pk = encode_pubkey(enc);

    if (protocol == proto::tcp) {
        result.reserve(6 /*tcp:// */ + host.size() + 6 /*:port*/);
        result += "tcp://";
        result += host;
        result += ':';
        result += std::to_string(port);
    } else if (protocol == proto::tcp_curve) {
        result.reserve(8 /*curve:// */ + host.size() + 6 /*:port*/ + 1 /* / */ + pk.size());
        result += "curve://";
        result += host;
        result += ':';
        result += std::to_string(port);
        result += '/';
        result += pk;
    } else if (protocol == proto::ipc) {
        result.reserve(6 /*ipc:// */ + socket.size());
        result += "ipc://";
        result += socket;
    } else if (protocol == proto::ipc_curve) {
        result.reserve(12 /*ipc+curve:// */ + socket.size() + 1 /* / */ + pk.size());
        result += "ipc+curve://";
        result += socket;
        result += '/';
        result += pk;
    } else {
        throw std::logic_error{"Invalid protocol"};
    }

    return result;
}

std::string address::zmq_address() const {
    std::string result;
    if (tcp()) {
        result.reserve(6 /*tcp:// */ + host.size() + 6 /*:port*/);
        result += "tcp://";
        result += host;
        result += ':';
        result += std::to_string(port);
    } else {
        result.reserve(6 /*ipc:// */ + socket.size());
        result += "ipc://";
        result += socket;
    }
    return result;
}

std::string address::qr_address() const {
    if (protocol != proto::tcp && protocol != proto::tcp_curve)
        throw std::logic_error("Cannot construct a QR-friendly address for a non-TCP address");
    if (host.empty())
        throw std::logic_error("Cannot construct a QR-friendly address with an empty TCP host");
    std::string result;
    result.reserve((curve() ? 8 /*CURVE:// */ : 6 /*TCP:// */) + host.size() + 6 /*:port*/ +
            (curve() ? 1 + enc_length(encoding::BASE32Z) : 0));
    result += curve() ? "CURVE://" : "TCP://";
    std::string uc_host = host;
    for (auto& c : uc_host)
        if (c >= 'a' && c <= 'z')
            c = c - 'a' + 'A';

    if (uc_host.front() == '[' && uc_host.back() == ']') {
        uc_host.front() = '$';
        uc_host.back() = '$';
    }
    result += uc_host;
    result += ':';
    result += std::to_string(port);

    if (curve()) {
        result += '/';
        result += encode_pubkey(encoding::BASE32Z);
    }

    return result;
}

bool address::operator==(const address& other) const {
    if (protocol != other.protocol)
        return false;
    if (tcp())
        if (host != other.host || port != other.port)
            return false;
    if (ipc())
        if (socket != other.socket)
            return false;
    if (curve())
        if (pubkey != other.pubkey)
            return false;
    return true;
}

address address::tcp(std::string host, uint16_t port) {
    address a;
    a.protocol = proto::tcp;
    a.host = std::move(host);
    a.port = port;
    return a;
}

address address::tcp_curve(std::string host, uint16_t port, std::string pubkey) {
    address a;
    a.protocol = proto::tcp_curve;
    a.host = std::move(host);
    a.port = port;
    a.pubkey = std::move(pubkey);
    return a;
}

address address::ipc(std::string path) {
    address a;
    a.protocol = proto::ipc;
    a.socket = std::move(path);
    return a;
}

address address::ipc_curve(std::string path, std::string pubkey) {
    address a;
    a.protocol = proto::ipc_curve;
    a.socket = std::move(path);
    a.pubkey = std::move(pubkey);
    return a;
}

std::ostream& operator<<(std::ostream& o, const address& a) { return o << a.full_address(); }

}
