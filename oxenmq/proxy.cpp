#include "oxenmq.h"
#include "oxenmq-internal.h"
#include "hex.h"

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
extern "C" {
#include <pthread.h>
#include <pthread_np.h>
}
#endif

#ifndef _WIN32
extern "C" {
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
}
#endif

namespace oxenmq {

void OxenMQ::proxy_quit() {
    LMQ_LOG(debug, "Received quit command, shutting down proxy thread");

    assert(std::none_of(workers.begin(), workers.end(), [](auto& worker) { return worker.worker_thread.joinable(); }));
    assert(std::none_of(tagged_workers.begin(), tagged_workers.end(), [](auto& worker) { return std::get<0>(worker).worker_thread.joinable(); }));

    command.set(zmq::sockopt::linger, 0);
    command.close();
    {
        std::lock_guard lock{control_sockets_mutex};
        proxy_shutting_down = true; // To prevent threads from opening new control sockets
    }
    workers_socket.close();
    int linger = std::chrono::milliseconds{CLOSE_LINGER}.count();
    for (auto& [id, s] : connections)
        s.set(zmq::sockopt::linger, linger);
    connections.clear();
    peers.clear();

    LMQ_LOG(debug, "Proxy thread teardown complete");
}

void OxenMQ::proxy_send(bt_dict_consumer data) {
    // NB: bt_dict_consumer goes in alphabetical order
    std::string_view hint;
    std::chrono::milliseconds keep_alive{DEFAULT_SEND_KEEP_ALIVE};
    std::chrono::milliseconds request_timeout{DEFAULT_REQUEST_TIMEOUT};
    bool optional = false;
    bool outgoing = false;
    bool incoming = false;
    bool request = false;
    bool have_conn_id = false;
    ConnectionID conn_id;

    std::string request_tag;
    ReplyCallback request_callback;
    if (data.skip_until("conn_id")) {
        conn_id.id = data.consume_integer<long long>();
        if (conn_id.id == -1)
            throw std::runtime_error("Invalid error: invalid conn_id value (-1)");
        have_conn_id = true;
    }
    if (data.skip_until("conn_pubkey")) {
        if (have_conn_id)
            throw std::runtime_error("Internal error: Invalid proxy send command; conn_id and conn_pubkey are exclusive");
        conn_id.pk = data.consume_string();
        conn_id.id = ConnectionID::SN_ID;
    } else if (!have_conn_id)
        throw std::runtime_error("Internal error: Invalid proxy send command; conn_pubkey or conn_id missing");
    if (data.skip_until("conn_route"))
        conn_id.route = data.consume_string();
    if (data.skip_until("hint"))
        hint = data.consume_string_view();
    if (data.skip_until("incoming"))
        incoming = data.consume_integer<bool>();
    if (data.skip_until("keep_alive"))
        keep_alive = std::chrono::milliseconds{data.consume_integer<uint64_t>()};
    if (data.skip_until("optional"))
        optional = data.consume_integer<bool>();
    if (data.skip_until("outgoing"))
        outgoing = data.consume_integer<bool>();

    if (data.skip_until("request"))
        request = data.consume_integer<bool>();
    if (request) {
        if (!data.skip_until("request_callback"))
            throw std::runtime_error("Internal error: received request without request_callback");

        request_callback = detail::deserialize_object<ReplyCallback>(data.consume_integer<uintptr_t>());

        if (!data.skip_until("request_tag"))
            throw std::runtime_error("Internal error: received request without request_name");
        request_tag = data.consume_string();
        if (data.skip_until("request_timeout"))
            request_timeout = std::chrono::milliseconds{data.consume_integer<uint64_t>()};
    }
    if (!data.skip_until("send"))
        throw std::runtime_error("Internal error: Invalid proxy send command; send parts missing");
    bt_list_consumer send = data.consume_list_consumer();

    send_option::queue_failure::callback_t callback_nosend;
    if (data.skip_until("send_fail"))
        callback_nosend = detail::deserialize_object<decltype(callback_nosend)>(data.consume_integer<uintptr_t>());

    send_option::queue_full::callback_t callback_noqueue;
    if (data.skip_until("send_full_q"))
        callback_noqueue = detail::deserialize_object<decltype(callback_noqueue)>(data.consume_integer<uintptr_t>());

    // Now figure out which socket to send to and do the actual sending.  We can repeat this loop
    // multiple times, if we're sending to a SN, because it's possible that we have multiple
    // connections open to that SN (e.g. one out + one in) so if one fails we can clean up that
    // connection and try the next one.
    bool retry = true, sent = false, nowarn = false;
    while (retry) {
        retry = false;
        zmq::socket_t *send_to;
        if (conn_id.sn()) {
            auto sock_route = proxy_connect_sn(conn_id.pk, hint, optional, incoming, outgoing, EPHEMERAL_ROUTING_ID, keep_alive);
            if (!sock_route.first) {
                nowarn = true;
                if (optional)
                    LMQ_LOG(debug, "Not sending: send is optional and no connection to ",
                            to_hex(conn_id.pk), " is currently established");
                else
                    LMQ_LOG(error, "Unable to send to ", to_hex(conn_id.pk), ": no valid connection address found");
                break;
            }
            send_to = sock_route.first;
            conn_id.route = std::move(sock_route.second);
        } else if (!conn_id.route.empty()) { // incoming non-SN connection
            auto it = connections.find(conn_id.id);
            if (it == connections.end()) {
                LMQ_LOG(warn, "Unable to send to ", conn_id, ": incoming listening socket not found");
                break;
            }
            send_to = &it->second;
        } else {
            auto pr = peers.equal_range(conn_id);
            if (pr.first == peers.end()) {
                LMQ_LOG(warn, "Unable to send: connection id ", conn_id, " is not (or is no longer) a valid outgoing connection");
                break;
            }
            auto& peer = pr.first->second;
            auto it = connections.find(peer.conn_id);
            if (it == connections.end()) {
                LMQ_LOG(warn, "Unable to send: peer connection id ", conn_id, " is not (or is no longer) a valid outgoing connection");
                break;
            }
            send_to = &it->second;
        }

        try {
            sent = send_message_parts(*send_to, build_send_parts(send, conn_id.route));
        } catch (const zmq::error_t &e) {
            if (e.num() == EHOSTUNREACH && !conn_id.route.empty() /*= incoming conn*/) {

                LMQ_LOG(debug, "Incoming connection is no longer valid; removing peer details");

                auto pr = peers.equal_range(conn_id);
                if (pr.first != peers.end()) {
                    if (!conn_id.sn()) {
                        peers.erase(pr.first);
                    } else {
                        bool removed;
                        for (auto it = pr.first; it != pr.second; ) {
                            auto& peer = it->second;
                            if (peer.route == conn_id.route) {
                                peers.erase(it);
                                removed = true;
                                break;
                            }
                        }
                        // The incoming connection to the SN is no longer good, but we can retry because
                        // we may have another active connection with the SN (or may want to open one).
                        if (removed) {
                            LMQ_LOG(debug, "Retrying sending to SN ", to_hex(conn_id.pk), " using other sockets");
                            retry = true;
                        }
                    }
                }
            }
            if (!retry) {
                if (!conn_id.sn() && !conn_id.route.empty()) { // incoming non-SN connection
                    LMQ_LOG(debug, "Unable to send message to incoming connection ", conn_id, ": ", e.what(),
                            "; remote has probably disconnected");
                } else {
                    LMQ_LOG(warn, "Unable to send message to ", conn_id, ": ", e.what());
                }
                nowarn = true;
                if (callback_nosend) {
                    job([callback = std::move(callback_nosend), error = e] { callback(&error); });
                    callback_nosend = nullptr;
                }
            }
        }
    }
    if (request) {
        if (sent) {
            LMQ_LOG(debug, "Added new pending request ", to_hex(request_tag));
            pending_requests.insert({ request_tag, {
                std::chrono::steady_clock::now() + request_timeout, std::move(request_callback) }});
        } else {
            LMQ_LOG(debug, "Could not send request, scheduling request callback failure");
            job([callback = std::move(request_callback)] { callback(false, {{"TIMEOUT"s}}); });
        }
    }
    if (!sent) {
        if (callback_nosend)
            job([callback = std::move(callback_nosend)] { callback(nullptr); });
        else if (callback_noqueue)
            job(std::move(callback_noqueue));
        else if (!nowarn)
            LMQ_LOG(warn, "Unable to send message to ", conn_id, ": sending would block");
    }
}

void OxenMQ::proxy_reply(bt_dict_consumer data) {
    bool have_conn_id = false;
    ConnectionID conn_id{0};
    if (data.skip_until("conn_id")) {
        conn_id.id = data.consume_integer<long long>();
        if (conn_id.id == -1)
            throw std::runtime_error("Invalid error: invalid conn_id value (-1)");
        have_conn_id = true;
    }
    if (data.skip_until("conn_pubkey")) {
        if (have_conn_id)
            throw std::runtime_error("Internal error: Invalid proxy reply command; conn_id and conn_pubkey are exclusive");
        conn_id.pk = data.consume_string();
        conn_id.id = ConnectionID::SN_ID;
    } else if (!have_conn_id)
        throw std::runtime_error("Internal error: Invalid proxy reply command; conn_pubkey or conn_id missing");
    if (!data.skip_until("send"))
        throw std::runtime_error("Internal error: Invalid proxy reply command; send parts missing");

    bt_list_consumer send = data.consume_list_consumer();

    auto pr = peers.equal_range(conn_id);
    if (pr.first == pr.second) {
        LMQ_LOG(warn, "Unable to send tagged reply: the connection is no longer valid");
        return;
    }

    // We try any connections until one works (for ordinary remotes there will be just one, but for
    // SNs there might be one incoming and one outgoing).
    for (auto it = pr.first; it != pr.second; ) {
        try {
            send_message_parts(connections[it->second.conn_id], build_send_parts(send, it->second.route));
            break;
        } catch (const zmq::error_t &err) {
            if (err.num() == EHOSTUNREACH) {
                if (it->second.outgoing()) {
                    LMQ_LOG(debug, "Unable to send reply to non-SN request on outgoing socket: "
                            "remote is no longer connected; closing connection");
                    proxy_close_connection(it->second.conn_id, CLOSE_LINGER);
                    it = peers.erase(it);
                    ++it;
                } else {
                    LMQ_LOG(debug, "Unable to send reply to non-SN request on incoming socket: "
                            "remote is no longer connected; removing peer details");
                    it = peers.erase(it);
                }
            } else {
                LMQ_LOG(warn, "Unable to send reply to incoming non-SN request: ", err.what());
                ++it;
            }
        }
    }
}

void OxenMQ::proxy_control_message(std::vector<zmq::message_t>& parts) {
    // We throw an uncaught exception here because we only generate control messages internally in
    // oxenmq code: if one of these condition fail it's a oxenmq bug.
    if (parts.size() < 2)
        throw std::logic_error("OxenMQ bug: Expected 2-3 message parts for a proxy control message");
    auto route = view(parts[0]), cmd = view(parts[1]);
    LMQ_TRACE("control message: ", cmd);
    if (parts.size() == 3) {
        LMQ_TRACE("...: ", parts[2]);
        auto data = view(parts[2]);
        if (cmd == "SEND") {
            LMQ_TRACE("proxying message");
            return proxy_send(data);
        } else if (cmd == "REPLY") {
            LMQ_TRACE("proxying reply to non-SN incoming message");
            return proxy_reply(data);
        } else if (cmd == "BATCH") {
            LMQ_TRACE("proxy batch jobs");
            auto ptrval = bt_deserialize<uintptr_t>(data);
            return proxy_batch(reinterpret_cast<detail::Batch*>(ptrval));
        } else if (cmd == "INJECT") {
            LMQ_TRACE("proxy inject");
            return proxy_inject_task(detail::deserialize_object<injected_task>(bt_deserialize<uintptr_t>(data)));
        } else if (cmd == "SET_SNS") {
            return proxy_set_active_sns(data);
        } else if (cmd == "UPDATE_SNS") {
            return proxy_update_active_sns(data);
        } else if (cmd == "CONNECT_SN") {
            proxy_connect_sn(data);
            return;
        } else if (cmd == "CONNECT_REMOTE") {
            return proxy_connect_remote(data);
        } else if (cmd == "DISCONNECT") {
            return proxy_disconnect(data);
        } else if (cmd == "TIMER") {
            return proxy_timer(data);
        } else if (cmd == "TIMER_DEL") {
            return proxy_timer_del(bt_deserialize<int>(data));
        } else if (cmd == "BIND") {
            auto b = detail::deserialize_object<bind_data>(bt_deserialize<uintptr_t>(data));
            if (proxy_bind(b, bind.size()))
                bind.push_back(std::move(b));
            return;
        }
    } else if (parts.size() == 2) {
        if (cmd == "START") {
            // Command send by the owning thread during startup; we send back a simple READY reply to
            // let it know we are running.
            return route_control(command, route, "READY");
        } else if (cmd == "QUIT") {
            // Asked to quit: set max_workers to zero and tell any idle ones to quit.  We will
            // close workers as they come back to READY status, and then close external
            // connections once all workers are done.
            max_workers = 0;
            for (const auto &route : idle_workers)
                route_control(workers_socket, workers[route].worker_routing_id, "QUIT");
            idle_workers.clear();
            for (auto& [run, busy, queue] : tagged_workers)
                if (!busy)
                    route_control(workers_socket, run.worker_routing_id, "QUIT");
            return;
        }
    }
    throw std::runtime_error("OxenMQ bug: Proxy received invalid control command: " +
            std::string{cmd} + " (" + std::to_string(parts.size()) + ")");
}

bool OxenMQ::proxy_bind(bind_data& b, size_t bind_index) {
    zmq::socket_t listener{context, zmq::socket_type::router};
    setup_incoming_socket(listener, b.curve, pubkey, privkey, bind_index);

    bool good = true;
    try {
        listener.bind(b.address);
    } catch (const zmq::error_t&) {
        good = false;
    }
    if (b.on_bind) {
        b.on_bind(good);
        b.on_bind = nullptr;
    }
    if (!good) {
        LMQ_LOG(warn, "OxenMQ failed to listen on ", b.address);
        return false;
    }

    LMQ_LOG(info, "OxenMQ listening on ", b.address);

    b.conn_id = next_conn_id++;
    connections.emplace_hint(connections.end(), b.conn_id, std::move(listener));

    connections_updated = true;

    return true;
}

void OxenMQ::proxy_loop() {

#if defined(__linux__) || defined(__sun) || defined(__MINGW32__)
    pthread_setname_np(pthread_self(), "lmq-proxy");
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), "lmq-proxy");
#elif defined(__MACH__)
    pthread_setname_np("lmq-proxy");
#endif

    zap_auth.set(zmq::sockopt::linger, 0);
    zap_auth.bind(ZMQ_ADDR_ZAP);

    workers_socket.set(zmq::sockopt::router_mandatory, true);
    workers_socket.bind(SN_ADDR_WORKERS);

    assert(general_workers > 0);
    if (batch_jobs_reserved < 0)
        batch_jobs_reserved = (general_workers + 1) / 2;
    if (reply_jobs_reserved < 0)
        reply_jobs_reserved = (general_workers + 7) / 8;

    max_workers = general_workers + batch_jobs_reserved + reply_jobs_reserved;
    for (const auto& cat : categories) {
        max_workers += cat.second.reserved_threads;
    }

    if (log_level() >= LogLevel::debug) {
        LMQ_LOG(debug, "Reserving space for ", max_workers, " max workers = ", general_workers, " general plus reservations for:");
        for (const auto& cat : categories)
            LMQ_LOG(debug, "    - ", cat.first, ": ", cat.second.reserved_threads);
        LMQ_LOG(debug, "    - (batch jobs): ", batch_jobs_reserved);
        LMQ_LOG(debug, "    - (reply jobs): ", reply_jobs_reserved);
        LMQ_LOG(debug, "Plus ", tagged_workers.size(), " tagged worker threads");
    }

    workers.reserve(max_workers);
    if (!workers.empty())
        throw std::logic_error("Internal error: proxy thread started with active worker threads");

#ifndef _WIN32
    int saved_umask = -1;
    if (STARTUP_UMASK >= 0)
        saved_umask = umask(STARTUP_UMASK);
#endif

    for (size_t i = 0; i < bind.size(); i++) {
        if (!proxy_bind(bind[i], i)) {
            LMQ_LOG(warn, "OxenMQ failed to listen on ", bind[i].address);
            throw zmq::error_t{};
        }
    }

#ifndef _WIN32
    if (saved_umask != -1)
        umask(saved_umask);

    // set socket gid / uid if it is provided
    if (SOCKET_GID != -1 or SOCKET_UID != -1) {
        for (auto& b : bind) {
            const address addr(b.address);
            if (addr.ipc())
                if (chown(addr.socket.c_str(), SOCKET_UID, SOCKET_GID) == -1)
                    throw std::runtime_error("cannot set group on " + addr.socket + ": " + strerror(errno));
        }
    }
#endif

    connections_updated = true;

    // Also add an internal connection to self so that calling code can avoid needing to
    // special-case rare situations where we are supposed to talk to a quorum member that happens to
    // be ourselves (which can happen, for example, with cross-quoum Blink communication)
    // FIXME: not working
    //listener.bind(SN_ADDR_SELF);

    if (!timers)
        timers.reset(zmq_timers_new());

    auto do_conn_cleanup = [this] { proxy_conn_cleanup(); };
    using CleanupLambda = decltype(do_conn_cleanup);
    if (-1 == zmq_timers_add(timers.get(),
            std::chrono::milliseconds{CONN_CHECK_INTERVAL}.count(),
            // Wrap our lambda into a C function pointer where we pass in the lambda pointer as extra arg
            [](int /*timer_id*/, void* cleanup) { (*static_cast<CleanupLambda*>(cleanup))(); },
            &do_conn_cleanup)) {
        throw zmq::error_t{};
    }

    std::vector<zmq::message_t> parts;

    // Wait for tagged worker threads to get ready and connect to us (we get a "STARTING" message)
    // and send them back a "START" to let them know to go ahead with startup.  We need this
    // synchronization dance to guarantee that the workers are routable before we can proceed.
    if (!tagged_workers.empty()) {
        LMQ_LOG(debug, "Waiting for tagged workers");
        std::unordered_set<std::string_view> waiting_on;
        for (auto& w : tagged_workers)
            waiting_on.emplace(std::get<run_info>(w).worker_routing_id);
        for (; !waiting_on.empty(); parts.clear()) {
            recv_message_parts(workers_socket, parts);
            if (parts.size() != 2 || view(parts[1]) != "STARTING"sv) {
                LMQ_LOG(error, "Received invalid message on worker socket while waiting for tagged thread startup");
                continue;
            }
            LMQ_LOG(debug, "Received STARTING message from ", view(parts[0]));
            if (auto it = waiting_on.find(view(parts[0])); it != waiting_on.end())
                waiting_on.erase(it);
            else
                LMQ_LOG(error, "Received STARTING message from unknown worker ", view(parts[0]));
        }

        for (auto&w : tagged_workers) {
            LMQ_LOG(debug, "Telling tagged thread worker ", std::get<run_info>(w).worker_routing_id, " to finish startup");
            route_control(workers_socket, std::get<run_info>(w).worker_routing_id, "START");
        }
    }

    while (true) {
        std::chrono::milliseconds poll_timeout;
        if (max_workers == 0) { // Will be 0 only if we are quitting
            if (std::none_of(workers.begin(), workers.end(), [](auto &w) { return w.worker_thread.joinable(); }) &&
                    std::none_of(tagged_workers.begin(), tagged_workers.end(), [](auto &w) { return std::get<0>(w).worker_thread.joinable(); })) {
                // All the workers have finished, so we can finish shutting down
                return proxy_quit();
            }
            poll_timeout = 1s; // We don't keep running timers when we're quitting, so don't have a timer to check
        } else {
            poll_timeout = std::chrono::milliseconds{zmq_timers_timeout(timers.get())};
        }

        if (connections_updated)
            rebuild_pollitems();

        if (proxy_skip_one_poll)
            proxy_skip_one_poll = false;
        else {
            LMQ_TRACE("polling for new messages");

            // We poll the control socket and worker socket for any incoming messages.  If we have
            // available worker room then also poll incoming connections and outgoing connections
            // for messages to forward to a worker.  Otherwise, we just look for a control message
            // or a worker coming back with a ready message.
            zmq::poll(pollitems.data(), pollitems.size(), poll_timeout);
        }

        LMQ_TRACE("processing control messages");
        // Retrieve any waiting incoming control messages
        for (parts.clear(); recv_message_parts(command, parts, zmq::recv_flags::dontwait); parts.clear()) {
            proxy_control_message(parts);
        }

        LMQ_TRACE("processing worker messages");
        for (parts.clear(); recv_message_parts(workers_socket, parts, zmq::recv_flags::dontwait); parts.clear()) {
            proxy_worker_message(parts);
        }

        LMQ_TRACE("processing timers");
        zmq_timers_execute(timers.get());

        // Handle any zap authentication
        LMQ_TRACE("processing zap requests");
        process_zap_requests();

        // See if we can drain anything from the current queue before we potentially add to it
        // below.
        LMQ_TRACE("processing queued jobs and messages");
        proxy_process_queue();

        LMQ_TRACE("processing new incoming messages");

        // We round-robin connections when pulling off pending messages one-by-one rather than
        // pulling off all messages from one connection before moving to the next; thus in cases of
        // contention we end up fairly distributing.
        std::vector<std::pair<const int64_t, zmq::socket_t>*> queue; // Used as a circular buffer
        queue.reserve(connections.size() + 1);
        for (auto& id_sock : connections)
            queue.push_back(&id_sock);
        queue.push_back(nullptr);
        size_t end = queue.size() - 1;

        for (size_t pos = 0; pos != end; ++pos %= queue.size()) {
            parts.clear();
            auto& [id, sock] = *queue[pos];

            if (!recv_message_parts(sock, parts, zmq::recv_flags::dontwait))
                continue;

            // We only pull this one message now but then requeue the socket so that after we check
            // all other sockets we come back to this one to check again.
            queue[end] = queue[pos];
            ++end %= queue.size();

            if (parts.empty()) {
                LMQ_LOG(warn, "Ignoring empty (0-part) incoming message");
                continue;
            }

            if (!proxy_handle_builtin(id, sock, parts))
                proxy_to_worker(id, sock, parts);

            if (connections_updated) {
                // If connections got updated then our points are stale, to restart the proxy loop;
                // if there are still messages waiting we'll end up right back here.
                LMQ_TRACE("connections became stale; short-circuiting incoming message loop");
                break;
            }
        }

        LMQ_TRACE("done proxy loop");
    }
}

static bool is_error_response(std::string_view cmd) {
    return cmd == "FORBIDDEN" || cmd == "FORBIDDEN_SN" || cmd == "NOT_A_SERVICE_NODE" || cmd == "UNKNOWNCOMMAND" || cmd == "NO_REPLY_TAG";
}

// Return true if we recognized/handled the builtin command (even if we reject it for whatever
// reason)
bool OxenMQ::proxy_handle_builtin(int64_t conn_id, zmq::socket_t& sock, std::vector<zmq::message_t>& parts) {
    // Doubling as a bool and an offset:
    size_t incoming = sock.get(zmq::sockopt::type) == ZMQ_ROUTER;

    std::string_view route, cmd;
    if (parts.size() < 1 + incoming) {
        LMQ_LOG(warn, "Received empty message; ignoring");
        return true;
    }
    if (incoming) {
        route = view(parts[0]);
        cmd = view(parts[1]);
    } else {
        cmd = view(parts[0]);
    }
    LMQ_TRACE("Checking for builtins: '", cmd, "' from ", peer_address(parts.back()));

    if (cmd == "REPLY") {
        size_t tag_pos = 1 + incoming;
        if (parts.size() <= tag_pos) {
            LMQ_LOG(warn, "Received REPLY without a reply tag; ignoring");
            return true;
        }
        std::string reply_tag{view(parts[tag_pos])};
        auto it = pending_requests.find(reply_tag);
        if (it != pending_requests.end()) {
            LMQ_LOG(debug, "Received REPLY for pending command ", to_hex(reply_tag), "; scheduling callback");
            std::vector<std::string> data;
            data.reserve(parts.size() - (tag_pos + 1));
            for (auto it = parts.begin() + (tag_pos + 1); it != parts.end(); ++it)
                data.emplace_back(view(*it));
            proxy_schedule_reply_job([callback=std::move(it->second.second), data=std::move(data)] {
                    callback(true, std::move(data));
            });
            pending_requests.erase(it);
        } else {
            LMQ_LOG(warn, "Received REPLY with unknown or already handled reply tag (", to_hex(reply_tag), "); ignoring");
        }
        return true;
    } else if (cmd == "HI") {
        if (!incoming) {
            LMQ_LOG(warn, "Got invalid 'HI' message on an outgoing connection; ignoring");
            return true;
        }
        LMQ_LOG(debug, "Incoming client from ", peer_address(parts.back()), " sent HI, replying with HELLO");
        try {
            send_routed_message(sock, std::string{route}, "HELLO");
        } catch (const std::exception &e) { LMQ_LOG(warn, "Couldn't reply with HELLO: ", e.what()); }
        return true;
    } else if (cmd == "HELLO") {
        if (incoming) {
            LMQ_LOG(warn, "Got invalid 'HELLO' message on an incoming connection; ignoring");
            return true;
        }
        auto it = std::find_if(pending_connects.begin(), pending_connects.end(),
                [&](auto& pc) { return std::get<int64_t>(pc) == conn_id; });
        if (it == pending_connects.end()) {
            LMQ_LOG(warn, "Got invalid 'HELLO' message on an already handshaked incoming connection; ignoring");
            return true;
        }
        auto& pc = *it;
        auto pit = peers.find(std::get<int64_t>(pc));
        if (pit == peers.end()) {
            LMQ_LOG(warn, "Got invalid 'HELLO' message with invalid conn_id; ignoring");
            return true;
        }

        LMQ_LOG(debug, "Got initial HELLO server response from ", peer_address(parts.back()));
        proxy_schedule_reply_job([on_success=std::move(std::get<ConnectSuccess>(pc)),
                conn=pit->first] {
            on_success(conn);
        });
        pending_connects.erase(it);
        return true;
    } else if (cmd == "BYE") {
        if (!incoming) {
            LMQ_LOG(debug, "BYE command received; disconnecting from ", peer_address(parts.back()));
            proxy_close_connection(conn_id, 0s);
        } else {
            LMQ_LOG(warn, "Got invalid 'BYE' command on an incoming socket; ignoring");
        }

        return true;
    }
    else if (is_error_response(cmd)) {
        // These messages (FORBIDDEN, UNKNOWNCOMMAND, etc.) are sent in response to us trying to
        // invoke something that doesn't exist or we don't have permission to access.  These have
        // two forms (the latter is only sent by remotes running 1.1.0+).
        // - ["XXX", "whatever.command"]
        // - ["XXX", "REPLY", replytag]
        // (ignoring the routing prefix on incoming commands).
        // For the former, we log; for the latter we trigger the reply callback with a failure

        if (parts.size() == (1 + incoming) && cmd == "UNKNOWNCOMMAND") {
            // pre-1.1.0 sent just a plain UNKNOWNCOMMAND (without the actual command); this was not
            // useful, but also this response is *expected* for things 1.0.5 didn't understand, like
            // FORBIDDEN_SN: so log it only at debug level and move on.
            LMQ_LOG(debug, "Received plain UNKNOWNCOMMAND; remote is probably an older oxenmq. Ignoring.");
            return true;
        }

        if (parts.size() == (3 + incoming) && view(parts[1 + incoming]) == "REPLY") {
            std::string reply_tag{view(parts[2 + incoming])};
            auto it = pending_requests.find(reply_tag);
            if (it != pending_requests.end()) {
                LMQ_LOG(debug, "Received ", cmd, " REPLY for pending command ", to_hex(reply_tag), "; scheduling failure callback");
                proxy_schedule_reply_job([callback=std::move(it->second.second), cmd=std::string{cmd}] {
                    callback(false, {{std::move(cmd)}});
                });
                pending_requests.erase(it);
            } else {
                LMQ_LOG(warn, "Received REPLY with unknown or already handled reply tag (", to_hex(reply_tag), "); ignoring");
            }
        } else {
            LMQ_LOG(warn, "Received ", cmd, ':', (parts.size() > 1 + incoming ? view(parts[1 + incoming]) : "(unknown command)"sv),
                        " from ", peer_address(parts.back()));
        }
        return true;
    }
    return false;
}

void OxenMQ::proxy_process_queue() {
    if (max_workers == 0) // shutting down
        return;

    // First: send any tagged thread tasks to the tagged threads, if idle
    for (auto& [run, busy, queue] : tagged_workers) {
        if (!busy && !queue.empty()) {
            busy = true;
            proxy_run_worker(run.load(std::move(queue.front()), false, run.worker_id));
            queue.pop();
        }
    }

    // Second: process any batch jobs; since these are internal they are given higher priority.
    proxy_run_batch_jobs(batch_jobs, batch_jobs_reserved, batch_jobs_active, false);

    // Next any reply batch jobs (which are a bit different from the above, since they are
    // externally triggered but for things we initiated locally).
    proxy_run_batch_jobs(reply_jobs, reply_jobs_reserved, reply_jobs_active, true);

    // Finally general incoming commands
    for (auto it = pending_commands.begin(); it != pending_commands.end() && active_workers() < max_workers; ) {
        auto& pending = *it;
        if (pending.cat.active_threads < pending.cat.reserved_threads
                || active_workers() < general_workers) {
            proxy_run_worker(get_idle_worker().load(std::move(pending)));
            pending.cat.queued--;
            pending.cat.active_threads++;
            assert(pending.cat.queued >= 0);
            it = pending_commands.erase(it);
        } else {
            ++it; // no available general or reserved worker spots for this job right now
        }
    }
}

}
