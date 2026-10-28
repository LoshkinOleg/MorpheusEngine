#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "core/Text.h"

using namespace morpheus;

TEST_CASE("PathText gives UTF-8 with forward slashes", "[text]")
{
    const std::filesystem::path path = std::filesystem::path(u8"saves") / u8"caf\u00e9" / u8"run.db";
    CHECK(core::PathText(path) == "saves/caf\xC3\xA9/run.db");
}

TEST_CASE("ShortJson is compact ASCII cut at SHORT_JSON_CHARS", "[text]")
{
    CHECK(core::ShortJson(nlohmann::json{{"a", 1}}) == R"({"a":1})");
    CHECK(core::ShortJson(nlohmann::json("caf\xC3\xA9")) == R"("caf\u00e9")");
    const std::string cut = core::ShortJson(nlohmann::json(std::string(500, 'x')));
    CHECK(cut.size() == core::SHORT_JSON_CHARS + 3);
    CHECK(cut.ends_with("..."));
}

TEST_CASE("CutUtf8 never splits a character", "[text]")
{
    CHECK(core::CutUtf8("abc", 10) == "abc");
    CHECK(core::CutUtf8("abcdef", 3) == "abc");
    CHECK(core::CutUtf8("caf\xC3\xA9", 4) == "caf"); // The 2-byte character does not fit whole.
    CHECK(core::CutUtf8("caf\xC3\xA9", 5) == "caf\xC3\xA9");
}

TEST_CASE("Trim removes surrounding ASCII whitespace", "[text]")
{
    CHECK(core::Trim("  a b \r\n\t") == "a b");
    CHECK(core::Trim(" \n ").empty());
}
