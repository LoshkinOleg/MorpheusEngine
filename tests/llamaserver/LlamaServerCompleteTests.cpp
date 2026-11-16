#include <catch2/catch_approx.hpp>
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
#include "core/Thinking.h"
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
    spec.sampling = {0.5f, 20, 0.75f, 77, 0.05f};
    backend.Complete(test::SERVER_PROMPT, spec, {}, {});

    const std::vector<test::FakeLlamaServer::Request> requests = chat.RequestsTo("/completion");
    REQUIRE(requests.size() == 1);
    const nlohmann::json& body = requests[0].body;
    // The prompt as token ids: no content is ever parsed for control tokens.
    CHECK(body.at("prompt") == nlohmann::json(test::ServerPromptTokens()));
    CHECK(body.at("n_predict") == 16);
    CHECK(body.at("grammar") == YES_NO);
    // The sampling of LlamaCppBackend's chain: top-k, top-p keeping at least one token, min-p, temperature, then the
    // draw.
    CHECK(body.at("samplers") == nlohmann::json::array({"top_k", "top_p", "min_p", "temperature"}));
    CHECK(body.at("top_k") == 20);
    CHECK(body.at("top_p").get<double>() == 0.75);
    CHECK(body.at("min_p").get<double>() == Catch::Approx(0.05));
    CHECK(body.at("min_keep") == 1);
    CHECK(body.at("temperature").get<double>() == 0.5);
    CHECK(body.at("dynatemp_range").get<double>() == 0.0);
    CHECK(body.at("dynatemp_exponent").get<double>() == 1.0);
    CHECK(body.at("seed") == 77);
    // Every other sampler llama-server knows is neutral, whatever its command line set.
    CHECK(body.at("mirostat") == 0);
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
    const std::vector<int> opened = Joined({test::ServerPromptTokens(), Tokens(core::QWEN_THINKING.open)});
    // The thinking call: the open block, no grammar, its budget less the closing sentence, stopping at the end marker.
    CHECK(requests[0].body.at("prompt") == nlohmann::json(opened));
    CHECK_FALSE(requests[0].body.contains("grammar"));
    CHECK(requests[0].body.at("n_predict") == 256 - static_cast<int>(Tokens(core::QWEN_THINKING.close).size()));
    CHECK(requests[0].body.at("stop") == nlohmann::json::array({"</think>"}));
    // The answer call continues the same tokens on the same slot: the thinking text as plain text, the end marker and
    // the blank line, then the answer under the grammar.
    CHECK(requests[1].body.at("prompt") == nlohmann::json(Joined({opened, Tokens("The gate is shut."),
                                                                 Tokens(core::QWEN_THINKING.end),
                                                                 Tokens(core::QWEN_THINKING.afterEnd)})));
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
    CHECK(completion.thinkingTokens == 4 + static_cast<int>(Tokens(core::QWEN_THINKING.afterEnd).size()));
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
    const int closeTokens = static_cast<int>(Tokens(core::QWEN_THINKING.close).size());
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
    CHECK(requests[1].body.at("prompt") ==
          nlohmann::json(Joined({test::ServerPromptTokens(), Tokens(core::QWEN_THINKING.open), Tokens("Hmm, "),
                                 Tokens(core::QWEN_THINKING.close)})));
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
    spec.thinkingMaxTokens = static_cast<int>(Tokens(core::QWEN_THINKING.close).size());
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

TEST_CASE("LlamaServerBackend::Complete keeps its connection and sends a request once more when it fails unanswered",
          "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"yes"};
    chat.QueueCompletion(script);
    chat.QueueCompletion(script);
    chat.QueueCompletion(script);

    // Two calls in a row share one kept-alive connection.
    CHECK(backend.Complete(test::SERVER_PROMPT, inference::GenerationSpec{}, {}, {}).text == "yes");
    CHECK(backend.Complete(test::SERVER_PROMPT, inference::GenerationSpec{}, {}, {}).text == "yes");
    std::vector<test::FakeLlamaServer::Request> requests = chat.RequestsTo("/completion");
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].remotePort == requests[0].remotePort);

    // The server closes that connection as the next request leaves on it (llama-server closes one idle for 5 s, as
    // between two turns): not a byte of answer came, so the request goes again, over a new connection.
    chat.DropNextAnswers("/completion");
    CHECK(backend.Complete(test::SERVER_PROMPT, inference::GenerationSpec{}, {}, {}).text == "yes");
    requests = chat.RequestsTo("/completion");
    REQUIRE(requests.size() == 4);
    CHECK(requests[2].remotePort == requests[0].remotePort);
    CHECK(requests[3].remotePort != requests[0].remotePort);
}

TEST_CASE("LlamaServerBackend::Complete never sends again a request whose answer had begun", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"yes"};
    chat.QueueCompletion(script);
    CHECK(backend.Complete(test::SERVER_PROMPT, inference::GenerationSpec{}, {}, {}).text == "yes");

    // Over the kept connection, the stream breaks after its first piece: the server had started generating.
    test::FakeLlamaServer::CompletionScript broken;
    broken.pieces = {"The "};
    broken.dropAfterPieces = true;
    chat.QueueCompletion(broken);
    chat.QueueCompletion(script);
    std::string streamed;
    CHECK_THROWS_WITH(backend.Complete(test::SERVER_PROMPT, inference::GenerationSpec{},
                                       [&](std::string_view piece) { streamed += piece; }, {}),
                      ContainsSubstring(chat.Url() + "/completion"));
    CHECK(streamed == "The ");
    CHECK(chat.RequestsTo("/completion").size() == 2);
}

TEST_CASE("A stop from another thread ends LlamaServerBackend::Complete while the server sends nothing", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript script;
    script.silent = true; // A server still reading a long prompt: no piece, no ping.
    chat.QueueCompletion(script);

    std::stop_source source;
    const auto start = std::chrono::steady_clock::now();
    std::jthread stopper([&source, &chat] {
        // Stop only once the fake has received /completion (up to 5 s), so a slow first write cannot beat the stop.
        const auto sent = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (chat.RequestsTo("/completion").empty() && std::chrono::steady_clock::now() < sent) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        source.request_stop();
    });
    const inference::Completion completion =
        backend.Complete(test::SERVER_PROMPT, inference::GenerationSpec{}, {}, source.get_token());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(completion.cancelled);
    CHECK(!chat.RequestsTo("/completion").empty());
    // The stop closed the connection at once, not when the silent server gave up (10 s).
    CHECK(elapsed < std::chrono::seconds(5));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!chat.ClientClosedStream() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(chat.ClientClosedStream());
}

TEST_CASE("LlamaServerBackend::Complete checks the server's count of the answer's prompt after thinking", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    chat.SetProps(test::ThinkingProps());
    llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
    test::FakeLlamaServer::CompletionScript thought;
    thought.pieces = {"The gate is shut."};
    thought.stopType = "word";
    chat.QueueCompletion(thought);
    test::FakeLlamaServer::CompletionScript answer;
    answer.pieces = {"no"};
    answer.tokensEvaluated = 999;
    chat.QueueCompletion(answer);

    inference::GenerationSpec spec;
    spec.thinkingMaxTokens = 256;
    CHECK_THROWS_WITH(backend.Complete(test::SERVER_PROMPT, spec, {}, {}),
                      ContainsSubstring("999") && ContainsSubstring("prompt tokens"));
}

TEST_CASE("LlamaServerBackend::Complete thinks in the block its setting names", "[llamaserver][thinking]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    // A custom format on a template without the enable_thinking switch.
    const core::ThinkingFormat format = {"[T]", "", "[/T]", "\n", "Answer now.[/T]\n"};
    llamaserver::LlamaServerConfig config = test::ConfigFor(chat, embed);
    config.thinking = {core::ThinkingMode::Format, format};
    llamaserver::LlamaServerBackend backend(config);
    test::FakeLlamaServer::CompletionScript thought;
    thought.pieces = {"The gate is shut."};
    thought.stopType = "word";
    thought.predictedN = 4;
    chat.QueueCompletion(thought);
    test::FakeLlamaServer::CompletionScript answer;
    answer.pieces = {"no"};
    chat.QueueCompletion(answer);

    inference::GenerationSpec spec;
    spec.grammar = YES_NO;
    spec.maxTokens = 16;
    spec.thinkingMaxTokens = 256;
    const inference::Completion completion = backend.Complete(test::SERVER_PROMPT, spec, {}, {});

    const std::vector<test::FakeLlamaServer::Request> requests = chat.RequestsTo("/completion");
    REQUIRE(requests.size() == 2);
    // The thinking call: the prompt ends with the format's open text, it stops at the format's end marker, and its
    // budget is less the format's close text.
    CHECK(requests[0].body.at("prompt") == nlohmann::json(Joined({test::ServerPromptTokens(), Tokens("[T]")})));
    CHECK(requests[0].body.at("stop") == nlohmann::json::array({"[/T]"}));
    CHECK(requests[0].body.at("n_predict") == 256 - static_cast<int>(Tokens("Answer now.[/T]\n").size()));
    // The answer call continues with the format's end marker and its after-end text.
    CHECK(requests[1].body.at("prompt") ==
          nlohmann::json(Joined({test::ServerPromptTokens(), Tokens("[T]"), Tokens("The gate is shut."), Tokens("[/T]"),
                                 Tokens("\n")})));
    CHECK(completion.thinking == "The gate is shut.");
    CHECK(completion.text == "no");
}
