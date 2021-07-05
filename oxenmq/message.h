#pragma once
#include <vector>
#include "connections.h"

namespace oxenmq {

class OxenMQ;

/// Encapsulates an incoming message from a remote connection with message details plus extra
/// info need to send a reply back through the proxy thread via the `reply()` method.  Note that
/// this object gets reused: callbacks should use but not store any reference beyond the callback.
class Message {
public:
    OxenMQ& oxenmq; ///< The owning OxenMQ object
    std::vector<std::string_view> data; ///< The provided command data parts, if any.
    ConnectionID conn; ///< The connection info for routing a reply; also contains the pubkey/sn status.
    std::string reply_tag; ///< If the invoked command is a request command this is the required reply tag that will be prepended by `send_reply()`.
    Access access; ///< The access level of the invoker.  This can be higher than the access level of the command, for example for an admin invoking a basic command.
    std::string remote; ///< Some sort of remote address from which the request came.  Often "IP" for TCP connections and "localhost:UID:GID:PID" for unix socket connections.

    /// Constructor
    Message(OxenMQ& lmq, ConnectionID cid, Access access, std::string remote)
        : oxenmq{lmq}, conn{std::move(cid)}, access{std::move(access)}, remote{std::move(remote)} {}

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
    void send_back(std::string_view command, Args&&... args);

    /// Sends a reply to a request.  This takes no command: the command is always the built-in
    /// "REPLY" command, followed by the unique reply tag, then any reply data parts.  All other
    /// arguments are as in `send_back()`.  You should only send one reply for a command expecting
    /// replies, though this is not enforced: attempting to send multiple replies will simply be
    /// dropped when received by the remote.  (Note, however, that it is possible to send multiple
    /// messages -- e.g. you could send a reply and then also call send_back() and/or send_request()
    /// to send more requests back to the sender).
    template <typename... Args>
    void send_reply(Args&&... args);

    /// Sends a request back to whomever sent this message.  This is effectively a wrapper around
    /// lmq.request() that takes care of setting up the recipient arguments.
    template <typename ReplyCallback, typename... Args>
    void send_request(std::string_view command, ReplyCallback&& callback, Args&&... args);

    /** Class returned by `send_later()` that can be used to call `back()`, `reply()`, or
     * `request()` beyond the lifetime of the Message instance as if calling `msg.send_back()`,
     * `msg.send_reply()`, or `msg.send_request()`.  For example:
     *
     *     auto send = msg.send_later();
     *     // ... later, perhaps in a lambda or scheduled job:
     *     send.reply("content");
     *
     * is equivalent to
     *
     *     msg.send_reply("content");
     *
     * except that it is valid even if `msg` is no longer valid.
     */
    class DeferredSend {
    public:
        OxenMQ& oxenmq; ///< The owning OxenMQ object
        ConnectionID conn; ///< The connection info for routing a reply; also contains the pubkey/sn status
        std::string reply_tag; ///< If the invoked command is a request command this is the required reply tag that will be prepended by `reply()`.

        explicit DeferredSend(Message& m) : oxenmq{m.oxenmq}, conn{m.conn}, reply_tag{m.reply_tag} {}

        /// Equivalent to msg.send_back(...), but can be invoked later.
        template <typename... Args>
        void back(std::string_view command, Args&&... args) const;

        /// Equivalent to msg.send_reply(...), but can be invoked later.
        template <typename... Args>
        void reply(Args&&... args) const;

        /// Equivalent to msg.send_request(...), but can be invoked later.
        template <typename ReplyCallback, typename... Args>
        void request(std::string_view command, ReplyCallback&& callback, Args&&... args) const;
    };

    /// Returns a DeferredSend object that can be used to send replies to this message even if the
    /// message expires.  Typically this is used when sending a reply requires waiting on another
    /// task to complete without needing to block the handler thread.
    DeferredSend send_later() { return DeferredSend{*this}; }
};

}
