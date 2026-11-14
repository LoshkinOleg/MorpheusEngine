#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/Errors.h"
#include "fakes/ScriptedBackend.h"
#include "inference/Llm.h"
#include "server/SlotBackend.h"

using namespace morpheus;

namespace {

const inference::Prompt PROMPT = {{{inference::Role::System, "Be brief."}, {inference::Role::User, "Hi"}}, 1};

} // namespace

TEST_CASE("SlotBackend puts calls left on the default slot on its current slot, and keeps a slot a call chose",
          "[hosted-server][slots]")
{
    test::ScriptedBackend inner(4096,
                                [](const inference::Prompt&, const inference::GenerationSpec&) { return std::string("ok"); });
    server::SlotBackend backend(inner);
    CHECK(backend.Slot() == -1);
    inference::GenerationSpec spec;
    spec.maxTokens = 8;
    CHECK(backend.Complete(PROMPT, spec, {}, {}).text == "ok"); // No slot set yet: -1 stays.
    backend.SetSlot(2);
    backend.Complete(PROMPT, spec, {}, {});
    inference::GenerationSpec chosen = spec;
    chosen.slot = 1;
    backend.Complete(PROMPT, chosen, {}, {});
    REQUIRE(inner.Calls().size() == 3);
    CHECK(inner.Calls()[0].spec.slot == -1);
    CHECK(inner.Calls()[1].spec.slot == 2);
    CHECK(inner.Calls()[2].spec.slot == 1);
    CHECK(backend.Slot() == 2);
}

TEST_CASE("SlotBackend forwards counting, embedding and the model id, and refuses a slot below -1",
          "[hosted-server][slots]")
{
    test::ScriptedBackend inner(4096);
    server::SlotBackend backend(inner);
    CHECK(backend.CountTokens("abcdefgh") == inner.CountTokens("abcdefgh"));
    CHECK(backend.CountPromptTokens(PROMPT) == inner.CountPromptTokens(PROMPT));
    CHECK(backend.ContextSize() == 4096);
    CHECK(backend.Embed({"a lighthouse"}, inference::EmbedPurpose::Query) ==
          inner.Embed({"a lighthouse"}, inference::EmbedPurpose::Query));
    CHECK(backend.EmbeddingModelId() == inner.EmbeddingModelId());
    CHECK_THROWS_AS(backend.SetSlot(-2), core::ContractError);
}
