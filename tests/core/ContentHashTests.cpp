#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

#include "core/ContentHash.h"

using morpheus::core::ContentHash;

TEST_CASE("ContentHash matches FNV-1a 64 reference values", "[hash]")
{
    // Reference vectors from the FNV specification.
    CHECK(ContentHash().Hex() == "cbf29ce484222325");

    ContentHash a;
    a.Add("a");
    CHECK(a.Hex() == "af63dc4c8601ec8c");

    ContentHash foobar;
    foobar.Add("foobar");
    CHECK(foobar.Hex() == "85944171f73967e8");
}

TEST_CASE("ContentHash is streaming: chunks hash like their concatenation", "[hash]")
{
    ContentHash chunked;
    chunked.Add("foo");
    chunked.Add("bar");

    ContentHash whole;
    whole.Add("foobar");

    CHECK(chunked.Value() == whole.Value());
}

TEST_CASE("ContentHash handles high-bit bytes as unsigned", "[hash]")
{
    // FNV-1a 64 of the single byte 0xFF (independently computed: (offset ^ 0xff) * prime mod 2^64).
    ContentHash hash;
    hash.Add(std::string_view("\xff", 1));
    CHECK(hash.Hex() == "af64724c8602eb6e");
}

TEST_CASE("ContentHash ignores an empty Add", "[hash]")
{
    ContentHash hash;
    hash.Add("foobar");
    const std::uint64_t before = hash.Value();
    hash.Add("");
    CHECK(hash.Value() == before);
}
