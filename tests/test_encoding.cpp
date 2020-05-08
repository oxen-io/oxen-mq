#include "lokimq/hex.h"
#include <iostream>
#include "lokimq/base32z.h"
#include "common.h"

using namespace std::literals;

TEST_CASE("hex encoding/decoding", "[encoding][decoding][hex]") {
    REQUIRE( lokimq::to_hex("\xff\x42\x12\x34") == "ff421234"s );
    REQUIRE( lokimq::from_hex("12345678ffEDbca9") == "\x12\x34\x56\x78\xff\xed\xbc\xa9"s );
    REQUIRE( lokimq::is_hex("1234567890abcdefABCDEF1234567890abcdefABCDEF") );
    REQUIRE_FALSE( lokimq::is_hex("1234567890abcdefABCDEF1234567890aGcdefABCDEF") );
    REQUIRE_FALSE( lokimq::is_hex("1234567890abcdefABCDEF1234567890agcdefABCDEF") );
    REQUIRE_FALSE( lokimq::is_hex("\x11\xff") );
}

TEST_CASE("base32z encoding/decoding", "[encoding][decoding][base32z]") {
    REQUIRE( lokimq::to_base32z("\0\0\0\0\0"s) == "yyyyyyyy" );
    REQUIRE( lokimq::to_base32z("\x01\x23\x45\x67\x89\xab\xcd\xef\x01\x23\x45\x67\x89\xab\xcd\xef\x01\x23\x45\x67\x89\xab\xcd\xef\x01\x23\x45\x67\x89\xab\xcd\xef"_sv)
            == "yrtwk3hjixg66yjdeiuauk6p7hy1gtm8tgih55abrpnsxnpm3zzo");

    REQUIRE( lokimq::from_base32z("yrtwk3hjixg66yjdeiuauk6p7hy1gtm8tgih55abrpnsxnpm3zzo")
            == "\x01\x23\x45\x67\x89\xab\xcd\xef\x01\x23\x45\x67\x89\xab\xcd\xef\x01\x23\x45\x67\x89\xab\xcd\xef\x01\x23\x45\x67\x89\xab\xcd\xef"_sv);

    REQUIRE( lokimq::from_base32z("YRTWK3HJIXG66YJDEIUAUK6P7HY1GTM8TGIH55ABRPNSXNPM3ZZO")
            == "\x01\x23\x45\x67\x89\xab\xcd\xef\x01\x23\x45\x67\x89\xab\xcd\xef\x01\x23\x45\x67\x89\xab\xcd\xef\x01\x23\x45\x67\x89\xab\xcd\xef"_sv);

    auto five_nulls = lokimq::from_base32z("yyyyyyyy");
    REQUIRE( five_nulls.size() == 5 );
    REQUIRE( five_nulls == "\0\0\0\0\0"s );

    // 00000 00001 00010 00011 00100 00101 00110 00111
    // ==
    // 00000000 01000100 00110010 00010100 11000111
    REQUIRE( lokimq::from_base32z("ybndrfg8") == "\x00\x44\x32\x14\xc7"s );

    // Special case 1: 7 base32z digits with 3 trailing 0 bits -> 4 bytes (the trailing 0s are dropped)
    // 00000 00001 00010 00011 00100 00101 11000
    // ==
    // 00000000 01000100 00110010 00010111
    REQUIRE( lokimq::from_base32z("ybndrfa") == "\x00\x44\x32\x17"s );

    // Round-trip it:
    REQUIRE( lokimq::from_base32z(lokimq::to_base32z("\x00\x44\x32\x17"_sv)) == "\x00\x44\x32\x17"_sv );
    REQUIRE( lokimq::to_base32z(lokimq::from_base32z("ybndrfa")) == "ybndrfa" );

    // Special case 2: 7 base32z digits with 3 trailing bits 010; we just ignore the trailing stuff,
    // as if it was specified as 0.  (The last digit here is 11010 instead of 11000).
    REQUIRE( lokimq::from_base32z("ybndrf4") == "\x00\x44\x32\x17"s );
    // This one won't round-trip to the same value since it has ignored garbage bytes at the end
    REQUIRE( lokimq::to_base32z(lokimq::from_base32z("ybndrf4"s)) == "ybndrfa" );
}
