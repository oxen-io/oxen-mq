#include "common.h"
#include <future>

using namespace lokimq;

TEST_CASE("basic requests", "[requests]") {
    std::string listen = "tcp://127.0.0.1:5678";
    LokiMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        {listen},
        [](auto &) { return ""; },
        [](auto /*ip*/, auto /*pk*/) { return Allow{AuthLevel::none, false}; },
    };

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

    client.connect_remote(listen,
            [&](std::string pk) { pubkey = std::move(pk); connected = true; },
            [&](string_view) { failed = true; },
            server.get_pubkey());

    int i;
    for (i = 0; i < 5; i++) {
        if (connected.load())
            break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE( connected.load() );
    REQUIRE( !failed.load() );
    REQUIRE( i <= 1 );
    REQUIRE( pubkey == server.get_pubkey() );

    std::atomic<bool> got_reply{false};
    bool success;
    std::vector<std::string> data;
    client.request(pubkey, "public.hello", [&](bool ok, std::vector<std::string> data_) {
            got_reply = true;
            success = ok;
            data = std::move(data_);
    });

    std::this_thread::sleep_for(50ms);
    REQUIRE( got_reply.load() );
    REQUIRE( success );
    REQUIRE( data == std::vector<std::string>{{"123"}} );
}
