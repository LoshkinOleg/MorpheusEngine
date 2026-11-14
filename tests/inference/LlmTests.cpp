#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <type_traits>
#include <vector>

#include "core/Errors.h"
#include "fakes/FakeBackend.h"
#include "inference/Llm.h"

using namespace morpheus::inference;

TEST_CASE("RoleName spells roles as chat templates expect", "[inference]")
{
    CHECK(RoleName(Role::System) == "system");
    CHECK(RoleName(Role::User) == "user");
    CHECK(RoleName(Role::Assistant) == "assistant");
}

TEST_CASE("FakeBackend counts tokens deterministically through the interface", "[inference]")
{
    morpheus::test::FakeBackend fake(1000);
    LlmBackend& backend = fake;

    CHECK(backend.CountTokens("") == 0);
    CHECK(backend.CountTokens("abcd") == 1);
    CHECK(backend.CountTokens("abcde") == 2);

    Prompt prompt;
    prompt.messages = {{Role::System, "abcd"}, {Role::User, "abcdefgh"}};
    CHECK(backend.CountPromptTokens(prompt) == (1 + 4) + (2 + 4));
    CHECK(backend.ContextSize() == 1000);
    CHECK_THROWS_AS(backend.Complete(prompt, GenerationSpec{}, {}, {}), morpheus::core::ContractError);
}

TEST_CASE("LlmBackend is the union of Tokenizer, ChatModel and Embedder", "[inference]")
{
    static_assert(std::is_base_of_v<Tokenizer, LlmBackend>);
    static_assert(std::is_base_of_v<ChatModel, LlmBackend>);
    static_assert(std::is_base_of_v<Embedder, LlmBackend>);

    // A caller can hold one part alone, as sub-project D's code will.
    morpheus::test::FakeBackend fake(1000);
    Tokenizer& tokenizer = fake;
    CHECK(tokenizer.CountTokens("abcde") == 2);
    CHECK(tokenizer.ContextSize() == 1000);
    ChatModel& chat = fake;
    CHECK_THROWS_AS(chat.SaveState(), morpheus::core::ContractError);
    Embedder& embedder = fake;
    CHECK_THROWS_AS(embedder.EmbeddingModelId(), morpheus::core::ContractError);
}

TEST_CASE("GenerationSpec's slot defaults to the backend's default slot", "[inference]")
{
    CHECK(GenerationSpec{}.slot == -1);
}

TEST_CASE("ContentMarker picks a marker no content contains, and MarkContents puts it in every content", "[inference]")
{
    Prompt prompt;
    prompt.messages = {{Role::System, "rules"}, {Role::User, "hello"}};
    prompt.stableCount = 1;
    CHECK(ContentMarker(prompt) == "\x1E");
    prompt.messages[1].content = "a \x1E b";
    CHECK(ContentMarker(prompt) == "\x1E" "1\x1E");
    prompt.messages[0].content = "\x1E" "1\x1E";
    CHECK(ContentMarker(prompt) == "\x1E" "2\x1E");

    const Prompt marked = MarkContents(prompt, "\x1E" "2\x1E");
    REQUIRE(marked.messages.size() == 2);
    CHECK(marked.messages[0].role == Role::System);
    CHECK(marked.messages[0].content == "\x1E" "2\x1E");
    CHECK(marked.messages[1].content == "\x1E" "2\x1E");
    CHECK(marked.stableCount == 1);
}

TEST_CASE("TemplatePieces cuts a rendered prompt at its content markers", "[inference]")
{
    CHECK(TemplatePieces("<s>A:\x1E|B:\x1E|", "\x1E", 2) == std::vector<std::string>{"<s>A:", "|B:", "|"});
    CHECK(TemplatePieces("\x1E\x1E", "\x1E", 2) == std::vector<std::string>{"", "", ""});
    CHECK(TemplatePieces("only text", "\x1E", 0) == std::vector<std::string>{"only text"});
    CHECK_THROWS_AS(TemplatePieces("A:\x1E|B:\x1E|C:\x1E", "\x1E", 2), InferenceError);
    CHECK_THROWS_WITH(TemplatePieces("A:\x1E|B:\x1E|C:\x1E", "\x1E", 2), "the chat template repeats a message's content");
    CHECK_THROWS_WITH(TemplatePieces("A:\x1E|", "\x1E", 2), "the chat template drops a message's content");
}
