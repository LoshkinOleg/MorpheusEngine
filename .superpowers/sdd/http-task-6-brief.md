### Task 6: LlamaServerBackend::Complete: streaming, thinking, slots, telemetry, cancel

`Complete` replaces the stub: one streamed `/completion` call, or two when the model thinks. The prompt goes as token ids (Task 5's `TokenizePrompt`), so a content never becomes control tokens; the request body carries the slot, `cache_prompt`, and the sampling of `LlamaCppBackend`'s chain with every other llama-server sampler neutral; the server's `timings` fill the telemetry; a stop closes the connection at once.

**Files:**
- Modify: `src/llamaserver/LlamaServerBackend.cpp`
- Test: `tests/llamaserver/LlamaServerCompleteTests.cpp`

**Interfaces:**
- Consumes: everything of Task 5; `test::FakeLlamaServer::CompletionScript` (Task 4); `inference::THINKING_*` and `CLOSE_THINKING_TEXT` (Task 5).
- Produces: `LlamaServerBackend::Complete` as `ChatModel::Complete` documents it. The body of each `/completion` request: `prompt` (an array of token ids), `n_predict`, the sampling (`samplers: ["top_k", "top_p", "temperature"]`, `top_k`, `top_p`, `min_keep: 1`, `temperature`, `dynatemp_range: 0`, `dynatemp_exponent: 1`, `seed`, and neutral `mirostat: 0`, `min_p: 0`, `typical_p: 1`, `top_n_sigma: -1`, `xtc_probability: 0`, `repeat_penalty: 1`, `repeat_last_n: 0`, `frequency_penalty: 0`, `presence_penalty: 0`, `dry_multiplier: 0`, `logit_bias: []`, `ignore_eos: false`, `n_probs: 0`), `id_slot` (`spec.slot`, or the configured slot when -1), `cache_prompt: true`, `stream: true`; `grammar` on the answer call when set; on the thinking call `stop: ["</think>"]` and `n_predict` = `thinkingMaxTokens` minus the closing sentence's tokens. The answer call after thinking sends the thinking call's prompt tokens, then the thinking text tokenized as plain text (`/tokenize`, `add_special` and `parse_special` false), then the end marker and blank line, or the closing sentence, as tokens. Errors: `ContractError` for `maxTokens <= 0`, a negative `thinkingMaxTokens`, one too small to close the block, a slot below -1; `InferenceError` for a prompt plus generation over `ContextSize()`, thinking asked of a model without a thinking mode, an HTTP error, a malformed stream, an unknown `stop_type`, a stream that ends early, and a server prompt count that differs from `/tokenize`'s.

- [ ] **Step 1: Write the failing tests**

Create `tests/llamaserver/LlamaServerCompleteTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "fakes/FakeLlamaServer.h"
#include "inference/Llm.h"
#include "llamaserver/LlamaServerBackend.h"
#include "llamaserver/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string YES_NO = "root ::= \"yes\" | \"no\"\n";

// `text`'s tokens by the fake's default tokenizer, as an array of ids.
std::vector<int> Tokens(std::string_view text)
{
    return test::FakeLlamaServer::DefaultTokens(std::string(text), false, false);
}

// The ids of `parts`, one after the other.
std::vector<int> Joined(std::initializer_list<std::vector<int>> parts)
{
    std::vector<int> tokens;
    for (const std::vector<int>& part : parts) {
        tokens.insert(tokens.end(), part.begin(), part.end());
    }
    return tokens;
}

} // namespace

TEST_CASE("LlamaServerBackend::Complete sends one streamed /completion request", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"yes"};
    chat.QueueCompletion(script);
    chat.QueueCompletion(script);

    inference::GenerationSpec spec;
    spec.grammar = YES_NO;
    spec.maxTokens = 16;
    spec.sampling = {0.5f, 20, 0.75f, 77};
    backend.Complete(test::SERVER_PROMPT, spec, {}, {});

    const std::vector<test::FakeLlamaServer::Request> requests = chat.RequestsTo("/completion");
    REQUIRE(requests.size() == 1);
    const nlohmann::json& body = requests[0].body;
    // The prompt as token ids: no content is ever parsed for control tokens.
    CHECK(body.at("prompt") == nlohmann::json(test::ServerPromptTokens()));
    CHECK(body.at("n_predict") == 16);
    CHECK(body.at("grammar") == YES_NO);
    // The sampling of LlamaCppBackend's chain: top-k, top-p keeping at least one token, temperature, then the draw.
    CHECK(body.at("samplers") == nlohmann::json::array({"top_k", "top_p", "temperature"}));
    CHECK(body.at("top_k") == 20);
    CHECK(body.at("top_p").get<double>() == 0.75);
    CHECK(body.at("min_keep") == 1);
    CHECK(body.at("temperature").get<double>() == 0.5);
    CHECK(body.at("dynatemp_range").get<double>() == 0.0);
    CHECK(body.at("dynatemp_exponent").get<double>() == 1.0);
    CHECK(body.at("seed") == 77);
    // Every other sampler llama-server knows is neutral, whatever its command line set.
    CHECK(body.at("mirostat") == 0);
    CHECK(body.at("min_p").get<double>() == 0.0);
    CHECK(body.at("typical_p").get<double>() == 1.0);
    CHECK(body.at("top_n_sigma").get<double>() == -1.0);
    CHECK(body.at("xtc_probability").get<double>() == 0.0);
    CHECK(body.at("repeat_penalty").get<double>() == 1.0);
    CHECK(body.at("repeat_last_n") == 0);
    CHECK(body.at("frequency_penalty").get<double>() == 0.0);
    CHECK(body.at("presence_penalty").get<double>() == 0.0);
    CHECK(body.at("dry_multiplier").get<double>() == 0.0);
    CHECK(body.at("logit_bias") == nlohmann::json::array());
    CHECK(body.at("ignore_eos") == false);
    CHECK(body.at("n_probs") == 0);
    CHECK(body.at("id_slot") == 1); // spec.slot -1: the configured slot.
    CHECK(body.at("cache_prompt") == true);
    CHECK(body.at("stream") == true);
    CHECK_FALSE(body.contains("stop"));

    // Without a grammar, none is sent.
    spec.grammar.clear();
    backend.Complete(test::SERVER_PROMPT, spec, {}, {});
    CHECK_FALSE(chat.RequestsTo("/completion").back().body.contains("grammar"));
}

TEST_CASE("LlamaServerBackend::Complete sends an explicit slot as id_slot", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"yes"};
    chat.QueueCompletion(script);

    inference::GenerationSpec spec;
    spec.slot = 2;
    backend.Complete(test::SERVER_PROMPT, spec, {}, {});
    CHECK(chat.RequestsTo("/completion").back().body.at("id_slot") == 2);
}

TEST_CASE("LlamaServerBackend::Complete streams the pieces in order and fills the telemetry from timings", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"The ", "gate ", "opens."};
    script.promptN = 4; // The slot's cache held all but 4 of the prompt's tokens.
    script.promptMs = 12.5;
    script.predictedMs = 40.0;
    chat.QueueCompletion(script);

    std::vector<std::string> pieces;
    const inference::Completion completion = backend.Complete(
        test::SERVER_PROMPT, inference::GenerationSpec{}, [&](std::string_view piece) { pieces.emplace_back(piece); }, {});
    CHECK(pieces == std::vector<std::string>{"The ", "gate ", "opens."});
    CHECK(completion.text == "The gate opens.");
    const int promptTokens = static_cast<int>(test::ServerPromptTokens().size());
    CHECK(completion.promptTokens == promptTokens);
    CHECK(completion.tokensEvaluated == 4);
    CHECK(completion.tokensReused == promptTokens - 4);
    CHECK(completion.tokensGenerated == 3);
    CHECK(completion.promptMs == 12.5);
    CHECK(completion.generateMs == 40.0);
    CHECK_FALSE(completion.stoppedAtLimit);
    CHECK_FALSE(completion.cancelled);
    CHECK(completion.thinking.empty());
    CHECK(completion.thinkingTokens == 0);
}

TEST_CASE("LlamaServerBackend::Complete reports an answer stopped at its token limit", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"The gate"};
    script.stopType = "limit";
    chat.QueueCompletion(script);
    CHECK(backend.Complete(test::SERVER_PROMPT, inference::GenerationSpec{}, {}, {}).stoppedAtLimit);
}

TEST_CASE("LlamaServerBackend::Complete fails on an HTTP error or a stream it cannot read", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"a"};
    auto complete = [&] { return backend.Complete(test::SERVER_PROMPT, inference::GenerationSpec{}, {}, {}); };

    SECTION("an HTTP error")
    {
        chat.SetStatus("/completion", 500);
        CHECK_THROWS_AS(complete(), inference::InferenceError);
        CHECK_THROWS_WITH(complete(), ContainsSubstring(chat.Url() + "/completion") && ContainsSubstring("500"));
    }
    SECTION("a line that is not a message")
    {
        script.extraLine = "event: nonsense";
        chat.QueueCompletion(script);
        CHECK_THROWS_WITH(complete(), ContainsSubstring("event: nonsense"));
    }
    SECTION("a message that is not JSON")
    {
        script.extraLine = "data: {not json";
        chat.QueueCompletion(script);
        CHECK_THROWS_WITH(complete(), ContainsSubstring("{not json"));
    }
    SECTION("a message without its fields")
    {
        script.extraLine = R"(data: {"stop":false})";
        chat.QueueCompletion(script);
        CHECK_THROWS_AS(complete(), inference::InferenceError);
    }
    SECTION("an error in the stream")
    {
        script.extraLine = R"(data: {"error":{"code":500,"message":"out of memory","type":"server_error"}})";
        chat.QueueCompletion(script);
        CHECK_THROWS_WITH(complete(), ContainsSubstring("out of memory"));
    }
    SECTION("a stream that ends before its last message")
    {
        script.endEarly = true;
        chat.QueueCompletion(script);
        CHECK_THROWS_WITH(complete(), ContainsSubstring("ended before its last message"));
    }
    SECTION("an unknown stop_type")
    {
        script.stopType = "tired";
        chat.QueueCompletion(script);
        CHECK_THROWS_WITH(complete(), ContainsSubstring("tired"));
    }
}

TEST_CASE("LlamaServerBackend::Complete checks its contract like the in-process backend", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    inference::GenerationSpec spec;

    spec.maxTokens = 0;
    CHECK_THROWS_AS(backend.Complete(test::SERVER_PROMPT, spec, {}, {}), core::ContractError);
    spec.maxTokens = 16;
    spec.thinkingMaxTokens = -1;
    CHECK_THROWS_AS(backend.Complete(test::SERVER_PROMPT, spec, {}, {}), core::ContractError);
    spec.thinkingMaxTokens = 0;
    spec.slot = -2;
    CHECK_THROWS_AS(backend.Complete(test::SERVER_PROMPT, spec, {}, {}), core::ContractError);
    spec.slot = -1;

    // The prompt plus the answer over the context size (8192): refused before anything is generated.
    spec.maxTokens = 8192;
    CHECK_THROWS_AS(backend.Complete(test::SERVER_PROMPT, spec, {}, {}), inference::InferenceError);
    CHECK_THROWS_WITH(backend.Complete(test::SERVER_PROMPT, spec, {}, {}), ContainsSubstring("exceeds the context size (8192)"));
    CHECK(chat.RequestsTo("/completion").empty());

    // The server must count the prompt as /tokenize does: the budgets rely on that count.
    spec.maxTokens = 16;
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"yes"};
    script.tokensEvaluated = 999;
    chat.QueueCompletion(script);
    CHECK_THROWS_AS(backend.Complete(test::SERVER_PROMPT, spec, {}, {}), inference::InferenceError);
    chat.QueueCompletion(script);
    CHECK_THROWS_WITH(backend.Complete(test::SERVER_PROMPT, spec, {}, {}), ContainsSubstring("999"));
}

TEST_CASE("LlamaServerBackend::Complete thinks in a first call, then answers in a second that extends it", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    chat.SetProps(test::ThinkingProps());
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    // The model closes its block: llama-server stops at the stop word "</think>" and leaves it out of the text.
    test::FakeLlamaServer::CompletionScript thought;
    thought.pieces = {"The gate ", "is shut."};
    thought.stopType = "word";
    thought.promptN = 5;
    thought.promptMs = 10.0;
    thought.predictedN = 4;
    thought.predictedMs = 30.0;
    chat.QueueCompletion(thought);
    test::FakeLlamaServer::CompletionScript answer;
    answer.pieces = {"no"};
    answer.promptMs = 2.0;
    answer.predictedMs = 8.0;
    chat.QueueCompletion(answer);

    inference::GenerationSpec spec;
    spec.grammar = YES_NO;
    spec.maxTokens = 16;
    spec.thinkingMaxTokens = 256;
    std::string streamed;
    const inference::Completion completion =
        backend.Complete(test::SERVER_PROMPT, spec, [&](std::string_view piece) { streamed += piece; }, {});

    const std::vector<test::FakeLlamaServer::Request> requests = chat.RequestsTo("/completion");
    REQUIRE(requests.size() == 2);
    // The prompt's tokens end with the open block (the marker tokenized with special tokens parsed).
    const std::vector<int> opened = Joined({test::ServerPromptTokens(), Tokens(inference::THINKING_OPEN_TEXT)});
    // The thinking call: the open block, no grammar, its budget less the closing sentence, stopping at the end marker.
    CHECK(requests[0].body.at("prompt") == nlohmann::json(opened));
    CHECK_FALSE(requests[0].body.contains("grammar"));
    CHECK(requests[0].body.at("n_predict") == 256 - static_cast<int>(Tokens(inference::CLOSE_THINKING_TEXT).size()));
    CHECK(requests[0].body.at("stop") == nlohmann::json::array({"</think>"}));
    // The answer call continues the same tokens on the same slot: the thinking text as plain text, the end marker and
    // the blank line, then the answer under the grammar.
    CHECK(requests[1].body.at("prompt") == nlohmann::json(Joined({opened, Tokens("The gate is shut."),
                                                                 Tokens(inference::THINKING_END_TEXT),
                                                                 Tokens(inference::THINKING_AFTER_END_TEXT)})));
    const std::vector<test::FakeLlamaServer::Request> tokenized = chat.RequestsTo("/tokenize");
    CHECK(std::any_of(tokenized.begin(), tokenized.end(), [](const test::FakeLlamaServer::Request& request) {
        return request.body == nlohmann::json{{"content", "The gate is shut."}, {"add_special", false}, {"parse_special", false}};
    }));
    CHECK(requests[1].body.at("grammar") == YES_NO);
    CHECK(requests[1].body.at("n_predict") == 16);
    CHECK_FALSE(requests[1].body.contains("stop"));
    CHECK(requests[1].body.at("id_slot") == requests[0].body.at("id_slot"));

    // The thinking stays out of the text and the stream.
    CHECK(completion.text == "no");
    CHECK(streamed == "no");
    CHECK(completion.thinking == "The gate is shut.");
    // The prompt's numbers come from the first call; the block's tokens are what the server generated plus the blank
    // line after the end marker; its time covers the answer call's reading of it.
    const int promptTokens = static_cast<int>(opened.size());
    CHECK(completion.promptTokens == promptTokens);
    CHECK(completion.tokensEvaluated == 5);
    CHECK(completion.tokensReused == promptTokens - 5);
    CHECK(completion.promptMs == 10.0);
    CHECK(completion.thinkingTokens == 4 + static_cast<int>(Tokens(inference::THINKING_AFTER_END_TEXT).size()));
    CHECK(completion.thinkingMs == 30.0 + 2.0);
    CHECK_FALSE(completion.thinkingStoppedAtLimit);
    CHECK(completion.tokensGenerated == 1);
    CHECK(completion.generateMs == 8.0);
}

TEST_CASE("LlamaServerBackend::Complete closes a thinking block that reaches its budget", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    chat.SetProps(test::ThinkingProps());
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    const int closeTokens = static_cast<int>(Tokens(inference::CLOSE_THINKING_TEXT).size());
    test::FakeLlamaServer::CompletionScript thought;
    thought.pieces = {"Hmm, "};
    thought.stopType = "limit";
    thought.predictedN = 256 - closeTokens;
    chat.QueueCompletion(thought);
    test::FakeLlamaServer::CompletionScript answer;
    answer.pieces = {"yes"};
    chat.QueueCompletion(answer);

    inference::GenerationSpec spec;
    spec.grammar = YES_NO;
    spec.thinkingMaxTokens = 256;
    const inference::Completion completion = backend.Complete(test::SERVER_PROMPT, spec, {}, {});

    const std::vector<test::FakeLlamaServer::Request> requests = chat.RequestsTo("/completion");
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].body.at("prompt") == nlohmann::json(Joined({test::ServerPromptTokens(), Tokens(inference::THINKING_OPEN_TEXT),
                                                                 Tokens("Hmm, "), Tokens(inference::CLOSE_THINKING_TEXT)})));
    CHECK(completion.thinkingStoppedAtLimit);
    CHECK(completion.thinkingTokens == 256);
    CHECK(completion.thinking == "Hmm, ");
    CHECK(completion.text == "yes");
}

TEST_CASE("LlamaServerBackend::Complete gives no answer when the model ends its reply while thinking", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    chat.SetProps(test::ThinkingProps());
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript thought;
    thought.pieces = {"Nothing to say."};
    thought.stopType = "eos";
    chat.QueueCompletion(thought);

    inference::GenerationSpec spec;
    spec.thinkingMaxTokens = 256;
    const inference::Completion completion = backend.Complete(test::SERVER_PROMPT, spec, {}, {});
    CHECK(chat.RequestsTo("/completion").size() == 1);
    CHECK(completion.thinking == "Nothing to say.");
    CHECK(completion.text.empty());
    CHECK_FALSE(completion.cancelled);
}

TEST_CASE("LlamaServerBackend::Complete refuses thinking it cannot do", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    inference::GenerationSpec spec;
    spec.thinkingMaxTokens = 256;

    // A model without a thinking mode: an InferenceError, as in-process, before anything is generated.
    llamaserver::LlamaServerBackend plain(test::ConfigFor(chat, embed));
    CHECK_THROWS_AS(plain.Complete(test::SERVER_PROMPT, spec, {}, {}), inference::InferenceError);
    CHECK_THROWS_WITH(plain.Complete(test::SERVER_PROMPT, spec, {}, {}), ContainsSubstring("no thinking mode"));
    CHECK(chat.RequestsTo("/completion").empty());

    // A budget that cannot hold the closing sentence: an engine bug.
    chat.SetProps(test::ThinkingProps());
    llamaserver::LlamaServerBackend thinking(test::ConfigFor(chat, embed));
    spec.thinkingMaxTokens = static_cast<int>(Tokens(inference::CLOSE_THINKING_TEXT).size());
    CHECK_THROWS_AS(thinking.Complete(test::SERVER_PROMPT, spec, {}, {}), core::ContractError);
}

TEST_CASE("A stop ends LlamaServerBackend::Complete mid-stream and closes the connection", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"The ", "gate"};
    script.holdOpen = true; // The stream stays open after the pieces, as a server still generating.
    chat.QueueCompletion(script);

    // The first piece asks for the stop.
    std::stop_source source;
    std::string streamed;
    const inference::Completion completion = backend.Complete(
        test::SERVER_PROMPT, inference::GenerationSpec{},
        [&](std::string_view piece) {
            streamed += piece;
            source.request_stop();
        },
        source.get_token());
    CHECK(completion.cancelled);
    CHECK(streamed == "The ");

    // llama-server cancels a task whose connection closed: the fake sees the close on one of its next pings.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!chat.ClientClosedStream() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(chat.ClientClosedStream());

    // A stop requested before the call sends no completion request.
    const inference::Completion early = backend.Complete(test::SERVER_PROMPT, inference::GenerationSpec{}, {}, source.get_token());
    CHECK(early.cancelled);
    CHECK(chat.RequestsTo("/completion").size() == 1);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "LlamaServerBackend::Complete*" "A stop ends LlamaServerBackend*"`
Expected: FAIL: most cases throw `InferenceError` "LlamaServerBackend::Complete is not implemented yet" (the contract test's first `CHECK_THROWS_AS(..., core::ContractError)` fails the same way).

- [ ] **Step 3: Write Complete**

In `src/llamaserver/LlamaServerBackend.cpp`, add `#include <exception>`, `#include <functional>` and `#include <stop_token>` to the standard includes, keeping them sorted.

In the anonymous namespace, after the `Ask` template, add:

```cpp

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

// Streams one POST /completion to `url`: each message's text goes to `onPiece` as it arrives, and the last message's
// stop_type and timings fill the result. A stop closes the connection at once, even while the server is still reading
// the prompt and sends nothing (llama-server then cancels the task), and the result is `cancelled`. Throws
// InferenceError naming the URL for an unreachable server, an HTTP error, a line that is not a message, a message
// without its fields, an error message in the stream, an unknown stop_type, or a stream that ends before its last
// message.
StreamResult StreamCompletion(const std::string& url, const nlohmann::json& body,
                              const std::function<void(std::string_view)>& onPiece, std::stop_token stop)
{
    StreamResult result;
    if (stop.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    httplib::Client client(url);
    SetTimeouts(client);
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

    const httplib::Result response = client.send(request);
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
```

Replace the stub

```cpp
inference::Completion LlamaServerBackend::Complete(const inference::Prompt& /*prompt*/,
                                                   const inference::GenerationSpec& /*spec*/,
                                                   const inference::TokenCallback& /*onToken*/, std::stop_token /*stop*/)
{
    throw InferenceError("LlamaServerBackend::Complete is not implemented yet");
}
```

with

```cpp
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
    // one token, temperature, then llama-server's draw with the seed (at temperature 0 only the most likely token is
    // left: greedy, as in-process). Every other sampler llama-server knows is named neutral, so its command-line
    // defaults (penalties, DRY, top-n-sigma, typical, min-p, XTC, mirostat, a logit bias) cannot change what the model
    // writes. Then the slot, whose cache the calls share, and the stream.
    nlohmann::json body = {{"prompt", tokens},
                           {"samplers", nlohmann::json::array({"top_k", "top_p", "temperature"})},
                           {"top_k", spec.sampling.topK},
                           {"top_p", spec.sampling.topP},
                           {"min_keep", 1},
                           {"temperature", spec.sampling.temperature},
                           {"dynatemp_range", 0.0},
                           {"dynatemp_exponent", 1.0},
                           {"seed", spec.sampling.seed},
                           {"mirostat", 0},
                           {"min_p", 0.0},
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
    // The first call reads the prompt: its numbers are the prompt's. The server must have evaluated exactly the tokens
    // sent, since the budgets rely on that count.
    auto takePromptNumbers = [&](const StreamResult& result) {
        if (result.promptTokens != completion.promptTokens) {
            throw InferenceError(std::format("{} counted {} prompt tokens where Morpheus sent {}",
                                             _config.chatUrl, result.promptTokens, completion.promptTokens));
        }
        completion.tokensEvaluated = result.promptEvaluated;
        completion.tokensReused = completion.promptTokens - result.promptEvaluated;
        completion.promptMs = result.promptMs;
    };

    // Think: one call without the grammar and without streaming to onToken, stopping at the end marker or at the
    // budget less the closing sentence. Its text is the thinking.
    if (think) {
        nlohmann::json thinking = body;
        thinking["n_predict"] = spec.thinkingMaxTokens - static_cast<int>(_thinking->close.size());
        thinking["stop"] = nlohmann::json::array({std::string(inference::THINKING_END_TEXT)});
        const StreamResult thought = StreamCompletion(
            _config.chatUrl, thinking, [&completion](std::string_view piece) { completion.thinking += piece; }, stop);
        if (thought.cancelled) {
            completion.cancelled = true;
            return completion;
        }
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
        _config.chatUrl, answer,
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
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver]"`
Expected: builds with no warnings; `All tests passed` (21 test cases).

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 400`.

- [ ] **Step 5: Run the AddressSanitizer preset on the new tests**

Run: `cmake --preset asan && cmake --build --preset asan && ./build/asan/bin/Debug/morpheus_tests.exe "[llamaserver],[fake-llama-server]"`
Expected: `All tests passed`, no AddressSanitizer report (the stream receiver, the stop callback and the fake's threads touch shared buffers).

- [ ] **Step 6: Byte check and commit**

```bash
check_bytes src/llamaserver/LlamaServerBackend.cpp tests/llamaserver/LlamaServerCompleteTests.cpp
git add src/llamaserver/LlamaServerBackend.cpp tests/llamaserver/LlamaServerCompleteTests.cpp
git commit -m "LlamaServerBackend::Complete: token prompts, in-process sampling, thinking in two calls, slots, telemetry, cancel" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

