#include <queue>
#include <map>
#include <cassert>
extern "C" {
#include <sodium.h>
}
#include "lokimq.h"
#include "hex.h"

namespace lokimq {

constexpr char SN_ADDR_COMMAND[] = "inproc://sn-command";
constexpr char SN_ADDR_WORKERS[] = "inproc://sn-workers";
constexpr char SN_ADDR_SELF[] = "inproc://sn-self";
constexpr char ZMQ_ADDR_ZAP[] = "inproc://zeromq.zap.01";

// Inside some method:
//     LMQ_LOG(warn, "bad ", 42, " stuff");
#define LMQ_LOG(level, ...) log_(LogLevel::level, __FILE__, __LINE__, __VA_ARGS__)

// This is the domain used for listening service nodes.
constexpr const char AUTH_DOMAIN_SN[] = "loki.sn";



namespace {

/// Destructor for create_message(std::string&&) that zmq calls when it's done with the message.
extern "C" void message_buffer_destroy(void*, void* hint) {
    delete reinterpret_cast<std::string*>(hint);
}

/// Creates a message without needing to reallocate the provided string data
zmq::message_t create_message(std::string &&data) {
    auto *buffer = new std::string(std::move(data));
    return zmq::message_t{&(*buffer)[0], buffer->size(), message_buffer_destroy, buffer};
};

/// Create a message copying from a string_view
zmq::message_t create_message(string_view data) {
    return zmq::message_t{data.begin(), data.end()};
}

/*
/// Create a message that references the existing data.  In order to use this safely you need to be
/// sure that the referenced data lasts sufficiently long, which can be tricky.
zmq::message_t create_message_reference(string_view data) {
    return zmq::message_t{const_cast<char*>(&data[0]), data.size(), (zmq::free_fn*) nullptr, nullptr};
}
*/

/// Creates a message by bt-serializing the given value (string, number, list, or dict)
template <typename T>
zmq::message_t create_bt_message(T&& data) { return create_message(bt_serialize(std::forward<T>(data))); }

/// Sends a control message to a specific destination by prefixing the worker name (or identity)
/// then appending the command and optional data (if non-empty).  (This is needed when sending the control message
/// to a router socket, i.e. inside the proxy thread).
void route_control(zmq::socket_t& sock, string_view identity, string_view cmd, const std::string& data = {}) {
    sock.send(create_message(identity), zmq::send_flags::sndmore);
    detail::send_control(sock, cmd, data);
}

// Receive all the parts of a single message from the given socket.  Returns true if a message was
// received, false if called with flags=zmq::recv_flags::dontwait and no message was available.
template <typename OutputIt>
bool recv_message_parts(zmq::socket_t &sock, OutputIt it, const zmq::recv_flags flags = zmq::recv_flags::none) {
    bool more = true;
    while (more) {
        zmq::message_t msg;
        if (!sock.recv(msg, flags))
            return false;
        more = msg.more();
        *it = std::move(msg);
    }
    return true;
}

template <typename It>
void send_message_parts(zmq::socket_t &sock, It begin, It end) {
    while (begin != end) {
        // FIXME: for outgoing connections on ZMQ_DEALER we want to use ZMQ_DONTWAIT and handle
        // EAGAIN error (which either means the peer HWM is hit -- probably indicating a connection
        // failure -- or the underlying connect() system call failed).  Assuming it's an outgoing
        // connection, we should destroy it.
        zmq::message_t &msg = *begin++;
        sock.send(msg, begin == end ? zmq::send_flags::none : zmq::send_flags::sndmore);
    }
}

template <typename Container>
void send_message_parts(zmq::socket_t &sock, Container &&c) {
    send_message_parts(sock, c.begin(), c.end());
}

void send_routed_message(zmq::socket_t &socket, std::string route, std::string msg, std::string data = {}) {
    std::array<zmq::message_t, 3> msgs{{create_message(std::move(route)), create_message(std::move(msg))}};
    if (!data.empty())
        msgs[2] = create_message(std::move(data));
    send_message_parts(socket, msgs.begin(), data.empty() ? std::prev(msgs.end()) : msgs.end());
}

void send_direct_message(zmq::socket_t &socket, std::string msg, std::string data = {}) {
    std::array<zmq::message_t, 2> msgs{{create_message(std::move(msg))}};
    if (!data.empty())
        msgs[1] = create_message(std::move(data));
    send_message_parts(socket, msgs.begin(), data.empty() ? std::prev(msgs.end()) : msgs.end());
}


template <typename MessageContainer>
std::vector<std::string> as_strings(const MessageContainer& msgs) {
    std::vector<std::string> result;
    result.reserve(msgs.size());
    for (const auto &msg : msgs)
        result.emplace_back(msg.template data<char>(), msg.size());
    return result;
}

// Returns a string view of the given message data.  If real std::string_views are available,
// returns one, otherwise returns a simple partial implementation of string_view.  It's the caller's
// responsibility to keep the referenced message alive.
string_view view(const zmq::message_t &m) {
    return {m.data<char>(), m.size()};
}

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

void check_not_started(const std::thread& proxy_thread) {
    if (proxy_thread.get_id() != std::thread::id{})
        throw std::logic_error("Cannot add categories/commands/aliases after calling `start()`");
}

// Extracts and builds the "send" part of a message for proxy_send/proxy_reply
std::list<zmq::message_t> build_send_parts(bt_dict &data, const std::string &route) {
    std::list<zmq::message_t> parts;
    if (!route.empty())
        parts.push_back(create_message(route));
    for (auto &s : data.at("send").get<bt_list>())
        parts.push_back(create_message(std::move(s.get<std::string>())));
    return parts;
}

std::string to_string(AuthLevel a) {
    switch (a) {
        case AuthLevel::denied: return "denied";
        case AuthLevel::none:   return "none";
        case AuthLevel::basic:  return "basic";
        case AuthLevel::admin:  return "admin";
        default:                return "(unknown)";
    }
}

/// Extracts a pubkey and SN status from a zmq message properties.  Throws on failure.
void extract_pubkey(zmq::message_t& msg, std::string& pubkey, bool& service_node) {
    string_view pubkey_hex{msg.gets("User-Id")};
    // The ZAP handler sets the User-Id to S: (service node) or C: (non-SN) followed by the
    // pubkey.
    if (pubkey_hex.size() != 66 || (pubkey_hex[0] != 'S' && pubkey_hex[0] != 'C') || pubkey_hex[1] != ':')
        throw std::logic_error("bad user-id");
    assert(is_hex(pubkey_hex.begin() + 2, pubkey_hex.end()));
    pubkey.reserve(32);
    from_hex(pubkey_hex.begin() + 2, pubkey_hex.end(), std::back_inserter(pubkey));
    service_node = pubkey_hex[0] == 'S';
}

const char* peer_address(zmq::message_t& msg) {
    try { return msg.gets("Peer-Address"); } catch (...) {}
    return "(unknown)";
}

} // anonymous namespace


namespace detail {

// Sends a control messages between proxy and threads or between proxy and workers consisting of a
// single command codes with an optional data part (the data frame is omitted if empty).
void send_control(zmq::socket_t& sock, string_view cmd, std::string data) {
    auto c = create_message(std::move(cmd));
    if (data.empty()) {
        sock.send(c, zmq::send_flags::none);
    } else {
        auto d = create_message(std::move(data));
        sock.send(c, zmq::send_flags::sndmore);
        sock.send(d, zmq::send_flags::none);
    }
}

} // namespace detail



void LokiMQ::add_pollitem(zmq::socket_t& sock) {
    pollitems.emplace_back();
    auto &p = pollitems.back();
    p.socket = static_cast<void *>(sock);
    p.fd = 0;
    p.events = ZMQ_POLLIN;
}

void LokiMQ::log_level(LogLevel level) {
    log_lvl.store(level, std::memory_order_relaxed);
}

LogLevel LokiMQ::log_level() const {
    return log_lvl.load(std::memory_order_relaxed);
}


void LokiMQ::add_category(std::string name, Access access_level, unsigned int reserved_threads, int max_queue) {
    check_not_started(proxy_thread);

    if (name.size() > MAX_CATEGORY_LENGTH)
        throw std::runtime_error("Invalid category name `" + name + "': name too long (> " + std::to_string(MAX_CATEGORY_LENGTH) + ")");

    if (name.empty() || name.find('.') != std::string::npos)
        throw std::runtime_error("Invalid category name `" + name + "'");

    auto it = categories.find(name);
    if (it != categories.end())
        throw std::runtime_error("Unable to add category `" + name + "': that category already exists");

    categories.emplace(std::move(name), category{access_level, reserved_threads, max_queue});
}

void LokiMQ::add_command(const std::string& category, std::string name, CommandCallback callback) {
    check_not_started(proxy_thread);

    if (name.size() > MAX_COMMAND_LENGTH)
        throw std::runtime_error("Invalid command name `" + name + "': name too long (> " + std::to_string(MAX_COMMAND_LENGTH) + ")");

    auto catit = categories.find(category);
    if (catit == categories.end())
        throw std::runtime_error("Cannot add a command to unknown category `" + category + "'");

    std::string fullname = category + '.' + name;
    if (command_aliases.count(fullname))
        throw std::runtime_error("Cannot add command `" + fullname + "': a command alias with that name is already defined");

    auto ins = catit->second.commands.emplace(std::move(name), std::move(callback));
    if (!ins.second)
        throw std::runtime_error("Cannot add command `" + fullname + "': that command already exists");
}

void LokiMQ::add_command_alias(std::string from, std::string to) {
    check_not_started(proxy_thread);

    if (from.empty())
        throw std::runtime_error("Cannot add an alias for empty command");
    size_t fromdot = from.find('.');
    if (fromdot == 0) // We don't have to have a ., but if we do it can't be at the beginning.
        throw std::runtime_error("Invalid command alias `" + from + "'");

    size_t todot = to.find('.');
    if (todot == 0 || todot == std::string::npos) // must have a dot for the target
        throw std::runtime_error("Invalid command alias target `" + to + "'");

    if (fromdot != std::string::npos) {
        auto catit = categories.find(from.substr(0, fromdot));
        if (catit != categories.end() && catit->second.commands.count(from.substr(fromdot+1)))
            throw std::runtime_error("Invalid command alias: `" + from + "' would mask an existing command");
    }

    auto ins = command_aliases.emplace(std::move(from), std::move(to));
    if (!ins.second)
        throw std::runtime_error("Cannot add command alias `" + ins.first->first + "': that alias already exists");
}

std::atomic<int> next_id{1};

/// We have one mutex here that is generally used once per thread: to create a thread-local command
/// socket to talk to the proxy thread's control socket.  We need the proxy thread to also have a
/// copy of it so that it can close them when it is exiting, and to guard against trying to create
/// one while the proxy is trying to quit.
std::mutex control_sockets_mutex;

/// Accesses a thread-local command socket connected to the proxy's command socket used to issue
/// commands in a thread-safe manner.  A mutex is only required here the first time a thread
/// accesses the control socket.
zmq::socket_t& LokiMQ::get_control_socket() {
    // Maps the LokiMQ unique ID to a local thread command socket.
    static thread_local std::map<int, std::shared_ptr<zmq::socket_t>> control_sockets;
    static thread_local std::pair<int, std::shared_ptr<zmq::socket_t>> last{-1, nullptr};

    // Optimize by caching the last value; LokiMQ is often a singleton and in that case we're
    // going to *always* hit this optimization.  Even if it isn't, we're probably likely to need the
    // same control socket from the same thread multiple times sequentially so this may still help.
    if (object_id == last.first)
        return *last.second;

    auto it = control_sockets.find(object_id);
    if (it != control_sockets.end()) {
        last = *it;
        return *last.second;
    }

    std::lock_guard<std::mutex> lock{control_sockets_mutex};
    if (proxy_shutting_down)
        throw std::runtime_error("Unable to obtain LokiMQ control socket: proxy thread is shutting down");
    auto control = std::make_shared<zmq::socket_t>(context, zmq::socket_type::dealer);
    control->setsockopt<int>(ZMQ_LINGER, 0);
    control->connect(SN_ADDR_COMMAND);
    thread_control_sockets.push_back(control);
    control_sockets.emplace(object_id, control);
    last.first = object_id;
    last.second = std::move(control);
    return *last.second;
}


LokiMQ::LokiMQ(
        std::string pubkey_,
        std::string privkey_,
        bool service_node,
        std::vector<std::string> bind_,
        SNRemoteAddress lookup,
        AllowFunc allow,
        Logger logger,
        unsigned int general_workers)
    : object_id{next_id++}, pubkey{std::move(pubkey_)}, privkey{std::move(privkey_)}, local_service_node{service_node},
        bind{std::move(bind_)}, peer_lookup{std::move(lookup)}, allow_connection{std::move(allow)}, logger{logger},
        poll_remote_offset{poll_internal_size + (bind.empty() ? 0 : 1)}, general_workers{general_workers} {

    LMQ_LOG(trace, "Constructing listening LokiMQ, id=", object_id, ", this=", this);

    if (pubkey.empty() != privkey.empty()) {
        throw std::invalid_argument("LokiMQ construction failed: one (and only one) of pubkey/privkey is empty. Both must be specified, or both empty to generate a key.");
    } else if (pubkey.empty()) {
        if (service_node)
            throw std::invalid_argument("Cannot construct a service node mode LokiMQ without a keypair");
        LMQ_LOG(debug, "generating x25519 keypair for remote-only LokiMQ instance");
        pubkey.resize(crypto_box_PUBLICKEYBYTES);
        privkey.resize(crypto_box_SECRETKEYBYTES);
        crypto_box_keypair(reinterpret_cast<unsigned char*>(&pubkey[0]), reinterpret_cast<unsigned char*>(&privkey[0]));
    } else if (pubkey.size() != crypto_box_PUBLICKEYBYTES) {
        throw std::invalid_argument("pubkey has invalid size " + std::to_string(pubkey.size()) + ", expected " + std::to_string(crypto_box_PUBLICKEYBYTES));
    } else if (privkey.size() != crypto_box_SECRETKEYBYTES) {
        throw std::invalid_argument("privkey has invalid size " + std::to_string(privkey.size()) + ", expected " + std::to_string(crypto_box_SECRETKEYBYTES));
    } else {
        // Verify the pubkey.  We could get by with taking just the privkey and just generate this
        // for ourselves, but this provides an extra check to make sure we and the caller agree
        // cryptographically (e.g. to make sure they don't pass us an ed25519 keypair by mistake)
        std::string verify_pubkey{crypto_box_PUBLICKEYBYTES, 0};
        crypto_scalarmult_base(reinterpret_cast<unsigned char*>(&verify_pubkey), reinterpret_cast<unsigned char*>(&privkey[0]));
        if (verify_pubkey != pubkey)
            throw std::invalid_argument("Invalid pubkey/privkey values given to LokiMQ construction: pubkey verification failed");
    }

    // If we're not binding to anything then we don't listen, i.e. we can only establish outbound
    // connections.  Don't allow this if we are in service_node mode because, if we aren't
    // listening, we are useless as a service node.
    if (bind.empty() && service_node)
        throw std::invalid_argument{"Cannot create a service node listener with no address(es) to bind"};
}

void LokiMQ::start() {
    if (proxy_thread.get_id() != std::thread::id{})
        throw std::logic_error("Cannot call start() multiple times!");

    LMQ_LOG(info, "Initializing LokiMQ ", bind.empty() ? "remote-only" : "listener", " with pubkey ", to_hex(pubkey));

    // We bind `command` here so that the `get_control_socket()` below is always connecting to a
    // bound socket, but we do nothing else here: the proxy thread is responsible for everything
    // except binding it.
    command.bind(SN_ADDR_COMMAND);
    proxy_thread = std::thread{&LokiMQ::proxy_loop, this};

    LMQ_LOG(debug, "Waiting for proxy thread to get ready...");
    auto &control = get_control_socket();
    detail::send_control(control, "START");
    LMQ_LOG(trace, "Sent START command");

    zmq::message_t ready_msg;
    std::vector<zmq::message_t> parts;
    try { recv_message_parts(control, std::back_inserter(parts)); }
    catch (const zmq::error_t &e) { throw std::runtime_error("Failure reading from LokiMQ::Proxy thread: "s + e.what()); }

    if (!(parts.size() == 1 && view(parts.front()) == "READY"))
        throw std::runtime_error("Invalid startup message from proxy thread (didn't get expected READY message)");
    LMQ_LOG(debug, "Proxy thread is ready");
}


void LokiMQ::worker_thread(unsigned int index) {
    std::string worker_id = "w" + std::to_string(index);
    zmq::socket_t sock{context, zmq::socket_type::dealer};
#if ZMQ_VERSION >= ZMQ_MAKE_VERSION (4, 3, 0)
    sock.setsockopt(ZMQ_ROUTING_ID, worker_id.data(), worker_id.size());
#else
    sock.setsockopt(ZMQ_IDENTITY, worker_id.data(), worker_id.size());
#endif
    LMQ_LOG(debug, "New worker thread ", worker_id, " started");
    sock.connect(SN_ADDR_WORKERS);

    Message message{*this};
    std::vector<zmq::message_t> parts;
    run_info& run = workers[index]; // This is our first job, and will be updated later with subsequent jobs

    while (true) {
        try {
            message.pubkey = {run.pubkey.data(), 32};
            message.service_node = run.service_node;
            message.data.clear();
            for (auto& m : run.message_parts)
                message.data.emplace_back(m.data<char>(), m.size());

            LMQ_LOG(trace, "worker thread ", worker_id, " invoking ", run.command, " callback with ", message.data.size(), " message parts");
            (*run.callback)(message);

            /*
             * FIXME: BYE should be handled by the proxy thread, not the worker.
             */
            /*
            if (msg.command == "BYE") {
                LMQ_LOG(info, "peer asked us to disconnect");
                detail::send_control(get_control_socket(), "DISCONNECT", {{"pubkey",msg.pubkey}});
                continue;
            }
            */

            /* FIXME: this lookup and auth check belongs in the proxy */
            /*
            auto cmdit = commands.find(msg.command);
            if (cmdit == commands.end()) {
                LMQ_LOG(warn, worker_id, " received unknown command '", msg.command, "' from " <<
                        (msg.sn ? "SN " : "non-SN ") << to_hex(msg.pubkey));
                continue;
            }

            auto cmd_type = cmdit->second.second;
            const bool command_accepted = (
                cmd_type == command_type::response ? msg.sn :
                cmd_type == command_type::quorum ? msg.sn && is_service_node() :
                cmd_type == command_type::public_ ? is_service_node() :
                false);
            if (!command_accepted) {
                // If they aren't valid, tell them so that they can disconnect (and attempt to reconnect later with appropriate authentication)
                LMQ_LOG(warn, worker_id, "/", object_id, " received disallowed ", cmd_type, " command ", msg.command <<
                        " from " << (msg.sn ? "non-" : "") << "SN remote " << to_hex(msg.pubkey) << "; replying with a BYE");
                send(msg.pubkey, "BYE", send_option::incoming{});
                detail::send_control(get_control_socket(), "DISCONNECT", {{"pubkey",msg.pubkey}});
                continue;
            }

            */
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
            detail::send_control(sock, "READY");
            LMQ_LOG(trace, "worker ", worker_id, " waiting for requests");
            parts.clear();
            recv_message_parts(sock, std::back_inserter(parts));

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

void LokiMQ::proxy_quit() {
    LMQ_LOG(debug, "Received quit command, shutting down proxy thread");

    assert(std::none_of(workers.begin(), workers.end(), [](auto& worker) { return worker.thread.joinable(); }));

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
    if (listener.connected()) {
        listener.setsockopt(ZMQ_LINGER, linger);
        listener.close();
    }
    for (auto &r : remotes)
        r.second.setsockopt(ZMQ_LINGER, linger);
    remotes.clear();
    peers.clear(); 

    LMQ_LOG(debug, "Proxy thread teardown complete");
}

std::pair<zmq::socket_t *, std::string>
LokiMQ::proxy_connect(const std::string &remote, const std::string &connect_hint, bool optional, bool incoming_only, std::chrono::milliseconds keep_alive) {
    auto &peer = peers[remote]; // We may auto-vivify here, but that's okay; it'll get cleaned up in idle_expiry if no connection gets established

    std::pair<zmq::socket_t *, std::string> result = {nullptr, ""s};

    bool outgoing = false;
    if (peer.outgoing >= 0 && !incoming_only) {
        result.first = &remotes[peer.outgoing].second;
        outgoing = true;
    } else if (!peer.incoming.empty() && listener.connected()) {
        result.first = &listener;
        result.second = peer.incoming;
    }

    if (result.first) {
        LMQ_LOG(trace, "proxy asked to connect to ", to_hex(remote), "; reusing existing connection");
        if (outgoing) {
            if (peer.idle_expiry < keep_alive) {
                LMQ_LOG(debug, "updating existing outgoing peer connection idle expiry time from ",
                        peer.idle_expiry.count(), "ms to ", keep_alive.count(), "ms");
                peer.idle_expiry = keep_alive;
            }
            peer.activity();
        }
        return result;
    } else if (optional || incoming_only) {
        LMQ_LOG(debug, "proxy asked for optional or incoming connection, but no appropriate connection exists so cancelling connection attempt");
        return result;
    }

    // No connection so establish a new one
    LMQ_LOG(debug, "proxy establishing new outbound connection to ", to_hex(remote));
    std::string addr;
    if (remote == pubkey) {
        // special inproc connection if self that doesn't need any external connection
        addr = SN_ADDR_SELF;
    } else {
        addr = connect_hint;
        if (addr.empty())
            addr = peer_lookup(remote);
        else
            LMQ_LOG(debug, "using connection hint ", connect_hint);

        if (addr.empty()) {
            LMQ_LOG(error, "peer lookup failed for ", to_hex(remote));
            return result;
        }
    }

    LMQ_LOG(debug, to_hex(pubkey), " connecting to ", addr, " to reach ", to_hex(remote));
    zmq::socket_t socket{context, zmq::socket_type::dealer};
    socket.setsockopt(ZMQ_CURVE_SERVERKEY, remote.data(), remote.size());
    socket.setsockopt(ZMQ_CURVE_PUBLICKEY, pubkey.data(), pubkey.size());
    socket.setsockopt(ZMQ_CURVE_SECRETKEY, privkey.data(), privkey.size());
    socket.setsockopt(ZMQ_HANDSHAKE_IVL, SN_HANDSHAKE_TIME);
    socket.setsockopt<int64_t>(ZMQ_MAXMSGSIZE, SN_ZMQ_MAX_MSG_SIZE);
#if ZMQ_VERSION >= ZMQ_MAKE_VERSION (4, 3, 0)
    socket.setsockopt(ZMQ_ROUTING_ID, pubkey.data(), pubkey.size());
#else
    socket.setsockopt(ZMQ_IDENTITY, pubkey.data(), pubkey.size());
#endif
    socket.connect(addr);
    peer.idle_expiry = keep_alive;

    add_pollitem(socket);
    peer.outgoing = remotes.size();
    remotes.emplace_back(remote, std::move(socket));
    peer.service_node = true;
    peer.activity();

    result.first = &remotes.back().second;
    return result;
}

std::pair<zmq::socket_t *, std::string> LokiMQ::proxy_connect(bt_dict &&data) {
    auto remote_pubkey = data.at("pubkey").get<std::string>();
    std::chrono::milliseconds keep_alive{get_int<int>(data.at("keep-alive"))};
    std::string hint;
    auto hint_it = data.find("hint");
    if (hint_it != data.end())
        hint = data.at("hint").get<std::string>();

    bool optional = data.count("optional"), incoming = data.count("incoming");

    return proxy_connect(remote_pubkey, hint, optional, incoming, keep_alive);
}

void LokiMQ::proxy_send(bt_dict &&data) {
    const auto &remote_pubkey = data.at("pubkey").get<std::string>();
    std::string hint;
    auto hint_it = data.find("hint");
    if (hint_it != data.end())
        hint = hint_it->second.get<std::string>();

    auto idle_it = data.find("keep-alive");
    std::chrono::milliseconds keep_alive = idle_it != data.end()
        ? std::chrono::milliseconds{get_int<uint64_t>(idle_it->second)}
        : DEFAULT_SEND_KEEP_ALIVE;

    bool optional = data.count("optional"), incoming = data.count("incoming");

    auto sock_route = proxy_connect(remote_pubkey, hint, optional, incoming, keep_alive);
    if (!sock_route.first) {
        if (optional)
            LMQ_LOG(debug, "Not sending: send is optional and no connection to ", to_hex(remote_pubkey), " is currently established");
        else
            LMQ_LOG(error, "Unable to send to ", to_hex(remote_pubkey), ": no connection could be established");
        return;
    }
    try {
        send_message_parts(*sock_route.first, build_send_parts(data, sock_route.second));
    } catch (const zmq::error_t &e) {
        if (e.num() == EHOSTUNREACH && sock_route.first == &listener && !sock_route.second.empty()) {
            // We *tried* to route via the incoming connection but it is no longer valid.  Drop it,
            // establish a new connection, and try again.
            auto &peer = peers[remote_pubkey];
            peer.incoming.clear(); // Don't worry about cleaning the map entry if outgoing is also < 0: that will happen at the next idle cleanup
            LMQ_LOG(debug, "Could not route back to SN ", to_hex(remote_pubkey), " via listening socket; trying via new outgoing connection");
            return proxy_send(std::move(data));
        }
        LMQ_LOG(warn, "Unable to send message to remote SN ", to_hex(remote_pubkey), ": ", e.what());
    }
}

void LokiMQ::proxy_reply(bt_dict &&data) {
    const auto &route = data.at("route").get<std::string>();
    assert(!route.empty());
    if (!listener.connected()) {
        LMQ_LOG(error, "Internal error: proxy_reply called but that shouldn't be possible as we have no listener!");
        return;
    }

    try {
        send_message_parts(listener, build_send_parts(data, route));
    } catch (const zmq::error_t &err) {
        if (err.num() == EHOSTUNREACH) {
            LMQ_LOG(info, "Unable to send reply to incoming non-SN request: remote is no longer connected");
        } else {
            LMQ_LOG(warn, "Unable to send reply to incoming non-SN request: ", err.what());
        }
    }
}

void LokiMQ::proxy_control_message(std::vector<zmq::message_t> parts) {
    if (parts.size() < 2 || parts.size() > 3)
        throw std::logic_error("Expected 2-3 message parts for a proxy control message");
    auto route = view(parts[0]), cmd = view(parts[1]);
    bt_dict data;
    if (parts.size() > 2) {
        bt_deserialize(view(parts[2]), data);
    }
    LMQ_LOG(trace, "control message: ", cmd);
    if (cmd == "START") {
        // Command send by the owning thread during startup; we send back a simple READY reply to
        // let it know we are running.
        route_control(command, route, "READY");
    } else if (cmd == "QUIT") {
        // Asked to quit: set max_workers to zero and tell any idle ones to quit.  We will
        // close workers as they come back to READY status, and then close external
        // connections once all workers are done.
        max_workers = 0;
        for (const auto &route : idle_workers)
            route_control(workers_socket, workers[route].routing_id, "QUIT");
        idle_workers.clear();
    } else if (cmd == "CONNECT") {
        proxy_connect(std::move(data));
    } else if (cmd == "DISCONNECT") {
        proxy_disconnect(data.at("pubkey").get<std::string>());
    } else if (cmd == "SEND") {
        LMQ_LOG(trace, "proxying message to ", to_hex(data.at("pubkey").get<std::string>()));
        proxy_send(std::move(data));
    } else if (cmd == "REPLY") {
        LMQ_LOG(trace, "proxying reply to non-SN incoming message");
        proxy_reply(std::move(data));
    } else {
        throw std::runtime_error("Proxy received invalid control command: " + std::string{cmd} +
                " (" + std::to_string(parts.size()) + ")");
    }
}

auto LokiMQ::proxy_close_outgoing(decltype(peers)::iterator it) -> decltype(it) {
    auto &peer = *it;
    auto &info = peer.second;

    if (info.outgoing >= 0) {
        remotes[info.outgoing].second.setsockopt<int>(ZMQ_LINGER, std::chrono::milliseconds{CLOSE_LINGER}.count());
        pollitems.erase(pollitems.begin() + poll_remote_offset + info.outgoing);
        remotes.erase(remotes.begin() + info.outgoing);
        assert(remotes.size() == pollitems.size() + poll_remote_offset);

        for (auto &p : peers)
            if (p.second.outgoing > info.outgoing)
                --p.second.outgoing;

        info.outgoing = -1;
    }

    if (info.incoming.empty())
        // Neither incoming nor outgoing connections left, so erase the peer info
        return peers.erase(it);

    return std::next(it);
}

void LokiMQ::proxy_disconnect(const std::string &remote) {
    auto it = peers.find(remote);
    if (it == peers.end())
        return;
    if (it->second.outgoing >= 0)
        LMQ_LOG(debug, "Closing outgoing connection to ", to_hex(it->first));
    proxy_close_outgoing(it);
}

void LokiMQ::proxy_expire_idle_peers() {
    for (auto it = peers.begin(); it != peers.end(); ) {
        auto &info = it->second;
        if (info.outgoing >= 0) {
            auto idle = info.last_activity - std::chrono::steady_clock::now();
            if (idle <= info.idle_expiry) {
                ++it;
                continue;
            }
            LMQ_LOG(info, "Closing outgoing connection to ", to_hex(it->first), ": idle timeout reached");
        }

        // Deliberately outside the above if: this *also* removes the peer from the map if if has
        // neither an incoming or outgoing connection
        it = proxy_close_outgoing(it);
    }
}

void LokiMQ::proxy_loop() {
    zmq::socket_t zap_auth{context, zmq::socket_type::rep};
    zap_auth.setsockopt<int>(ZMQ_LINGER, 0);
    zap_auth.bind(ZMQ_ADDR_ZAP);

    workers_socket.setsockopt<int>(ZMQ_ROUTER_MANDATORY, 1);
    workers_socket.bind(SN_ADDR_WORKERS);

    if (!general_workers)
        general_workers = std::thread::hardware_concurrency();

    max_workers = general_workers;
    for (const auto& cat : categories)
        max_workers += cat.second.reserved_threads;

    workers.reserve(max_workers);
    if (!workers.empty())
        throw std::logic_error("Internal error: proxy thread started with active worker threads");

    add_pollitem(command);
    add_pollitem(workers_socket);
    add_pollitem(zap_auth);
    assert(pollitems.size() == poll_internal_size);

    if (!bind.empty()) {
        // Set up the public tcp listener(s):
        listener = {context, zmq::socket_type::router};
        listener.setsockopt(ZMQ_ZAP_DOMAIN, AUTH_DOMAIN_SN, sizeof(AUTH_DOMAIN_SN)-1);
        listener.setsockopt<int>(ZMQ_CURVE_SERVER, 1);
        listener.setsockopt(ZMQ_CURVE_PUBLICKEY, pubkey.data(), pubkey.size());
        listener.setsockopt(ZMQ_CURVE_SECRETKEY, privkey.data(), privkey.size());
        listener.setsockopt<int64_t>(ZMQ_MAXMSGSIZE, SN_ZMQ_MAX_MSG_SIZE);
        listener.setsockopt<int>(ZMQ_ROUTER_HANDOVER, 1);
        listener.setsockopt<int>(ZMQ_ROUTER_MANDATORY, 1);

        for (const auto &b : bind) {
            LMQ_LOG(info, "LokiMQ listening on ", b);
            listener.bind(b);
        }

        // Also add an internal connection to self so that calling code can avoid needing to
        // special-case rare situations where we are supposed to talk to a quorum member that happens to
        // be ourselves (which can happen, for example, with cross-quoum Blink communication)
        listener.bind(SN_ADDR_SELF);

        add_pollitem(listener);
    }

    assert(pollitems.size() == poll_remote_offset);

    constexpr auto poll_timeout = 5000ms; // Maximum time we spend in each poll
    constexpr auto timeout_check_interval = 10000ms; // Minimum time before for checking for connections to close since the last check
    auto last_conn_timeout = std::chrono::steady_clock::now();

    size_t last_conn_index = 0; // Index of the connection we last received a message from; see below

    std::vector<zmq::message_t> parts;

    while (true) {
        if (max_workers == 0) { // Will be 0 only if we are quitting
            if (workers.empty()) {
                // All the workers have finished, so we can finish shutting down
                return proxy_quit();
            }
        }

        // We poll the control socket and worker socket for any incoming messages.  If we have
        // available worker room then also poll incoming connections and outgoing connections for
        // messages to forward to a worker.  Otherwise, we just look for a control message or a
        // worker coming back with a ready message.
        bool workers_available = idle_workers.size() > 0 || workers.size() < max_workers;
        zmq::poll(pollitems.data(), workers_available ? pollitems.size() : poll_internal_size, poll_timeout);

        LMQ_LOG(trace, "processing control messages");
        // Retrieve any waiting incoming control messages
        for (parts.clear(); recv_message_parts(command, std::back_inserter(parts), zmq::recv_flags::dontwait); parts.clear()) {
            proxy_control_message(std::move(parts));
        }

        LMQ_LOG(trace, "processing worker messages");
        // Process messages sent by workers
        for (parts.clear(); recv_message_parts(workers_socket, std::back_inserter(parts), zmq::recv_flags::dontwait); parts.clear()) {
            if (parts.size() != 2) {
                LMQ_LOG(error, "Received send invalid ", parts.size(), "-part message");
                continue;
            }
            auto route = view(parts[0]), cmd = view(parts[1]);
            assert(route.size() >= 2 && route[0] == 'w' && route[1] >= '1' && route[1] <= '9');
            string_view worker_id_str{&route[1], route.size()-1}; // Chop off the leading "w"
            unsigned int worker_id = detail::extract_unsigned(worker_id_str);
            if (!worker_id_str.empty() /* didn't consume everything */ || worker_id >= workers.size()) {
                LMQ_LOG(error, "Worker id '", route, "' is invalid, unable to process worker command");
                continue;
            }

            LMQ_LOG(trace, "received ", cmd, " command from ", route);
            if (cmd == "READY") {
                LMQ_LOG(debug, "Worker ", route, " is ready");
                if (max_workers == 0) { // Shutting down
                    LMQ_LOG(trace, "Telling worker ", route, " to quit");
                    route_control(workers_socket, route, "QUIT");
                } else {
                    idle_workers.push_back(worker_id);
                }
            } else if (cmd == "QUITTING") {
                workers[worker_id].thread.join();
                LMQ_LOG(debug, "Worker ", route, " exited normally");
            } else {
                LMQ_LOG(error, "Worker ", route, " sent unknown control message: `", cmd, "'");
            }
        }

        // Handle any zap authentication
        LMQ_LOG(trace, "processing zap requests");
        process_zap_requests(zap_auth);

        workers_available = idle_workers.size() > 0 || workers.size() < max_workers; // recheck - idle_workers could have changed above
        if (max_workers > 0 && workers_available) {
            LMQ_LOG(trace, "processing incoming messages");

            // FIXME process any queued messages (i.e. that couldn't run but we had reserved tasks
            // still free) first

            // We round-robin connection queues for any pending messages (as long as we have enough
            // waiting workers), but we don't want a lot of earlier connection requests to starve
            // later request so each time through we continue from wherever we left off in the
            // previous queue.

            const size_t num_sockets = remotes.size() + listener.connected();
            if (last_conn_index >= num_sockets)
                last_conn_index = 0;
            std::queue<size_t> queue_index;
            for (size_t i = 1; i <= num_sockets; i++)
                queue_index.push((last_conn_index + i) % num_sockets);

            for (parts.clear(); !queue_index.empty() && workers.size() < max_workers; parts.clear()) {
                size_t i = queue_index.front();
                queue_index.pop();
                auto &sock = listener.connected() ? (i == 0 ? listener : remotes[i - 1].second) : remotes[i].second;

                if (!recv_message_parts(sock, std::back_inserter(parts), zmq::recv_flags::dontwait))
                    continue;

                last_conn_index = i;
                queue_index.push(i); // We just read one, but there might be more messages waiting so requeue it at the end

                if (parts.empty()) {
                    LMQ_LOG(warn, "Ignoring empty (0-part) incoming message");
                    continue;
                }

                if (proxy_handle_builtin(last_conn_index, parts)) continue;

                proxy_to_worker(last_conn_index, parts);
            }
        }

        // Drop idle connections (if we haven't done it in a while) but *only* if we have some idle
        // general workers: if we don't have any idle workers then we may still have incoming messages which
        // we haven't processed yet and those messages might end up reset the last activity time.
        if (workers.size() < general_workers) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_conn_timeout >= timeout_check_interval) {
                LMQ_LOG(trace, "closing idle connections");
                proxy_expire_idle_peers();
                last_conn_timeout = now;
            }
        }

        LMQ_LOG(trace, "done proxy loop");
    }
}

std::pair<const LokiMQ::category*, const LokiMQ::CommandCallback*> LokiMQ::get_command(std::string& command) {
    if (command.size() > MAX_CATEGORY_LENGTH + 1 + MAX_COMMAND_LENGTH) {
        LMQ_LOG(warn, "Invalid command '", command, "': command too long");
        return {};
    }

    if (!command_aliases.empty()) {
        auto it = command_aliases.find(command);
        if (it != command_aliases.end())
            command = it->second;
    }

    auto dot = command.find('.');
    if (dot == 0 || dot == std::string::npos) {
        LMQ_LOG(warn, "Invalid command '", command, "': expected <category>.<command>");
        return {};
    }
    string_view catname{&command[0], dot};
    std::string cmd = command.substr(dot + 1);

    auto catit = categories.find(catname);
    if (catit == categories.end()) {
        LMQ_LOG(warn, "Invalid command category '", catname, "'");
        return {};
    }

    const auto& category = catit->second;
    auto callback_it = category.commands.find(cmd);
    if (callback_it == category.commands.end()) {
        LMQ_LOG(warn, "Invalid command '", command, "'");
        return {};
    }

    return {&catit->second, &callback_it->second};
}

std::pair<std::string, LokiMQ::peer_info> &LokiMQ::proxy_lookup_peer(zmq::message_t& msg, ) {
    std::string pubkey;
    bool service_node;
    try {
        extract_pubkey(parts.back(), pubkey, service_node);
    } catch (...) {
        LMQ_LOG(error, "Internal error: socket User-Id not set or invalid; dropping message");
        return;
    }

    auto it = peers.find(pubkey);
    if (it == peers.end())
        it = peers.emplace(std::move(pubkey), peer_info{});
    auto &peer_info = peers[pubkey];
    peer_info.service_node |= service_node;

}


bool LokiMQ::proxy_handle_builtin(size_t conn_index, std::vector<zmq::message_t>& parts) {
    auto cmd = view(parts.front());
    if (cmd == "BYE") {
        
    }
}

void LokiMQ::proxy_to_worker(size_t conn_index, std::vector<zmq::message_t>& parts) {
    std::string pubkey;
    bool service_node;
    try {
        extract_pubkey(parts.back(), pubkey, service_node);
    } catch (...) {
        LMQ_LOG(error, "Internal error: socket User-Id not set or invalid; dropping message");
        return;
    }

    bool is_outgoing_conn = !listener.connected() || conn_index > 0;
    size_t command_part_index = is_outgoing_conn ? 0 : 1;
    std::string command = parts[command_part_index].to_string();
    auto cat_call = get_command(command);

    if (!cat_call.first) {
        if (is_outgoing_conn)
            send_direct_message(remotes[conn_index - listener.connected()].second, "UNKNOWNCOMMAND", command);
        else
            send_routed_message(listener, pubkey, "UNKNOWNCOMMAND", command);
        return;
    }

    auto& category = *cat_call.first;

    auto &peer_info = peers[pubkey];
    peer_info.service_node |= service_node;

    if (!proxy_check_auth(pubkey, conn_index, peer_info, command, category, parts.back()))
        return;

    bool can_work = false;
    if (category.active_threads < category.reserved_threads)
        // Our category still has reserved spots so we get to run even if it would exceed
        // general_workers.
        can_work = true;
    else {
        // We don't have a reserved spot, so the only way we get to run now is if there is an idle
        // worker *and* we don't already have >= `general_workers` threads already doing things.
        unsigned int working_threads = workers.size() - idle_workers.size();
        if (working_threads < general_workers)
            can_work = true;
    }

    if (!can_work) {
        // We can't handle this now so queue it for later consideration when some workers free up.
        LMQ_LOG(debug, "No available free workers, queuing task for later");
        // FIXME TODO
        return;
    }

    size_t index;
    std::unique_ptr<run_info> new_run;
    if (idle_workers.empty()) {
        index = workers.size();
        assert(workers.capacity() > index);
        new_run = std::make_unique<run_info>();
        new_run->routing_id = "w" + std::to_string(index);
    } else {
        index = idle_workers.back();
    }

    auto& run = new_run ? *new_run : workers[index];
    run.pubkey = std::move(pubkey);
    run.service_node = service_node;

    if (is_outgoing_conn) {
        peer_info.activity(); // outgoing connection activity, pump the activity timer
    } else {
        // incoming connection; the route is the first argument.  Update the peer route info in case
        // it has changed (e.g. new connection).
        auto route = view(parts[0]);
        if (string_view(peer_info.incoming) != route)
            peer_info.incoming = route;
    }

    LMQ_LOG(trace, __FILE__, __LINE__, "Invoking incoming ", command, " from ",
            run.service_node ? "SN " : "non-SN ", to_hex(run.pubkey), " @ ", peer_address(parts.back()),
            " on worker ", index);

    run.command = std::move(command);
    run.callback = cat_call.second;

    // Steal any extra argument messages (after the command name)
    run.message_parts.clear();
    for (size_t i = (command_part_index + 1); i < parts.size(); i++)
        run.message_parts.push_back(std::move(parts[i]));

    if (new_run) {
        // The thread processes the first job immediately upon startup, so just start it and don't
        // send anything.
        workers.push_back(std::move(run));
        workers.back().thread = std::thread{&LokiMQ::worker_thread, this, index};
    } else {
        // The worker is idling, send it a RUN (prefixed with the route, for the ROUTER socket)
        // to kick it into action
        idle_workers.pop_back();
        send_routed_message(workers_socket, run.routing_id, "RUN");
    }
}

bool LokiMQ::proxy_check_auth(const std::string& pubkey, size_t conn_index, const peer_info& peer, const std::string& command, const category& cat, zmq::message_t& msg) {
    bool is_outgoing_conn = !listener.connected() || conn_index > 0;
    std::string reply;
    if (peer.auth_level < cat.access.auth) {
        LMQ_LOG(warn, "Access denied to ", command, " for peer ", to_hex(pubkey), " @ ", peer_address(msg),
                ": peer auth level ", to_string(peer.auth_level), " < ", to_string(cat.access.auth));
        reply = "FORBIDDEN";
    }
    else if (cat.access.local_sn && !local_service_node) {
        LMQ_LOG(warn, "Access denied to ", command, " for peer ", to_hex(pubkey), " @ ", peer_address(msg),
                ": that command is only available when this LokiMQ is running in service node mode");
        reply = "NOT_A_SERVICE_NODE";
    }
    else if (cat.access.remote_sn && !peer.service_node) {
        LMQ_LOG(warn, "Access denied to ", command, " for peer ", to_hex(pubkey), " @ ", peer_address(msg),
                ": remote is not recognized as a service node");
        // Disconnect: we don't think the remote is a SN, but it issued a command only SNs should be
        // issuing.  Drop the connection; if the remote has something important to relay it will
        // reconnect, at which point we will reassess the SN status on the new incoming connection.
        if (!is_outgoing_conn)
            send_routed_message(listener, pubkey, "BYE");
        else
            proxy_disconnect(pubkey);
        return false;
    }

    if (reply.empty())
        return true;

    if (is_outgoing_conn)
        send_direct_message(remotes[conn_index - listener.connected()].second, std::move(reply), command);
    else
        send_routed_message(listener, pubkey, std::move(reply), command);
    return true;
}

void LokiMQ::process_zap_requests(zmq::socket_t &zap_auth) {
    std::vector<zmq::message_t> frames;
    for (frames.reserve(7); recv_message_parts(zap_auth, std::back_inserter(frames), zmq::recv_flags::dontwait); frames.clear()) {
        if (LogLevel::trace >= log_level()) {
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
        } else {
            LMQ_LOG(debug, "Processing ZAP authentication request");
        }

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
            response_vals[1] = view(frames[1]); // unique identifier
        std::string &status_code = response_vals[2], &status_text = response_vals[3];

        if (frames.size() < 6 || view(frames[0]) != "1.0") {
            LMQ_LOG(error, "Bad ZAP authentication request: version != 1.0 or invalid ZAP message parts");
            status_code = "500";
            status_text = "Internal error: invalid auth request";
        }
        else if (frames.size() != 7 || view(frames[5]) != "CURVE") {
            LMQ_LOG(error, "Bad ZAP authentication request: invalid CURVE authentication request");
            status_code = "500";
            status_text = "Invalid CURVE authentication request\n";
        }
        else if (frames[6].size() != 32) {
            LMQ_LOG(error, "Bad ZAP authentication request: invalid request pubkey");
            status_code = "500";
            status_text = "Invalid public key size for CURVE authentication";
        }
        else {
            auto domain = view(frames[2]);
            if (domain != AUTH_DOMAIN_SN) {
                LMQ_LOG(error, "Bad ZAP authentication request: invalid auth domain '", domain, "'");
                status_code = "400";
                status_text = "Unknown authentication domain: " + std::string{domain};
            } else {
                auto ip = view(frames[3]), pubkey = view(frames[6]);
                auto result = allow_connection(ip, pubkey);
                bool sn = result.remote_sn;
                auto& user_id = response_vals[4];
                user_id.reserve(64);
                to_hex(pubkey.begin(), pubkey.end(), std::back_inserter(user_id));

                if (result.auth <= AuthLevel::denied || result.auth > AuthLevel::admin) {
                    LMQ_LOG(info, "Access denied for incoming ", (sn ? "service node" : "non-SN client"),
                            " connection from ", user_id, " at ", ip, " with initial auth level ", to_string(result.auth));
                    status_code = "400";
                    status_text = "Access denied";
                    user_id.clear();
                }
                LMQ_LOG(info, "Accepted incoming ", (sn ? "service node" : "non-SN client"),
                        " connection with authentication level ", to_string(result.auth),
                        " from ", string_view{&user_id[2], user_id.size()-2}, " at ", ip);

                auto& metadata = response_vals[5];
                if (result.remote_sn)
                    metadata += zmtp_metadata("X-SN", "1");
                if (result.auth != AuthLevel::none)
                    metadata += zmtp_metadata("X-AuthLevel", to_string(result.auth));

                status_code = "200";
                status_text = "";
            }
        }

        LMQ_LOG(trace, "ZAP request result: ", status_code, " ", status_text);

        std::vector<zmq::message_t> response;
        response.reserve(response_vals.size());
        for (auto &r : response_vals) response.push_back(create_message(std::move(r)));
        send_message_parts(zap_auth, response.begin(), response.end());
    }
}

LokiMQ::~LokiMQ() {
    LMQ_LOG(info, "LokiMQ shutting down proxy thread");
    detail::send_control(get_control_socket(), "QUIT");
    proxy_thread.join();
    LMQ_LOG(info, "LokiMQ proxy thread has stopped");
}

void LokiMQ::connect(const std::string &pubkey, std::chrono::milliseconds keep_alive, const std::string &hint) {
    detail::send_control(get_control_socket(), "CONNECT", bt_serialize<bt_dict>({{"pubkey",pubkey}, {"keep-alive",keep_alive.count()}, {"hint",hint}}));
}


}

// vim:sw=4:et
