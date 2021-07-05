#include "oxenmq.h"
#include "hex.h"
#include "oxenmq-internal.h"
#include <ostream>
#include <sstream>

namespace oxenmq {

std::ostream& operator<<(std::ostream& o, AuthLevel a) {
    return o << to_string(a);
}

namespace {

// Builds a ZMTP metadata key-value pair.  These will be available on every message from that peer.
// Keys must start with X- and be <= 255 characters.
std::string zmtp_metadata(std::string_view key, std::string_view value) {
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


bool OxenMQ::proxy_check_auth(int64_t conn_id, bool outgoing, const peer_info& peer,
        zmq::message_t& cmd, const cat_call_t& cat_call, std::vector<zmq::message_t>& data) {
    auto command = view(cmd);
    std::string reply;

    if (!cat_call.first) {
        LMQ_LOG(warn, "Invalid command '", command, "' sent by remote [", to_hex(peer.pubkey), "]/", peer_address(cmd));
        reply = "UNKNOWNCOMMAND";
    } else if (peer.auth_level < cat_call.first->access.auth) {
        LMQ_LOG(warn, "Access denied to ", command, " for peer [", to_hex(peer.pubkey), "]/", peer_address(cmd),
                ": peer auth level ", peer.auth_level, " < ", cat_call.first->access.auth);
        reply = "FORBIDDEN";
    } else if (cat_call.first->access.local_sn && !local_service_node) {
        LMQ_LOG(warn, "Access denied to ", command, " for peer [", to_hex(peer.pubkey), "]/", peer_address(cmd),
                ": that command is only available when this OxenMQ is running in service node mode");
        reply = "NOT_A_SERVICE_NODE";
    } else if (cat_call.first->access.remote_sn && !peer.service_node) {
        LMQ_LOG(warn, "Access denied to ", command, " for peer [", to_hex(peer.pubkey), "]/", peer_address(cmd),
                ": remote is not recognized as a service node");
        reply = "FORBIDDEN_SN";
    } else if (cat_call.second->second /*is_request*/ && data.empty()) {
        LMQ_LOG(warn, "Received an invalid request for '", command, "' with no reply tag from remote [",
                to_hex(peer.pubkey), "]/", peer_address(cmd));
        reply = "NO_REPLY_TAG";
    } else {
        return true;
    }

    std::vector<zmq::message_t> msgs;
    msgs.reserve(4);
    if (!outgoing)
        msgs.push_back(create_message(peer.route));
    msgs.push_back(create_message(reply));
    if (cat_call.second && cat_call.second->second /*request command*/ && !data.empty()) {
        msgs.push_back(create_message("REPLY"sv));
        msgs.push_back(create_message(view(data.front()))); // reply tag
    } else {
        msgs.push_back(create_message(view(cmd)));
    }

    try {
        send_message_parts(connections.at(conn_id), msgs);
    } catch (const zmq::error_t& err) {
        /* can't send: possibly already disconnected.  Ignore. */
        LMQ_LOG(debug, "Couldn't send auth failure message ", reply, " to peer [", to_hex(peer.pubkey), "]/", peer_address(cmd), ": ", err.what());
    }

    return false;
}

void OxenMQ::set_active_sns(pubkey_set pubkeys) {
    if (proxy_thread.joinable()) {
        auto data = bt_serialize(detail::serialize_object(std::move(pubkeys)));
        detail::send_control(get_control_socket(), "SET_SNS", data);
    } else {
        proxy_set_active_sns(std::move(pubkeys));
    }
}
void OxenMQ::proxy_set_active_sns(std::string_view data) {
    proxy_set_active_sns(detail::deserialize_object<pubkey_set>(bt_deserialize<uintptr_t>(data)));
}
void OxenMQ::proxy_set_active_sns(pubkey_set pubkeys) {
    pubkey_set added, removed;
    for (auto it = pubkeys.begin(); it != pubkeys.end(); ) {
        auto& pk = *it;
        if (pk.size() != 32) {
            LMQ_LOG(warn, "Invalid private key of length ", pk.size(), " (", to_hex(pk), ") passed to set_active_sns");
            it = pubkeys.erase(it);
            continue;
        }
        if (!active_service_nodes.count(pk))
            added.insert(std::move(pk));
        ++it;
    }
    if (added.empty() && active_service_nodes.size() == pubkeys.size()) {
        LMQ_LOG(debug, "set_active_sns(): new set of SNs is unchanged, skipping update");
        return;
    }
    for (const auto& pk : active_service_nodes) {
        if (!pubkeys.count(pk))
            removed.insert(pk);
        if (active_service_nodes.size() + added.size() - removed.size() == pubkeys.size())
            break;
    }
    proxy_update_active_sns_clean(std::move(added), std::move(removed));
}

void OxenMQ::update_active_sns(pubkey_set added, pubkey_set removed) {
    LMQ_LOG(info, "uh, ", added.size());
    if (proxy_thread.joinable()) {
        std::array<uintptr_t, 2> data;
        data[0] = detail::serialize_object(std::move(added));
        data[1] = detail::serialize_object(std::move(removed));
        detail::send_control(get_control_socket(), "UPDATE_SNS", bt_serialize(data));
    } else {
        proxy_update_active_sns(std::move(added), std::move(removed));
    }
}
void OxenMQ::proxy_update_active_sns(bt_list_consumer data) {
    auto added = detail::deserialize_object<pubkey_set>(data.consume_integer<uintptr_t>());
    auto remed = detail::deserialize_object<pubkey_set>(data.consume_integer<uintptr_t>());
    proxy_update_active_sns(std::move(added), std::move(remed));
}
void OxenMQ::proxy_update_active_sns(pubkey_set added, pubkey_set removed) {
    // We take a caller-provided set of added/removed then filter out any junk (bad pks, conflicting
    // values, pubkeys that already(added) or do not(removed) exist), then pass the purified lists
    // to the _clean version.

    LMQ_LOG(info, "uh, ", added.size(), ", ", removed.size());
    for (auto it = removed.begin(); it != removed.end(); ) {
        const auto& pk = *it;
        if (pk.size() != 32) {
            LMQ_LOG(warn, "Invalid private key of length ", pk.size(), " (", to_hex(pk), ") passed to update_active_sns (removed)");
            it = removed.erase(it);
        } else if (!active_service_nodes.count(pk) || added.count(pk) /* added wins if in both */) {
            it = removed.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = added.begin(); it != added.end(); ) {
        const auto& pk = *it;
        if (pk.size() != 32) {
            LMQ_LOG(warn, "Invalid private key of length ", pk.size(), " (", to_hex(pk), ") passed to update_active_sns (added)");
            it = added.erase(it);
        } else if (active_service_nodes.count(pk)) {
            it = added.erase(it);
        } else {
            ++it;
        }
    }

    proxy_update_active_sns_clean(std::move(added), std::move(removed));
}

void OxenMQ::proxy_update_active_sns_clean(pubkey_set added, pubkey_set removed) {
    LMQ_LOG(debug, "Updating SN auth status with +", added.size(), "/-", removed.size(), " pubkeys");

    // For anything we remove we want close the connection to the SN (if outgoing), and remove the
    // stored peer_info (incoming or outgoing).
    for (const auto& pk : removed) {
        ConnectionID c{pk};
        active_service_nodes.erase(pk);
        auto range = peers.equal_range(c);
        for (auto it = range.first; it != range.second; ) {
            bool outgoing = it->second.outgoing();
            auto conn_id = it->second.conn_id;
            it = peers.erase(it);
            if (outgoing) {
                LMQ_LOG(debug, "Closing outgoing connection to ", c);
                proxy_close_connection(conn_id, CLOSE_LINGER);
            }
        }
    }

    // For pubkeys we add there's nothing special to be done beyond adding them to the pubkey set
    for (auto& pk : added)
        active_service_nodes.insert(std::move(pk));
}

void OxenMQ::process_zap_requests() {
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
            log(LogLevel::trace, __FILE__, __LINE__, o.str());
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
            } else if (bind[bind_id].curve
                    ? !(frames.size() == 7 && view(frames[5]) == "CURVE")
                    : !(frames.size() == 6 && view(frames[5]) == "NULL")) {
                LMQ_LOG(error, "Bad ZAP authentication request: invalid ",
                        bind[bind_id].curve ? "CURVE" : "NULL", " authentication request");
                status_code = "500";
                status_text = "Invalid authentication request mechanism";
            } else if (bind[bind_id].curve && frames[6].size() != 32) {
                LMQ_LOG(error, "Bad ZAP authentication request: invalid request pubkey");
                status_code = "500";
                status_text = "Invalid public key size for CURVE authentication";
            } else {
                auto ip = view(frames[3]);
                std::string_view pubkey;
                bool sn = false;
                if (bind[bind_id].curve) {
                    pubkey = view(frames[6]);
                    sn = active_service_nodes.count(std::string{pubkey});
                }
                auto auth = bind[bind_id].allow(ip, pubkey, sn);
                auto& user_id = response_vals[4];
                if (bind[bind_id].curve) {
                    user_id.reserve(64);
                    to_hex(pubkey.begin(), pubkey.end(), std::back_inserter(user_id));
                }

                if (auth <= AuthLevel::denied || auth > AuthLevel::admin) {
                    LMQ_LOG(info, "Access denied for incoming ", view(frames[5]), (sn ? " service node" : " client"),
                            " connection from ", !user_id.empty() ? user_id + " at " : ""s, ip,
                            " with initial auth level ", auth);
                    status_code = "400";
                    status_text = "Access denied";
                    user_id.clear();
                } else {
                    LMQ_LOG(debug, "Accepted incoming ", view(frames[5]), (sn ? " service node" : " client"),
                            " connection with authentication level ", auth,
                            " from ", !user_id.empty() ? user_id + " at " : ""s, ip);

                    auto& metadata = response_vals[5];
                    metadata += zmtp_metadata("X-AuthLevel", to_string(auth));

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
