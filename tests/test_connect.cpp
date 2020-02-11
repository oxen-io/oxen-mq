#include "common.h"
#include <future>

TEST_CASE("connections", "[curve][connect]") {
    std::string listen = "tcp://127.0.0.1:4455";
    LokiMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        {listen},
        [](auto &) { return ""; },
        [](auto /*ip*/, auto /*pk*/) { return Allow{AuthLevel::none, false}; },
        get_logger("S» ")
    };
//    server.log_level(LogLevel::trace);

    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "hello", [&](Message& m) { m.send_reply("hi"); });
    server.start();

    LokiMQ client{get_logger("C» ")};
    client.log_level(LogLevel::trace);

    client.start();

    auto pubkey = server.get_pubkey();
    std::atomic<int> connected{0};
    client.connect_remote(listen,
            [&](std::string pk) { connected = 1; },
            [&](string_view reason) { auto lock = catch_lock(); INFO("connection failed: " << reason); },
            pubkey);

    int i;
    for (i = 0; i < 5; i++) {
        if (connected.load())
            break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE( i <= 1 );
    REQUIRE( connected.load() );

    bool success = false;
    std::vector<std::string> parts;
    client.request(pubkey, "public.hello", [&](auto success_, auto parts_) { success = success_; parts = parts_; });
    std::this_thread::sleep_for(50ms);
    REQUIRE( success );

}

