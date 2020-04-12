#include "lokimq.h"
#include "batch.h"
#include "hex.h"
#include "lokimq-internal.h"

namespace lokimq {

void LokiMQ::worker_thread(unsigned int index) {
    std::string worker_id = "w" + std::to_string(index);
    zmq::socket_t sock{context, zmq::socket_type::dealer};
    sock.setsockopt(ZMQ_ROUTING_ID, worker_id.data(), worker_id.size());
    LMQ_LOG(debug, "New worker thread ", worker_id, " started");
    sock.connect(SN_ADDR_WORKERS);

    Message message{*this, 0};
    std::vector<zmq::message_t> parts;
    run_info& run = workers[index]; // This contains our first job, and will be updated later with subsequent jobs

    while (true) {
        try {
            if (run.is_batch_job) {
                if (run.batch_jobno >= 0) {
                    LMQ_TRACE("worker thread ", worker_id, " running batch ", run.batch, "#", run.batch_jobno);
                    run.batch->run_job(run.batch_jobno);
                } else if (run.batch_jobno == -1) {
                    LMQ_TRACE("worker thread ", worker_id, " running batch ", run.batch, " completion");
                    run.batch->job_completion();
                }
            } else {
                message.conn = run.conn;
                message.data.clear();

                LMQ_TRACE("Got incoming command from ", message.conn, message.conn.route.empty() ? "(outgoing)" : "(incoming)");

                if (run.callback->second /*is_request*/) {
                    message.reply_tag = {run.data_parts[0].data<char>(), run.data_parts[0].size()};
                    for (auto it = run.data_parts.begin() + 1; it != run.data_parts.end(); ++it)
                        message.data.emplace_back(it->data<char>(), it->size());
                } else {
                    for (auto& m : run.data_parts)
                        message.data.emplace_back(m.data<char>(), m.size());
                }

                LMQ_TRACE("worker thread ", worker_id, " invoking ", run.command, " callback with ", message.data.size(), " message parts");
                run.callback->first(message);
            }
        }
        catch (const bt_deserialize_invalid& e) {
            LMQ_LOG(warn, worker_id, " deserialization failed: ", e.what(), "; ignoring request");
        }
        catch (const mapbox::util::bad_variant_access& e) {
            LMQ_LOG(warn, worker_id, " deserialization failed: found unexpected serialized type (", e.what(), "); ignoring request");
        }
        catch (const std::out_of_range& e) {
            LMQ_LOG(warn, worker_id, " deserialization failed: invalid data - required field missing (", e.what(), "); ignoring request");
        }
        catch (const std::exception& e) {
            LMQ_LOG(warn, worker_id, " caught exception when processing command: ", e.what());
        }
        catch (...) {
            LMQ_LOG(warn, worker_id, " caught non-standard exception when processing command");
        }

        while (true) {
            // Signal that we are ready for another job and wait for it.  (We do this down here
            // because our first job gets set up when the thread is started).
            detail::send_control(sock, "RAN");
            LMQ_TRACE("worker ", worker_id, " waiting for requests");
            parts.clear();
            recv_message_parts(sock, parts);

            if (parts.size() != 1) {
                LMQ_LOG(error, "Internal error: worker ", worker_id, " received invalid ", parts.size(), "-part worker instruction");
                continue;
            }
            auto command = view(parts[0]);
            if (command == "RUN") {
                LMQ_LOG(debug, "worker ", worker_id, " running command ", run.command);
                break; // proxy has set up a command for us, go back and run it.
            } else if (command == "QUIT") {
                LMQ_LOG(debug, "worker ", worker_id, " shutting down");
                detail::send_control(sock, "QUITTING");
                sock.setsockopt<int>(ZMQ_LINGER, 1000);
                sock.close();
                return;
            } else {
                LMQ_LOG(error, "Internal error: worker ", worker_id, " received invalid command: `", command, "'");
            }
        }
    }
}


LokiMQ::run_info& LokiMQ::get_idle_worker() {
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

void LokiMQ::proxy_worker_message(std::vector<zmq::message_t>& parts) {
    // Process messages sent by workers
    if (parts.size() != 2) {
        LMQ_LOG(error, "Received send invalid ", parts.size(), "-part message");
        return;
    }
    auto route = view(parts[0]), cmd = view(parts[1]);
    LMQ_TRACE("worker message from ", route);
    assert(route.size() >= 2 && route[0] == 'w' && route[1] >= '0' && route[1] <= '9');
    string_view worker_id_str{&route[1], route.size()-1}; // Chop off the leading "w"
    unsigned int worker_id = detail::extract_unsigned(worker_id_str);
    if (!worker_id_str.empty() /* didn't consume everything */ || worker_id >= workers.size()) {
        LMQ_LOG(error, "Worker id '", route, "' is invalid, unable to process worker command");
        return;
    }

    auto& run = workers[worker_id];

    LMQ_TRACE("received ", cmd, " command from ", route);
    if (cmd == "RAN") {
        LMQ_LOG(debug, "Worker ", route, " finished ", run.command);
        if (run.is_batch_job) {
            auto& jobs = run.is_reply_job ? reply_jobs : batch_jobs;
            auto& active = run.is_reply_job ? reply_jobs_active : batch_jobs_active;
            assert(active > 0);
            active--;
            bool clear_job = false;
            if (run.batch_jobno == -1) {
                // Returned from the completion function
                clear_job = true;
            } else {
                auto status = run.batch->job_finished();
                if (status == detail::BatchStatus::complete) {
                    jobs.emplace(run.batch, -1);
                } else if (status == detail::BatchStatus::complete_proxy) {
                    try {
                        run.batch->job_completion(); // RUN DIRECTLY IN PROXY THREAD
                    } catch (const std::exception &e) {
                        // Raise these to error levels: the caller really shouldn't be doing
                        // anything non-trivial in an in-proxy completion function!
                        LMQ_LOG(error, "proxy thread caught exception when processing in-proxy completion command: ", e.what());
                    } catch (...) {
                        LMQ_LOG(error, "proxy thread caught non-standard exception when processing in-proxy completion command");
                    }
                    clear_job = true;
                } else if (status == detail::BatchStatus::done) {
                    clear_job = true;
                }
            }

            if (clear_job) {
                batches.erase(run.batch);
                delete run.batch;
                run.batch = nullptr;
            }
        } else {
            assert(run.cat->active_threads > 0);
            run.cat->active_threads--;
        }
        if (max_workers == 0) { // Shutting down
            LMQ_TRACE("Telling worker ", route, " to quit");
            route_control(workers_socket, route, "QUIT");
        } else {
            idle_workers.push_back(worker_id);
        }
    } else if (cmd == "QUITTING") {
        workers[worker_id].worker_thread.join();
        LMQ_LOG(debug, "Worker ", route, " exited normally");
    } else {
        LMQ_LOG(error, "Worker ", route, " sent unknown control message: `", cmd, "'");
    }
}

void LokiMQ::proxy_run_worker(run_info& run) {
    if (!run.worker_thread.joinable())
        run.worker_thread = std::thread{&LokiMQ::worker_thread, this, run.worker_id};
    else
        send_routed_message(workers_socket, run.worker_routing_id, "RUN");
}

void LokiMQ::proxy_to_worker(size_t conn_index, std::vector<zmq::message_t>& parts) {
    bool outgoing = connections[conn_index].getsockopt<int>(ZMQ_TYPE) == ZMQ_DEALER;

    peer_info tmp_peer;
    tmp_peer.conn_index = conn_index;
    if (!outgoing) tmp_peer.route = parts[0].to_string();
    peer_info* peer = nullptr;
    if (outgoing) {
        auto it = peers.find(conn_index_to_id[conn_index]);
        if (it == peers.end()) {
            LMQ_LOG(warn, "Internal error: connection index ", conn_index, " not found");
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
                if (it->second.conn_index == tmp_peer.conn_index && it->second.route == tmp_peer.route) {
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
    if (!proxy_check_auth(conn_index, outgoing, *peer, parts[command_part_index], cat_call, data_parts))
        return;

    auto& category = *cat_call.first;

    if (category.active_threads >= category.reserved_threads && active_workers() >= general_workers) {
        // No free reserved or general spots, try to queue it for later
        if (category.max_queue >= 0 && category.queued >= category.max_queue) {
            LMQ_LOG(warn, "No space to queue incoming command ", command, "; already have ", category.queued,
                    "commands queued in that category (max ", category.max_queue, "); dropping message");
            return;
        }

        LMQ_LOG(debug, "No available free workers, queuing ", command, " for later");
        ConnectionID conn{peer->service_node ? ConnectionID::SN_ID : conn_index_to_id[conn_index].id, peer->pubkey, std::move(tmp_peer.route)};
        pending_commands.emplace_back(category, std::move(command), std::move(data_parts), cat_call.second, std::move(conn));
        category.queued++;
        return;
    }

    if (cat_call.second->second /*is_request*/ && data_parts.empty()) {
        LMQ_LOG(warn, "Received an invalid request command with no reply tag; dropping message");
        return;
    }

    auto& run = get_idle_worker();
    {
        ConnectionID c{peer->service_node ? ConnectionID::SN_ID : conn_index_to_id[conn_index].id, peer->pubkey};
        c.route = std::move(tmp_peer.route);
        if (outgoing || peer->service_node)
            tmp_peer.route.clear();
        run.load(&category, std::move(command), std::move(c), std::move(data_parts), cat_call.second);
    }

    if (outgoing)
        peer->activity(); // outgoing connection activity, pump the activity timer

    LMQ_TRACE("Forwarding incoming ", run.command, " from ", run.conn, " @ ", peer_address(parts[command_part_index]),
            " to worker ", run.worker_routing_id);

    proxy_run_worker(run);
    category.active_threads++;
}


}
