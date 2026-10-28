#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace morpheus::core {

// A path as UTF-8 with '/' separators, for messages and for C APIs that take UTF-8. path::string() and
// generic_string() convert through the ANSI code page on Windows, which mangles non-ASCII folder names.
std::string PathText(const std::filesystem::path& path);

// Longest JSON text ShortJson keeps; longer text is cut and ends with "...".
constexpr std::size_t SHORT_JSON_CHARS = 80;

// `value` as compact ASCII JSON (non-ASCII is escaped, so a cut never splits a character), cut to SHORT_JSON_CHARS.
// For rejection reasons, which reach the narrate prompt: a huge proposed value must not flood it.
std::string ShortJson(const nlohmann::json& value);

// The longest prefix of `text` of at most maxBytes bytes that ends on a UTF-8 character boundary.
std::string CutUtf8(std::string_view text, std::size_t maxBytes);

// `text` without leading and trailing spaces, tabs, carriage returns and newlines.
std::string Trim(std::string_view text);

} // namespace morpheus::core
