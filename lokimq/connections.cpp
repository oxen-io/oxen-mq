#include "lokimq.h"
#include "lokimq-internal.h"
#include "hex.h"

namespace lokimq {

std::ostream& operator<<(std::ostream& o, const ConnectionID& conn) {
    if (!conn.pk.empty())
        return o << (conn.sn() ? "SN " : "non-SN authenticated remote ") << to_hex(conn.pk);
    else
        return o << "unauthenticated remote [" << conn.id << "]";
}

namespace {

void add_pollitem(std::vector<zmq::pollitem_t>& pollitems, zmq::socket_t& sock) {
    pollitems.emplace_back();
    auto &p = pollitems.back();
    p.socket = static_cast<void *>(sock);
    p.fd = 0;
    p.events = ZMQ_POLLIN;
}

} // anonymous namespace


void LokiMQ::rebuild_pollitems() {
    pollitems.clear();
    add_pollitem(pollitems, command);
    add_pollitem(pollitems, workers_socket);
    add_pollitem(pollitems, zap_auth);

    for (auto& s : connections)
        add_pollitem(pollitems, s);
    pollitems_stale = false;
}

void LokiMQ::setup_external_socket(zmq::socket_t& socket) {
    socket.set(zmq::sockopt::reconnect_ivl, (int) RECONNECT_INTERVAL.count());
    socket.set(zmq::sockopt::reconnect_ivl_max, (int) RECONNECT_INTERVAL_MAX.count());
    socket.set(zmq::sockopt::handshake_ivl, (int) HANDSHAKE_TIME.count());
    socket.set(zmq::sockopt::maxmsgsize, MAX_MSG_SIZE);
    if (CONN_HEARTBEAT > 0s) {
        socket.set(zmq::sockopt::heartbeat_ivl, (int) CONN_HEARTBEAT.count());
        if (CONN_HEARTBEAT_TIMEOUT > 0s)
            socket.set(zmq::sockopt::heartbeat_timeout, (int) CONN_HEARTBEAT_TIMEOUT.count());
    }
}

void LokiMQ::setup_outgoing_socket(zmq::socket_t& socket, std::string_view remote_pubkey) {

    setup_external_socket(socket);

    if (!remote_pubkey.empty()) {
        socket.set(zmq::sockopt::curve_serverkey, remote_pubkey);
        socket.set(zmq::sockopt::curve_publickey, pubkey);
        socket.set(zmq::sockopt::curve_secretkey, privkey);
    }

    if (PUBKEY_BASED_ROUTING_ID) {
        std::string routing_id;
        routing_id.reserve(33);
        routing_id += 'L'; // Prefix because routing id's starting with \0 are reserved by zmq (and our pubkey might start with \0)
        routing_id.append(pubkey.begin(), pubkey.end());
        socket.set(zmq::sockopt::routing_id, routing_id);
    }
    // else let ZMQ pick a random one
}

ConnectionID LokiMQ::connect_sn(std::string_view pubkey, std::chrono::milliseconds keep_alive, std::string_view hint) {
    if (!proxy_thread.joinable())
        throw std::logic_error("Cannot call connect_sn() before calling `start()`");

    detail::send_control(get_control_socket(), "CONNECT_SN", bt_serialize<bt_dict>({{"pubkey",pubkey}, {"keep_alive",keep_alive.count()}, {"hint",hint}}));

    return pubkey;
}

ConnectionID LokiMQ::connect_remote(const address& remote, ConnectSuccess on_connect, ConnectFailure on_failure,
        AuthLevel auth_level, std::chrono::milliseconds timeout) {
    if (!proxy_thread.joinable())
        throw std::logic_error("Cannot call connect_remote() before calling `start()`");

    auto id = next_conn_id++;
    LMQ_TRACE("telling proxy to connect to ", remote, ", id ", id);
    detail::send_control(get_control_socket(), "CONNECT_REMOTE", bt_serialize<bt_dict>({
        {"auth_level", static_cast<std::underlying_type_t<AuthLevel>>(auth_level)},
        {"conn_id", id},
        {"connect", detail::serialize_object(std::move(on_connect))},
        {"failure", detail::serialize_object(std::move(on_failure))},
        {"pubkey", remote.curve() ? remote.pubkey : ""},
        {"remote", remote.zmq_address()},
        {"timeout", timeout.count()},
    }));

    return id;
}

ConnectionID LokiMQ::connect_remote(std::string_view remote, ConnectSuccess on_connect, ConnectFailure on_failure,
        std::string_view pubkey, AuthLevel auth_level, std::chrono::milliseconds timeout) {
    return connect_remote(address{remote}.set_pubkey(pubkey),
            std::move(on_connect), std::move(on_failure), auth_level, timeout);
}

void LokiMQ::disconnect(ConnectionID id, std::chrono::milliseconds linger) {
    detail::send_control(get_control_socket(), "DISCONNECT", bt_serialize<bt_dict>({
            {"conn_id", id.id},
            {"linger_ms", linger.count()},
            {"pubkey", id.pk},
    }));
}

std::pair<zmq::socket_t *, std::string>
LokiMQ::proxy_connect_sn(std::string_view remote, std::string_view connect_hint, bool optional, bool incoming_only, bool outgoing_only, std::chrono::milliseconds keep_alive) {
    ConnectionID remote_cid{remote};
    auto its = peers.equal_range(remote_cid);
    peer_info* peer = nullptr;
    for (auto it = its.first; it != its.second; ++it) {
        if (incoming_only && it->second.route.empty())
            continue; // outgoing connection but we were asked to only use incoming connections
        if (outgoing_only && !it->second.route.empty())
            continue;
        peer = &it->second;
        break;
    }

    if (peer) {
        LMQ_TRACE("proxy asked to connect to ", to_hex(remote), "; reusing existing connection");
        if (peer->route.empty() /* == outgoing*/) {
            if (peer->idle_expiry < keep_alive) {
                LMQ_LOG(debug, "updating existing outgoing peer connection idle expiry time from ",
                        peer->idle_expiry.count(), "ms to ", keep_alive.count(), "ms");
                peer->idle_expiry = keep_alive;
            }
            peer->activity();
        }
        return {&connections[peer->conn_index], peer->route};
    } else if (optional || incoming_only) {
        LMQ_LOG(debug, "proxy asked for optional or incoming connection, but no appropriate connection exists so aborting connection attempt");
        return {nullptr, ""s};
    }

    // No connection so establish a new one
    LMQ_LOG(debug, "proxy establishing new outbound connection to ", to_hex(remote));
    std::string addr;
    bool to_self = false && remote == pubkey; // FIXME; need to use a separate listening socket for this, otherwise we can't easily
                                              // tell it wasn't from a remote.
    if (to_self) {
        // special inproc connection if self that doesn't need any external connection
        addr = SN_ADDR_SELF;
    } else {
        addr = std::string{connect_hint};
        if (addr.empty())
            addr = sn_lookup(remote);
        else
            LMQ_LOG(debug, "using connection hint ", connect_hint);

        if (addr.empty()) {
            LMQ_LOG(error, "peer lookup failed for ", to_hex(remote));
            return {nullptr, ""s};
        }
    }

    LMQ_LOG(debug, to_hex(pubkey), " (me) connecting to ", addr, " to reach ", to_hex(remote));
    zmq::socket_t socket{context, zmq::socket_type::dealer};
    setup_outgoing_socket(socket, remote);
    try {
        socket.connect(addr);
    } catch (const zmq::error_t& e) {
        // Note that this failure cases indicates something serious went wrong that means zmq isn't
        // even going to try connecting (for example an unparseable remote address).
        LMQ_LOG(error, "Outgoing connection to ", addr, " failed: ", e.what());
        return {nullptr, ""s};
    }
    peer_info p{};
    p.service_node = true;
    p.pubkey = std::string{remote};
    p.conn_index = connections.size();
    p.idle_expiry = keep_alive;
    p.activity();
    conn_index_to_id.push_back(remote_cid);
    peers.emplace(std::move(remote_cid), std::move(p));
    connections.push_back(std::move(socket));
    pollitems_stale = true;

    return {&connections.back(), ""s};
}

std::pair<zmq::socket_t *, std::string> LokiMQ::proxy_connect_sn(bt_dict_consumer data) {
    std::string_view hint, remote_pk;
    std::chrono::milliseconds keep_alive;
    bool optional = false, incoming_only = false, outgoing_only = false;

    // Alphabetical order
    if (data.skip_until("hint"))
        hint = data.consume_string_view();
    if (data.skip_until("incoming"))
        incoming_only = data.consume_integer<bool>();
    if (data.skip_until("keep_alive"))
        keep_alive = std::chrono::milliseconds{data.consume_integer<uint64_t>()};
    if (data.skip_until("optional"))
        optional = data.consume_integer<bool>();
    if (data.skip_until("outgoing_only"))
        outgoing_only = data.consume_integer<bool>();
    if (!data.skip_until("pubkey"))
        throw std::runtime_error("Internal error: Invalid proxy_connect_sn command; pubkey missing");
    remote_pk = data.consume_string_view();

    return proxy_connect_sn(remote_pk, hint, optional, incoming_only, outgoing_only, keep_alive);
}

template <typename Container, typename AccessIndex>
void update_connection_indices(Container& c, size_t index, AccessIndex get_index) {
    for (auto it = c.begin(); it != c.end(); ) {
        size_t& i = get_index(*it);
        if (index == i) {
            it = c.erase(it);
            continue;
        }
        if (i > index)
            --i;
        ++it;
    }
}

/// Closes outgoing connections and removes all references.  Note that this will call `erase()`
/// which can invalidate iterators on the various connection containers - if you don't want that,
/// delete it first so that the container won't contain the element being deleted.
void LokiMQ::proxy_close_connection(size_t index, std::chrono::milliseconds linger) {
    connections[index].set(zmq::sockopt::linger, linger > 0ms ? (int) linger.count() : 0);
    pollitems_stale = true;
    connections.erase(connections.begin() + index);

    LMQ_LOG(debug, "Closing conn index ", index);
    update_connection_indices(peers, index,
            [](auto& p) -> size_t& { return p.second.conn_index; });
    update_connection_indices(pending_connects, index,
            [](auto& pc) -> size_t& { return std::get<size_t>(pc); });
    update_connection_indices(bind, index,
            [](auto& b) -> size_t& { return b.second.index; });
    update_connection_indices(incoming_conn_index, index,
            [](auto& oci) -> size_t& { return oci.second; });
    assert(index < conn_index_to_id.size());
    conn_index_to_id.erase(conn_index_to_id.begin() + index);
}

void LokiMQ::proxy_expire_idle_peers() {
    for (auto it = peers.begin(); it != peers.end(); ) {
        auto &info = it->second;
        if (info.outgoing()) {
            auto idle = std::chrono::steady_clock::now() - info.last_activity;
            if (idle > info.idle_expiry) {
                LMQ_LOG(debug, "Closing outgoing connection to ", it->first, ": idle time (",
                        std::chrono::duration_cast<std::chrono::milliseconds>(idle).count(), "ms) reached connection timeout (",
                        info.idle_expiry.count(), "ms)");
                ++it; // The below is going to delete our current element
                proxy_close_connection(info.conn_index, CLOSE_LINGER);
            } else {
                LMQ_LOG(trace, "Not closing ", it->first, ": ", std::chrono::duration_cast<std::chrono::milliseconds>(idle).count(),
                        "ms <= ", info.idle_expiry.count(), "ms");
                ++it;
                continue;
            }
        } else {
            ++it;
        }
    }
}

void LokiMQ::proxy_conn_cleanup() {
    LMQ_TRACE("starting proxy connections cleanup");

    // Drop idle connections (if we haven't done it in a while)
    LMQ_TRACE("closing idle connections");
    proxy_expire_idle_peers();

    auto now = std::chrono::steady_clock::now();

    // FIXME - check other outgoing connections to see if they died and if so purge them

    LMQ_TRACE("Timing out pending outgoing connections");
    // Check any pending outgoing connections for timeout
    for (auto it = pending_connects.begin(); it != pending_connects.end(); ) {
        auto& pc = *it;
        if (std::get<std::chrono::steady_clock::time_point>(pc) < now) {
            job([cid = ConnectionID{std::get<long long>(pc)}, callback = std::move(std::get<ConnectFailure>(pc))] { callback(cid, "connection attempt timed out"); });
            it = pending_connects.erase(it); // Don't let the below erase it (because it invalidates iterators)
            proxy_close_connection(std::get<size_t>(pc), CLOSE_LINGER);
        } else {
            ++it;
        }
    }

    LMQ_TRACE("Timing out pending requests");
    // Remove any expired pending requests and schedule their callback with a failure
    for (auto it = pending_requests.begin(); it != pending_requests.end(); ) {
        auto& callback = it->second;
        if (callback.first < now) {
            LMQ_LOG(debug, "pending request ", to_hex(it->first), " expired, invoking callback with failure status and removing");
            job([callback = std::move(callback.second)] { callback(false, {{"TIMEOUT"s}}); });
            it = pending_requests.erase(it);
        } else {
            ++it;
        }
    }

    LMQ_TRACE("done proxy connections cleanup");
};

void LokiMQ::proxy_connect_remote(bt_dict_consumer data) {
    AuthLevel auth_level = AuthLevel::none;
    long long conn_id = -1;
    ConnectSuccess on_connect;
    ConnectFailure on_failure;
    std::string remote;
    std::string remote_pubkey;
    std::chrono::milliseconds timeout = REMOTE_CONNECT_TIMEOUT;

    if (data.skip_until("auth_level"))
        auth_level = static_cast<AuthLevel>(data.consume_integer<std::underlying_type_t<AuthLevel>>());
    if (data.skip_until("conn_id"))
        conn_id = data.consume_integer<long long>();
    if (data.skip_until("connect")) {
        on_connect = detail::deserialize_object<ConnectSuccess>(data.consume_integer<uintptr_t>());
    }
    if (data.skip_until("failure")) {
        on_failure = detail::deserialize_object<ConnectFailure>(data.consume_integer<uintptr_t>());
    }
    if (data.skip_until("pubkey")) {
        remote_pubkey = data.consume_string();
        assert(remote_pubkey.size() == 32 || remote_pubkey.empty());
    }
    if (data.skip_until("remote"))
        remote = data.consume_string();
    if (data.skip_until("timeout"))
        timeout = std::chrono::milliseconds{data.consume_integer<uint64_t>()};

    if (conn_id == -1 || remote.empty())
        throw std::runtime_error("Internal error: CONNECT_REMOTE proxy command missing required 'conn_id' and/or 'remote' value");

    LMQ_LOG(debug, "Establishing remote connection to ", remote, remote_pubkey.empty() ? " (NULL auth)" : " via CURVE expecting pubkey " + to_hex(remote_pubkey));

    assert(conn_index_to_id.size() == connections.size());

    zmq::socket_t sock{context, zmq::socket_type::dealer};
    try {
        setup_outgoing_socket(sock, remote_pubkey);
        sock.connect(remote);
    } catch (const zmq::error_t &e) {
        proxy_schedule_reply_job([conn_id, on_failure=std::move(on_failure), what="connect() failed: "s+e.what()] {
                on_failure(conn_id, std::move(what));
        });
        return;
    }

    connections.push_back(std::move(sock));
    pollitems_stale = true;
    LMQ_LOG(debug, "Opened new zmq socket to ", remote, ", conn_id ", conn_id, "; sending HI");
    send_direct_message(connections.back(), "HI");
    pending_connects.emplace_back(connections.size()-1, conn_id, std::chrono::steady_clock::now() + timeout,
            std::move(on_connect), std::move(on_failure));
    peer_info peer;
    peer.pubkey = std::move(remote_pubkey);
    peer.service_node = false;
    peer.auth_level = auth_level;
    peer.conn_index = connections.size() - 1;
    ConnectionID conn{conn_id, peer.pubkey};
    conn_index_to_id.push_back(conn);
    assert(connections.size() == conn_index_to_id.size());
    peer.idle_expiry = 24h * 10 * 365; // "forever"
    peer.activity();
    peers.emplace(std::move(conn), std::move(peer));
}

void LokiMQ::proxy_disconnect(bt_dict_consumer data) {
    ConnectionID connid{-1};
    std::chrono::milliseconds linger = 1s;

    if (data.skip_until("conn_id"))
        connid.id = data.consume_integer<long long>();
    if (data.skip_until("linger_ms"))
        linger = std::chrono::milliseconds(data.consume_integer<long long>());
    if (data.skip_until("pubkey"))
        connid.pk = data.consume_string();

    if (connid.sn() && connid.pk.size() != 32)
        throw std::runtime_error("Error: invalid disconnect of SN without a valid pubkey");

    proxy_disconnect(std::move(connid), linger);
}
void LokiMQ::proxy_disconnect(ConnectionID conn, std::chrono::milliseconds linger) {
    LMQ_TRACE("Disconnecting outgoing connection to ", conn);
    auto pr = peers.equal_range(conn);
    for (auto it = pr.first; it != pr.second; ++it) {
        auto& peer = it->second;
        if (peer.outgoing()) {
            LMQ_LOG(debug, "Closing outgoing connection to ", conn);
            proxy_close_connection(peer.conn_index, linger);
            return;
        }
    }
    LMQ_LOG(warn, "Failed to disconnect ", conn, ": no such outgoing connection");
}




}
