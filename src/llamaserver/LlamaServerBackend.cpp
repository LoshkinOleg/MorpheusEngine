#include "llamaserver/LlamaServerBackend.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // cpp-httplib includes windows.h: leave out what Morpheus never uses.
#endif
#endif
#include <httplib.h>

#include "core/Errors.h"

namespace morpheus::llamaserver {

using inference::InferenceError;

namespace {

// How long a request may take to connect, and to wait for the server's next bytes. A long prompt is read before the
// first piece of its answer arrives, so the read wait is generous.
constexpr int CONNECT_TIMEOUT_SECONDS = 5;
constexpr int READ_TIMEOUT_SECONDS = 600;
// How much of a server's answer an error message quotes, in bytes.
constexpr std::size_t QUOTED_ANSWER_BYTES = 300;
// A server URL is this, then a host with an optional port: no HTTPS and no path.
constexpr std::string_view URL_SCHEME = "http://";

// Gives `client` Morpheus's timeouts.
void SetTimeouts(httplib::Client& client)
{
    client.set_connection_timeout(CONNECT_TIMEOUT_SECONDS, 0);
    client.set_read_timeout(READ_TIMEOUT_SECONDS, 0);
}

// The start of a server's answer, for an error message.
std::string Quoted(std::string_view answer)
{
    return std::string(answer.substr(0, QUOTED_ANSWER_BYTES));
}

// `body` as JSON text. JSON needs UTF-8, so a text that is not UTF-8 cannot be sent.
std::string BodyText(const nlohmann::json& body)
{
    try {
        return body.dump();
    } catch (const nlohmann::json::type_error& error) {
        throw InferenceError(std::format("a text for llama-server is not UTF-8: {}", error.what()));
    }
}

// Sends GET `path` (no body) or POST `path` with a JSON body to `url`, and reads the JSON answer with `read`.
// Throws InferenceError naming the URL and path for an unreachable server, an HTTP error, an answer that is not JSON,
// or one `read` cannot read (a missing field, a wrong type).
template <typename Read>
auto Ask(const std::string& url, const std::string& path, const std::optional<nlohmann::json>& body, Read&& read)
{
    httplib::Client client(url);
    SetTimeouts(client);
    const httplib::Result response = body ? client.Post(path, BodyText(*body), "application/json") : client.Get(path);
    if (!response) {
        throw InferenceError(std::format("cannot reach {}{}: {}", url, path, httplib::to_string(response.error())));
    }
    if (response->status != 200) {
        throw InferenceError(std::format("{}{} answered HTTP {}: {}", url, path, response->status, Quoted(response->body)));
    }
    try {
        return read(nlohmann::json::parse(response->body));
    } catch (const nlohmann::json::exception& error) {
        throw InferenceError(
            std::format("{}{} gave an unexpected answer ({}): {}", url, path, error.what(), Quoted(response->body)));
    }
}

} // namespace

LlamaServerBackend::LlamaServerBackend(LlamaServerConfig config)
    : _config(std::move(config))
{
    MORPHEUS_REQUIRE(_config.contextSize > 0, "LlamaServerBackend: the context size must be positive");
    MORPHEUS_REQUIRE(_config.slot >= 0, "LlamaServerBackend: the slot must be at least 0");

    // Each URL is "http://" and a host with an optional port. cpp-httplib would quietly drop a path, and HTTPS is
    // not supported.
    for (const std::string* pUrl : {&_config.chatUrl, &_config.embeddingUrl}) {
        const std::string_view host = std::string_view(*pUrl).substr(std::min(pUrl->size(), URL_SCHEME.size()));
        if (!pUrl->starts_with(URL_SCHEME) || host.empty() || host.find_first_of("/?#") != std::string_view::npos) {
            throw InferenceError(
                std::format("'{}' is not a llama-server URL: it must be http://host:port, without a path", *pUrl));
        }
    }

    // The chat server: each slot must hold the context the budgets plan for, and its template tells whether the
    // model thinks.
    const auto [slotContext, chatTemplate] = Ask(_config.chatUrl, "/props", std::nullopt, [](const nlohmann::json& props) {
        return std::pair(props.at("default_generation_settings").at("n_ctx").get<int>(),
                         props.at("chat_template").get<std::string>());
    });
    if (slotContext < _config.contextSize) {
        throw InferenceError(std::format("the chat server at {} holds {} tokens per slot, less than the context size "
                                         "{} (start llama-server with a --ctx-size of at least the context size times "
                                         "--parallel)",
                                         _config.chatUrl, slotContext, _config.contextSize));
    }
    if (chatTemplate.find("enable_thinking") != std::string::npos) {
        // The thinking block's texts, tokenized once as the in-process backend tokenizes them: the markers with
        // special tokens parsed, the blank line after the end marker as plain text.
        ThinkingMarkers markers;
        markers.open = ServerTokens(inference::THINKING_OPEN_TEXT, false, true);
        markers.skip = ServerTokens(inference::THINKING_SKIP_TEXT, false, true);
        markers.close = ServerTokens(inference::CLOSE_THINKING_TEXT, false, true);
        markers.end = ServerTokens(inference::THINKING_END_TEXT, false, true);
        markers.afterEnd = ServerTokens(inference::THINKING_AFTER_END_TEXT, false, false);
        _thinking = std::move(markers);
    }
    // The BOS the model adds, if any: what add_special puts in front of an empty text.
    if (const std::vector<std::int32_t> special = ServerTokens("", true, false); !special.empty()) {
        _bos = special.front();
    }

    // The embedding server: its model file's name, without the extension, names the vectors it makes, as the
    // in-process backend names them, so a run's stored vectors stay valid when the backend changes.
    const std::string modelPath = Ask(_config.embeddingUrl, "/props", std::nullopt, [](const nlohmann::json& props) {
        return props.at("model_path").get<std::string>();
    });
    const std::size_t nameStart = modelPath.find_last_of("/\\");
    const std::string fileName = modelPath.substr(nameStart == std::string::npos ? 0 : nameStart + 1);
    const std::size_t dot = fileName.rfind('.');
    _embeddingModelId = dot == std::string::npos || dot == 0 ? fileName : fileName.substr(0, dot);
    if (_embeddingModelId.empty()) {
        throw InferenceError(std::format("the embedding server at {} names no model file in /props (model_path '{}')",
                                         _config.embeddingUrl, modelPath));
    }
}

int LlamaServerBackend::CountTokens(std::string_view text)
{
    return static_cast<int>(ServerTokens(text, false, false).size());
}

int LlamaServerBackend::CountPromptTokens(const inference::Prompt& prompt)
{
    return static_cast<int>(TokenizePrompt(prompt).size());
}

int LlamaServerBackend::ContextSize() const
{
    return _config.contextSize;
}

inference::Completion LlamaServerBackend::Complete(const inference::Prompt& /*prompt*/,
                                                   const inference::GenerationSpec& /*spec*/,
                                                   const inference::TokenCallback& /*onToken*/, std::stop_token /*stop*/)
{
    throw InferenceError("LlamaServerBackend::Complete is not implemented yet");
}

std::vector<std::vector<float>> LlamaServerBackend::Embed(const std::vector<std::string>& texts,
                                                          inference::EmbedPurpose purpose)
{
    const std::string& prefix = purpose == inference::EmbedPurpose::Query ? _config.embeddingQueryPrefix
                                                                          : _config.embeddingDocumentPrefix;
    std::vector<std::vector<float>> vectors;
    vectors.reserve(texts.size());
    for (const std::string& text : texts) {
        // One result for the one text, holding one vector: a pooled sentence-embedding model. A model without
        // pooling answers a vector per token.
        std::vector<float> vector = Ask(_config.embeddingUrl, "/embedding", nlohmann::json{{"content", prefix + text}},
                                        [this](const nlohmann::json& answer) {
                                            const std::size_t vectorCount =
                                                answer.size() == 1 ? answer.at(0).at("embedding").size() : 0;
                                            if (answer.size() != 1 || vectorCount != 1) {
                                                throw InferenceError(std::format(
                                                    "{}/embedding answered {} results and {} vectors for one text; it "
                                                    "needs one pooled vector (a sentence-embedding model)",
                                                    _config.embeddingUrl, answer.size(), vectorCount));
                                            }
                                            return answer.at(0).at("embedding").at(0).get<std::vector<float>>();
                                        });

        // L2-normalize, so cosine similarity is a dot product.
        double norm = 0.0;
        for (const float value : vector) {
            norm += static_cast<double>(value) * value;
        }
        norm = std::sqrt(norm);
        if (norm == 0.0) {
            throw InferenceError(std::format("{}/embedding returned a zero vector", _config.embeddingUrl));
        }
        for (float& value : vector) {
            value = static_cast<float>(value / norm);
        }
        vectors.push_back(std::move(vector));
    }
    return vectors;
}

std::string LlamaServerBackend::EmbeddingModelId() const
{
    return _embeddingModelId;
}

std::vector<std::uint8_t> LlamaServerBackend::SaveState()
{
    throw InferenceError("saving the chat cache is not supported by the llama-server backend");
}

void LlamaServerBackend::RestoreState(std::span<const std::uint8_t> /*state*/)
{
    throw InferenceError("restoring the chat cache is not supported by the llama-server backend");
}

std::vector<std::int32_t> LlamaServerBackend::TokenizePrompt(const inference::Prompt& prompt) const
{
    MORPHEUS_REQUIRE(!prompt.messages.empty(), "TokenizePrompt: the prompt has no messages");

    // The server's template renders the prompt with a marker in place of each content. add_generation_prompt: the
    // text ends where the assistant's reply begins. A thinking model's template renders with its switch on, so it
    // writes no empty block of its own: the markers are the backend's, as in-process.
    const std::string marker = inference::ContentMarker(prompt);
    nlohmann::json messages = nlohmann::json::array();
    for (const inference::Message& message : inference::MarkContents(prompt, marker).messages) {
        messages.push_back(nlohmann::json{{"role", inference::RoleName(message.role)}, {"content", message.content}});
    }
    nlohmann::json body = {{"messages", messages}, {"add_generation_prompt", true}};
    if (_thinking) {
        body["chat_template_kwargs"] = {{"enable_thinking", true}};
    }
    std::string rendered = Ask(_config.chatUrl, "/apply-template", body, [](const nlohmann::json& answer) {
        return answer.at("prompt").get<std::string>();
    });
    // A template that opens the block itself when thinking is on ends with the open marker; the markers come from the
    // backend, so it goes.
    if (_thinking && rendered.ends_with(inference::THINKING_OPEN_TEXT)) {
        rendered.resize(rendered.size() - inference::THINKING_OPEN_TEXT.size());
    }
    const std::vector<std::string> pieces = inference::TemplatePieces(rendered, marker, prompt.messages.size());

    // The pieces apart: template text with special tokens parsed (its control tokens are real; BOS on the first piece
    // when the model adds one), contents as plain text, whatever they hold.
    std::vector<std::int32_t> tokens;
    auto append = [&tokens](const std::vector<std::int32_t>& more) { tokens.insert(tokens.end(), more.begin(), more.end()); };
    for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
        const bool addBos = piece == 0 && _bos.has_value();
        if (!pieces[piece].empty() || addBos) {
            append(ServerTokens(pieces[piece], addBos, true));
        }
        if (piece < prompt.messages.size() && !prompt.messages[piece].content.empty()) {
            append(ServerTokens(prompt.messages[piece].content, false, false));
        }
    }
    // A template that writes BOS itself gets no second one, as in-process.
    if (_bos && tokens.size() >= 2 && tokens[0] == *_bos && tokens[1] == *_bos) {
        tokens.erase(tokens.begin());
    }
    // A thinking model's reply starts with its thinking block: empty here (thinking off), as its template writes it.
    if (_thinking) {
        append(_thinking->skip);
    }
    MORPHEUS_REQUIRE(!tokens.empty(), "TokenizePrompt: the rendered prompt has no tokens");
    return tokens;
}

std::vector<std::int32_t> LlamaServerBackend::ServerTokens(std::string_view text, bool addSpecial, bool parseSpecial) const
{
    const nlohmann::json body = {{"content", std::string(text)}, {"add_special", addSpecial}, {"parse_special", parseSpecial}};
    return Ask(_config.chatUrl, "/tokenize", body, [](const nlohmann::json& answer) {
        return answer.at("tokens").get<std::vector<std::int32_t>>();
    });
}

} // namespace morpheus::llamaserver
