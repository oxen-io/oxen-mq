#include "common.h"
#include <future>
#include <lokimq/hex.h>
#include <map>
#include <set>

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
    {
        auto lock = catch_lock();
        REQUIRE( connected.load() );
        REQUIRE( i <= 1 ); // should be fast
        REQUIRE( !failed.load() );
        REQUIRE( to_hex(pubkey) == to_hex(server.get_pubkey()) );
    }

    client.send(c, "public.hello");
    client.send(c, "public.client.pubkey");

    std::this_thread::sleep_for(50ms);
    {
        auto lock = catch_lock();
        REQUIRE( hellos == 1 );
        REQUIRE( his == 1 );
        REQUIRE( to_hex(client_pubkey) == to_hex(client.get_pubkey()) );
    }

    for (int i = 0; i < 50; i++)
        client.send(c, "public.hello");

    std::this_thread::sleep_for(100ms);
    {
        auto lock = catch_lock();
        REQUIRE( hellos == 51 );
        REQUIRE( his == 26 );
    }
}

TEST_CASE("outgoing auth level", "[commands][auth]") {
    std::string listen = "tcp://127.0.0.1:4567";
    LokiMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
        get_logger("S» ")
    };
    server.log_level(LogLevel::trace);
    server.listen_curve(listen, [](auto /*ip*/, auto /*pk*/) { return Allow{AuthLevel::none, false}; });

    std::atomic<int> hellos{0};

    server.add_category("public", Access{AuthLevel::none});
    server.add_command("public", "reflect", [&](Message& m) { m.send_back(m.data[0]); });

    server.start();

    LokiMQ client{
        get_logger("C» ")
    };
    client.log_level(LogLevel::trace);

    std::atomic<int> public_hi{0}, basic_hi{0}, admin_hi{0};
    client.add_category("public", Access{AuthLevel::none});
    client.add_category("basic", Access{AuthLevel::basic});
    client.add_category("admin", Access{AuthLevel::admin});
    client.add_command("public", "hi", [&](auto&) { public_hi++; });
    client.add_command("basic", "hi", [&](auto&) { basic_hi++; });
    client.add_command("admin", "hi", [&](auto&) { admin_hi++; });
    client.start();

    client.PUBKEY_BASED_ROUTING_ID = false; // establishing multiple connections below, so we need unique routing ids

    auto public_c = client.connect_remote(listen, [](...) {}, [](...) {}, server.get_pubkey());
    auto basic_c = client.connect_remote(listen, [](...) {}, [](...) {}, server.get_pubkey(), AuthLevel::basic);
    auto admin_c = client.connect_remote(listen, [](...) {}, [](...) {}, server.get_pubkey(), AuthLevel::admin);

    client.send(public_c, "public.reflect", "public.hi");
    std::this_thread::sleep_for(50ms);

    {
        auto lock = catch_lock();
        REQUIRE( public_hi == 1 );
    }

    client.send(basic_c, "public.reflect", "basic.hi");
    client.send(admin_c, "public.reflect", "admin.hi");
    client.send(admin_c, "public.reflect", "admin.hi");
    client.send(public_c, "public.reflect", "public.hi");
    client.send(admin_c, "public.reflect", "admin.hi");
    client.send(basic_c, "public.reflect", "basic.hi");

    std::this_thread::sleep_for(50ms);
    {
        auto lock = catch_lock();
        REQUIRE( admin_hi == 3 );
        REQUIRE( basic_hi == 2 );
        REQUIRE( public_hi == 2 );
    }

    admin_hi = 0;
    basic_hi = 0;
    public_hi = 0;

    client.send(public_c, "public.reflect", "admin.hi");
    client.send(public_c, "public.reflect", "basic.hi");
    client.send(public_c, "public.reflect", "public.hi");
    client.send(basic_c, "public.reflect", "admin.hi");
    client.send(basic_c, "public.reflect", "basic.hi");
    client.send(basic_c, "public.reflect", "public.hi");
    client.send(admin_c, "public.reflect", "admin.hi");
    client.send(admin_c, "public.reflect", "basic.hi");
    client.send(admin_c, "public.reflect", "public.hi");

    std::this_thread::sleep_for(50ms);
    auto lock = catch_lock();
    REQUIRE( admin_hi == 1 );
    REQUIRE( basic_hi == 2 );
    REQUIRE( public_hi == 3 );
}

TEST_CASE("deferred replies on incoming connections", "[commands][hey google]") {
    // Tests that the ConnectionID from a Message can be stored and reused later to contact the
    // original node.

    std::string listen = "tcp://127.0.0.1:4567";
    LokiMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
        get_logger("S» ")
    };
    server.log_level(LogLevel::trace);
    server.listen_curve(listen, [](auto /*ip*/, auto /*pk*/) { return Allow{AuthLevel::none, false}; });

    std::vector<std::pair<ConnectionID, std::string>> subscribers;
    ConnectionID backdoor;

    server.add_category("hey google", Access{AuthLevel::none});
    server.add_request_command("hey google", "remember", [&](Message& m) {
            auto l = catch_lock();
            subscribers.emplace_back(m.conn, std::string{m.data[0]});
            m.send_reply("Okay, I'll remember that.");

            if (backdoor)
                m.lokimq.send(backdoor, "backdoor.data", m.data[0]);
    });
    server.add_command("hey google", "recall", [&](Message& m) {
            auto l = catch_lock();
            for (auto& s : subscribers) {
                server.send(s.first, "personal.detail", s.second);
            }
    });
    server.add_command("hey google", "install backdoor", [&](Message& m) {
            auto l = catch_lock();
            backdoor = m.conn;
    });

    server.start();

    auto connect_success = [&](...) { auto l = catch_lock(); REQUIRE(true); };
    auto connect_failure = [&](...) { auto l = catch_lock(); REQUIRE(false); };


    std::set<std::string> backdoor_details;

    LokiMQ nsa{get_logger("NSA» ")};
    nsa.add_category("backdoor", Access{AuthLevel::admin});
    nsa.add_command("backdoor", "data", [&](Message& m) {
            backdoor_details.emplace(m.data[0]);
    });
    nsa.start();
    auto nsa_c = nsa.connect_remote(listen, connect_success, connect_failure, server.get_pubkey(), AuthLevel::admin);
    nsa.send(nsa_c, "hey google.install backdoor");

    std::this_thread::sleep_for(50ms);

    {
        auto l = catch_lock();
        REQUIRE( backdoor );
    }

    std::vector<std::unique_ptr<LokiMQ>> clients;
    std::vector<ConnectionID> conns;
    std::map<int, std::set<std::string>> personal_details{
        {0, {"Loretta"s, "photos"s}},
        {1, {"moustache hatred"s}},
        {2, {"Alaska"s, "scallops"s}},
        {3, {"snorted when she laughed"s, "tickled pink"s}},
        {4, {"I'm the luckiest man in the world"s, "because all my life are belong to Google"s}}
    };
    std::set<std::string> all_the_things;
    for (auto& pd : personal_details) all_the_things.insert(pd.second.begin(), pd.second.end());

    std::map<int, std::set<std::string>> google_knows;
    int things_remembered{0};
    for (int i = 0; i < 5; i++) {
        clients.push_back(std::make_unique<LokiMQ>(get_logger("C" + std::to_string(i) + "» ")));
        auto& c = clients.back();
        c->log_level(LogLevel::trace);
        c->add_category("personal", Access{AuthLevel::basic});
        c->add_command("personal", "detail", [&,i](Message& m) {
            auto l = catch_lock();
            google_knows[i].emplace(m.data[0]);
        });
        c->start();
        conns.push_back(
                c->connect_remote(listen, connect_success, connect_failure, server.get_pubkey(), AuthLevel::basic));
        for (auto& personal_detail : personal_details[i])
            c->request(conns.back(), "hey google.remember",
                [&](bool success, std::vector<std::string> data) {
                    auto l = catch_lock();
                    REQUIRE( success );
                    REQUIRE( data.size() == 1 );
                    REQUIRE( data[0] == "Okay, I'll remember that." );
                    things_remembered++;
                },
                personal_detail);
    }
    std::this_thread::sleep_for(50ms);
    {
        auto l = catch_lock();
        REQUIRE( things_remembered == all_the_things.size() );
        REQUIRE( backdoor_details == all_the_things );
    }

    clients[0]->send(conns[0], "hey google.recall");
    std::this_thread::sleep_for(50ms);
    {
        auto l = catch_lock();
        REQUIRE( google_knows == personal_details );
    }
}
