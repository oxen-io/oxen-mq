#pragma once
#include "lokimq/lokimq.h"
#include <catch2/catch.hpp>

using namespace lokimq;

static auto startup = std::chrono::steady_clock::now();

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
