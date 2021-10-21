#include "common.hpp"
#include <chrono>
#include <exception>
#include <oxenmq/oxenmq.h>
#include <oxenmq/address.h>
#include <pybind11/attr.h>
#include <pybind11/chrono.h>
#include <pybind11/detail/common.h>
#include <pybind11/functional.h>
#include <pybind11/gil.h>
#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/stl.h>
#include <future>
#include <memory>
#include <variant>

namespace oxenmq {

// Convert a py::object containing a str, bytes, or iterable over str/bytes to a vector of message
// parts.  Throws on invalid input, otherwise returns a vector of parts.
//
// The gil must be held.
//
// The first version appends into `parts`, the second version returns a vector.
void extract_data_parts(std::vector<std::string>& parts, py::handle obj) {
    if (py::isinstance<py::bytes>(obj))
        parts.push_back(obj.cast<py::bytes>());
    else if (py::isinstance<py::str>(obj))
        parts.push_back(obj.cast<py::str>());
    else if (py::isinstance<py::iterable>(obj)) {
        for (auto o : obj) {
            if (py::isinstance<py::bytes>(o))
                parts.push_back(o.cast<py::bytes>());
            else if (py::isinstance<py::str>(o))
                parts.push_back(o.cast<py::str>());
            else
                throw std::runtime_error{"invalid iterable containing '" + std::string{py::repr(o)} + "': expected bytes/str"};
        }
    } else {
        throw std::runtime_error{"invalid value '" + std::string{py::repr(obj)} + "': expected bytes/str/iterable"};
    }
}
std::vector<std::string> extract_data_parts(py::handle obj) {
    std::vector<std::string> parts;
    extract_data_parts(parts, obj);
    return parts;
}
std::vector<std::string> extract_data_parts(py::args& args) {
    std::vector<std::string> data;
    for (auto arg: args)
        extract_data_parts(data, arg);
    return data;
}

// Quick and dirty logger that logs to stderr.  It would be much nicer to take a python function,
// but that deadlocks pretty much right away because of the crappiness of the gil.
struct stderr_logger {
    inline static std::mutex log_mutex;

    void operator()(LogLevel lvl, const char* file, int line, std::string msg) {
        std::lock_guard l{log_mutex};
        std::cerr << '[' << lvl << "][" << file << ':' << line << "]: " << msg << "\n";
    }
};

constexpr auto noopt = [] {};

void
OxenMQ_Init(py::module & mod)
{
    using namespace pybind11::literals;
    constexpr py::kw_only kwonly{};

    py::class_<ConnectionID>(mod, "ConnectionID")
        .def(py::self == py::self)
        .def(py::self != py::self)
        .def_property_readonly("service_node", &ConnectionID::sn)
        .def_property_readonly("pubkey", [](const ConnectionID& c) { return py::bytes(c.pubkey()); })
        ;
    py::class_<address>(mod, "Address")
        .def(py::init<std::string_view>(), "addr"_a,
                R"(Constructs from an encoded address such as 'curve://HOSTNAME:PORT/PUBKEY'.

See oxenmq::Address C++ documentation for more details)")
        .def(py::init([](std::string_view addr, py::bytes pubkey) { return address{addr, (std::string) pubkey}; }),
                "addr"_a, "pubkey"_a,
                R"(Constructs from a ZMQ connection string and a 32-byte pubkey.

This can be used for ipc+curve connections by using an address such as ipc:///path/to/socket)")
        .def(py::init(py::overload_cast<std::string, uint16_t>(&address::tcp)),
                "host"_a, "port"_a,
                R"(Construct a TCP address from a host and port.

The connection will be plaintext.  If the host is an IPv6 address it *must* be surrounded with [ and ].)")
        .def(py::init([](std::string host, uint16_t port, py::bytes pubkey) {
            return address::tcp_curve(std::move(host), port, pubkey); }),
                "host"_a, "port"_a, "pubkey"_a,
                "Constructs a curve-encrypted TCP address")

        .def_property("pubkey",
                [](const address& a) { return py::bytes(a.pubkey); },
                [](address& a, std::optional<py::bytes> pubkey) {
                    if (pubkey) a.set_pubkey(pubkey->operator std::string());
                    else a.set_pubkey("");
                },
                R"(Sets or clears the address pubkey.

If specifying a pubkey then this address is set to use curve encryption with the given 32-byte
`bytes` pubkey required for the remote endpoint.  An existing pubkey is replaced, if present.

If set to None then this address is set to use an unencrypted plaintext connection.)")
        .def_property_readonly("curve", py::overload_cast<>(&address::curve, py::const_), "true if this is a curve-enabled address")
        .def_property_readonly("tcp", py::overload_cast<>(&address::tcp, py::const_), "true if this is a tcp address")
        .def_property_readonly("ipc", py::overload_cast<>(&address::ipc, py::const_), "true if this is an ipc address")
        .def_property_readonly("zmq_address", &address::zmq_address,
                "accesses the zmq address portion of the address (note that this does not contain any curve encryption information)")
        .def_property_readonly("full_address", [](const address &a) { return a.full_address(address::encoding::base32z); },
                "returns the full address, including curve information, encoding the curve pubkey as base32z")
        .def_property_readonly("full_address_b64", [](const address &a) { return a.full_address(address::encoding::base64); },
                "returns the full address, including curve information, encoding the curve pubkey as base64")
        .def_property_readonly("full_address_hex", [](const address &a) { return a.full_address(address::encoding::hex); },
                "returns the full address, including curve information, encoding the curve pubkey as hex")
        .def_property_readonly("qr", &address::qr_address,
                R"(Access the full address as a RQ-encoding optimized string.

Only available for tcp addresses. The resulting string only contains characters from the
Alphanumeric QR alphabet (i.e. all uppercase, and uses $...$ for IPv6 addresses instead of [...]),
allowing for more efficient QR encoding.  This format can be passed to the single-argument Address
constructor.)")
        .def(py::self == py::self)
        .def(py::self != py::self)
        ;
    py::class_<TaggedThreadID>(mod, "TaggedThreadID");
    py::class_<TimerID>(mod, "TimerID");

    py::enum_<AuthLevel>(mod, "AuthLevel")
        .value("denied", AuthLevel::denied,
                "Not actually an auth level, but can be returned by the AllowFunc to deny an incoming connection.")
        .value("none", AuthLevel::none,
                "No authentication at all; any random incoming ZMQ connection can invoke this command.")
        .value("basic", AuthLevel::basic,
                "Basic authentication commands require a login, or a node that is specifically configured to be a public node (e.g. for public RPC).")
        .value("admin", AuthLevel::admin,
                R"(Advanced authentication commands require an admin user, either via explicit login or by implicit login from localhost.

This typically protects administrative commands like shutting down or access to sensitive data.)")
        ;

    py::class_<Access>(mod, "Access", "The access level for a command category.")
        .def(py::init<AuthLevel, bool, bool>(),
                "auth"_a = AuthLevel::none,
                "remote_sn"_a = false,
                "local_sn"_a = false,
                R"(Specifies a command category access level.

- auth - the required access level to access commands in this category.

- remote_sn - if True then this command may only be invoked by remote connections who we recognize
  (by pubkey) as being service nodes.  (Requires sn_lookup to be provided during construction).

- local_sn - if True then this command will be unavailable if the OxenMQ object was not constructed
  with service_node=True.)");
        py::implicitly_convertible<AuthLevel, Access>();

    py::class_<Message> msg(mod, "Message", "Temporary object containing details of a just-received message");
    msg
        .def_property_readonly("is_reply", [](const Message& m) { return !m.reply_tag.empty(); },
                "True if this message is expecting a reply (i.e. it was received on a request_command endpoint)")
        .def_readonly("remote", &Message::remote, R"(Some sort of remote address from which the request came.

Typically the IP address string for TCP connections and "localhost:UID:GID:PID" for unix socket IPC connections.)")
        .def_readonly("conn", &Message::conn, "The connection ID info for routing a reply")
        .def_readonly("access", &Message::access,
                "The access level of the invoker (which can be higher than the access level required for the command category")
        .def("dataview", [](const Message& m) {
            py::list l{m.data.size()};
            for (auto& part : m.data)
                l.append(py::memoryview::from_memory(part.data(), part.size()));
            return l;
        },
        R"(Returns a list of the data message parts as memoryviews.

Note that the returned views are only valid for the duration of the callback invoked with the
Message; if you need them beyond that then you must copy them (e.g. by calling message.data()
or .to_bytes() on each one)"
        )
        .def("data", [](const Message& m) {
            py::list l{m.data.size()};
            for (auto& part : m.data)
                l.append(py::bytes{part.data(), part.size()});
            return l;
        },
        "Returns a *copy* of the data message parts as a list of `bytes`."
        )
        .def("reply", [](Message& m, py::args args) {
            m.send_reply(send_option::data_parts(extract_data_parts(args)));
        },
        R"(Sends a reply back to this caller.

`args` must be bytes, str, or iterables thereof (and will be flatted).  Should only be used from a
request_command endpoint (i.e. when .is_reply is true)")
        .def("back", [](Message& m, std::string command, py::args args) {
            m.send_back(command, send_option::data_parts(extract_data_parts(args)));
        },
        "command"_a,
        R"(Sends a new message (NOT a reply) to the caller.

This is used to send a simple message back to the caller which is *not* specifically a request reply
but rather is simply an endpoint to invoke on the caller who sent this message to us. `command`
should be the command endpoint, and `response` must be none (for no data parts), bytes, str, or an
iterable thereof.

This is a shortcut for `oxenmq.send(msg.conn, command, *args)`; use the full version if you need
extra send functionality.)")
        .def("request", [](Message& m, std::string command, OxenMQ::ReplyCallback callback, py::args args) {
            std::vector<std::string> data;
            m.send_request(command, std::move(callback), send_option::data_parts(extract_data_parts(args)));
        },
        "command"_a, "on_reply"_a,
        R"(Sends a new request (NOT a reply) back to the remote caller.

This is used to send a new request back to the caller which is *not* specifically a request reply
but rather is simply a new request endpoint to invoke on the caller who sent this message to us.
`command` should be the command endpoint, and `response` must be none (for no data parts), bytes,
str, or an iterable thereof.

This is a shortcut for `oxenmq.request(msg.conn, command, on_reply, *args)`; use the full version if you need
extra send functionality.)")
        .def("later", &Message::send_later,
                R"(Returns a proxy object which can be stored and used to reply to the remote caller at a later point.

Unlike Message, the value returned here may outlive the command callback (as long as the OxenMQ
instance is still alive).)")
        ;

    py::class_<Message::DeferredSend>(msg, "DeferredSend")
        .def_property_readonly("is_reply", [](const Message::DeferredSend& m) { return !m.reply_tag.empty(); },
                "True if this message is expecting a reply (i.e. it was received on a request_command endpoint)")
        .def("reply", [](Message::DeferredSend& d, py::args args) {
            d.reply(send_option::data_parts(extract_data_parts(args)));
        },
        "response"_a,
        "Same as Message.reply(), but deferrable")
        .def("back", [](Message::DeferredSend& d, std::string command, py::args args) {
            d.back(command, send_option::data_parts(extract_data_parts(args)));
        },
        "command"_a, "response"_a = py::none(),
        "Same as Message.back(), but deferrable")
        .def("request", [](Message::DeferredSend& d, std::string command, OxenMQ::ReplyCallback callback, py::args args) {
            d.request(command, std::move(callback), send_option::data_parts(extract_data_parts(args)));
        },
        "command"_a, "on_reply"_a, "response"_a = py::none(),
        "Same as Message.request(), but deferrable")
        .def("__call__", [](Message::DeferredSend& d, py::args args, py::kwargs kwargs) {
            auto method = py::cast(&d).attr(!d.reply_tag.empty() ? "reply" : "back");
            return method(*args, **kwargs);
        },
        "Equivalent to `reply(...)` for a request message, `back(...)` for a non-request message")
        ;

    py::class_<CatHelper>(mod, "Category",
            "Helper class to add in registering category commands, returned from OxenMQ.add_category(...)")
        .def("add_command", &CatHelper::add_command)
        .def("add_request_command",
                [](CatHelper &cat,
                    std::string name,
                    std::function<py::object(Message* msg)> handler)
                {
                    cat.add_request_command(name, [handler](Message& msg) {
                        std::vector<std::string> result;
                        {
                            py::gil_scoped_acquire gil;

                            py::object obj = handler(&msg);
                            if (obj.is_none())
                                return;
                            try {
                                result = extract_data_parts(obj);
                            } catch (const std::exception& e) {
                                msg.oxenmq.log(LogLevel::warn, __FILE__, __LINE__,
                                        "Python callback returned "s + e.what());
                                return;
                            }
                        }
                        msg.send_reply(send_option::data_parts(result));
                    });
                },
                R"(Add a request command to this category.

Adds a request command, that is, a command that is always expected to reply, to this category.  The
callback must return one of:

- None - no reply will be sent; typically returned because you sent it yourself (via
  Message.send_reply()), or because you want to send it later via Message.send_later().
- bytes - will be sent as is in a single-part reply.
- str - will be sent in utf-8 encoding in a single-part reply.
- iterable object containing bytes and/or str elements: will be sent as a multi-part reply where
  each part is sent as-is (bytes) or utf8-encoded (str).

The callback also must take care not to save the provided `Message` value beyond the end of the
callback itself.)")
                ;

    py::enum_<LogLevel>(mod, "LogLevel")
        .value("fatal", LogLevel::fatal).value("error", LogLevel::error).value("warn", LogLevel::warn)
        .value("info", LogLevel::info).value("debug", LogLevel::debug).value("trace", LogLevel::trace);

    py::class_<OxenMQ> oxenmq{mod, "OxenMQ"};
    oxenmq
        .def(py::init<>())
        .def(py::init([](LogLevel level) {
            // Quick and dirty logger that logs to stderr.  It would be much nicer to take a python
            // function, but that deadlocks pretty much right away because of the crappiness of the gil.
            return std::make_unique<OxenMQ>(stderr_logger{}, level);
        }))
        .def(py::init([](py::bytes pubkey, py::bytes privkey, bool sn, OxenMQ::SNRemoteAddress sn_lookup, std::optional<LogLevel> log_level) {
            return std::make_unique<OxenMQ>(pubkey, privkey, sn, std::move(sn_lookup),
                    log_level ? OxenMQ::Logger{stderr_logger{}} : nullptr,
                    log_level.value_or(LogLevel::warn));
        }),
                "pubkey"_a = "", "privkey"_a = "", "service_node"_a = false,
                "sn_lookup"_a = nullptr, "log_level"_a = LogLevel::warn,
                R"(OxenMQ constructor.

This constructs the object but does not start it; you will typically want to first add categories
and commands, then finish startup by invoking `start()`.  (Categories and commands cannot be added
after startup).

Parameters:
- pubkey - the x25519 public key (32-byte binary string).  For a service node this is the service
  node x25519 keypair.  For non-service nodes this (and privkey) can be empty strings to
  automatically generate an ephemeral keypair.

- privkey the service node's private key (32-byte binary string), or empty to generate one.

- service_node - True if this instance should be considered a service node for the purpose of
  allowing "Access::local_sn" remote calls.  (This should be true if we are *capable* of being a
  service node, whether or not we are currently actively).  If specified as true then the pubkey and
  privkey values must not be empty.

- sn_lookup - function that takes a pubkey key (32-byte bytes) and returns a connection string such
  as "tcp://1.2.3.4:23456" to which a connection should be established to reach that service node.
  Note that this function is only called if there is no existing connection to that service node,
  and that the function is never called for a connection to self (that uses an internal connection
  instead).  Also note that the service node must be listening in curve25519 mode (otherwise we
  couldn't verify its authenticity).  Should return empty for not found or if SN lookups are not
  supported.  If omitted a stub function is used that always returns empty.

- log_level the initial log level; defaults to warn.  The log level can be changed later by calling
  log_level(...).  Note that currently all logging goes directly to stderr because of obstacles with
  Python's GIL.  In the future a Python callback may be supported for log messages.
)")

        .def_readwrite("handshake_time", &OxenMQ::HANDSHAKE_TIME,
                "How long to wait for handshaking to complete on new connections before timing out.")
        .def_readwrite("ephemeral_routing_id", &OxenMQ::EPHEMERAL_ROUTING_ID,
                R"(Whether to use random connection IDs for outgoing connections.

If set to True then use random connection IDs for each outgoing connection rather than the default
IDs based on the local pubkey (False).  Using the same connection ID allows re-establishing
connections (even after an application restart) with a remote without losing incoming messages, but
does not allow multiple connections to a remote using the same keypair.)")
        .def_readwrite("max_message_size", &OxenMQ::MAX_MSG_SIZE,
                R"(Maximum incoming message size.

If a remote attempts to send something larger the connection is closed.  -1 means no limit.)")
        .def_readwrite("max_sockets", &OxenMQ::MAX_SOCKETS,
                R"(Maximum open sockets supported by the internal zmq layer.

Defaults to a large number.  If changing then this must be set before start() is called.)")
        .def_readwrite("reconnect_interval", &OxenMQ::RECONNECT_INTERVAL,
                "Minimum time to wait before attempting to reconnect a failed connection.")
        .def_readwrite("reconnect_interval_max", &OxenMQ::RECONNECT_INTERVAL_MAX,
                R"(Maximum reconnect interval.

When larger than omq.reconnect_interval then upon subsequent reconnection failures an
exponential backoff will be used, up to at most this interval between reconnection attempts.)")
        .def_readwrite("close_linger", &OxenMQ::CLOSE_LINGER,
                "How long (at most) to wait for connections to close cleanly when closing.")
        .def_readwrite("connection_check_interval", &OxenMQ::CONN_CHECK_INTERVAL,
                R"(How frequently we cleanup connections.

Cleaning up involves closing idle connections and calling connect or request failure callbacks.
Making this slower results in more "overshoot" before failure callbacks are invoked; making it too
fast results in more proxy thread overhead.  Any change to this variable must be set before calling
start().)")
        .def_readwrite("connection_heartbeat", &OxenMQ::CONN_HEARTBEAT,
                R"(Whether to enable heartbeats on incoming/outgoing connections.

If set to > 0 then we set up ZMQ to send a heartbeat ping over the socket this often, which helps
keep the connection alive and lets failed connections be detected sooner (see also
connection_heartbeat_timeout).  Changing the value only affects new connections.

Defaults to 15s)")
        .def_readwrite("connection_heartbeat_timeout", &OxenMQ::CONN_HEARTBEAT_TIMEOUT,
                R"(How long after missing heartbeats to consider a socket dead.

When .conn_heartbeat is enabled, this sets how long we wait for a reply on a socket before
considering the socket to have died and closing it.  Changing the value only affects new
connections.)")
        .def_readwrite("startup_umask", &OxenMQ::STARTUP_UMASK,
                R"(The umask to apply when constructing sockets

This primarily affects any new ipc:// listening sockets that get created.  Does nothing if set to -1
(the default), and does nothing on Windows. Note that the umask is applied temporarily during
`start()`, so may affect other threads that create files/directories at the same time as the start()
call.)")
        .def_property_readonly("pubkey",
                [](const OxenMQ& self) {
                    auto& pub = self.get_pubkey();
                    return py::bytes(pub.data(), pub.size());
                },
                "Accesses this OxenMQ's x25519 public key, as bytes.")
        .def_property_readonly("privkey",
                [](const OxenMQ& self) {
                    auto& priv = self.get_privkey();
                    return py::bytes(priv.data(), priv.size());
                },
                "Accesses this OxenMQ's x25519 private key, as bytes.")
        .def("start", &OxenMQ::start, R"(Starts the OxenMQ object.

This is called after all initialization (categories, etc.) is configured.  This binds to the bind
locations given in the constructor and launches the proxy thread to handle message dispatching
between remote nodes and worker threads.

Things you want to do before calling this:
- Use `add_category`/`add_command` to set up any commands remote connections can invoke.
- If any commands require SN authentication, specify a list of currently active service node
  pubkeys via `set_active_sns()` (and make sure this gets updated when things change by
  another `set_active_sns()` or a `update_active_sns()` call).  It *is* possible to make the
  initial call after calling `start()`, but that creates a window during which incoming
  remote SN connections will be erroneously treated as non-SN connections.
- If this LMQ instance should accept incoming connections, set up any listening ports via
  `listen_curve()` and/or `listen_plain()`.)")
        .def("listen", [](OxenMQ& self,
                    std::string bind,
                    bool curve,
                    OxenMQ::AllowFunc allow,
                    std::function<void(bool success)> on_bind) {
            if (curve)
                self.listen_curve(bind, std::move(allow), std::move(on_bind));
            else
                self.listen_plain(bind, std::move(allow), std::move(on_bind));
        },
        "bind"_a, "curve"_a, kwonly, "allow_connection"_a = nullptr, "on_bind"_a = nullptr,
        R"(Start listening on the given bind address.

Incoming connections can come from anywhere.  `allow_connection` is invoked for any incoming
connections on this address to determine the incoming remote's access and authentication level.

This method may be called after start if dynamic listening is required, but it is generally
recommended that long-term fixed listening endpoints be set up by calling this *before* start().

Parameters:

- bind - can be any bind address string zmq supports, for example a tcp IP/port combination such as:
  "tcp://*:4567" or "tcp://1.2.3.4:5678".

- curve - whether the connection is curve-encrypted (True) or plaintext (False).  For plaintext
  connections the allow_connection callback will be invoked with an empty remote pubkey and
  service_node set to False.

- allow_connection function to call to determine whether to allow the connection and, if so, the
  authentication level it receives.  The function is called with the remote's address, the remote's
  32-byte pubkey (only for curve; empty for plaintext), and whether the remote is recognized as a
  service node (always False for plaintext; requires sn_lookup being configured in construction).
  If omitted (or null) the default returns AuthLevel::none access for all incoming connections.

- on_bind a callback to invoke when the port has been successfully opened or failed to open, called
  with a single boolean argument of True for success, False for failure.  For addresses set up
  before .start() this will be called during `start()` itself; for post-start listens this will be
  called from the proxy thread when it opens the new port.  Note that this function is called
  directly from the proxy thread and so should be fast and non-blocking.
)")
        .def("add_tagged_thread", [](OxenMQ& self, std::string name, std::function<void()> start) {
            return self.add_tagged_thread(std::move(name), std::move(start));
        },
        "name"_a, kwonly, "start"_a = std::nullopt,
        R"(Creates a "tagged thread".

This creates a thread with a specific "tag" and starts it immediately.  A tagged thread is one that
batches, jobs, and timer jobs can be sent to specifically, typically to perform coordination of some
thread-unsafe work, that will be reserved for use only for jobs that specifically request its use.

Tagged threads will *only* process jobs sent specifically to them; they do not participate in the
thread pool used for regular jobs.  Each tagged thread also has its own job queue completely
separate from any other jobs.

Tagged threads must be created *before* `start()` is called.  The name will be used to set the
thread name in the process table (if supported on the OS).

Parameters:

- name - the name of the thread; will be used in log messages and (if supported by the OS) as the
  system thread name.

- start - an optional callback to invoke from the thread as soon as OxenMQ itself starts up (i.e.
  after a call to `start()`).

Returns a TaggedThreadID object that can be passed to job(), batch(), or add_timer() to direct the
task to the tagged thread.
)")
        .def("set_general_threads", &OxenMQ::set_general_threads, "threads"_a,
                R"(Sets the number of general threads to use for handling requests.

This controls the maximum number of threads that may be created to deal with general tasks such as
handling incoming commands.  Cannot be called after `start()`.

If not called then the default is to use the number of CPUs/threads detected on the system.  

Changing this also affects the default value of set_batch_threads() and set_reply_threads().)")
        .def("set_reply_threads", &OxenMQ::set_reply_threads, "threads"_a,

                R"(Set the minimum number of threads used for processing replies.

This sets the minimum number of threads dedicated to processing incoming request commands (i.e. that
need a reply) and related operations such as the on_success/on_failure handlers for establishing
connections.  Cannot be called after `start()`.

If unset, this defaults to one-eighth of the number of general threads, rounded up.

Note that any such tasks would *first* use general threads; this limit would allow spawning new
threads only if all general threads are currently busy *and* there are not at least this many reply
tasks currently being processed.)")

        .def("set_batched_threads", &OxenMQ::set_batch_threads,
                "threads"_a, R"(Set the maximum number of threads to spawn for batch jobs.

This sets the limit on the maximum number of threads that may be created to process batch jobs,
including explicit batch jobs and timers scheduled via add_timer(), but not including jobs that are
sent to a specific TaggedThreadID.  Cannot be called after `start()`.

Batch jobs will first attempt to use an available general thread; an extra thread will be spawned to
handle a batch job only if all general threads are currently busy *and* fewer than this many threads
are currently processing batch jobs.)")

        .def("add_timer", py::overload_cast<
                std::function<void()>,
                std::chrono::milliseconds,
                bool,
                std::optional<TaggedThreadID>>(&OxenMQ::add_timer),
                "job"_a, "interval"_a, kwonly, "squelch"_a = true, "thread"_a = std::nullopt,
                R"(Adds a callback to be invoked on a repeating timer.

The callback will be invoke approximately every `interval`.

Optional parameters:

- squelch - When True (the default) this job will not be double-booked: that is, the callback will
  be skipped if a previous callback from this timer is already scheduled (or is still running).  If
  set to False then the callback will be scheduled even if an existing callback has not yet
  completed.

- thread - a TaggedThreadID specifying a tagged thread (created with `add_tagged_thread`) in which
  the timer should run.  If unspecified then the timer runs in the general batch job queue.)")
        .def("cancel_timer", &OxenMQ::cancel_timer, R"(Cancels a timer previously added with add_timer().

Note that this does not queue any already-scheduled timer job and as a result it is possible for an
already-scheduled timer job to run *after* this call removes the timer.

It is permitted to call this with the same TimerID multiple times; subsequent calls have no effect.)")
        .def("job", &OxenMQ::job, "job"_a, py::kw_only(), "thread"_a = std::nullopt,
                R"(Queues a job to be run as an OxenMQ job.

This submits a callback to be invoked by OxenMQ.  The job can either be scheduled with general batch
jobs or can be directed to a specific tagged thread (created with `add_tagged_thread`).)")
        .def("add_category", &OxenMQ::add_category,
                "name"_a, "access_level"_a, kwonly, "reserved_threads"_a = 0, "max_queue"_a = 200,
                py::keep_alive<0, 1>(),
                R"(Add a new command category.

Returns an object that adds commands or request commands to the category.

Parameters:

name - the category name; must be non-empy and may not contain a ".".

access_level - the access requirements for remote invocation of the commands inside this category.
Can either be a full Access instance or, when you don't care about SN status, simply an AuthLevel
value.

reserved_threads - if > 0 then always allow this many endpoints of this category to be processed at
once.  In particular if no worker thread is available and fewer than this are currently processing
commands in this category then we will spawn a new thread to handle the task.  This is used to
ensure that some categories always have processing capacity even if other long-running tasks are
using all general threads.

max_queue - the maximum number of incoming commands that will be queued for commands in this
category waiting for an available thread to process them before we start dropping new incoming
commands.  -1 means unlimited, 0 means we never queue (i.e. we drop if no thread is immediately
available).
)")
        .def("add_command_alias", &OxenMQ::add_command_alias,
                "from"_a, "to"_a,
                R"(Adds a command alias.

This allows adding backwards-compatible aliases for commands that have moved.  For example mapping
"cat.meow" to "dog.bark" would make any incoming requests to the cat.meow endpoint be treated as if
they had requested the dog.bark endpoint.  Note that this mapping happens *before* applying category
permissions: in this example, the required permissions the access the endpoint would be those of the
"dog" category rather than the "cat" category.)")
        .def("connect_remote", [](OxenMQ& self,
                    const address& remote,
                    OxenMQ::ConnectSuccess on_success,
                    OxenMQ::ConnectFailure on_failure,
                    std::optional<std::chrono::milliseconds> timeout,
                    std::optional<bool> ephemeral_routing_id) {

            return self.connect_remote(remote, std::move(on_success), std::move(on_failure), 
                    connect_option::timeout{timeout.value_or(oxenmq::REMOTE_CONNECT_TIMEOUT)},
                    connect_option::ephemeral_routing_id{ephemeral_routing_id.value_or(self.EPHEMERAL_ROUTING_ID)}
                    );
        },
        "remote"_a, "on_success"_a, "on_failure"_a,
        kwonly,
        "timeout"_a = std::nullopt, "ephemeral_routing_id"_a = std::nullopt,
        R"(
Starts connecting to a remote address and return immediately.  The connection can be used
immediately, however messages will only be queued until the connection is established (or dropped if
the connection fails).  The given callbacks are invoked for success or failure.

`ephemeral_routing_id` and `timeout` allowing overriding the defaults (oxenmq.EPHEMERAL_ROUTING_ID
and 10s, respectively).
)")
        .def("connect_remote", [](OxenMQ& self, const address& remote, std::chrono::milliseconds timeout) {
            std::promise<ConnectionID> promise;
            self.connect_remote(
                    remote,
                    [&promise](ConnectionID id) { promise.set_value(std::move(id)); },
                    [&promise](auto, std::string_view reason) {
                        promise.set_exception(std::make_exception_ptr(
                                    std::runtime_error{"Connection failed: " + std::string{reason}}));
                    });
            return promise.get_future().get();
        }, "remote"_a, "timeout"_a = oxenmq::REMOTE_CONNECT_TIMEOUT,
        R"(Simpler version of connect_remote that connects to a remote address synchronously.

This will block until the connection is established or times out; throws on connection failure,
returns the ConnectionID on success.

Takes the address and an optional `timeout` to override the timeout (default 10s))")
        .def("connect_sn", [](OxenMQ& self,
                    py::bytes pubkey,
                    std::optional<std::chrono::milliseconds> keep_alive,
                    std::optional<std::string> remote_hint,
                    std::optional<bool> ephemeral_routing_id) {
            return self.connect_sn(std::string{pubkey},
                    connect_option::keep_alive{keep_alive.value_or(-1ms)},
                    connect_option::hint{remote_hint.value_or("")},
                    connect_option::ephemeral_routing_id{ephemeral_routing_id.value_or(self.EPHEMERAL_ROUTING_ID)});
        }, "pubkey"_a, kwonly, "keep_alive"_a, "remote_hint"_a, "ephemeral_routing_id"_a,
        R"(Connect to a remote service node by pubkey.

Try to initiate a connection to the given SN in anticipation of needing a connection in the future.
If a connection is already established, the connection's idle timer will be reset (so that the
connection will not be closed too soon).  If the given idle timeout is greater than the current idle
timeout then the timeout increases to the new value; if less than the current timeout it is ignored.
(Note that idle timeouts only apply if the existing connection is an outgoing connection).

Note that this method (along with send) doesn't block waiting for a connection; it merely instructs
the proxy thread that it should establish a connection.

Parameters:
- pubkey - the public key (length 32 bytes value) of the service node to connect to; the remote's
  address will be determined by calling the sn_lookup function giving during construction.

- keep_alive - how long the SN connection will be kept alive after valid activity.  Defaults to 5
  minutes, which is notably longer than the default 30s for automatic connections established when
  using `send()` with a pubkey.

- remote_hint - a connection string that *may* be used instead of doing a lookup via the sn_lookup
  callback to find the remote address.  Typically provided only if the location has already been
  looked up for some other reason.

- ephemeral_routing_id - if set, override the default OxenMQ.EPHEMERAL_ROUTING_ID for this
  connection.

Returns a ConnectionID that identifies an connection with the given SN.  Typically you *don't* need
to worry about saving this (and can just discard it): you can always simply pass the pubkey into
send/request methods to send to the SN by pubkey.
)")
        .def("connect_inproc", &OxenMQ::connect_inproc<>,
                "on_success"_a, "on_failure"_a,
                R"(Establish a connection to ourself.

Connects to the built-in in-process listening socket of this OxenMQ server for local communication.
Note that auth_level defaults to admin (unlike connect_remote), and the default timeout is much
shorter.

Also note that incoming inproc requests are unauthenticated: that is, they will always have
admin-level access.
)")
        .def("disconnect", &OxenMQ::disconnect,
                "conn"_a, "linger"_a = 1s,
                R"(Disconnect an established connection.

Disconnects an established outgoing connection established with `connect_remote()` (or, less
commonly, `connect_sn()`).

`linger` allows you to control how long (at most) the connection will be allowed to linger if there
are still pending messages to be delivered; if those messages are not delivered within the given
time then the connection is closed anyway.  (Note that this is non-blocking: the lingering occurs in
the background).)")

        .def("send", [](OxenMQ& self, std::variant<ConnectionID, py::bytes> conn,
                    std::string command,
                    py::args args, py::kwargs kwargs) {

            if (auto* bytes = std::get_if<py::bytes>(&conn)) {
                if (len(*bytes) != 32)
                    throw std::logic_error{"Error: send(...) to=pubkey requires 32-byte pubkey"};
                conn.emplace<ConnectionID>(*bytes);
            }

            bool request = kwargs.contains("request") && kwargs["request"].cast<bool>();
            std::optional<py::function> on_reply, on_reply_failure;
            if (request) {
                if (kwargs.contains("on_reply"))
                    on_reply = kwargs["on_reply"].cast<py::function>();
                if (kwargs.contains("on_reply_failure"))
                    on_reply_failure = kwargs["on_reply_failure"].cast<py::function>();
            } else if (kwargs.contains("on_reply") || kwargs.contains("on_reply_failure")) {
                throw std::logic_error{"Error: send(...) on_reply=/on_reply_failure= option "
                    "requires request=True (perhaps you meant to use `.request(...)` instead?)"};
            }
            send_option::hint hint{kwargs.contains("remote_hint") ? kwargs["remote_hint"].cast<std::string>() : ""s};
            send_option::optional optional{kwargs.contains("optional") && kwargs["optional"].cast<bool>()};
            send_option::incoming incoming{kwargs.contains("incoming_only") && kwargs["incoming_only"].cast<bool>()};
            send_option::outgoing outgoing{kwargs.contains("outgoing") && kwargs["outgoing"].cast<bool>()};
            send_option::keep_alive keep_alive{
                kwargs.contains("keep_alive") ? kwargs["keep_alive"].cast<std::chrono::milliseconds>() : -1ms};
            send_option::request_timeout request_timeout{
                kwargs.contains("request_timeout") ? kwargs["request_timeout"].cast<std::chrono::milliseconds>() : -1ms};
            send_option::queue_failure qfail;
            if (kwargs.contains("queue_failure"))
                qfail.callback = [f = kwargs["queue_failure"].cast<std::function<void(std::string error)>>()]
                    (const zmq::error_t* exc) {
                        if (exc)
                            f(exc->what());
                    };
            send_option::queue_full qfull;
            if (kwargs.contains("queue_full"))
                qfull.callback = kwargs["queue_full"].cast<std::function<void()>>();

            std::vector<std::string> data;
            for (auto arg: args)
                extract_data_parts(data, arg);

            if (!request) {
                self.send(std::get<ConnectionID>(conn), command,
                        hint, optional, incoming, outgoing, keep_alive, request_timeout,
                        std::move(qfail), std::move(qfull));
            } else {
                auto reply_cb = [on_reply = std::move(on_reply), on_fail = std::move(on_reply_failure)]
                    (bool success, std::vector<std::string> data) {

                        if (success ? !on_reply : !on_fail)
                            return;
                        py::gil_scoped_acquire gil;
                        py::list l;
                        if (success) {
                            for (const auto& part : data)
                                l.append(py::memoryview::from_memory(part.data(), part.size()));
                            (*on_reply)(l);
                        } else if (on_fail) {
                            for (const auto& part : data)
                                l.append(py::bytes(part.data(), part.size()));
                            (*on_fail)(l);
                        }
                    };

                self.request(std::get<ConnectionID>(conn), command, std::move(reply_cb),
                        hint, optional, incoming, outgoing, keep_alive, request_timeout,
                        std::move(qfail), std::move(qfull));
            }
        },
        "conn"_a, "command"_a,
        R"(Sends a message or request to a remote.

The message is passed to the internal proxy thread to be queued for delivery (i.e. this function
does not block).

Parameters:

- conn - the ConnectionID or the 32-byte `bytes` pubkey to send the message to; the latter generally
  requires construction with a sn_lookup callback to resolve pubkeys to connection strings.

- command - the endpoint name, e.g. "category.command".  This can be a command or a request endpoint
  on the remote, but note that to properly speak to request endpoints you will also need to specify
  `request=True` (or use the `.request()` wrapper method).

- args - any additional non-keyword arguments must be str, bytes, or iterables of strings or bytes.
  These will be flattened and sent as the data part of the message.  str values are decoded to utf8;
  bytes values are sent as-is.

The following keyword arguments may be provided:

- request - if true, send this as a request rather than a command.  This *must* be True for request
  endpoints and must be False (or omitted) for non-request endpoints.  See also `request()` which
  wraps `send()` to specify this for you.

- on_reply - function to call when a response to the request is received, when making a request with
  request=True.  The function will be invoked with a single argument of a list of memoryviews into
  the data returned by the remote side.  Note that these views are only valid for the duration of
  the callback: if the data needs to be preserved beyond the callback then the callback must copy
  it.  If this value is omitted or None then any successful response is simply discarded.

- on_reply_failure - function to call if we do not get a successful reply, either for a timeout or
  because the remote sent us a failure reply.  Called with a list of bytes containing failure
  information.  The most common responses are:

    [b'TIMEOUT'] - no reply was received within the request timeout period
    [b'UNKNOWNCOMMAND'] - the remote did not understand the given request command
    [b'FORBIDDEN'] - this client does not have the requested access to invoke that command
    [b'FORBIDDEN_SN'] - the command is only available to service nodes and our pubkey was not
        recognized as an active service node pubkey.
    [b'NOT_A_SERVICE_NODE'] - the command is only invokable on service nodes, and the remote is not
        currently running as a service node.

  Note, however, that empty responses are possible, and that the array can contain multiple elements
  if additional context is provided by the other end.

- request_timeout - how long to wait for a reply to the request before giving up and calling the
  reply callback with a failure status.  The default, if unspecified, is 15 seconds.  Should only be
  specified when request=True.

- queue_failure - a callback to invoke with an error message if we are unable to queue the message
  for some reason (e.g. because the recipient is no longer reachable available).  This does *not*
  include an inability because we have too much queued already: see the next option for that.  Note
  that queueing occurs in another thread and so this method will be invoked sometime *after* the
  send() call returns.

- queue_full - a callback to invoke if we are unable to queue the message because we have too many
  messages already queued for delivery to that remote.  Typically this indicates some network
  connectivity problem preventing delivery, or the remote may be non-responsive.

For messages being sent to service nodes by pubkey (which requires having provided a `sn_lookup`
function during construction) the following options are also available:

- optional - if True then only send this message if we are already connected to the given service
  node, but do not establish a new connection if we are not.  Only has an effect when `conn` refers
  to a service node pubkey.

- incoming_only - if True then only send this message along an incoming connection from the
  service node pubkey, and do not send it if we have no such connection.

- outgoing - if True then only send this message to the remote using an outgoing connection; an
  existing connection will be used if already established, otherwise a new outgoing connection will
  be made.  Unlike a send() call without this option, existing *incoming* connections from the
  referenced service nodes will not be used to send the message.

- keep_alive - if specified and set to a datetime.timedelta then the given value will be used for
  the keep-alive of an outgoing connection; for existing connections the keep-alive will be
  increased if currently shorter, and for new connections this sets the keep-alive.  Has no effect
  if the messages uses an existing incoming connection.
)")
        .def("request", [](py::handle self, py::args args, py::kwargs kwargs) {
            self.attr("send")(*args, **kwargs, "request"_a = true);
        },
        "Convenience shortcut for oxenmq.send(..., request=True)")
        .def("inject_task", &OxenMQ::inject_task,
                "category"_a, "command"_a, "remote"_a, "callback"_a,
                R"(Inject a callback as if it were a remotely invoked command.

This method takes a callback and queues it to be invoked as if it had been called remotely.  This allows
external command processing to be combined with oxenmq task scheduling.

For example, oxen-core uses this to handle RPC requests coming in over HTTP as if they were incoming
OxenMQ RPC requests, with the same scheduling and queuing of requests applied to both HTTP and
OxenMQ requests so that both are treated fairly in terms of processing priority.)")
        ;

    py::enum_<std::future_status>(mod, "future_status")
        .value("deferred", std::future_status::deferred)
        .value("ready", std::future_status::ready)
        .value("timeout", std::future_status::timeout);
    py::class_<std::future<py::list>>(mod, "ResultFuture",
            "Wrapper around a C++ future allowing inspecting and waiting for the result to become available.")
        .def("get", [](std::future<py::list>& f) {
            {
                py::gil_scoped_release no_gil;
                f.wait();
            }
            return f.get();
        }, "Gets the result (or raises an exception if the result raised an exception); must only be called once")
        .def("valid", [](std::future<py::list>& f) { return f.valid(); },
                "Returns true if the result is available")
        .def("wait", &std::future<py::list>::wait, py::call_guard<py::gil_scoped_release>(),
                "Waits indefinitely for the result to become available")
        .def("wait_for", &std::future<py::list>::template wait_for<double, std::ratio<1>>,
                py::call_guard<py::gil_scoped_release>(),
                "Waits up to the given timedelta for the result to become available")
        .def("wait_for", [](std::future<py::list>& f, double seconds) {
            return f.wait_for(std::chrono::duration<double>{seconds}); },
            py::call_guard<py::gil_scoped_release>(),
            "Waits up to the given number of seconds for the result to become available")
        .def("wait_until",
                &std::future<py::list>::template wait_until<std::chrono::system_clock, std::chrono::system_clock::duration>,
                py::call_guard<py::gil_scoped_release>(),
                "Wait until the given datetime for the result to become available")
        ;

    oxenmq.def("request_future", [](py::handle self, py::args args, py::kwargs kwargs) {
        if (kwargs.contains("on_reply") || kwargs.contains("on_reply_failure"))
            throw std::logic_error{"Cannot call request_wait(...) with on_reply= or on_reply_failure="};

        auto result = std::make_shared<std::promise<py::list>>();
        auto fut = result->get_future();
        auto on_reply = [result](py::list value) {
            assert(len(value) == 0 || py::isinstance<py::memoryview>(value[0]));
            for (int i = len(value) - 1; i >= 0; i--)
                value[i] = value[i].attr("tobytes")();
        };
        auto on_fail = [result](py::list value) {
            if (len(value) > 0 && (std::string) py::bytes(value[0]) == "TIMEOUT"sv) {
                auto msg = len(value) > 1 ? (std::string) py::bytes(value[1]) : "Request timed out"s;
                PyErr_SetString(PyExc_TimeoutError, msg.c_str());
                result->set_exception(std::make_exception_ptr(py::error_already_set{}));
            }

            std::string err;
            for (auto& m : value) {
                if (!err.empty()) err += ", ";
                err += py::str(m);
            }
            result->set_exception(std::make_exception_ptr(std::runtime_error{"Request failed: " + err}));
        };

        self.attr("request")(*args, **kwargs,
                "on_reply"_a = std::move(on_reply),
                "on_reply_failure"_a = std::move(on_fail));
        return fut;
    }, R"(Initiate a request with a future.

Initiates a request and returns a future that is used to check and wait for a response to the
request.  Takes the same arguments as .request(...), but without the `on_reply` and
`on_reply_failure` options.

This can be used to make a synchronous request by simply calling .get() on the returned future:

    try:
        response = omq.request_future(conn, "cat.cmd", "data").get()
    except TimeoutError as e:
        print("Request timed out!")
    except Exception:
        print("Request failed :(")

More powerfully, you can issue multiple, parallel requests storing the returned futures then .get()
all of them to collect the responses.)");
}

} // namespace oxenmq
