#include "oxenmq.h"
#include "oxenmq-internal.h"
#include "zmq.hpp"
#include <map>
#include <mutex>
#include <random>
#include <ostream>
#include <thread>
#include <future>

extern "C" {
#include <sodium/core.h>
#include <sodium/crypto_box.h>
#include <sodium/crypto_scalarmult.h>
}
#include <oxenc/hex.h>
#include <oxenc/variant.h>

namespace oxenmq {

namespace {


/// Creates a message by bt-serializing the given value (string, number, list, or dict)
template <typename T>
zmq::message_t create_bt_message(T&& data) { return create_message(oxenc::bt_serialize(std::forward<T>(data))); }

template <typename MessageContainer>
std::vector<std::string> as_strings(const MessageContainer& msgs) {
    std::vector<std::string> result;
    result.reserve(msgs.size());
    for (const auto &msg : msgs)
        result.emplace_back(msg.template data<char>(), msg.size());
    return result;
}

void check_not_started(const std::thread& proxy_thread, const std::string &verb) {
    if (proxy_thread.joinable())
        throw std::logic_error("Cannot " + verb + " after calling `start()`");
}

} // anonymous namespace


namespace detail {

// Sends a control messages between proxy and threads or between proxy and workers consisting of a
// single command codes with an optional data part (the data frame is omitted if empty).
void send_control(zmq::socket_t& sock, std::string_view cmd, std::string data) {
    auto c = create_message(std::move(cmd));
    if (data.empty()) {
        sock.send(c, zmq::send_flags::none);
    } else {
        auto d = create_message(std::move(data));
        sock.send(c, zmq::send_flags::sndmore);
        sock.send(d, zmq::send_flags::none);
    }
}

/// Extracts a pubkey and and auth level from a zmq message received on a *listening* socket.
std::pair<std::string, AuthLevel> extract_metadata(zmq::message_t& msg) {
    auto result = std::make_pair(""s, AuthLevel::none);
    try {
        std::string_view pubkey_hex{msg.gets("User-Id")};
        if (pubkey_hex.size() != 64)
            throw std::logic_error("bad user-id");
        assert(oxenc::is_hex(pubkey_hex.begin(), pubkey_hex.end()));
        result.first.resize(32, 0);
        oxenc::from_hex(pubkey_hex.begin(), pubkey_hex.end(), result.first.begin());
    } catch (...) {}

    try {
        result.second = auth_from_string(msg.gets("X-AuthLevel"));
    } catch (...) {}

    return result;
}


} // namespace detail

void OxenMQ::set_zmq_context_option(zmq::ctxopt option, int value) {
    context.set(option, value);
}

void OxenMQ::log_level(LogLevel level) {
    log_lvl.store(level, std::memory_order_relaxed);
}

LogLevel OxenMQ::log_level() const {
    return log_lvl.load(std::memory_order_relaxed);
}


CatHelper OxenMQ::add_category(std::string name, Access access_level, unsigned int reserved_threads, int max_queue) {
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

void OxenMQ::add_command(const std::string& category, std::string name, CommandCallback callback) {
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

void OxenMQ::add_request_command(const std::string& category, std::string name, CommandCallback callback) {
    add_command(category, name, std::move(callback));
    categories.at(category).commands.at(name).second = true;
}

void OxenMQ::add_command_alias(std::string from, std::string to) {
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

/// Accesses a thread-local command socket connected to the proxy's command socket used to issue
/// commands in a thread-safe manner.  A mutex is only required here the first time a thread
/// accesses the control socket.
zmq::socket_t& OxenMQ::get_control_socket() {
    assert(proxy_thread.joinable());

    // Optimize by caching the last value; OxenMQ is often a singleton and in that case we're
    // going to *always* hit this optimization.  Even if it isn't, we're probably likely to need the
    // same control socket from the same thread multiple times sequentially so this may still help.
    static thread_local int last_id = -1;
    static thread_local zmq::socket_t* last_socket = nullptr;
    if (object_id == last_id)
        return *last_socket;

    std::lock_guard lock{control_sockets_mutex};

    if (proxy_shutting_down)
        throw std::runtime_error("Unable to obtain OxenMQ control socket: proxy thread is shutting down");

    auto& socket = control_sockets[std::this_thread::get_id()];
    if (!socket) {
        socket = std::make_unique<zmq::socket_t>(context, zmq::socket_type::dealer);
        socket->set(zmq::sockopt::linger, 0);
        socket->connect(SN_ADDR_COMMAND);
    }
    last_id = object_id;
    last_socket = socket.get();
    return *last_socket;
}


OxenMQ::OxenMQ(
        std::string pubkey_,
        std::string privkey_,
        bool service_node,
        SNRemoteAddress lookup,
        Logger logger,
        LogLevel level)
    : object_id{next_id++}, pubkey{std::move(pubkey_)}, privkey{std::move(privkey_)}, local_service_node{service_node},
        sn_lookup{std::move(lookup)}, log_lvl{level}, logger{std::move(logger)}
{

    OMQ_TRACE("Constructing OxenMQ, id=", object_id, ", this=", this);

    if (sodium_init() == -1)
        throw std::runtime_error{"libsodium initialization failed"};

    if (pubkey.empty() != privkey.empty()) {
        throw std::invalid_argument("OxenMQ construction failed: one (and only one) of pubkey/privkey is empty. Both must be specified, or both empty to generate a key.");
    } else if (pubkey.empty()) {
        if (service_node)
            throw std::invalid_argument("Cannot construct a service node mode OxenMQ without a keypair");
        OMQ_LOG(debug, "generating x25519 keypair for remote-only OxenMQ instance");
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
            throw std::invalid_argument("Invalid pubkey/privkey values given to OxenMQ construction: pubkey verification failed");
    }
}

void OxenMQ::start() {
    if (proxy_thread.joinable())
        throw std::logic_error("Cannot call start() multiple times!");

    OMQ_LOG(info, "Initializing OxenMQ ", bind.empty() ? "remote-only" : "listener", " with pubkey ", oxenc::to_hex(pubkey));

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
        OMQ_LOG(debug, "Reserving space for ", max_workers, " max workers = ", general_workers, " general plus reservations for:");
        for (const auto& cat : categories)
            OMQ_LOG(debug, "    - ", cat.first, ": ", cat.second.reserved_threads);
        OMQ_LOG(debug, "    - (batch jobs): ", batch_jobs_reserved);
        OMQ_LOG(debug, "    - (reply jobs): ", reply_jobs_reserved);
        OMQ_LOG(debug, "Plus ", tagged_workers.size(), " tagged worker threads");
    }

    if (MAX_SOCKETS != 0) {
        // The max sockets setting we apply to the context here is used during zmq context
        // initialization, which happens when the first socket is constructed using this context:
        // hence we set this *before* constructing any socket_t on the context.
        int zmq_socket_limit = context.get(zmq::ctxopt::socket_limit);
        int want_sockets = MAX_SOCKETS < 0 ? zmq_socket_limit :
            std::min<int>(zmq_socket_limit,
                    MAX_SOCKETS + max_workers + tagged_workers.size()
                    + 4 /* zap_auth, workers_socket, command, inproc_listener */);
        context.set(zmq::ctxopt::max_sockets, want_sockets);
    }

    // We bind `command` here so that the `get_control_socket()` below is always connecting to a
    // bound socket, but we do nothing else here: the proxy thread is responsible for everything
    // except binding it.
    command = zmq::socket_t{context, zmq::socket_type::router};
    command.bind(SN_ADDR_COMMAND);
    std::promise<void> startup_prom;
    auto proxy_startup = startup_prom.get_future();
    proxy_thread = std::thread{&OxenMQ::proxy_loop, this, std::move(startup_prom)};

    OMQ_LOG(debug, "Waiting for proxy thread to initialize...");
    proxy_startup.get(); // Rethrows exceptions from the proxy startup (e.g. failure to bind)

    OMQ_LOG(debug, "Waiting for proxy thread to get ready...");
    auto &control = get_control_socket();
    detail::send_control(control, "START");
    OMQ_TRACE("Sent START command");

    zmq::message_t ready_msg;
    std::vector<zmq::message_t> parts;
    try { recv_message_parts(control, parts); }
    catch (const zmq::error_t &e) { throw std::runtime_error("Failure reading from OxenMQ::Proxy thread: "s + e.what()); }

    if (!(parts.size() == 1 && view(parts.front()) == "READY"))
        throw std::runtime_error("Invalid startup message from proxy thread (didn't get expected READY message)");
    OMQ_LOG(debug, "Proxy thread is ready");
}

void OxenMQ::listen_curve(std::string bind_addr, AllowFunc allow_connection, std::function<void(bool)> on_bind) {
    if (std::string_view{bind_addr}.substr(0, 9) == "inproc://")
        throw std::logic_error{"inproc:// cannot be used with listen_curve"};
    if (!allow_connection) allow_connection = [](auto&&...) { return AuthLevel::none; };
    bind_data d{std::move(bind_addr), true, std::move(allow_connection), std::move(on_bind)};
    if (proxy_thread.joinable())
        detail::send_control(get_control_socket(), "BIND", oxenc::bt_serialize(detail::serialize_object(std::move(d))));
    else
        bind.push_back(std::move(d));
}

void OxenMQ::listen_plain(std::string bind_addr, AllowFunc allow_connection, std::function<void(bool)> on_bind) {
    if (std::string_view{bind_addr}.substr(0, 9) == "inproc://")
        throw std::logic_error{"inproc:// cannot be used with listen_plain"};
    if (!allow_connection) allow_connection = [](auto&&...) { return AuthLevel::none; };
    bind_data d{std::move(bind_addr), false, std::move(allow_connection), std::move(on_bind)};
    if (proxy_thread.joinable())
        detail::send_control(get_control_socket(), "BIND", oxenc::bt_serialize(detail::serialize_object(std::move(d))));
    else
        bind.push_back(std::move(d));
}


std::pair<OxenMQ::category*, const std::pair<OxenMQ::CommandCallback, bool>*> OxenMQ::get_command(std::string& command) {
    if (command.size() > MAX_CATEGORY_LENGTH + 1 + MAX_COMMAND_LENGTH) {
        OMQ_LOG(warn, "Invalid command '", command, "': command too long");
        return {};
    }

    if (!command_aliases.empty()) {
        auto it = command_aliases.find(command);
        if (it != command_aliases.end())
            command = it->second;
    }

    auto dot = command.find('.');
    if (dot == 0 || dot == std::string::npos) {
        OMQ_LOG(warn, "Invalid command '", command, "': expected <category>.<command>");
        return {};
    }
    std::string catname = command.substr(0, dot);
    std::string cmd = command.substr(dot + 1);

    auto catit = categories.find(catname);
    if (catit == categories.end()) {
        OMQ_LOG(warn, "Invalid command category '", catname, "'");
        return {};
    }

    const auto& category = catit->second;
    auto callback_it = category.commands.find(cmd);
    if (callback_it == category.commands.end()) {
        OMQ_LOG(warn, "Invalid command '", command, "'");
        return {};
    }

    return {&catit->second, &callback_it->second};
}

void OxenMQ::set_batch_threads(int threads) {
    if (proxy_thread.joinable())
        throw std::logic_error("Cannot change reserved batch threads after calling `start()`");
    if (threads < -1) // -1 is the default which is based on general threads
        throw std::out_of_range("Invalid set_batch_threads() value " + std::to_string(threads));
    batch_jobs_reserved = threads;
}

void OxenMQ::set_reply_threads(int threads) {
    if (proxy_thread.joinable())
        throw std::logic_error("Cannot change reserved reply threads after calling `start()`");
    if (threads < -1) // -1 is the default which is based on general threads
        throw std::out_of_range("Invalid set_reply_threads() value " + std::to_string(threads));
    reply_jobs_reserved = threads;
}

void OxenMQ::set_general_threads(int threads) {
    if (proxy_thread.joinable())
        throw std::logic_error("Cannot change general thread count after calling `start()`");
    if (threads < 1)
        throw std::out_of_range("Invalid set_general_threads() value " + std::to_string(threads) + ": general threads must be > 0");
    general_workers = threads;
}

OxenMQ::run_info& OxenMQ::run_info::load(category* cat_, std::string command_, ConnectionID conn_, Access access_, std::string remote_,
                std::vector<zmq::message_t> data_parts_, const std::pair<CommandCallback, bool>* callback_) {
    reset();
    cat = cat_;
    command = std::move(command_);
    conn = std::move(conn_);
    access = std::move(access_);
    remote = std::move(remote_);
    data_parts = std::move(data_parts_);
    to_run = callback_;
    return *this;
}

OxenMQ::run_info& OxenMQ::run_info::load(category* cat_, std::string command_, std::string remote_, std::function<void()> callback) {
    reset();
    is_injected = true;
    cat = cat_;
    command = std::move(command_);
    conn = {};
    access = {};
    remote = std::move(remote_);
    to_run = std::move(callback);
    return *this;
}

OxenMQ::run_info& OxenMQ::run_info::load(pending_command&& pending) {
    if (auto *f = std::get_if<std::function<void()>>(&pending.callback))
        return load(&pending.cat, std::move(pending.command), std::move(pending.remote), std::move(*f));

    assert(pending.callback.index() == 0);
    return load(&pending.cat, std::move(pending.command), std::move(pending.conn), std::move(pending.access),
            std::move(pending.remote), std::move(pending.data_parts), var::get<0>(pending.callback));
}

OxenMQ::run_info& OxenMQ::run_info::load(batch_job&& bj, bool reply_job, int tagged_thread) {
    reset();
    is_batch_job = true;
    is_reply_job = reply_job;
    is_tagged_thread_job = tagged_thread > 0;
    batch_jobno = bj.second;
    to_run = bj.first;
    return *this;
}


OxenMQ::~OxenMQ() {
    if (!proxy_thread.joinable()) {
        if (!tagged_workers.empty()) {
            // We have tagged workers that are waiting on a signal for startup, but we didn't start
            // up, so signal them so that they can end themselves.
            {
                std::lock_guard lock{tagged_startup_mutex};
                tagged_go = tagged_go_mode::SHUTDOWN;
            }
            tagged_cv.notify_all();
            for (auto& [run, busy, queue] : tagged_workers)
                run.worker_thread.join();
        }

        return;
    }

    OMQ_LOG(info, "OxenMQ shutting down proxy thread");
    detail::send_control(get_control_socket(), "QUIT");
    proxy_thread.join();
    OMQ_LOG(info, "OxenMQ proxy thread has stopped");
}

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
    std::string rando;
    rando.reserve(size);
    while (rando.size() < size) {
        uint64_t x = rng();
        rando.append(reinterpret_cast<const char*>(&x), std::min<size_t>(size - rando.size(), 8));
    }
    return rando;
}

} // namespace oxenmq
// vim:sw=4:et
