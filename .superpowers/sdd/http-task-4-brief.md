### Task 4: cpp-httplib and the fake llama-server

The new dependency, its option, and the test double every later task uses. `tests/fakes/FakeLlamaServer.h` is a cpp-httplib server on a free local port that answers llama-server's endpoints from a script, streams completions as llama-server does, and records every request.

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/fakes/FakeLlamaServer.h`
- Test: `tests/llamaserver/FakeLlamaServerTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: CMake option `MORPHEUS_BUILD_LLAMA_SERVER` (default ON); INTERFACE target `morpheus_httplib` (SYSTEM include of cpp-httplib's folder, links `Threads::Threads` and on Windows `ws2_32`). `class morpheus::test::FakeLlamaServer` with:
  - `struct Request { std::string method; std::string path; nlohmann::json body; }` (`body` null for a GET);
  - `struct CompletionScript { std::vector<std::string> pieces; std::string stopType = "eos"; int promptN = -1; int tokensEvaluated = -1; double promptMs = 12.5; double predictedMs = 40.0; int predictedN = -1; std::string extraLine; bool endEarly = false; bool holdOpen = false; }`;
  - `using Tokenizer = std::function<std::vector<int>(const std::string& content, bool addSpecial, bool parseSpecial)>`, `using Renderer = std::function<std::string(const nlohmann::json& body)>`, `using EmbeddingMaker = std::function<std::vector<std::vector<float>>(const std::string& content)>`;
  - `std::string Url() const` (`"http://127.0.0.1:<port>"`), `static std::vector<int> DefaultTokens(const std::string& content, bool addSpecial, bool parseSpecial)` (the default tokenizer), `void SetProps(nlohmann::json)`, `void SetTokenizer(Tokenizer)`, `void SetRenderer(Renderer)`, `void SetEmbeddingMaker(EmbeddingMaker)`, `void QueueCompletion(CompletionScript)`, `void SetStatus(const std::string& path, int status)` (0 clears), `std::vector<Request> Requests() const`, `std::vector<Request> RequestsTo(const std::string& path) const`, `bool ClientClosedStream() const`.
  - Defaults: `/props` = `{"default_generation_settings": {"n_ctx": 16384}, "total_slots": 3, "chat_template": "<a chatml template without enable_thinking>", "model_path": "/models/nomic-embed-text-v1.5.Q8_0.gguf"}`; `DefaultTokens`: one token per 4 bytes (the last one shorter), whatever the flags, a token's id its first byte plus 256 times its length, no BOS; a `/completion` prompt is token ids (its length is `tokens_evaluated`) or text (tokenized with `add_special` and `parse_special`); chatml rendering (`<|im_start|>role\ncontent<|im_end|>\n` per message, then `<|im_start|>assistant\n` when `add_generation_prompt`); the embedding `{{3, 4}}`.

- [ ] **Step 1: Write the failing tests**

Create `tests/llamaserver/FakeLlamaServerTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "fakes/FakeLlamaServer.h"

using namespace morpheus;

TEST_CASE("FakeLlamaServer answers from its script and records each request", "[fake-llama-server]")
{
    test::FakeLlamaServer fake;
    httplib::Client client(fake.Url());

    const httplib::Result props = client.Get("/props");
    REQUIRE(props);
    CHECK(props->status == 200);
    CHECK(nlohmann::json::parse(props->body).at("default_generation_settings").at("n_ctx") == 16384);

    // One token per 4 bytes, rounded up.
    const httplib::Result tokens =
        client.Post("/tokenize", R"({"content":"abcde","add_special":false,"parse_special":false})", "application/json");
    REQUIRE(tokens);
    CHECK(nlohmann::json::parse(tokens->body).at("tokens").size() == 2);

    const httplib::Result rendered = client.Post(
        "/apply-template", R"({"messages":[{"role":"user","content":"Hi"}],"add_generation_prompt":true})", "application/json");
    REQUIRE(rendered);
    CHECK(nlohmann::json::parse(rendered->body).at("prompt") == "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n");

    // llama-server's own /embedding answer: one result per text, holding the pooled vector.
    const httplib::Result embedded = client.Post("/embedding", R"({"content":"a lighthouse"})", "application/json");
    REQUIRE(embedded);
    CHECK(nlohmann::json::parse(embedded->body) == nlohmann::json::parse(R"([{"index":0,"embedding":[[3.0,4.0]]}])"));

    // A scripted failure.
    fake.SetStatus("/props", 503);
    const httplib::Result refused = client.Get("/props");
    REQUIRE(refused);
    CHECK(refused->status == 503);

    const std::vector<test::FakeLlamaServer::Request> requests = fake.Requests();
    REQUIRE(requests.size() == 5);
    CHECK(requests[0].method == "GET");
    CHECK(requests[0].path == "/props");
    CHECK(requests[0].body.is_null());
    CHECK(requests[1].method == "POST");
    CHECK(requests[1].path == "/tokenize");
    CHECK(requests[1].body.at("content") == "abcde");
    CHECK(fake.RequestsTo("/props").size() == 2);
}

TEST_CASE("FakeLlamaServer streams a scripted completion as llama-server does", "[fake-llama-server]")
{
    test::FakeLlamaServer fake;
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"Hel", "lo"};
    script.stopType = "limit";
    script.promptN = 1;
    fake.QueueCompletion(script);
    test::FakeLlamaServer::CompletionScript cut;
    cut.pieces = {"Hi"};
    cut.endEarly = true;
    fake.QueueCompletion(cut);

    // The messages of one streamed answer, in order: each "data: {...}" line, without blank lines and pings.
    httplib::Client client(fake.Url());
    auto stream = [&client](const std::string& body) {
        std::string received;
        const httplib::Result result =
            client.Post("/completion", httplib::Headers{}, body, "application/json", [&received](const char* pData, std::size_t length) {
                received.append(pData, length);
                return true;
            });
        REQUIRE(result);
        REQUIRE(result->status == 200);
        std::vector<nlohmann::json> messages;
        std::size_t start = 0;
        for (std::size_t end = received.find('\n'); end != std::string::npos; end = received.find('\n', start)) {
            const std::string line = received.substr(start, end - start);
            start = end + 1;
            if (line.starts_with("data: ")) {
                messages.push_back(nlohmann::json::parse(line.substr(6)));
            }
        }
        return messages;
    };

    // "abcdefgh" is 2 tokens by the default tokenizer: every message carries it as tokens_evaluated.
    const std::vector<nlohmann::json> messages = stream(R"({"prompt":"abcdefgh","id_slot":2})");
    REQUIRE(messages.size() == 3);
    CHECK(messages[0].at("content") == "Hel");
    CHECK(messages[0].at("stop") == false);
    CHECK(messages[0].at("tokens_evaluated") == 2);
    CHECK(messages[0].at("id_slot") == 2);
    CHECK(messages[1].at("content") == "lo");
    CHECK(messages[2].at("content") == "");
    CHECK(messages[2].at("stop") == true);
    CHECK(messages[2].at("stop_type") == "limit");
    CHECK(messages[2].at("tokens_evaluated") == 2);
    CHECK(messages[2].at("tokens_cached") == 4);
    CHECK(messages[2].at("timings").at("prompt_n") == 1);
    CHECK(messages[2].at("timings").at("predicted_n") == 2);
    CHECK(messages[2].at("timings").at("prompt_ms") == 12.5);
    CHECK(messages[2].at("timings").at("predicted_ms") == 40.0);

    // A script that ends early sends its pieces and no last message. A prompt of token ids counts as its length.
    const std::vector<nlohmann::json> cutMessages = stream(R"({"prompt":[5,6,7]})");
    REQUIRE(cutMessages.size() == 1);
    CHECK(cutMessages[0].at("content") == "Hi");
    CHECK(cutMessages[0].at("tokens_evaluated") == 3);

    // With nothing left to answer, the fake fails the request as llama-server fails one.
    const httplib::Result none = client.Post("/completion", R"({"prompt":"abc"})", "application/json");
    REQUIRE(none);
    CHECK(none->status == 500);
}

TEST_CASE("FakeLlamaServer sees a client close a held stream", "[fake-llama-server]")
{
    test::FakeLlamaServer fake;
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"Hel"};
    script.holdOpen = true;
    fake.QueueCompletion(script);
    {
        // The receiver refuses the first bytes, which ends the request; the client closes its connection.
        httplib::Client client(fake.Url());
        const httplib::Result result = client.Post("/completion", httplib::Headers{}, R"({"prompt":"abc"})",
                                                   "application/json", [](const char*, std::size_t) { return false; });
        CHECK_FALSE(result);
        CHECK(result.error() == httplib::Error::Canceled);
    }
    // The fake notices on one of its next pings.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fake.ClientClosedStream() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(fake.ClientClosedStream());
}
```

In `tests/CMakeLists.txt`, after the block

```cmake
if(NOT MORPHEUS_BUILD_LLAMA)
    list(FILTER MORPHEUS_TEST_SOURCES EXCLUDE REGEX "/tests/llama/")
endif()
```

add

```cmake
# The llama-server backend's tests need cpp-httplib; presets without it skip their sources too.
if(NOT MORPHEUS_BUILD_LLAMA_SERVER)
    list(FILTER MORPHEUS_TEST_SOURCES EXCLUDE REGEX "/tests/llamaserver/")
endif()
```

and after the block

```cmake
if(MORPHEUS_BUILD_LLAMA)
    target_link_libraries(morpheus_tests PRIVATE morpheus_llama)
endif()
```

add

```cmake
if(MORPHEUS_BUILD_LLAMA_SERVER)
    target_link_libraries(morpheus_tests PRIVATE morpheus_httplib) # The fake llama-server.
endif()
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: `fatal error C1083: Cannot open include file: 'fakes/FakeLlamaServer.h': No such file or directory` in `tests/llamaserver/FakeLlamaServerTests.cpp` (CMake takes the unknown `morpheus_httplib` for a library file name, so nothing fails before the compiler).

- [ ] **Step 3: Fetch cpp-httplib and add the option**

In the root `CMakeLists.txt`, after the line

```cmake
option(MORPHEUS_BUILD_LLAMA "Build the llama.cpp backend, morpheus_probe and morpheus_play (fetches and builds llama.cpp)" ${PROJECT_IS_TOP_LEVEL})
```

add

```cmake
# On in every build, vendored ones included: the llama-server backend needs no GPU or model, only cpp-httplib.
option(MORPHEUS_BUILD_LLAMA_SERVER "Build the llama-server backend, morpheus_llamaserver (fetches cpp-httplib, header-only, no OpenSSL)" ON)
```

After the block

```cmake
if(MORPHEUS_BUILD_APP AND NOT MORPHEUS_BUILD_LLAMA)
    message(FATAL_ERROR "MORPHEUS_BUILD_APP needs MORPHEUS_BUILD_LLAMA.")
endif()
```

add

```cmake
# The app, morpheus_play and morpheus_probe build either backend, as settings.json chooses.
if(MORPHEUS_BUILD_LLAMA AND NOT MORPHEUS_BUILD_LLAMA_SERVER)
    message(FATAL_ERROR "MORPHEUS_BUILD_LLAMA (the app, morpheus_play and morpheus_probe) needs MORPHEUS_BUILD_LLAMA_SERVER.")
endif()
```

After the SQLite block (the line `set_target_properties(morpheus_sqlite3 PROPERTIES COMPILE_WARNING_AS_ERROR OFF)`), add

```cmake

if(MORPHEUS_BUILD_LLAMA_SERVER)
    # cpp-httplib (the llama-server backend and the tests' fake server): header-only, used without OpenSSL, zlib,
    # brotli or zstd. Its own CMakeLists.txt would look for them, so it is not used: SOURCE_SUBDIR names a folder
    # without one, and FetchContent only downloads. The target is defined here under a name of Morpheus's own, like
    # SQLite's, so it never meets llama.cpp's vendored cpp-httplib target or a host project's httplib target.
    FetchContent_Declare(
        morpheus_cpp_httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG        cf3693cb5cc0d39b0e6f4122ba89bda9edb5ac6b       # commit SHA to get, v0.59.0
        GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
        SOURCE_SUBDIR  no-cmake-build                                 # No CMakeLists.txt there: download only
    )
    FetchContent_MakeAvailable(morpheus_cpp_httplib)
    add_library(morpheus_httplib INTERFACE)
    # SYSTEM: its warnings are not Morpheus's (no /W4 /WX on httplib.h).
    target_include_directories(morpheus_httplib SYSTEM INTERFACE ${morpheus_cpp_httplib_SOURCE_DIR})
    find_package(Threads REQUIRED)
    target_link_libraries(morpheus_httplib INTERFACE Threads::Threads)
    if(WIN32)
        target_link_libraries(morpheus_httplib INTERFACE ws2_32)
    endif()
    if(MSVC)
        # httplib.h is about 23,000 lines of inline code: a file that includes it can pass MSVC's section limit.
        target_compile_options(morpheus_httplib INTERFACE /bigobj)
    endif()
endif()
```

- [ ] **Step 4: Write the fake server**

Create `tests/fakes/FakeLlamaServer.h`:

```cpp
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
// SetStatus makes a path answer an HTTP error instead. Handlers run on the server's threads; every member they use
// is guarded by one mutex.
class FakeLlamaServer {
public:
    // One request as it arrived.
    struct Request {
        std::string method = {};
        std::string path = {};
        nlohmann::json body = nullptr; // The JSON body; null for a GET.
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
    mutable std::mutex _mutex = {};
    nlohmann::json _props = {{"default_generation_settings", {{"n_ctx", 16384}}},
                             {"total_slots", 3},
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
    std::map<std::string, int> _statuses = {}; // Path -> the HTTP error it answers.
    std::vector<Request> _requests = {};
    bool _clientClosedStream = false;
    std::atomic<bool> _stopping = false; // Set by the destructor: held streams end at once.
    httplib::Server _server = {};
    int _port = 0;
    std::jthread _listener = {}; // Last: joined first on destruction, after the destructor has stopped the server.

public:
    FakeLlamaServer()
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
            const nlohmann::json body = nlohmann::json::parse(request.body);
            std::scoped_lock lock(_mutex);
            const std::vector<int> tokens = _tokenizer(body.at("content").get<std::string>(),
                                                       body.value("add_special", false), body.value("parse_special", true));
            response.set_content(nlohmann::json{{"tokens", tokens}}.dump(), "application/json");
        });
        _server.Post("/apply-template", [this](const httplib::Request& request, httplib::Response& response) {
            if (!Record(request, response)) {
                return;
            }
            const nlohmann::json body = nlohmann::json::parse(request.body);
            std::scoped_lock lock(_mutex);
            response.set_content(nlohmann::json{{"prompt", _renderer(body)}}.dump(), "application/json");
        });
        _server.Post("/embedding", [this](const httplib::Request& request, httplib::Response& response) {
            if (!Record(request, response)) {
                return;
            }
            const nlohmann::json body = nlohmann::json::parse(request.body);
            std::scoped_lock lock(_mutex);
            const nlohmann::json answer = nlohmann::json::array(
                {{{"index", 0}, {"embedding", _embeddingMaker(body.at("content").get<std::string>())}}});
            response.set_content(answer.dump(), "application/json");
        });
        _server.Post("/completion", [this](const httplib::Request& request, httplib::Response& response) {
            if (!Record(request, response)) {
                return;
            }
            const nlohmann::json body = nlohmann::json::parse(request.body);
            CompletionScript script;
            int promptTokens = 0;
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
                // The prompt's tokens as llama-server counts them: token ids as they are, a text tokenized with
                // special tokens added and parsed.
                const nlohmann::json& prompt = body.at("prompt");
                promptTokens = script.tokensEvaluated >= 0 ? script.tokensEvaluated
                               : prompt.is_array()        ? static_cast<int>(prompt.size())
                                   : static_cast<int>(_tokenizer(prompt.get<std::string>(), true, true).size());
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
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                        bool closed = false;
                        while (!closed && !_stopping && std::chrono::steady_clock::now() < deadline) {
                            closed = !send(":\n\n");
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                        std::scoped_lock lock(_mutex);
                        _clientClosedStream = closed;
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
        _port = _server.bind_to_any_port("127.0.0.1");
        if (_port <= 0) {
            throw std::runtime_error("FakeLlamaServer: cannot bind a local port");
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
    // Records `request`. Returns false after answering the HTTP error SetStatus set for its path, true otherwise.
    bool Record(const httplib::Request& request, httplib::Response& response)
    {
        std::scoped_lock lock(_mutex);
        _requests.push_back({request.method, request.path,
                             request.body.empty() ? nlohmann::json(nullptr) : nlohmann::json::parse(request.body)});
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
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[fake-llama-server]"`
Expected: the configure fetches cpp-httplib; builds with no warnings; `All tests passed` (3 test cases). If MSVC reports a warning located inside `httplib.h`, the SYSTEM include is not reaching that file: check that `morpheus_tests` gets `httplib.h` only through `morpheus_httplib`. If it reports `C1128: number of sections exceeded`, the `/bigobj` option is missing.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 377`.

- [ ] **Step 6: Check the dev configure**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: CMake re-runs (new option, new dependency), fetches cpp-httplib next to llama.cpp's vendored copy without a target clash, and builds with no warnings.

- [ ] **Step 7: Byte check and commit**

```bash
check_bytes CMakeLists.txt tests/CMakeLists.txt tests/fakes/FakeLlamaServer.h tests/llamaserver/FakeLlamaServerTests.cpp
git add CMakeLists.txt tests/CMakeLists.txt tests/fakes/FakeLlamaServer.h tests/llamaserver/FakeLlamaServerTests.cpp
git commit -m "Fetch cpp-httplib behind MORPHEUS_BUILD_LLAMA_SERVER; add a fake llama-server for the tests" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

