#include <catch2/catch.hpp>
#include "lokimq/string_view.h"
#include <future>

using namespace lokimq;

using namespace std::literals;

TEST_CASE("string view", "[string_view]") {
    std::string foo = "abc 123 xyz";
    string_view f1{foo};
    string_view f2{"def 789 uvw"};
    string_view f3{"nu\0ll", 5};

    REQUIRE( f1 == "abc 123 xyz" );
    REQUIRE( f2 == "def 789 uvw" );
    REQUIRE( f3.size() == 5 );
    REQUIRE( f3 == std::string{"nu\0ll", 5} );
    REQUIRE( f3 != "nu" );
    REQUIRE( f3.data() == "nu"s );
    REQUIRE( string_view(f3) == f3 );

    auto f4 = f3;
    REQUIRE( f4 == f3 );
    f4 = f2;
    REQUIRE( f4 == "def 789 uvw" );

    REQUIRE( f1.size() == 11 );
    REQUIRE( f3.length() == 5 );

    string_view f5{""};
    REQUIRE( !f3.empty() );
    REQUIRE( f5.empty() );

    REQUIRE( f1[5] == '2' );
    size_t i = 0;
    for (auto c : f3)
        REQUIRE(c == f3[i++]);

    std::string backwards;
    for (auto it = std::rbegin(f2); it != f2.crend(); ++it)
        backwards += *it;

    REQUIRE( backwards == "wvu 987 fed" );

    REQUIRE( f1.at(10) == 'z' );
    REQUIRE_THROWS_AS( f1.at(15), std::out_of_range );
    REQUIRE_THROWS_AS( f1.at(11), std::out_of_range );

    f4 = f1;
    f4.remove_prefix(2);
    REQUIRE( f4 == "c 123 xyz" );
    f4.remove_prefix(2);
    f4.remove_suffix(4);
    REQUIRE( f4 == "123" );
    f4.remove_prefix(1);
    REQUIRE( f4 == "23" );
    REQUIRE( f1 == "abc 123 xyz" );
    f4.swap(f1);
    REQUIRE( f1 == "23" );
    REQUIRE( f4 == "abc 123 xyz" );
    f1.remove_suffix(2);
    REQUIRE( f1.empty() );
    REQUIRE( f4 == "abc 123 xyz" );
    f1.swap(f4);
    REQUIRE( f4.empty() );
    REQUIRE( f1 == "abc 123 xyz" );

    REQUIRE( f1.front() == 'a' );
    REQUIRE( f1.back() == 'z' );
    REQUIRE( f1.compare("abc") > 0 );
    REQUIRE( f1.compare("abd") < 0 );
    REQUIRE( f1.compare("abc 123 xyz") == 0 );
    REQUIRE( f1.compare("abc 123 xyza") < 0 );
    REQUIRE( f1.compare("abc 123 xy") > 0 );

    std::string buf;
    buf.resize(5);
    f1.copy(&buf[0], 5, 2);
    REQUIRE( buf == "c 123" );
    buf.resize(100, 'X');
    REQUIRE( f1.copy(&buf[0], 100) == 11 );
    REQUIRE( buf.substr(0, 11) == f1 );
    REQUIRE( buf.substr(11) == std::string(89, 'X') );
    REQUIRE( f1.substr(4) == "123 xyz" );
    REQUIRE( f1.substr(4, 3) == "123" );
    REQUIRE_THROWS_AS( f1.substr(500, 3), std::out_of_range );
    REQUIRE( f1.substr(11, 2) == "" );
    REQUIRE( f1.substr(8, 500) == "xyz" );
    REQUIRE( f1.find("123") == 4 );
    REQUIRE( f1.find("abc") == 0 );
    REQUIRE( f1.find("xyz") == 8 );
    REQUIRE( f1.find("abc 123 xyz 7") == string_view::npos );
    REQUIRE( f1.find("23") == 5 );
    REQUIRE( f1.find("234") == string_view::npos );

    string_view f6{"zz abc abcd abcde abcdef"};
    REQUIRE( f6.find("abc") == 3 );
    REQUIRE( f6.find("abc", 3) == 3 );
    REQUIRE( f6.find("abc", 4) == 7 );
    REQUIRE( f6.find("abc", 7) == 7 );
    REQUIRE( f6.find("abc", 8) == 12 );
    REQUIRE( f6.find("abc", 18) == 18 );
    REQUIRE( f6.find("abc", 19) == string_view::npos );
    REQUIRE( f6.find("abcd") == 7 );
    REQUIRE( f6.rfind("abc") == 18 );
    REQUIRE( f6.rfind("abcd") == 18 );
    REQUIRE( f6.rfind("bcd") == 19 );
    REQUIRE( f6.rfind("abc", 19) == 18 );
    REQUIRE( f6.rfind("abc", 18) == 18 );
    REQUIRE( f6.rfind("abc", 17) == 12 );
    REQUIRE( f6.rfind("abc", 17) == 12 );
    REQUIRE( f6.rfind("abc", 8) == 7 );
    REQUIRE( f6.rfind("abc", 7) == 7 );
    REQUIRE( f6.rfind("abc", 6) == 3 );
    REQUIRE( f6.rfind("abc", 3) == 3 );
    REQUIRE( f6.rfind("abc", 2) == string_view::npos );

    REQUIRE( f6.find('a') == 3 );
    REQUIRE( f6.find('a', 17) == 18 );
    REQUIRE( f6.find('a', 20) == string_view::npos );

    REQUIRE( f6.rfind('a') == 18 );
    REQUIRE( f6.rfind('a', 17) == 12 );
    REQUIRE( f6.rfind('a', 2) == string_view::npos );

    string_view f7{"abc\0def", 7};
    REQUIRE( f7.find("c\0d", 0, 3) == 2 );
    REQUIRE( f7.find("c\0e", 0, 3) == string_view::npos );
    REQUIRE( f7.rfind("c\0d", string_view::npos, 3) == 2 );
    REQUIRE( f7.rfind("c\0e", 0, 3) == string_view::npos );

    REQUIRE( f6.find_first_of("c789b") == 4 );
    REQUIRE( f6.find_first_of("c789") == 5 );
    REQUIRE( f2.find_first_of("c789b") == 4 );
    REQUIRE( f6.find_first_of("c789b", 6) == 8 );

    REQUIRE( f6.find_last_of("c789b") == 20 );
    REQUIRE( f6.find_last_of("789b") == 19 );
    REQUIRE( f2.find_last_of("c789b") == 6 );
    REQUIRE( f6.find_last_of("c789b", 6) == 5 );
    REQUIRE( f6.find_last_of("c789b", 5) == 5 );
    REQUIRE( f6.find_last_of("c789b", 4) == 4 );
    REQUIRE( f6.find_last_of("c789b", 3) == string_view::npos );

    REQUIRE( f2.find_first_of(f7) == 0 );
    REQUIRE( f3.find_first_of(f7) == 2 );
    REQUIRE( f3.find_first_of('\0') == 2 );
    REQUIRE( f3.find_first_of("jk\0", 0, 3) == 2 );

    REQUIRE( f1.find_first_not_of("abc") == 3 );
    REQUIRE( f1.find_first_not_of("abc ", 3) == 4 );
    REQUIRE( f1.find_first_not_of(" 123", 3) == 8 );
    REQUIRE( f1.find_last_not_of("abc") == 10 );
    REQUIRE( f1.find_last_not_of("xyz") == 7 );
    REQUIRE( f1.find_last_not_of("xyz 321") == 2 );
    REQUIRE( f1.find_last_not_of("xay z1b2c3") == string_view::npos );
    REQUIRE( f6.find_last_not_of("def") == 20 );
    REQUIRE( f6.find_last_not_of("abcdef") == 17 );
    REQUIRE( f6.find_last_not_of("abcdef ") == 1 );
    REQUIRE( f6.find_first_not_of('z') == 2 );
    REQUIRE( f6.find_first_not_of("z ") == 3 );
    REQUIRE( f6.find_first_not_of("a ", 2) == 4 );
    REQUIRE( f6.find_last_not_of("abc ", 9) == 1 );

    REQUIRE( string_view{"abc"} == string_view{"abc"} );
    REQUIRE_FALSE( string_view{"abc"} == string_view{"abd"} );
    REQUIRE_FALSE( string_view{"abc"} == string_view{"abcd"} );
    REQUIRE( string_view{"abc"} != string_view{"abd"} );
    REQUIRE( string_view{"abc"} != string_view{"abcd"} );
    REQUIRE( string_view{"abc"} < string_view{"abcd"} );
    REQUIRE( string_view{"abc"} < string_view{"abd"} );
    REQUIRE( string_view{"abd"} > string_view{"abc"} );
    REQUIRE( string_view{"abcd"} > string_view{"abc"} );
    REQUIRE( string_view{"abc"} <= string_view{"abcd"} );
    REQUIRE( string_view{"abc"} <= string_view{"abc"} );
    REQUIRE( string_view{"abc"} <= string_view{"abd"} );
    REQUIRE( string_view{"abd"} >= string_view{"abc"} );
    REQUIRE( string_view{"abc"} >= string_view{"abc"} );
    REQUIRE( string_view{"abcd"} >= string_view{"abc"} );
}
