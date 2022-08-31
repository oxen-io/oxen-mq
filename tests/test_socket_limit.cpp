#include "common.h"
#include <oxenc/hex.h>

using namespace oxenmq;

TEST_CASE("zmq socket limit", "[zmq][socket-limit]") {
    // Make sure setting .MAX_SOCKETS works as expected.  (This test was added when a bug was fixed
    // that was causing it not to be applied).
    std::string listen = random_localhost();
    OxenMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    server.listen_plain(listen);
    server.start();

    std::atomic<int> failed = 0, good = 0, failed_toomany = 0;
    OxenMQ client;
    client.MAX_SOCKETS = 15;
    client.start();

    std::vector<ConnectionID> conns;
    address server_addr{listen};
    for (int i = 0; i < 16; i++)
        client.connect_remote(server_addr,
            [&](auto) { good++; },
            [&](auto cid, auto msg) {
                if (msg == "connect() failed: Too many open files")
                    failed_toomany++;
                else
                    failed++;
            });


    wait_for([&] { return good > 0 && failed_toomany > 0; });
    {
        auto lock = catch_lock();
        REQUIRE( good > 0 );
        REQUIRE( failed == 0 );
        REQUIRE( failed_toomany > 0 );
    }
}
