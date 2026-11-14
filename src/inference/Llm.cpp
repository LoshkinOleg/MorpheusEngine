#include "inference/Llm.h"

#include <algorithm>
#include <format>

#include "core/Errors.h"

namespace morpheus::inference {

namespace {

// The first candidate for the marker that stands in for message contents while a chat template renders.
constexpr char CONTENT_MARKER_BYTE = '\x1E';

} // namespace

std::string RoleName(Role role)
{
    switch (role) {
    case Role::System:
        return "system";
    case Role::User:
        return "user";
    case Role::Assistant:
        return "assistant";
    }
    throw core::ContractError("RoleName: unhandled Role");
}

std::string ContentMarker(const Prompt& prompt)
{
    auto anyContentHas = [&prompt](const std::string& candidate) {
        return std::any_of(prompt.messages.begin(), prompt.messages.end(), [&candidate](const Message& message) {
            return message.content.find(candidate) != std::string::npos;
        });
    };
    std::string marker(1, CONTENT_MARKER_BYTE);
    for (int candidate = 1; anyContentHas(marker); ++candidate) {
        marker = std::format("{}{}{}", CONTENT_MARKER_BYTE, candidate, CONTENT_MARKER_BYTE);
    }
    return marker;
}

Prompt MarkContents(const Prompt& prompt, std::string_view marker)
{
    Prompt marked = prompt;
    for (Message& message : marked.messages) {
        message.content = std::string(marker);
    }
    return marked;
}

std::vector<std::string> TemplatePieces(std::string_view rendered, std::string_view marker, std::size_t contentCount)
{
    MORPHEUS_REQUIRE(!marker.empty(), "TemplatePieces: the marker is empty");
    // The text up to each marker is a piece; after the last marker, the rest is the last piece.
    std::vector<std::string> pieces;
    std::size_t start = 0;
    for (;;) {
        const std::size_t found = rendered.find(marker, start);
        pieces.emplace_back(rendered.substr(start, found == std::string_view::npos ? std::string_view::npos : found - start));
        if (found == std::string_view::npos) {
            break;
        }
        if (pieces.size() > contentCount) {
            throw InferenceError("the chat template repeats a message's content");
        }
        start = found + marker.size();
    }
    if (pieces.size() != contentCount + 1) {
        throw InferenceError("the chat template drops a message's content");
    }
    return pieces;
}

} // namespace morpheus::inference
