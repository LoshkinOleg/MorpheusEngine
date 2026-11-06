### Task 7: Engine text, reading the resolve output, applying it, the narrate message

**Files:**
- Create: `src/turn/EngineText.h`, `src/turn/Resolve.h`, `src/turn/Resolve.cpp`, `tests/turn/ResolveTests.cpp`
- Modify: `src/turn/CMakeLists.txt` (add `Resolve.cpp`)

**Interfaces:**
- Consumes:
  - `core::RollCheck`, `core::CheckSeed`, `core::ApplyChanges`
  - `context::RenderChangePart` (Task 5)
  - `store::EmbeddingKind` (Task 3)
- Produces:
  - `EngineText.h`: `inline constexpr std::string_view` constants `ENGINE_RULES`, `ASSISTED_RULES`, `RESOLVE_INSTRUCTION`, `NARRATE_INSTRUCTION`, `LOOKUP_INSTRUCTION`, `SUMMARIZE_INSTRUCTION`, `CONDENSE_INSTRUCTION`
  - `struct turn::ProposedCheck`, `struct turn::ResolveProposal`, `turn::ParseResolveOutput(std::string_view) -> ResolveProposal`
  - `struct turn::Resolution`, `turn::ApplyProposal(const core::World&, const nlohmann::json& state, const ResolveProposal&, std::uint64_t runSeed, int turn) -> Resolution`
  - `turn::RenderNarrateMessage(const ResolveProposal&, const Resolution&, int allowance, inference::LlmBackend&) -> std::string`
  - `struct turn::Lookup { store::EmbeddingKind source; std::string query; }`, `turn::ParseLookupOutput(std::string_view, std::string& error) -> std::vector<Lookup>`

- [ ] **Step 1: Write the failing tests**

Create `tests/turn/ResolveTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <format>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Dice.h"
#include "core/Errors.h"
#include "core/World.h"
#include "fakes/FakeBackend.h"
#include "turn/EngineText.h"
#include "turn/Resolve.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

core::World MiniWorld()
{
    return core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
}

constexpr const char* CLIMB = R"({"intent":"climb","checks":[{"stat":"agility","difficulty":"hard","stakes":"a fall",)"
                              R"("on_success":[{"op":"set","path":"player.location","value":"cliff top","reason":"climbs"}],)"
                              R"("on_failure":[{"op":"add","path":"player.health","value":-2,"reason":"falls"}]}],)"
                              R"("changes":[{"op":"set","path":"player.location","value":"cliff base","reason":"walks over"}]})";

} // namespace

TEST_CASE("ParseResolveOutput reads a proposal and reports unreadable output", "[resolve]")
{
    const turn::ResolveProposal proposal =
        turn::ParseResolveOutput(R"({"intent":"listen","checks":[],"changes":[],"notes":["a"]})");
    CHECK(proposal.error.empty());
    CHECK(proposal.intent == "listen");
    CHECK(proposal.changes == nlohmann::json::array());
    CHECK(proposal.notes == std::vector<std::string>{"a"});

    CHECK_THAT(turn::ParseResolveOutput(R"({"intent":"listen","checks":[)").error, ContainsSubstring("not complete JSON"));
    CHECK_THAT(turn::ParseResolveOutput(R"({"checks":[],"changes":[]})").error, ContainsSubstring("'intent'"));
    CHECK_THAT(turn::ParseResolveOutput(R"({"intent":"x","checks":[{"stat":1}],"changes":[]})").error,
               ContainsSubstring("check 1"));
    const turn::ResolveProposal bad = turn::ParseResolveOutput(R"({"intent":"x","checks":{},"changes":[]})");
    CHECK(bad.intent.empty()); // Nothing is kept from unreadable output.
}

TEST_CASE("ApplyProposal rolls checks and applies the matching branch after the unconditional changes", "[resolve]")
{
    const core::World world = MiniWorld();
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(CLIMB);
    REQUIRE(proposal.error.empty());
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);

    const core::CheckResult expected = core::RollCheck(world.dice, 3, 16, core::CheckSeed(42, 1, 0));
    REQUIRE(resolution.checks.size() == 1);
    CHECK(resolution.checks[0].stat == "agility");
    CHECK(resolution.checks[0].stakes == "a fall");
    CHECK(resolution.checks[0].result.rolls == expected.rolls);
    CHECK(resolution.checks[0].result.success == expected.success);

    const nlohmann::json& fields = resolution.state.at("player").at("fields");
    CHECK(fields.at("location") == (expected.success ? "cliff top" : "cliff base"));
    CHECK(fields.at("health") == (expected.success ? 10 : 8));
    REQUIRE(resolution.applied.size() == 2);
    CHECK(resolution.applied[0].change.reason == "walks over"); // Unconditional changes come first.

    // Same seed, same turn: the same rolls (regenerate keeps the dice).
    CHECK(turn::ApplyProposal(world, world.initialState, proposal, 42, 1).checks[0].result.rolls == expected.rolls);
}

TEST_CASE("A check with an unknown stat is rejected and keeps later checks' seeds", "[resolve]")
{
    const core::World world = MiniWorld();
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(
        R"({"intent":"x","checks":[)"
        R"({"stat":"strength","difficulty":"easy","stakes":"","on_success":[{"op":"set","path":"player.location","value":"x","reason":""}],"on_failure":[]},)"
        R"({"stat":"agility","difficulty":"easy","stakes":"","on_success":[],"on_failure":[]}],"changes":[]})");
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);

    REQUIRE(resolution.checks.size() == 1);
    CHECK(resolution.checks[0].result.rolls == core::RollCheck(world.dice, 3, 8, core::CheckSeed(42, 1, 1)).rolls);
    REQUIRE(resolution.rejected.size() == 1);
    CHECK_THAT(resolution.rejected[0].reason, ContainsSubstring("unknown stat 'strength'"));
    CHECK(resolution.state.at("player").at("fields").at("location") == "shore");
}

TEST_CASE("RenderNarrateMessage reports the resolution, then the narrate instruction", "[resolve]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(16384);
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(
        R"({"intent":"climb","checks":[],"changes":[{"op":"add","path":"player.health","value":-2,"reason":"scrapes"},)"
        R"({"op":"add","path":"player.inventory.oil","value":-1,"reason":"spills"}]})");
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);

    const std::string message = turn::RenderNarrateMessage(proposal, resolution, 512, backend);
    CHECK(message == "# Resolution\nIntent: climb\nApplied: player.health -2\n"
                     "Rejected: add player.inventory.oil (no item 'oil' to decrease)\n\n# Task\n" +
                         std::string(turn::NARRATE_INSTRUCTION));

    // Over the allowance, the rejected changes go first.
    const std::string squeezed = turn::RenderNarrateMessage(proposal, resolution, backend.CountTokens(message) - 1, backend);
    CHECK_THAT(squeezed, !ContainsSubstring("Rejected:"));
    CHECK_THAT(squeezed, ContainsSubstring("Applied: player.health -2"));
    CHECK_THROWS_AS(turn::RenderNarrateMessage(proposal, resolution, 5, backend), core::DataError);
}

TEST_CASE("RenderNarrateMessage shows each roll and an unreadable proposal", "[resolve]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(16384);
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(
        R"({"intent":"hop","checks":[{"stat":"agility","difficulty":"easy","stakes":"a slip","on_success":[],"on_failure":[]}],"changes":[]})");
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);
    const core::CheckResult roll = core::RollCheck(world.dice, 3, 8, core::CheckSeed(42, 1, 0));
    CHECK_THAT(turn::RenderNarrateMessage(proposal, resolution, 512, backend),
               ContainsSubstring(std::format("Check: agility vs easy (8): rolled {} +3 = {}, {}. Stakes: a slip\n"
                                             "Applied: none\n",
                                             roll.rolls.at(0), roll.total, roll.success ? "success" : "failure")));

    const turn::ResolveProposal unreadable = turn::ParseResolveOutput(R"({"intent":"hop")");
    const turn::Resolution nothing = turn::ApplyProposal(world, world.initialState, unreadable, 42, 1);
    CHECK(nothing.applied.empty());
    CHECK(nothing.state == world.initialState);
    CHECK_THAT(turn::RenderNarrateMessage(unreadable, nothing, 512, backend),
               ContainsSubstring("The action could not be resolved"));
}

TEST_CASE("ParseLookupOutput reads lookups and reports unreadable output", "[resolve]")
{
    std::string error;
    const std::vector<turn::Lookup> lookups = turn::ParseLookupOutput(
        R"({"lookups":[{"source":"lore","query":"keeper"},{"source":"history","query":"the key"},{"source":"notes","query":"debts"}]})",
        error);
    CHECK(error.empty());
    REQUIRE(lookups.size() == 3);
    CHECK(lookups[0].source == store::EmbeddingKind::Lore);
    CHECK(lookups[1].source == store::EmbeddingKind::Turn);
    CHECK(lookups[2].source == store::EmbeddingKind::Note);
    CHECK(lookups[1].query == "the key");

    CHECK(turn::ParseLookupOutput(R"({"lookups":[{"source":"dreams","query":"x"}]})", error).empty());
    CHECK_THAT(error, ContainsSubstring("source"));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: the build fails. `turn/Resolve.h` cannot be opened.

- [ ] **Step 3: Implement**

Create `src/turn/EngineText.h`:

```cpp
#pragma once

#include <string_view>

namespace morpheus::turn {

// L0: how the game works and the shape of the resolve output. World-independent: the world's stats, difficulties
// and entity types are in L1 (context::RenderWorldReference).
inline constexpr std::string_view ENGINE_RULES = R"(# Engine
You are the game master of a solo text role-playing game. A game engine runs the game with you: it keeps the game state and rolls the dice. You decide what the player's action attempts, and you narrate what happens.

Every turn has two steps.

Step 1, resolve. Reply with one JSON object and nothing else:
- "intent": what the player is trying to do, in a few words.
- "checks": the rolls the action needs, usually none or one. Call for a check only when the outcome is uncertain and failure would matter. A check names a "stat" and a "difficulty" from the game reference, the "stakes", and the changes that follow "on_success" and "on_failure".
- "changes": changes that happen whatever the dice say.
A change has an "op", a "path", usually a "value", and a "reason". "set" replaces a field. "add" adds to a number (a negative value subtracts), to a list, or to an item count at "entity.field.item". "remove" takes an item from a list or an item count. "create" makes a new entity of a creatable type. Change only what the action affects, and only entities and fields that exist in the state, except through "create".

Step 2, narrate. The engine rolls the checks, applies the changes and reports the resolution. Narrate the outcome for the player. The resolution is final: narrate what it says, failures and rejected changes included, and never contradict the state.

In the transcript, the bracketed line after a player's input is the engine's record of that turn: rolls first, then applied changes.)";

// Appended to L0 in assisted memory mode.
inline constexpr std::string_view ASSISTED_RULES = R"(# Memory
You keep notes for yourself. In step 1 the JSON object also has "notes": at most two short facts worth remembering many turns from now (a promise, a secret, a name). Leave it empty when nothing new matters. Before step 1 you may be asked for lookups: searches of the lore, of earlier turns no longer shown, or of your notes.)";

// T4 of the resolve call.
inline constexpr std::string_view RESOLVE_INSTRUCTION =
    "Step 1: resolve the player's action above. Reply with the JSON object only.";

// Ends the user message the narrate call appends, after the resolution record.
inline constexpr std::string_view NARRATE_INSTRUCTION =
    "Step 2: narrate this outcome for the player, following the world's narration rules. Prose only: no JSON, no "
    "lists, no dice or engine terms. End where the player can act.";

// T4 of the assisted-mode lookup call.
inline constexpr std::string_view LOOKUP_INSTRUCTION =
    "Before step 1: list what you need to look up to resolve the player's action, at most three searches. "
    "\"source\" is \"lore\" for world facts, \"history\" for earlier turns no longer shown, or \"notes\" for your "
    "notes; \"query\" says what to find. Reply with an empty list when the prompt already has what you need.";

// Ends a fold call: the system message, the oldest turns, then this.
inline constexpr std::string_view SUMMARIZE_INSTRUCTION =
    "Summarize the turns above for your own memory, not for the player. Keep names, places, items, promises, open "
    "threads and the order of causes and effects. Leave out dice, JSON and engine details. Plain prose, at most "
    "250 words.";

// Ends a condense call: the system message (whose story-so-far section holds every summary), then this.
inline constexpr std::string_view CONDENSE_INSTRUCTION =
    "Condense the story so far, from the system message, into one summary for your own memory, not for the "
    "player. Keep names, places, items, promises, open threads and the order of causes and effects. Plain prose, "
    "at most 300 words.";

} // namespace morpheus::turn
```

Create `src/turn/Resolve.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/TurnRecord.h"
#include "core/World.h"
#include "inference/Llm.h"
#include "store/RunStore.h"

namespace morpheus::turn {

struct ProposedCheck {
    std::string stat = {};
    std::string difficulty = {};
    std::string stakes = {};
    nlohmann::json onSuccess = nlohmann::json::array(); // Raw changes, validated when applied.
    nlohmann::json onFailure = nlohmann::json::array();
};

// The resolve call's output, read. When it cannot be read, `error` says why and everything else is empty: the turn
// goes on with nothing proposed (a model mistake, not an exception).
struct ResolveProposal {
    std::string intent = {};
    std::vector<ProposedCheck> checks = {};
    nlohmann::json changes = nlohmann::json::array(); // Raw changes, validated when applied.
    std::vector<std::string> notes = {};              // Assisted mode.
    std::string error = {};
};

// Reads the resolve output. Output cut off at the token limit, or not of the grammar's shape, gives `error`.
ResolveProposal ParseResolveOutput(std::string_view text);

// What the engine made of a proposal.
struct Resolution {
    nlohmann::json state = nullptr; // State after the applied changes.
    std::vector<core::CheckOutcome> checks = {};
    std::vector<core::AppliedChange> applied = {};
    std::vector<core::RejectedChange> rejected = {};
};

// Rolls the i-th proposed check with CheckSeed(runSeed, turn, i), so a regenerated turn keeps its rolls. Then
// applies the unconditional changes, followed by each rolled check's success or failure changes in check order.
// A check with an unknown stat or difficulty is rejected (raw: its stat, difficulty and stakes), and its changes
// are dropped. `state` must be validated state.
Resolution ApplyProposal(const core::World& world, const nlohmann::json& state, const ResolveProposal& proposal,
                         std::uint64_t runSeed, int turn);

// The user message the narrate call appends: the resolution record, then NARRATE_INSTRUCTION.
//   # Resolution
//   Intent: climb
//   Check: agility vs hard (16): rolled 11 +3 = 14, failure. Stakes: a fall
//   Applied: player.health -2
//   Rejected: add player.inventory.oil (no item 'oil' to decrease)
//
//   # Task
//   <NARRATE_INSTRUCTION>
// An unreadable proposal gets "The action could not be resolved (<error>). Nothing changed." instead. When the text
// exceeds `allowance` tokens (budget.narrate_allowance), the Rejected line goes first, then Applied entries from the
// end ("and N more"). Throws DataError if it still does not fit (narrate_allowance is too small).
std::string RenderNarrateMessage(const ResolveProposal& proposal, const Resolution& resolution, int allowance,
                                 inference::LlmBackend& backend);

// One assisted-mode lookup: where to search and for what.
struct Lookup {
    store::EmbeddingKind source = store::EmbeddingKind::Lore; // lore -> Lore, history -> Turn, notes -> Note.
    std::string query = {};
};

// Reads the lookup call's output. Unreadable output gives no lookups and sets `error` (a model mistake).
std::vector<Lookup> ParseLookupOutput(std::string_view text, std::string& error);

} // namespace morpheus::turn
```

Create `src/turn/Resolve.cpp`:

```cpp
#include "turn/Resolve.h"

#include <algorithm>
#include <cstddef>
#include <format>

#include "context/Transcript.h"
#include "core/Dice.h"
#include "core/Errors.h"
#include "turn/EngineText.h"

namespace morpheus::turn {

ResolveProposal ParseResolveOutput(std::string_view text)
{
    auto failed = [](std::string why) {
        ResolveProposal proposal;
        proposal.error = std::move(why);
        return proposal;
    };
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded()) {
        return failed("the output is not complete JSON; it may have hit resolve_max_tokens");
    }
    if (!json.is_object() || !json.contains("intent") || !json.at("intent").is_string()) {
        return failed("'intent' must be a string");
    }
    if (!json.contains("checks") || !json.at("checks").is_array()) {
        return failed("'checks' must be an array");
    }
    if (!json.contains("changes") || !json.at("changes").is_array()) {
        return failed("'changes' must be an array");
    }

    ResolveProposal proposal;
    proposal.intent = json.at("intent").get<std::string>();
    proposal.changes = json.at("changes");
    const nlohmann::json& checks = json.at("checks");
    for (std::size_t i = 0; i < checks.size(); ++i) {
        const nlohmann::json& check = checks[i];
        auto has = [&check](const char* key, bool string) {
            return check.contains(key) && (string ? check.at(key).is_string() : check.at(key).is_array());
        };
        if (!check.is_object() || !has("stat", true) || !has("difficulty", true) || !has("stakes", true) ||
            !has("on_success", false) || !has("on_failure", false)) {
            return failed(std::format("check {} needs strings 'stat', 'difficulty' and 'stakes' and arrays "
                                      "'on_success' and 'on_failure'",
                                      i + 1));
        }
        proposal.checks.push_back({check.at("stat").get<std::string>(), check.at("difficulty").get<std::string>(),
                                   check.at("stakes").get<std::string>(), check.at("on_success"),
                                   check.at("on_failure")});
    }
    if (json.contains("notes")) {
        const nlohmann::json& notes = json.at("notes");
        if (!notes.is_array() || !std::all_of(notes.begin(), notes.end(), [](const auto& n) { return n.is_string(); })) {
            return failed("'notes' must be an array of strings");
        }
        for (const nlohmann::json& note : notes) {
            proposal.notes.push_back(note.get<std::string>());
        }
    }
    return proposal;
}

Resolution ApplyProposal(const core::World& world, const nlohmann::json& state, const ResolveProposal& proposal,
                         std::uint64_t runSeed, int turn)
{
    MORPHEUS_REQUIRE(state.contains(world.checkEntity), "ApplyProposal: the state has no check entity");
    const nlohmann::json& statFields = state.at(world.checkEntity).at("fields");

    Resolution resolution;
    nlohmann::json changes = proposal.changes;
    std::vector<core::RejectedChange> badChecks;
    for (std::size_t i = 0; i < proposal.checks.size(); ++i) {
        const ProposedCheck& check = proposal.checks[i];
        const bool knownStat = std::find(world.stats.begin(), world.stats.end(), check.stat) != world.stats.end();
        const auto difficulty = world.difficulties.find(check.difficulty);
        if (!knownStat || difficulty == world.difficulties.end()) {
            const nlohmann::json raw = {{"stat", check.stat}, {"difficulty", check.difficulty}, {"stakes", check.stakes}};
            badChecks.push_back({raw, !knownStat ? std::format("unknown stat '{}'", check.stat)
                                                 : std::format("unknown difficulty '{}'", check.difficulty)});
            continue;
        }
        // The seed index is the check's position in the proposal, so one rejected check never shifts the others.
        core::CheckOutcome outcome = {check.stat, check.difficulty, check.stakes,
                                      core::RollCheck(world.dice, statFields.at(check.stat).get<int>(),
                                                      difficulty->second,
                                                      core::CheckSeed(runSeed, turn, static_cast<int>(i)))};
        const nlohmann::json& branch = outcome.result.success ? check.onSuccess : check.onFailure;
        changes.insert(changes.end(), branch.begin(), branch.end());
        resolution.checks.push_back(std::move(outcome));
    }

    core::ApplyResult applied = core::ApplyChanges(world.schema, state, changes);
    resolution.state = std::move(applied.state);
    resolution.applied = std::move(applied.applied);
    resolution.rejected = std::move(badChecks);
    resolution.rejected.insert(resolution.rejected.end(), applied.rejected.begin(), applied.rejected.end());
    return resolution;
}

std::string RenderNarrateMessage(const ResolveProposal& proposal, const Resolution& resolution, int allowance,
                                 inference::LlmBackend& backend)
{
    const std::string task = "\n# Task\n" + std::string(NARRATE_INSTRUCTION);
    if (!proposal.error.empty()) {
        return std::format("# Resolution\nThe action could not be resolved ({}). Nothing changed.\n{}", proposal.error,
                           task);
    }

    std::string checks;
    for (const core::CheckOutcome& check : resolution.checks) {
        std::string rolls;
        for (const int roll : check.result.rolls) {
            rolls += (rolls.empty() ? "" : " + ") + std::to_string(roll);
        }
        const std::string stat = check.result.statValue != 0 ? std::format(" {:+}", check.result.statValue) : "";
        checks += std::format("Check: {} vs {} ({}): rolled {}{} = {}, {}. Stakes: {}\n", check.stat, check.difficulty,
                              check.result.target, rolls, stat, check.result.total,
                              check.result.success ? "success" : "failure", check.stakes);
    }
    std::vector<std::string> applied;
    for (const core::AppliedChange& change : resolution.applied) {
        applied.push_back(context::RenderChangePart(change));
    }
    std::vector<std::string> rejected;
    for (const core::RejectedChange& change : resolution.rejected) {
        const nlohmann::json& raw = change.raw;
        const bool named = raw.is_object() && raw.contains("op") && raw.at("op").is_string() && raw.contains("path") &&
                           raw.at("path").is_string();
        rejected.push_back(named ? std::format("{} {} ({})", raw.at("op").get<std::string>(),
                                               raw.at("path").get<std::string>(), change.reason)
                                 : change.reason);
    }
    auto join = [](const std::vector<std::string>& parts, std::size_t count) {
        std::string text;
        for (std::size_t i = 0; i < count; ++i) {
            text += (i == 0 ? "" : "; ") + parts[i];
        }
        return text;
    };

    // Render, then shed detail until the message fits the narrate allowance: the rejected changes first, then
    // applied changes from the end.
    bool showRejected = !rejected.empty();
    std::size_t appliedShown = applied.size();
    for (;;) {
        std::string text = "# Resolution\nIntent: " + proposal.intent + "\n" + checks;
        if (applied.empty()) {
            text += "Applied: none\n";
        } else {
            text += "Applied: " + join(applied, appliedShown);
            if (appliedShown < applied.size()) {
                text += std::format("{}and {} more", appliedShown == 0 ? "" : "; ", applied.size() - appliedShown);
            }
            text += "\n";
        }
        if (showRejected) {
            text += "Rejected: " + join(rejected, rejected.size()) + "\n";
        }
        text += task;
        if (backend.CountTokens(text) <= allowance) {
            return text;
        }
        if (showRejected) {
            showRejected = false;
        } else if (appliedShown > 0) {
            --appliedShown;
        } else {
            throw core::DataError(std::format("narrate_allowance {} is too small for the resolution record ({} tokens)",
                                              allowance, backend.CountTokens(text)));
        }
    }
}

std::vector<Lookup> ParseLookupOutput(std::string_view text, std::string& error)
{
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("lookups") || !json.at("lookups").is_array()) {
        error = "the lookup output is not {\"lookups\": [...]}";
        return {};
    }
    std::vector<Lookup> lookups;
    for (const nlohmann::json& entry : json.at("lookups")) {
        if (!entry.is_object() || !entry.contains("source") || !entry.at("source").is_string() ||
            !entry.contains("query") || !entry.at("query").is_string()) {
            error = "each lookup needs a string 'source' and 'query'";
            return {};
        }
        const std::string source = entry.at("source").get<std::string>();
        Lookup lookup;
        if (source == "lore") {
            lookup.source = store::EmbeddingKind::Lore;
        } else if (source == "history") {
            lookup.source = store::EmbeddingKind::Turn;
        } else if (source == "notes") {
            lookup.source = store::EmbeddingKind::Note;
        } else {
            error = std::format("unknown lookup source '{}'", source);
            return {};
        }
        lookup.query = entry.at("query").get<std::string>();
        lookups.push_back(std::move(lookup));
    }
    return lookups;
}

} // namespace morpheus::turn
```

Notes:
- The message format is `"# Resolution\n...\n" + "\n# Task\n" + instruction`, which gives the blank line before `# Task` that the test expects.
- Add `#include <string>`, `<utility>` and `<vector>` where they are used.
- Add `Resolve.cpp` to `src/turn/CMakeLists.txt`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[resolve]"`
Expected: PASS.

Then run `ctest --preset tests`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/turn tests/turn/ResolveTests.cpp
git commit -m "Read resolve output, roll and apply it, and write the narrate message" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---
