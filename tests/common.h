#pragma once
#include "lokimq/lokimq.h"
#include <catch2/catch.hpp>

using namespace lokimq;

static auto startup = std::chrono::steady_clock::now();

/// Waits up to 100ms for something to happen.
template <typename Func>
inline void wait_for(Func f) {
    for (int i = 0; i < 10; i++) {
        if (f())
            break;
        std::this_thread::sleep_for(10ms);
    }
}

/// Waits on an atomic bool for up to 100ms for an initial connection, which is more than enough
/// time for an initial connection + request.
inline void wait_for_conn(std::atomic<bool> &c) {
    wait_for([&c] { return c.load(); });
}

/// Waits enough time for us to receive a reply from a localhost remote.
inline void reply_sleep() { std::this_thread::sleep_for(10ms); }

// Catch2 macros aren't thread safe, so guard with a mutex
inline std::unique_lock<std::mutex> catch_lock() {
    static std::mutex mutex;
    return std::unique_lock<std::mutex>{mutex};
}

inline LokiMQ::Logger get_logger(std::string prefix = "") {
    std::string me = "tests/common.h";
    std::string strip = __FILE__;
    if (strip.substr(strip.size() - me.size()) == me)
        strip.resize(strip.size() - me.size());
    else
        strip.clear();

    return [prefix,strip](LogLevel lvl, std::string file, int line, std::string msg) {
        if (!strip.empty() && file.substr(0, strip.size()) == strip)
            file = file.substr(strip.size());

        auto lock = catch_lock();
        UNSCOPED_INFO(prefix << "[" << file << ":" << line << "/"
                "+" << std::chrono::duration<double>(std::chrono::steady_clock::now() - startup).count() << "s]: "
                << lvl << ": " << msg);
    };
}
