#include "common.h"
#include "oxenmq/pubsub.h"

#include <oxenc/hex.h>

using namespace oxenmq;
using namespace std::chrono_literals;

TEST_CASE("sub OK", "[pubsub]") {
    std::string listen = random_localhost();
    OxenMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    server.listen_curve(listen);

    Subscription<> greetings{"greetings"};

    std::atomic<bool> is_new{false};
    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "greetings", [&](Message& m) {
            is_new = greetings.subscribe(m.conn);
            m.send_reply("OK");
    });
    server.start();

    OxenMQ client(
        [](LogLevel, const char* file, int line, std::string msg) { std::cerr << file << ":" << line << " --C-- " << msg << "\n"; }
        );

    std::atomic<int> reply_count{0};
    client.add_category("notify", Access{AuthLevel::none});
    client.add_command("notify", "greetings", [&](Message& m) {
            const auto& data = m.data;
            if (!data.size())
            {
                std::cerr << "client received public.greetings with empty data\n";
                return;
            }
            if (data[0] == "hello")
                reply_count++;
    });

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
        REQUIRE( oxenc::to_hex(pubkey) == oxenc::to_hex(server.get_pubkey()) );
    }

    std::atomic<bool> got_reply{false};
    bool success;
    std::vector<std::string> data;
    client.request(c, "public.greetings", [&](bool ok, std::vector<std::string> data_) {
            got_reply = true;
            success = ok;
            data = std::move(data_);
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got_reply.load() );
        REQUIRE( success );
        REQUIRE( data == std::vector<std::string>{{"OK"}} );
    }
    
    greetings.publish([&](auto& conn) {
            server.send(conn, "notify.greetings", "hello");
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( reply_count == 1 );
    }

    greetings.publish([&](auto& conn) {
            server.send(conn, "notify.greetings", "hello");
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( reply_count == 2 );
    }

}

TEST_CASE("user data", "[pubsub]") {
    std::string listen = random_localhost();
    OxenMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    server.listen_curve(listen);

    Subscription<std::string> greetings{"greetings"};

    std::atomic<bool> is_new{false};
    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "greetings", [&](Message& m) {
            is_new = greetings.subscribe(m.conn, std::string{m.data[0]});
            m.send_reply("OK");
    });
    server.start();

    OxenMQ client(
        [](LogLevel, const char* file, int line, std::string msg) { std::cerr << file << ":" << line << " --C-- " << msg << "\n"; }
        );

    std::string response{"foo"};
    std::atomic<int> reply_count{0};
    std::atomic<int> foo_count{0};
    client.add_category("notify", Access{AuthLevel::none});
    client.add_command("notify", "greetings", [&](Message& m) {
            const auto& data = m.data;
            if (!data.size())
            {
                std::cerr << "client received public.greetings with empty data\n";
                return;
            }
            if (data[0] == response)
                reply_count++;
            if (data[0] == "foo")
                foo_count++;
    });

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
        REQUIRE( oxenc::to_hex(pubkey) == oxenc::to_hex(server.get_pubkey()) );
    }

    std::atomic<bool> got_reply{false};
    std::atomic<bool> success;
    std::vector<std::string> data;
    client.request(c, "public.greetings", [&](bool ok, std::vector<std::string> data_) {
            got_reply = true;
            success = ok;
            data = std::move(data_);
    }, response);

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got_reply.load() );
        REQUIRE( success );
        REQUIRE( is_new );
        REQUIRE( data == std::vector<std::string>{{"OK"}} );
    }

    got_reply = false;
    success = false;
    client.request(c, "public.greetings", [&](bool ok, std::vector<std::string> data_) {
            got_reply = true;
            success = ok;
            data = std::move(data_);
    }, response);

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got_reply.load() );
        REQUIRE( success );
        REQUIRE_FALSE( is_new );
        REQUIRE( data == std::vector<std::string>{{"OK"}} );
    }
    
    greetings.publish([&](auto& conn, std::string user) {
            server.send(conn, "notify.greetings", user);
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( reply_count == 1 );
        REQUIRE( foo_count == 1 );
    }

    got_reply = false;
    success = false;
    response = "bar";
    client.request(c, "public.greetings", [&](bool ok, std::vector<std::string> data_) {
            got_reply = true;
            success = ok;
            data = std::move(data_);
    }, response);

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got_reply.load() );
        REQUIRE( success );
        REQUIRE( is_new );
        REQUIRE( data == std::vector<std::string>{{"OK"}} );
    }
    
    greetings.publish([&](auto& conn, std::string user) {
            server.send(conn, "notify.greetings", user);
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( reply_count == 2 );
        REQUIRE( foo_count == 1 );
    }

}

TEST_CASE("unsubscribe", "[pubsub]") {
    std::string listen = random_localhost();
    OxenMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    server.listen_curve(listen);

    Subscription<> greetings{"greetings"};

    std::atomic<bool> was_subbed{false};
    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "greetings", [&](Message& m) {
            greetings.subscribe(m.conn);
            m.send_reply("OK");
    });
    server.add_request_command("public", "goodbye", [&](Message& m) {
            was_subbed = greetings.unsubscribe(m.conn);
            m.send_reply("OK");
    });
    server.start();

    OxenMQ client(
        [](LogLevel, const char* file, int line, std::string msg) { std::cerr << file << ":" << line << " --C-- " << msg << "\n"; }
        );

    std::atomic<int> reply_count{0};
    client.add_category("notify", Access{AuthLevel::none});
    client.add_command("notify", "greetings", [&](Message& m) {
            const auto& data = m.data;
            if (!data.size())
            {
                std::cerr << "client received public.greetings with empty data\n";
                return;
            }
            if (data[0] == "hello")
                reply_count++;
    });

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
        REQUIRE( oxenc::to_hex(pubkey) == oxenc::to_hex(server.get_pubkey()) );
    }

    std::atomic<bool> got_reply{false};
    std::atomic<bool> success;
    std::vector<std::string> data;
    client.request(c, "public.greetings", [&](bool ok, std::vector<std::string> data_) {
            got_reply = true;
            success = ok;
            data = std::move(data_);
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got_reply.load() );
        REQUIRE( success );
        REQUIRE( data == std::vector<std::string>{{"OK"}} );
    }
    
    greetings.publish([&](auto& conn) {
            server.send(conn, "notify.greetings", "hello");
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( reply_count == 1 );
    }

    got_reply = false;
    success = false;
    client.request(c, "public.goodbye", [&](bool ok, std::vector<std::string> data_) {
            got_reply = true;
            success = ok;
            data = std::move(data_);
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got_reply.load() );
        REQUIRE( success );
        REQUIRE( data == std::vector<std::string>{{"OK"}} );
        REQUIRE( was_subbed );
    }

    greetings.publish([&](auto& conn) {
            server.send(conn, "notify.greetings", "hello");
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( reply_count == 1 );
    }

    got_reply = false;
    success = false;
    client.request(c, "public.goodbye", [&](bool ok, std::vector<std::string> data_) {
            got_reply = true;
            success = ok;
            data = std::move(data_);
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got_reply.load() );
        REQUIRE( success );
        REQUIRE( data == std::vector<std::string>{{"OK"}} );
        REQUIRE( was_subbed == false);
    }

}

TEST_CASE("expire", "[pubsub]") {
    std::string listen = random_localhost();
    OxenMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    server.listen_curve(listen);

    Subscription<> greetings{"greetings", 250ms};

    std::atomic<bool> was_subbed{false};
    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "greetings", [&](Message& m) {
            greetings.subscribe(m.conn);
            m.send_reply("OK");
    });
    server.add_request_command("public", "goodbye", [&](Message& m) {
            was_subbed = greetings.unsubscribe(m.conn);
            m.send_reply("OK");
    });
    server.start();

    OxenMQ client(
        [](LogLevel, const char* file, int line, std::string msg) { std::cerr << file << ":" << line << " --C-- " << msg << "\n"; }
        );

    std::atomic<int> reply_count{0};
    client.add_category("notify", Access{AuthLevel::none});
    client.add_command("notify", "greetings", [&](Message& m) {
            const auto& data = m.data;
            if (!data.size())
            {
                std::cerr << "client received public.greetings with empty data\n";
                return;
            }
            if (data[0] == "hello")
                reply_count++;
    });

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
        REQUIRE( oxenc::to_hex(pubkey) == oxenc::to_hex(server.get_pubkey()) );
    }

    std::atomic<bool> got_reply{false};
    bool success;
    std::vector<std::string> data;
    client.request(c, "public.greetings", [&](bool ok, std::vector<std::string> data_) {
            got_reply = true;
            success = ok;
            data = std::move(data_);
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got_reply.load() );
        REQUIRE( success );
        REQUIRE( data == std::vector<std::string>{{"OK"}} );
    }

    // should be expired by now
    std::this_thread::sleep_for(500ms);

    greetings.remove_expired();

    got_reply = false;
    success = false;
    client.request(c, "public.goodbye", [&](bool ok, std::vector<std::string> data_) {
            got_reply = true;
            success = ok;
            data = std::move(data_);
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got_reply.load() );
        REQUIRE( success );
        REQUIRE( data == std::vector<std::string>{{"OK"}} );
        REQUIRE( was_subbed == false);
    }

}

TEST_CASE("multiple subs", "[pubsub]") {
    std::string listen = random_localhost();
    OxenMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    server.listen_curve(listen);

    Subscription<> greetings{"greetings"};

    std::atomic<bool> is_new{false};
    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "greetings", [&](Message& m) {
            is_new = greetings.subscribe(m.conn);
            m.send_reply("OK");
    });
    server.start();

/* client 1 */
    std::atomic<int> reply_count_c1{0};
    std::atomic<bool> connected_c1{false}, failed_c1{false};
    std::atomic<bool> got_reply_c1{false};
    bool success_c1;
    std::vector<std::string> data_c1;
    std::string pubkey_c1;
    OxenMQ client1(
        [](LogLevel, const char* file, int line, std::string msg) { std::cerr << file << ":" << line << " --C-- " << msg << "\n"; }
        );

    client1.add_category("notify", Access{AuthLevel::none});
    client1.add_command("notify", "greetings", [&](Message& m) {
            const auto& data = m.data;
            if (!data.size())
            {
                std::cerr << "client received public.greetings with empty data\n";
                return;
            }
            if (data[0] == "hello")
                reply_count_c1++;
    });

    client1.start();

    auto c1 = client1.connect_remote(address{listen, server.get_pubkey()},
            [&](auto conn) { pubkey_c1 = conn.pubkey(); connected_c1 = true; },
            [&](auto, auto) { failed_c1 = true; });

    wait_for([&] { return connected_c1 || failed_c1; });
    {
        auto lock = catch_lock();
        REQUIRE( connected_c1 );
        REQUIRE_FALSE( failed_c1 );
        REQUIRE( oxenc::to_hex(pubkey_c1) == oxenc::to_hex(server.get_pubkey()) );
    }

    client1.request(c1, "public.greetings", [&](bool ok, std::vector<std::string> data_) {
            got_reply_c1 = true;
            success_c1 = ok;
            data_c1 = std::move(data_);
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got_reply_c1.load() );
        REQUIRE( success_c1 );
        REQUIRE( data_c1 == std::vector<std::string>{{"OK"}} );
    }
/* end client 1 */

/* client 2 */
    std::atomic<int> reply_count_c2{0};
    std::atomic<bool> connected_c2{false}, failed_c2{false};
    std::atomic<bool> got_reply_c2{false};
    bool success_c2;
    std::vector<std::string> data_c2;
    std::string pubkey_c2;
    OxenMQ client2(
        [](LogLevel, const char* file, int line, std::string msg) { std::cerr << file << ":" << line << " --C-- " << msg << "\n"; }
        );

    client2.add_category("notify", Access{AuthLevel::none});
    client2.add_command("notify", "greetings", [&](Message& m) {
            const auto& data = m.data;
            if (!data.size())
            {
                std::cerr << "client received public.greetings with empty data\n";
                return;
            }
            if (data[0] == "hello")
                reply_count_c2++;
    });

    client2.start();

    auto c2 = client2.connect_remote(address{listen, server.get_pubkey()},
            [&](auto conn) { pubkey_c2 = conn.pubkey(); connected_c2 = true; },
            [&](auto, auto) { failed_c2 = true; });

    wait_for([&] { return connected_c2 || failed_c2; });
    {
        auto lock = catch_lock();
        REQUIRE( connected_c2 );
        REQUIRE_FALSE( failed_c2 );
        REQUIRE( oxenc::to_hex(pubkey_c2) == oxenc::to_hex(server.get_pubkey()) );
    }

    client2.request(c2, "public.greetings", [&](bool ok, std::vector<std::string> data_) {
            got_reply_c2 = true;
            success_c2 = ok;
            data_c2 = std::move(data_);
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got_reply_c2.load() );
        REQUIRE( success_c2 );
        REQUIRE( data_c2 == std::vector<std::string>{{"OK"}} );
    }
/* end client2 */

    greetings.publish([&](auto& conn) {
            server.send(conn, "notify.greetings", "hello");
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( reply_count_c1 == 1 );
        REQUIRE( reply_count_c2 == 1 );
    }

    greetings.publish([&](auto& conn) {
            server.send(conn, "notify.greetings", "hello");
    });

    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( reply_count_c1 == 2 );
        REQUIRE( reply_count_c2 == 2 );
    }

}


// vim:sw=4:et
