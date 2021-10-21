// Copyright (c) 2019-2021, The Oxen Project
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
#include <string_view>
#include <list>
#include <queue>
#include <type_traits>
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
#include <cstdint>
#include "zmq.hpp"
#include "address.h"
#include "bt_serialize.h"
#include "connections.h"
#include "message.h"
#include "auth.h"

#if ZMQ_VERSION < ZMQ_MAKE_VERSION (4, 3, 0)
// Timers were not added until 4.3.0
#error "ZMQ >= 4.3.0 required"
#endif

namespace oxenmq {

using namespace std::literals;

/// Logging levels passed into LogFunc.  (Note that trace does nothing more than debug in a release
/// build).
enum class LogLevel { fatal, error, warn, info, debug, trace };

// Forward declarations; see batch.h
namespace detail { class Batch; }
template <typename R> class Batch;

/** The keep-alive time for a send() that results in a establishing a new outbound connection.  To
 * use a longer keep-alive to a host call `connect()` first with the desired keep-alive time or pass
 * the send_option::keep_alive.
 */
inline constexpr auto DEFAULT_SEND_KEEP_ALIVE = 30s;

/** Default keep-alive time for a connect_sn() (unless overridden via a connect_option::keep_alive
 * argument).
 */
inline constexpr auto DEFAULT_CONNECT_SN_KEEP_ALIVE = 5min;

// The default timeout for connect_remote()
inline constexpr auto REMOTE_CONNECT_TIMEOUT = 10s;

// Default timeout for connect_inproc()
inline constexpr auto INPROC_CONNECT_TIMEOUT = 50ms;

// The amount of time we wait for a reply to a REQUEST before calling the callback with
// `false` to signal a timeout.
inline constexpr auto DEFAULT_REQUEST_TIMEOUT = 15s;

/// Maximum length of a category
inline constexpr size_t MAX_CATEGORY_LENGTH = 50;

/// Maximum length of a command
inline constexpr size_t MAX_COMMAND_LENGTH = 200;

class CatHelper;

/// Opaque handle for a tagged thread constructed by add_tagged_thread(...).  Not directly
/// constructible, but is safe (and cheap) to copy.
struct TaggedThreadID {
private:
    int _id;
    explicit constexpr TaggedThreadID(int id) : _id{id} {}
    friend class OxenMQ;
    template <typename R> friend class Batch;
};

/// Opaque handler for a timer constructed by add_timer(...).  Safe (and cheap) to copy.  The only
/// real use of this is to pass it in to cancel_timer() to cancel a timer.
struct TimerID {
    // Default construction; creates an object with a non-timer internal id value.
    TimerID() : _id{0} {}
private:
    int _id;
    explicit constexpr TimerID(int id) : _id{id} {}
    friend class OxenMQ;
};

/**
 * Class that handles OxenMQ listeners, connections, proxying, and workers.  An application
 * typically has just one instance of this class.
 */
class OxenMQ {

private:

    /// The global context
    zmq::context_t context;

    /// A unique id for this OxenMQ instance, assigned in a thread-safe manner during construction.
    const int object_id;

    /// The x25519 keypair of this connection.  For service nodes these are the long-run x25519 keys
    /// provided at construction, for non-service-node connections these are generated during
    /// construction.
    std::string pubkey, privkey;

    /// True if *this* node is running in service node mode (whether or not actually active)
    bool local_service_node = false;

    /// The thread in which most of the intermediate work happens (handling external connections
    /// and proxying requests between them to worker threads)
    std::thread proxy_thread;

    /// Will be true (and is guarded by a mutex) if the proxy thread is quitting; guards against new
    /// control sockets from threads trying to talk to the proxy thread.
    bool proxy_shutting_down = false;

    /// We have one seldom-used mutex here: it is generally locked just once per thread (the first
    /// time the thread calls get_control_socket()) and once more by the proxy thread when it shuts
    /// down.
    std::mutex control_sockets_mutex;

    /// Called to obtain a "command" socket that attaches to `control` to send commands to the
    /// proxy thread from other threads.  This socket is unique per thread and OxenMQ instance.
    zmq::socket_t& get_control_socket();

    /// Per-thread control sockets used by oxenmq threads to talk to this object's proxy thread.
    std::unordered_map<std::thread::id, std::unique_ptr<zmq::socket_t>> control_sockets;

public:

    /// Callback type invoked to determine whether the given new incoming connection is allowed to
    /// connect to us and to set its authentication level.
    ///
    /// @param address - the address of the incoming connection.  For TCP connections this is an IP
    /// address; for UDP connections it's a string such as "localhost:UID:GID:PID".
    /// @param pubkey - the x25519 pubkey of the connecting client (32 byte string).  Note that this
    /// will only be non-empty for incoming connections on `listen_curve` sockets; `listen_plain`
    /// sockets do not have a pubkey.
    /// @param service_node - will be true if the `pubkey` is in the set of known active service
    /// nodes.
    ///
    /// @returns an `AuthLevel` enum value indicating the default auth level for the incoming
    /// connection, or AuthLevel::denied if the connection should be refused.
    using AllowFunc = std::function<AuthLevel(std::string_view address, std::string_view pubkey, bool service_node)>;

    /// Callback that is invoked when we need to send a "strong" message to a SN that we aren't
    /// already connected to and need to establish a connection.  This callback returns the ZMQ
    /// connection string we should use which is typically a string such as `tcp://1.2.3.4:5678`.
    using SNRemoteAddress = std::function<std::string(std::string_view pubkey)>;

    /// The callback type for registered commands.
    using CommandCallback = std::function<void(Message& message)>;

    /// The callback for making requests.  This is called with `true` and a (moved) vector of data
    /// part strings when we get a reply, or `false` and empty vector on timeout.
    using ReplyCallback = std::function<void(bool success, std::vector<std::string> data)>;

    /// Called to write a log message.  This will only be called if the `level` is >= the current
    /// OxenMQ object log level.  It must be a raw function pointer (or a capture-less lambda) for
    /// performance reasons.  Takes four arguments: the log level of the message, the filename and
    /// line number where the log message was invoked, and the log message itself.
    using Logger = std::function<void(LogLevel level, const char* file, int line, std::string msg)>;

    /// Callback for the success case of connect_remote()
    using ConnectSuccess = std::function<void(ConnectionID)>;
    /// Callback for the failure case of connect_remote()
    using ConnectFailure = std::function<void(ConnectionID, std::string_view)>;

    /// Explicitly non-copyable, non-movable because most things here aren't copyable, and a few
    /// things aren't movable, either.  If you need to pass the OxenMQ instance around, wrap it
    /// in a unique_ptr or shared_ptr.
    OxenMQ(const OxenMQ&) = delete;
    OxenMQ& operator=(const OxenMQ&) = delete;
    OxenMQ(OxenMQ&&) = delete;
    OxenMQ& operator=(OxenMQ&&) = delete;

    /** How long to wait for handshaking to complete on external connections before timing out and
     * closing the connection.  Setting this only affects new outgoing connections. */
    std::chrono::milliseconds HANDSHAKE_TIME = 10s;

    /** Whether to use a random zmq routing ID, or one based on the pubkey for new outgoing
     * connections.  Using the pubkey is desirable when connections between endpoints are unique as
     * it allows the listener to recognize that the incoming connection is a reconnection from the
     * same remote and handover routing to the new socket while closing off the (likely dead) old
     * socket.  This, however, prevents a single OxenMQ instance (or multiple OxenMQ instances using
     * the same keys) from establishing multiple connections to the same listening OxenMQ, which is
     * sometimes useful (for example when testing, or when sharing an authentication key), and so
     * this option can be overridden to `true` to use completely random zmq routing ids on outgoing
     * connections (which will thus allow multiple connections).
     *
     * Note that this only affects the default for outgoing connections: you can override an
     * individual connection by passing a connect_option::ephemeral_routing_id option into the
     * connect_sn/connect_remote method.
     */
    bool EPHEMERAL_ROUTING_ID = false;

    /** Maximum incoming message size; if a remote tries sending a message larger than this they get
     * disconnected. -1 means no limit. */
    int64_t MAX_MSG_SIZE = 1 * 1024 * 1024;

    /** Maximum open sockets, passed to the ZMQ context during start().  The default here is 10k,
     * designed to be enough to be more than enough to allow a full-mesh SN layer connection if
     * necessary for the forseeable future. */
    int MAX_SOCKETS = 10000;

    /** Minimum reconnect interval: when a connection fails or dies, wait this long before
     * attempting to reconnect.  (ZMQ may randomize the value somewhat to avoid reconnection
     * storms).  See RECONNECT_INTERVAL_MAX as well.  The OxenMQ default is 250ms.
     */
    std::chrono::milliseconds RECONNECT_INTERVAL = 250ms;

    /** Maximum reconnect interval.  When this is set to a value larger than RECONNECT_INTERVAL then
     * ZMQ's reconnection logic uses an exponential backoff: each reconnection attempts waits twice
     * as long as the previous attempt, up to this maximum.  The OxenMQ default is 5 seconds.
     */
    std::chrono::milliseconds RECONNECT_INTERVAL_MAX = 5s;

    /** How long (in ms) to linger sockets when closing them; this is the maximum time zmq spends
     * trying to sending pending messages before dropping them and closing the underlying socket
     * after the high-level zmq socket is closed. */
    std::chrono::milliseconds CLOSE_LINGER = 5s;

    /** How frequently we cleanup connections (closing idle connections, calling connect or request
     * failure callbacks).  Making this slower results in more "overshoot" before failure callbacks
     * are invoked; making it too fast results in more proxy thread overhead.  Any change to this
     * variable must be set before calling start().
     */
    std::chrono::milliseconds CONN_CHECK_INTERVAL = 250ms;

    /** Whether to enable heartbeats on incoming/outgoing connections.  If set to > 0 then we set up
     * ZMQ to send a heartbeat ping over the socket this often, which helps keep the connection
     * alive and lets failed connections be detected sooner (see the next option).
     *
     * Only new connections created after changing this are affected, so if changing it is
     * recommended to set it before calling `start()`.
     */
    std::chrono::milliseconds CONN_HEARTBEAT = 15s;

    /** When CONN_HEARTBEAT is enabled, this sets how long we wait for a reply on a socket before
     * considering the socket to have died and closing it.
     *
     * Only new connections created after changing this are affected, so if changing it is
     * recommended to set it before calling `start()`.
     */
    std::chrono::milliseconds CONN_HEARTBEAT_TIMEOUT = 30s;

    /// Allows you to set options on the internal zmq context object.  For advanced use only.
    void set_zmq_context_option(zmq::ctxopt option, int value);

    /** The umask to apply when constructing sockets (which affects any new ipc:// listening sockets
     * that get created).  Does nothing if set to -1 (the default), and does nothing on Windows.
     * Note that the umask is applied temporarily during `start()`, so may affect other threads that
     * create files/directories at the same time as the start() call.
     */
    int STARTUP_UMASK = -1;

    /** The gid that owns any sockets when constructed (same as umask)
     */
    int SOCKET_GID = -1;
    /** The uid that owns any sockets when constructed (same as umask but requires root)
     */
    int SOCKET_UID = -1;

    /// A special TaggedThreadID value that always refers to the proxy thread; the main use of this is
    /// to direct very simple batch completion jobs to be executed directly in the proxy thread.
    inline static constexpr TaggedThreadID run_in_proxy{-1};

    /// Writes a message to the logging system; intended mostly for internal use.
    template <typename... T>
    void log(LogLevel lvl, const char* filename, int line, const T&... stuff);

private:

    /// The lookup function that tells us where to connect to a peer, or empty if not found.
    SNRemoteAddress sn_lookup;

    /// The log level; this is atomic but we use relaxed order to set and access it (so changing it
    /// might not be instantly visible on all threads, but that's okay).
    std::atomic<LogLevel> log_lvl{LogLevel::warn};

    /// The callback to call with log messages
    Logger logger;

    ///////////////////////////////////////////////////////////////////////////////////
    /// NB: The following are all the domain of the proxy thread (once it is started)!

    /// The socket we listen on for handling ZAP authentication requests (the other end is internal
    /// to zmq which sends requests to us as needed).
    zmq::socket_t zap_auth{context, zmq::socket_type::rep};

    struct bind_data {
        std::string address;
        bool curve;
        int64_t conn_id;
        AllowFunc allow;
        std::function<void(bool)> on_bind;
        bind_data(std::string addr, bool curve, AllowFunc allow, std::function<void(bool)> on_bind)
            : address{std::move(addr)}, curve{curve}, conn_id{0}, allow{std::move(allow)}, on_bind{std::move(on_bind)} {}
    };

    /// Addresses on which we are listening (or, before start(), on which we will listen).
    std::vector<bind_data> bind;

    /// Info about a peer's established connection with us.  Note that "established" means both
    /// connected and authenticated.  Note that we only store peer info data for SN connections (in
    /// or out), and outgoing non-SN connections.  Incoming non-SN connections are handled on the
    /// fly.
    struct peer_info {
        /// Pubkey of the remote, if this connection is a curve25519 connection; empty otherwise.
        std::string pubkey;

        /// True if we've authenticated this peer as a service node.  This gets set on incoming
        /// messages when we check the remote's pubkey, and immediately on outgoing connections to
        /// SNs (since we know their pubkey -- we'll fail to connect if it doesn't match).
        bool service_node = false;

        /// The auth level of this peer, as returned by the AllowFunc for incoming connections or
        /// specified during outgoing connections.
        AuthLevel auth_level = AuthLevel::none;

        /// The socket id through which this connection is established
        int64_t conn_id;

        /// Will be set to a non-empty routing prefix *if* one is necessary on the connection.  This
        /// is used only for SN peers (non-SN incoming connections don't have a peer_info record,
        /// and outgoing connections don't have a route).
        std::string route;

        /// Returns true if this is an outgoing connection.  (This is simply an alias for
        /// route.empty() -- outgoing connections never have a route, incoming connections always
        /// do).
        bool outgoing() const { return route.empty(); }

        /// The last time we sent or received a message (or had some other relevant activity) on
        /// this connection.  Used for closing outgoing connections that have reached an inactivity
        /// expiry time (closing inactive conns for incoming connections is done by the other end).
        std::chrono::steady_clock::time_point last_activity;

        /// Updates last_activity to the current time
        void activity() { last_activity = std::chrono::steady_clock::now(); }

        /// After more than this much inactivity we will close an idle (outgoing) connection
        std::chrono::milliseconds idle_expiry;
    };

    /// Currently peer connections: id -> peer_info.  The ID is as returned by connect_remote or a
    /// SN pubkey string.
    std::unordered_multimap<ConnectionID, peer_info> peers;

    /// For outgoing connections to service nodes `peers` contains the service node connection id,
    /// but we sometimes need to be able to get the peer info from a numeric connection id (for
    /// example, for incoming messages on a connection we made); this map lets us do that.
    std::map<int64_t, ConnectionID> outgoing_sn_conns;

    /// The next ConnectionID value we should use (for outgoing, non-SN connections).
    std::atomic<int64_t> next_conn_id{1};

    /// Remotes we are still trying to connect to (via connect_remote(), not connect_sn()); when
    /// we pass handshaking we move them out of here and (if set) trigger the on_connect callback.
    /// Unlike regular node-to-node peers, these have an extra "HI"/"HELLO" sequence that we used
    /// before we consider ourselves connected to the remote.
    std::list<std::tuple<int64_t /*conn_id*/, std::chrono::steady_clock::time_point, ConnectSuccess, ConnectFailure>>
        pending_connects;

    /// Pending requests that have been sent out but not yet received a matching "REPLY".  The value
    /// is the timeout timestamp.
    std::unordered_map<std::string, std::pair<std::chrono::steady_clock::time_point, ReplyCallback>>
        pending_requests;

    /// different polling sockets the proxy handler polls: this always contains some internal
    /// sockets for inter-thread communication followed by a pollitem for every connection (both
    /// incoming and outgoing) in `connections`.  We rebuild this from `connections` whenever
    /// `connections_updated` is set to true.
    std::vector<zmq::pollitem_t> pollitems;

    /// Rebuilds pollitems to include the internal sockets + all incoming/outgoing sockets.
    void rebuild_pollitems();

    /// The connections to/from remotes we currently have open, both listening and outgoing.
    std::map<int64_t, zmq::socket_t> connections;

    /// The connection ID of the built-in inproc listener for making requests to self
    int64_t inproc_listener_connid;

    /// If set then it indicates a change in `connections` which means we need to rebuild pollitems
    /// and stop using existing connections iterators.
    bool connections_updated = true;

    /// Socket we listen on to receive control messages in the proxy thread. Each thread has its own
    /// internal "control" connection (returned by `get_control_socket()`) to this socket used to
    /// give instructions to the proxy such as instructing it to initiate a connection to a remote
    /// or send a message.
    zmq::socket_t command{context, zmq::socket_type::router};

    /// Timers.  TODO: once cppzmq adds an interface around the zmq C timers API then switch to it.
    struct TimersDeleter { void operator()(void* timers); };
    struct timer_data { std::function<void()> function; bool squelch; bool running; int thread; };
    std::unordered_map<int, timer_data> timer_jobs; // keys are zmq timer ids
    std::unique_ptr<void, TimersDeleter> timers;
    // The next internal timer id (returned opaquely via TimerID return from add_timer)
    std::atomic<int> next_timer_id = 1;
    // Maps our internal timer id values (returned by add_timer) to zmq timer ids; used for
    // delete_timer().
    std::unordered_map<int, int> timer_zmq_id;
public:
    // This needs to be public because we have to be able to call it from a plain C function.
    // Nothing external may call it!
    void _queue_timer_job(int);
private:

    /// Router socket to reach internal worker threads from proxy
    zmq::socket_t workers_socket{context, zmq::socket_type::router};

    /// indices of idle, active workers
    std::vector<unsigned int> idle_workers;

    /// Maximum number of general task workers, specified by set_general_threads()
    int general_workers = std::max<int>(1, std::thread::hardware_concurrency());

    /// Maximum number of possible worker threads we can have.  This is calculated when starting,
    /// and equals general_workers plus the sum of all categories' reserved threads counts plus the
    /// reserved batch workers count.  This is also used to signal a shutdown; we set it to 0 when
    /// quitting.
    int max_workers;

    /// Number of active workers
    int active_workers() const { return workers.size() - idle_workers.size(); }

    /// Worker thread loop.  Tagged and start are provided for a tagged worker thread.
    void worker_thread(unsigned int index, std::optional<std::string> tagged = std::nullopt, std::function<void()> start = nullptr);

    /// If set, skip polling for one proxy loop iteration (set when we know we have something
    /// processible without having to shove it onto a socket, such as scheduling an internal job).
    bool proxy_skip_one_poll = false;

    /// Does the proxying work
    void proxy_loop();

    void proxy_conn_cleanup();

    void proxy_worker_message(std::vector<zmq::message_t>& parts);

    void proxy_process_queue();

    void proxy_schedule_reply_job(std::function<void()> f);

    /// Looks up a peers element given a connect id (for outgoing connections where we already
    /// knew the pubkey and SN status) or an incoming zmq message (which has the pubkey and sn
    /// status metadata set during initial connection authentication), creating a new peer element
    /// if required.
    decltype(peers)::iterator proxy_lookup_peer(int64_t conn_id, zmq::message_t& msg);

    /// Handles built-in primitive commands in the proxy thread for things like "BYE" that have to
    /// be done in the proxy thread anyway (if we forwarded to a worker the worker would just have
    /// to send an instruction back to the proxy to do it).  Returns true if one was handled, false
    /// to continue with sending to a worker.
    bool proxy_handle_builtin(int64_t conn_id, zmq::socket_t& sock, std::vector<zmq::message_t>& parts);

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
    void proxy_to_worker(int64_t conn_id, zmq::socket_t& sock, std::vector<zmq::message_t>& parts);

    /// proxy thread command handlers for commands sent from the outer object QUIT.  This doesn't
    /// get called immediately on a QUIT command: the QUIT commands tells workers to quit, then this
    /// gets called after all works have done so.
    void proxy_quit();

    /// proxy handler for binding to addresses given via listen_*().
    bool proxy_bind(bind_data& bind, size_t bind_index);

    // Common setup code for setting up an external (incoming or outgoing) socket.
    void setup_external_socket(zmq::socket_t& socket);

    // Sets the various properties on an outgoing socket prior to connection.  If remote_pubkey is
    // provided then the connection will be curve25519 encrypted and authenticate; otherwise it will
    // be unencrypted and unauthenticated.  Note that the remote end must be in the same mode (i.e.
    // either accepting curve connections, or not accepting curve).
    void setup_outgoing_socket(zmq::socket_t& socket, std::string_view remote_pubkey, bool use_ephemeral_routing_id);

    /// Sets the various properties on an listening socket prior to binding.
    void setup_incoming_socket(zmq::socket_t& socket, bool curve, std::string_view pubkey, std::string_view privkey, size_t bind_index);

    /// Common connection implementation used by proxy_connect/proxy_send.  Returns the socket and,
    /// if a routing prefix is needed, the required prefix (or an empty string if not needed).  For
    /// an optional connect that fails (or some other connection failure), returns nullptr for the
    /// socket.
    ///
    /// @param pubkey the pubkey to connect to
    /// @param connect_hint if we need a new connection and this is non-empty then we *may* use it
    /// instead of doing a call to `sn_lookup()`.
    /// @param optional if we don't already have a connection then don't establish a new one
    /// @param incoming_only only relay this if we have an established incoming connection from the
    /// given SN, otherwise don't connect (like `optional`)
    /// @param keep_alive the keep alive for the connection, if we establish a new outgoing
    /// connection.  If we already have an outgoing connection then its keep-alive gets increased to
    /// this if currently less than this.
    /// @param ephemeral_routing_id whether or not to use a random (true) or pubkey-based (false) routing id
    std::pair<zmq::socket_t*, std::string> proxy_connect_sn(std::string_view pubkey,
            std::string_view connect_hint, bool optional, bool incoming_only, bool outgoing_only,
            bool ephemeral_routing_id, std::chrono::milliseconds keep_alive);

    /// CONNECT_SN command telling us to connect to a new pubkey.  Returns the socket (which could
    /// be existing or a new one).  This basically just unpacks arguments and passes them on to
    /// proxy_connect_sn().
    std::pair<zmq::socket_t*, std::string> proxy_connect_sn(bt_dict_consumer data);

    /// Opens a new connection to a remote, with callbacks.  This is the proxy-side implementation
    /// of the `connect_remote()` call.
    void proxy_connect_remote(bt_dict_consumer data);

    /// Called to disconnect our remote connection to the given id (if we have one).
    void proxy_disconnect(bt_dict_consumer data);
    void proxy_disconnect(ConnectionID conn, std::chrono::milliseconds linger);

    /// SEND command.  Does a connect first, if necessary.
    void proxy_send(bt_dict_consumer data);

    /// REPLY command.  Like SEND, but only has a listening socket route to send back to and so is
    /// weaker (i.e. it cannot reconnect to the SN if the connection is no longer open).
    void proxy_reply(bt_dict_consumer data);

    /// Currently active batch/reply jobs; this is the container that owns the Batch instances
    std::unordered_set<detail::Batch*> batches;
    /// Individual batch jobs waiting to run; .second is the 0-n batch number or -1 for the
    /// completion job
    using batch_job = std::pair<detail::Batch*, int>;
    std::queue<batch_job> batch_jobs, reply_jobs;
    int batch_jobs_active = 0;
    int reply_jobs_active = 0;
    int batch_jobs_reserved = -1;
    int reply_jobs_reserved = -1;
    /// Runs any queued batch jobs
    void proxy_run_batch_jobs(std::queue<batch_job>& jobs, int reserved, int& active, bool reply);

    /// BATCH command.  Called with a Batch<R> (see oxenmq/batch.h) object pointer for the proxy to
    /// take over and queue batch jobs.
    void proxy_batch(detail::Batch* batch);

    /// TIMER command.  Called with a serialized list containing: our local timer_id, function
    /// pointer to assume ownership of, an interval count (in ms), and whether or not jobs should be
    /// squelched (see `add_timer()`).
    void proxy_timer(bt_list_consumer timer_data);

    /// Same, but deserialized
    void proxy_timer(int timer_id, std::function<void()> job, std::chrono::milliseconds interval, bool squelch, int thread);

    /// TIMER_DEL command.  Called with a timer_id to delete an active timer.
    void proxy_timer_del(int timer_id);

    /// ZAP (https://rfc.zeromq.org/spec:27/ZAP/) authentication handler; this does non-blocking
    /// processing of any waiting authentication requests for new incoming connections.
    void process_zap_requests();

    /// Handles a control message from some outer thread to the proxy
    void proxy_control_message(std::vector<zmq::message_t>& parts);

    /// Closing any idle connections that have outlived their idle time.  Note that this only
    /// affects outgoing connections; incomings connections are the responsibility of the other end.
    void proxy_expire_idle_peers();

    /// Helper method to actually close a remote connection and update the stuff that needs updating.
    void proxy_close_connection(int64_t removed, std::chrono::milliseconds linger);

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

    using cat_call_t = std::pair<category*, const std::pair<CommandCallback, bool>*>;
    /// Retrieve category and callback from a command name, including alias mapping.  Warns on
    /// invalid commands and returns nullptrs.  The command name will be updated in place if it is
    /// aliased to another command.
    cat_call_t get_command(std::string& command);

    /// Checks a peer's authentication level.  Returns true if allowed, warns and returns false if
    /// not.
    bool proxy_check_auth(int64_t conn_id, bool outgoing, const peer_info& peer,
            zmq::message_t& command, const cat_call_t& cat_call, std::vector<zmq::message_t>& data);

    struct injected_task {
        category& cat;
        std::string command;
        std::string remote;
        std::function<void()> callback;
    };

    /// Injects a external callback to be handled by a worker; this is the proxy side of
    /// inject_task().
    void proxy_inject_task(injected_task task);


    /// Set of active service nodes.
    pubkey_set active_service_nodes;

    /// Resets or updates the stored set of active SN pubkeys
    void proxy_set_active_sns(std::string_view data);
    void proxy_set_active_sns(pubkey_set pubkeys);
    void proxy_update_active_sns(bt_list_consumer data);
    void proxy_update_active_sns(pubkey_set added, pubkey_set removed);
    void proxy_update_active_sns_clean(pubkey_set added, pubkey_set removed);

    /// Details for a pending command; such a command already has authenticated access and is just
    /// waiting for a thread to become available to handle it.  This also gets used (via the
    /// `callback` variant) for injected external jobs to be able to integrate some external
    /// interface with the oxenmq job queue.
    struct pending_command {
        category& cat;
        std::string command;
        std::vector<zmq::message_t> data_parts;
        std::variant<
            const std::pair<CommandCallback, bool>*, // Normal command callback
            std::function<void()> // Injected external callback
        > callback;
        ConnectionID conn;
        Access access;
        std::string remote;

        // Normal ctor for an actual lmq command being processed
        pending_command(category& cat, std::string command, std::vector<zmq::message_t> data_parts,
                const std::pair<CommandCallback, bool>* callback, ConnectionID conn, Access access, std::string remote)
            : cat{cat}, command{std::move(command)}, data_parts{std::move(data_parts)},
            callback{callback}, conn{std::move(conn)}, access{std::move(access)}, remote{std::move(remote)} {}

        // Ctor for an injected external command.
        pending_command(category& cat, std::string command, std::function<void()> callback, std::string remote)
            : cat{cat}, command{std::move(command)}, callback{std::move(callback)}, remote{std::move(remote)} {}
    };
    std::list<pending_command> pending_commands;


    /// End of proxy-specific members
    ///////////////////////////////////////////////////////////////////////////////////


    /// Structure that contains the data for a worker thread - both the thread itself, plus any
    /// transient data we are passing into the thread.
    struct run_info {
        bool is_batch_job = false;
        bool is_reply_job = false;
        bool is_tagged_thread_job = false;
        bool is_injected = false;

        // resets the job type bools, above.
        void reset() { is_batch_job = is_reply_job = is_tagged_thread_job = is_injected = false; }

        // If is_batch_job is false then these will be set appropriate (if is_batch_job is true then
        // these shouldn't be accessed and likely contain stale data).  Note that if the command is
        // an external, injected command then conn, access, conn_route, and data_parts will be
        // empty/default constructed.
        category *cat;
        std::string command;
        ConnectionID conn; // The connection (or SN pubkey) to reply on/to.
        Access access; // The access level of the invoker (actual level, can be higher than the command's requirement)
        std::string remote; // The remote address from which we received the request.
        std::string conn_route; // if non-empty this is the reply routing prefix (for incoming connections)
        std::vector<zmq::message_t> data_parts;

        // If is_batch_job true then these are set (if is_batch_job false then don't access these!):
        int batch_jobno; // >= 0 for a job, -1 for the completion job

        // The callback or batch job to run.  The first of these is for regular tasks, the second
        // for batch jobs, the third for injected external tasks.
        std::variant<
            const std::pair<CommandCallback, bool>*,
            detail::Batch*,
            std::function<void()>
        > to_run;

        // These belong to the proxy thread and must not be accessed by a worker:
        std::thread worker_thread;
        size_t worker_id; // The index in `workers` (0-n) or index+1 in `tagged_workers` (1-n)
        std::string worker_routing_id; // "w123" where 123 == worker_id; "n123" for tagged threads.

        /// Loads the run info with an incoming command
        run_info& load(category* cat, std::string command, ConnectionID conn, Access access, std::string remote,
                std::vector<zmq::message_t> data_parts, const std::pair<CommandCallback, bool>* callback);

        /// Loads the run info with an injected external command
        run_info& load(category* cat, std::string command, std::string remote, std::function<void()> callback);

        /// Loads the run info with a stored pending command
        run_info& load(pending_command&& pending);

        /// Loads the run info with a batch job
        run_info& load(batch_job&& bj, bool reply_job = false, int tagged_thread = 0);
    };
    /// Data passed to workers for the RUN command.  The proxy thread sets elements in this before
    /// sending RUN to a worker then the worker uses it to get call info, and only allocates it
    /// once, before starting any workers.  Workers may only access their own index and may not
    /// change it.
    std::vector<run_info> workers;

    /// Workers that are reserved for tagged thread tasks (as created with add_tagged_thread).  The
    /// queue here is similar to worker_jobs, but contains only the tagged thread's jobs.  The bool
    /// is whether the worker is currently busy (true) or available (false).
    std::vector<std::tuple<run_info, bool, std::queue<batch_job>>> tagged_workers;

public:
    /**
     * OxenMQ constructor.  This constructs the object but does not start it; you will typically
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
     * @param sn_lookup function that takes a pubkey key (32-byte binary string) and returns a
     * connection string such as "tcp://1.2.3.4:23456" to which a connection should be established
     * to reach that service node.  Note that this function is only called if there is no existing
     * connection to that service node, and that the function is never called for a connection to
     * self (that uses an internal connection instead).  Also note that the service node must be
     * listening in curve25519 mode (otherwise we couldn't verify its authenticity).  Should return
     * empty for not found or if SN lookups are not supported.
     *
     * @param log a function or callable object that writes a log message.  If omitted then all log
     * messages are suppressed.
     *
     * @param level the initial log level; defaults to warn.  The log level can be changed later by
     * calling log_level(...).
     */
    OxenMQ( std::string pubkey,
            std::string privkey,
            bool service_node,
            SNRemoteAddress sn_lookup,
            Logger logger = [](LogLevel, const char*, int, std::string) { },
            LogLevel level = LogLevel::warn);

    /**
     * Simplified OxenMQ constructor for a non-listening client or simple listener without any
     * outgoing SN connection lookup capabilities.  The OxenMQ object will not be able to establish
     * new connections (including reconnections) to service nodes by pubkey.
     */
    explicit OxenMQ(
            Logger logger = [](LogLevel, const char*, int, std::string) { },
            LogLevel level = LogLevel::warn)
        : OxenMQ("", "", false, [](auto) { return ""s; /*no peer lookups*/ }, std::move(logger), level) {}

    /**
     * Destructor; instructs the proxy to quit.  The proxy tells all workers to quit, waits for them
     * to quit and rejoins the threads then quits itself.  The outer thread (where the destructor is
     * running) rejoins the proxy thread.
     */
    ~OxenMQ();

    /// Sets the log level of the OxenMQ object.
    void log_level(LogLevel level);

    /// Gets the log level of the OxenMQ object.
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
     *
     * @returns a CatHelper object that makes adding commands slightly less verbose (see the
     * CatHelper describe, above).
     */
    CatHelper add_category(std::string name, Access access_level, unsigned int reserved_threads = 0, int max_queue = 200);

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
     * currently enforced on the `from` name (for backwards compatility with Oxen's quorumnet code)
     * but will be at some point.
     *
     * Access permissions for an aliased command depend only on the mapped-to value; for example, if
     * `cat.meow` is aliased to `dog.bark` then it is the access permissions on `dog` that apply,
     * not those of `cat`, even if `cat` is more restrictive than `dog`.
     */
    void add_command_alias(std::string from, std::string to);

    /** Creates a "tagged thread" and starts it immediately.  A tagged thread is one that batches,
     * jobs, and timer jobs can be sent to specifically, typically to perform coordination of some
     * thread-unsafe work.
     *
     * Tagged threads will *only* process jobs sent specifically to them; they do not participate in
     * the thread pool used for regular jobs.  Each tagged thread also has its own job queue
     * completely separate from any other jobs.
     *
     * Tagged threads must be created *before* `start()` is called.  The name will be used to set the
     * thread name in the process table (if supported on the OS).
     *
     * \param name - the name of the thread; will be used in log messages and (if supported by the
     * OS) as the system thread name.
     *
     * \param start - an optional callback to invoke from the thread as soon as OxenMQ itself starts
     * up (i.e. after a call to `start()`).
     *
     * \returns a TaggedThreadID object that can be passed to job(), batch(), or add_timer() to
     * direct the task to the tagged thread.
     */
    TaggedThreadID add_tagged_thread(std::string name, std::function<void()> start = nullptr);

    /**
     * Sets the number of worker threads reserved for batch jobs.  If not explicitly called then
     * this defaults to half the general worker threads configured (rounded up).  This works exactly
     * like reserved_threads for a category, but allows to batch jobs.  See category for details.
     *
     * Note that some internal jobs are counted as batch jobs: in particular timers added via
     * add_timer() are scheduled as batch jobs.
     *
     * Cannot be called after start()ing the OxenMQ instance.
     */
    void set_batch_threads(int threads);

    /**
     * Sets the number of worker threads reserved for handling replies from servers; this is
     * mostly for responses to `request()` calls, but also gets used for other network-related
     * events such as the ConnectSuccess/ConnectFailure callbacks for establishing remote non-SN
     * connections.
     *
     * Defaults to one-eighth of the number of configured general threads, rounded up.
     *
     * Cannot be changed after start()ing the OxenMQ instance.
     */
    void set_reply_threads(int threads);

    /**
     * Sets the number of general worker threads.  This is the target number of threads to run that
     * we generally try not to exceed.  These threads can be used for any command, and will be
     * created (up to the limit) on demand.  Note that individual categories (or batch jobs) with
     * reserved threads can create threads in addition to the amount specified here if necessary to
     * fulfill the reserved threads count for the category.
     *
     * Adjusting this also adjusts the default values of batch and reply threads, above.
     *
     * Defaults to `std::thread::hardware_concurrency()`.
     *
     * Cannot be called after start()ing the OxenMQ instance.
     */
    void set_general_threads(int threads);

    /**
     * Finish starting up: binds to the bind locations given in the constructor and launches the
     * proxy thread to handle message dispatching between remote nodes and worker threads.
     *
     * Things you want to do before calling this:
     * - Use `add_category`/`add_command` to set up any commands remote connections can invoke.
     * - If any commands require SN authentication, specify a list of currently active service node
     *   pubkeys via `set_active_sns()` (and make sure this gets updated when things change by
     *   another `set_active_sns()` or a `update_active_sns()` call).  It *is* possible to make the
     *   initial call after calling `start()`, but that creates a window during which incoming
     *   remote SN connections will be erroneously treated as non-SN connections.
     * - If this LMQ instance should accept incoming connections, set up any listening ports via
     *   `listen_curve()` and/or `listen_plain()`.
     */
    void start();

    /** Start listening on the given bind address using curve authentication/encryption.  Incoming
     * connections will only be allowed from clients that already have the server's pubkey, and
     * will be encrypted.  `allow_connection` is invoked for any incoming connections on this
     * address to determine the incoming remote's access and authentication level.
     *
     * If called before `start()` then the given bind address is mandatory and start() will throw if
     * the bind fails.  If called after `start()` then the bind may fail (in which case the callback
     * will be used to notify of the failure).
     *
     * @param bind address - can be any string zmq supports; typically a tcp IP/port combination
     * such as: "tcp://\*:4567" or "tcp://1.2.3.4:5678".
     *
     * @param allow_connection function to call to determine whether to allow the connection and, if
     * so, the authentication level it receives.  If omitted (or null) the default returns
     * AuthLevel::none access for all connections.
     *
     * @param on_bind function to call when the port has been successfully opened or failed to
     * open.  For addresses set up before .start() this will be called during `start()` itself; for
     * post-start listens this will be called from the proxy thread when it opens the new port.
     * Note that this function must is called directly from the proxy thread and so should be fast
     * and non-blocking.
     */
    void listen_curve(std::string bind,
            AllowFunc allow_connection = nullptr,
            std::function<void(bool success)> on_bind = nullptr);

    /** Start listening on the given bind address in unauthenticated plain text mode.  Incoming
     * connections can come from anywhere.  `allow_connection` is invoked for any incoming
     * connections on this address to determine the incoming remote's access and authentication
     * level.  Note that `allow_connection` here will be called with an empty pubkey.
     *
     * @param bind address - can be any string zmq supports, for example a tcp IP/port combination
     * such as: "tcp://\*:4567" or "tcp://1.2.3.4:5678".
     *
     * @param allow_connection function to call to determine whether to allow the connection and, if
     * so, the authentication level it receives.  If omitted (or null) the default returns
     * AuthLevel::none access for all connections.
     *
     * @param on_result called after binding with the result; see `listen_curve` for details.
     */
    void listen_plain(std::string bind,
            AllowFunc allow_connection = nullptr,
            std::function<void(bool success)> on_bind = nullptr);

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
     * @param options - connection options; see the structs in `connect_option`, in particular:
     *        - keep_alive -- how long the SN connection will be kept alive after valid activity
     *        - hint -- a remote address hint that may be used instead of doing a lookup
     *        - ephemeral_routing_id -- allows you to override the EPHEMERAL_ROUTING_ID option for
     *          this connection.
     *
     * For backwards compatibility you may also directly pass (as a `options` value):
     * - a std::chrono::duration duration (equivalent to connect_option::keep_alive{duration})
     * - a string or string_view hint (equivalent to connect_option::hint{hint})
     * but these should be considered deprecated and the connection_option versions preferred.
     *
     * @returns a ConnectionID that identifies an connection with the given SN.  Typically you
     * *don't* need to worry about this (and can just discard it): you can always simply pass the
     * pubkey as a string wherever a ConnectionID is called.
     */
    template <typename... Option>
    ConnectionID connect_sn(std::string_view pubkey, const Option&... opts);

    /**
     * Establish a connection to the given remote with callbacks invoked on a successful or failed
     * connection.  Returns a ConnectionID associated with the connection being attempted.  It is
     * possible to send to the remote before the successful callback is invoked, but there is no
     * guarantee that the messages will be delivered (e.g. if the connection ultimately fails).
     *
     * For connections to a service node you generally want connect_sn() instead (which verifies
     * that it is talking to the SN and encrypts the connection).
     *
     * Unlike `connect_sn`, the connection established here will be kept open indefinitely (until
     * you call disconnect).
     *
     * The `on_connect` and `on_failure` callbacks are invoked when a connection has been
     * established or failed to establish.
     *
     * @param remote the remote connection address either as implicitly from a string or as a full
     * oxenmq::address object; see address.h for details.  This specifies both the connection
     * address and whether curve encryption should be used.
     * @param on_connect called with the identifier after the connection has been established.
     * @param on_failure called with the identifier and failure message if we fail to connect.
     * @param options supports various connection options:
     *        - passing an AuthLevel here sets the auth_level for incoming messages on this
     *          connection (instead of AuthLevel::none).
     *        - anything else should be one of the `oxenmq::connect_option` structs.
     *        - passing a std::chrono::duration type is permitted (but deprecated) for backwards
     *          compatibility; it is equivalent to `connection_option::timeout{duration}`.
     *
     * @returns ConnectionID that uniquely identifies the connection to this remote node.  In order
     * to talk to it you will need the returned value (or a copy of it).
     */
    template <typename... Option>
    ConnectionID connect_remote(const address& remote, ConnectSuccess on_connect, ConnectFailure on_failure,
            const Option&... options);

    /// Deprecated connect_remote variants that take the address as a string view.  The second
    /// version also takes a pubkey (for a secure connection) as a separate argument.  Use of these
    /// is deprecated and discouraged: use an address with connect_option::whatever arguments
    /// instead.
    [[deprecated("use connect_remote() with a oxenmq::address instead")]]
    ConnectionID connect_remote(std::string_view remote, ConnectSuccess on_connect, ConnectFailure on_failure,
            AuthLevel auth_level = AuthLevel::none, std::chrono::milliseconds timeout = REMOTE_CONNECT_TIMEOUT);

    /// Deprecated version of the above that takes the remote address and remote pubkey for curve
    /// encryption as separate arguments.  New code should either use a pubkey-embedded address
    /// string, or specify remote address and pubkey with an `address` object such as:
    ///     connect_remote(address{remote, pubkey}, ...)
    [[deprecated("use connect_remote() with a oxenmq::address instead")]]
    ConnectionID connect_remote(std::string_view remote, ConnectSuccess on_connect, ConnectFailure on_failure,
            std::string_view pubkey,
            AuthLevel auth_level = AuthLevel::none,
            std::chrono::milliseconds timeout = REMOTE_CONNECT_TIMEOUT);

    /// Connects to the built-in in-process listening socket of this OxenMQ server for local
    /// communication.  Note that auth_level defaults to admin (unlike connect_remote), and the
    /// default timeout is much shorter.
    ///
    /// Also note that incoming inproc requests are unauthenticated: that is, they will always have
    /// admin-level access.
    template <typename... Option>
    ConnectionID connect_inproc(ConnectSuccess on_connect, ConnectFailure on_failure,
            const Option&... options);

    /**
     * Disconnects an established outgoing connection established with `connect_remote()` (or, less
     * commonly, `connect_sn()`).
     *
     * @param id the connection id, as returned by `connect_remote()` or the SN pubkey.
     *
     * @param linger how long to allow the connection to linger while there are still pending
     * outbound messages to it before disconnecting and dropping any pending messages.  (Note that
     * this lingering is internal; the disconnect() call does not block).  The default is 1 second.
     *
     * If given a pubkey, we try to close an outgoing connection to the given SN if one exists; note
     * however that this is often not particularly useful as messages to that SN can immediately
     * reopen the connection.
     */
    void disconnect(ConnectionID id, std::chrono::milliseconds linger = 1s);

    /**
     * Queue a message to be relayed to the given service node or remote without requiring a reply.
     * OxenMQ will attempt to relay the message (first connecting and handshaking to the remote SN
     * if not already connected).
     *
     * If a new connection is established it will have a relatively short (30s) idle timeout.  If
     * the connection should stay open longer you should either call `connect_sn(pubkey, IDLETIME)`
     * or pass a a `send_option::keep_alive{IDLETIME}` in `opts`.
     *
     * Note that this method (along with connect) doesn't block waiting for a connection or for the
     * message to send; it merely instructs the proxy thread that it should send.  ZMQ will
     * generally try hard to deliver it (reconnecting if the connection fails), but if the
     * connection fails persistently the message will eventually be dropped.
     *
     * @param remote - either a ConnectionID value returned by connect_remote, or a service node
     *                 pubkey string.  In the latter case, sending the message may trigger a new
     *                 connection being established to the service node (i.e. you do not have to
     *                 call connect() first).
     * @param cmd - the first data frame value which is almost always the remote "category.command" name
     * @param opts - any number of std::string (or string_views) and send options.  Each send option
     *               affects how the send works; each string becomes a message part.  May also
     *               contain std::optional<T> values: the value will be applied as a string or send
     *               option if set and skipped if null.
     *
     * Example:
     *
     *     // Send to a SN, connecting to it if we aren't already connected:
     *     lmq.send(pubkey, "hello.world", "abc", send_option::hint("tcp://localhost:1234"), "def");
     *
     *     // Start connecting to a remote and immediately queue a message for it
     *     auto conn = lmq.connect_remote("tcp://127.0.0.1:1234",
     *         [](ConnectionID) { std::cout << "connected\n"; },
     *         [](ConnectionID, string_view why) { std::cout << "connection failed: " << why << \n"; });
     *     lmq.send(conn, "hello.world", "abc", "def");
     *
     * Both of these send the command `hello.world` to the given pubkey, containing additional
     * message parts "abc" and "def".  In the first case, if not currently connected, the given
     * connection hint may be used rather than performing a connection address lookup on the pubkey.
     */
    template <typename... T>
    void send(ConnectionID to, std::string_view cmd, const T&... opts);

    /** Send a command configured as a "REQUEST" command to a service node: the data parts will be
     * prefixed with a random identifier.  The remote is expected to reply with a ["REPLY",
     * <identifier>, ...] message, at which point we invoke the given callback with any [...] parts
     * of the reply.
     *
     * Like `send()`, a new connection to the service node will be established if not already
     * connected.
     *
     * @param to - the pubkey string or ConnectionID to send this request to
     * @param cmd - the command name
     * @param callback - the callback to invoke when we get a reply.  Called with a true value and
     * the data strings when a reply is received, or false with error string(s) indicating the
     * failure reason upon failure or timeout.
     * @param opts - anything else (i.e. strings, send_options) is forwarded to send().
     *
     * Possible error data values:
     * - ["TIMEOUT"] - we got no reply within the timeout window
     * - ["UNKNOWNCOMMAND"] - the remote did not recognize the given request command
     * - ["NO_REPLY_TAG"] - the invoked command is a request command but no reply tag was included
     * - ["FORBIDDEN"] - the command requires an authorization level (e.g. Basic or Admin) that we
     *   do not have.
     * - ["FORBIDDEN_SN"] - the command requires service node authentication, but the remote did not
     *   recognize us as a service node.  You *may* want to retry the request a limited number of
     *   times (but do not retry indefinitely as that can be an infinite loop!) because this is
     *   typically also followed by a disconnection; a retried message would reconnect and
     *   reauthenticate which *may* result in picking up the SN authentication.
     * - ["NOT_A_SERVICE_NODE"] - this command is only invokable on service nodes, and the remote is
     *   not running as a service node.
     */
    template <typename... T>
    void request(ConnectionID to, std::string_view cmd, ReplyCallback callback, const T&... opts);

    /** Injects an external task into the oxenmq command queue.  This is used to allow connecting
     * non-OxenMQ requests into the OxenMQ thread pool as if they were ordinary requests, to be
     * scheduled as commands of an individual category.  For example, you might support rpc requests
     * via OxenMQ as `rpc.some_command` and *also* accept them over HTTP.  Using `inject_task()`
     * allows you to handle processing the request in the same thread pool with the same priority as
     * `rpc.*` commands.
     *
     * @param category - the category name that should handle the request for the purposes of
     * scheduling the job.  The category must have been added using add_category().  The category
     * can be an actual category with added commands, in which case the injected tasks are queued
     * along with LMQ requests for that category, or can have no commands to set up a distinct
     * category for the injected jobs.
     *
     * @param command - a command name; this is mainly used for debugging and does not need to
     * actually exist (and, in fact, is often less confusing if it does not).  It is recommended for
     * clarity purposes to use something that doesn't look like a typical command, for example
     * "(http)".
     *
     * @param remote - some free-form identifier of the remote connection.  For example, this could
     * be a remote IP address.  Can be blank if there is nothing suitable.
     *
     * @param callback - the function to call from a worker thread when the injected task is
     * processed.  Takes no arguments.
     */
    void inject_task(const std::string& category, std::string command, std::string remote, std::function<void()> callback);

    /// The key pair this OxenMQ was created with; if empty keys were given during construction then
    /// this returns the generated keys.
    const std::string& get_pubkey() const { return pubkey; }
    const std::string& get_privkey() const { return privkey; }

    /** Updates (or initially sets) OxenMQ's list of service node pubkeys with the given list.
     *
     * This has two main effects:
     *
     * - All commands processed after the update will have SN status determined by the new list.
     * - All outgoing connections to service nodes that are no longer on the list will be closed.
     *   This includes both explicit connections (established by `connect_sn()`) and implicit ones
     *   (established by sending to a SN that wasn't connected).
     *
     * As this update is potentially quite heavy it is recommended that this be called only when
     * necessary--i.e. when the list has changed (or potentially changed), but *not* on a short
     * periodic timer.
     *
     * This method may (and should!) be called before start() to load an initial set of SNs.
     *
     * Once a full list has been set, updates on changes can either call this again with the new
     * list, or use the more efficient update_active_sns() call if incremental results are
     * available.
     */
    void set_active_sns(pubkey_set pubkeys);

    /** Updates the list of active pubkeys by adding or removing the given pubkeys from the existing
     * list.  This is more efficient when the incremental information is already available; if it
     * isn't, simply call set_active_sns with a new list to have OxenMQ figure out what was added or
     * removed.
     *
     * \param added new pubkeys that were added since the last set_active_sns or update_active_sns
     * call.
     *
     * \param removed pubkeys that were removed from active SN status since the last call.  If a
     * pubkey is in both `added` and `removed` for some reason then its presence in `removed` will
     * be ignored.
     */
    void update_active_sns(pubkey_set added, pubkey_set removed);

    /**
     * Batches a set of jobs to be executed by workers, optionally followed by a completion function.
     *
     * Must include oxenmq/batch.h to use.
     */
    template <typename R>
    void batch(Batch<R>&& batch);

    /**
     * Queues a single job to be executed with no return value.  This is a shortcut for creating and
     * submitting a single-job, no-completion-function batch job.
     *
     * \param f the callback to invoke
     * \param thread an optional tagged thread in which this job should run.  You may *not* pass the
     * proxy thread here.
     */
    void job(std::function<void()> f, std::optional<TaggedThreadID> = std::nullopt);

    /**
     * Adds a timer that gets scheduled periodically in the job queue.  Normally jobs are not
     * double-booked: that is, a new timed job will not be scheduled if the timer fires before a
     * previously scheduled callback of the job has completed.  If you want to override this (so
     * that, under heavy load or long jobs, there can be more than one of the same job scheduled or
     * running at a time) then specify `squelch` as `false`.
     *
     * The returned value can be kept and later passed into `cancel_timer()` if you want to be able
     * to cancel a timer.
     *
     * \param thread specifies a thread (added with add_tagged_thread()) on which this timer must run.
     */
    TimerID add_timer(std::function<void()> job, std::chrono::milliseconds interval, bool squelch = true, std::optional<TaggedThreadID> = std::nullopt);

    /** Same as add_timer, above, except that it sets `timer` directly before adding the timer
     * rather than returning it.
     *
     * This is recommended over the above in cases where the timer is extremely fast *and*
     * cancellation will occur inside the job itself.  This version of the method guarantees that
     * `timer` will be assigned to before the job is added to the job schedule so as to guarantee
     * that `job` can safely use `timer` without needing to synchronize the assignment with the
     * thread creating the timer.
     *
     * If in doubt and you need to cancel a job from within the job itself, use this method.
     *
     * Example usage:
     *
     *     auto timer = std::make_shared<TimerID>();
     *     auto& timer_ref = *timer; // Get reference before we move away the shared_ptr
     *     omq.add_timer(timer_ref, [timer=std::move(timer)] { ...; cancel_timer(*timer); });
     */
    void add_timer(TimerID& timer, std::function<void()> job, std::chrono::milliseconds interval, bool squelch = true, std::optional<TaggedThreadID> = std::nullopt);

    /**
     * Cancels a running timer.  Note that an existing timer job (or multiple, if the timer disabled
     * squelch) that have already been scheduled may still be executed after cancel_timer is called.
     *
     * It is safe (though does nothing) to call this more than once with the same TimerID value.
     *
     * \param timer a TimerID value as returned by add_timer.
     */
    void cancel_timer(TimerID timer);
};

/// Helper class that slightly simplifies adding commands to a category.
///
/// This allows simplifying:
///
/// lmq.add_category("foo", ...);
/// lmq.add_command("foo", "a", ...);
/// lmq.add_command("foo", "b", ...);
/// lmq.add_request_command("foo", "c", ...);
///
/// to:
///
/// lmq.add_category("foo", ...)
///     .add_command("a", ...)
///     .add_command("b", ...)
///     .add_request_command("b", ...)
///     ;
class CatHelper {
    OxenMQ& lmq;
    std::string cat;

public:
    CatHelper(OxenMQ& lmq, std::string cat) : lmq{lmq}, cat{std::move(cat)} {}

    CatHelper& add_command(std::string name, OxenMQ::CommandCallback callback) {
        lmq.add_command(cat, std::move(name), std::move(callback));
        return *this;
    }

    CatHelper& add_request_command(std::string name, OxenMQ::CommandCallback callback) {
        lmq.add_request_command(cat, std::move(name), std::move(callback));
        return *this;
    }
};


/// Namespace for options to the send() method
namespace send_option {

template <typename InputIt>
struct data_parts_impl {
    InputIt begin, end;
    data_parts_impl(InputIt begin, InputIt end) : begin{std::move(begin)}, end{std::move(end)} {}
};

/// Specifies an iterator pair of data parts to send, for when the number of arguments to send()
/// cannot be determined at compile time.  The iterator pair must be over strings or string_view (or
/// something convertible to a string_view).
template <typename InputIt, typename = std::enable_if_t<std::is_convertible_v<decltype(*std::declval<InputIt>()), std::string_view>>>
data_parts_impl<InputIt> data_parts(InputIt begin, InputIt end) { return {std::move(begin), std::move(end)}; }

/// Shortcut for send_option::data_parts(container.begin(), container.end())
template <typename Container>
auto data_parts(const Container& c) { return data_parts(c.begin(), c.end()); }

/// Specifies a connection hint when passed in to send().  If there is no current connection to the
/// peer then the hint is used to save a call to the SNRemoteAddress to get the connection location.
/// (Note that there is no guarantee that the given hint will be used or that a SNRemoteAddress call
/// will not also be done.)
struct hint {
    std::string connect_hint;
    // Constructor taking a hint.  If the hint is an empty string then no hint will be used.
    explicit hint(std::string connect_hint) : connect_hint{std::move(connect_hint)} {}
};

/// Does a send() if we already have a connection (incoming or outgoing) with the given peer,
/// otherwise drops the message.
struct optional {
    bool is_optional = true;
    // Constructor; default construction gives you an optional, but the bool parameter can be
    // specified as false to explicitly make a connection non-optional instead.
    explicit optional(bool opt = true) : is_optional{opt} {}
};

/// Specifies that the message should be sent only if it can be sent on an existing incoming socket,
/// and dropped otherwise.
struct incoming {
    bool is_incoming = true;
    // Constructor; default construction gives you an incoming-only, but the bool parameter can be
    // specified as false to explicitly disable incoming-only behaviour.
    explicit incoming(bool inc = true) : is_incoming{inc} {}
};

/// Specifies that the message must use an outgoing connection; for messages to a service node the
/// message will be delivered over an existing outgoing connection, if one is established, and a new
/// outgoing connection opened to deliver the message if none is currently established.  For non-SN
/// messages, the message will simply be dropped if it is attempting to be sent on an incoming
/// socket, and send otherwise on an outgoing socket (this option is primarily aimed at SN
/// messages).
struct outgoing {
    bool is_outgoing = true;
    // Constructor; default construction gives you an outgoing-only, but the bool parameter can be
    // specified as false to explicitly disable the outgoing-only flag.
    explicit outgoing(bool out = true) : is_outgoing{out} {}
};

/// Specifies the idle timeout for the connection - if a new or existing outgoing connection is used
/// for the send and its current idle timeout setting is less than this value then it is updated.
///
/// A negative value is treated as if the option were not supplied at all.
struct keep_alive {
    std::chrono::milliseconds time;
    explicit keep_alive(std::chrono::milliseconds time) : time{std::move(time)} {}
};

/// Specifies the amount of time to wait before triggering a failure callback for a request.  If a
/// request reply arrives *after* the failure timeout has been triggered then it will be dropped.
/// (This has no effect if specified on a non-request() call).  Note that requests failures are only
/// processed in the CONN_CHECK_INTERVAL timer, so it can be up to that much longer than the time
/// specified here before a failure callback is invoked.
///
/// Specifying a negative timeout is equivalent to not specifying the option at all.
struct request_timeout {
    std::chrono::milliseconds time;
    explicit request_timeout(std::chrono::milliseconds time) : time{std::move(time)} {}
};

/// Specifies a callback to invoke if the message couldn't be queued for delivery.  There are
/// generally two failure modes here: a full queue, and a send exception.  This callback is invoked
/// for both; to only catch full queues see `queue_full` instead.
///
/// A full queue means there are too many messages queued for delivery already that haven't been
/// delivered yet (i.e. because the remote is slow); this error is potentially recoverable if the
/// remote end wakes up and receives/acknoledges its messages.
///
/// A send exception is not recoverable: it indicates some failure such as the remote having
/// disconnected or an internal send error.
///
/// This callback can be used by a caller to log, attempt to resend, or take other appropriate
/// action.
///
/// Note that this callback is *not* exhaustive for all possible send failures: there are failure
/// cases (such as when a message is queued but the connection fails before delivery) that do not
/// trigger this failure at all; rather this callback only signals an immediate queuing failure.
struct queue_failure {
    using callback_t = std::function<void(const zmq::error_t* exc)>;
    /// Callback; invoked with nullptr for a queue full failure, otherwise will be set to a copy of
    /// the raised exception.
    callback_t callback;
};

/// This is similar to queue_failure_callback, but is only invoked on a (potentially recoverable)
/// full queue failure.  Send failures are simply dropped.
struct queue_full {
    using callback_t = std::function<void()>;
    callback_t callback;
};

}

/// Namespace for options to the connect_remote()/connect_sn() methods
namespace connect_option {

/// Specifies whether the connection should use pubkey-based routing for this connection, overriding
/// the default (OxenMQ::EPHEMERAL_ROUTING_ID).  See OxenMQ::EPHEMERAL_ROUTING_ID for a description
/// of this.
///
/// Typically use: `connect_option::ephemeral_routing_id{}` or `connect_option::ephemeral_routing_id{false}`.
struct ephemeral_routing_id {
    bool use_ephemeral_routing_id = true;
    // Constructor; default construction gives you ephemeral routing id, but the bool parameter can
    // be specified as false to use pubkey routing flag.
    explicit ephemeral_routing_id(bool use = true) : use_ephemeral_routing_id{use} {}
};

/// Sets the connection timeout (instead of the default REMOTE_CONNECT_TIMEOUT).  Only applies to
/// connect_remote(), not connect_sn().
struct timeout {
    std::chrono::milliseconds time;
    explicit timeout(std::chrono::milliseconds time) : time{std::move(time)} {}
};

/// Sets the connection keep-alive (only applies to connect_sn(), not connect_remote()).  The
/// connection will be kept alive if there was valid activity within the past `keep_alive`
/// milliseconds.  If an outgoing connection already exists, the longer of the existing and the
/// given keep alive is used.
///
/// A negative value is treated as if the keep_alive option had not been specified.
///
/// Note that, if not specified, the default keep-alive for a connection established via
/// connect_sn() is 5 minutes (which is much longer than the default for an implicit connect() by
/// calling send() directly with a pubkey.)
struct keep_alive {
    std::chrono::milliseconds time;
    explicit keep_alive(std::chrono::milliseconds time) : time{std::move(time)} {}
};

/// Sets a remote address hint for an outgoing SN connection (only applies to connect_sn()).  If a
/// new outgoing connection needs to be made this hint value may be used instead of calling the
/// lookup function.  (Note that there is no guarantee that the hint will be used; it is only
/// usefully specified if the connection address has already been incidentally determined to save a
/// potentially expensive lookup call).
struct hint {
    std::string address;
    // Constructor taking a hint.  If the hint is an empty string then no hint will be used.
    explicit hint(std::string_view address) : address{address} {}
};

}

namespace detail {

/// Takes an rvalue reference, moves it into a new instance then returns a uintptr_t value
/// containing the pointer to be serialized to pass (via oxenmq queues) from one thread to another.
/// Must be matched with a deserializer_pointer on the other side to reconstitute the object and
/// destroy the intermediate pointer.
template <typename T>
uintptr_t serialize_object(T&& obj) {
    static_assert(std::is_rvalue_reference<decltype(obj)>::value, "serialize_object must be given an rvalue reference");
    auto* ptr = new T{std::forward<T>(obj)};
    return reinterpret_cast<uintptr_t>(ptr);
}

/// Takes a uintptr_t as produced by serialize_pointer and the type, converts the serialized value
/// back into a pointer, moves it into a new instance (to be returned) and destroys the
/// intermediate.
template <typename T> T deserialize_object(uintptr_t ptrval) {
    auto* ptr = reinterpret_cast<T*>(ptrval);
    T ret{std::move(*ptr)};
    delete ptr;
    return ret;
}

// Sends a control message to the given socket consisting of the command plus optional dict
// data (only sent if the data is non-empty).
void send_control(zmq::socket_t& sock, std::string_view cmd, std::string data = {});

/// Base case: takes a string-like value and appends it to the message parts
inline void apply_send_option(bt_list& parts, bt_dict&, std::string_view arg) {
    parts.emplace_back(arg);
}

/// std::optional<T>: if the optional is set, we unwrap it and apply as a send_option, otherwise we
/// ignore it.
template <typename T>
inline void apply_send_option(bt_list& parts, bt_dict& control_data, const std::optional<T>& opt) {
    if (opt) apply_send_option(parts, control_data, *opt);
}

/// `data_parts` specialization: appends a range of serialized data parts to the parts to send
template <typename InputIt>
void apply_send_option(bt_list& parts, bt_dict&, const send_option::data_parts_impl<InputIt> data) {
    for (auto it = data.begin; it != data.end; ++it)
        parts.emplace_back(*it);
}

/// `hint` specialization: sets the hint in the control data
inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::hint& hint) {
    if (hint.connect_hint.empty()) return;
    control_data["hint"] = hint.connect_hint;
}

/// `optional` specialization: sets the optional flag in the control data
inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::optional& o) {
    control_data["optional"] = o.is_optional;
}

/// `incoming` specialization: sets the incoming-only flag in the control data
inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::incoming& i) {
    control_data["incoming"] = i.is_incoming;
}

/// `outgoing` specialization: sets the outgoing-only flag in the control data
inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::outgoing& o) {
    control_data["outgoing"] = o.is_outgoing;
}

/// `keep_alive` specialization: increases the outgoing socket idle timeout (if shorter)
inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::keep_alive& timeout) {
    if (timeout.time >= 0ms)
        control_data["keep_alive"] = timeout.time.count();
}

/// `request_timeout` specialization: set the timeout time for a request
inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::request_timeout& timeout) {
    if (timeout.time >= 0ms)
        control_data["request_timeout"] = timeout.time.count();
}

/// `queue_failure` specialization
inline void apply_send_option(bt_list&, bt_dict& control_data, send_option::queue_failure f) {
    control_data["send_fail"] = serialize_object(std::move(f.callback));
}

/// `queue_full` specialization
inline void apply_send_option(bt_list&, bt_dict& control_data, send_option::queue_full f) {
    control_data["send_full_q"] = serialize_object(std::move(f.callback));
}

/// Extracts a pubkey and auth level from a zmq message received on a *listening* socket.
std::pair<std::string, AuthLevel> extract_metadata(zmq::message_t& msg);

template <typename... T>
bt_dict build_send(ConnectionID to, std::string_view cmd, T&&... opts) {
    bt_dict control_data;
    bt_list parts{{cmd}};
    (detail::apply_send_option(parts, control_data, std::forward<T>(opts)),...);

    if (to.sn())
        control_data["conn_pubkey"] = std::move(to.pk);
    else {
        control_data["conn_id"] = to.id;
        control_data["conn_route"] = std::move(to.route);
    }
    control_data["send"] = std::move(parts);
    return control_data;

}

inline void apply_connect_option(OxenMQ& omq, bool remote, bt_dict& opts, const AuthLevel& auth) {
    if (remote) opts["auth_level"] = static_cast<std::underlying_type_t<AuthLevel>>(auth);
    else omq.log(LogLevel::warn, __FILE__, __LINE__, "AuthLevel ignored for connect_sn(...)");
}
inline void apply_connect_option(OxenMQ&, bool, bt_dict& opts, const connect_option::ephemeral_routing_id& er) {
    opts["ephemeral_rid"] = er.use_ephemeral_routing_id;
}
inline void apply_connect_option(OxenMQ& omq, bool remote, bt_dict& opts, const connect_option::timeout& timeout) {
    if (remote) opts["timeout"] = timeout.time.count();
    else omq.log(LogLevel::warn, __FILE__, __LINE__, "connect_option::timeout ignored for connect_sn(...)");
}
inline void apply_connect_option(OxenMQ& omq, bool remote, bt_dict& opts, const connect_option::keep_alive& ka) {
    if (ka.time < 0ms) return;
    else if (!remote) opts["keep_alive"] = ka.time.count();
    else omq.log(LogLevel::warn, __FILE__, __LINE__, "connect_option::keep_alive ignored for connect_remote(...)");
}
inline void apply_connect_option(OxenMQ& omq, bool remote, bt_dict& opts, const connect_option::hint& hint) {
    if (hint.address.empty()) return;
    if (!remote) opts["hint"] = hint.address;
    else omq.log(LogLevel::warn, __FILE__, __LINE__, "connect_option::hint ignored for connect_remote(...)");
}
[[deprecated("use oxenmq::connect_option::keep_alive or ::timeout instead")]]
inline void apply_connect_option(OxenMQ&, bool remote, bt_dict& opts, std::chrono::milliseconds time) {
    if (remote) opts["timeout"] = time.count();
    else opts["keep_alive"] = time.count();
}
[[deprecated("use oxenmq::connect_option::hint{hint} instead of a direct string argument")]]
inline void apply_connect_option(OxenMQ& omq, bool remote, bt_dict& opts, std::string_view hint) {
    if (!remote) opts["hint"] = hint;
    else omq.log(LogLevel::warn, __FILE__, __LINE__, "string argument ignored for connect_remote(...)");
}

} // namespace detail

template <typename... Option>
ConnectionID OxenMQ::connect_remote(const address& remote, ConnectSuccess on_connect, ConnectFailure on_failure,
            const Option&... options) {
    bt_dict opts;
    (detail::apply_connect_option(*this, true, opts, options), ...);

    auto id = next_conn_id++;
    opts["conn_id"] = id;
    opts["connect"] = detail::serialize_object(std::move(on_connect));
    opts["failure"] = detail::serialize_object(std::move(on_failure));
    if (remote.curve()) opts["pubkey"] = remote.pubkey;
    opts["remote"] = remote.zmq_address();

    detail::send_control(get_control_socket(), "CONNECT_REMOTE", bt_serialize(opts));

    return id;
}

template <typename... Option>
ConnectionID OxenMQ::connect_sn(std::string_view pubkey, const Option&... options) {
    bt_dict opts{
        {"keep_alive", std::chrono::microseconds{DEFAULT_CONNECT_SN_KEEP_ALIVE}.count()},
        {"ephemeral_rid", EPHEMERAL_ROUTING_ID},
    };

    (detail::apply_connect_option(*this, false, opts, options), ...);

    opts["pubkey"] = pubkey;

    detail::send_control(get_control_socket(), "CONNECT_SN", bt_serialize(opts));

    return pubkey;
}

template <typename... Option>
ConnectionID OxenMQ::connect_inproc(ConnectSuccess on_connect, ConnectFailure on_failure,
            const Option&... options) {
    bt_dict opts{
        {"timeout", INPROC_CONNECT_TIMEOUT.count()},
        {"auth_level", static_cast<std::underlying_type_t<AuthLevel>>(AuthLevel::admin)}
    };

    (detail::apply_connect_option(*this, true, opts, options), ...);

    auto id = next_conn_id++;
    opts["conn_id"] = id;
    opts["connect"] = detail::serialize_object(std::move(on_connect));
    opts["failure"] = detail::serialize_object(std::move(on_failure));
    opts["remote"] = "inproc://sn-self";

    detail::send_control(get_control_socket(), "CONNECT_REMOTE", bt_serialize(opts));

    return id;
}

template <typename... T>
void OxenMQ::send(ConnectionID to, std::string_view cmd, const T&... opts) {
    detail::send_control(get_control_socket(), "SEND",
            bt_serialize(detail::build_send(std::move(to), cmd, opts...)));
}

std::string make_random_string(size_t size);

template <typename... T>
void OxenMQ::request(ConnectionID to, std::string_view cmd, ReplyCallback callback, const T &...opts) {
    const auto reply_tag = make_random_string(15); // 15 random bytes is lots and should keep us in most stl implementations' small string optimization
    bt_dict control_data = detail::build_send(std::move(to), cmd, reply_tag, opts...);
    control_data["request"] = true;
    control_data["request_callback"] = detail::serialize_object(std::move(callback));
    control_data["request_tag"] = std::string_view{reply_tag};
    detail::send_control(get_control_socket(), "SEND", bt_serialize(std::move(control_data)));
}

template <typename... Args>
void Message::send_back(std::string_view command, Args&&... args) {
    oxenmq.send(conn, command, send_option::optional{!conn.sn()}, std::forward<Args>(args)...);
}
template <typename... Args>
void Message::DeferredSend::back(std::string_view command, Args&&... args) const {
    oxenmq.send(conn, command, send_option::optional{!conn.sn()}, std::forward<Args>(args)...);
}

template <typename... Args>
void Message::send_reply(Args&&... args) {
    assert(!reply_tag.empty());
    oxenmq.send(conn, "REPLY", reply_tag, send_option::optional{!conn.sn()}, std::forward<Args>(args)...);
}
template <typename... Args>
void Message::DeferredSend::reply(Args&&... args) const {
    assert(!reply_tag.empty());
    oxenmq.send(conn, "REPLY", reply_tag, send_option::optional{!conn.sn()}, std::forward<Args>(args)...);
}

template <typename Callback, typename... Args>
void Message::send_request(std::string_view cmd, Callback&& callback, Args&&... args) {
    oxenmq.request(conn, cmd, std::forward<Callback>(callback),
            send_option::optional{!conn.sn()}, std::forward<Args>(args)...);
}
template <typename Callback, typename... Args>
void Message::DeferredSend::request(std::string_view cmd, Callback&& callback, Args&&... args) const {
    oxenmq.request(conn, cmd, std::forward<Callback>(callback),
            send_option::optional{!conn.sn()}, std::forward<Args>(args)...);
}

// When log messages are invoked we strip out anything before this in the filename:
constexpr std::string_view LOG_PREFIX{"oxenmq/", 7};
inline std::string_view trim_log_filename(std::string_view local_file) {
    auto chop = local_file.rfind(LOG_PREFIX);
    if (chop != local_file.npos)
        local_file.remove_prefix(chop);
    return local_file;
}

template <typename... T>
void OxenMQ::log(LogLevel lvl, const char* file, int line, const T&... stuff) {
    if (log_level() < lvl)
        return;

    std::ostringstream os;
    (os << ... << stuff);
    logger(lvl, trim_log_filename(file).data(), line, os.str());
}

std::ostream &operator<<(std::ostream &os, LogLevel lvl);

} // namespace oxenmq

// vim:sw=4:et
