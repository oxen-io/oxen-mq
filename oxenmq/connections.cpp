#include "oxenmq.h"
#include "oxenmq-internal.h"
#include <oxenc/hex.h>
#include <optional>

#ifdef OXENMQ_USE_EPOLL
extern "C" {
#include <sys/epoll.h>
#include <unistd.h>
}
#endif

namespace oxenmq {

std::ostream& operator<<(std::ostream& o, const ConnectionID& conn) {
    return o << conn.to_string();
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

void OxenMQ::rebuild_pollitems() {

#ifdef OXENMQ_USE_EPOLL
    if (using_epoll) {
        if (epoll_fd != -1)
            close(epoll_fd);
        epoll_fd = epoll_create1(0);

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.u64 = EPOLL_COMMAND_ID;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, command.get(zmq::sockopt::fd), &ev);

        ev.data.u64 = EPOLL_WORKER_ID;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, workers_socket.get(zmq::sockopt::fd), &ev);

        ev.data.u64 = EPOLL_ZAP_ID;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, zap_auth.get(zmq::sockopt::fd), &ev);

        for (auto& [id, s] : connections) {
            ev.data.u64 = id;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s.get(zmq::sockopt::fd), &ev);
        }
        connections_updated = false;
        return;
    }
#endif

    // No epoll, or epoll not enabled.
    pollitems.clear();
    add_pollitem(pollitems, command);
    add_pollitem(pollitems, workers_socket);
    add_pollitem(pollitems, zap_auth);

    for (auto& [id, s] : connections)
        add_pollitem(pollitems, s);
    connections_updated = false;
}

void OxenMQ::setup_external_socket(zmq::socket_t& socket) {
    socket.set(zmq::sockopt::reconnect_ivl, (int) RECONNECT_INTERVAL.count());
    socket.set(zmq::sockopt::reconnect_ivl_max, (int) RECONNECT_INTERVAL_MAX.count());
    socket.set(zmq::sockopt::handshake_ivl, (int) HANDSHAKE_TIME.count());
    socket.set(zmq::sockopt::maxmsgsize, MAX_MSG_SIZE);
    if (IPV6)
        socket.set(zmq::sockopt::ipv6, 1);

    if (CONN_HEARTBEAT > 0s) {
        socket.set(zmq::sockopt::heartbeat_ivl, (int) CONN_HEARTBEAT.count());
        if (CONN_HEARTBEAT_TIMEOUT > 0s)
            socket.set(zmq::sockopt::heartbeat_timeout, (int) CONN_HEARTBEAT_TIMEOUT.count());
    }
}

void OxenMQ::setup_outgoing_socket(zmq::socket_t& socket, std::string_view remote_pubkey, bool use_ephemeral_routing_id) {

    setup_external_socket(socket);

    if (!remote_pubkey.empty()) {
        socket.set(zmq::sockopt::curve_serverkey, remote_pubkey);
        socket.set(zmq::sockopt::curve_publickey, pubkey);
        socket.set(zmq::sockopt::curve_secretkey, privkey);
    }

    if (!use_ephemeral_routing_id) {
        std::string routing_id;
        routing_id.reserve(33);
        routing_id += 'L'; // Prefix because routing id's starting with \0 are reserved by zmq (and our pubkey might start with \0)
        routing_id.append(pubkey.begin(), pubkey.end());
        socket.set(zmq::sockopt::routing_id, routing_id);
    }
    // else let ZMQ pick a random one
}


void OxenMQ::setup_incoming_socket(zmq::socket_t& listener, bool curve, std::string_view pubkey, std::string_view privkey, size_t bind_index) {

    setup_external_socket(listener);

    listener.set(zmq::sockopt::zap_domain, oxenc::bt_serialize(bind_index));
    if (curve) {
        listener.set(zmq::sockopt::curve_server, true);
        listener.set(zmq::sockopt::curve_publickey, pubkey);
        listener.set(zmq::sockopt::curve_secretkey, privkey);
    }
    listener.set(zmq::sockopt::router_handover, true);
    listener.set(zmq::sockopt::router_mandatory, true);
}

// Deprecated versions:
ConnectionID OxenMQ::connect_remote(std::string_view remote, ConnectSuccess on_connect,
        ConnectFailure on_failure, AuthLevel auth_level, std::chrono::milliseconds timeout) {
    return connect_remote(address{remote}, std::move(on_connect), std::move(on_failure),
            auth_level, connect_option::timeout{timeout});
}

ConnectionID OxenMQ::connect_remote(std::string_view remote, ConnectSuccess on_connect,
        ConnectFailure on_failure, std::string_view pubkey, AuthLevel auth_level,
        std::chrono::milliseconds timeout) {
    return connect_remote(address{remote}.set_pubkey(pubkey), std::move(on_connect),
            std::move(on_failure), auth_level, connect_option::timeout{timeout});
}

void OxenMQ::disconnect(ConnectionID id, std::chrono::milliseconds linger) {
    detail::send_control(get_control_socket(), "DISCONNECT", oxenc::bt_serialize<oxenc::bt_dict>({
            {"conn_id", id.id},
            {"linger_ms", linger.count()},
            {"pubkey", id.pk},
    }));
}

std::pair<zmq::socket_t *, std::string>
OxenMQ::proxy_connect_sn(std::string_view remote, std::string_view connect_hint, bool optional, bool incoming_only, bool outgoing_only, bool use_ephemeral_routing_id, std::chrono::milliseconds keep_alive) {
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
        OMQ_TRACE("proxy asked to connect to ", oxenc::to_hex(remote), "; reusing existing connection");
        if (peer->route.empty() /* == outgoing*/) {
            if (peer->idle_expiry < keep_alive) {
                OMQ_LOG(debug, "updating existing outgoing peer connection idle expiry time from ",
                        peer->idle_expiry.count(), "ms to ", keep_alive.count(), "ms");
                peer->idle_expiry = keep_alive;
            }
            peer->activity();
        }
        return {&connections[peer->conn_id], peer->route};
    } else if (optional || incoming_only) {
        OMQ_LOG(debug, "proxy asked for optional or incoming connection, but no appropriate connection exists so aborting connection attempt");
        return {nullptr, ""s};
    }

    // No connection so establish a new one
    OMQ_LOG(debug, "proxy establishing new outbound connection to ", oxenc::to_hex(remote));
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
            OMQ_LOG(debug, "using connection hint ", connect_hint);

        if (addr.empty()) {
            OMQ_LOG(error, "peer lookup failed for ", oxenc::to_hex(remote));
            return {nullptr, ""s};
        }
    }

    OMQ_LOG(debug, oxenc::to_hex(pubkey), " (me) connecting to ", addr, " to reach ", oxenc::to_hex(remote));
    std::optional<zmq::socket_t> socket;
    try {
        socket.emplace(context, zmq::socket_type::dealer);
        setup_outgoing_socket(*socket, remote, use_ephemeral_routing_id);
        socket->connect(addr);
    } catch (const zmq::error_t& e) {
        // Note that this failure cases indicates something serious went wrong that means zmq isn't
        // even going to try connecting (for example an unparseable remote address).
        OMQ_LOG(error, "Outgoing connection to ", addr, " failed: ", e.what());
        return {nullptr, ""s};
    }

    auto& p = peers.emplace(std::move(remote_cid), peer_info{})->second;
    p.service_node = true;
    p.pubkey = std::string{remote};
    p.conn_id = next_conn_id++;
    p.idle_expiry = keep_alive;
    p.activity();
    connections_updated = true;
    outgoing_sn_conns.emplace_hint(outgoing_sn_conns.end(), p.conn_id, ConnectionID{remote});
    auto it = connections.emplace_hint(connections.end(), p.conn_id, *std::move(socket));

    return {&it->second, ""s};
}

std::pair<zmq::socket_t *, std::string> OxenMQ::proxy_connect_sn(oxenc::bt_dict_consumer data) {
    std::string_view hint, remote_pk;
    std::chrono::milliseconds keep_alive;
    bool optional = false, incoming_only = false, outgoing_only = false, ephemeral_rid = EPHEMERAL_ROUTING_ID;

    // Alphabetical order
    if (data.skip_until("ephemeral_rid"))
        ephemeral_rid = data.consume_integer<bool>();
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

    return proxy_connect_sn(remote_pk, hint, optional, incoming_only, outgoing_only, ephemeral_rid, keep_alive);
}

/// Closes outgoing connections and removes all references.  Note that this will call `erase()`
/// which can invalidate iterators on the various connection containers - if you don't want that,
/// delete it first so that the container won't contain the element being deleted.
void OxenMQ::proxy_close_connection(int64_t id, std::chrono::milliseconds linger) {
    auto it = connections.find(id);
    if (it == connections.end()) {
        OMQ_LOG(warn, "internal error: connection to close (", id, ") doesn't exist!");
        return;
    }
    OMQ_LOG(debug, "Closing conn ", id);
    it->second.set(zmq::sockopt::linger, linger > 0ms ? (int) linger.count() : 0);
    connections.erase(it);
    connections_updated = true;

    outgoing_sn_conns.erase(id);
}

void OxenMQ::proxy_expire_idle_peers() {
    for (auto it = peers.begin(); it != peers.end(); ) {
        auto &info = it->second;
        if (info.outgoing()) {
            auto idle = std::chrono::steady_clock::now() - info.last_activity;
            if (idle > info.idle_expiry) {
                OMQ_LOG(debug, "Closing outgoing connection to ", it->first, ": idle time (",
                        std::chrono::duration_cast<std::chrono::milliseconds>(idle).count(), "ms) reached connection timeout (",
                        info.idle_expiry.count(), "ms)");
                proxy_close_connection(info.conn_id, CLOSE_LINGER);
                it = peers.erase(it);
            } else {
                OMQ_LOG(trace, "Not closing ", it->first, ": ", std::chrono::duration_cast<std::chrono::milliseconds>(idle).count(),
                        "ms <= ", info.idle_expiry.count(), "ms");
                ++it;
                continue;
            }
        } else {
            ++it;
        }
    }
}

void OxenMQ::proxy_conn_cleanup() {
    OMQ_TRACE("starting proxy connections cleanup");

    // Drop idle connections (if we haven't done it in a while)
    OMQ_TRACE("closing idle connections");
    proxy_expire_idle_peers();

    auto now = std::chrono::steady_clock::now();

    // FIXME - check other outgoing connections to see if they died and if so purge them

    OMQ_TRACE("Timing out pending outgoing connections");
    // Check any pending outgoing connections for timeout
    for (auto it = pending_connects.begin(); it != pending_connects.end(); ) {
        auto& pc = *it;
        if (std::get<std::chrono::steady_clock::time_point>(pc) < now) {
            auto id = std::get<int64_t>(pc);
            job([cid = ConnectionID{id}, callback = std::move(std::get<ConnectFailure>(pc))] { callback(cid, "connection attempt timed out"); });
            it = pending_connects.erase(it); // Don't let the below erase it (because it invalidates iterators)
            proxy_close_connection(id, CLOSE_LINGER);
        } else {
            ++it;
        }
    }

    OMQ_TRACE("Timing out pending requests");
    // Remove any expired pending requests and schedule their callback with a failure
    for (auto it = pending_requests.begin(); it != pending_requests.end(); ) {
        auto& callback = it->second;
        if (callback.first < now) {
            OMQ_LOG(debug, "pending request ", oxenc::to_hex(it->first), " expired, invoking callback with failure status and removing");
            job([callback = std::move(callback.second)] { callback(false, {{"TIMEOUT"s}}); });
            it = pending_requests.erase(it);
        } else {
            ++it;
        }
    }

    OMQ_TRACE("done proxy connections cleanup");
};

void OxenMQ::proxy_connect_remote(oxenc::bt_dict_consumer data) {
    AuthLevel auth_level = AuthLevel::none;
    long long conn_id = -1;
    ConnectSuccess on_connect;
    ConnectFailure on_failure;
    std::string remote;
    std::string remote_pubkey;
    std::chrono::milliseconds timeout = REMOTE_CONNECT_TIMEOUT;
    bool ephemeral_rid = EPHEMERAL_ROUTING_ID;

    if (data.skip_until("auth_level"))
        auth_level = static_cast<AuthLevel>(data.consume_integer<std::underlying_type_t<AuthLevel>>());
    if (data.skip_until("conn_id"))
        conn_id = data.consume_integer<long long>();
    if (data.skip_until("connect"))
        on_connect = detail::deserialize_object<ConnectSuccess>(data.consume_integer<uintptr_t>());
    if (data.skip_until("ephemeral_rid"))
        ephemeral_rid = data.consume_integer<bool>();
    if (data.skip_until("failure"))
        on_failure = detail::deserialize_object<ConnectFailure>(data.consume_integer<uintptr_t>());
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

    OMQ_LOG(debug, "Establishing remote connection to ", remote,
            remote_pubkey.empty() ? " (NULL auth)" : " via CURVE expecting pubkey " + oxenc::to_hex(remote_pubkey));

    std::optional<zmq::socket_t> sock;
    try {
        sock.emplace(context, zmq::socket_type::dealer);
        setup_outgoing_socket(*sock, remote_pubkey, ephemeral_rid);
        sock->connect(remote);
    } catch (const zmq::error_t &e) {
        proxy_schedule_reply_job([conn_id, on_failure=std::move(on_failure), what="connect() failed: "s+e.what()] {
                on_failure(conn_id, std::move(what));
        });
        return;
    }

    auto &s = connections.emplace_hint(connections.end(), conn_id, std::move(*sock))->second;
    connections_updated = true;
    OMQ_LOG(debug, "Opened new zmq socket to ", remote, ", conn_id ", conn_id, "; sending HI");
    send_direct_message(s, "HI");
    pending_connects.emplace_back(conn_id, std::chrono::steady_clock::now() + timeout,
            std::move(on_connect), std::move(on_failure));
    auto& peer = peers.emplace(ConnectionID{conn_id, remote_pubkey}, peer_info{})->second;
    peer.pubkey = std::move(remote_pubkey);
    peer.service_node = false;
    peer.auth_level = auth_level;
    peer.conn_id = conn_id;
    peer.idle_expiry = 24h * 10 * 365; // "forever"
    peer.activity();
}

void OxenMQ::proxy_disconnect(oxenc::bt_dict_consumer data) {
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
void OxenMQ::proxy_disconnect(ConnectionID conn, std::chrono::milliseconds linger) {
    OMQ_TRACE("Disconnecting outgoing connection to ", conn);
    auto pr = peers.equal_range(conn);
    for (auto it = pr.first; it != pr.second; ++it) {
        auto& peer = it->second;
        if (peer.outgoing()) {
            OMQ_LOG(debug, "Closing outgoing connection to ", conn);
            proxy_close_connection(peer.conn_id, linger);
            peers.erase(it);
            return;
        }
    }
    OMQ_LOG(warn, "Failed to disconnect ", conn, ": no such outgoing connection");
}

std::string ConnectionID::to_string() const {
    if (!pk.empty())
        return (sn() ? std::string("SN ") : std::string("non-SN authenticated remote ")) + oxenc::to_hex(pk);
    else
        return std::string("unauthenticated remote [") + std::to_string(id) + "]";
}


}
