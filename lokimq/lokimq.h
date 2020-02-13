// Copyright (c) 2019-2020, The Loki Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once

#include <string>
#include <list>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <iostream>
#include <chrono>
#include <atomic>
#include <cassert>
#include <zmq.hpp>
#include "bt_serialize.h"
#include "string_view.h"

#if ZMQ_VERSION < ZMQ_MAKE_VERSION (4, 3, 0)
// Timers were not added until 4.3.0
#error "ZMQ >= 4.3.0 required"
#endif

namespace lokimq {

using namespace std::literals;

/// Logging levels passed into LogFunc.  (Note that trace does nothing more than debug in a release
/// build).
enum class LogLevel { fatal, error, warn, info, debug, trace };

/// Authentication levels for command categories and connections
enum class AuthLevel {
    denied, ///< Not actually an auth level, but can be returned by the AllowFunc to deny an incoming connection.
    none, ///< No authentication at all; any random incoming ZMQ connection can invoke this command.
    basic, ///< Basic authentication commands require a login, or a node that is specifically configured to be a public node (e.g. for public RPC).
    admin, ///< Advanced authentication commands require an admin user, either via explicit login or by implicit login from localhost.  This typically protects administrative commands like shutting down, starting mining, or access sensitive data.
};

/// The access level for a command category
struct Access {
    /// Minimum access level required
    AuthLevel auth = AuthLevel::none;
    /// If true only remote SNs may call the category commands
    bool remote_sn = false;
    /// If true the category requires that the local node is a SN
    bool local_sn = false;
};

/// Return type of the AllowFunc: this determines whether we allow the connection at all, and if
/// so, sets the initial authentication level and tells LokiMQ whether the other hand is an
/// active SN.
struct Allow {
    AuthLevel auth = AuthLevel::none;
    bool remote_sn = false;
};

class LokiMQ;

/// Encapsulates an incoming message from a remote connection with message details plus extra
/// info need to send a reply back through the proxy thread via the `reply()` method.  Note that
/// this object gets reused: callbacks should use but not store any reference beyond the callback.
class Message {
public:
    LokiMQ& lokimq; ///< The owning LokiMQ object
    std::vector<string_view> data; ///< The provided command data parts, if any.
    string_view id; ///< The remote's unique, opaque id for routing.
    string_view pubkey; ///< The remote's pubkey (32 bytes)
    bool service_node; ///< True if the pubkey is an active SN (note that this is only checked on initial connection, not every received message)
    std::string reply_tag; ///< If the invoked command is a request command this is the required reply tag that will be prepended by `send_reply()`.

    /// Constructor
    Message(LokiMQ& lmq) : lokimq{lmq} {}

    // Non-copyable
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;

    /// Sends a command back to whomever sent this message.  Arguments are forwarded to send() but
    /// with send_option::optional{} added if the originator is not a SN.  For SN messages (i.e.
    /// where `sn` is true) this is a "strong" reply by default in that the proxy will attempt to
    /// establish a new connection to the SN if no longer connected.  For non-SN messages the reply
    /// will be attempted using the available routing information, but if the connection has already
    /// been closed the reply will be dropped.
    ///
    /// If you want to send a non-strong reply even when the remote is a service node then add
    /// an explicit `send_option::optional()` argument.
    template <typename... Args>
    void send_back(const std::string& command, Args&&... args);

    /// Sends a reply to a request.  This takes no command: the command is always the built-in
    /// "REPLY" command, followed by the unique reply tag, then any reply data parts.  All other
    /// arguments are as in `send_back()`.
    template <typename... Args>
    void send_reply(Args&&... args);
};

// Forward declarations; see batch.h
namespace detail { class Batch; }
template <typename R> class Batch;

/** The keep-alive time for a send() that results in a establishing a new outbound connection.  To
 * use a longer keep-alive to a host call `connect()` first with the desired keep-alive time or pass
 * the send_option::keep_alive.
 */
static constexpr auto DEFAULT_SEND_KEEP_ALIVE = 30s;

// How frequently we cleanup connections (closing idle connections, calling connect or request failure callbacks)
static constexpr auto CONN_CHECK_INTERVAL = 1s;

// The default timeout for connect_remote()
static constexpr auto REMOTE_CONNECT_TIMEOUT = 10s;

// The minimum amount of time we wait for a reply to a REQUEST before calling the callback with
// `false` to signal a timeout.
static constexpr auto REQUEST_TIMEOUT = 15s;

/// Maximum length of a category
static constexpr size_t MAX_CATEGORY_LENGTH = 50;

/// Maximum length of a command
static constexpr size_t MAX_COMMAND_LENGTH = 200;

/**
 * Class that handles LokiMQ listeners, connections, proxying, and workers.  An application
 * typically has just one instance of this class.
 */
class LokiMQ {

private:

    /// The global context
    zmq::context_t context;

    /// A unique id for this LokiMQ instance, assigned in a thread-safe manner during construction.
    const int object_id;

    /// The x25519 keypair of this connection.  For service nodes these are the long-run x25519 keys
    /// provided at construction, for non-service-node connections these are generated during
    /// construction.
    std::string pubkey, privkey;

    /// True if *this* node is running in service node mode (whether or not actually active)
    bool local_service_node = false;

    /// Addresses on which to listen, or empty if we only establish outgoing connections but aren't
    /// listening.
    std::vector<std::string> bind;

    /// The thread in which most of the intermediate work happens (handling external connections
    /// and proxying requests between them to worker threads)
    std::thread proxy_thread;

    /// Will be true (and is guarded by a mutex) if the proxy thread is quitting; guards against new
    /// control sockets from threads trying to talk to the proxy thread.
    bool proxy_shutting_down = false;

    /// Called to obtain a "command" socket that attaches to `control` to send commands to the
    /// proxy thread from other threads.  This socket is unique per thread and LokiMQ instance.
    zmq::socket_t& get_control_socket();

    /// Stores all of the sockets created in different threads via `get_control_socket`.  This is
    /// only used during destruction to close all of those open sockets, and is protected by an
    /// internal mutex which is only locked by new threads getting a control socket and the
    /// destructor.
    std::vector<std::shared_ptr<zmq::socket_t>> thread_control_sockets;

public:

    /// Callback type invoked to determine whether the given new incoming connection is allowed to
    /// connect to us and to set its initial authentication level.
    ///
    /// @param ip - the ip address of the incoming connection
    /// @param pubkey - the x25519 pubkey of the connecting client (32 byte string)
    ///
    /// @returns an `AuthLevel` enum value indicating the default auth level for the incoming
    /// connection, or AuthLevel::denied if the connection should be refused.
    using AllowFunc = std::function<Allow(string_view ip, string_view pubkey)>;

    /// Callback that is invoked when we need to send a "strong" message to a SN that we aren't
    /// already connected to and need to establish a connection.  This callback returns the ZMQ
    /// connection string we should use which is typically a string such as `tcp://1.2.3.4:5678`.
    using SNRemoteAddress = std::function<std::string(const std::string& pubkey)>;

    /// The callback type for registered commands.
    using CommandCallback = std::function<void(Message& message)>;

    /// The callback for making requests.  This is called with `true` and a (moved) vector of data
    /// part strings when we get a reply, or `false` and empty vector on timeout.
    using ReplyCallback = std::function<void(bool success, std::vector<std::string> data)>;

    /// Called to write a log message.  This will only be called if the `level` is >= the current
    /// LokiMQ object log level.  It must be a raw function pointer (or a capture-less lambda) for
    /// performance reasons.  Takes four arguments: the log level of the message, the filename and
    /// line number where the log message was invoked, and the log message itself.
    using Logger = std::function<void(LogLevel level, const char* file, int line, std::string msg)>;

    /// Callback for the success case of connect_remote()
    using ConnectSuccess = std::function<void(const std::string& pubkey)>;
    /// Callback for the failure case of connect_remote()
    using ConnectFailure = std::function<void(const std::string& reason)>;

    /// Explicitly non-copyable, non-movable because most things here aren't copyable, and a few
    /// things aren't movable, either.  If you need to pass the LokiMQ instance around, wrap it
    /// in a unique_ptr or shared_ptr.
    LokiMQ(const LokiMQ&) = delete;
    LokiMQ& operator=(const LokiMQ&) = delete;
    LokiMQ(LokiMQ&&) = delete;
    LokiMQ& operator=(LokiMQ&&) = delete;

    /** How long to wait for handshaking to complete on external connections before timing out and
     * closing the connection.  Setting this only affects new outgoing connections. */
    std::chrono::milliseconds HANDSHAKE_TIME = 10s;

    /** Maximum incoming message size; if a remote tries sending a message larger than this they get
     * disconnected. -1 means no limit. */
    int64_t MAX_MSG_SIZE = 1 * 1024 * 1024;

    /** How long (in ms) to linger sockets when closing them; this is the maximum time zmq spends
     * trying to sending pending messages before dropping them and closing the underlying socket
     * after the high-level zmq socket is closed. */
    std::chrono::milliseconds CLOSE_LINGER = 5s;

private:

    /// The lookup function that tells us where to connect to a peer, or empty if not found.
    SNRemoteAddress peer_lookup;

    /// Callback to see whether the incoming connection is allowed
    AllowFunc allow_connection;

    /// The log level; this is atomic but we use relaxed order to set and access it (so changing it
    /// might not be instantly visible on all threads, but that's okay).
    std::atomic<LogLevel> log_lvl{LogLevel::warn};

    /// The callback to call with log messages
    Logger logger;

    /// Logging implementation
    template <typename... T>
    void log_(LogLevel lvl, const char* filename, int line, const T&... stuff);

    ///////////////////////////////////////////////////////////////////////////////////
    /// NB: The following are all the domain of the proxy thread (once it is started)!

    /// Addresses to bind to in `start()`
    std::vector<std::string> bind_addresses;

    /// Our listening ROUTER socket for incoming connections (will be left unconnected if not
    /// listening).
    zmq::socket_t listener;

    /// Info about a peer's established connection to us.  Note that "established" means both
    /// connected and authenticated.
    struct peer_info {
        /// Pubkey of the remote; can be empty (especially before handshake) but will only be set if
        /// the pubkey has been verified.
        std::string pubkey;

        /// True if we've authenticated this peer as a service node.
        bool service_node = false;

        /// The auth level of this peer
        AuthLevel auth_level = AuthLevel::none;

        /// Will be set to a non-empty routing prefix if if we have (or at least recently had) an
        /// established incoming connection with this peer.  Will be empty if there is no incoming
        /// connection.
        std::string incoming;

        /// The index in `remotes` if we have an established outgoing connection to this peer, -1 if
        /// we have no outgoing connection to this peer.
        int outgoing = -1;

        /// The last time we sent or received a message (or had some other relevant activity) with
        /// this peer.  Used for closing outgoing connections that have reached an inactivity expiry
        /// time.
        std::chrono::steady_clock::time_point last_activity;

        /// Updates last_activity to the current time
        void activity() { last_activity = std::chrono::steady_clock::now(); }

        /// After more than this much inactivity we will close an idle connection
        std::chrono::milliseconds idle_expiry;
    };

    /// Currently peer connections: id -> peer_info.  id == pubkey for incoming and outgoing SN
    /// connections; random string for outgoing direct connections.
    std::unordered_map<std::string, peer_info> peers;

    /// Remotes we are still trying to connect to (via connect_remote(), not connect_sn()); when
    /// we pass handshaking we move them out of here and (if set) trigger the on_connect callback.
    /// Unlike regular node-to-node peers, these have an extra "HI"/"HELLO" sequence that we used
    /// before we consider ourselves connected to the remote.
    std::vector<std::tuple<int /*remotes index*/, std::chrono::steady_clock::time_point, ConnectSuccess, ConnectFailure>> pending_connects;

    /// Pending requests that have been sent out but not yet received a matching "REPLY".  The value
    /// is the timeout timestamp.
    std::unordered_map<std::string, std::pair<std::chrono::steady_clock::time_point, ReplyCallback>>
        pending_requests;

    /// different polling sockets the proxy handler polls: this always contains some internal
    /// sockets for inter-thread communication followed by listener socket and a pollitem for every
    /// (outgoing) remote socket in `remotes`.  This must be in a sequential vector because of zmq
    /// requirements (otherwise it would be far nicer to not have to synchronize the two vectors).
    std::vector<zmq::pollitem_t> pollitems;

    /// Properly adds a socket to poll for input to pollitems
    void add_pollitem(zmq::socket_t& sock);

    /// The number of internal sockets in `pollitems`
    static constexpr size_t poll_internal_size = 3;

    /// The pollitems location corresponding to `remotes[0]`.
    const size_t poll_remote_offset; // will be poll_internal_size + 1 for a full listener (the +1 is the listening socket); poll_internal_size for a remote-only

    /// The outgoing remote connections we currently have open along with the remote pubkeys.  Each
    /// element [i] here corresponds to an the pollitem_t at pollitems[i+1+poll_internal_size].
    /// (Ideally we'd use one structure, but zmq requires the pollitems be in contiguous storage).
    /// For new connections established via connect_remote the pubkey will be empty until we
    /// do the HI/HELLO handshake over the socket.
    std::vector<std::pair<std::string, zmq::socket_t>> remotes;

    /// Socket we listen on to receive control messages in the proxy thread. Each thread has its own
    /// internal "control" connection (returned by `get_control_socket()`) to this socket used to
    /// give instructions to the proxy such as instructing it to initiate a connection to a remote
    /// or send a message.
    zmq::socket_t command{context, zmq::socket_type::router};

    /// Timers.  TODO: once cppzmq adds an interface around the zmq C timers API then switch to it.
    struct TimersDeleter { void operator()(void* timers); };
    std::unordered_map<int, std::tuple<std::function<void()>, bool, bool>> timer_jobs; // id => {func, squelch, running}
    std::unique_ptr<void, TimersDeleter> timers;
public:
    // This needs to be public because we have to be able to call it from a plain C function.
    // Nothing external may call it!
    void _queue_timer_job(int);
private:

    /// Router socket to reach internal worker threads from proxy
    zmq::socket_t workers_socket{context, zmq::socket_type::router};

    /// indices of idle, active workers
    std::vector<unsigned int> idle_workers;

    /// Maximum number of general task workers, specified by g`/during construction
    unsigned int general_workers = std::thread::hardware_concurrency();

    /// Maximum number of possible worker threads we can have.  This is calculated when starting,
    /// and equals general_workers plus the sum of all categories' reserved threads counts plus the
    /// reserved batch workers count.  This is also used to signal a shutdown; we set it to 0 when
    /// quitting.
    unsigned int max_workers;

    /// Number of active workers
    unsigned int active_workers() const { return workers.size() - idle_workers.size(); }

    /// Worker thread loop
    void worker_thread(unsigned int index);

    /// If set, skip polling for one proxy loop iteration (set when we know we have something
    /// processible without having to shove it onto a socket, such as scheduling an internal job).
    bool proxy_skip_poll = false;

    /// Does the proxying work
    void proxy_loop();

    void proxy_conn_cleanup();

    void proxy_worker_message(std::vector<zmq::message_t>& parts);

    void proxy_process_queue();

    Batch<void>* proxy_schedule_job(std::function<void()> f);

    /// Looks up a peers element given a connect index (for outgoing connections where we already
    /// knew the pubkey and SN status) or an incoming zmq message (which has the pubkey and sn
    /// status metadata set during initial connection authentication), creating a new peer element
    /// if required.
    decltype(peers)::iterator proxy_lookup_peer(int conn_index, zmq::message_t& msg);

    /// Handles built-in primitive commands in the proxy thread for things like "BYE" that have to
    /// be done in the proxy thread anyway (if we forwarded to a worker the worker would just have
    /// to send an instruction back to the proxy to do it).  Returns true if one was handled, false
    /// to continue with sending to a worker.
    bool proxy_handle_builtin(int conn_index, std::vector<zmq::message_t>& parts);

    struct run_info;
    /// Gets an idle worker's run_info and removes the worker from the idle worker list.  If there
    /// is no idle worker this creates a new `workers` element for a new worker (and so you should
    /// only call this if new workers are permitted).  Note that if this creates a new work info the
    /// worker will *not* yet be started, so the caller must create the thread (in `.thread`) after
    /// setting up the job if `.thread.joinable()` is false.
    run_info& get_idle_worker();

    /// Runs the worker; called after the `run` object has been set up.  If the worker thread hasn't
    /// been created then it is spawned; otherwise it is sent a RUN command.
    void proxy_run_worker(run_info& run);

    /// Sets up a job for a worker then signals the worker (or starts a worker thread)
    void proxy_to_worker(size_t conn_index, std::vector<zmq::message_t>& parts);

    /// proxy thread command handlers for commands sent from the outer object QUIT.  This doesn't
    /// get called immediately on a QUIT command: the QUIT commands tells workers to quit, then this
    /// gets called after all works have done so.
    void proxy_quit();

    // Sets the various properties on an outgoing socket prior to connection.
    void setup_outgoing_socket(zmq::socket_t& socket, string_view remote_pubkey = {});

    /// Common connection implementation used by proxy_connect/proxy_send.  Returns the socket
    /// and, if a routing prefix is needed, the required prefix (or an empty string if not needed).
    /// For an optional connect that fail, returns nullptr for the socket.
    std::pair<zmq::socket_t*, std::string> proxy_connect_sn(const std::string& pubkey, const std::string& connect_hint, bool optional, bool incoming_only, std::chrono::milliseconds keep_alive);

    /// CONNECT_SN command telling us to connect to a new pubkey.  Returns the socket (which could be
    /// existing or a new one).
    std::pair<zmq::socket_t*, std::string> proxy_connect_sn(bt_dict&& data);

    /// Opens a new connection to a remote, with callbacks.  This is the proxy-side implementation
    /// of the `connect_remote()` call.
    void proxy_connect_remote(bt_dict_consumer data);

    /// Called to disconnect our remote connection to the given pubkey (if we have one).
    void proxy_disconnect(const std::string& pubkey);

    /// SEND command.  Does a connect first, if necessary.
    void proxy_send(bt_dict_consumer data);

    /// REPLY command.  Like SEND, but only has a listening socket route to send back to and so is
    /// weaker (i.e. it cannot reconnect to the SN if the connection is no longer open).
    void proxy_reply(bt_dict_consumer data);

    /// Currently active batches.
    std::unordered_set<detail::Batch*> batches;
    /// Individual batch jobs waiting to run
    using batch_job = std::pair<detail::Batch*, int>;
    std::queue<batch_job> batch_jobs;
    unsigned int batch_jobs_active = 0;
    unsigned int batch_jobs_reserved = std::max((std::thread::hardware_concurrency() + 1) / 2, 1u);

    /// BATCH command.  Called with a Batch<R> (see lokimq/batch.h) object pointer for the proxy to
    /// take over and queue batch jobs.
    void proxy_batch(detail::Batch* batch);

    /// TIMER command.  Called with a serialized list containing: function pointer to assume
    /// ownership of, an interval count (in ms), and whether or not jobs should be squelched (see
    /// `add_timer()`).
    void proxy_timer(bt_list_consumer timer_data);

    /// Same, but deserialized
    void proxy_timer(std::function<void()> job, std::chrono::milliseconds interval, bool squelch);

    /// ZAP (https://rfc.zeromq.org/spec:27/ZAP/) authentication handler; this is called with the
    /// zap auth socket to do non-blocking processing of any waiting authentication requests waiting
    /// on it to verify whether the connection is from a valid/allowed SN.
    void process_zap_requests(zmq::socket_t& zap_auth);

    /// Handles a control message from some outer thread to the proxy
    void proxy_control_message(std::vector<zmq::message_t>& parts);

    /// Closing any idle connections that have outlived their idle time.  Note that this only
    /// affects outgoing connections; incomings connections are the responsibility of the other end.
    void proxy_expire_idle_peers();

    /// Helper method to actually close a remote connection and update the stuff that needs updating.
    void proxy_close_remote(int removed, bool linger = true);

    /// Closes an outgoing connection immediately, updates internal variables appropriately.
    /// Returns the next iterator (the original may or may not be removed from peers, depending on
    /// whether or not it also has an active incoming connection).
    decltype(peers)::iterator proxy_close_outgoing(decltype(peers)::iterator it);

    struct category {
        Access access;
        std::unordered_map<std::string, std::pair<CommandCallback, bool /*is_request*/>> commands;
        unsigned int reserved_threads = 0;
        unsigned int active_threads = 0;
        int max_queue = 200;
        int queued = 0;

        category(Access access, unsigned int reserved_threads, int max_queue)
            : access{access}, reserved_threads{reserved_threads}, max_queue{max_queue} {}
    };

    /// Categories, mapped by category name.
    std::unordered_map<std::string, category> categories;

    /// For enabling backwards compatibility with command renaming: this allows mapping one command
    /// to another in a different category (which happens before the category and command lookup is
    /// done).
    std::unordered_map<std::string, std::string> command_aliases;

    /// Retrieve category and callback from a command name, including alias mapping.  Warns on
    /// invalid commands and returns nullptrs.  The command name will be updated in place if it is
    /// aliased to another command.
    std::pair<category*, const std::pair<CommandCallback, bool>*> get_command(std::string& command);

    /// Checks a peer's authentication level.  Returns true if allowed, warns and returns false if
    /// not.
    bool proxy_check_auth(string_view pubkey, size_t conn_index, const peer_info& peer,
            const std::string& command, const category& cat, zmq::message_t& msg);

    /// Details for a pending command; such a command already has authenticated access and is just
    /// waiting for a thread to become available to handle it.
    struct pending_command {
        category& cat;
        std::string command;
        std::vector<zmq::message_t> data_parts;
        const std::pair<CommandCallback, bool>* callback;
        std::string pubkey;
        std::string id;
        bool service_node;

        pending_command(category& cat, std::string command, std::vector<zmq::message_t> data_parts, const std::pair<CommandCallback, bool>* callback, std::string pubkey, bool service_node)
            : cat{cat}, command{std::move(command)}, data_parts{std::move(data_parts)}, callback{callback}, pubkey{std::move(pubkey)}, service_node{service_node} {}
    };
    std::list<pending_command> pending_commands;


    /// End of proxy-specific members
    ///////////////////////////////////////////////////////////////////////////////////


    /// Structure that contains the data for a worker thread - both the thread itself, plus any
    /// transient data we are passing into the thread.
    struct run_info {
        bool is_batch_job = false;

        // If is_batch_job is false then these will be set appropriate (if is_batch_job is true then
        // these shouldn't be accessed and likely contain stale data).
        category *cat;
        std::string command;
        std::string pubkey;
        bool service_node = false;
        std::vector<zmq::message_t> data_parts;

        // If is_batch_job true then these are set (if is_batch_job false then don't access these!):
        int batch_jobno; // >= 0 for a job, -1 for the completion job

        union {
            const std::pair<CommandCallback, bool>* callback; // set if !is_batch_job
            detail::Batch* batch;                             // set if is_batch_job
        };

        // These belong to the proxy thread and must not be accessed by a worker:
        std::thread thread;
        size_t worker_id; // The index in `workers`
        std::string routing_id; // "w123" where 123 == worker_id

        /// Loads the run info with a pending command
        run_info& operator=(pending_command&& pending);
        /// Loads the run info with a batch job
        run_info& operator=(batch_job&& bj);
    };
    /// Data passed to workers for the RUN command.  The proxy thread sets elements in this before
    /// sending RUN to a worker then the worker uses it to get call info, and only allocates it
    /// once, before starting any workers.  Workers may only access their own index and may not
    /// change it.
    std::vector<run_info> workers;

public:
    /**
     * LokiMQ constructor.  This constructs the object but does not start it; you will typically
     * want to first add categories and commands, then finish startup by invoking `start()`.
     * (Categories and commands cannot be added after startup).
     *
     * @param pubkey the public key (32-byte binary string).  For a service node this is the service
     * node x25519 keypair.  For non-service nodes this (and privkey) can be empty strings to
     * automatically generate an ephemeral keypair.
     *
     * @param privkey the service node's private key (32-byte binary string), or empty to generate
     * one.
     *
     * @param service_node - true if this instance should be considered a service node for the
     * purpose of allowing "Access::local_sn" remote calls.  (This should be true if we are
     * *capable* of being a service node, whether or not we are currently actively).  If specified
     * as true then the pubkey and privkey values must not be empty.
     *
     * @param bind list of addresses to bind to.  Can be any string zmq supports; typically a tcp
     * IP/port combination such as: "tcp://\*:4567" or "tcp://1.2.3.4:5678".  Can be empty to not
     * listen at all.
     *
     * @param peer_lookup function that takes a pubkey key (32-byte binary string) and returns a
     * connection string such as "tcp://1.2.3.4:23456" to which a connection should be established
     * to reach that service node.  Note that this function is only called if there is no existing
     * connection to that service node, and that the function is never called for a connection to
     * self (that uses an internal connection instead).  Should return empty for not found.
     *
     * @param allow_incoming is a callback that LokiMQ can use to determine whether an incoming
     * connection should be allowed at all and, if so, whether the connection is from a known
     * service node.  Called with the connecting IP, the remote's verified x25519 pubkey, and the 
     * called on incoming connections with the (verified) incoming connection
     * pubkey (32-byte binary string) to determine whether the given SN should be allowed to
     * connect.
     *
     * @param log a function or callable object that writes a log message.  If omitted then all log
     * messages are suppressed.
     */
    LokiMQ( std::string pubkey,
            std::string privkey,
            bool service_node,
            std::vector<std::string> bind,
            SNRemoteAddress peer_lookup,
            AllowFunc allow_connection,
            Logger logger = [](LogLevel, const char*, int, std::string) { });

    /**
     * Simplified LokiMQ constructor for a client.  This does not bind, generates ephemeral keys,
     * and doesn't have peer_lookup capabilities, and treats all remotes as "basic", non-service
     * node connections (for command authenication purposes).
     */
    explicit LokiMQ(Logger logger = [](LogLevel, const char*, int, std::string) { })
        : LokiMQ("", "", false, {},
                [](const auto&) { return std::string{}; },
                [](string_view, string_view) { return Allow{AuthLevel::basic}; },
                std::move(logger)) {}

    /**
     * Destructor; instructs the proxy to quit.  The proxy tells all workers to quit, waits for them
     * to quit and rejoins the threads then quits itself.  The outer thread (where the destructor is
     * running) rejoins the proxy thread.
     */
    ~LokiMQ();

    /// Sets the log level of the LokiMQ object.
    void log_level(LogLevel level);

    /// Gets the log level of the LokiMQ object.
    LogLevel log_level() const;

    /**
     * Add a new command category.  This method may not be invoked after `start()` has been called.
     * This method is also not thread safe, and is generally intended to be called (along with
     * add_command) immediately after construction and immediately before calling start().
     *
     * @param name - the category name which must consist of one or more characters and may not
     * contain a ".".
     *
     * @param access_level the access requirements for remote invocation of the commands inside this
     * category.
     *
     * @param reserved_threads if non-zero then the worker thread pool will ensure there are at at
     * least this many threads either current processing or available to process commands in this
     * category.  This is used to ensure that a category's commands can be invoked even if
     * long-running commands in some other category are currently using all worker threads.  This
     * can increase the number of worker threads above the `general_workers` parameter given in the
     * constructor, but will only do so if the need arised: that is, if a command request arrives
     * for a category when all workers are busy and no worker is currently processing any command in
     * that category.
     *
     * @param max_queue is the maximum number of incoming messages in this category that we will
     * queue up when waiting for a worker to become available for this category.  Once the queue for
     * a category exceeds this many incoming messages then new messages will be dropped until some
     * messages are processed off the queue.  -1 means unlimited, 0 means we will never queue (which
     * means just dropping messages for this category if no workers are available to instantly
     * handle the request).
     */
    void add_category(std::string name, Access access_level, unsigned int reserved_threads = 0, int max_queue = 200);

    /**
     * Adds a new command to an existing category.  This method may not be invoked after `start()`
     * has been called.
     *
     * @param category - the category name (must already be created by a call to `add_category`)
     *
     * @param name - the command name, without the `category.` prefix.
     *
     * @param callback - a callable object which is callable as `callback(zeromq::Message &)`
     */
    void add_command(const std::string& category, std::string name, CommandCallback callback);

    /**
     * Adds a new "request" command to an existing category.  These commands are just like normal
     * commands, but are expected to call `msg.send_reply()` with any data parts on every request,
     * while normal commands are more general.
     *
     * Parameters given here are identical to `add_command()`.
     */
    void add_request_command(const std::string& category, std::string name, CommandCallback callback);

    /**
     * Adds a command alias; this is intended for temporary backwards compatibility: if any aliases
     * are defined then every command (not just aliased ones) has to be checked on invocation to see
     * if it is defined in the alias list.  May not be invoked after `start()`.
     *
     * Aliases should follow the `category.command` format for both the from and to names, and
     * should only be called for `to` categories that are already defined.  The category name is not
     * currently enforced on the `from` name (for backwards compatility with Loki's quorumnet code)
     * but will be at some point.
     *
     * Access permissions for an aliased command depend only on the mapped-to value; for example, if
     * `cat.meow` is aliased to `dog.bark` then it is the access permissions on `dog` that apply,
     * not those of `cat`, even if `cat` is more restrictive than `dog`.
     */
    void add_command_alias(std::string from, std::string to);

    /**
     * Sets the number of worker threads reserved for batch jobs.  If not called this defaults to
     * half the number of hardware threads available (rounded up).  This works exactly like
     * reserved_threads for a category, but allows to batch jobs.  See category for details.
     *
     * Note that some internal jobs are counted as batch jobs: in particular timers added via
     * add_timer() and replies received in response to request commands currently each take a batch
     * job slot when invoked.
     *
     * Cannot be called after start()ing the LokiMQ instance.
     */
    void set_batch_threads(unsigned int threads);

    /**
     * Sets the number of general worker threads.  This is the target number of threads to run that
     * we generally try not to exceed.  These threads can be used for any command, and will be
     * created (up to the limit) on demand.  Note that individual categories (or batch jobs) with
     * reserved threads can create threads in addition to the amount specified here if necessary to
     * fulfill the reserved threads count for the category.
     *
     * Cannot be called after start()ing the LokiMQ instance.
     */
    void set_general_threads(unsigned int threads);

    /**
     * Finish starting up: binds to the bind locations given in the constructor and launches the
     * proxy thread to handle message dispatching between remote nodes and worker threads.
     *
     * You will need to call `add_category` and `add_command` to register commands before calling
     * `start()`; once start() is called commands cannot be changed.
     */
    void start();

    /**
     * Try to initiate a connection to the given SN in anticipation of needing a connection in the
     * future.  If a connection is already established, the connection's idle timer will be reset
     * (so that the connection will not be closed too soon).  If the given idle timeout is greater
     * than the current idle timeout then the timeout increases to the new value; if less than the
     * current timeout it is ignored.  (Note that idle timeouts only apply if the existing
     * connection is an outgoing connection).
     *
     * Note that this method (along with send) doesn't block waiting for a connection; it merely
     * instructs the proxy thread that it should establish a connection.
     *
     * @param pubkey - the public key (32-byte binary string) of the service node to connect to
     * @param keep_alive - the connection will be kept alive if there was valid activity within
     *                     the past `keep_alive` milliseconds.  If an outgoing connection already
     *                     exists, the longer of the existing and the given keep alive is used.
     *                     Note that the default applied here is much longer than the default for an
     *                     implicit connect() by calling send() directly.
     * @param hint - if non-empty and a new outgoing connection needs to be made this hint value
     *               may be used instead of calling the lookup function.  (Note that there is no
     *               guarantee that the hint will be used; it is only usefully specified if the
     *               connection location has already been incidentally determined).
     */
    void connect_sn(string_view pubkey, std::chrono::milliseconds keep_alive = 5min, string_view hint = {});

    /**
     * Establish a connection to the given remote with callbacks invoked on a successful or failed
     * connection.  The success callback gives you the pubkey of the remote, which can then be used
     * to send commands to the remote (via `send()`). is generally intended for cases where the remote is
     * being treated as the "server" and the local connection as a "client"; for connections between
     * peers (i.e. between SNs) you generally want connect_sn() instead.  If pubkey is non-empty
     * then the remote must have that pubkey; if empty then any pubkey is allowed.
     *
     * Unlike `connect_sn`, the connection established here will be kept open
     * indefinitely (until you call disconnect).
     *
     * The `on_connect` and `on_failure` callbacks are invoked when a connection has been
     * established or failed to establish.
     *
     * @param remote the remote connection address, such as `tcp://localhost:1234`.
     * @param on_connect called with the identifier and the remote's pubkey after the connection has
     * been established and handshaked.
     * @param on_failure called with a failure message if we fail to connect.
     * @param pubkey the required remote pubkey (empty to accept any).
     * @param timeout how long to try before aborting the connection attempt and calling the
     * on_failure callback.  Note that the connection can fail for various reasons before the
     * timeout.
     */
    void connect_remote(string_view remote, ConnectSuccess on_connect, ConnectFailure on_failure,
            string_view pubkey = {}, std::chrono::milliseconds timeout = REMOTE_CONNECT_TIMEOUT);

    /**
     * Disconnects an established outgoing connection established with `connect_remote()`.
     *
     * @param id the connection id, as returned by `connect_remote()`.
     *
     * @param linger how long to allow the connection to linger while there are still pending
     * outbound messages to it before disconnecting and dropping any pending messages.  (Note that
     * this lingering is internal; the disconnect_remote() call does not block).  The default is 1
     * second.
     */
    void disconnect_remote(string_view id, std::chrono::milliseconds linger = 1s);

    /**
     * Queue a message to be relayed to the node identified with the given identifier (for SNs and
     * incoming connections this is a pubkey; for connections established with `connect()` this will
     * be the opaque string returned by `connect()`), without expecting a reply.  LokiMQ will
     * attempt to relay the message (first connecting and handshaking if not already connected
     * and the given pubkey is a service node's pubkey).
     *
     * If a new connection is established it will have a relatively short (30s) idle timeout.  If
     * the connection should stay open longer you should call `connect(pubkey, IDLETIME)` first.
     *
     * Note that this method (along with connect) doesn't block waiting for a connection or for the
     * message to send; it merely instructs the proxy thread that it should send.  ZMQ will
     * generally try hard to deliver it (reconnecting if the connection fails), but if the
     * connection fails persistently the message will eventually be dropped.
     *
     * @param id - the pubkey or identifier returned by `connect()` to send this to
     * @param cmd - the first data frame value which is almost always the remote "category.command" name
     * @param opts - any number of std::string and send options.  Each send option affects
     *               how the send works; each string becomes a serialized message part.
     *
     * Example:
     *
     *     lmq.send(pubkey, "hello", "abc", send_option::hint("tcp://localhost:1234"), "def");
     *
     * sends the command `hello` to the given pubkey, containing additional message parts "abc" and
     * "def", and, if not currently connected, using the given connection hint rather than
     * performing a connection address lookup on the pubkey.
     */
    template <typename... T>
    void send(const std::string& pubkey, const std::string& cmd, const T&... opts);

    /** Send a command configured as a "REQUEST" command: the data parts will be prefixed with a
     * random identifier.  The remote is expected to reply with a ["REPLY", <identifier>, ...]
     * message, at which point we invoke the given callback with any [...] parts of the reply.
     *
     * @param pubkey - the pubkey to send this request to
     * @param cmd - the command name
     * @param callback - the callback to invoke when we get a reply.  Called with a true value and
     * the data strings when a reply is received, or false and an empty vector of data parts if we
     * get no reply in the timeout interval.
     * @param opts - anything else (i.e. strings, send_options) is forwarded to send().
     */
    template <typename... T>
    void request(const std::string& pubkey, const std::string& cmd, ReplyCallback callback,
            const T&... opts);

    /// The key pair this LokiMQ was created with; if empty keys were given during construction then
    /// this returns the generated keys.
    const std::string& get_pubkey() const { return pubkey; }
    const std::string& get_privkey() const { return privkey; }

    /**
     * Batches a set of jobs to be executed by workers, optionally followed by a completion function.
     *
     * Must include lokimq/batch.h to use.
     */
    template <typename R>
    void batch(Batch<R>&& batch);

    /**
     * Queues a single job to be executed with no return value.  This is a shortcut for creating and
     * submitting a single-job, no-completion batch.
     */
    void job(std::function<void()> f);

    /**
     * Adds a timer that gets scheduled periodically in the job queue.  Normally jobs are not
     * double-booked: that is, a new timed job will not be scheduled if the timer fires before a
     * previously scheduled callback of the job has not yet completed.  If you want to override this
     * (so that, under heavy load or long jobs, there can be more than one of the same job scheduled
     * or running at a time) then specify `squelch` as `false`.
     */
    void add_timer(std::function<void()> job, std::chrono::milliseconds interval, bool squelch = true);
};

/// Namespace for options to the send() method
namespace send_option {

template <typename InputIt>
struct data_parts_impl {
    InputIt begin, end;
    data_parts_impl(InputIt begin, InputIt end) : begin{std::move(begin)}, end{std::move(end)} {}
};

/// Specifies an iterator pair of data options to send, for when the number of arguments to send()
/// cannot be determined at compile time.
template <typename InputIt>
data_parts_impl<InputIt> data_parts(InputIt begin, InputIt end) { return {std::move(begin), std::move(end)}; }

/// Specifies a connection hint when passed in to send().  If there is no current connection to the
/// peer then the hint is used to save a call to the SNRemoteAddress to get the connection location.
/// (Note that there is no guarantee that the given hint will be used or that a SNRemoteAddress call
/// will not also be done.)
struct hint {
    std::string connect_hint;
    explicit hint(std::string connect_hint) : connect_hint{std::move(connect_hint)} {}
};

/// Does a send() if we already have a connection (incoming or outgoing) with the given peer,
/// otherwise drops the message.
struct optional {};

/// Specifies that the message should be sent only if it can be sent on an existing incoming socket,
/// and dropped otherwise.
struct incoming {};

/// Specifies the idle timeout for the connection - if a new or existing outgoing connection is used
/// for the send and its current idle timeout setting is less than this value then it is updated.
struct keep_alive {
    std::chrono::milliseconds time;
    keep_alive(std::chrono::milliseconds time) : time{std::move(time)} {}
};

}

namespace detail {

// Sends a control message to the given socket consisting of the command plus optional dict
// data (only sent if the data is non-empty).
void send_control(zmq::socket_t& sock, string_view cmd, std::string data = {});

/// Base case: takes a string-like value and appends it to the message parts
inline void apply_send_option(bt_list& parts, bt_dict&, string_view arg) {
    parts.push_back(arg);
}

/// `data_parts` specialization: appends a range of serialized data parts to the parts to send
template <typename InputIt>
void apply_send_option(bt_list& parts, bt_dict&, const send_option::data_parts_impl<InputIt> data) {
    for (auto it = data.begin; it != data.end; ++it)
        parts.push_back(lokimq::bt_deserialize(*it));
}

/// `hint` specialization: sets the hint in the control data
inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::hint& hint) {
    control_data["hint"] = hint.connect_hint;
}

/// `optional` specialization: sets the optional flag in the control data
inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::optional &) {
    control_data["optional"] = 1;
}

/// `incoming` specialization: sets the optional flag in the control data
inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::incoming &) {
    control_data["incoming"] = 1;
}

/// `keep_alive` specialization: increases the outgoing socket idle timeout (if shorter)
inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::keep_alive& timeout) {
    control_data["keep-alive"] = timeout.time.count();
}

} // namespace detail

template <typename... T>
void LokiMQ::send(const std::string& pubkey, const std::string& cmd, const T &...opts) {
    bt_dict control_data;
    bt_list parts{{cmd}};
#ifdef __cpp_fold_expressions
    (detail::apply_send_option(parts, control_data, opts),...);
#else
    (void) std::initializer_list<int>{(detail::apply_send_option(parts, control_data, opts), 0)...};
#endif

    control_data["pubkey"] = pubkey;
    control_data["send"] = std::move(parts);
    detail::send_control(get_control_socket(), "SEND", bt_serialize(control_data));
}

std::string make_random_string(size_t size);

template <typename... T>
void LokiMQ::request(const std::string& pubkey, const std::string& cmd, ReplyCallback callback, const T &...opts) {
    auto reply_tag = make_random_string(15); // 15 should keep us in most stl implementations' small string optimization
    bt_dict control_data;
    bt_list parts{{cmd, reply_tag}};
#ifdef __cpp_fold_expressions
    (detail::apply_send_option(parts, control_data, opts),...);
#else
    (void) std::initializer_list<int>{(detail::apply_send_option(parts, control_data, opts), 0)...};
#endif

    control_data["pubkey"] = pubkey;
    control_data["send"] = std::move(parts);
    control_data["request"] = true;
    control_data["request_callback"] = reinterpret_cast<uintptr_t>(new ReplyCallback{std::move(callback)});
    control_data["request_tag"] = std::move(reply_tag);
    detail::send_control(get_control_socket(), "SEND", bt_serialize(control_data));
}

template <typename... Args>
void Message::send_back(const std::string& command, Args&&... args) {
    assert(reply_tag.empty());
    if (service_node) lokimq.send(pubkey, command, std::forward<Args>(args)...);
    else lokimq.send(pubkey, command, send_option::optional{}, std::forward<Args>(args)...);
}

template <typename... Args>
void Message::send_reply(Args&&... args) {
    assert(!reply_tag.empty());
    if (service_node) lokimq.send(pubkey, "REPLY", reply_tag, std::forward<Args>(args)...);
    else lokimq.send(pubkey, "REPLY", reply_tag, send_option::optional{}, std::forward<Args>(args)...);
}




template <typename... T>
void LokiMQ::log_(LogLevel lvl, const char* file, int line, const T&... stuff) {
    if (log_level() < lvl)
        return;

    std::ostringstream os;
#ifdef __cpp_fold_expressions
    os << ... << stuff;
#else
    (void) std::initializer_list<int>{(os << stuff, 0)...};
#endif
    logger(lvl, file, line, os.str());
}

std::ostream &operator<<(std::ostream &os, LogLevel lvl);

}

// vim:sw=4:et
