#include "llamaserver/LlamaServerBackend.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
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
#include "core/Settings.h"
#include "core/Thinking.h"

namespace morpheus::llamaserver {

using inference::InferenceError;

namespace {

// A model's id as Morpheus names it: the file name in a server's /props model_path, without the extension, as the
// in-process backend names its models; "" when the path names no file.
std::string ModelIdFromPath(const std::string& modelPath)
{
    const std::size_t nameStart = modelPath.find_last_of("/\\");
    const std::string fileName = modelPath.substr(nameStart == std::string::npos ? 0 : nameStart + 1);
    const std::size_t dot = fileName.rfind('.');
    return dot == std::string::npos || dot == 0 ? fileName : fileName.substr(0, dot);
}

// How long a request may take to connect, and to wait for the server's next bytes. These requests (/props,
// /tokenize, /apply-template, /embedding) answer at once; the streaming /completion, which reads a long prompt before
// its first piece, waits longer on a kept client of its own.
constexpr int CONNECT_TIMEOUT_SECONDS = 5;
constexpr int READ_TIMEOUT_SECONDS = 60;
constexpr int STREAM_READ_TIMEOUT_SECONDS = 600;
// How much of a server's answer an error message quotes, in bytes.
constexpr std::size_t QUOTED_ANSWER_BYTES = 300;
// A server URL is this, then a host with an optional port (DEFAULT_PORT without one): no HTTPS, user or path.
constexpr std::string_view URL_SCHEME = "http://";
constexpr int DEFAULT_PORT = 80;
constexpr int MAX_PORT = 65535;
// Tokenized with add_special to tell a BOS (in front of it) from an EOS (after it). Never a special token itself.
constexpr std::string_view BOS_PROBE_TEXT = "a";

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

// A client of the llama-server at `url`, connecting to the host and port read from it, that waits at most
// `readTimeoutSeconds` for the server's next bytes and keeps its connection alive between requests. Throws
// InferenceError naming the URL if cpp-httplib cannot make it.
std::unique_ptr<httplib::Client> MakeClient(const std::string& url, const std::string& host, int port,
                                            int readTimeoutSeconds)
{
    auto pClient = std::make_unique<httplib::Client>(host, port);
    if (!pClient->is_valid()) {
        throw InferenceError(std::format("cannot make an HTTP client for the llama-server at '{}'", url));
    }
    pClient->set_connection_timeout(CONNECT_TIMEOUT_SECONDS, 0);
    pClient->set_read_timeout(readTimeoutSeconds, 0);
    pClient->set_keep_alive(true);
    // Each request goes out in small writes (headers, then body); without TCP_NODELAY, Linux holds the second until
    // the peer's delayed ACK, about 40 ms a request.
    pClient->set_tcp_nodelay(true);
    return pClient;
}

// Sends GET `path` (no body) or POST `path` with a JSON body through `client`, the kept-alive client of the server at
// `url`, and reads the JSON answer with `read`. A request that fails to be written or read under a connection kept
// from an earlier request is sent once more. Throws InferenceError naming the URL and path for an unreachable server,
// an HTTP error, an answer that is not JSON, or one `read` cannot read (a missing field, a wrong type).
template <typename Read>
auto Ask(httplib::Client& client, const std::string& url, const std::string& path,
         const std::optional<nlohmann::json>& body, Read&& read)
{
    // The server may close a kept connection just as this request leaves on it (llama-server closes one idle for 5 s;
    // cpp-httplib reconnects only when it sees the close before sending). These requests change nothing on the
    // server, so a write or read failure under a reused connection is sent once more, over a new one (cpp-httplib
    // drops the failed connection). cpp-httplib reports a read timeout as a read failure too, so a request that hangs
    // on a reused connection is also sent once more: it fails after two read timeouts. A failure to connect, a
    // failure on a new connection, an HTTP error and an answer that cannot be read are the server's own, and are never
    // repeated.
    auto send = [&] { return body ? client.Post(path, BodyText(*body), "application/json") : client.Get(path); };
    const bool reused = client.is_socket_open() != 0;
    httplib::Result response = send();
    if (!response && reused &&
        (response.error() == httplib::Error::Read || response.error() == httplib::Error::Write)) {
        response = send();
    }
    if (!response) {
        throw InferenceError(std::format("cannot reach {}{}: {}", url, path, httplib::to_string(response.error())));
    }
    // A 4xx answer refuses this request's input (a text too long for the server's context answers 400); any other
    // error is the server's own failure.
    if (response->status >= 400 && response->status < 500) {
        throw inference::InputRefused(
            std::format("{}{} answered HTTP {}: {}", url, path, response->status, Quoted(response->body)));
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

// What one streamed /completion call returned.
struct StreamResult {
    bool cancelled = false;    // A stop ended it before its last message.
    std::string stopType = {}; // Why generation stopped: "eos", "limit" or "word" (a stop string).
    int promptTokens = 0;      // tokens_evaluated: the prompt's tokens, as the server counted them.
    int promptEvaluated = 0;   // timings.prompt_n: the prompt tokens it evaluated (the rest came from the slot's cache).
    int generated = 0;         // timings.predicted_n
    double promptMs = 0.0;     // timings.prompt_ms
    double generateMs = 0.0;   // timings.predicted_ms
};

// Streams one POST /completion through `client`, the kept-alive client the backend uses for /completion only, to the
// server at `url`: each message's text goes to `onPiece` as it arrives, and the last message's stop_type and timings
// fill the result. A stop closes the connection at once, even while the server is still reading the prompt and sends
// nothing (llama-server then cancels the task), and the result is `cancelled`. A request that fails under the reused
// connection before a byte of answer arrived is sent once more; any other failure is not. Throws InferenceError
// naming the URL for an unreachable server, an HTTP error, a line that is not a message, a message without its
// fields, an error message in the stream, an unknown stop_type, or a stream that ends before its last message.
StreamResult StreamCompletion(httplib::Client& client, const std::string& url, const nlohmann::json& body,
                              const std::function<void(std::string_view)>& onPiece, std::stop_token stop)
{
    StreamResult result;
    if (stop.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    // The client serves /completion alone, so a stop can shut its connection down from any thread without touching
    // the other requests' connection; the next call connects again. cpp-httplib holds the client's socket lock while
    // it connects, and marks the request in flight under that same lock, so a stop that comes during a connect waits
    // for it (up to the connect timeout) on the stopping thread, then finds the request in flight and shuts its socket
    // down: the write fails, the retry below is barred by the stop, and the call ends cancelled. A stop that lands
    // before the body is written is also seen by upload_progress below.
    std::stop_callback closeOnStop(stop, [&client] { client.stop(); });

    // The status arrives before the body: an error's body is kept whole for the message, a stream is read line by
    // line as it arrives. A failure inside the receiver ends the request and is thrown once cpp-httplib returns.
    int status = 0;
    std::string errorBody;
    std::string pending; // Received bytes after the last complete line.
    bool finished = false;
    std::exception_ptr failure = nullptr;
    httplib::Request request;
    request.method = "POST";
    request.path = "/completion";
    request.body = BodyText(body);
    request.set_header("Content-Type", "application/json");
    request.upload_progress = [&stop](std::size_t /*current*/, std::size_t /*total*/) { return !stop.stop_requested(); };
    request.response_handler = [&status](const httplib::Response& response) {
        status = response.status;
        return true;
    };
    request.content_receiver = [&](const char* pData, std::size_t length, std::size_t /*offset*/, std::size_t /*total*/) {
        if (status != 200) {
            errorBody.append(pData, length);
            return true;
        }
        try {
            pending.append(pData, length);
            // Server-sent events: "data: {json}" lines, blank lines between them, and ":" lines (pings) to skip.
            for (std::size_t end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
                if (stop.stop_requested()) {
                    return false;
                }
                std::string line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty() || line.front() == ':') {
                    continue;
                }
                if (!line.starts_with("data: ")) {
                    throw InferenceError(std::format("{}/completion sent a line that is not a message: '{}'", url, Quoted(line)));
                }
                nlohmann::json message;
                try {
                    message = nlohmann::json::parse(std::string_view(line).substr(6));
                } catch (const nlohmann::json::parse_error&) {
                    throw InferenceError(std::format("{}/completion sent a message that is not JSON: '{}'", url, Quoted(line)));
                }
                if (message.contains("error")) {
                    throw InferenceError(std::format("{}/completion failed: {}", url, Quoted(message.at("error").dump())));
                }
                try {
                    // Every message carries the prompt's count and a piece of text; the last one says why generation
                    // stopped and how long each part took.
                    result.promptTokens = message.at("tokens_evaluated").get<int>();
                    const std::string& content = message.at("content").get_ref<const std::string&>();
                    if (!content.empty()) {
                        onPiece(content);
                    }
                    if (message.at("stop").get<bool>()) {
                        finished = true;
                        result.stopType = message.at("stop_type").get<std::string>();
                        const nlohmann::json& timings = message.at("timings");
                        result.promptEvaluated = timings.at("prompt_n").get<int>();
                        result.generated = timings.at("predicted_n").get<int>();
                        result.promptMs = timings.at("prompt_ms").get<double>();
                        result.generateMs = timings.at("predicted_ms").get<double>();
                    }
                } catch (const nlohmann::json::exception& error) {
                    throw InferenceError(std::format("{}/completion sent a message without its fields ({}): '{}'", url,
                                                     error.what(), Quoted(line)));
                }
                if (finished && result.stopType != "eos" && result.stopType != "limit" && result.stopType != "word") {
                    throw InferenceError(std::format("{}/completion stopped for an unknown reason '{}'", url, result.stopType));
                }
            }
            return true;
        } catch (...) {
            failure = std::current_exception();
            return false;
        }
    };

    // The connection kept from the previous call may have been closed by the server as this request left on it
    // (llama-server closes one idle for 5 s, as between two turns). A write or read failure on that reused
    // connection before a byte of answer arrived (no status) is sent once more, over a new connection (cpp-httplib
    // drops the failed one). "Reused" is judged by is_socket_open() before the request: when the server had already
    // closed the socket, cpp-httplib reconnects silently, and a failure on that fresh connection is retried too (one
    // extra attempt; a connect to a server that is gone fails fast). llama-server sends a streaming /completion's
    // headers only after the first token, so "no status yet" covers the whole prompt read: a failure or a read
    // timeout while a long prompt is read is retried, so a hung server takes about two read timeouts (about 1200 s)
    // to fail, and a retried prompt runs again on the same slot, mostly from its cache. A failure after any byte of
    // answer (the server has started generating), a stop, and a failure on a connection not reused are never
    // repeated.
    const bool reused = client.is_socket_open() != 0;
    httplib::Result response = client.send(request);
    if (!response && reused && status == 0 && !failure && !stop.stop_requested() &&
        (response.error() == httplib::Error::Read || response.error() == httplib::Error::Write)) {
        response = client.send(request);
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (!finished && stop.stop_requested()) {
        // The stop closed the connection (or came as the reply ended): the call is cancelled, as in-process.
        result.cancelled = true;
        return result;
    }
    if (!response) {
        throw InferenceError(std::format("cannot reach {}/completion: {}", url, httplib::to_string(response.error())));
    }
    if (status != 200) {
        throw InferenceError(std::format("{}/completion answered HTTP {}: {}", url, status, Quoted(errorBody)));
    }
    if (!finished) {
        throw InferenceError(std::format("the stream from {}/completion ended before its last message", url));
    }
    return result;
}

} // namespace

LlamaServerBackend::LlamaServerBackend(LlamaServerConfig config)
    : _config(std::move(config))
{
    MORPHEUS_REQUIRE(_config.contextSize > 0, "LlamaServerBackend: the context size must be positive");
    MORPHEUS_REQUIRE(_config.slot >= 0, "LlamaServerBackend: the slot must be at least 0");

    // One kept-alive client per server. Its URL is "http://", a host (letters, digits, '.' and '-'), then optionally
    // ':' and a port from 1 to 65535, and nothing else: cpp-httplib would quietly drop a path, does not do HTTPS here,
    // and makes an unusable client (one that crashes on first use) from a port it cannot parse, so the URL is read
    // here and the clients made from its host and port. The chat server gets two: one for /completion alone, so a
    // stop can close its connection without touching the other requests', with the longer read timeout.
    auto readUrl = [](const std::string& url) {
        const std::string_view rest = std::string_view(url).substr(std::min(url.size(), URL_SCHEME.size()));
        const std::size_t colon = rest.find(':');
        const std::string_view host = rest.substr(0, colon);
        const bool hostValid = !host.empty() && std::all_of(host.begin(), host.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        });
        int port = DEFAULT_PORT;
        bool portValid = true;
        if (colon != std::string_view::npos) {
            // Digits only (from_chars alone would take a leading '-'), all of the rest, within range.
            const std::string_view portText = rest.substr(colon + 1);
            const auto [pEnd, error] = std::from_chars(portText.data(), portText.data() + portText.size(), port);
            portValid = !portText.empty() && portText.find_first_not_of("0123456789") == std::string_view::npos &&
                        error == std::errc() && pEnd == portText.data() + portText.size() && port >= 1 &&
                        port <= MAX_PORT;
        }
        if (!url.starts_with(URL_SCHEME) || !hostValid || !portValid) {
            throw InferenceError(std::format(
                "'{}' is not a llama-server URL: it must be http://host or http://host:port, with a port from 1 to {}, "
                "and nothing else",
                url, MAX_PORT));
        }
        return std::pair(std::string(host), port);
    };
    const auto [chatHost, chatPort] = readUrl(_config.chatUrl);
    _pChatClient = MakeClient(_config.chatUrl, chatHost, chatPort, READ_TIMEOUT_SECONDS);
    _pStreamClient = MakeClient(_config.chatUrl, chatHost, chatPort, STREAM_READ_TIMEOUT_SECONDS);
    const auto [embeddingHost, embeddingPort] = readUrl(_config.embeddingUrl);
    _pEmbeddingClient = MakeClient(_config.embeddingUrl, embeddingHost, embeddingPort, READ_TIMEOUT_SECONDS);

    // The chat server: each slot must hold the context the budgets plan for, and its template tells whether the
    // model thinks.
    struct ChatProps {
        int slotContext = 0;
        std::string chatTemplate = {};
        int slots = 0;
        std::string modelPath = {};
    };
    const ChatProps chat = Ask(*_pChatClient, _config.chatUrl, "/props", std::nullopt, [](const nlohmann::json& props) {
        return ChatProps{props.at("default_generation_settings").at("n_ctx").get<int>(),
                         props.at("chat_template").get<std::string>(), props.at("total_slots").get<int>(),
                         props.at("model_path").get<std::string>()};
    });
    const int slotContext = chat.slotContext;
    const std::string& chatTemplate = chat.chatTemplate;
    // Its slots, which morpheus_server shares between conversations, and its model's id, for /health.
    if (chat.slots < 1) {
        throw InferenceError(
            std::format("the chat server at {} reports {} slots in /props (total_slots)", _config.chatUrl, chat.slots));
    }
    _slotCount = chat.slots;
    _chatModelId = ModelIdFromPath(chat.modelPath);
    if (_chatModelId.empty()) {
        throw InferenceError(std::format("the chat server at {} names no model file in /props (model_path '{}')",
                                         _config.chatUrl, chat.modelPath));
    }
    if (slotContext < _config.contextSize) {
        throw InferenceError(std::format("the chat server at {} holds {} tokens per slot, less than the context size "
                                         "{} (start llama-server with a --ctx-size of at least the context size times "
                                         "--parallel)",
                                         _config.chatUrl, slotContext, _config.contextSize));
    }
    // The thinking format: settings.json's, Auto reading the chat template's enable_thinking switch. Its texts are
    // tokenized once as the in-process backend tokenizes them: special tokens parsed, the text after the end marker
    // as plain text.
    if (const std::optional<core::ThinkingFormat> format = core::ResolveThinking(_config.thinking, chatTemplate)) {
        ThinkingMarkers markers;
        markers.open = ServerTokens(format->open, false, true);
        markers.skip = format->skip.empty() ? std::vector<std::int32_t>() : ServerTokens(format->skip, false, true);
        markers.close = ServerTokens(format->close, false, true);
        // A close text must fit the smallest thinking budget, or no budget could hold it (a data fault, found here).
        if (markers.close.size() >= static_cast<std::size_t>(core::MIN_THINKING_TOKENS)) {
            throw InferenceError(std::format("the thinking format's close text uses {} tokens; it must be under {} "
                                             "(the smallest thinking budget)",
                                             markers.close.size(), core::MIN_THINKING_TOKENS));
        }
        markers.end = ServerTokens(format->end, false, true);
        markers.afterEnd =
            format->afterEnd.empty() ? std::vector<std::int32_t>() : ServerTokens(format->afterEnd, false, false);
        markers.openText = format->open;
        markers.templateWritesOpen = core::HasThinkingSwitch(chatTemplate);
        markers.endText = format->end;
        _thinking = std::move(markers);
    }
    // The BOS the model adds, if any. What add_special puts around an empty text are the model's special tokens: one
    // in front of a text is a BOS, one after it an EOS (which the backend never asks for). More is refused rather than
    // guessed at.
    const std::vector<std::int32_t> special = ServerTokens("", true, false);
    if (special.size() > 1) {
        throw InferenceError(std::format("the chat server at {} adds {} special tokens to a text; the llama-server "
                                         "backend handles a model that adds at most a BOS",
                                         _config.chatUrl, special.size()));
    }
    if (special.size() == 1) {
        const std::vector<std::int32_t> probe = ServerTokens(BOS_PROBE_TEXT, true, false);
        if (!probe.empty() && probe.front() == special.front()) {
            _bos = special.front();
        }
    }

    // The embedding server: its model file's name, without the extension, names the vectors it makes, as the
    // in-process backend names them, so a run's stored vectors stay valid when the backend changes.
    const std::string modelPath =
        Ask(*_pEmbeddingClient, _config.embeddingUrl, "/props", std::nullopt,
            [](const nlohmann::json& props) { return props.at("model_path").get<std::string>(); });
    _embeddingModelId = ModelIdFromPath(modelPath);
    if (_embeddingModelId.empty()) {
        throw InferenceError(std::format("the embedding server at {} names no model file in /props (model_path '{}')",
                                         _config.embeddingUrl, modelPath));
    }
}

// Out of line: httplib::Client is complete only here.
LlamaServerBackend::~LlamaServerBackend() = default;

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

inference::Completion LlamaServerBackend::Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                                   const inference::TokenCallback& onToken, std::stop_token stop)
{
    MORPHEUS_REQUIRE(spec.maxTokens > 0, "Complete: maxTokens must be positive");
    MORPHEUS_REQUIRE(spec.thinkingMaxTokens >= 0, "Complete: thinkingMaxTokens must not be negative");
    MORPHEUS_REQUIRE(spec.slot >= -1, "Complete: slot must be -1 (the configured slot) or a server slot");

    // Thinking is asked of the model: it must have a thinking mode, and the budget must hold the closing sentence.
    const bool think = spec.thinkingMaxTokens > 0;
    if (think && !_thinking) {
        throw InferenceError(std::format("the chat model at {} has no thinking mode, so it cannot think before "
                                         "answering (set the thinking budgets to 0)",
                                         _config.chatUrl));
    }
    MORPHEUS_REQUIRE(!think || spec.thinkingMaxTokens > static_cast<int>(_thinking->close.size()),
                     "Complete: thinkingMaxTokens is too small to close the thinking block");

    // The prompt's tokens, made once for this call: a thinking model's end with the open block when the call thinks
    // (in place of the empty block), as in-process. They must leave room for the thinking and the answer.
    std::vector<std::int32_t> tokens = TokenizePrompt(prompt);
    if (think) {
        tokens.resize(tokens.size() - _thinking->skip.size());
        tokens.insert(tokens.end(), _thinking->open.begin(), _thinking->open.end());
    }
    inference::Completion completion;
    completion.promptTokens = static_cast<int>(tokens.size());
    if (spec.thinkingMaxTokens + spec.maxTokens > ContextSize() - completion.promptTokens) {
        throw InferenceError(std::format("prompt ({} tokens) plus thinking ({}) and max_tokens ({}) exceeds the "
                                         "context size ({})",
                                         completion.promptTokens, spec.thinkingMaxTokens, spec.maxTokens, ContextSize()));
    }

    // What every call of this completion sends. The prompt as token ids: /completion evaluates them as they are (it
    // adds BOS only to a prompt given as text). The sampling is LlamaCppBackend's chain: top-k, top-p keeping at least
    // one token, min-p, temperature, then llama-server's draw with the seed (at temperature 0 only the most likely
    // token is left: greedy, as in-process). Every other sampler llama-server knows is named neutral, so its
    // command-line defaults (penalties, DRY, top-n-sigma, typical, XTC, mirostat, a logit bias) cannot change what the
    // model writes. Then the slot, whose cache the calls share, and the stream.
    nlohmann::json body = {{"prompt", tokens},
                           {"samplers", nlohmann::json::array({"top_k", "top_p", "min_p", "temperature"})},
                           {"top_k", spec.sampling.topK},
                           {"top_p", spec.sampling.topP},
                           {"min_p", spec.sampling.minP},
                           {"min_keep", 1},
                           {"temperature", spec.sampling.temperature},
                           {"dynatemp_range", 0.0},
                           {"dynatemp_exponent", 1.0},
                           {"seed", spec.sampling.seed},
                           {"mirostat", 0},
                           {"typical_p", 1.0},
                           {"top_n_sigma", -1.0},
                           {"xtc_probability", 0.0},
                           {"repeat_penalty", 1.0},
                           {"repeat_last_n", 0},
                           {"frequency_penalty", 0.0},
                           {"presence_penalty", 0.0},
                           {"dry_multiplier", 0.0},
                           {"logit_bias", nlohmann::json::array()},
                           {"ignore_eos", false},
                           {"n_probs", 0},
                           {"id_slot", spec.slot == -1 ? _config.slot : spec.slot},
                           {"cache_prompt", true},
                           {"stream", true}};
    // Each call's prompt is `tokens` as it was sent: the server must have evaluated exactly those, since the budgets
    // rely on that count.
    auto checkPromptCount = [&](const StreamResult& result) {
        if (result.promptTokens != static_cast<int>(tokens.size())) {
            throw InferenceError(std::format("{} counted {} prompt tokens where Morpheus sent {}", _config.chatUrl,
                                             result.promptTokens, tokens.size()));
        }
    };
    // The first call reads the prompt: its numbers are the prompt's.
    auto takePromptNumbers = [&](const StreamResult& result) {
        completion.tokensEvaluated = result.promptEvaluated;
        completion.tokensReused = completion.promptTokens - result.promptEvaluated;
        completion.promptMs = result.promptMs;
    };

    // Think: one call without the grammar and without streaming to onToken, stopping at the end marker or at the
    // budget less the closing sentence. Its text is the thinking.
    if (think) {
        nlohmann::json thinking = body;
        thinking["n_predict"] = spec.thinkingMaxTokens - static_cast<int>(_thinking->close.size());
        thinking["stop"] = nlohmann::json::array({_thinking->endText});
        const StreamResult thought =
            StreamCompletion(*_pStreamClient, _config.chatUrl, thinking,
                             [&completion](std::string_view piece) { completion.thinking += piece; }, stop);
        if (thought.cancelled) {
            completion.cancelled = true;
            return completion;
        }
        checkPromptCount(thought);
        takePromptNumbers(thought);
        completion.thinkingTokens = thought.generated;
        completion.thinkingMs = thought.generateMs;
        if (thought.stopType == "eos") {
            return completion; // The model ended its reply inside the block: there is no answer.
        }
        // The answer call continues the same tokens. The stream cannot give the ids the model wrote (llama-server sends
        // no message for a token that ends inside a UTF-8 character, and in stream mode its last message carries no
        // ids), so the thinking text is tokenized again, as plain text: where the model's own split differs, the
        // server's cache reuse stops there.
        auto append = [&tokens](const std::vector<std::int32_t>& more) { tokens.insert(tokens.end(), more.begin(), more.end()); };
        if (!completion.thinking.empty()) {
            append(ServerTokens(completion.thinking, false, false));
        }
        if (thought.stopType == "word") {
            // The model closed the block: the end marker (the stop word, which the server leaves out of the text),
            // then the blank line its template puts before the answer.
            append(_thinking->end);
            append(_thinking->afterEnd);
            completion.thinkingTokens += static_cast<int>(_thinking->afterEnd.size());
        } else {
            // Out of budget ("limit"): close the block for the model.
            append(_thinking->close);
            completion.thinkingTokens += static_cast<int>(_thinking->close.size());
            completion.thinkingStoppedAtLimit = true;
        }
        body["prompt"] = tokens;
    }

    // Answer: under the grammar, each piece streamed to onToken.
    nlohmann::json answer = body;
    answer["n_predict"] = spec.maxTokens;
    if (!spec.grammar.empty()) {
        answer["grammar"] = spec.grammar;
    }
    const StreamResult answered = StreamCompletion(
        *_pStreamClient, _config.chatUrl, answer,
        [&](std::string_view piece) {
            completion.text += piece;
            if (onToken) {
                onToken(piece);
            }
        },
        stop);
    if (answered.cancelled) {
        completion.cancelled = true;
        return completion;
    }
    checkPromptCount(answered);
    if (think) {
        completion.thinkingMs += answered.promptMs; // Reading the block back is part of thinking.
    } else {
        takePromptNumbers(answered);
    }
    completion.tokensGenerated = answered.generated;
    completion.generateMs = answered.generateMs;
    completion.stoppedAtLimit = answered.stopType == "limit";
    return completion;
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
        const nlohmann::json body = {{"content", prefix + text}};
        std::vector<float> vector =
            Ask(*_pEmbeddingClient, _config.embeddingUrl, "/embedding", body, [this](const nlohmann::json& answer) {
                const std::size_t vectorCount = answer.size() == 1 ? answer.at(0).at("embedding").size() : 0;
                if (answer.size() != 1 || vectorCount != 1) {
                    throw InferenceError(std::format("{}/embedding answered {} results and {} vectors for one text; it "
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

int LlamaServerBackend::SlotCount() const
{
    return _slotCount;
}

std::string LlamaServerBackend::ChatModelId() const
{
    return _chatModelId;
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
    std::string rendered = Ask(*_pChatClient, _config.chatUrl, "/apply-template", body,
                               [](const nlohmann::json& answer) { return answer.at("prompt").get<std::string>(); });
    // A template with the enable_thinking switch opens the block itself when thinking is on, so its prompt ends with
    // the open text; the texts come from the backend, so it goes. A template without the switch writes no block: its
    // prompt may end with the open text only by chance, and keeps it.
    if (_thinking && _thinking->templateWritesOpen && rendered.ends_with(_thinking->openText)) {
        rendered.resize(rendered.size() - _thinking->openText.size());
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
    return Ask(*_pChatClient, _config.chatUrl, "/tokenize", body, [](const nlohmann::json& answer) {
        return answer.at("tokens").get<std::vector<std::int32_t>>();
    });
}

} // namespace morpheus::llamaserver
