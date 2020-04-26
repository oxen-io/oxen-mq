#include "lokimq.h"
#include "lokimq-internal.h"
#include <map>
#include <random>

extern "C" {
#include <sodium.h>
}
#include "hex.h"

namespace lokimq {

namespace {


/// Creates a message by bt-serializing the given value (string, number, list, or dict)
template <typename T>
zmq::message_t create_bt_message(T&& data) { return create_message(bt_serialize(std::forward<T>(data))); }

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

/// Extracts a pubkey and and auth level from a zmq message received on a *listening* socket.
std::pair<std::string, AuthLevel> extract_metadata(zmq::message_t& msg) {
    auto result = std::make_pair(""s, AuthLevel::none);
    try {
        string_view pubkey_hex{msg.gets("User-Id")};
        if (pubkey_hex.size() != 64)
            throw std::logic_error("bad user-id");
        assert(is_hex(pubkey_hex.begin(), pubkey_hex.end()));
        result.first.resize(32, 0);
        from_hex(pubkey_hex.begin(), pubkey_hex.end(), result.first.begin());
    } catch (...) {}

    try {
        result.second = auth_from_string(msg.gets("X-AuthLevel"));
    } catch (...) {}

    return result;
}


} // namespace detail

int LokiMQ::set_zmq_context_option(int option, int value) {
    return context.setctxopt(option, value);
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
        Logger logger,
        LogLevel level)
    : object_id{next_id++}, pubkey{std::move(pubkey_)}, privkey{std::move(privkey_)}, local_service_node{service_node},
        sn_lookup{std::move(lookup)}, log_lvl{level}, logger{std::move(logger)}
{

    LMQ_TRACE("Constructing LokiMQ, id=", object_id, ", this=", this);

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

    int zmq_socket_limit = context.getctxopt(ZMQ_SOCKET_LIMIT);
    if (MAX_SOCKETS > 1 && MAX_SOCKETS <= zmq_socket_limit)
        context.setctxopt(ZMQ_MAX_SOCKETS, MAX_SOCKETS);
    else
        LMQ_LOG(error, "Not applying LokiMQ::MAX_SOCKETS setting: ", MAX_SOCKETS, " must be in [1, ", zmq_socket_limit, "]");

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

LokiMQ::run_info& LokiMQ::run_info::load(category* cat_, std::string command_, ConnectionID conn_, Access access_, std::string remote_,
                std::vector<zmq::message_t> data_parts_, const std::pair<CommandCallback, bool>* callback_) {
    is_batch_job = false;
    is_reply_job = false;
    cat = cat_;
    command = std::move(command_);
    conn = std::move(conn_);
    access = std::move(access_);
    remote = std::move(remote_);
    data_parts = std::move(data_parts_);
    callback = callback_;
    return *this;
}

LokiMQ::run_info& LokiMQ::run_info::load(pending_command&& pending) {
    return load(&pending.cat, std::move(pending.command), std::move(pending.conn), std::move(pending.access),
            std::move(pending.remote), std::move(pending.data_parts), pending.callback);
}

LokiMQ::run_info& LokiMQ::run_info::load(batch_job&& bj, bool reply_job) {
    is_batch_job = true;
    is_reply_job = reply_job;
    batch_jobno = bj.second;
    batch = bj.first;
    return *this;
}


LokiMQ::~LokiMQ() {
    if (!proxy_thread.joinable())
        return;

    LMQ_LOG(info, "LokiMQ shutting down proxy thread");
    detail::send_control(get_control_socket(), "QUIT");
    proxy_thread.join();
    LMQ_LOG(info, "LokiMQ proxy thread has stopped");
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
    static thread_local std::uniform_int_distribution<char> dist{std::numeric_limits<char>::min(), std::numeric_limits<char>::max()};
    std::string rando;
    rando.reserve(size);
    for (size_t i = 0; i < size; i++)
        rando += dist(rng);
    return rando;
}

} // namespace lokimq
// vim:sw=4:et
