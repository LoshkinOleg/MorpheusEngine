#include "core/Text.h"

namespace morpheus::core {

std::string PathText(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string ShortJson(const nlohmann::json& value)
{
    std::string text = value.dump(-1, ' ', true);
    if (text.size() > SHORT_JSON_CHARS) {
        text.resize(SHORT_JSON_CHARS);
        text += "...";
    }
    return text;
}

std::string CutUtf8(std::string_view text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes) {
        return std::string(text);
    }
    // Back off while the first byte left out is a continuation byte (10xxxxxx): the cut is inside a character.
    std::size_t length = maxBytes;
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
        --length;
    }
    return std::string(text.substr(0, length));
}

std::string Trim(std::string_view text)
{
    constexpr std::string_view WHITESPACE = " \t\r\n";
    const std::size_t first = text.find_first_not_of(WHITESPACE);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(WHITESPACE);
    return std::string(text.substr(first, last - first + 1));
}

} // namespace morpheus::core
