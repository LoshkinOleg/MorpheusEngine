#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include "core/Csv.h"
#include "core/Errors.h"

using morpheus::core::DataError;
using morpheus::core::ParseCsv;
using Catch::Matchers::ContainsSubstring;
using Rows = std::vector<std::vector<std::string>>;

TEST_CASE("ParseCsv splits plain records and fields", "[csv]")
{
    CHECK(ParseCsv("a,b\nc,d\n") == Rows{{"a", "b"}, {"c", "d"}});
}

TEST_CASE("ParseCsv accepts CRLF endings and a missing final newline", "[csv]")
{
    CHECK(ParseCsv("a,b\r\nc,d") == Rows{{"a", "b"}, {"c", "d"}});
}

TEST_CASE("ParseCsv keeps empty fields", "[csv]")
{
    CHECK(ParseCsv("a,,c\n,\n") == Rows{{"a", "", "c"}, {"", ""}});
}

TEST_CASE("ParseCsv handles quoted fields", "[csv]")
{
    // Commas, doubled quotes and newlines inside quotes all belong to the field.
    CHECK(ParseCsv("\"x, y\",\"say \"\"hi\"\"\"\n\"line1\nline2\",z\n") ==
          Rows{{"x, y", "say \"hi\""}, {"line1\nline2", "z"}});
}

TEST_CASE("ParseCsv skips blank lines", "[csv]")
{
    CHECK(ParseCsv("a\n\n\r\nb\n") == Rows{{"a"}, {"b"}});
}

TEST_CASE("ParseCsv rejects malformed quoting with a line number", "[csv]")
{
    CHECK_THROWS_AS(ParseCsv("\"unterminated\n"), DataError);
    CHECK_THROWS_WITH(ParseCsv("a,b\nc\"d,e\n"), ContainsSubstring("line 2"));
    CHECK_THROWS_AS(ParseCsv("\"quoted\"tail\n"), DataError);
    CHECK_THROWS_AS(ParseCsv("a\rb\n"), DataError);
}
