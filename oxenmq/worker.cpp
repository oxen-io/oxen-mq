#include "oxenmq.h"
#include "batch.h"
#include "oxenmq-internal.h"

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
extern "C" {
#include <pthread.h>
#include <pthread_np.h>
}
#endif

namespace oxenmq {

namespace {

// Waits for a specific command or "QUIT" on the given socket.  Returns true if the command was
// received.  If "QUIT" was received, replies with "QUITTING" on the socket and closes it, then
// returns false.
[[gnu::always_inline]] inline
bool worker_wait_for(OxenMQ& lmq, zmq::socket_t& sock, std::vector<zmq::message_t>& parts, const std::string_view worker_id, const std::string_view expect) {
    while (true) {
        lmq.log(LogLevel::debug, __FILE__, __LINE__, "worker ", worker_id, " waiting for ", expect);
        parts.clear();
        recv_message_parts(sock, parts);
        if (parts.size() != 1) {
            lmq.log(LogLevel::error, __FILE__, __LINE__, "Internal error: worker ", worker_id, " received invalid ", parts.size(), "-part control msg");
            continue;
        }
        auto command = view(parts[0]);
        if (command == expect) {
#ifndef NDEBUG
            lmq.log(LogLevel::trace, __FILE__, __LINE__, "Worker ", worker_id, " received waited-for ", expect, " command");
#endif
            return true;
        } else if (command == "QUIT"sv) {
            lmq.log(LogLevel::debug, __FILE__, __LINE__, "Worker ", worker_id, " received QUIT command, shutting down");
            detail::send_control(sock, "QUITTING");
            sock.set(zmq::sockopt::linger, 1000);
            sock.close();
            return false;
        } else {
            lmq.log(LogLevel::error, __FILE__, __LINE__, "Internal error: worker ", worker_id, " received invalid command: `", command, "'");
        }
    }
}

}

void OxenMQ::worker_thread(unsigned int index, std::optional<std::string> tagged, std::function<void()> start) {
    std::string routing_id = (tagged ? "t" : "w") + std::to_string(index); // for routing
    std::string_view worker_id{tagged ? *tagged : routing_id};              // for debug

    [[maybe_unused]] std::string thread_name = tagged.value_or("lmq-" + routing_id);
#if defined(__linux__) || defined(__sun) || defined(__MINGW32__)
    if (thread_name.size() > 15) thread_name.resize(15);
    pthread_setname_np(pthread_self(), thread_name.c_str());
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), thread_name.c_str());
#elif defined(__MACH__)
    pthread_setname_np(thread_name.c_str());
#endif

    zmq::socket_t sock{context, zmq::socket_type::dealer};
    sock.set(zmq::sockopt::routing_id, routing_id);
    LMQ_LOG(debug, "New worker thread ", worker_id, " (", routing_id, ") started");
    sock.connect(SN_ADDR_WORKERS);
    if (tagged)
        detail::send_control(sock, "STARTING");

    Message message{*this, 0, AuthLevel::none, ""s};
    std::vector<zmq::message_t> parts;

    bool waiting_for_command;
    if (tagged) {
        // If we're a tagged worker then we got started up before OxenMQ started, so we need to wait
        // for an all-clear signal from OxenMQ first, then we fire our `start` callback, then we can
        // start waiting for commands in the main loop further down.  (We also can't get the
        // reference to our `tagged_workers` element until the main proxy threads is running).

        waiting_for_command = true;

        if (!worker_wait_for(*this, sock, parts, worker_id, "START"sv))
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
            if (!worker_wait_for(*this, sock, parts, worker_id, "RUN"sv))
                return;
        }

        try {
            if (run.is_batch_job) {
                auto* batch = var::get<detail::Batch*>(run.to_run);
                if (run.batch_jobno >= 0) {
                    LMQ_TRACE("worker thread ", worker_id, " running batch ", batch, "#", run.batch_jobno);
                    batch->run_job(run.batch_jobno);
                } else if (run.batch_jobno == -1) {
                    LMQ_TRACE("worker thread ", worker_id, " running batch ", batch, " completion");
                    batch->job_completion();
                }
            } else if (run.is_injected) {
                auto& func = var::get<std::function<void()>>(run.to_run);
                LMQ_TRACE("worker thread ", worker_id, " invoking injected command ", run.command);
                func();
                func = nullptr;
            } else {
                message.conn = run.conn;
                message.access = run.access;
                message.remote = std::move(run.remote);
                message.data.clear();

                LMQ_TRACE("Got incoming command from ", message.remote, "/", message.conn, message.conn.route.empty() ? " (outgoing)" : " (incoming)");

                auto& [callback, is_request] = *var::get<const std::pair<CommandCallback, bool>*>(run.to_run);
                if (is_request) {
                    message.reply_tag = {run.data_parts[0].data<char>(), run.data_parts[0].size()};
                    for (auto it = run.data_parts.begin() + 1; it != run.data_parts.end(); ++it)
                        message.data.emplace_back(it->data<char>(), it->size());
                } else {
                    for (auto& m : run.data_parts)
                        message.data.emplace_back(m.data<char>(), m.size());
                }

                LMQ_TRACE("worker thread ", worker_id, " invoking ", run.command, " callback with ", message.data.size(), " message parts");
                callback(message);
            }
        }
        catch (const bt_deserialize_invalid& e) {
            LMQ_LOG(warn, worker_id, " deserialization failed: ", e.what(), "; ignoring request");
        }
#ifndef BROKEN_APPLE_VARIANT
        catch (const std::bad_variant_access& e) {
            LMQ_LOG(warn, worker_id, " deserialization failed: found unexpected serialized type (", e.what(), "); ignoring request");
        }
#endif
        catch (const std::out_of_range& e) {
            LMQ_LOG(warn, worker_id, " deserialization failed: invalid data - required field missing (", e.what(), "); ignoring request");
        }
        catch (const std::exception& e) {
            LMQ_LOG(warn, worker_id, " caught exception when processing command: ", e.what());
        }
        catch (...) {
            LMQ_LOG(warn, worker_id, " caught non-standard exception when processing command");
        }

        // Tell the proxy thread that we are ready for another job
        detail::send_control(sock, "RAN");
        waiting_for_command = true;
    }
}


OxenMQ::run_info& OxenMQ::get_idle_worker() {
    if (idle_workers.empty()) {
        size_t id = workers.size();
        assert(workers.capacity() > id);
        workers.emplace_back();
        auto& r = workers.back();
        r.worker_id = id;
        r.worker_routing_id = "w" + std::to_string(id);
        return r;
    }
    size_t id = idle_workers.back();
    idle_workers.pop_back();
    return workers[id];
}

void OxenMQ::proxy_worker_message(std::vector<zmq::message_t>& parts) {
    // Process messages sent by workers
    if (parts.size() != 2) {
        LMQ_LOG(error, "Received send invalid ", parts.size(), "-part message");
        return;
    }
    auto route = view(parts[0]), cmd = view(parts[1]);
    LMQ_TRACE("worker message from ", route);
    assert(route.size() >= 2 && (route[0] == 'w' || route[0] == 't') && route[1] >= '0' && route[1] <= '9');
    bool tagged_worker = route[0] == 't';
    std::string_view worker_id_str{&route[1], route.size()-1}; // Chop off the leading "w" (or "t")
    unsigned int worker_id = detail::extract_unsigned(worker_id_str);
    if (!worker_id_str.empty() /* didn't consume everything */ ||
            (tagged_worker
                ? 0 == worker_id || worker_id > tagged_workers.size() // tagged worker ids are indexed from 1 to N (0 means untagged)
                : worker_id >= workers.size() // regular worker ids are indexed from 0 to N-1
            )
    ) {
        LMQ_LOG(error, "Worker id '", route, "' is invalid, unable to process worker command");
        return;
    }

    auto& run = tagged_worker ? std::get<run_info>(tagged_workers[worker_id - 1]) : workers[worker_id];

    LMQ_TRACE("received ", cmd, " command from ", route);
    if (cmd == "RAN"sv) {
        LMQ_TRACE("Worker ", route, " finished ", run.is_batch_job ? "batch job" : run.command);
        if (run.is_batch_job) {
            if (tagged_worker) {
                std::get<bool>(tagged_workers[worker_id - 1]) = false;
            } else {
                auto& active = run.is_reply_job ? reply_jobs_active : batch_jobs_active;
                assert(active > 0);
                active--;
            }
            bool clear_job = false;
            auto* batch = var::get<detail::Batch*>(run.to_run);
            if (run.batch_jobno == -1) {
                // Returned from the completion function
                clear_job = true;
            } else {
                auto [state, thread] = batch->job_finished();
                if (state == detail::BatchState::complete) {
                    if (thread == -1) { // run directly in proxy
                        LMQ_TRACE("Completion job running directly in proxy");
                        try {
                            batch->job_completion(); // RUN DIRECTLY IN PROXY THREAD
                        } catch (const std::exception &e) {
                            // Raise these to error levels: the caller really shouldn't be doing
                            // anything non-trivial in an in-proxy completion function!
                            LMQ_LOG(error, "proxy thread caught exception when processing in-proxy completion command: ", e.what());
                        } catch (...) {
                            LMQ_LOG(error, "proxy thread caught non-standard exception when processing in-proxy completion command");
                        }
                        clear_job = true;
                    } else {
                        auto& jobs =
                            thread > 0
                            ? std::get<std::queue<batch_job>>(tagged_workers[thread - 1]) // run in tagged thread
                            : run.is_reply_job
                              ? reply_jobs
                              : batch_jobs;
                        jobs.emplace(batch, -1);
                    }
                } else if (state == detail::BatchState::done) {
                    // No completion job
                    clear_job = true;
                }
                // else the job is still running
            }

            if (clear_job) {
                batches.erase(batch);
                delete batch;
                run.to_run = static_cast<detail::Batch*>(nullptr);
            }
        } else {
            assert(run.cat->active_threads > 0);
            run.cat->active_threads--;
        }
        if (max_workers == 0) { // Shutting down
            LMQ_TRACE("Telling worker ", route, " to quit");
            route_control(workers_socket, route, "QUIT");
        } else if (!tagged_worker) {
            idle_workers.push_back(worker_id);
        }
    } else if (cmd == "QUITTING"sv) {
        run.worker_thread.join();
        LMQ_LOG(debug, "Worker ", route, " exited normally");
    } else {
        LMQ_LOG(error, "Worker ", route, " sent unknown control message: `", cmd, "'");
    }
}

void OxenMQ::proxy_run_worker(run_info& run) {
    if (!run.worker_thread.joinable())
        run.worker_thread = std::thread{[this, id=run.worker_id] { worker_thread(id); }};
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
            LMQ_LOG(warn, "Internal error: connection id ", conn_id, " not found");
            return;
        }
        peer = &it->second;
    } else {
        std::tie(tmp_peer.pubkey, tmp_peer.auth_level) = detail::extract_metadata(parts.back());
        tmp_peer.service_node = tmp_peer.pubkey.size() == 32 && active_service_nodes.count(tmp_peer.pubkey);

        if (tmp_peer.service_node) {
            // It's a service node so we should have a peer_info entry; see if we can find one with
            // the same route, and if not, add one.
            auto pr = peers.equal_range(tmp_peer.pubkey);
            for (auto it = pr.first; it != pr.second; ++it) {
                if (it->second.conn_id == tmp_peer.conn_id && it->second.route == tmp_peer.route) {
                    peer = &it->second;
                    // Update the stored auth level just in case the peer reconnected
                    peer->auth_level = tmp_peer.auth_level;
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
            LMQ_LOG(warn, "No space to queue incoming command ", command, "; already have ", category.queued,
                    "commands queued in that category (max ", category.max_queue, "); dropping message");
            return;
        }

        LMQ_LOG(debug, "No available free workers, queuing ", command, " for later");
        ConnectionID conn{peer->service_node ? ConnectionID::SN_ID : conn_id, peer->pubkey, std::move(tmp_peer.route)};
        pending_commands.emplace_back(category, std::move(command), std::move(data_parts), cat_call.second,
                std::move(conn), std::move(access), peer_address(parts[command_part_index]));
        category.queued++;
        return;
    }

    if (cat_call.second->second /*is_request*/ && data_parts.empty()) {
        LMQ_LOG(warn, "Received an invalid request command with no reply tag; dropping message");
        return;
    }

    auto& run = get_idle_worker();
    {
        ConnectionID c{peer->service_node ? ConnectionID::SN_ID : conn_id, peer->pubkey};
        c.route = std::move(tmp_peer.route);
        if (outgoing || peer->service_node)
            tmp_peer.route.clear();
        run.load(&category, std::move(command), std::move(c), std::move(access), peer_address(parts[command_part_index]),
                std::move(data_parts), cat_call.second);
    }

    if (outgoing)
        peer->activity(); // outgoing connection activity, pump the activity timer

    LMQ_TRACE("Forwarding incoming ", run.command, " from ", run.conn, " @ ", peer_address(parts[command_part_index]),
            " to worker ", run.worker_routing_id);

    proxy_run_worker(run);
    category.active_threads++;
}

void OxenMQ::inject_task(const std::string& category, std::string command, std::string remote, std::function<void()> callback) {
    if (!callback) return;
    auto it = categories.find(category);
    if (it == categories.end())
        throw std::out_of_range{"Invalid category `" + category + "': category does not exist"};
    detail::send_control(get_control_socket(), "INJECT", bt_serialize(detail::serialize_object(
                injected_task{it->second, std::move(command), std::move(remote), std::move(callback)})));
}

void OxenMQ::proxy_inject_task(injected_task task) {
    auto& category = task.cat;
    if (category.active_threads >= category.reserved_threads && active_workers() >= general_workers) {
        // No free worker slot, queue for later
        if (category.max_queue >= 0 && category.queued >= category.max_queue) {
            LMQ_LOG(warn, "No space to queue injected task ", task.command, "; already have ", category.queued,
                    "commands queued in that category (max ", category.max_queue, "); dropping task");
            return;
        }
        LMQ_LOG(debug, "No available free workers for injected task ", task.command, "; queuing for later");
        pending_commands.emplace_back(category, std::move(task.command), std::move(task.callback), std::move(task.remote));
        category.queued++;
        return;
    }

    auto& run = get_idle_worker();
    LMQ_TRACE("Forwarding incoming injected task ", task.command, " from ", task.remote, " to worker ", run.worker_routing_id);
    run.load(&category, std::move(task.command), std::move(task.remote), std::move(task.callback));

    proxy_run_worker(run);
    category.active_threads++;
}



}
