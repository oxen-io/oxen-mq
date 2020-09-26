#include "common.h"
#include <lokimq/hex.h>

using namespace lokimq;

TEST_CASE("basic requests", "[requests]") {
    std::string listen = "tcp://127.0.0.1:5678";
    LokiMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    server.listen_curve(listen);

    std::atomic<int> hellos{0}, his{0};

    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "hello", [&](Message& m) {
            m.send_reply("123");
    });
    server.start();

    LokiMQ client(
        [](LogLevel, const char* file, int line, std::string msg) { std::cerr << file << ":" << line << " --C-- " << msg << "\n"; }
        );
    //client.log_level(LogLevel::trace);

    client.start();

    std::atomic<bool> connected{false}, failed{false};
    std::string pubkey;

    auto c = client.connect_remote(address{listen, server.get_pubkey()},
            [&](auto conn) { pubkey = conn.pubkey(); connected = true; },
            [&](auto, auto) { failed = true; });

    wait_for([&] { return connected || failed; });
    {
        auto lock = catch_lock();
        REQUIRE( connected );
        REQUIRE_FALSE( failed );
        REQUIRE( to_hex(pubkey) == to_hex(server.get_pubkey()) );
    }

    std::atomic<bool> got_reply{false};
    bool success;
    std::vector<std::string> data;
    client.request(c, "public.hello", [&](bool ok, std::vector<std::string> data_) {
            got_reply = true;
            success = ok;
            data = std::move(data_);
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got_reply.load() );
        REQUIRE( success );
        REQUIRE( data == std::vector<std::string>{{"123"}} );
    }
}

TEST_CASE("request from server to client", "[requests]") {
    std::string listen = "tcp://127.0.0.1:5678";
    LokiMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    server.listen_curve(listen);

    std::atomic<int> hellos{0}, his{0};

    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "hello", [&](Message& m) {
            m.send_reply("123");
    });
    server.start();

    LokiMQ client(
        [](LogLevel, const char* file, int line, std::string msg) { std::cerr << file << ":" << line << " --C-- " << msg << "\n"; }
        );
    //client.log_level(LogLevel::trace);

    client.start();

    std::atomic<bool> connected{false}, failed{false};
    std::string pubkey;

    auto c = client.connect_remote(address{listen, server.get_pubkey()},
            [&](auto conn) { pubkey = conn.pubkey(); connected = true; },
            [&](auto, auto) { failed = true; });

    int i;
    for (i = 0; i < 5; i++) {
        if (connected.load())
            break;
        std::this_thread::sleep_for(50ms);
    }
    {
        auto lock = catch_lock();
        REQUIRE( connected.load() );
        REQUIRE( !failed.load() );
        REQUIRE( i <= 1 );
        REQUIRE( to_hex(pubkey) == to_hex(server.get_pubkey()) );
    }

    std::atomic<bool> got_reply{false};
    bool success;
    std::vector<std::string> data;
    client.request(c, "public.hello", [&](bool ok, std::vector<std::string> data_) {
            got_reply = true;
            success = ok;
            data = std::move(data_);
    });

    std::this_thread::sleep_for(50ms);
    {
        auto lock = catch_lock();
        REQUIRE( got_reply.load() );
        REQUIRE( success );
        REQUIRE( data == std::vector<std::string>{{"123"}} );
    }
}

TEST_CASE("request timeouts", "[requests][timeout]") {
    std::string listen = "tcp://127.0.0.1:5678";
    LokiMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    server.listen_curve(listen);

    std::atomic<int> hellos{0}, his{0};

    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "blackhole", [&](Message& m) { /* doesn't reply */ });
    server.start();

    LokiMQ client(
        [](LogLevel, const char* file, int line, std::string msg) { std::cerr << file << ":" << line << " --C-- " << msg << "\n"; }
        );
    //client.log_level(LogLevel::trace);

    client.CONN_CHECK_INTERVAL = 10ms; // impatience (don't set this low in production code)
    client.start();

    std::atomic<bool> connected{false}, failed{false};
    std::string pubkey;

    auto c = client.connect_remote(address{listen, server.get_pubkey()},
            [&](auto conn) { pubkey = conn.pubkey(); connected = true; },
            [&](auto, auto) { failed = true; });

    wait_for([&] { return connected || failed; });

    REQUIRE( connected );
    REQUIRE_FALSE( failed );
    REQUIRE( to_hex(pubkey) == to_hex(server.get_pubkey()) );

    std::atomic<bool> got_triggered{false};
    bool success;
    std::vector<std::string> data;
    client.request(c, "public.blackhole", [&](bool ok, std::vector<std::string> data_) {
            got_triggered = true;
            success = ok;
            data = std::move(data_);
        },
        lokimq::send_option::request_timeout{10ms}
    );

    std::atomic<bool> got_triggered2{false};
    client.request(c, "public.blackhole", [&](bool ok, std::vector<std::string> data_) {
            got_triggered = true;
            success = ok;
            data = std::move(data_);
        },
        lokimq::send_option::request_timeout{200ms}
    );

    std::this_thread::sleep_for(100ms);
    REQUIRE( got_triggered );
    REQUIRE_FALSE( got_triggered2 );
    REQUIRE_FALSE( success );
    REQUIRE( data == std::vector<std::string>{{"TIMEOUT"}} );

}
