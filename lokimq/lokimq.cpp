#include "lokimq.h"
#include <map>
#include <random>

extern "C" {
#include <sodium.h>
}
#include "batch.h"
#include "hex.h"

namespace lokimq {

constexpr char SN_ADDR_COMMAND[] = "inproc://sn-command";
constexpr char SN_ADDR_WORKERS[] = "inproc://sn-workers";
constexpr char SN_ADDR_SELF[] = "inproc://sn-self";
constexpr char ZMQ_ADDR_ZAP[] = "inproc://zeromq.zap.01";

// Inside some method:
//     LMQ_LOG(warn, "bad ", 42, " stuff");
#define LMQ_LOG(level, ...) log_(LogLevel::level, __FILE__, __LINE__, __VA_ARGS__)

#ifndef NDEBUG
// Same as LMQ_LOG(trace, ...) when not doing a release build; nothing under a release build.
#  define LMQ_TRACE(...) log_(LogLevel::trace, __FILE__, __LINE__, __VA_ARGS__)
#else
#  define LMQ_TRACE(...)
#endif



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
bool recv_message_parts(zmq::socket_t &sock, std::vector<zmq::message_t>& parts, const zmq::recv_flags flags = zmq::recv_flags::none) {
    do {
        zmq::message_t msg;
        if (!sock.recv(msg, flags))
            return false;
        parts.push_back(std::move(msg));
    } while (parts.back().more());
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

/// Sends a message with an initial route.  `msg` and `data` can be empty: if `msg` is empty then
/// the msg frame will be an empty message; if `data` is empty then the data frame will be omitted.
void send_routed_message(zmq::socket_t &socket, std::string route, std::string msg = {}, std::string data = {}) {
    assert(!route.empty());
    std::array<zmq::message_t, 3> msgs{{create_message(std::move(route))}};
    if (!msg.empty())
        msgs[1] = create_message(std::move(msg));
    if (!data.empty())
        msgs[2] = create_message(std::move(data));
    send_message_parts(socket, msgs.begin(), data.empty() ? std::prev(msgs.end()) : msgs.end());
}

// Sends some stuff to a socket directly.
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

// Returns a string view of the given message data.  It's the caller's responsibility to keep the
// referenced message alive.  If you want a std::string instead just call `m.to_string()`
string_view view(const zmq::message_t& m) {
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

void check_started(const std::thread& proxy_thread, const std::string &verb) {
    if (!proxy_thread.joinable())
        throw std::logic_error("Cannot " + verb + " before calling `start()`");
}

void check_not_started(const std::thread& proxy_thread, const std::string &verb) {
    if (proxy_thread.joinable())
        throw std::logic_error("Cannot " + verb + " after calling `start()`");
}

// Extracts and builds the "send" part of a message for proxy_send/proxy_reply
std::list<zmq::message_t> build_send_parts(bt_list_consumer send, string_view route) {
    std::list<zmq::message_t> parts;
    if (!route.empty())
        parts.push_back(create_message(route));
    while (!send.is_finished())
        parts.push_back(create_message(send.consume_string()));
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
AuthLevel auth_from_string(string_view a) {
    if (a == "none") return AuthLevel::none;
    if (a == "basic") return AuthLevel::basic;
    if (a == "admin") return AuthLevel::admin;
    return AuthLevel::denied;
}

/// Extracts a pubkey, SN status, and auth level from a zmq message received on a *listening*
/// socket.
std::tuple<std::string, bool, AuthLevel> extract_metadata(zmq::message_t& msg) {
    auto result = std::make_tuple(""s, false, AuthLevel::none);
    try {
        string_view pubkey_hex{msg.gets("User-Id")};
        if (pubkey_hex.size() != 64)
            throw std::logic_error("bad user-id");
        assert(is_hex(pubkey_hex.begin(), pubkey_hex.end()));
        auto& pubkey = std::get<std::string>(result);
        pubkey.resize(32, 0);
        from_hex(pubkey_hex.begin(), pubkey_hex.end(), pubkey.begin());
    } catch (...) {}

    try {
        string_view is_sn{msg.gets("X-SN")};
        if (is_sn.size() == 1 && is_sn[0] == '1')
            std::get<bool>(result) = true;
    } catch (...) {}

    try {
        string_view auth_level{msg.gets("X-AuthLevel")};
        std::get<AuthLevel>(result) = auth_from_string(auth_level);
    } catch (...) {}

    return result;
}

const char* peer_address(zmq::message_t& msg) {
    try { return msg.gets("Peer-Address"); } catch (...) {}
    return "(unknown)";
}

void add_pollitem(std::vector<zmq::pollitem_t>& pollitems, zmq::socket_t& sock) {
    pollitems.emplace_back();
    auto &p = pollitems.back();
    p.socket = static_cast<void *>(sock);
    p.fd = 0;
    p.events = ZMQ_POLLIN;
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

std::ostream& operator<<(std::ostream& o, AuthLevel a) {
    return o << to_string(a);
}

std::ostream& operator<<(std::ostream& o, const ConnectionID& conn) {
    if (!conn.pk.empty())
        return o << (conn.sn() ? "SN " : "non-SN authenticated remote ") << to_hex(conn.pk);
    else
        return o << "unauthenticated remote [" << conn.id << "]";
}


void LokiMQ::rebuild_pollitems() {
    pollitems.clear();
    add_pollitem(pollitems, command);
    add_pollitem(pollitems, workers_socket);
    add_pollitem(pollitems, zap_auth);

    for (auto& s : connections)
        add_pollitem(pollitems, s);
}

void LokiMQ::log_level(LogLevel level) {
    log_lvl.store(level, std::memory_order_relaxed);
}

LogLevel LokiMQ::log_level() const {
    return log_lvl.load(std::memory_order_relaxed);
}


CatHelper LokiMQ::add_category(std::string name, Access access_level, unsigned int reserved_threads, int max_queue) {
    check_not_started(proxy_thread, "add a category");

    if (name.size() > MAX_CATEGORY_LENGTH)
        throw std::runtime_error("Invalid category name `" + name + "': name too long (> " + std::to_string(MAX_CATEGORY_LENGTH) + ")");

    if (name.empty() || name.find('.') != std::string::npos)
        throw std::runtime_error("Invalid category name `" + name + "'");

    auto it = categories.find(name);
    if (it != categories.end())
        throw std::runtime_error("Unable to add category `" + name + "': that category already exists");

    CatHelper ret{*this, name};
    categories.emplace(std::move(name), category{access_level, reserved_threads, max_queue});
    return ret;
}

void LokiMQ::add_command(const std::string& category, std::string name, CommandCallback callback) {
    check_not_started(proxy_thread, "add a command");

    if (name.size() > MAX_COMMAND_LENGTH)
        throw std::runtime_error("Invalid command name `" + name + "': name too long (> " + std::to_string(MAX_COMMAND_LENGTH) + ")");

    auto catit = categories.find(category);
    if (catit == categories.end())
        throw std::runtime_error("Cannot add a command to unknown category `" + category + "'");

    std::string fullname = category + '.' + name;
    if (command_aliases.count(fullname))
        throw std::runtime_error("Cannot add command `" + fullname + "': a command alias with that name is already defined");

    auto ins = catit->second.commands.insert({std::move(name), {std::move(callback), false}});
    if (!ins.second)
        throw std::runtime_error("Cannot add command `" + fullname + "': that command already exists");
}

void LokiMQ::add_request_command(const std::string& category, std::string name, CommandCallback callback) {
    add_command(category, name, std::move(callback));
    categories.at(category).commands.at(name).second = true;
}

void LokiMQ::add_command_alias(std::string from, std::string to) {
    check_not_started(proxy_thread, "add a command alias");

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
    assert(proxy_thread.joinable());

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
        SNRemoteAddress lookup,
        Logger logger)
    : object_id{next_id++}, pubkey{std::move(pubkey_)}, privkey{std::move(privkey_)}, local_service_node{service_node},
        sn_lookup{std::move(lookup)}, logger{std::move(logger)}
{

    LMQ_TRACE("Constructing listening LokiMQ, id=", object_id, ", this=", this);

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
        std::string verify_pubkey(crypto_box_PUBLICKEYBYTES, 0);
        crypto_scalarmult_base(reinterpret_cast<unsigned char*>(&verify_pubkey[0]), reinterpret_cast<unsigned char*>(&privkey[0]));
        if (verify_pubkey != pubkey)
            throw std::invalid_argument("Invalid pubkey/privkey values given to LokiMQ construction: pubkey verification failed");
    }
}

void LokiMQ::start() {
    if (proxy_thread.joinable())
        throw std::logic_error("Cannot call start() multiple times!");

    // If we're not binding to anything then we don't listen, i.e. we can only establish outbound
    // connections.  Don't allow this if we are in service_node mode because, if we aren't
    // listening, we are useless as a service node.
    if (bind.empty() && local_service_node)
        throw std::invalid_argument{"Cannot create a service node listener with no address(es) to bind"};

    LMQ_LOG(info, "Initializing LokiMQ ", bind.empty() ? "remote-only" : "listener", " with pubkey ", to_hex(pubkey));

    // We bind `command` here so that the `get_control_socket()` below is always connecting to a
    // bound socket, but we do nothing else here: the proxy thread is responsible for everything
    // except binding it.
    command.bind(SN_ADDR_COMMAND);
    proxy_thread = std::thread{&LokiMQ::proxy_loop, this};

    LMQ_LOG(debug, "Waiting for proxy thread to get ready...");
    auto &control = get_control_socket();
    detail::send_control(control, "START");
    LMQ_TRACE("Sent START command");

    zmq::message_t ready_msg;
    std::vector<zmq::message_t> parts;
    try { recv_message_parts(control, parts); }
    catch (const zmq::error_t &e) { throw std::runtime_error("Failure reading from LokiMQ::Proxy thread: "s + e.what()); }

    if (!(parts.size() == 1 && view(parts.front()) == "READY"))
        throw std::runtime_error("Invalid startup message from proxy thread (didn't get expected READY message)");
    LMQ_LOG(debug, "Proxy thread is ready");
}


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

void LokiMQ::setup_outgoing_socket(zmq::socket_t& socket, string_view remote_pubkey) {
    if (!remote_pubkey.empty()) {
        socket.setsockopt(ZMQ_CURVE_SERVERKEY, remote_pubkey.data(), remote_pubkey.size());
        socket.setsockopt(ZMQ_CURVE_PUBLICKEY, pubkey.data(), pubkey.size());
        socket.setsockopt(ZMQ_CURVE_SECRETKEY, privkey.data(), privkey.size());
    }
    socket.setsockopt(ZMQ_HANDSHAKE_IVL, (int) HANDSHAKE_TIME.count());
    socket.setsockopt<int64_t>(ZMQ_MAXMSGSIZE, MAX_MSG_SIZE);
    if (PUBKEY_BASED_ROUTING_ID) {
        std::string routing_id;
        routing_id.reserve(33);
        routing_id += 'L'; // Prefix because routing id's starting with \0 are reserved by zmq (and our pubkey might start with \0)
        routing_id.append(pubkey.begin(), pubkey.end());
        socket.setsockopt(ZMQ_ROUTING_ID, routing_id.data(), routing_id.size());
    }
    // else let ZMQ pick a random one
}

std::pair<zmq::socket_t *, std::string>
LokiMQ::proxy_connect_sn(string_view remote, string_view connect_hint, bool optional, bool incoming_only, std::chrono::milliseconds keep_alive) {
    ConnectionID remote_cid{remote};
    auto its = peers.equal_range(remote_cid);
    peer_info* peer = nullptr;
    for (auto it = its.first; it != its.second; ++it) {
        if (incoming_only && it->second.route.empty())
            continue; // outgoing connection but we were asked to only use incoming connections
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
    socket.connect(addr);
    peer_info p{};
    p.service_node = true;
    p.pubkey = std::string{remote};
    p.conn_index = connections.size();
    p.idle_expiry = keep_alive;
    p.activity();
    peers.emplace(std::move(remote_cid), std::move(p));
    connections.push_back(std::move(socket));

    return {&connections.back(), ""s};
}

std::pair<zmq::socket_t *, std::string> LokiMQ::proxy_connect_sn(bt_dict_consumer data) {
    string_view hint, remote_pk;
    std::chrono::milliseconds keep_alive;
    bool optional = false, incoming_only = false;

    // Alphabetical order
    if (data.skip_until("hint"))
        hint = data.consume_string();
    if (data.skip_until("incoming"))
        incoming_only = data.consume_integer<bool>();
    if (data.skip_until("keep-alive"))
        keep_alive = std::chrono::milliseconds{data.consume_integer<uint64_t>()};
    if (data.skip_until("optional"))
        optional = data.consume_integer<bool>();
    if (!data.skip_until("pubkey"))
        throw std::runtime_error("Internal error: Invalid proxy_connect_sn command; pubkey missing");
    remote_pk = data.consume_string();

    return proxy_connect_sn(remote_pk, hint, optional, incoming_only, keep_alive);
}

void LokiMQ::proxy_send(bt_dict_consumer data) {
    // NB: bt_dict_consumer goes in alphabetical order
    string_view hint;
    std::chrono::milliseconds keep_alive{DEFAULT_SEND_KEEP_ALIVE};
    bool optional = false;
    bool incoming = false;
    bool request = false;
    bool have_conn_id = false;
    ConnectionID conn_id;

    std::string request_tag;
    std::unique_ptr<ReplyCallback> request_cbptr;
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
    if (data.skip_until("keep-alive"))
        keep_alive = std::chrono::milliseconds{data.consume_integer<uint64_t>()};
    if (data.skip_until("optional"))
        optional = data.consume_integer<bool>();

    if (data.skip_until("request"))
        request = data.consume_integer<bool>();
    if (request) {
        if (!data.skip_until("request_callback"))
            throw std::runtime_error("Internal error: received request without request_callback");
        request_cbptr.reset(reinterpret_cast<ReplyCallback*>(data.consume_integer<uintptr_t>()));
        if (!data.skip_until("request_tag"))
            throw std::runtime_error("Internal error: received request without request_name");
        request_tag = data.consume_string();
    }
    if (!data.skip_until("send"))
        throw std::runtime_error("Internal error: Invalid proxy send command; send parts missing");
    bt_list_consumer send = data.consume_list_consumer();

    zmq::socket_t *send_to;
    if (conn_id.sn()) {
        auto sock_route = proxy_connect_sn(conn_id.pk, hint, optional, incoming, keep_alive);
        if (!sock_route.first) {
            if (optional)
                LMQ_LOG(debug, "Not sending: send is optional and no connection to ",
                        to_hex(conn_id.pk), " is currently established");
            else
                LMQ_LOG(error, "Unable to send to ", to_hex(conn_id.pk), ": no connection address found");
            return;
        }
        send_to = sock_route.first;
        conn_id.route = std::move(sock_route.second);
    } else if (!conn_id.route.empty()) { // incoming non-SN connection
        auto it = incoming_conn_index.find(conn_id);
        if (it == incoming_conn_index.end()) {
            LMQ_LOG(warn, "Unable to send to ", conn_id, ": incoming listening socket not found");
            return;
        }
        send_to = &connections[it->second];
    } else {
        auto pr = peers.equal_range(conn_id);
        if (pr.first == peers.end()) {
            LMQ_LOG(warn, "Unable to send: connection id ", conn_id, " is not (or is no longer) a valid outgoing connection");
            return;
        }
        auto& peer = pr.first->second;
        send_to = &connections[peer.conn_index];
    }

    if (request) {
        LMQ_LOG(debug, "Added new pending request ", to_hex(request_tag));
        pending_requests.insert({ request_tag, {
            std::chrono::steady_clock::now() + REQUEST_TIMEOUT, std::move(*request_cbptr) }});
    }

    try {
        send_message_parts(*send_to, build_send_parts(send, conn_id.route));
    } catch (const zmq::error_t &e) {
        if (e.num() == EHOSTUNREACH && !conn_id.route.empty() /*= incoming conn*/) {
            LMQ_LOG(debug, "Incoming connection is no longer valid; removing peer details");
            // Our incoming connection no longer exists; remove it from `peers`.
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
                        return proxy_send(std::move(data));
                    }
                }
            }
        }
        LMQ_LOG(warn, "Unable to send message to ", conn_id, ": ", e.what());
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
                LMQ_LOG(info, "Unable to send reply to incoming non-SN request: remote is no longer connected");
                LMQ_LOG(debug, "Incoming connection is no longer valid; removing peer details");
                it = peers.erase(it);
            } else {
                LMQ_LOG(warn, "Unable to send reply to incoming non-SN request: ", err.what());
                ++it;
            }
        }
    }
}

void LokiMQ::proxy_batch(detail::Batch* batch) {
    batches.insert(batch);
    const int jobs = batch->size();
    for (int i = 0; i < jobs; i++)
        batch_jobs.emplace(batch, i);
    proxy_skip_one_poll = true;
}

void LokiMQ::proxy_schedule_reply_job(std::function<void()> f) {
    auto* b = new Batch<void>;
    b->add_job(std::move(f));
    batches.insert(b);
    reply_jobs.emplace(static_cast<detail::Batch*>(b), 0);
    proxy_skip_one_poll = true;
}

// Called either within the proxy thread, or before the proxy thread has been created; actually adds
// the timer.  If the timer object hasn't been set up yet it gets set up here.
void LokiMQ::proxy_timer(std::function<void()> job, std::chrono::milliseconds interval, bool squelch) {
    if (!timers)
        timers.reset(zmq_timers_new());

    int timer_id = zmq_timers_add(timers.get(),
            interval.count(),
            [](int timer_id, void* self) { static_cast<LokiMQ*>(self)->_queue_timer_job(timer_id); },
            this);
    if (timer_id == -1)
        throw zmq::error_t{};
    timer_jobs[timer_id] = {std::move(job), squelch, false};
}

void LokiMQ::proxy_timer(bt_list_consumer timer_data) {
    std::unique_ptr<std::function<void()>> func{reinterpret_cast<std::function<void()>*>(timer_data.consume_integer<uintptr_t>())};
    auto interval = std::chrono::milliseconds{timer_data.consume_integer<uint64_t>()};
    auto squelch = timer_data.consume_integer<bool>();
    if (!timer_data.is_finished())
        throw std::runtime_error("Internal error: proxied timer request contains unexpected data");
    proxy_timer(std::move(*func), interval, squelch);
}

void LokiMQ::proxy_control_message(std::vector<zmq::message_t>& parts) {
    if (parts.size() < 2)
        throw std::logic_error("Expected 2-3 message parts for a proxy control message");
    auto route = view(parts[0]), cmd = view(parts[1]);
    LMQ_TRACE("control message: ", cmd);
    if (parts.size() == 3) {
        LMQ_TRACE("...: ", parts[2]);
        if (cmd == "SEND") {
            LMQ_TRACE("proxying message");
            return proxy_send(view(parts[2]));
        } else if (cmd == "REPLY") {
            LMQ_TRACE("proxying reply to non-SN incoming message");
            return proxy_reply(view(parts[2]));
        } else if (cmd == "BATCH") {
            LMQ_TRACE("proxy batch jobs");
            auto ptrval = bt_deserialize<uintptr_t>(view(parts[2]));
            return proxy_batch(reinterpret_cast<detail::Batch*>(ptrval));
        } else if (cmd == "CONNECT_SN") {
            proxy_connect_sn(view(parts[2]));
            return;
        } else if (cmd == "CONNECT_REMOTE") {
            return proxy_connect_remote(view(parts[2]));
        } else if (cmd == "DISCONNECT") {
            return proxy_disconnect(view(parts[2]));
        } else if (cmd == "TIMER") {
            return proxy_timer(view(parts[2]));
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
    throw std::runtime_error("Proxy received invalid control command: " + std::string{cmd} +
            " (" + std::to_string(parts.size()) + ")");
}

template <typename Container, typename AccessIndex>
void update_connection_indices(Container& c, size_t index, AccessIndex get_index) {
    for (auto it = c.begin(); it != c.end(); ) {
        size_t& i = get_index(*it);
        if (index == i) {
            it = c.erase(it);
            continue;
        }
        if (index > i)
            --i;
        ++it;
    }
}

void LokiMQ::proxy_close_connection(size_t index, std::chrono::milliseconds linger) {
    connections[index].setsockopt<int>(ZMQ_LINGER, linger > 0ms ? linger.count() : 0);
    pollitems_stale = true;
    connections.erase(connections.begin() + index);

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
            auto idle = info.last_activity - std::chrono::steady_clock::now();
            if (idle <= info.idle_expiry) {
                ++it;
                continue;
            }
            LMQ_LOG(info, "Closing outgoing connection to ", it->first, ": idle timeout reached");
            proxy_close_connection(info.conn_index, CLOSE_LINGER);
            it = peers.erase(it);
        }
    }
}

void LokiMQ::proxy_conn_cleanup() {
    // Drop idle connections (if we haven't done it in a while) but *only* if we have some idle
    // general workers: if we don't have any idle workers then we may still have incoming messages which
    // we haven't processed yet and those messages might end up resetting the last activity time.
    if (static_cast<int>(workers.size()) < general_workers) {
        LMQ_TRACE("closing idle connections");
        proxy_expire_idle_peers();
    }

    auto now = std::chrono::steady_clock::now();

    // FIXME - check other outgoing connections to see if they died and if so purge them

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

    // Remove any expired pending requests and schedule their callback with a failure
    for (auto it = pending_requests.begin(); it != pending_requests.end(); ) {
        auto& callback = it->second;
        if (callback.first < now) {
            LMQ_LOG(debug, "pending request ", to_hex(it->first), " expired, invoking callback with failure status and removing");
            job([callback = std::move(callback.second)] { callback(false, {}); });
            it = pending_requests.erase(it);
        } else {
            ++it;
        }
    }
};

void LokiMQ::listen_curve(std::string bind_addr, AllowFunc allow_connection) {
    // TODO: there's no particular reason we can't start listening after starting up; just needs to
    // be implemented.  (But if we can start we'll probably also want to be able to stop, so it's
    // more than just binding that needs implementing).
    check_not_started(proxy_thread, "start listening");

    bind.emplace_back(std::move(bind_addr), bind_data{true, std::move(allow_connection)});
}

void LokiMQ::listen_plain(std::string bind_addr, AllowFunc allow_connection) {
    // TODO: As above.
    check_not_started(proxy_thread, "start listening");

    bind.emplace_back(std::move(bind_addr), bind_data{false, std::move(allow_connection)});
}

void LokiMQ::proxy_loop() {

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

#ifndef NDEBUG
    if (log_level() >= LogLevel::trace) {
        LMQ_TRACE("Reserving space for ", max_workers, " max workers = ", general_workers, " general plus reservations for:");
        for (const auto& cat : categories)
            LMQ_TRACE("    - ", cat.first, ": ", cat.second.reserved_threads);
        LMQ_TRACE("    - (batch jobs): ", batch_jobs_reserved);
        LMQ_TRACE("    - (reply jobs): ", reply_jobs_reserved);
    }
#endif

    workers.reserve(max_workers);
    if (!workers.empty())
        throw std::logic_error("Internal error: proxy thread started with active worker threads");

    for (size_t i = 0; i < bind.size(); i++) {
        auto& b = bind[i].second;
        zmq::socket_t listener{context, zmq::socket_type::router};

        std::string auth_domain = bt_serialize(i);
        listener.setsockopt(ZMQ_ZAP_DOMAIN, auth_domain.c_str(), auth_domain.size());
        if (b.curve) {
            listener.setsockopt<int>(ZMQ_CURVE_SERVER, 1);
            listener.setsockopt(ZMQ_CURVE_PUBLICKEY, pubkey.data(), pubkey.size());
            listener.setsockopt(ZMQ_CURVE_SECRETKEY, privkey.data(), privkey.size());
        }
        listener.setsockopt(ZMQ_HANDSHAKE_IVL, (int) HANDSHAKE_TIME.count());
        listener.setsockopt<int64_t>(ZMQ_MAXMSGSIZE, MAX_MSG_SIZE);
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

        for (parts.clear(); !queue_index.empty() && static_cast<int>(workers.size()) < max_workers; parts.clear()) {
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
        }

        LMQ_TRACE("done proxy loop");
    }
}

std::pair<LokiMQ::category*, const std::pair<LokiMQ::CommandCallback, bool>*> LokiMQ::get_command(std::string& command) {
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
    std::string catname = command.substr(0, dot);
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

// Return true if we recognized/handled the builtin command (even if we reject it for whatever
// reason)
bool LokiMQ::proxy_handle_builtin(size_t conn_index, std::vector<zmq::message_t>& parts) {
    bool outgoing = connections[conn_index].getsockopt<int>(ZMQ_TYPE) == ZMQ_DEALER;

    string_view route, cmd;
    if (parts.size() < (outgoing ? 1 : 2)) {
        LMQ_LOG(warn, "Received empty message; ignoring");
        return true;
    }
    if (outgoing) {
        cmd = view(parts[0]);
    } else {
        route = view(parts[0]);
        cmd = view(parts[1]);
    }
    LMQ_TRACE("Checking for builtins: ", cmd, " from ", peer_address(parts.back()));

    if (cmd == "REPLY") {
        size_t tag_pos = (outgoing ? 1 : 2);
        if (parts.size() <= tag_pos) {
            LMQ_LOG(warn, "Received REPLY without a reply tag; ignoring");
            return true;
        }
        std::string reply_tag{view(parts[tag_pos])};
        auto it = pending_requests.find(reply_tag);
        if (it != pending_requests.end()) {
            LMQ_LOG(debug, "Received REPLY for pending command", to_hex(reply_tag), "; scheduling callback");
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
        if (outgoing) {
            LMQ_LOG(warn, "Got invalid 'HI' message on an outgoing connection; ignoring");
            return true;
        }
        LMQ_LOG(info, "Incoming client from ", peer_address(parts.back()), " sent HI, replying with HELLO");
        try {
            send_routed_message(connections[conn_index], std::string{route}, "HELLO");
        } catch (const std::exception &e) { LMQ_LOG(warn, "Couldn't reply with HELLO: ", e.what()); }
        return true;
    } else if (cmd == "HELLO") {
        if (!outgoing) {
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

        LMQ_LOG(info, "Got initial HELLO server response from ", peer_address(parts.back()));
        proxy_schedule_reply_job([on_success=std::move(std::get<ConnectSuccess>(pc)),
                conn=conn_index_to_id[conn_index]] {
            on_success(conn);
        });
        pending_connects.erase(it);
        return true;
    } else if (cmd == "BYE") {
        if (outgoing) {
            std::string pk;
            bool sn;
            AuthLevel a;
            std::tie(pk, sn, a) = extract_metadata(parts.back());
            ConnectionID conn = sn ? ConnectionID{std::move(pk)} : conn_index_to_id[conn_index];
            LMQ_LOG(info, "BYE command received; disconnecting from ", conn);
            proxy_disconnect(conn, 1s);
        } else {
            LMQ_LOG(warn, "Got invalid 'BYE' command on an incoming socket; ignoring");
        }

        return true;
    }
    else if (cmd == "FORBIDDEN" || cmd == "NOT_A_SERVICE_NODE") {
        return true; // FIXME - ignore these?  Log?
    }
    return false;
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

void LokiMQ::set_batch_threads(int threads) {
    if (proxy_thread.joinable())
        throw std::logic_error("Cannot change reserved batch threads after calling `start()`");
    if (threads < -1) // -1 is the default which is based on general threads
        throw std::out_of_range("Invalid set_batch_threads() value " + std::to_string(threads));
    batch_jobs_reserved = threads;
}

void LokiMQ::set_reply_threads(int threads) {
    if (proxy_thread.joinable())
        throw std::logic_error("Cannot change reserved reply threads after calling `start()`");
    if (threads < -1) // -1 is the default which is based on general threads
        throw std::out_of_range("Invalid set_reply_threads() value " + std::to_string(threads));
    reply_jobs_reserved = threads;
}

void LokiMQ::set_general_threads(int threads) {
    if (proxy_thread.joinable())
        throw std::logic_error("Cannot change general thread count after calling `start()`");
    if (threads < 1)
        throw std::out_of_range("Invalid set_general_threads() value " + std::to_string(threads) + ": general threads must be > 0");
    general_workers = threads;
}

LokiMQ::run_info& LokiMQ::run_info::load(category* cat_, std::string command_, ConnectionID conn_,
                std::vector<zmq::message_t> data_parts_, const std::pair<CommandCallback, bool>* callback_) {
    is_batch_job = false;
    is_reply_job = false;
    cat = cat_;
    command = std::move(command_);
    conn = std::move(conn_);
    data_parts = std::move(data_parts_);
    callback = callback_;
    return *this;
}

LokiMQ::run_info& LokiMQ::run_info::load(pending_command&& pending) {
    return load(&pending.cat, std::move(pending.command), std::move(pending.conn),
            std::move(pending.data_parts), pending.callback);
}

LokiMQ::run_info& LokiMQ::run_info::load(batch_job&& bj, bool reply_job) {
    is_batch_job = true;
    is_reply_job = reply_job;
    batch_jobno = bj.second;
    batch = bj.first;
    return *this;
}

void LokiMQ::proxy_run_worker(run_info& run) {
    if (!run.worker_thread.joinable())
        run.worker_thread = std::thread{&LokiMQ::worker_thread, this, run.worker_id};
    else
        send_routed_message(workers_socket, run.worker_routing_id, "RUN");
}

void LokiMQ::proxy_run_batch_jobs(std::queue<batch_job>& jobs, const int reserved, int& active, bool reply) {
    while (!jobs.empty() &&
            (active < reserved || static_cast<int>(workers.size() - idle_workers.size()) < general_workers)) {
        proxy_run_worker(get_idle_worker().load(std::move(jobs.front()), reply));
        jobs.pop();
        active++;
    }
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

void LokiMQ::proxy_to_worker(size_t conn_index, std::vector<zmq::message_t>& parts) {
    bool outgoing = connections[conn_index].getsockopt<int>(ZMQ_TYPE) == ZMQ_DEALER;

    peer_info tmp_peer;
    tmp_peer.conn_index = conn_index;
    if (!outgoing) tmp_peer.route = parts[0].to_string();
    peer_info* peer = nullptr;
    if (outgoing) {
        auto it = peers.find(conn_index_to_id[conn_index]);
        if (it == peers.end()) {
            LMQ_LOG(warn, "Internal error: connection index not found");
            return;
        }
        peer = &it->second;
    } else {
        std::tie(tmp_peer.pubkey, tmp_peer.service_node, tmp_peer.auth_level) = extract_metadata(parts.back());
        if (tmp_peer.service_node) {
            // It's a service node so we should have a peer_info entry; see if we can find one with
            // the same route, and if not, add one.
            auto pr = peers.equal_range(tmp_peer.pubkey);
            for (auto it = pr.first; it != pr.second; ++it) {
                if (it->second.route == tmp_peer.route) {
                    peer = &it->second;
                    // Upgrade permissions in case we have something higher on the socket
                    peer->service_node |= tmp_peer.service_node;
                    if (tmp_peer.auth_level > peer->auth_level)
                        peer->auth_level = tmp_peer.auth_level;
                    break;
                }
            }
            if (!peer) {
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
    auto cat_call = get_command(command);

    if (!cat_call.first) {
        if (outgoing)
            send_direct_message(connections[conn_index], "UNKNOWNCOMMAND", command);
        else
            send_routed_message(connections[conn_index], peer->route, "UNKNOWNCOMMAND", command);
        return;
    }

    auto& category = *cat_call.first;

    if (!proxy_check_auth(conn_index, outgoing, *peer, command, category, parts.back()))
        return;

    // Steal any data message parts
    size_t data_part_index = command_part_index + 1;
    std::vector<zmq::message_t> data_parts;
    data_parts.reserve(parts.size() - data_part_index);
    for (auto it = parts.begin() + data_part_index; it != parts.end(); ++it)
        data_parts.push_back(std::move(*it));

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
        if (outgoing)
            proxy_disconnect(peer.service_node ? ConnectionID{peer.pubkey} : conn_index_to_id[conn_index], 1s);
        else
            send_routed_message(connections[conn_index], peer.route, "BYE");
        return false;
    }

    if (reply.empty())
        return true;

    if (outgoing)
        send_direct_message(connections[conn_index], std::move(reply), command);
    else
        send_routed_message(connections[conn_index], peer.route, std::move(reply), command);
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
                    LMQ_LOG(info, "Accepted incoming ", view(frames[5]), (sn ? " service node" : " client"),
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

LokiMQ::~LokiMQ() {
    if (!proxy_thread.joinable())
        return;

    LMQ_LOG(info, "LokiMQ shutting down proxy thread");
    detail::send_control(get_control_socket(), "QUIT");
    proxy_thread.join();
    LMQ_LOG(info, "LokiMQ proxy thread has stopped");
}

ConnectionID LokiMQ::connect_sn(string_view pubkey, std::chrono::milliseconds keep_alive, string_view hint) {
    check_started(proxy_thread, "connect");

    detail::send_control(get_control_socket(), "CONNECT_SN", bt_serialize<bt_dict>({{"pubkey",pubkey}, {"keep-alive",keep_alive.count()}, {"hint",hint}}));

    return pubkey;
}

ConnectionID LokiMQ::connect_remote(string_view remote, ConnectSuccess on_connect, ConnectFailure on_failure,
        string_view pubkey, AuthLevel auth_level, std::chrono::milliseconds timeout) {
    if (!proxy_thread.joinable())
        LMQ_LOG(warn, "connect_remote() called before start(); this won't take effect until start() is called");

    if (remote.size() < 7 || !(remote.substr(0, 6) == "tcp://" || remote.substr(0, 6) == "ipc://" /* unix domain sockets */))
        throw std::runtime_error("Invalid connect_remote: remote address '" + std::string{remote} + "' is not a valid or supported zmq connect string");

    auto id = next_conn_id++;
    LMQ_TRACE("telling proxy to connect to ", remote, ", id ", id,
            pubkey.empty() ? "using NULL auth" : ", using CURVE with remote pubkey [" + to_hex(pubkey) + "]");
    detail::send_control(get_control_socket(), "CONNECT_REMOTE", bt_serialize<bt_dict>({
        {"auth_level", static_cast<std::underlying_type_t<AuthLevel>>(auth_level)},
        {"conn_id", id},
        {"connect", reinterpret_cast<uintptr_t>(new ConnectSuccess{std::move(on_connect)})},
        {"failure", reinterpret_cast<uintptr_t>(new ConnectFailure{std::move(on_failure)})},
        {"pubkey", pubkey},
        {"remote", remote},
        {"timeout", timeout.count()},
    }));

    return id;
}

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
        auto* ptr = reinterpret_cast<ConnectSuccess*>(data.consume_integer<uintptr_t>());
        on_connect = std::move(*ptr);
        delete ptr;
    }
    if (data.skip_until("failure")) {
        auto* ptr = reinterpret_cast<ConnectFailure*>(data.consume_integer<uintptr_t>());
        on_failure = std::move(*ptr);
        delete ptr;
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

    LMQ_LOG(info, "Establishing remote connection to ", remote, remote_pubkey.empty() ? " (NULL auth)" : " via CURVE expecting pubkey " + to_hex(remote_pubkey));

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

void LokiMQ::disconnect(ConnectionID id, std::chrono::milliseconds linger) {
    detail::send_control(get_control_socket(), "DISCONNECT", bt_serialize<bt_dict>({
            {"conn_id", id.id},
            {"linger_ms", linger.count()},
            {"pubkey", id.pk},
    }));
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
            LMQ_LOG(info, "Closing outgoing connection to ", conn);
            proxy_close_connection(peer.conn_index, linger);
            peers.erase(it);
            return;
        }
    }
    LMQ_LOG(warn, "Failed to disconnect ", conn, ": no such outgoing connection");
}

void LokiMQ::job(std::function<void()> f) {
    auto* b = new Batch<void>;
    b->add_job(std::move(f));
    auto* baseptr = static_cast<detail::Batch*>(b);
    detail::send_control(get_control_socket(), "BATCH", bt_serialize(reinterpret_cast<uintptr_t>(baseptr)));
}

void LokiMQ::_queue_timer_job(int timer_id) {
    auto it = timer_jobs.find(timer_id);
    if (it == timer_jobs.end()) {
        LMQ_LOG(warn, "Could not find timer job ", timer_id);
        return;
    }
    auto& timer = it->second;
    auto& squelch = std::get<1>(timer);
    auto& running = std::get<2>(timer);
    if (squelch && running) {
        LMQ_LOG(debug, "Not running timer job ", timer_id, " because a job for that timer is still running");
        return;
    }

    auto* b = new Batch<void>;
    b->add_job(std::get<0>(timer));
    if (squelch) {
        running = true;
        b->completion_proxy([this,timer_id](auto results) {
            try { results[0].get(); }
            catch (const std::exception &e) { LMQ_LOG(warn, "timer job ", timer_id, " raised an exception: ", e.what()); }
            catch (...) { LMQ_LOG(warn, "timer job ", timer_id, " raised a non-std exception"); }
            auto it = timer_jobs.find(timer_id);
            if (it != timer_jobs.end())
                std::get<2>(it->second)/*running*/ = false;
        });
    }
    batches.insert(b);
    batch_jobs.emplace(static_cast<detail::Batch*>(b), 0);
    assert(b->size() == 1);
}

void LokiMQ::add_timer(std::function<void()> job, std::chrono::milliseconds interval, bool squelch) {
    if (proxy_thread.joinable()) {
        auto *jobptr = new std::function<void()>{std::move(job)};
        detail::send_control(get_control_socket(), "TIMER", bt_serialize(bt_list{{
                    reinterpret_cast<uintptr_t>(jobptr),
                    interval.count(),
                    squelch}}));
    } else {
        proxy_timer(std::move(job), interval, squelch);
    }
}

void LokiMQ::TimersDeleter::operator()(void* timers) { zmq_timers_destroy(&timers); }

std::ostream &operator<<(std::ostream &os, LogLevel lvl) {
    os <<  (lvl == LogLevel::trace ? "trace" :
            lvl == LogLevel::debug ? "debug" :
            lvl == LogLevel::info  ? "info"  :
            lvl == LogLevel::warn  ? "warn"  :
            lvl == LogLevel::error ? "ERROR" :
            lvl == LogLevel::fatal ? "FATAL" :
            "unknown");
    return os;
}

std::string make_random_string(size_t size) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static thread_local std::uniform_int_distribution<char> dist{std::numeric_limits<char>::min(), std::numeric_limits<char>::max()};
    std::string rando;
    rando.reserve(size);
    for (size_t i = 0; i < size; i++)
        rando += dist(rng);
    return rando;
}

} // namespace lokimq
// vim:sw=4:et
