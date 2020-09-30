#include "common.h"

using namespace lokimq;

TEST_CASE("injected external commands", "[injected]") {
    std::string listen = "tcp://127.0.0.1:4567";
    LokiMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
        get_logger("S» "),
        LogLevel::trace
    };
    server.set_general_threads(1);
    server.listen_curve(listen);

    std::atomic<int> hellos = 0;
    std::atomic<bool> done = false;
    server.add_category("public", AuthLevel::none, 3);
    server.add_command("public", "hello", [&](Message& m) {
        hellos++;
        while (!done) std::this_thread::sleep_for(10ms);
    });

    server.start();

    LokiMQ client{get_logger("C» "), LogLevel::trace};
    client.start();

    std::atomic<bool> got{false};
    bool success = false;

    auto c = client.connect_remote(listen,
            [&](auto conn) { success = true; got = true; },
            [&](auto conn, std::string_view) { got = true; },
            server.get_pubkey());

    wait_for_conn(got);
    {
        auto lock = catch_lock();
        REQUIRE( got );
        REQUIRE( success );
    }

    // First make sure that basic message respects the 3 thread limit
    client.send(c, "public.hello");
    client.send(c, "public.hello");
    client.send(c, "public.hello");
    client.send(c, "public.hello");
    wait_for([&] { return hellos >= 3; });
    std::this_thread::sleep_for(20ms);
    {
        auto lock = catch_lock();
        REQUIRE( hellos == 3 );
    }
    done = true;
    wait_for([&] { return hellos >= 4; });
    {
        auto lock = catch_lock();
        REQUIRE( hellos == 4 );
    }

    // Now try injecting external commands
    done = false;
    hellos = 0;
    client.send(c, "public.hello");
    wait_for([&] { return hellos >= 1; });
    server.inject_task("public", "(injected)", "localhost", [&] { hellos += 10; while (!done) std::this_thread::sleep_for(10ms); });
    wait_for([&] { return hellos >= 11; });
    client.send(c, "public.hello");
    wait_for([&] { return hellos >= 12; });
    server.inject_task("public", "(injected)", "localhost", [&] { hellos += 10; while (!done) std::this_thread::sleep_for(10ms); });
    server.inject_task("public", "(injected)", "localhost", [&] { hellos += 10; while (!done) std::this_thread::sleep_for(10ms); });
    server.inject_task("public", "(injected)", "localhost", [&] { hellos += 10; while (!done) std::this_thread::sleep_for(10ms); });
    wait_for([&] { return hellos >= 12; });
    std::this_thread::sleep_for(20ms);
    {
        auto lock = catch_lock();
        REQUIRE( hellos == 12 );
    }
    done = true;
    wait_for([&] { return hellos >= 42; });
    {
        auto lock = catch_lock();
        REQUIRE( hellos == 42 );
    }
}
