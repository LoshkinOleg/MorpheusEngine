### Task 9: The app's Settings, Turn log and Memory windows

The Settings window edits `narration_check`, `compact_transcript`, `prose_turns` and `review_thinking_tokens` (beside the other thinking budgets), and lists `review_max_tokens` with the read-only budget. The Turn log shows each review's verdict, problems and digest, the renarrate's call, and the narration it replaced. The Memory window shows each hit's reasons next to its score, and the latest turn's scene values. (The Play window already shows only the new narration after a restart: `RunModel` clears it, Task 5.)

**Files:**
- Modify: `src/app/model/SettingsEdit.h`, `src/app/model/SettingsEdit.cpp`, `src/app/model/Inspect.h`, `src/app/model/Inspect.cpp`, `src/app/ui/Windows.h`, `src/app/ui/SettingsWindow.cpp`, `src/app/ui/TurnLogWindow.cpp`, `src/app/ui/MemoryWindow.cpp`
- Test: `tests/app/SettingsEditTests.cpp`, `tests/app/InspectTests.cpp`, `tests/app/UiTests.cpp`

**Interfaces:**
- Consumes: the settings keys (Task 1); the telemetry `review`, `renarrate`, `retrieval[].reasons`, `scene` (Tasks 4 and 8); `turn::REVIEW_RULES`, `turn::RENARRATE_CORRECTION` (Task 3); `test::SceneWorldDir` (Task 8).
- Produces:
  - `app::SettingsEdit::reviewThinkingTokens` (`int`), `narrationCheck` (`bool`), `compactTranscript` (`bool`), `proseTurns` (`int`).
  - `std::vector<std::string> app::RetrievalRow::reasons`.
  - `struct app::ReviewRow { CallStats call; std::optional<bool> consistent; std::vector<std::string> problems; std::string digest; std::string error; }`.
  - `std::vector<app::ReviewRow> app::ReviewRows(const nlohmann::json& timings)`, `std::optional<std::string> app::ReplacedNarration(const nlohmann::json& timings)`, `std::vector<std::string> app::SceneValues(const nlohmann::json& timings)`.

- [ ] **Step 1: Write the failing settings edit tests**

In `tests/app/SettingsEditTests.cpp`, in "ReadSettingsEdit reads the edited values as the file writes them", after the `narrateThinkingTokens` check add:

```cpp
    CHECK(edit.reviewThinkingTokens == file.at("budget").at("review_thinking_tokens").get<int>());
    CHECK(edit.narrationCheck == file.at("narration_check").get<bool>());
    CHECK(edit.compactTranscript == file.at("compact_transcript").get<bool>());
    CHECK(edit.proseTurns == file.at("prose_turns").get<int>());
```

Append:

```cpp
TEST_CASE("SaveSettingsEdit writes the review settings and refuses prose_turns 0", "[settings_edit]")
{
    const std::filesystem::path dir = test::SettingsFolder("settings_edit_review");
    const std::filesystem::path file = dir / "settings.json";
    app::SettingsEdit edit = app::ReadSettingsEdit(file);
    edit.narrationCheck = false;
    edit.compactTranscript = false;
    edit.proseTurns = 3;
    edit.reviewThinkingTokens = 128;

    const core::Settings saved = app::SaveSettingsEdit(file, edit);
    CHECK_FALSE(saved.narrationCheck);
    CHECK_FALSE(saved.compactTranscript);
    CHECK(saved.proseTurns == 3);
    CHECK(saved.budget.reviewThinkingTokens == 128);
    CHECK(saved.budget.reviewMaxTokens == 384); // Not edited: kept.

    // The loader's rule reaches the player, and the file stays as it was.
    const std::string before = test::ReadFile(file);
    edit.proseTurns = 0;
    CHECK_THROWS_WITH(app::SaveSettingsEdit(file, edit), ContainsSubstring("prose_turns"));
    CHECK(test::ReadFile(file) == before);
}
```

- [ ] **Step 2: Write the failing inspector tests**

In `tests/app/InspectTests.cpp`, in "RetrievalRows shows each hit's text", replace the `timings` initializer with:

```cpp
    const nlohmann::json timings = nlohmann::json::parse(R"({"retrieval": [
        {"source": "lore", "ref": "keeper", "score": 0.8, "reasons": ["name", "scene"]},
        {"source": "history", "ref": "1", "score": 0.6, "reasons": []},
        {"source": "notes", "ref": "1.1", "score": 0.5, "reasons": []}]})");
```

after `CHECK(rows[0].score == 0.8);` add:

```cpp
    CHECK(rows[0].reasons == std::vector<std::string>{"name", "scene"});
    CHECK(rows[1].reasons.empty());
```

and replace the `gone` initializer with:

```cpp
    const nlohmann::json gone =
        nlohmann::json::parse(R"({"retrieval": [{"source": "history", "ref": "7", "score": 0.5, "reasons": []}]})");
```

Append:

```cpp
TEST_CASE("ReviewRows and ReplacedNarration read a renarrated turn's reviews", "[inspect]")
{
    const nlohmann::json timings = nlohmann::json::parse(R"({
        "review": [
            {"prompt_tokens": 400, "reused": 0, "evaluated": 400, "generated": 40, "prompt_ms": 5.0,
             "generate_ms": 300.0, "stopped_at_limit": false,
             "consistent": false, "problems": ["The climb failed."], "digest": "The player reached the top."},
            {"prompt_tokens": 410, "reused": 0, "evaluated": 410, "generated": 30, "prompt_ms": 5.0,
             "generate_ms": 250.0, "stopped_at_limit": false, "consistent": true, "digest": "The player slipped.",
             "thinking": {"tokens": 70, "ms": 900.0, "stopped_at_limit": false, "text": "Check the roll."}}],
        "renarrate": {"prompt_tokens": 1200, "reused": 1100, "evaluated": 100, "generated": 90, "prompt_ms": 20.0,
                      "generate_ms": 1500.0, "stopped_at_limit": false, "replaced": "You reach the top."}})");

    const std::vector<app::ReviewRow> rows = app::ReviewRows(timings);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].call.promptTokens == 400);
    CHECK(rows[0].consistent == false);
    CHECK(rows[0].problems == std::vector<std::string>{"The climb failed."});
    CHECK(rows[0].digest == "The player reached the top.");
    CHECK_FALSE(rows[0].call.thinking);
    CHECK(rows[1].consistent == true);
    CHECK(rows[1].problems.empty());
    REQUIRE(rows[1].call.thinking);
    CHECK(rows[1].call.thinking->text == "Check the roll.");
    CHECK(app::ReplacedNarration(timings) == "You reach the top.");
    const std::optional<app::CallStats> renarrate = app::CallStatsOf(timings, "renarrate");
    REQUIRE(renarrate);
    CHECK(renarrate->generated == 90);

    // An unreadable review keeps only its reason; a turn without reviews has none.
    const std::vector<app::ReviewRow> failed = app::ReviewRows(nlohmann::json::parse(R"({"review": [
        {"prompt_tokens": 400, "reused": 0, "evaluated": 400, "generated": 384, "prompt_ms": 5.0,
         "generate_ms": 3000.0, "stopped_at_limit": true, "error": "the review output is not complete JSON"}]})"));
    REQUIRE(failed.size() == 1);
    CHECK_FALSE(failed[0].consistent);
    CHECK(failed[0].call.stoppedAtLimit);
    CHECK(failed[0].error == "the review output is not complete JSON");
    CHECK(app::ReviewRows(nlohmann::json::object()).empty());
    CHECK_FALSE(app::ReplacedNarration(nlohmann::json::object()));
    CHECK_THROWS_AS(app::ReviewRows(nlohmann::json::parse(R"({"review": [{"consistent": true}]})")), core::DataError);
}

TEST_CASE("SceneValues reads the scene a turn's retrieval searched with", "[inspect]")
{
    CHECK(app::SceneValues(nlohmann::json::parse(R"({"scene": ["shore", "npc:marta"]})")) ==
          std::vector<std::string>{"shore", "npc:marta"});
    CHECK(app::SceneValues(nlohmann::json::parse(R"({"scene": []})")).empty());
    CHECK(app::SceneValues(nlohmann::json::object()).empty());
    CHECK_THROWS_AS(app::SceneValues(nlohmann::json::parse(R"({"scene": "shore"})")), core::DataError);
}
```

- [ ] **Step 3: Write the failing UI test**

In `tests/app/UiTests.cpp`, add to the includes:

```cpp
#include <algorithm>
#include <vector>

#include "SceneWorld.h"
#include "app/model/Inspect.h"
#include "turn/EngineText.h"
```

Inside the anonymous namespace, after `AssistedResponder`, add:

```cpp
const std::string RENARRATION = "You walk the last steps along the shore to the lighthouse door.";

// Resolve calls get MOVE. Each turn's first review finds a contradiction and its second none (the count is shared
// by every backend the factory makes). The first narration is NARRATION, the renarrate (its message carries the
// correction) RENARRATION.
test::ScriptedBackend::Responder RenarrateResponder()
{
    auto pReviews = std::make_shared<int>(0);
    return [pReviews](const inference::Prompt& prompt, const inference::GenerationSpec& spec) -> std::string {
        if (spec.grammar.find("intent") != std::string::npos) {
            return MOVE;
        }
        if (prompt.messages.front().content == turn::REVIEW_RULES) {
            return (*pReviews)++ % 2 == 0
                       ? R"({"consistent":false,"problems":["The player never reaches the lighthouse."],"digest":"The player set off."})"
                       : R"({"consistent":true,"problems":[],"digest":"The player walked to the lighthouse."})";
        }
        const bool renarrate = prompt.messages.back().content.find(turn::RENARRATE_CORRECTION) != std::string::npos;
        return renarrate ? RENARRATION : NARRATION;
    };
}
```

Append:

```cpp
TEST_CASE("The inspectors draw a renarrated turn and scene retrieval", "[ui]")
{
    test::HeadlessImGui imgui;
    // settings.example.json turns the narration check and the compact transcript on. The only world is the mini
    // world with a scene, whose player starts on the shore.
    const std::filesystem::path dir = test::SettingsFolder("ui_review");
    app::AppPaths paths = test::PathsIn(dir);
    paths.projectsDir = test::SceneWorldDir("ui_review_worlds").parent_path();
    app::AppController controller(paths, test::ScriptedFactory(RenarrateResponder()));
    app::Ui ui;

    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    controller.NewRun(controller.Worlds().at(0), "review", 7);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    controller.Submit("I look around.");
    test::PumpUntil(controller, [&] { return controller.CanPost(); });

    REQUIRE(controller.Run().History().size() == 1);
    const core::TurnRecord& turn = controller.Run().History().back();
    CHECK(turn.narration == RENARRATION);
    CHECK(app::ReplacedNarration(turn.timings) == NARRATION);
    CHECK(app::ReviewRows(turn.timings).size() == 2);
    CHECK(app::SceneValues(turn.timings) == std::vector<std::string>{"shore"});
    const std::vector<app::RetrievalRow> rows =
        app::RetrievalRows(turn.timings, controller.Run().History(), controller.Run().Memory().notes);
    const bool sceneHit = std::any_of(rows.begin(), rows.end(), [](const app::RetrievalRow& row) {
        return row.ref == "shore" && std::find(row.reasons.begin(), row.reasons.end(), "scene") != row.reasons.end();
    });
    CHECK(sceneHit);
    DrawFrames(imgui, controller, ui); // The Turn log with both reviews and the replaced narration; the Memory window
                                       // with the reasons and the scene; the Settings window with the new keys.
}
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors: `reviewThinkingTokens`, `narrationCheck`, `compactTranscript` and `proseTurns` are not members of `SettingsEdit`; `reasons` is not a member of `RetrievalRow`; `ReviewRows`, `ReviewRow`, `ReplacedNarration` and `SceneValues` are not members of `morpheus::app`.

- [ ] **Step 5: Read and save the new keys**

In `src/app/model/SettingsEdit.h`, replace the struct with:

```cpp
// The settings.json values the Settings window edits. Model paths stay as written in the file (often relative to
// its folder), so saving does not rewrite them as absolute paths.
struct SettingsEdit {
    std::string chatModel = {};
    std::string embeddingModel = {};
    int contextSize = 0;
    int gpuLayers = 0; // -1 puts every layer on the GPU.
    // The budget's thinking blocks, edited beside the model because they depend on it: a model without a thinking
    // mode needs all three at 0.
    int resolveThinkingTokens = 0;
    int narrateThinkingTokens = 0;
    int reviewThinkingTokens = 0;
    core::SamplingSettings resolveSampling = {};
    core::SamplingSettings narrateSampling = {};
    std::optional<core::MemoryMode> memoryMode = {}; // nullopt: each world's own default.
    bool narrationCheck = false;    // narration_check
    bool compactTranscript = false; // compact_transcript
    int proseTurns = 0;             // prose_turns
};
```

and in the `SaveSettingsEdit` comment replace `(the embedding prefixes, the budget)` with `(the embedding prefixes, the rest of the budget)`.

In `src/app/model/SettingsEdit.cpp`, in `ReadSettingsEdit` after the `narrateThinkingTokens` line add:

```cpp
        edit.reviewThinkingTokens = json.at("budget").at("review_thinking_tokens").get<int>();
```

and after the `narrateSampling` line add:

```cpp
        edit.narrationCheck = json.at("narration_check").get<bool>();
        edit.compactTranscript = json.at("compact_transcript").get<bool>();
        edit.proseTurns = json.at("prose_turns").get<int>();
```

In `SaveSettingsEdit` after the `narrate_thinking_tokens` line add:

```cpp
        json["budget"]["review_thinking_tokens"] = edit.reviewThinkingTokens;
```

and after the `json["sampling"]["narrate"]` line add:

```cpp
        json["narration_check"] = edit.narrationCheck;
        json["compact_transcript"] = edit.compactTranscript;
        json["prose_turns"] = edit.proseTurns;
```

- [ ] **Step 6: Read the reviews, reasons and scene from the telemetry**

In `src/app/model/Inspect.h`, replace `RetrievalRow` with:

```cpp
// One retrieval hit that reached the turn's resolve prompt.
struct RetrievalRow {
    std::string source = {}; // "lore", "history" or "notes"
    std::string ref = {};
    double score = 0.0;
    std::vector<std::string> reasons = {}; // "name", "scene"; empty for similarity alone.
    std::string text = {};                 // The lore subject, the turn's input, or the note.
};

// One review call of a turn, from its telemetry.
struct ReviewRow {
    CallStats call = {};
    std::optional<bool> consistent = {}; // Set when the narration check was on and the output was readable.
    std::vector<std::string> problems = {};
    std::string digest = {};
    std::string error = {}; // Why the review output could not be read.
};
```

Replace the `CallStatsOf` comment with:

```cpp
// The numbers of call `call` ("lookup", "resolve", "narrate" or "renarrate") in a turn's telemetry, with its thinking
// block when it had one; nullopt when the turn made no such call.
```

After the `RetrievalRows` declaration add:

```cpp
// A turn's review calls, in the order made: one, or two when the narration was renarrated. Empty when the turn made
// none. Throws DataError when an entry is malformed.
std::vector<ReviewRow> ReviewRows(const nlohmann::json& timings);

// The first narration of a renarrated turn, the one the second replaced; nullopt when the turn was not renarrated.
// Throws DataError when the renarrate entry has no replaced narration.
std::optional<std::string> ReplacedNarration(const nlohmann::json& timings);

// The scene values automatic retrieval searched with; empty when there were none. Throws DataError when the entry is
// not a list of strings.
std::vector<std::string> SceneValues(const nlohmann::json& timings);
```

In `src/app/model/Inspect.cpp`, add an anonymous namespace after the namespace opening:

```cpp
namespace {

// One call's numbers from its telemetry object, with its thinking block when it had one. Throws nlohmann's
// exceptions for a malformed object; the callers say which entry it was.
CallStats ReadCallStats(const nlohmann::json& numbers)
{
    CallStats stats{numbers.at("prompt_tokens").get<int>(), numbers.at("reused").get<int>(),
                    numbers.at("evaluated").get<int>(),     numbers.at("generated").get<int>(),
                    numbers.at("prompt_ms").get<double>(),  numbers.at("generate_ms").get<double>(),
                    numbers.at("stopped_at_limit").get<bool>()};
    // Only calls that were allowed to think carry a thinking block.
    if (numbers.contains("thinking")) {
        const nlohmann::json& thinking = numbers.at("thinking");
        stats.thinking = ThinkingStats{thinking.at("tokens").get<int>(), thinking.at("ms").get<double>(),
                                       thinking.at("stopped_at_limit").get<bool>(),
                                       thinking.at("text").get<std::string>()};
    }
    return stats;
}

} // namespace
```

Replace `CallStatsOf` with:

```cpp
std::optional<CallStats> CallStatsOf(const nlohmann::json& timings, std::string_view call)
{
    const std::string key(call);
    if (!timings.is_object() || !timings.contains(key)) {
        return std::nullopt;
    }
    try {
        return ReadCallStats(timings.at(key));
    } catch (const nlohmann::json::exception& error) {
        throw core::DataError(std::format("turn telemetry '{}': {}", key, error.what()));
    }
}
```

In `RetrievalRows`, after `row.score = hit.at("score").get<double>();` add:

```cpp
            row.reasons = hit.at("reasons").get<std::vector<std::string>>();
```

After `RetrievalRows`, add:

```cpp
std::vector<ReviewRow> ReviewRows(const nlohmann::json& timings)
{
    std::vector<ReviewRow> rows;
    if (!timings.is_object() || !timings.contains("review")) {
        return rows;
    }
    try {
        // Each entry is the call's numbers plus whatever the review said: its verdict and problems with the check on,
        // its digest with the compact transcript on, or why its output could not be read.
        for (const nlohmann::json& entry : timings.at("review")) {
            ReviewRow row;
            row.call = ReadCallStats(entry);
            if (entry.contains("consistent")) {
                row.consistent = entry.at("consistent").get<bool>();
            }
            row.problems = entry.value("problems", std::vector<std::string>());
            row.digest = entry.value("digest", std::string());
            row.error = entry.value("error", std::string());
            rows.push_back(std::move(row));
        }
    } catch (const nlohmann::json::exception& error) {
        throw core::DataError(std::format("turn telemetry 'review': {}", error.what()));
    }
    return rows;
}

std::optional<std::string> ReplacedNarration(const nlohmann::json& timings)
{
    if (!timings.is_object() || !timings.contains("renarrate")) {
        return std::nullopt;
    }
    const nlohmann::json& renarrate = timings.at("renarrate");
    if (!renarrate.is_object() || !renarrate.contains("replaced") || !renarrate.at("replaced").is_string()) {
        throw core::DataError("turn telemetry 'renarrate' has no 'replaced' narration");
    }
    return renarrate.at("replaced").get<std::string>();
}

std::vector<std::string> SceneValues(const nlohmann::json& timings)
{
    if (!timings.is_object() || !timings.contains("scene")) {
        return {};
    }
    try {
        return timings.at("scene").get<std::vector<std::string>>();
    } catch (const nlohmann::json::exception& error) {
        throw core::DataError(std::format("turn telemetry 'scene': {}", error.what()));
    }
}
```

Add `#include <utility>` to the includes of `src/app/model/Inspect.cpp`.

- [ ] **Step 7: Edit the new keys in the Settings window**

In `src/app/ui/SettingsWindow.cpp`, after

```cpp
            ImGui::InputInt("Narrate thinking tokens", &edit.narrateThinkingTokens, 256, 1024);
            ImGui::SetItemTooltip(THINKING_TOOLTIP, core::MIN_THINKING_TOKENS);
```

add

```cpp
            ImGui::InputInt("Review thinking tokens", &edit.reviewThinkingTokens, 256, 1024);
            ImGui::SetItemTooltip(THINKING_TOOLTIP, core::MIN_THINKING_TOKENS);
```

After the memory mode combo's closing brace (the `if (ImGui::Combo("Memory mode", ...)) { ... }` block), add:

```cpp
            // The narration review, and the compact transcript built from its digests.
            ImGui::SeparatorText("Narration review");
            ImGui::Checkbox("Narration check", &edit.narrationCheck);
            ImGui::SetItemTooltip("After each narration a review call checks it against the outcome; a narration "
                                  "that contradicts it is written once more.");
            ImGui::Checkbox("Compact transcript", &edit.compactTranscript);
            ImGui::SetItemTooltip("The review also writes a short digest of each turn, and older turns reach the "
                                  "prompt as their digest.");
            ImGui::BeginDisabled(!edit.compactTranscript);
            ImGui::InputInt("Prose turns", &edit.proseTurns);
            ImGui::SetItemTooltip("The newest turns that keep their full narration in the prompt. At least 1.");
            ImGui::EndDisabled();
```

Replace the budget `rows` array with:

```cpp
            const std::pair<const char*, int> rows[] = {
                {"resolve_max_tokens", budget.resolveMaxTokens}, {"narrate_max_tokens", budget.narrateMaxTokens},
                {"review_max_tokens", budget.reviewMaxTokens},   {"narrate_allowance", budget.narrateAllowance},
                {"tail_allowance", budget.tailAllowance},        {"engine_rules_cap", budget.engineRulesCap},
                {"world_cap", budget.worldCap},                  {"summary_cap", budget.summaryCap},
                {"retrieval_cap", budget.retrievalCap}};
```

- [ ] **Step 8: Show the reviews in the Turn log**

In `src/app/ui/Windows.h`, replace the `TurnLogWindow` and `MemoryWindow` comments with:

```cpp
// Every turn, newest first: input, intent, rolls, applied and rejected changes, notes, lookups, the narration
// review (verdicts, problems, digests and a replaced narration), per-call numbers and the raw resolve output.
```

```cpp
// The story summaries, the notes, and the latest turn's retrieval hits with their scores and reasons, and its scene.
```

Replace `src/app/ui/TurnLogWindow.cpp` with:

```cpp
#include "app/ui/Windows.h"

#include <cfloat>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include "app/model/Inspect.h"
#include "context/Transcript.h"
#include "core/Text.h"

namespace morpheus::app {

namespace {

constexpr std::size_t TITLE_INPUT_CHARS = 60; // The input shown in a turn's header, in bytes.
constexpr float RESOLVE_JSON_LINES = 12.0f;   // Height of the raw resolve output, in lines.
constexpr float THINKING_LINES = 16.0f;       // Height of a call's thinking text, in lines.
constexpr float REPLACED_LINES = 8.0f;        // Height of a replaced narration, in lines.
// Model calls with one telemetry entry each, in turn order. The reviews (a list) follow them.
constexpr const char* CALLS[] = {"lookup", "resolve", "narrate", "renarrate"};

} // namespace

void TurnLogWindow::Draw(AppController& controller)
{
    if (ImGui::Begin(TURN_LOG_WINDOW)) {
        const std::vector<core::TurnRecord>& history = controller.Run().History();
        if (history.empty()) {
            ImGui::TextDisabled("No turns yet.");
        }
        // Newest first; the latest starts open.
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            const core::TurnRecord& turn = *it;
            ImGui::PushID(turn.number);
            if (it == history.rbegin()) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            }
            // "###header" keeps the header's ID stable while its label changes (a regenerate adds an attempt).
            const std::string attempt = turn.attempt > 0 ? std::format(" (attempt {})", turn.attempt + 1) : std::string();
            const std::string title = std::format("Turn {}{}: {}###header", turn.number, attempt,
                                                  core::CutUtf8(turn.input, TITLE_INPUT_CHARS));
            if (ImGui::CollapsingHeader(title.c_str())) {
                // What the player asked and what the model made of it.
                ImGui::TextWrapped("Input: %s", turn.input.c_str());
                const nlohmann::json intent =
                    turn.resolve.is_object() ? turn.resolve.value("intent", nlohmann::json()) : nlohmann::json();
                ImGui::TextWrapped("Intent: %s", intent.is_string() ? intent.get<std::string>().c_str() : "(none)");

                if (!turn.checks.empty()) {
                    ImGui::SeparatorText("Checks");
                    for (const core::CheckOutcome& check : turn.checks) {
                        ImGui::Bullet();
                        ImGui::TextWrapped("%s (stakes: %s)", CheckText(check).c_str(), check.stakes.c_str());
                    }
                }
                if (!turn.applied.empty()) {
                    ImGui::SeparatorText("Applied");
                    for (const core::AppliedChange& applied : turn.applied) {
                        ImGui::Bullet();
                        ImGui::TextWrapped("%s - %s", context::RenderChangePart(applied).c_str(),
                                           applied.change.reason.c_str());
                    }
                }
                if (!turn.rejected.empty()) {
                    ImGui::SeparatorText("Rejected");
                    for (const core::RejectedChange& rejected : turn.rejected) {
                        TextWrappedColored(ERROR_COLOR, std::format("{} - {}", rejected.raw.dump(), rejected.reason));
                    }
                }
                if (!turn.notes.empty()) {
                    ImGui::SeparatorText("Notes");
                    for (const std::string& note : turn.notes) {
                        ImGui::Bullet();
                        ImGui::TextWrapped("%s", note.c_str());
                    }
                }

                // Assisted mode: what the lookup call asked for, and how much of it reached the prompt.
                const nlohmann::json& timings = turn.timings;
                if (timings.is_object() && timings.contains("lookup")) {
                    ImGui::SeparatorText("Lookups");
                    const nlohmann::json& lookup = timings.at("lookup");
                    for (const nlohmann::json& request : lookup.value("requests", nlohmann::json::array())) {
                        ImGui::Bullet();
                        ImGui::TextWrapped("%s \"%s\": %zu found, %zu reached the prompt",
                                           request.at("source").get<std::string>().c_str(),
                                           request.at("query").get<std::string>().c_str(), request.at("added").size(),
                                           request.value("kept", nlohmann::json::array()).size());
                    }
                    if (lookup.contains("error")) {
                        TextWrappedColored(ERROR_COLOR,
                                           "Unreadable lookup output: " + lookup.at("error").get<std::string>());
                    }
                }

                // The narration review: each review's verdict, its problems and digest, and the narration a renarrate
                // replaced.
                const std::vector<ReviewRow> reviews = ReviewRows(timings);
                if (!reviews.empty()) {
                    ImGui::SeparatorText("Review");
                    for (std::size_t i = 0; i < reviews.size(); ++i) {
                        const ReviewRow& review = reviews[i];
                        const char* verdict = !review.consistent ? "no verdict"
                                              : *review.consistent ? "consistent with the outcome"
                                                                   : "contradicts the outcome";
                        const std::string line = std::format("Review {}: {}", i + 1, verdict);
                        if (review.consistent == false) {
                            TextWrappedColored(WARNING_COLOR, line);
                        } else {
                            ImGui::TextWrapped("%s", line.c_str());
                        }
                        for (const std::string& problem : review.problems) {
                            ImGui::Bullet();
                            ImGui::TextWrapped("%s", problem.c_str());
                        }
                        if (!review.digest.empty()) {
                            ImGui::TextWrapped("Digest: %s", review.digest.c_str());
                        }
                        if (!review.error.empty()) {
                            TextWrappedColored(ERROR_COLOR, "Unreadable review output: " + review.error);
                        }
                    }
                    if (const std::optional<std::string> replaced = ReplacedNarration(timings);
                        replaced && ImGui::TreeNode("Replaced narration")) {
                        std::string text = *replaced;
                        ImGui::InputTextMultiline("##replaced", &text,
                                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * REPLACED_LINES),
                                                  ImGuiInputTextFlags_ReadOnly);
                        ImGui::TreePop();
                    }
                }

                // Each model call's numbers: one row per call, the reviews numbered.
                ImGui::SeparatorText("Calls");
                auto callRow = [](const std::string& label, const CallStats& stats) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(label.c_str());
                    if (stats.stoppedAtLimit) {
                        ImGui::SameLine();
                        ImGui::TextColored(WARNING_COLOR, "(hit max tokens)");
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", stats.promptTokens);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", stats.reused);
                    // A call that did not think shows dashes, not zeros.
                    ImGui::TableNextColumn();
                    if (stats.thinking) {
                        ImGui::Text("%d", stats.thinking->tokens);
                        if (stats.thinking->stoppedAtLimit) {
                            ImGui::SameLine();
                            ImGui::TextColored(WARNING_COLOR, "(hit budget)");
                        }
                    } else {
                        ImGui::TextDisabled("-");
                    }
                    ImGui::TableNextColumn();
                    if (stats.thinking) {
                        ImGui::Text("%.0f", stats.thinking->ms);
                    } else {
                        ImGui::TextDisabled("-");
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", stats.generated);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", stats.promptMs);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", stats.generateMs);
                };
                if (ImGui::BeginTable("calls", 8,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                    for (const char* header :
                         {"Call", "Prompt", "Reused", "Thinking", "Thinking ms", "Generated", "Prompt ms", "Generate ms"}) {
                        ImGui::TableSetupColumn(header);
                    }
                    ImGui::TableHeadersRow();
                    for (const char* call : CALLS) {
                        if (const std::optional<CallStats> stats = CallStatsOf(timings, call)) {
                            callRow(call, *stats);
                        }
                    }
                    for (std::size_t i = 0; i < reviews.size(); ++i) {
                        callRow(std::format("review {}", i + 1), reviews[i].call);
                    }
                    ImGui::EndTable();
                }
                if (timings.is_object() && timings.contains("resolve") && timings.at("resolve").contains("error")) {
                    TextWrappedColored(ERROR_COLOR, "Unreadable resolve output: " +
                                                        timings.at("resolve").at("error").get<std::string>());
                }

                // Each call's reasoning, as the model wrote it in its thinking block.
                auto thinkingNode = [](const std::string& label, const CallStats& stats) {
                    if (!stats.thinking) {
                        return;
                    }
                    if (ImGui::TreeNode(label.c_str(), "%s thinking", label.c_str())) {
                        std::string text = stats.thinking->text;
                        ImGui::InputTextMultiline("##thinking", &text,
                                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * THINKING_LINES),
                                                  ImGuiInputTextFlags_ReadOnly);
                        ImGui::TreePop();
                    }
                };
                for (const char* call : CALLS) {
                    if (const std::optional<CallStats> stats = CallStatsOf(timings, call)) {
                        thinkingNode(call, *stats);
                    }
                }
                for (std::size_t i = 0; i < reviews.size(); ++i) {
                    thinkingNode(std::format("review {}", i + 1), reviews[i].call);
                }

                // The resolve output exactly as the model wrote it.
                if (ImGui::TreeNode("Resolve output (JSON)")) {
                    std::string json = turn.resolve.dump(2);
                    ImGui::InputTextMultiline("##resolve", &json,
                                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * RESOLVE_JSON_LINES),
                                              ImGuiInputTextFlags_ReadOnly);
                    ImGui::TreePop();
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
```

- [ ] **Step 9: Show the reasons and the scene in the Memory window**

In `src/app/ui/MemoryWindow.cpp`, replace everything from `            // What retrieval put in the latest turn's prompt.` through the table's `ImGui::EndTable();` and the closing brace of that `else if` with:

```cpp
            // What retrieval put in the latest turn's prompt, why each hit matched, and the scene it searched with.
            ImGui::SeparatorText("Retrieved for the latest turn");
            const core::TurnRecord* pLatest = run.LatestTurn();
            // Values joined with `separator`, for the scene line and a hit's reasons.
            auto joined = [](const std::vector<std::string>& values, const char* separator) {
                std::string text;
                for (const std::string& value : values) {
                    text += (text.empty() ? "" : separator) + value;
                }
                return text;
            };
            const std::vector<std::string> scene =
                pLatest ? SceneValues(pLatest->timings) : std::vector<std::string>();
            if (!scene.empty()) {
                ImGui::TextWrapped("Scene: %s", joined(scene, "; ").c_str());
            }
            const std::vector<RetrievalRow> rows =
                pLatest ? RetrievalRows(pLatest->timings, run.History(), memory.notes) : std::vector<RetrievalRow>();
            if (rows.empty()) {
                ImGui::TextDisabled("Nothing retrieved.");
            } else if (ImGui::BeginTable("hits", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Ref", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Reasons", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const RetrievalRow& row : rows) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(row.source.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(row.ref.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", row.score);
                    // Similarity alone shows a dash.
                    ImGui::TableNextColumn();
                    if (row.reasons.empty()) {
                        ImGui::TextDisabled("-");
                    } else {
                        ImGui::TextUnformatted(joined(row.reasons, ", ").c_str());
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", row.text.c_str());
                }
                ImGui::EndTable();
            }
```

- [ ] **Step 10: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[settings_edit],[inspect],[ui],[run_model]"`
Expected: all pass. (`IM_ASSERT` throws, so an unbalanced `TreeNode`/`TreePop` or `BeginDisabled`/`EndDisabled` fails the `[ui]` tests.)

Then: `ctest --preset tests`
Expected: all pass.

- [ ] **Step 11: Commit**

```bash
git add src/app/model/SettingsEdit.h src/app/model/SettingsEdit.cpp src/app/model/Inspect.h src/app/model/Inspect.cpp src/app/ui/Windows.h src/app/ui/SettingsWindow.cpp src/app/ui/TurnLogWindow.cpp src/app/ui/MemoryWindow.cpp tests/app/SettingsEditTests.cpp tests/app/InspectTests.cpp tests/app/UiTests.cpp
git commit -m "Show the narration review and scene retrieval in the app" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---
