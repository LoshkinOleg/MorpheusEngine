#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <string_view>

#include "core/Errors.h"
#include "turn/EngineText.h"
#include "turn/PromptPack.h"

using namespace morpheus;

namespace {

// The message of the DataError PromptPack::FromHost throws for `texts`, or "" when it accepts them.
std::string PackError(const std::map<std::string, std::string>& texts)
{
    try {
        turn::PromptPack::FromHost(texts);
    } catch (const core::DataError& error) {
        return error.what();
    }
    return "";
}

} // namespace

TEST_CASE("A prompt pack defaults to the engine's hosted texts", "[prompts]")
{
    const turn::PromptPack pack;
    CHECK(pack.system == std::string(turn::HOSTED_SYSTEM_TEXT));
    CHECK(pack.resolve == std::string(turn::HOSTED_RESOLVE_INSTRUCTION));
    CHECK(pack.speak == std::string(turn::HOSTED_SPEAK_INSTRUCTION));
    CHECK(pack.turnCue == std::string(turn::HOSTED_TURN_CUE));
    CHECK(pack.summarize == std::string(turn::HOSTED_SUMMARIZE_INSTRUCTION));
    CHECK(pack.condense == std::string(turn::HOSTED_CONDENSE_INSTRUCTION));

    // Text names each text by its host-data key.
    CHECK(&pack.Text("system") == &pack.system);
    CHECK(&pack.Text("resolve") == &pack.resolve);
    CHECK(&pack.Text("speak") == &pack.speak);
    CHECK(&pack.Text("turn_cue") == &pack.turnCue);
    CHECK(&pack.Text("summarize") == &pack.summarize);
    CHECK(&pack.Text("condense") == &pack.condense);
    CHECK_THROWS_AS(pack.Text("narrate"), core::ContractError);

    // A host that sends no text gets the defaults; each default passes the host's checks.
    const turn::PromptPack fromHost = turn::PromptPack::FromHost({});
    for (const turn::PromptText& text : turn::PROMPT_TEXTS) {
        INFO(std::string(text.key));
        CHECK(fromHost.Text(text.key) == pack.Text(text.key));
        CHECK(PackError({{std::string(text.key), pack.Text(text.key)}}).empty());
    }
}

TEST_CASE("A host's texts replace the defaults they name and leave the others", "[prompts]")
{
    const turn::PromptPack pack = turn::PromptPack::FromHost(
        {{"resolve", "Act, {speaker}, while {player} waits."}, {"condense", "Shorten what {actor} remembers."}});
    CHECK(pack.resolve == "Act, {speaker}, while {player} waits.");
    CHECK(pack.condense == "Shorten what {actor} remembers.");
    CHECK(pack.system == std::string(turn::HOSTED_SYSTEM_TEXT));
    CHECK(pack.speak == std::string(turn::HOSTED_SPEAK_INSTRUCTION));
    CHECK(pack.turnCue == std::string(turn::HOSTED_TURN_CUE));
    CHECK(pack.summarize == std::string(turn::HOSTED_SUMMARIZE_INSTRUCTION));
}

TEST_CASE("A pack's faults name the text and the variable", "[prompts]")
{
    CHECK(PackError({{"narrate", "Say it."}}) ==
          "prompts/narrate: unknown text; the pack's texts are system, resolve, speak, turn_cue, summarize, condense");
    CHECK(PackError({{"speak", ""}}) == "prompts/speak: empty");
    CHECK(PackError({{"system", "You are {speaker}."}}) ==
          "prompts/system: unknown variable {speaker}; this text takes no variable");
    CHECK(PackError({{"resolve", "{actor} acts."}}) ==
          "prompts/resolve: unknown variable {actor}; this text takes {speaker}, {player}");
    CHECK(PackError({{"speak", "Say it {loudly}."}}) ==
          "prompts/speak: unknown variable {loudly}; this text takes {speaker}, {player}");
    CHECK(PackError({{"turn_cue", "({speaker} and {friend})"}}) ==
          "prompts/turn_cue: unknown variable {friend}; this text takes {speaker}, {player}");
    CHECK(PackError({{"summarize", "Remember, {speaker}."}}) ==
          "prompts/summarize: unknown variable {speaker}; this text takes {actor}");
    CHECK(PackError({{"condense", "{player}"}}) == "prompts/condense: unknown variable {player}; this text takes {actor}");

    // The first fault: an unknown key before any text; then texts in the table's order, variables in text order.
    CHECK(PackError({{"zzz", "x"}, {"system", ""}}) ==
          "prompts/zzz: unknown text; the pack's texts are system, resolve, speak, turn_cue, summarize, condense");
    CHECK(PackError({{"speak", "{a}"}, {"resolve", "{b} {c}"}}) ==
          "prompts/resolve: unknown variable {b}; this text takes {speaker}, {player}");
    CHECK(PackError({{"speak", ""}, {"resolve", "{b}"}}) ==
          "prompts/resolve: unknown variable {b}; this text takes {speaker}, {player}");
}

TEST_CASE("Braces that are not a variable are literal text", "[prompts]")
{
    const std::string system =
        R"(Reply like {"action": "wait", "params": {}}; {}, {Name}, {a-b} and { speaker } are text.)";
    const turn::PromptPack pack = turn::PromptPack::FromHost({{"system", system}});
    CHECK(pack.system == system);
    CHECK(turn::PromptPack::Fill(system, {}) == system);
}

TEST_CASE("Fill puts the names in every variable a text takes", "[prompts]")
{
    CHECK(turn::PromptPack::Fill("{speaker} greets {player}; {speaker} smiles.", {.speaker = "Hulda", .player = "Aela"}) ==
          "Hulda greets Aela; Hulda smiles.");
    CHECK(turn::PromptPack::Fill("What {actor} remembers.", {.actor = "Hulda"}) == "What Hulda remembers.");
    // A name that holds a variable is put in as written, once per variable.
    CHECK(turn::PromptPack::Fill("{actor} and {actor}", {.actor = "{actor} the Bold"}) ==
          "{actor} the Bold and {actor} the Bold");
    // A variable with no value, or one the engine does not fill, is an engine bug: FromHost refused it.
    CHECK_THROWS_AS(turn::PromptPack::Fill("{speaker} speaks.", {}), core::ContractError);
    CHECK_THROWS_AS(turn::PromptPack::Fill("{location}", {.speaker = "Hulda"}), core::ContractError);
}

TEST_CASE("The default texts do not invite naming the player and say most turns need no action", "[prompts]")
{
    CHECK(std::string(turn::HOSTED_SPEAK_INSTRUCTION) ==
          "Step 2: write what {speaker} says now. Spoken words only, in character.");
    CHECK(std::string(turn::HOSTED_SYSTEM_TEXT)
              .find("Choose what the speaker would do now. Most turns need no action: when the speaker would only talk, "
                    "choose the action that does nothing, if the turn allows one. The host decides whether it happens") !=
          std::string::npos);
}
