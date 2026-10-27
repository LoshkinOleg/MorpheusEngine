#include <stdexcept>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestPaths.h"

using Catch::Matchers::ContainsSubstring;

TEST_CASE("WriteFile throws when the file cannot be written", "[testpaths]")
{
    const auto dir = morpheus::test::FreshTempDir("write_file_failure");
    const auto file = dir / "missing_folder" / "x.txt";
    CHECK_THROWS_AS(morpheus::test::WriteFile(file, "text"), std::runtime_error);
    CHECK_THROWS_WITH(morpheus::test::WriteFile(file, "text"), ContainsSubstring("x.txt"));
}
