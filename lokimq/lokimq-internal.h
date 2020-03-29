#pragma once
#include "lokimq.h"

// Inside some method:
//     LMQ_LOG(warn, "bad ", 42, " stuff");
//
// (The "this->" is here to work around gcc 5 bugginess when called in a `this`-capturing lambda.)
#define LMQ_LOG(level, ...) this->log_(LogLevel::level, __FILE__, __LINE__, __VA_ARGS__)

#ifndef NDEBUG
// Same as LMQ_LOG(trace, ...) when not doing a release build; nothing under a release build.
#  define LMQ_TRACE(...) this->log_(LogLevel::trace, __FILE__, __LINE__, __VA_ARGS__)
#else
#  define LMQ_TRACE(...)
#endif

namespace lokimq {

constexpr char SN_ADDR_COMMAND[] = "inproc://sn-command";
constexpr char SN_ADDR_WORKERS[] = "inproc://sn-workers";
constexpr char SN_ADDR_SELF[] = "inproc://sn-self";
constexpr char ZMQ_ADDR_ZAP[] = "inproc://zeromq.zap.01";

/// Destructor for create_message(std::string&&) that zmq calls when it's done with the message.
extern "C" inline void message_buffer_destroy(void*, void* hint) {
    delete reinterpret_cast<std::string*>(hint);
}

/// Creates a message without needing to reallocate the provided string data
inline zmq::message_t create_message(std::string &&data) {
    auto *buffer = new std::string(std::move(data));
    return zmq::message_t{&(*buffer)[0], buffer->size(), message_buffer_destroy, buffer};
};

/// Create a message copying from a string_view
inline zmq::message_t create_message(string_view data) {
    return zmq::message_t{data.begin(), data.end()};
}

template <typename It>
bool send_message_parts(zmq::socket_t &sock, It begin, It end) {
    while (begin != end) {
        zmq::message_t &msg = *begin++;
        if (!sock.send(msg, begin == end ? zmq::send_flags::dontwait : zmq::send_flags::dontwait | zmq::send_flags::sndmore))
            return false;
    }
    return true;
}

template <typename Container>
bool send_message_parts(zmq::socket_t &sock, Container &&c) {
    return send_message_parts(sock, c.begin(), c.end());
}

/// Sends a message with an initial route.  `msg` and `data` can be empty: if `msg` is empty then
/// the msg frame will be an empty message; if `data` is empty then the data frame will be omitted.
/// `flags` is passed through to zmq: typically given `zmq::send_flags::dontwait` to throw rather
/// than block if a message can't be queued.
inline bool send_routed_message(zmq::socket_t &socket, std::string route, std::string msg = {}, std::string data = {}) {
    assert(!route.empty());
    std::array<zmq::message_t, 3> msgs{{create_message(std::move(route))}};
    if (!msg.empty())
        msgs[1] = create_message(std::move(msg));
    if (!data.empty())
        msgs[2] = create_message(std::move(data));
    return send_message_parts(socket, msgs.begin(), data.empty() ? std::prev(msgs.end()) : msgs.end());
}

// Sends some stuff to a socket directly.  If dontwait is true then we throw instead of blocking if
// the message cannot be accepted by zmq (i.e. because the outgoing buffer is full).
inline bool send_direct_message(zmq::socket_t &socket, std::string msg, std::string data = {}) {
    std::array<zmq::message_t, 2> msgs{{create_message(std::move(msg))}};
    if (!data.empty())
        msgs[1] = create_message(std::move(data));
    return send_message_parts(socket, msgs.begin(), data.empty() ? std::prev(msgs.end()) : msgs.end());
}

// Receive all the parts of a single message from the given socket.  Returns true if a message was
// received, false if called with flags=zmq::recv_flags::dontwait and no message was available.
inline bool recv_message_parts(zmq::socket_t &sock, std::vector<zmq::message_t>& parts, const zmq::recv_flags flags = zmq::recv_flags::none) {
    do {
        zmq::message_t msg;
        if (!sock.recv(msg, flags))
            return false;
        parts.push_back(std::move(msg));
    } while (parts.back().more());
    return true;
}

inline const char* peer_address(zmq::message_t& msg) {
    try { return msg.gets("Peer-Address"); } catch (...) {}
    return "(unknown)";
}

// Returns a string view of the given message data.  It's the caller's responsibility to keep the
// referenced message alive.  If you want a std::string instead just call `m.to_string()`
inline string_view view(const zmq::message_t& m) {
    return {m.data<char>(), m.size()};
}

inline std::string to_string(AuthLevel a) {
    switch (a) {
        case AuthLevel::denied: return "denied";
        case AuthLevel::none:   return "none";
        case AuthLevel::basic:  return "basic";
        case AuthLevel::admin:  return "admin";
        default:                return "(unknown)";
    }
}

inline AuthLevel auth_from_string(string_view a) {
    if (a == "none") return AuthLevel::none;
    if (a == "basic") return AuthLevel::basic;
    if (a == "admin") return AuthLevel::admin;
    return AuthLevel::denied;
}

// Extracts and builds the "send" part of a message for proxy_send/proxy_reply
inline std::list<zmq::message_t> build_send_parts(bt_list_consumer send, string_view route) {
    std::list<zmq::message_t> parts;
    if (!route.empty())
        parts.push_back(create_message(route));
    while (!send.is_finished())
        parts.push_back(create_message(send.consume_string()));
    return parts;
}

/// Sends a control message to a specific destination by prefixing the worker name (or identity)
/// then appending the command and optional data (if non-empty).  (This is needed when sending the control message
/// to a router socket, i.e. inside the proxy thread).
inline void route_control(zmq::socket_t& sock, string_view identity, string_view cmd, const std::string& data = {}) {
    sock.send(create_message(identity), zmq::send_flags::sndmore);
    detail::send_control(sock, cmd, data);
}



}
