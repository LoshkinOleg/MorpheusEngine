#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/PromptBuilder.h"
#include "context/Transcript.h"
#include "core/Errors.h"
#include "fakes/FakeBackend.h"
#include "turn/EngineText.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

core::BudgetSettings RoomyBudget()
{
    return {64, 64, 48, 512, 256, 512, 256};
}

// An L3 entry of two messages: what came before the line, then the line.
context::TranscriptEntry Entry(int turn, const std::string& user, const std::string& assistant)
{
    context::TranscriptEntry entry;
    entry.turn = turn;
    entry.messages = {{inference::Role::User, user}, {inference::Role::Assistant, assistant}};
    entry.foldMessages = entry.messages;
    entry.fact = user + " " + assistant;
    return entry;
}

// Every layer filled.
context::HostedInputs Inputs()
{
    context::HostedInputs inputs;
    inputs.systemText = "HOSTED SYSTEM";
    inputs.lore = {{"Whiterun", "A city.", true}, {"Saadia", "Hides from hunters.", false}};
    inputs.participants = {{"0001A6A4", "Hulda", "A barkeep."}, {"00000007", "Aela", ""}};
    inputs.summaries = {{1, 2, "They met."}};
    inputs.transcript = {Entry(3, "Aela: Hello.", "Hulda [none]: Welcome.")};
    inputs.retrieved = {"Turn 1: Aela: A room, please."};
    inputs.observation = {{"location", "inn"}};
    inputs.events = {"Door opens."};
    inputs.playerLine = "Aela: Follow me.";
    inputs.instruction = "TASK";
    return inputs;
}

} // namespace

TEST_CASE("AssembleHostedPrompt lays out the hosted layers, most stable first", "[context][hosted]")
{
    test::FakeBackend backend(8192);
    const context::Assembly assembly = context::AssembleHostedPrompt(Inputs(), RoomyBudget(), backend);
    const inference::Prompt& prompt = assembly.prompt;
    REQUIRE(prompt.messages.size() == 4);
    CHECK(prompt.stableCount == 3);

    // L0, L1 (always-on lore only), the participants in join order, L2.
    CHECK(prompt.messages[0].role == inference::Role::System);
    CHECK(prompt.messages[0].content == "HOSTED SYSTEM\n\n"
                                        "# Lore\n- Whiterun: A city.\n\n"
                                        "# Participants\n- Hulda (0001A6A4): A barkeep.\n- Aela (00000007)\n\n"
                                        "# Story so far\nTurns 1-2: They met.\n");
    // L3.
    CHECK(prompt.messages[1].role == inference::Role::User);
    CHECK(prompt.messages[1].content == "Aela: Hello.");
    CHECK(prompt.messages[2].role == inference::Role::Assistant);
    CHECK(prompt.messages[2].content == "Hulda [none]: Welcome.");
    // The tail.
    CHECK(prompt.messages[3].role == inference::Role::User);
    CHECK(prompt.messages[3].content == "# Relevant\n- Turn 1: Aela: A room, please.\n\n"
                                        "# Observation\n{\"location\":\"inn\"}\n\n"
                                        "# New events\n(Door opens.)\n\n"
                                        "# Player\nAela: Follow me.\n\n"
                                        "# Task\nTASK");
}

TEST_CASE("AssembleHostedPrompt leaves out empty optional layers", "[context][hosted]")
{
    test::FakeBackend backend(8192);
    context::HostedInputs inputs = Inputs();
    inputs.lore = {{"Saadia", "Hides from hunters.", false}}; // Retrieved only: never in L1.
    inputs.participants = {{"0001A6A4", "Hulda", "A barkeep."}};
    inputs.summaries.clear();
    inputs.transcript.clear();
    inputs.retrieved.clear();
    inputs.events.clear();
    inputs.playerLine.clear();
    const context::Assembly assembly = context::AssembleHostedPrompt(inputs, RoomyBudget(), backend);
    REQUIRE(assembly.prompt.messages.size() == 2);
    CHECK(assembly.prompt.stableCount == 1);
    CHECK(assembly.prompt.messages[0].content == "HOSTED SYSTEM\n\n# Participants\n- Hulda (0001A6A4): A barkeep.\n");
    CHECK(assembly.prompt.messages[1].content == "# Observation\n{\"location\":\"inn\"}\n\n# Task\nTASK");

    // The system text, a participant and the instruction are required: their absence is an engine bug.
    inputs.participants.clear();
    CHECK_THROWS_AS(context::AssembleHostedPrompt(inputs, RoomyBudget(), backend), core::ContractError);
}

TEST_CASE("AssembleHostedPrompt counts and budgets like the resolve prompt", "[context][hosted]")
{
    const core::BudgetSettings budget = RoomyBudget();
    test::FakeBackend backend(8192);
    const context::Assembly assembly = context::AssembleHostedPrompt(Inputs(), budget, backend);
    CHECK(assembly.tokens.engineRules == backend.CountTokens("HOSTED SYSTEM"));
    CHECK(assembly.tokens.world == backend.CountTokens("# Lore\n- Whiterun: A city.\n\n# Participants\n"
                                                       "- Hulda (0001A6A4): A barkeep.\n- Aela (00000007)\n"));
    CHECK(assembly.tokens.summary == backend.CountTokens("# Story so far\nTurns 1-2: They met.\n"));
    CHECK(assembly.tokens.transcript ==
          backend.CountTokens("Aela: Hello.") + backend.CountTokens("Hulda [none]: Welcome."));
    CHECK(assembly.tokens.tail == backend.CountTokens(assembly.prompt.messages[3].content));
    CHECK(assembly.tokens.total == backend.CountPromptTokens(assembly.prompt));
    const int systemCost = backend.CountTokens(assembly.prompt.messages[0].content) + 4;
    CHECK(assembly.transcriptBudget == 8192 - 64 - 64 - 48 - 512 - systemCost);
    CHECK(assembly.transcriptCost ==
          backend.CountTokens("Aela: Hello.") + 4 + backend.CountTokens("Hulda [none]: Welcome.") + 4);
    CHECK(assembly.Fits());

    // The flags the shared upkeep acts on.
    context::HostedInputs longTail = Inputs();
    longTail.playerLine = "Aela: " + std::string(4000, 'y');
    CHECK(context::AssembleHostedPrompt(longTail, budget, backend).tailOverBudget);
    context::HostedInputs longSummary = Inputs();
    longSummary.summaries.front().text = std::string(2000, 's');
    CHECK(context::AssembleHostedPrompt(longSummary, budget, backend).summaryOverBudget);
    context::HostedInputs longTranscript = Inputs();
    longTranscript.transcript.front().messages.back().content = std::string(40000, 'x');
    const context::Assembly over = context::AssembleHostedPrompt(longTranscript, budget, backend);
    CHECK(over.transcriptOverBudget);
    CHECK_FALSE(over.Fits());

    // Lore and participants over world_cap are the host's data error, the system text over engine_rules_cap an engine
    // bug, and a context with no room for L3 a data error.
    core::BudgetSettings smallWorld = budget;
    smallWorld.worldCap = 2;
    CHECK_THROWS_AS(context::AssembleHostedPrompt(Inputs(), smallWorld, backend), core::DataError);
    CHECK_THROWS_WITH(context::AssembleHostedPrompt(Inputs(), smallWorld, backend),
                      ContainsSubstring("host lore and participants use"));
    core::BudgetSettings smallRules = budget;
    smallRules.engineRulesCap = 1;
    CHECK_THROWS_AS(context::AssembleHostedPrompt(Inputs(), smallRules, backend), core::ContractError);
    test::FakeBackend tiny(600);
    CHECK_THROWS_WITH(context::AssembleHostedPrompt(Inputs(), budget, tiny), ContainsSubstring("no room for the transcript"));
}

TEST_CASE("A join changes the hosted prompt only from the participants layer on", "[context][hosted]")
{
    test::FakeBackend backend(8192);
    context::HostedInputs joinedInputs = Inputs();
    joinedInputs.participants.push_back({"0001B07C", "Saadia", "Serves at the inn."});
    const context::Assembly before = context::AssembleHostedPrompt(Inputs(), RoomyBudget(), backend);
    const context::Assembly joined = context::AssembleHostedPrompt(joinedInputs, RoomyBudget(), backend);
    const std::string& a = before.prompt.messages[0].content;
    const std::string& b = joined.prompt.messages[0].content;

    // The first difference lies past the start of the participants layer: L0 and L1 stay cached.
    const std::size_t participantsAt = a.find("# Participants\n");
    REQUIRE(participantsAt != std::string::npos);
    std::size_t same = 0;
    while (same < a.size() && same < b.size() && a[same] == b[same]) {
        ++same;
    }
    CHECK(a != b);
    CHECK(same > participantsAt);
    CHECK_THAT(b, ContainsSubstring("- Aela (00000007)\n- Saadia (0001B07C): Serves at the inn.\n"));

    // L3 and the tail are as they were.
    REQUIRE(joined.prompt.messages.size() == before.prompt.messages.size());
    for (std::size_t i = 1; i < before.prompt.messages.size(); ++i) {
        CHECK(joined.prompt.messages[i].content == before.prompt.messages[i].content);
    }
}

TEST_CASE("A hosted prompt keeps its stable prefix when the conversation grows by a turn", "[context][hosted]")
{
    test::FakeBackend backend(8192);
    context::HostedInputs nextInputs = Inputs();
    nextInputs.transcript.push_back(Entry(4, "(Door opens.)\nAela: Follow me.", "Hulda [follow]: Lead the way."));
    nextInputs.events.clear();
    nextInputs.playerLine = "Aela: Faster.";
    const context::Assembly first = context::AssembleHostedPrompt(Inputs(), RoomyBudget(), backend);
    const context::Assembly next = context::AssembleHostedPrompt(nextInputs, RoomyBudget(), backend);
    REQUIRE(next.prompt.stableCount == first.prompt.stableCount + 2);
    for (std::size_t i = 0; i < first.prompt.stableCount; ++i) {
        CHECK(next.prompt.messages[i].role == first.prompt.messages[i].role);
        CHECK(next.prompt.messages[i].content == first.prompt.messages[i].content);
    }
}

TEST_CASE("The default hosted texts are generic, fit their cap and use only the engine's variables",
          "[context][hosted]")
{
    // L0 has no variable, so it never changes between turns.
    CHECK(turn::HOSTED_SYSTEM_TEXT.find('{') == std::string_view::npos);
    CHECK(turn::HOSTED_RESOLVE_INSTRUCTION.find("{speaker}") != std::string_view::npos);
    CHECK(turn::HOSTED_SPEAK_INSTRUCTION.find("{speaker}") != std::string_view::npos);
    CHECK(turn::HOSTED_SPEAK_INSTRUCTION.find("{player}") != std::string_view::npos);
    CHECK(std::string(turn::HOSTED_TURN_CUE) == "({speaker} speaks.)");
    // Every brace opens one of the two variables the engine fills.
    for (const std::string_view text :
         {turn::HOSTED_RESOLVE_INSTRUCTION, turn::HOSTED_SPEAK_INSTRUCTION, turn::HOSTED_TURN_CUE}) {
        for (std::size_t at = text.find('{'); at != std::string_view::npos; at = text.find('{', at + 1)) {
            const std::string_view rest = text.substr(at);
            CHECK((rest.starts_with("{speaker}") || rest.starts_with("{player}")));
        }
    }
    // L0 fits settings.example.json's engine_rules_cap (1024) as the fast suite counts tokens.
    test::FakeBackend backend(8192);
    CHECK(backend.CountTokens(turn::HOSTED_SYSTEM_TEXT) <= 1024);
}
