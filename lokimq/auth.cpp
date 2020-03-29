#include "lokimq.h"
#include "hex.h"
#include "lokimq-internal.h"

namespace lokimq {

std::ostream& operator<<(std::ostream& o, AuthLevel a) {
    return o << to_string(a);
}

namespace {

// Builds a ZMTP metadata key-value pair.  These will be available on every message from that peer.
// Keys must start with X- and be <= 255 characters.
std::string zmtp_metadata(string_view key, string_view value) {
    assert(key.size() > 2 && key.size() <= 255 && key[0] == 'X' && key[1] == '-');

    std::string result;
    result.reserve(1 + key.size() + 4 + value.size());
    result += static_cast<char>(key.size()); // Size octet of key
    result.append(&key[0], key.size()); // key data
    for (int i = 24; i >= 0; i -= 8) // 4-byte size of value in network order
        result += static_cast<char>((value.size() >> i) & 0xff);
    result.append(&value[0], value.size()); // value data

    return result;
}

}


bool LokiMQ::proxy_check_auth(size_t conn_index, bool outgoing, const peer_info& peer,
        const std::string& command, const category& cat, zmq::message_t& msg) {
    std::string reply;
    if (peer.auth_level < cat.access.auth) {
        LMQ_LOG(warn, "Access denied to ", command, " for peer [", to_hex(peer.pubkey), "]/", peer_address(msg),
                ": peer auth level ", peer.auth_level, " < ", cat.access.auth);
        reply = "FORBIDDEN";
    }
    else if (cat.access.local_sn && !local_service_node) {
        LMQ_LOG(warn, "Access denied to ", command, " for peer [", to_hex(peer.pubkey), "]/", peer_address(msg),
                ": that command is only available when this LokiMQ is running in service node mode");
        reply = "NOT_A_SERVICE_NODE";
    }
    else if (cat.access.remote_sn && !peer.service_node) {
        LMQ_LOG(warn, "Access denied to ", command, " for peer [", to_hex(peer.pubkey), "]/", peer_address(msg),
                ": remote is not recognized as a service node");
        // Disconnect: we don't think the remote is a SN, but it issued a command only SNs should be
        // issuing.  Drop the connection; if the remote has something important to relay it will
        // reconnect, at which point we will reassess the SN status on the new incoming connection.
        if (outgoing) {
            proxy_disconnect(peer.service_node ? ConnectionID{peer.pubkey} : conn_index_to_id[conn_index], 1s);
            return false;
        }
        else
            reply = "BYE";
    }

    if (reply.empty())
        return true;

    try {
        if (outgoing)
            send_direct_message(connections[conn_index], std::move(reply), command);
        else
            send_routed_message(connections[conn_index], peer.route, std::move(reply), command);
    } catch (const zmq::error_t&) { /* can't send: possibly already disconnected.  Ignore. */ }

    return false;
}

void LokiMQ::process_zap_requests() {
    for (std::vector<zmq::message_t> frames; recv_message_parts(zap_auth, frames, zmq::recv_flags::dontwait); frames.clear()) {
#ifndef NDEBUG
        if (log_level() >= LogLevel::trace) {
            std::ostringstream o;
            o << "Processing ZAP authentication request:";
            for (size_t i = 0; i < frames.size(); i++) {
                o << "\n[" << i << "]: ";
                auto v = view(frames[i]);
                if (i == 1 || i == 6)
                    o << to_hex(v);
                else
                    o << v;
            }
            log_(LogLevel::trace, __FILE__, __LINE__, o.str());
        } else
#endif
            LMQ_LOG(debug, "Processing ZAP authentication request");

        // https://rfc.zeromq.org/spec:27/ZAP/
        //
        // The request message SHALL consist of the following message frames:
        //
        //     The version frame, which SHALL contain the three octets "1.0".
        //     The request id, which MAY contain an opaque binary blob.
        //     The domain, which SHALL contain a (non-empty) string.
        //     The address, the origin network IP address.
        //     The identity, the connection Identity, if any.
        //     The mechanism, which SHALL contain a string.
        //     The credentials, which SHALL be zero or more opaque frames.
        //
        // The reply message SHALL consist of the following message frames:
        //
        //     The version frame, which SHALL contain the three octets "1.0".
        //     The request id, which MAY contain an opaque binary blob.
        //     The status code, which SHALL contain a string.
        //     The status text, which MAY contain a string.
        //     The user id, which SHALL contain a string.
        //     The metadata, which MAY contain a blob.
        //
        // (NB: there are also null address delimiters at the beginning of each mentioned in the
        // RFC, but those have already been removed through the use of a REP socket)

        std::vector<std::string> response_vals(6);
        response_vals[0] = "1.0"; // version
        if (frames.size() >= 2)
            response_vals[1] = std::string{view(frames[1])}; // unique identifier
        std::string &status_code = response_vals[2], &status_text = response_vals[3];

        if (frames.size() < 6 || view(frames[0]) != "1.0") {
            LMQ_LOG(error, "Bad ZAP authentication request: version != 1.0 or invalid ZAP message parts");
            status_code = "500";
            status_text = "Internal error: invalid auth request";
        } else {
            auto auth_domain = view(frames[2]);
            size_t bind_id = (size_t) -1;
            try {
                bind_id = bt_deserialize<size_t>(view(frames[2]));
            } catch (...) {}

            if (bind_id >= bind.size()) {
                LMQ_LOG(error, "Bad ZAP authentication request: invalid auth domain '", auth_domain, "'");
                status_code = "400";
                status_text = "Unknown authentication domain: " + std::string{auth_domain};
            } else if (bind[bind_id].second.curve
                    ? !(frames.size() == 7 && view(frames[5]) == "CURVE")
                    : !(frames.size() == 6 && view(frames[5]) == "NULL")) {
                LMQ_LOG(error, "Bad ZAP authentication request: invalid ",
                        bind[bind_id].second.curve ? "CURVE" : "NULL", " authentication request");
                status_code = "500";
                status_text = "Invalid authentication request mechanism";
            } else if (bind[bind_id].second.curve && frames[6].size() != 32) {
                LMQ_LOG(error, "Bad ZAP authentication request: invalid request pubkey");
                status_code = "500";
                status_text = "Invalid public key size for CURVE authentication";
            } else {
                auto ip = view(frames[3]);
                string_view pubkey;
                if (bind[bind_id].second.curve)
                    pubkey = view(frames[6]);
                auto result = bind[bind_id].second.allow(ip, pubkey);
                bool sn = result.remote_sn;
                auto& user_id = response_vals[4];
                if (bind[bind_id].second.curve) {
                    user_id.reserve(64);
                    to_hex(pubkey.begin(), pubkey.end(), std::back_inserter(user_id));
                }

                if (result.auth <= AuthLevel::denied || result.auth > AuthLevel::admin) {
                    LMQ_LOG(info, "Access denied for incoming ", view(frames[5]), (sn ? " service node" : " client"),
                            " connection from ", !user_id.empty() ? user_id + " at " : ""s, ip,
                            " with initial auth level ", result.auth);
                    status_code = "400";
                    status_text = "Access denied";
                    user_id.clear();
                } else {
                    LMQ_LOG(debug, "Accepted incoming ", view(frames[5]), (sn ? " service node" : " client"),
                            " connection with authentication level ", result.auth,
                            " from ", !user_id.empty() ? user_id + " at " : ""s, ip);

                    auto& metadata = response_vals[5];
                    metadata += zmtp_metadata("X-SN", result.remote_sn ? "1" : "0");
                    metadata += zmtp_metadata("X-AuthLevel", to_string(result.auth));

                    status_code = "200";
                    status_text = "";
                }
            }
        }

        LMQ_TRACE("ZAP request result: ", status_code, " ", status_text);

        std::vector<zmq::message_t> response;
        response.reserve(response_vals.size());
        for (auto &r : response_vals) response.push_back(create_message(std::move(r)));
        send_message_parts(zap_auth, response.begin(), response.end());
    }
}

}
