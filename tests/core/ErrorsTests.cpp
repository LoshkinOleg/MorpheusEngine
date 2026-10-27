#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Errors.h"

using Catch::Matchers::ContainsSubstring;

TEST_CASE("MORPHEUS_REQUIRE reports the source location", "[errors]")
{
    CHECK_THROWS_WITH([] { MORPHEUS_REQUIRE(false, "broken contract"); }(), ContainsSubstring("broken contract [") &&
                                                                              ContainsSubstring("ErrorsTests.cpp:"));
}
