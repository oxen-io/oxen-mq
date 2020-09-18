#include <catch2/catch.hpp>

#include <lokimq/bt_serialize.h>

using namespace lokimq;

TEST_CASE("fixed buffer bt serialization", "[bencode][serialization]") {
    char buffer[1000];

    {
        fixed_buffer_producer test(buffer, buffer + 999);

        REQUIRE_NOTHROW(test.start_list());
        REQUIRE_NOTHROW(test.start_dict());

        REQUIRE_NOTHROW(test.append_integer(1337, "leet"));
        REQUIRE_NOTHROW(test.end_dict());
        REQUIRE_NOTHROW(test.end_list());

        std::string_view buffer_view(buffer, test.size());

        REQUIRE(buffer_view == "ld4:leeti1337eee");
    }

    {
        fixed_buffer_producer test(buffer, buffer + 50);

        REQUIRE_NOTHROW(test.start_list());
        REQUIRE_NOTHROW(test.start_dict());

        REQUIRE_NOTHROW(test.append_string("foobarbazyooooo", "thisisthekey"));

        REQUIRE_NOTHROW(test.end_dict());
        REQUIRE_NOTHROW(test.end_list());

        std::string_view buffer_view(buffer, test.size());

        REQUIRE(buffer_view == "ld12:thisisthekey15:foobarbazyoooooee");
    }

    {
        fixed_buffer_producer test(buffer, buffer + 20);

        REQUIRE_NOTHROW(test.start_list());
        REQUIRE_NOTHROW(test.start_dict());

        REQUIRE_THROWS_AS(test.append_string("foobarbazyooooo", "thisisthekey"), lokimq::bt_serialize_length_error);
    }
}
