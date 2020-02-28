#include "common.h"
#include <future>
#include <lokimq/hex.h>

using namespace lokimq;

TEST_CASE("basic commands", "[commands]") {
    std::string listen = "tcp://127.0.0.1:4567";
    LokiMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
        get_logger("S» ")
    };
    server.log_level(LogLevel::trace);
    server.listen_curve(listen, [](auto /*ip*/, auto /*pk*/) { return Allow{AuthLevel::none, false}; });

    std::atomic<int> hellos{0}, his{0};

    server.add_category("public", Access{AuthLevel::none});
    server.add_command("public", "hello", [&](Message& m) {
            // On every 1st, 3rd, 5th, ... hello send back a hi
            if (hellos++ % 2 == 0)
                m.send_back("public.hi");
    });
    std::string client_pubkey;
    server.add_command("public", "client.pubkey", [&](Message& m) {
            client_pubkey = std::string{m.conn.pubkey()};
    });

    server.start();

    LokiMQ client{
        get_logger("C» ")
    };
    client.log_level(LogLevel::trace);

    client.add_category("public", Access{AuthLevel::none});
    client.add_command("public", "hi", [&](auto&) { his++; });
    client.start();

    std::atomic<bool> connected{false}, failed{false};
    std::string pubkey;

    auto c = client.connect_remote(listen,
            [&](auto conn) { pubkey = conn.pubkey(); connected = true; },
            [&](auto conn, string_view) { failed = true; },
            server.get_pubkey());

    int i;
    for (i = 0; i < 5; i++) {
        if (connected.load())
            break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE( connected.load() );
    REQUIRE( i <= 1 ); // should be fast
    REQUIRE( !failed.load() );
    REQUIRE( to_hex(pubkey) == to_hex(server.get_pubkey()) );

    client.send(c, "public.hello");
    client.send(c, "public.client.pubkey");

    std::this_thread::sleep_for(50ms);
    REQUIRE( hellos == 1 );
    REQUIRE( his == 1 );
    REQUIRE( to_hex(client_pubkey) == to_hex(client.get_pubkey()) );

    for (int i = 0; i < 50; i++)
        client.send(c, "public.hello");

    std::this_thread::sleep_for(100ms);
    REQUIRE( hellos == 51 );
    REQUIRE( his == 26 );
}
