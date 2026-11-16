#include "turn/PromptPack.h"

#include <algorithm>
#include <cstddef>
#include <format>

#include "core/Errors.h"

namespace morpheus::turn {

namespace {

// The pack's members, in PROMPT_TEXTS' order: index i holds the text whose key is PROMPT_TEXTS[i].key.
constexpr std::array<std::string PromptPack::*, 8> MEMBERS = {
    &PromptPack::system, &PromptPack::resolve,   &PromptPack::speak,    &PromptPack::turnCue,
    &PromptPack::decide, &PromptPack::summarize, &PromptPack::condense,  &PromptPack::review};

// The length of the variable that starts at `at` in `text` ("{", one or more of a-z and _, "}"), or 0 when none does.
std::size_t VariableLength(std::string_view text, std::size_t at)
{
    if (text[at] != '{') {
        return 0;
    }
    std::size_t end = at + 1;
    while (end < text.size() && ((text[end] >= 'a' && text[end] <= 'z') || text[end] == '_')) {
        ++end;
    }
    // At least one name character, then the closing brace.
    return end > at + 1 && end < text.size() && text[end] == '}' ? end - at + 1 : 0;
}

} // namespace

PromptPack PromptPack::FromHost(const std::map<std::string, std::string>& texts)
{
    // 1. Every key the host sent names a text of the pack. The map gives the keys in order.
    for (const auto& entry : texts) {
        const std::string& key = entry.first;
        const bool known = std::any_of(PROMPT_TEXTS.begin(), PROMPT_TEXTS.end(),
                                       [&key](const PromptText& candidate) { return candidate.key == key; });
        if (!known) {
            std::string keys;
            for (const PromptText& text : PROMPT_TEXTS) {
                keys += std::format("{}{}", keys.empty() ? "" : ", ", text.key);
            }
            throw core::DataError(std::format("prompts/{}: unknown text; the pack's texts are {}", key, keys));
        }
    }

    // 2. Each sent text, in the table's order: not empty, and naming only the variables it takes. It then replaces the
    // default.
    PromptPack pack;
    for (std::size_t i = 0; i < PROMPT_TEXTS.size(); ++i) {
        const PromptText& entry = PROMPT_TEXTS[i];
        const auto it = texts.find(std::string(entry.key));
        if (it == texts.end()) {
            continue;
        }
        const std::string& text = it->second;
        if (text.empty()) {
            throw core::DataError(std::format("prompts/{}: empty", entry.key));
        }
        for (std::size_t at = 0; at < text.size(); ++at) {
            const std::size_t length = VariableLength(text, at);
            if (length == 0) {
                continue;
            }
            const std::string_view name = std::string_view(text).substr(at + 1, length - 2);
            if (std::find(entry.variables.begin(), entry.variables.end(), name) == entry.variables.end()) {
                // What the text takes, as the message lists it.
                std::string takes;
                for (const std::string_view variable : entry.variables) {
                    if (!variable.empty()) {
                        takes += std::format("{}{{{}}}", takes.empty() ? "" : ", ", variable);
                    }
                }
                throw core::DataError(std::format("prompts/{}: unknown variable {{{}}}; this text takes {}", entry.key,
                                                  name, takes.empty() ? "no variable" : takes));
            }
            at += length - 1; // Past the variable; the loop's ++at steps over its closing brace.
        }
        pack.*MEMBERS[i] = text;
    }
    return pack;
}

std::string PromptPack::Fill(std::string_view text, const PromptVariables& variables)
{
    // Left to right over `text`: a variable becomes its value, anything else is copied. A value is never scanned, so a
    // name holding a variable is put in as written.
    std::string filled;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t length = VariableLength(text, at);
        if (length == 0) {
            filled += text[at];
            ++at;
            continue;
        }
        const std::string_view name = text.substr(at + 1, length - 2);
        const std::string* pValue = name == "speaker" ? &variables.speaker
                                    : name == "player" ? &variables.player
                                    : name == "actor"  ? &variables.actor
                                                       : nullptr;
        MORPHEUS_REQUIRE(pValue != nullptr,
                         std::format("PromptPack::Fill: {{{}}} is not a variable the engine fills", name));
        MORPHEUS_REQUIRE(!pValue->empty(), std::format("PromptPack::Fill: no value for {{{}}}", name));
        filled += *pValue;
        at += length;
    }
    return filled;
}

const std::string& PromptPack::Text(std::string_view key) const
{
    // The member at the key's place in PROMPT_TEXTS.
    for (std::size_t i = 0; i < PROMPT_TEXTS.size(); ++i) {
        if (PROMPT_TEXTS[i].key == key) {
            return this->*MEMBERS[i];
        }
    }
    throw core::ContractError(std::format("PromptPack::Text: '{}' is not a text of the pack", key));
}

} // namespace morpheus::turn
