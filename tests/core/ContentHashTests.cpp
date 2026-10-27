#include <catch2/catch_test_macros.hpp>

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
