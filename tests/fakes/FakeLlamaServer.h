#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // cpp-httplib includes windows.h: leave out what Morpheus never uses.
#endif
#endif
#include <httplib.h>

namespace morpheus::test {

// A stand-in for llama-server in the fast suite: a cpp-httplib server on a free local port that answers the
// endpoints LlamaServerBackend uses from a script, and records every request.
// - GET /props: the props SetProps gave (by default a chatml template without a thinking switch, 16384 tokens per
//   slot, 3 slots and an embedding model's file).
// - POST /tokenize: the tokenizer's ids for content, add_special (default false) and parse_special (default true), as
//   llama-server defaults them. By default DefaultTokens.
// - POST /apply-template: the renderer's text as "prompt" (by default chatml, with the assistant's opening when
//   add_generation_prompt is set).
// - POST /completion: the next queued CompletionScript, streamed as llama-server streams: a "data: {...}" message
//   per piece, then the last message with stop_type, tokens_cached and timings. With none queued: HTTP 500.
// - POST /embedding: llama-server's own answer, [{"index": 0, "embedding": [vectors]}], with the embedding maker's
//   vectors (by default the one vector {3, 4}).
// SetStatus makes a path answer an HTTP error instead, SetRawAnswer a body that is not JSON. Handlers run on the server's threads; every member they use
// is guarded by one mutex. DropNextAnswers closes the connection under a path's next requests.
class FakeLlamaServer {
public:
    // One request as it arrived.
    struct Request {
        std::string method = {};
        std::string path = {};
        nlohmann::json body = nullptr; // The JSON body; null for a GET.
        int remotePort = -1;           // The client's port: requests over one connection share it.
    };

    // One /completion answer.
    struct CompletionScript {
        std::vector<std::string> pieces = {}; // Streamed one message each, in order.
        std::string stopType = "eos";         // The last message's stop_type: "eos", "limit" or "word".
        int promptN = -1;          // timings.prompt_n, the prompt tokens evaluated; -1: all of them.
        int tokensEvaluated = -1;  // tokens_evaluated, the prompt's tokens; -1: the length of a prompt of token ids, or
                                   // a text prompt tokenized with add_special and parse_special, as llama-server does.
        double promptMs = 12.5;    // timings.prompt_ms
        double predictedMs = 40.0; // timings.predicted_ms
        int predictedN = -1;       // timings.predicted_n; -1: one per piece.
        std::string extraLine = {}; // Sent as a line of its own after the pieces, when not empty (a malformed stream).
        bool endEarly = false;      // End the stream after the pieces, without the last message.
        bool holdOpen = false;      // After the pieces, keep the stream open (pinging) until the client closes it.
    };

    using Tokenizer = std::function<std::vector<int>(const std::string& content, bool addSpecial, bool parseSpecial)>;
    using Renderer = std::function<std::string(const nlohmann::json& body)>;
    using EmbeddingMaker = std::function<std::vector<std::vector<float>>(const std::string& content)>;

private:
    static constexpr int DEFAULT_CONTEXT_SIZE = 16384; // The default /props: tokens per slot.
    static constexpr int DEFAULT_SLOTS = 3;            // The default /props: total_slots.
    static constexpr int HOLD_LIMIT_SECONDS = 10;      // A held stream ends after this long, closed or not.
    static constexpr int PING_INTERVAL_MS = 10;        // Pause between the pings of a held stream.

    mutable std::mutex _mutex = {};
    nlohmann::json _props = {{"default_generation_settings", {{"n_ctx", DEFAULT_CONTEXT_SIZE}}},
                             {"total_slots", DEFAULT_SLOTS},
                             {"chat_template", "{% for message in messages %}<|im_start|>{{ message.role }}{% endfor %}"},
                             {"model_path", "/models/nomic-embed-text-v1.5.Q8_0.gguf"}};
    Tokenizer _tokenizer = DefaultTokens;
    Renderer _renderer = [](const nlohmann::json& body) {
        std::string prompt;
        for (const nlohmann::json& message : body.at("messages")) {
            prompt += "<|im_start|>" + message.at("role").get<std::string>() + "\n" +
                      message.at("content").get<std::string>() + "<|im_end|>\n";
        }
        if (body.value("add_generation_prompt", true)) {
            prompt += "<|im_start|>assistant\n";
        }
        return prompt;
    };
    EmbeddingMaker _embeddingMaker = [](const std::string&) { return std::vector<std::vector<float>>{{3.0f, 4.0f}}; };
    std::deque<CompletionScript> _completions = {};
    std::map<std::string, int> _statuses = {};           // Path -> the HTTP error it answers.
    std::map<std::string, std::string> _rawAnswers = {}; // Path -> the raw body it answers with HTTP 200.
    std::map<std::string, int> _drops = {};              // Path -> how many of its next requests lose their connection.
    std::vector<Request> _requests = {};
    bool _clientClosedStream = false;
    std::atomic<bool> _stopping = false; // Set by the destructor: held streams end at once.
    httplib::Server _server = {};
    int _port = 0;
    std::jthread _listener = {}; // Last: joined first on destruction, after the destructor has stopped the server.

public:
    // On `port` of 127.0.0.1 (a server restarted where another one stopped), or on a free port when it is 0.
    explicit FakeLlamaServer(int port = 0)
    {
        // Each handler records its request, then answers from the script, or with the status SetStatus gave its path.
        _server.Get("/props", [this](const httplib::Request& request, httplib::Response& response) {
            if (!Record(request, response)) {
                return;
            }
            std::scoped_lock lock(_mutex);
            response.set_content(_props.dump(), "application/json");
        });
        _server.Post("/tokenize", [this](const httplib::Request& request, httplib::Response& response) {
            if (!Record(request, response)) {
                return;
            }
            nlohmann::json body;
            if (!ParseBody(request, response, body)) {
                return;
            }
            // The callback runs outside the lock: it may call the fake, and requests run concurrently.
            const Tokenizer tokenizer = Copy(_tokenizer);
            const std::vector<int> tokens = tokenizer(body.at("content").get<std::string>(),
                                                      body.value("add_special", false), body.value("parse_special", true));
            response.set_content(nlohmann::json{{"tokens", tokens}}.dump(), "application/json");
        });
        _server.Post("/apply-template", [this](const httplib::Request& request, httplib::Response& response) {
            if (!Record(request, response)) {
                return;
            }
            nlohmann::json body;
            if (!ParseBody(request, response, body)) {
                return;
            }
            const Renderer renderer = Copy(_renderer);
            response.set_content(nlohmann::json{{"prompt", renderer(body)}}.dump(), "application/json");
        });
        _server.Post("/embedding", [this](const httplib::Request& request, httplib::Response& response) {
            if (!Record(request, response)) {
                return;
            }
            nlohmann::json body;
            if (!ParseBody(request, response, body)) {
                return;
            }
            const EmbeddingMaker maker = Copy(_embeddingMaker);
            const nlohmann::json answer =
                nlohmann::json::array({{{"index", 0}, {"embedding", maker(body.at("content").get<std::string>())}}});
            response.set_content(answer.dump(), "application/json");
        });
        _server.Post("/completion", [this](const httplib::Request& request, httplib::Response& response) {
            if (!Record(request, response)) {
                return;
            }
            nlohmann::json body;
            if (!ParseBody(request, response, body)) {
                return;
            }
            CompletionScript script;
            int promptTokens = 0;
            Tokenizer tokenizer;
            {
                std::scoped_lock lock(_mutex);
                if (_completions.empty()) {
                    response.status = 500;
                    response.set_content(
                        R"({"error":{"code":500,"message":"the fake server has no scripted completion","type":"server_error"}})",
                        "application/json");
                    return;
                }
                script = std::move(_completions.front());
                _completions.pop_front();
                tokenizer = _tokenizer;
            }
            {
                // The prompt's tokens as llama-server counts them: token ids as they are, a text tokenized with
                // special tokens added and parsed.
                const nlohmann::json& prompt = body.at("prompt");
                promptTokens = script.tokensEvaluated >= 0 ? script.tokensEvaluated
                               : prompt.is_array()        ? static_cast<int>(prompt.size())
                                   : static_cast<int>(tokenizer(prompt.get<std::string>(), true, true).size());
            }
            const int slot = std::max(0, body.value("id_slot", -1));
            response.set_chunked_content_provider(
                "text/event-stream", [this, script, promptTokens, slot](std::size_t, httplib::DataSink& sink) {
                    auto send = [&sink](const std::string& text) { return sink.write(text.data(), text.size()); };
                    // One message per piece, as llama-server sends one per token.
                    for (std::size_t i = 0; i < script.pieces.size(); ++i) {
                        const nlohmann::json message = {{"index", 0},
                                                        {"content", script.pieces[i]},
                                                        {"tokens", nlohmann::json::array({0})},
                                                        {"stop", false},
                                                        {"id_slot", slot},
                                                        {"tokens_predicted", i + 1},
                                                        {"tokens_evaluated", promptTokens}};
                        if (!send("data: " + message.dump() + "\n\n")) {
                            return false;
                        }
                    }
                    if (!script.extraLine.empty() && !send(script.extraLine + "\n\n")) {
                        return false;
                    }
                    if (script.holdOpen) {
                        // Pings (comment lines, as llama-server sends while it works) until a write fails: the client
                        // closed the stream. Bounded, so a test that never closes cannot hang the fake.
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(HOLD_LIMIT_SECONDS);
                        bool closed = false;
                        while (!closed && !_stopping && std::chrono::steady_clock::now() < deadline) {
                            closed = !send(":\n\n");
                            std::this_thread::sleep_for(std::chrono::milliseconds(PING_INTERVAL_MS));
                        }
                        if (closed) {
                            std::scoped_lock lock(_mutex);
                            _clientClosedStream = true;
                        }
                        return false;
                    }
                    if (!script.endEarly) {
                        // The last message: no text (stream mode sent it already), why it stopped, and the timings.
                        const int predicted =
                            script.predictedN >= 0 ? script.predictedN : static_cast<int>(script.pieces.size());
                        const int evaluated = script.promptN >= 0 ? script.promptN : promptTokens;
                        const nlohmann::json last = {
                            {"index", 0},
                            {"content", ""},
                            {"tokens", nlohmann::json::array()},
                            {"id_slot", slot},
                            {"stop", true},
                            {"stop_type", script.stopType},
                            {"tokens_predicted", predicted},
                            {"tokens_evaluated", promptTokens},
                            {"tokens_cached", promptTokens + predicted},
                            {"truncated", false},
                            {"timings",
                             {{"cache_n", promptTokens - evaluated},
                              {"prompt_n", evaluated},
                              {"prompt_ms", script.promptMs},
                              {"predicted_n", predicted},
                              {"predicted_ms", script.predictedMs}}}};
                        if (!send("data: " + last.dump() + "\n\n")) {
                            return false;
                        }
                    }
                    sink.done();
                    return true;
                });
        });

        // A free local port, then the server on a thread of its own.
        if (port == 0) {
            _port = _server.bind_to_any_port("127.0.0.1");
        } else if (_server.bind_to_port("127.0.0.1", port)) {
            _port = port;
        }
        if (_port <= 0) {
            throw std::runtime_error("FakeLlamaServer: cannot bind port " + std::to_string(port) + " of 127.0.0.1");
        }
        _listener = std::jthread([this] { _server.listen_after_bind(); });
        _server.wait_until_ready();
    }

    ~FakeLlamaServer()
    {
        // Held streams end, the server stops, and _listener's destructor joins its thread.
        _stopping = true;
        _server.stop();
    }

    FakeLlamaServer(const FakeLlamaServer&) = delete;
    FakeLlamaServer& operator=(const FakeLlamaServer&) = delete;

    // The default tokenizer: one token per 4 bytes (the last one shorter), whatever the flags, and no BOS. A token's
    // id is its first byte plus 256 times its length, so tests can tell texts apart by their ids.
    static std::vector<int> DefaultTokens(const std::string& content, bool /*addSpecial*/, bool /*parseSpecial*/)
    {
        std::vector<int> tokens;
        for (std::size_t at = 0; at < content.size(); at += 4) {
            const std::size_t length = std::min<std::size_t>(4, content.size() - at);
            tokens.push_back(static_cast<unsigned char>(content[at]) + 256 * static_cast<int>(length));
        }
        return tokens;
    }

    // The port it listens on, of 127.0.0.1.
    int Port() const
    {
        return _port;
    }

    // "http://127.0.0.1:<port>".
    std::string Url() const
    {
        return "http://127.0.0.1:" + std::to_string(_port);
    }

    void SetProps(nlohmann::json props)
    {
        std::scoped_lock lock(_mutex);
        _props = std::move(props);
    }

    void SetTokenizer(Tokenizer tokenizer)
    {
        std::scoped_lock lock(_mutex);
        _tokenizer = std::move(tokenizer);
    }

    void SetRenderer(Renderer renderer)
    {
        std::scoped_lock lock(_mutex);
        _renderer = std::move(renderer);
    }

    void SetEmbeddingMaker(EmbeddingMaker maker)
    {
        std::scoped_lock lock(_mutex);
        _embeddingMaker = std::move(maker);
    }

    // The next /completion request gets `script`; later ones the scripts queued after it.
    void QueueCompletion(CompletionScript script)
    {
        std::scoped_lock lock(_mutex);
        _completions.push_back(std::move(script));
    }

    // Requests to `path` answer HTTP `status` with an error body; 0 answers them normally again.
    void SetStatus(const std::string& path, int status)
    {
        std::scoped_lock lock(_mutex);
        if (status == 0) {
            _statuses.erase(path);
        } else {
            _statuses[path] = status;
        }
    }

    // Requests to `path` answer HTTP 200 with `body` as it is (an answer that is not JSON); "" answers them normally
    // again.
    void SetRawAnswer(const std::string& path, std::string body)
    {
        std::scoped_lock lock(_mutex);
        if (body.empty()) {
            _rawAnswers.erase(path);
        } else {
            _rawAnswers[path] = std::move(body);
        }
    }

    // The next `count` requests to `path` (recorded as usual) lose their connection: the fake sends an answer's status
    // line and headers, then closes the connection before the body, so the client fails with a read error, as it does
    // when llama-server closes a kept-alive connection under a request.
    void DropNextAnswers(const std::string& path, int count = 1)
    {
        std::scoped_lock lock(_mutex);
        _drops[path] += count;
    }

    // Every request so far, in order.
    std::vector<Request> Requests() const
    {
        std::scoped_lock lock(_mutex);
        return _requests;
    }

    // The requests to `path` so far, in order.
    std::vector<Request> RequestsTo(const std::string& path) const
    {
        std::scoped_lock lock(_mutex);
        std::vector<Request> matching;
        std::copy_if(_requests.begin(), _requests.end(), std::back_inserter(matching),
                     [&path](const Request& request) { return request.path == path; });
        return matching;
    }

    // Whether a client closed a held stream (CompletionScript::holdOpen) before the fake gave up on it.
    bool ClientClosedStream() const
    {
        std::scoped_lock lock(_mutex);
        return _clientClosedStream;
    }

private:
    // A copy of a callback member, taken under the lock so the caller can run it without.
    template <typename Callback>
    Callback Copy(const Callback& member) const
    {
        std::scoped_lock lock(_mutex);
        return member;
    }

    // Parses the body of `request` into `body`. A malformed body answers HTTP 400 and returns false.
    bool ParseBody(const httplib::Request& request, httplib::Response& response, nlohmann::json& body) const
    {
        body = nlohmann::json::parse(request.body, nullptr, false);
        if (!body.is_discarded()) {
            return true;
        }
        response.status = 400;
        response.set_content(R"({"error":{"code":400,"message":"malformed JSON body","type":"invalid_request_error"}})",
                             "application/json");
        return false;
    }

    // Records `request`. Returns false after dropping its connection (DropNextAnswers), or answering the HTTP error
    // SetStatus set for its path, or the raw body SetRawAnswer set for it; true otherwise.
    bool Record(const httplib::Request& request, httplib::Response& response)
    {
        std::scoped_lock lock(_mutex);
        // The raw body is kept first: a body that is not JSON is recorded as its text.
        nlohmann::json body = nullptr;
        if (!request.body.empty()) {
            body = nlohmann::json::parse(request.body, nullptr, false);
            if (body.is_discarded()) {
                body = request.body;
            }
        }
        _requests.push_back({request.method, request.path, std::move(body), request.remote_port});
        if (const auto drop = _drops.find(request.path); drop != _drops.end()) {
            if (--drop->second == 0) {
                _drops.erase(drop);
            }
            // A body of one byte that never comes: cpp-httplib writes the status line and headers, then closes the
            // connection when the provider fails.
            response.set_content_provider(1, "application/json",
                                          [](std::size_t, std::size_t, httplib::DataSink&) { return false; });
            return false;
        }
        if (const auto raw = _rawAnswers.find(request.path); raw != _rawAnswers.end()) {
            response.set_content(raw->second, "text/html");
            return false;
        }
        const auto status = _statuses.find(request.path);
        if (status == _statuses.end()) {
            return true;
        }
        response.status = status->second;
        const nlohmann::json error = {
            {"error", {{"code", status->second}, {"message", "scripted failure"}, {"type", "server_error"}}}};
        response.set_content(error.dump(), "application/json");
        return false;
    }
};

} // namespace morpheus::test
