#include "common.h"
#include <lokimq/hex.h>
extern "C" {
#include <sodium.h>
}


TEST_CASE("connections with curve authentication", "[curve][connect]") {
    std::string listen = "tcp://127.0.0.1:4455";
    LokiMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
        get_logger("S» "),
        LogLevel::trace
    };

    server.listen_curve(listen);
    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "hello", [&](Message& m) { m.send_reply("hi"); });
    server.start();

    LokiMQ client{get_logger("C» "), LogLevel::trace};

    client.start();

    auto pubkey = server.get_pubkey();
    std::atomic<bool> got{false};
    bool success = false;
    auto server_conn = client.connect_remote(listen,
            [&](auto conn) { success = true; got = true; },
            [&](auto conn, string_view reason) { auto lock = catch_lock(); INFO("connection failed: " << reason); got = true; },
            pubkey);

    wait_for_conn(got);
    {
        auto lock = catch_lock();
        REQUIRE( got );
        REQUIRE( success );
    }

    success = false;
    std::vector<std::string> parts;
    client.request(server_conn, "public.hello", [&](auto success_, auto parts_) { success = success_; parts = parts_; });
    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( success );
    }
}

TEST_CASE("self-connection SN optimization", "[connect][self]") {
    std::string pubkey, privkey;
    pubkey.resize(crypto_box_PUBLICKEYBYTES);
    privkey.resize(crypto_box_SECRETKEYBYTES);
    crypto_box_keypair(reinterpret_cast<unsigned char*>(&pubkey[0]), reinterpret_cast<unsigned char*>(&privkey[0]));
    LokiMQ sn{
        pubkey, privkey,
        true,
        [&](auto pk) { if (pk == pubkey) return "tcp://127.0.0.1:5544"; else return ""; },
        get_logger("S» "),
        LogLevel::trace
    };

    sn.listen_curve("tcp://127.0.0.1:5544", [&](auto ip, auto pk, auto sn) {
            auto lock = catch_lock();
            REQUIRE(ip == "127.0.0.1");
            REQUIRE(sn == (pk == pubkey));
            return AuthLevel::none;
    });
    sn.add_category("a", Access{AuthLevel::none});
    std::atomic<bool> invoked{false};
    sn.add_command("a", "b", [&](const Message& m) {
            invoked = true;
            auto lock = catch_lock();
            REQUIRE(m.conn.sn());
            REQUIRE(m.conn.pubkey() == pubkey);
            REQUIRE(!m.data.empty());
            REQUIRE(m.data[0] == "my data");
    });
    sn.set_active_sns({{pubkey}});

    sn.start();
    sn.send(pubkey, "a.b", "my data");
    wait_for_conn(invoked);
    {
        auto lock = catch_lock();
        REQUIRE(invoked);
    }
}

TEST_CASE("plain-text connections", "[plaintext][connect]") {
    std::string listen = "tcp://127.0.0.1:4455";
    LokiMQ server{get_logger("S» "), LogLevel::trace};

    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "hello", [&](Message& m) { m.send_reply("hi"); });

    server.listen_plain(listen);

    server.start();

    LokiMQ client{get_logger("C» "), LogLevel::trace};

    client.start();

    std::atomic<bool> got{false};
    bool success = false;
    auto c = client.connect_remote(listen,
            [&](auto conn) { success = true; got = true; },
            [&](auto conn, string_view reason) { auto lock = catch_lock(); INFO("connection failed: " << reason); got = true; }
            );

    wait_for_conn(got);
    {
        auto lock = catch_lock();
        REQUIRE( got );
        REQUIRE( success );
    }

    success = false;
    std::vector<std::string> parts;
    client.request(c, "public.hello", [&](auto success_, auto parts_) { success = success_; parts = parts_; });
    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( success );
    }
}

TEST_CASE("SN disconnections", "[connect][disconnect]") {
    std::vector<std::unique_ptr<LokiMQ>> lmq;
    std::vector<std::string> pubkey, privkey;
    std::unordered_map<std::string, std::string> conn;
    for (int i = 0; i < 3; i++) {
        pubkey.emplace_back();
        privkey.emplace_back();
        pubkey[i].resize(crypto_box_PUBLICKEYBYTES);
        privkey[i].resize(crypto_box_SECRETKEYBYTES);
        crypto_box_keypair(reinterpret_cast<unsigned char*>(&pubkey[i][0]), reinterpret_cast<unsigned char*>(&privkey[i][0]));
        conn.emplace(pubkey[i], "tcp://127.0.0.1:" + std::to_string(4450 + i));
    }
    std::atomic<int> his{0};
    for (int i = 0; i < pubkey.size(); i++) {
        lmq.push_back(std::make_unique<LokiMQ>(
            pubkey[i], privkey[i], true,
            [conn](auto pk) { auto it = conn.find((std::string) pk); if (it != conn.end()) return it->second; return ""s; },
            get_logger("S" + std::to_string(i) + "» "),
            LogLevel::trace
        ));
        auto& server = *lmq.back();

        server.listen_curve(conn[pubkey[i]]);
        server.add_category("sn", Access{AuthLevel::none, true})
            .add_command("hi", [&](Message& m) { his++; });
        server.set_active_sns({pubkey.begin(), pubkey.end()});
        server.start();
    }

    lmq[0]->send(pubkey[1], "sn.hi");
    lmq[0]->send(pubkey[2], "sn.hi");
    lmq[2]->send(pubkey[0], "sn.hi");
    lmq[2]->send(pubkey[1], "sn.hi");
    lmq[1]->send(pubkey[0], "BYE");
    lmq[0]->send(pubkey[2], "sn.hi");
    std::this_thread::sleep_for(50ms);

    auto lock = catch_lock();
    REQUIRE(his == 5);
}

TEST_CASE("SN auth checks", "[sandwich][auth]") {
    // When a remote connects, we check its authentication level; if at the time of connection it
    // isn't recognized as a SN but tries to invoke a SN command it'll be told to disconnect; if it
    // tries to send again it should reconnect and reauthenticate.  This test is meant to test this
    // pattern where the reconnection/reauthentication now authenticates it as a SN.
    std::string listen = "tcp://127.0.0.1:4455";
    std::string pubkey, privkey;
    pubkey.resize(crypto_box_PUBLICKEYBYTES);
    privkey.resize(crypto_box_SECRETKEYBYTES);
    crypto_box_keypair(reinterpret_cast<unsigned char*>(&pubkey[0]), reinterpret_cast<unsigned char*>(&privkey[0]));
    LokiMQ server{
        pubkey, privkey,
        true, // service node
        [](auto) { return ""; },
        get_logger("A» "),
        LogLevel::trace
    };

    std::atomic<bool> incoming_is_sn{false};
    server.listen_curve(listen);
    server.add_category("public", Access{AuthLevel::none})
        .add_request_command("hello", [&](Message& m) { m.send_reply("hi"); })
        .add_request_command("sudo", [&](Message& m) {
                server.update_active_sns({{m.conn.pubkey()}}, {});
                m.send_reply("making sandwiches");
        })
        .add_request_command("nosudo", [&](Message& m) {
                // Send the reply *first* because if we do it the other way we'll have just removed
                // ourselves from the list of SNs and thus would try to open an outbound connection
                // to deliver it since it's still queued as a message to a SN.
                m.send_reply("make them yourself");
                server.update_active_sns({}, {{m.conn.pubkey()}});
        });
    server.add_category("sandwich", Access{AuthLevel::none, true})
        .add_request_command("make", [&](Message& m) { m.send_reply("okay"); });
    server.start();

    LokiMQ client{
        "", "", false,
        [&](auto remote_pk) { if (remote_pk == pubkey) return listen; return ""s; },
        get_logger("B» "), LogLevel::trace};
    client.start();

    std::atomic<bool> got{false};
    bool success;
    client.request(pubkey, "public.hello", [&](auto success_, auto) { success = success_; got = true; });
    wait_for_conn(got);
    {
        auto lock = catch_lock();
        REQUIRE( got );
        REQUIRE( success );
    }

    got = false;
    using dvec = std::vector<std::string>;
    dvec data;
    client.request(pubkey, "sandwich.make", [&](auto success_, auto data_) {
            success = success_;
            data = std::move(data_);
            got = true;
    });
    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got );
        REQUIRE_FALSE( success );
        REQUIRE( data == dvec{{"FORBIDDEN_SN"}} );
    }

    // Somebody set up us the bomb.  Main sudo turn on.
    got = false;
    client.request(pubkey, "public.sudo", [&](auto success_, auto data_) { success = success_; data = data_; got = true; });
    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got );
        REQUIRE( success );
        REQUIRE( data == dvec{{"making sandwiches"}} );
    }

    got = false;
    client.request(pubkey, "sandwich.make", [&](auto success_, auto data_) {
            success = success_;
            data = std::move(data_);
            got = true;
    });
    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got );
        REQUIRE( success );
        REQUIRE( data == dvec{{"okay"}} );
    }

    // Take off every 'SUDO', You [not] know what you doing
    got = false;
    client.request(pubkey, "public.nosudo", [&](auto success_, auto data_) { success = success_; data = data_; got = true; });
    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got );
        REQUIRE( success );
        REQUIRE( data == dvec{{"make them yourself"}} );
    }

    got = false;
    client.request(pubkey, "sandwich.make", [&](auto success_, auto data_) {
            success = success_;
            data = std::move(data_);
            got = true;
    });
    reply_sleep();
    {
        auto lock = catch_lock();
        REQUIRE( got );
        REQUIRE_FALSE( success );
        REQUIRE( data == dvec{{"FORBIDDEN_SN"}} );
    }
}
