#include "lokimq.h"
#include "lokimq-internal.h"
#include "hex.h"

namespace lokimq {

void LokiMQ::proxy_quit() {
    LMQ_LOG(debug, "Received quit command, shutting down proxy thread");

    assert(std::none_of(workers.begin(), workers.end(), [](auto& worker) { return worker.worker_thread.joinable(); }));

    command.setsockopt<int>(ZMQ_LINGER, 0);
    command.close();
    {
        std::lock_guard<std::mutex> lock{control_sockets_mutex};
        for (auto &control : thread_control_sockets)
            control->close();
        proxy_shutting_down = true; // To prevent threads from opening new control sockets
    }
    workers_socket.close();
    int linger = std::chrono::milliseconds{CLOSE_LINGER}.count();
    for (auto& s : connections)
        s.setsockopt(ZMQ_LINGER, linger);
    connections.clear();
    peers.clear();

    LMQ_LOG(debug, "Proxy thread teardown complete");
}

void LokiMQ::proxy_send(bt_dict_consumer data) {
    // NB: bt_dict_consumer goes in alphabetical order
    string_view hint;
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
    bool retry = true, sent = false, warned = false;
    std::unique_ptr<zmq::error_t> send_error;
    while (retry) {
        retry = false;
        zmq::socket_t *send_to;
        if (conn_id.sn()) {
            auto sock_route = proxy_connect_sn(conn_id.pk, hint, optional, incoming, outgoing, keep_alive);
            if (!sock_route.first) {
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
            auto it = incoming_conn_index.find(conn_id);
            if (it == incoming_conn_index.end()) {
                LMQ_LOG(warn, "Unable to send to ", conn_id, ": incoming listening socket not found");
                break;
            }
            send_to = &connections[it->second];
        } else {
            auto pr = peers.equal_range(conn_id);
            if (pr.first == peers.end()) {
                LMQ_LOG(warn, "Unable to send: connection id ", conn_id, " is not (or is no longer) a valid outgoing connection");
                break;
            }
            auto& peer = pr.first->second;
            send_to = &connections[peer.conn_index];
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
                LMQ_LOG(warn, "Unable to send message to ", conn_id, ": ", e.what());
                warned = true;
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
        else if (!warned)
            LMQ_LOG(warn, "Unable to send message to ", conn_id, ": sending would block");
    }
}

void LokiMQ::proxy_reply(bt_dict_consumer data) {
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
            send_message_parts(connections[it->second.conn_index], build_send_parts(send, it->second.route));
            break;
        } catch (const zmq::error_t &err) {
            if (err.num() == EHOSTUNREACH) {
                LMQ_LOG(debug, "Unable to send reply to incoming non-SN request: remote is no longer connected; removing peer details");
                it = peers.erase(it);
            } else {
                LMQ_LOG(warn, "Unable to send reply to incoming non-SN request: ", err.what());
                ++it;
            }
        }
    }
}

void LokiMQ::proxy_control_message(std::vector<zmq::message_t>& parts) {
    // We throw an uncaught exception here because we only generate control messages internally in
    // lokimq code: if one of these condition fail it's a lokimq bug.
    if (parts.size() < 2)
        throw std::logic_error("LokiMQ bug: Expected 2-3 message parts for a proxy control message");
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
            return;
        }
    }
    throw std::runtime_error("LokiMQ bug: Proxy received invalid control command: " +
            std::string{cmd} + " (" + std::to_string(parts.size()) + ")");
}

void LokiMQ::proxy_loop() {

#if defined(__linux__) || defined(__sun) || defined(__MINGW32__)
    pthread_setname_np(pthread_self(), "lmq-proxy");
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), "lmq-proxy");
#elif defined(__MACH__)
    pthread_setname_np("lmq-proxy");
#endif

    zap_auth.setsockopt<int>(ZMQ_LINGER, 0);
    zap_auth.bind(ZMQ_ADDR_ZAP);

    workers_socket.setsockopt<int>(ZMQ_ROUTER_MANDATORY, 1);
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
    }

    workers.reserve(max_workers);
    if (!workers.empty())
        throw std::logic_error("Internal error: proxy thread started with active worker threads");

    for (size_t i = 0; i < bind.size(); i++) {
        auto& b = bind[i].second;
        zmq::socket_t listener{context, zmq::socket_type::router};

        std::string auth_domain = bt_serialize(i);
        setup_external_socket(listener);
        listener.setsockopt(ZMQ_ZAP_DOMAIN, auth_domain.c_str(), auth_domain.size());
        if (b.curve) {
            listener.setsockopt<int>(ZMQ_CURVE_SERVER, 1);
            listener.setsockopt(ZMQ_CURVE_PUBLICKEY, pubkey.data(), pubkey.size());
            listener.setsockopt(ZMQ_CURVE_SECRETKEY, privkey.data(), privkey.size());
        }
        listener.setsockopt<int>(ZMQ_ROUTER_HANDOVER, 1);
        listener.setsockopt<int>(ZMQ_ROUTER_MANDATORY, 1);

        listener.bind(bind[i].first);
        LMQ_LOG(info, "LokiMQ listening on ", bind[i].first);

        connections.push_back(std::move(listener));
        auto conn_id = next_conn_id++;
        conn_index_to_id.push_back(conn_id);
        incoming_conn_index[conn_id] = connections.size() - 1;
        b.index = connections.size() - 1;
    }
    pollitems_stale = true;

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

    while (true) {
        std::chrono::milliseconds poll_timeout;
        if (max_workers == 0) { // Will be 0 only if we are quitting
            if (std::none_of(workers.begin(), workers.end(), [](auto &w) { return w.worker_thread.joinable(); })) {
                // All the workers have finished, so we can finish shutting down
                return proxy_quit();
            }
            poll_timeout = 1s; // We don't keep running timers when we're quitting, so don't have a timer to check
        } else {
            poll_timeout = std::chrono::milliseconds{zmq_timers_timeout(timers.get())};
        }

        if (proxy_skip_one_poll)
            proxy_skip_one_poll = false;
        else {
            LMQ_TRACE("polling for new messages");

            if (pollitems_stale)
                rebuild_pollitems();

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
        const int num_sockets = connections.size();
        std::queue<int> queue_index;
        for (int i = 0; i < num_sockets; i++)
            queue_index.push(i);

        for (parts.clear(); !queue_index.empty(); parts.clear()) {
            size_t i = queue_index.front();
            queue_index.pop();
            auto& sock = connections[i];

            if (!recv_message_parts(sock, parts, zmq::recv_flags::dontwait))
                continue;

            // We only pull this one message now but then requeue the socket so that after we check
            // all other sockets we come back to this one to check again.
            queue_index.push(i);

            if (parts.empty()) {
                LMQ_LOG(warn, "Ignoring empty (0-part) incoming message");
                continue;
            }

            if (!proxy_handle_builtin(i, parts))
                proxy_to_worker(i, parts);

            if (pollitems_stale) {
                // If our items became stale then we may have just closed a connection and so our
                // queue index maybe also be stale, so restart the proxy loop (so that we rebuild
                // pollitems).
                LMQ_TRACE("pollitems became stale; short-circuiting incoming message loop");
                break;
            }
        }

        LMQ_TRACE("done proxy loop");
    }
}

static bool is_error_response(string_view cmd) {
    return cmd == "FORBIDDEN" || cmd == "FORBIDDEN_SN" || cmd == "NOT_A_SERVICE_NODE" || cmd == "UNKNOWNCOMMAND" || cmd == "NO_REPLY_TAG";
}

// Return true if we recognized/handled the builtin command (even if we reject it for whatever
// reason)
bool LokiMQ::proxy_handle_builtin(size_t conn_index, std::vector<zmq::message_t>& parts) {
    // Doubling as a bool and an offset:
    size_t incoming = connections[conn_index].getsockopt<int>(ZMQ_TYPE) == ZMQ_ROUTER;

    string_view route, cmd;
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
            send_routed_message(connections[conn_index], std::string{route}, "HELLO");
        } catch (const std::exception &e) { LMQ_LOG(warn, "Couldn't reply with HELLO: ", e.what()); }
        return true;
    } else if (cmd == "HELLO") {
        if (incoming) {
            LMQ_LOG(warn, "Got invalid 'HELLO' message on an incoming connection; ignoring");
            return true;
        }
        auto it = std::find_if(pending_connects.begin(), pending_connects.end(),
                [&](auto& pc) { return std::get<size_t>(pc) == conn_index; });
        if (it == pending_connects.end()) {
            LMQ_LOG(warn, "Got invalid 'HELLO' message on an already handshaked incoming connection; ignoring");
            return true;
        }
        auto& pc = *it;
        auto pit = peers.find(std::get<long long>(pc));
        if (pit == peers.end()) {
            LMQ_LOG(warn, "Got invalid 'HELLO' message with invalid conn_id; ignoring");
            return true;
        }

        LMQ_LOG(debug, "Got initial HELLO server response from ", peer_address(parts.back()));
        proxy_schedule_reply_job([on_success=std::move(std::get<ConnectSuccess>(pc)),
                conn=conn_index_to_id[conn_index]] {
            on_success(conn);
        });
        pending_connects.erase(it);
        return true;
    } else if (cmd == "BYE") {
        if (!incoming) {
            LMQ_LOG(debug, "BYE command received; disconnecting from ", peer_address(parts.back()));
            proxy_close_connection(conn_index, 0s);
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
            LMQ_LOG(debug, "Received plain UNKNOWNCOMMAND; remote is probably an older lokimq. Ignoring.");
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
            LMQ_LOG(warn, "Received ", cmd, ':', (parts.size() > 1 + incoming ? view(parts[1 + incoming]) : "(unknown command)"_sv),
                        " from ", peer_address(parts.back()));
        }
        return true;
    }
    return false;
}

void LokiMQ::proxy_process_queue() {
    // First up: process any batch jobs; since these are internal they are given higher priority.
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
