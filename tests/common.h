#pragma once
#include "oxenmq/oxenmq.h"
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <chrono>
#include <oxen/log.hpp>
#include "oxenmq/fmt.h"

using namespace oxenmq;

// Apple's mutexes, thread scheduling, and IO handling are garbage and it shows up with lots of
// spurious failures in this test suite (because it expects a system to not suck that badly), so we
// multiply the time-sensitive bits by this factor as a hack to make the test suite work.
constexpr int TIME_DILATION =
#ifdef __APPLE__
    5;
#elif defined(__x86_64__)
    1;
#else
    2;
#endif

static auto startup = std::chrono::steady_clock::now();

/// Returns a localhost connection string to listen on.  It can be considered random, though in
/// practice in the current implementation is sequential starting at 25432.
inline std::string random_localhost() {
    static std::atomic<uint16_t> last = 25432;
    last++;
    assert(last); // We should never call this enough to overflow
    return "tcp://127.0.0.1:" + std::to_string(last);
}


// Catch2 macros aren't thread safe, so guard with a mutex
inline std::unique_lock<std::mutex> catch_lock() {
    static std::mutex mutex;
    return std::unique_lock<std::mutex>{mutex};
}

/// Waits up to 200ms for something to happen.
template <typename Func>
inline void wait_for(Func f, std::chrono::milliseconds wait_time = 200ms) {
    auto start = std::chrono::steady_clock::now();
    auto end = start + wait_time * TIME_DILATION;
    while (std::chrono::steady_clock::now() < end) {
        if (f())
            break;
        std::this_thread::sleep_for(10ms * TIME_DILATION);
    }
    auto lock = catch_lock();
    UNSCOPED_INFO(
            "done waiting after " << (std::chrono::steady_clock::now() - start).count() << "ns");
}

/// Waits on an atomic bool for up to 100ms for an initial connection, which is more than enough
/// time for an initial connection + request.
inline void wait_for_conn(std::atomic<bool>& c) {
    wait_for([&c] { return c.load(); });
}

/// Waits enough time for us to receive a reply from a localhost remote.
inline void reply_sleep() {
    std::this_thread::sleep_for(10ms * TIME_DILATION);
}

namespace oxenmq {
class TestSuiteHelper {
  public:
    static size_t num_peers(const OxenMQ& omq) { return omq.peers.size(); }
};
}  // namespace oxenmq
