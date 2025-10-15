#include "oxenmq.h"
#include "batch.h"
#include "oxenmq-internal.h"

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
extern "C" {
#include <pthread.h>
#include <pthread_np.h>
}
#endif
#include <variant>

namespace oxenmq {

namespace {

auto cat = log::Cat("oxenmq");

// Waits for a specific command or "QUIT" on the given socket.  Returns true if the command was
// received.  If "QUIT" was received, replies with "QUITTING" on the socket and closes it, then
// returns false.
[[gnu::always_inline]] inline
bool worker_wait_for(zmq::socket_t& sock, std::vector<zmq::message_t>& parts, const std::string_view worker_id, const std::string_view expect) {
    while (true) {
        log::trace(cat, "worker {} waiting for {}", worker_id, expect);
        parts.clear();
        recv_message_parts(sock, parts);
        if (parts.size() != 1) {
            log::error(cat, "Internal error: worker {} received invalid {}-part control msg", worker_id, parts.size());
            continue;
        }
        auto command = view(parts[0]);
        if (command == expect) {
            log::trace(cat, "Worker {} received waited-for {} command", worker_id, expect);
            return true;
        } else if (command == "QUIT"sv) {
            log::debug(cat, "Worker {} received QUIT command, shutting down", worker_id);
            detail::send_control(sock, "QUITTING");
            sock.set(zmq::sockopt::linger, 1000);
            sock.close();
            return false;
        } else {
            log::error(cat, "Internal error: worker {} received invalid command: '{}'", worker_id, command);
        }
    }
}

}

void OxenMQ::worker_thread(unsigned int index, std::optional<std::string> tagged, std::function<void()> start) {
    std::string routing_id = (tagged ? "t" : "w") +
        std::string(reinterpret_cast<const char*>(&index), sizeof(index)); // for routing
    std::string worker_id{tagged ? *tagged : fmt::format("w{}", index)}; // for debug

    [[maybe_unused]] std::string thread_name = tagged.value_or("omq-" + worker_id);
#if defined(__linux__) || defined(__sun) || defined(__MINGW32__)
    if (thread_name.size() > 15) thread_name.resize(15);
    pthread_setname_np(pthread_self(), thread_name.c_str());
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), thread_name.c_str());
#elif defined(__MACH__)
    pthread_setname_np(thread_name.c_str());
#endif

    std::optional<zmq::socket_t> tagged_socket;
    if (tagged) {
        // If we're a tagged worker then we got started up before OxenMQ started, so we need to wait
        // for an all-clear signal from OxenMQ first, then we fire our `start` callback, then we can
        // start waiting for commands in the main loop further down.  (We also can't get the
        // reference to our `tagged_workers` element or create a socket until the main proxy thread
        // is running).
        {
            std::unique_lock lock{tagged_startup_mutex};
            tagged_cv.wait(lock, [this] { return tagged_go != tagged_go_mode::WAIT; });
        }
        if (tagged_go == tagged_go_mode::SHUTDOWN) // OxenMQ destroyed without starting
            return;
        tagged_socket.emplace(context, zmq::socket_type::dealer);
    }
    auto& sock = tagged ? *tagged_socket : worker_sockets[index];
    sock.set(zmq::sockopt::routing_id, routing_id);
    log::debug(cat, "New worker thread {} started", worker_id);
    sock.connect(SN_ADDR_WORKERS);
    if (tagged)
        detail::send_control(sock, "STARTING");

    Message message{*this, 0, AuthLevel::none, ""s};
    std::vector<zmq::message_t> parts;

    bool waiting_for_command;
    if (tagged) {
        waiting_for_command = true;

        if (!worker_wait_for(sock, parts, worker_id, "START"sv))
            return;
        if (start) start();
    } else {
        // Otherwise for a regular worker we can only be started by an active main proxy thread
        // which will have preloaded our first job so we can start off right away.
        waiting_for_command = false;
    }

    // This will always contains the current job, and is guaranteed to never be invalidated.
    run_info& run = tagged ? std::get<run_info>(tagged_workers[index - 1]) : workers[index];

    while (true) {
        if (waiting_for_command) {
            if (!worker_wait_for(sock, parts, worker_id, "RUN"sv))
                return;
        }

        try {
            if (run.is_batch_job) {
                auto* batch = std::get<detail::Batch*>(run.to_run);
                if (run.batch_jobno >= 0) {
                    log::trace(cat, "worker thread {} running batch @ {} # {}", worker_id, (void*)batch, run.batch_jobno);
                    batch->run_job(run.batch_jobno);
                } else if (run.batch_jobno == -1) {
                    log::trace(cat, "worker thread {} running batch @ {} completion", worker_id, (void*)batch);
                    batch->job_completion();
                }
            } else if (run.is_injected) {
                auto& func = std::get<std::function<void()>>(run.to_run);
                log::trace(cat, "worker thread {} invoking injected command {}", worker_id, run.command);
                func();
                func = nullptr;
            } else {
                message.conn = run.conn;
                message.access = run.access;
                message.remote = std::move(run.remote);
                message.data.clear();

                log::trace(cat, "Got incoming command from {}/{} ({})", message.remote, message.conn, message.conn.route.empty() ? "outgoing" : "incoming");

                auto& [callback, is_request] = *std::get<const std::pair<CommandCallback, bool>*>(run.to_run);
                if (is_request) {
                    message.reply_tag = {run.data_parts[0].data<char>(), run.data_parts[0].size()};
                    for (auto it = run.data_parts.begin() + 1; it != run.data_parts.end(); ++it)
                        message.data.emplace_back(it->data<char>(), it->size());
                } else {
                    for (auto& m : run.data_parts)
                        message.data.emplace_back(m.data<char>(), m.size());
                }

                log::trace(cat, "worker thread {} invoking {} callback with {} message parts", worker_id, run.command, message.data.size());
                callback(message);
            }
        }
        catch (const oxenc::bt_deserialize_invalid& e) {
            log::warning(cat, "{} deserialization failed: {}; ignoring request", worker_id, e.what());
        }
#ifndef BROKEN_APPLE_VARIANT
        catch (const std::bad_variant_access& e) {
            log::warning(cat, "{} deserialization failed: found unexpected serialized type ({}); ignoring request", worker_id, e.what());
        }
#endif
        catch (const std::out_of_range& e) {
            log::warning(cat, "{} deserialization failed: invalid data - required field missing ({}); ignoring request", worker_id, e.what());
        }
        catch (const std::exception& e) {
            log::warning(cat, "{} caught exception when processing command: {}", worker_id, e.what());
        }
        catch (...) {
            log::warning(cat, "{} caught non-standard exception when processing command", worker_id);
        }

        // Tell the proxy thread that we are ready for another job
        detail::send_control(sock, "RAN");
        waiting_for_command = true;
    }
}


OxenMQ::run_info& OxenMQ::get_idle_worker() {
    if (idle_worker_count == 0) {
        uint32_t id = workers.size();
        workers.emplace_back();
        auto& r = workers.back();
        r.worker_id = id;
        r.worker_routing_id = "w" + std::string(reinterpret_cast<const char*>(&id), sizeof(id));
        r.worker_routing_name = "w" + std::to_string(id);
        return r;
    }
    size_t id = idle_workers[--idle_worker_count];
    return workers[id];
}

void OxenMQ::proxy_worker_message(OxenMQ::control_message_array& parts, size_t len) {
    // Process messages sent by workers
    if (len != 2) {
        log::error(cat, "Received send invalid {}-part message", len);
        return;
    }
    auto route_raw = view(parts[0]), cmd = view(parts[1]);
    if (route_raw.size() != 5 || (route_raw[0] != 'w' && route_raw[0] != 't')) {
        log::error(cat, "Received malformed worker id in worker message; unable to process worker command");
        return;
    }
    char wtype = route_raw[0];
    bool tagged_worker = wtype == 't';
    uint32_t wid;
    std::memcpy(&wid, route_raw.data() + 1, 4);
    if (tagged_worker
            ? 0 == wid || wid > tagged_workers.size() // tagged worker ids are indexed from 1 to N (0 means untagged)
            : wid >= workers.size()) { // regular worker ids are indexed from 0 to N-1
        log::error(cat, "Received invalid worker id {}{} in worker message; unable to process worker command", wtype, wid);
        return;
    }

    auto& run = tagged_worker ? std::get<run_info>(tagged_workers[wid - 1]) : workers[wid];

    log::trace(cat, "received {} command from {}{}", cmd, wtype, wid);
    if (cmd == "RAN"sv) {
        log::trace(cat, "Worker {}{} finished {}", wtype, wid, run.is_batch_job ? "batch job" : run.command);
        if (run.is_batch_job) {
            if (tagged_worker) {
                std::get<bool>(tagged_workers[wid - 1]) = false;
            } else {
                auto& active = run.is_reply_job ? reply_jobs_active : batch_jobs_active;
                assert(active > 0);
                active--;
            }
            bool clear_job = false;
            auto* batch = std::get<detail::Batch*>(run.to_run);
            if (run.batch_jobno == -1) {
                // Returned from the completion function
                clear_job = true;
            } else {
                auto [state, thread] = batch->job_finished();
                if (state == detail::BatchState::complete) {
                    if (thread == -1) { // run directly in proxy
                        log::trace(cat, "Completion job running directly in proxy");
                        try {
                            batch->job_completion(); // RUN DIRECTLY IN PROXY THREAD
                        } catch (const std::exception &e) {
                            // Raise these to error levels: the caller really shouldn't be doing
                            // anything non-trivial in an in-proxy completion function!
                            log::error(cat, "proxy thread caught exception when processing in-proxy completion command: {}", e.what());
                        } catch (...) {
                            log::error(cat, "proxy thread caught non-standard exception when processing in-proxy completion command");
                        }
                        clear_job = true;
                    } else {
                        auto& jobs =
                            thread > 0
                            ? std::get<batch_queue>(tagged_workers[thread - 1]) // run in tagged thread
                            : run.is_reply_job
                              ? reply_jobs
                              : batch_jobs;
                        jobs.emplace_back(batch, -1);
                    }
                } else if (state == detail::BatchState::done) {
                    // No completion job
                    clear_job = true;
                }
                // else the job is still running
            }

            if (clear_job) {
                delete batch;
            }
        } else {
            assert(run.cat->active_threads > 0);
            run.cat->active_threads--;
        }
        if (max_workers == 0) { // Shutting down
            log::trace(cat, "Telling worker {}{} to quit", wtype, wid);
            route_control(workers_socket, route_raw, "QUIT");
        } else if (!tagged_worker) {
            idle_workers[idle_worker_count++] = wid;
        }
    } else if (cmd == "QUITTING"sv) {
        run.worker_thread.join();
        log::debug(cat, "Worker {}{} exited normally", wtype, wid);
    } else {
        log::error(cat, "Worker {}{} sent unknown control message: '{}'", wtype, wid, cmd);
    }
}

void OxenMQ::proxy_run_worker(run_info& run) {
    if (!run.worker_thread.joinable())
        run.worker_thread = std::thread{&OxenMQ::worker_thread, this, run.worker_id, std::nullopt, nullptr};
    else
        send_routed_message(workers_socket, run.worker_routing_id, "RUN");
}

void OxenMQ::proxy_to_worker(int64_t conn_id, zmq::socket_t& sock, std::vector<zmq::message_t>& parts) {
    bool outgoing = sock.get(zmq::sockopt::type) == ZMQ_DEALER;

    peer_info tmp_peer;
    tmp_peer.conn_id = conn_id;
    if (!outgoing) tmp_peer.route = parts[0].to_string();
    peer_info* peer = nullptr;
    if (outgoing) {
        auto snit = outgoing_sn_conns.find(conn_id);
        auto it = snit != outgoing_sn_conns.end()
            ? peers.find(snit->second)
            : peers.find(conn_id);

        if (it == peers.end()) {
            log::warning(cat, "Internal error: connection id {} not found", conn_id);
            return;
        }
        peer = &it->second;
        // Check SN status of the remote, so that even if we connect_remote but land on a SN,
        // messages we receive back on that connection that require SN status will be properly
        // accepted.
        if (!peer->pubkey.empty())
            peer->service_node = active_service_nodes.count(peer->pubkey);
    } else {
        if (conn_id == inproc_listener_connid) {
            tmp_peer.auth_level = AuthLevel::admin;
            tmp_peer.pubkey = pubkey;
            tmp_peer.service_node = active_service_nodes.count(pubkey);
        } else {
            std::tie(tmp_peer.pubkey, tmp_peer.auth_level) = detail::extract_metadata(parts.back());
            tmp_peer.service_node = tmp_peer.pubkey.size() == 32 && active_service_nodes.count(tmp_peer.pubkey);
        }
        if (tmp_peer.service_node) {
            // It's a service node so we should have a peer_info entry; see if we can find one with
            // the same route, and if not, add one.
            auto pr = peers.equal_range(tmp_peer.pubkey);
            for (auto it = pr.first; it != pr.second; ++it) {
                if (it->second.conn_id == tmp_peer.conn_id && it->second.route == tmp_peer.route) {
                    peer = &it->second;
                    // Update the stored auth level and service node status just in case the peer
                    // reconnected or the service node status changed
                    peer->auth_level = tmp_peer.auth_level;
                    peer->service_node = true;
                    break;
                }
            }
            if (!peer) {
                // We don't have a record: this is either a new SN connection or a new message on a
                // connection that recently gained SN status.
                peer = &peers.emplace(ConnectionID{tmp_peer.pubkey}, std::move(tmp_peer))->second;
            }
        } else {
            // Incoming, non-SN connection: we don't store a peer_info for this, so just use the
            // temporary one
            peer = &tmp_peer;
        }
    }

    size_t command_part_index = outgoing ? 0 : 1;
    std::string command = parts[command_part_index].to_string();

    // Steal any data message parts
    size_t data_part_index = command_part_index + 1;
    std::vector<zmq::message_t> data_parts;
    data_parts.reserve(parts.size() - data_part_index);
    for (auto it = parts.begin() + data_part_index; it != parts.end(); ++it)
        data_parts.push_back(std::move(*it));

    auto cat_call = get_command(command);

    // Check that command is valid, that we have permission, etc.
    if (!proxy_check_auth(conn_id, outgoing, *peer, parts[command_part_index], cat_call, data_parts))
        return;

    auto& category = *cat_call.first;
    Access access{peer->auth_level, peer->service_node, local_service_node};

    if (category.active_threads >= category.reserved_threads && active_workers() >= general_workers) {
        // No free reserved or general spots, try to queue it for later
        if (category.max_queue >= 0 && category.queued >= category.max_queue) {
            log::warning(cat, "No space to queue incoming command {}; already have {} commands queued in that category (max {}); dropping message",
                    command, category.queued, category.max_queue);
            return;
        }

        log::debug(cat, "No available free workers, queuing {} for later", command);
        ConnectionID conn{peer->service_node ? ConnectionID::SN_ID : conn_id, peer->pubkey, std::move(tmp_peer.route)};
        pending_commands.emplace_back(category, std::move(command), std::move(data_parts), cat_call.second,
                std::move(conn), std::move(access), get_peer_address(parts[command_part_index]));
        category.queued++;
        return;
    }

    if (cat_call.second->second /*is_request*/ && data_parts.empty()) {
        log::warning(cat, "Received an invalid request command with no reply tag; dropping message");
        return;
    }

    auto& run = get_idle_worker();
    {
        ConnectionID c{peer->service_node ? ConnectionID::SN_ID : conn_id, peer->pubkey};
        c.route = std::move(tmp_peer.route);
        if (outgoing || peer->service_node)
            tmp_peer.route.clear();
        run.load(&category, std::move(command), std::move(c), std::move(access), get_peer_address(parts[command_part_index]),
                std::move(data_parts), cat_call.second);
    }

    if (outgoing)
        peer->activity(); // outgoing connection activity, pump the activity timer

    log::trace(
            cat,
            "Forwarding incoming {} from {} @ {} to worker {}",
            run.command,
            run.conn,
            peer_address(parts[command_part_index]),
            run.worker_routing_name);

    proxy_run_worker(run);
    category.active_threads++;
}

void OxenMQ::inject_task(const std::string& category, std::string command, std::string remote, std::function<void()> callback) {
    if (!callback) return;
    auto it = categories.find(category);
    if (it == categories.end())
        throw std::out_of_range{"Invalid category `" + category + "': category does not exist"};
    detail::send_control(get_control_socket(), "INJECT", oxenc::bt_serialize(detail::serialize_object(
                injected_task{it->second, std::move(command), std::move(remote), std::move(callback)})));
}

void OxenMQ::proxy_inject_task(injected_task task) {
    auto& category = task.cat;
    if (category.active_threads >= category.reserved_threads && active_workers() >= general_workers) {
        // No free worker slot, queue for later
        if (category.max_queue >= 0 && category.queued >= category.max_queue) {
            log::warning(cat, "No space to queue injected task {}; already have {} commands queued in that category (max {}); dropping task",
                    task.command, category.queued, category.max_queue);
            return;
        }
        log::debug(cat, "No available free workers for injected task {}; queuing for later", task.command);
        pending_commands.emplace_back(category, std::move(task.command), std::move(task.callback), std::move(task.remote));
        category.queued++;
        return;
    }

    auto& run = get_idle_worker();
    log::trace(cat, "Forwarding incoming injected task {} from {} to worker {}", task.command, task.remote, run.worker_routing_name);
    run.load(&category, std::move(task.command), std::move(task.remote), std::move(task.callback));

    proxy_run_worker(run);
    category.active_threads++;
}



}
