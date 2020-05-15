#include "lokimq/bt_serialize.h"
#include "common.h"
#include <map>
#include <set>
#include <limits>

TEST_CASE("bt basic value serialization", "[bt][serialization]") {
    int x = 42;
    std::string x_ = bt_serialize(x);
    REQUIRE( bt_serialize(x) == "i42e" );

    int64_t  ibig = -8'000'000'000'000'000'000LL;
    uint64_t ubig = 10'000'000'000'000'000'000ULL;
    REQUIRE( bt_serialize(ibig) == "i-8000000000000000000e" );
    REQUIRE( bt_serialize(std::numeric_limits<int64_t>::min()) == "i-9223372036854775808e" );
    REQUIRE( bt_serialize(ubig) == "i10000000000000000000e" );
    REQUIRE( bt_serialize(std::numeric_limits<uint64_t>::max()) == "i18446744073709551615e" );

    std::unordered_map<std::string, int> m;
    m["hi"] = 123;
    m["omg"] = -7890;
    m["bye"] = 456;
    m["zap"] = 0;
    // bt values are always sorted:
    REQUIRE( bt_serialize(m) == "d3:byei456e2:hii123e3:omgi-7890e3:zapi0ee" );

    // Dict-like list serializes as a dict (and get sorted, as above)
    std::list<std::pair<std::string, std::string>> d{{
        {"c", "x"},
        {"a", "z"},
        {"b", "y"},
    }};
    REQUIRE( bt_serialize(d) == "d1:a1:z1:b1:y1:c1:xe" );

    std::vector<std::string> v{{"a", "", "\x00"s, "\x00\x00\x00goo"s}};
    REQUIRE( bt_serialize(v) == "l1:a0:1:\0006:\x00\x00\x00gooe"sv );

    std::array v2 = {"a"sv, ""sv, "\x00"sv, "\x00\x00\x00goo"sv};
    REQUIRE( bt_serialize(v2) == "l1:a0:1:\0006:\x00\x00\x00gooe"sv );
}

TEST_CASE("bt nested value serialization", "[bt][serialization]") {
    std::unordered_map<std::string, std::list<std::map<std::string, std::set<int>>>> x{{
        {"foo", {{{"a", {1,2,3}}, {"b", {}}}, {{"c", {4,-5}}}}},
        {"bar", {}}
    }};
    REQUIRE( bt_serialize(x) == "d3:barle3:foold1:ali1ei2ei3ee1:bleed1:cli-5ei4eeeee" );
}

TEST_CASE("bt basic value deserialization", "[bt][deserialization]") {
    REQUIRE( bt_deserialize<int>("i42e") == 42 );

    int64_t  ibig = -8'000'000'000'000'000'000LL;
    uint64_t ubig = 10'000'000'000'000'000'000ULL;
    REQUIRE( bt_deserialize<int64_t>("i-8000000000000000000e") == ibig );
    REQUIRE( bt_deserialize<uint64_t>("i10000000000000000000e") == ubig );
    REQUIRE( bt_deserialize<int64_t>("i-9223372036854775808e") == std::numeric_limits<int64_t>::min() );
    REQUIRE( bt_deserialize<uint64_t>("i18446744073709551615e") == std::numeric_limits<uint64_t>::max() );
    REQUIRE( bt_deserialize<uint32_t>("i4294967295e") == std::numeric_limits<uint32_t>::max() );

    REQUIRE_THROWS( bt_deserialize<int64_t>("i-9223372036854775809e") );
    REQUIRE_THROWS( bt_deserialize<uint64_t>("i-1e") );
    REQUIRE_THROWS( bt_deserialize<uint32_t>("i4294967296e") );

    std::unordered_map<std::string, int> m;
    m["hi"] = 123;
    m["omg"] = -7890;
    m["bye"] = 456;
    m["zap"] = 0;
    // bt values are always sorted:
    REQUIRE( bt_deserialize<std::unordered_map<std::string, int>>("d3:byei456e2:hii123e3:omgi-7890e3:zapi0ee") == m );

    // Dict-like list can be used for deserialization
    std::list<std::pair<std::string, std::string>> d{{
        {"a", "z"},
        {"b", "y"},
        {"c", "x"},
    }};
    REQUIRE( bt_deserialize<std::list<std::pair<std::string, std::string>>>("d1:a1:z1:b1:y1:c1:xe") == d );

    std::vector<std::string> v{{"a", "", "\x00"s, "\x00\x00\x00goo"s}};
    REQUIRE( bt_deserialize<std::vector<std::string>>("l1:a0:1:\0006:\x00\x00\x00gooe"sv) == v );

    std::vector v2 = {"a"sv, ""sv, "\x00"sv, "\x00\x00\x00goo"sv};
    REQUIRE( bt_deserialize<decltype(v2)>("l1:a0:1:\0006:\x00\x00\x00gooe"sv) == v2 );
}

TEST_CASE("bt_value serialization", "[bt][serialization][bt_value]") {
    bt_value dna{42};
    std::string x_ = bt_serialize(dna);
    REQUIRE( bt_serialize(dna) == "i42e" );

    bt_value ibig{-8'000'000'000'000'000'000LL};
    bt_value ubig{10'000'000'000'000'000'000ULL};
    int16_t ismall = -123;
    uint16_t usmall = 123;
    bt_dict nums{
        {"a", 0},
        {"b", -8'000'000'000'000'000'000LL},
        {"c", 10'000'000'000'000'000'000ULL},
        {"d", ismall},
        {"e", usmall},
    };

    REQUIRE( bt_serialize(ibig) == "i-8000000000000000000e" );
    REQUIRE( bt_serialize(ubig) == "i10000000000000000000e" );
    REQUIRE( bt_serialize(nums) == "d1:ai0e1:bi-8000000000000000000e1:ci10000000000000000000e1:di-123e1:ei123ee" );

    // Same as nested test, above, but with bt_* types
    bt_dict x{{
        {"foo", bt_list{{bt_dict{{ {"a", bt_list{{1,2,3}}}, {"b", bt_list{}}}}, bt_dict{{{"c", bt_list{{-5, 4}}}}}}}},
        {"bar", bt_list{}}
    }};
    REQUIRE( bt_serialize(x) == "d3:barle3:foold1:ali1ei2ei3ee1:bleed1:cli-5ei4eeeee" );
    std::vector<std::string> v{{"a", "", "\x00"s, "\x00\x00\x00goo"s}};
    REQUIRE( bt_serialize(v) == "l1:a0:1:\0006:\x00\x00\x00gooe"sv );

    std::array v2 = {"a"sv, ""sv, "\x00"sv, "\x00\x00\x00goo"sv};
    REQUIRE( bt_serialize(v2) == "l1:a0:1:\0006:\x00\x00\x00gooe"sv );
}

TEST_CASE("bt_value deserialization", "[bt][deserialization][bt_value]") {
    auto dna1 = bt_deserialize<bt_value>("i42e");
    auto dna2 = bt_deserialize<bt_value>("i-42e");
    REQUIRE( std::get<uint64_t>(dna1) == 42 );
    REQUIRE( std::get<int64_t>(dna2) == -42 );
    REQUIRE_THROWS( std::get<int64_t>(dna1) );
    REQUIRE_THROWS( std::get<uint64_t>(dna2) );
    REQUIRE( lokimq::get_int<int>(dna1) == 42 );
    REQUIRE( lokimq::get_int<int>(dna2) == -42 );
    REQUIRE( lokimq::get_int<unsigned>(dna1) == 42 );
    REQUIRE_THROWS( lokimq::get_int<unsigned>(dna2) );

    bt_value x = bt_deserialize<bt_value>("d3:barle3:foold1:ali1ei2ei3ee1:bleed1:cli-5ei4eeeee");
    REQUIRE( std::holds_alternative<bt_dict>(x) );
    bt_dict& a = std::get<bt_dict>(x);
    REQUIRE( a.count("bar") );
    REQUIRE( a.count("foo") );
    REQUIRE( a.size() == 2 );
    bt_list& foo = std::get<bt_list>(a["foo"]);
    REQUIRE( foo.size() == 2 );
    bt_dict& foo1 = std::get<bt_dict>(foo.front());
    bt_dict& foo2 = std::get<bt_dict>(foo.back());
    REQUIRE( foo1.size() == 2 );
    REQUIRE( foo2.size() == 1 );
    bt_list& foo1a = std::get<bt_list>(foo1.at("a"));
    bt_list& foo1b = std::get<bt_list>(foo1.at("b"));
    bt_list& foo2c = std::get<bt_list>(foo2.at("c"));
    std::list<int> foo1a_vals, foo1b_vals, foo2c_vals;
    for (auto& v : foo1a) foo1a_vals.push_back(lokimq::get_int<int>(v));
    for (auto& v : foo1b) foo1b_vals.push_back(lokimq::get_int<int>(v));
    for (auto& v : foo2c) foo2c_vals.push_back(lokimq::get_int<int>(v));
    REQUIRE( foo1a_vals == std::list{{1,2,3}} );
    REQUIRE( foo1b_vals == std::list<int>{} );
    REQUIRE( foo2c_vals == std::list{{-5, 4}} );

    REQUIRE( std::get<bt_list>(a.at("bar")).empty() );
}

#if 0
    {
        std::cout << "zomg consumption\n";
        bt_dict_consumer dc{zomg_};
        for (int i = 0; i < 5; i++)
            if (!dc.skip_until("b"))
                throw std::runtime_error("Couldn't find b, but I know it's there!");

        auto dc1 = dc;
        if (dc.skip_until("z")) {
            auto v = dc.consume_integer<int>();
            std::cout << "  - " << v.first << ": " << v.second << "\n";
        } else {
            std::cout << "  - no z (bad!)\n";
        }

        std::cout << "zomg (second pass)\n";
        for (auto &p : dc1.consume_dict().second) {
            std::cout << "  - " << p.first << " = (whatever)\n";
        }
        while (dc1) {
            auto v = dc1.consume_integer<int>();
            std::cout << "  - " << v.first << ": " << v.second << "\n";
        }
    }
#endif


