#include "lokimq/address.h"
#include "common.h"

const std::string pk = "\xf1\x6b\xa5\x59\x10\x39\xf0\x89\xb4\x2a\x83\x41\x75\x09\x30\x94\x07\x4d\x0d\x93\x7a\x79\xe5\x3e\x5c\xe7\x30\xf9\x46\xe1\x4b\x88";
const std::string pk_hex = "f16ba5591039f089b42a834175093094074d0d937a79e53e5ce730f946e14b88";
const std::string pk_HEX = "F16BA5591039F089B42A834175093094074D0D937A79E53E5CE730F946E14B88";
const std::string pk_b32z = "6fi4kseo88aeupbkopyzknjo1odw4dcuxjh6kx1hhhax1tzbjqry";
const std::string pk_B32Z = "6FI4KSEO88AEUPBKOPYZKNJO1ODW4DCUXJH6KX1HHHAX1TZBJQRY";
const std::string pk_b64 = "8WulWRA58Im0KoNBdQkwlAdNDZN6eeU+XOcw+UbhS4g"; // NB: padding '=' omitted

TEST_CASE("tcp addresses", "[address][tcp]") {
    address a{"tcp://1.2.3.4:5678"};
    REQUIRE( a.host == "1.2.3.4" );
    REQUIRE( a.port == 5678 );
    REQUIRE_FALSE( a.curve() );
    REQUIRE( a.tcp() );
    REQUIRE( a.zmq_address() == "tcp://1.2.3.4:5678" );
    REQUIRE( a.full_address() == "tcp://1.2.3.4:5678" );
    REQUIRE( a.qr_address() == "TCP://1.2.3.4:5678" );

    REQUIRE_THROWS_AS( address{"tcp://1:1:1"}, std::invalid_argument );
    REQUIRE_THROWS_AS( address{"tcpz://localhost:123"}, std::invalid_argument );
    REQUIRE_THROWS_AS( address{"tcp://abc"}, std::invalid_argument );
    REQUIRE_THROWS_AS( address{"tcpz://localhost:0"}, std::invalid_argument );
    REQUIRE_THROWS_AS( address{"tcpz://[::1:1080"}, std::invalid_argument );

    address b = address::tcp("example.com", 80);
    REQUIRE( b.host == "example.com" );
    REQUIRE( b.port == 80 );
    REQUIRE_FALSE( b.curve() );
    REQUIRE( b.tcp() );
    REQUIRE( b.zmq_address() == "tcp://example.com:80" );
    REQUIRE( b.full_address() == "tcp://example.com:80" );
    REQUIRE( b.qr_address() == "TCP://EXAMPLE.COM:80" );

    address c{"tcp://[::1]:1111"};
    REQUIRE( c.host == "[::1]" );
    REQUIRE( c.port == 1111 );
}

TEST_CASE("unix sockets", "[address][ipc]") {
    address a{"ipc:///path/to/foo"};
    REQUIRE( a.socket == "/path/to/foo" );
    REQUIRE_FALSE( a.curve() );
    REQUIRE_FALSE( a.tcp() );
    REQUIRE( a.zmq_address() == "ipc:///path/to/foo" );
    REQUIRE( a.full_address() == "ipc:///path/to/foo" );

    address b = address::ipc("../foo");
    REQUIRE( b.socket == "../foo" );
    REQUIRE_FALSE( b.curve() );
    REQUIRE_FALSE( b.tcp() );
    REQUIRE( b.zmq_address() == "ipc://../foo" );
    REQUIRE( b.full_address() == "ipc://../foo" );
}

TEST_CASE("pubkey formats", "[address][curve][pubkey]") {
    address a{"tcp+curve://a:1/" + pk_hex};
    address b{"curve://a:1/" + pk_b32z};
    address c{"curve://a:1/" + pk_b64};
    address d{"CURVE://A:1/" + pk_B32Z};
    REQUIRE( a.curve() );
    REQUIRE( a.host == "a" );
    REQUIRE( a.port == 1 );
    REQUIRE((b.curve() && c.curve() && d.curve()));
    REQUIRE( a.pubkey == pk );
    REQUIRE( b.pubkey == pk );
    REQUIRE( c.pubkey == pk );
    REQUIRE( d.pubkey == pk );

    address e{"ipc+curve://my.sock/" + pk_hex};
    address f{"ipc+curve://../my.sock/" + pk_b32z};
    address g{"ipc+curve:///my.sock/" + pk_B32Z};
    address h{"ipc+curve://./my.sock/" + pk_b64};
    REQUIRE( e.curve() );
    REQUIRE( e.ipc() );
    REQUIRE_FALSE( e.tcp() );
    REQUIRE((f.curve() && g.curve() && h.curve()));
    REQUIRE( e.socket == "my.sock" );
    REQUIRE( f.socket == "../my.sock" );
    REQUIRE( g.socket == "/my.sock" );
    REQUIRE( h.socket == "./my.sock" );
    REQUIRE( e.pubkey == pk );
    REQUIRE( f.pubkey == pk );
    REQUIRE( g.pubkey == pk );
    REQUIRE( h.pubkey == pk );

    REQUIRE( d.full_address(address::encoding::hex) == "curve://a:1/" + pk_hex );
    REQUIRE( c.full_address(address::encoding::base32z) == "curve://a:1/" + pk_b32z );
    REQUIRE( b.full_address(address::encoding::BASE32Z) == "curve://a:1/" + pk_B32Z );
    REQUIRE( a.full_address(address::encoding::base64) == "curve://a:1/" + pk_b64 );

    REQUIRE( h.full_address(address::encoding::hex) == "ipc+curve://./my.sock/" + pk_hex );
    REQUIRE( g.full_address(address::encoding::base32z) == "ipc+curve:///my.sock/" + pk_b32z );
    REQUIRE( f.full_address(address::encoding::BASE32Z) == "ipc+curve://../my.sock/" + pk_B32Z );
    REQUIRE( e.full_address(address::encoding::base64) == "ipc+curve://my.sock/" + pk_b64 );

    REQUIRE_THROWS_AS(address{"ipc+curve://my.sock/" + pk_hex.substr(0, 63)}, std::invalid_argument);
    REQUIRE_THROWS_AS(address{"ipc+curve://my.sock/" + pk_b32z.substr(0, 51)}, std::invalid_argument);
    REQUIRE_THROWS_AS(address{"ipc+curve://my.sock/" + pk_B32Z.substr(0, 51)}, std::invalid_argument);
    REQUIRE_THROWS_AS(address{"ipc+curve://my.sock/" + pk_b64.substr(0, 42)}, std::invalid_argument);
    REQUIRE_THROWS_AS(address{"ipc+curve://my.sock"}, std::invalid_argument);
    REQUIRE_THROWS_AS(address{"ipc+curve://my.sock/"}, std::invalid_argument);
}


TEST_CASE("tcp QR-code friendly addresses", "[address][tcp][qr]") {
    address a{"tcp://public.loki.foundation:12345"};
    address a_qr{"TCP://PUBLIC.LOKI.FOUNDATION:12345"};
    address b{"tcp://PUBLIC.LOKI.FOUNDATION:12345"};
    REQUIRE( a == a_qr );
    REQUIRE( a != b );
    REQUIRE( a.host == "public.loki.foundation" );
    REQUIRE( a.qr_address() == "TCP://PUBLIC.LOKI.FOUNDATION:12345" );

    address c = address::tcp_curve("public.loki.foundation", 12345, pk);
    REQUIRE( c.qr_address() == "CURVE://PUBLIC.LOKI.FOUNDATION:12345/" + pk_B32Z );
    REQUIRE( address{"CURVE://PUBLIC.LOKI.FOUNDATION:12345/" + pk_B32Z} == c );
    // We don't produce with upper-case hex, but we accept it:
    REQUIRE( address{"CURVE://PUBLIC.LOKI.FOUNDATION:12345/" + pk_HEX} == c );

    // lower case not permitted:                          ▾
    REQUIRE_THROWS_AS(address{"CURVE://PUBLIC.LOKI.FOUNDATiON:12345/" + pk_B32Z}, std::invalid_argument);
    // also only accept upper-base base32z and hex:
    REQUIRE_THROWS_AS(address{"CURVE://PUBLIC.LOKI.FOUNDATION:12345/" + pk_b32z}, std::invalid_argument);
    REQUIRE_THROWS_AS(address{"CURVE://PUBLIC.LOKI.FOUNDATION:12345/" + pk_hex}, std::invalid_argument);
    // don't accept base64 even if it's upper-case (because case-converting it changes the value)
    REQUIRE_THROWS_AS(address{"CURVE://PUBLIC.LOKI.FOUNDATION:12345/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}, std::invalid_argument);
    REQUIRE_THROWS_AS(address{"CURVE://PUBLIC.LOKI.FOUNDATION:12345/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="}, std::invalid_argument);
}

