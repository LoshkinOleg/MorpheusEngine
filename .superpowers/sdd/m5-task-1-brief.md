### Task 1: Settings keys and call kinds

The review and the compact transcript are switched and budgeted in `settings.json`. Every new key is required, like every other key. The two new call kinds get their own sampling seeds while the existing kinds keep theirs.

**Files:**
- Modify: `src/core/Settings.h`, `src/core/Settings.cpp`, `src/core/Dice.h`, `settings.example.json`
- Modify (not staged): `D:\MorpheusEngine\settings.json`
- Test: `tests/core/SettingsTests.cpp`, `tests/core/DiceTests.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `core::Settings::narrationCheck` (`bool`, default `false`), `compactTranscript` (`bool`, default `false`), `proseTurns` (`int`, default `2`), declared after `memoryMode`.
  - `core::BudgetSettings::reviewMaxTokens` and `reviewThinkingTokens` (`int`, default `0`), declared after `narrateThinkingTokens`, so the positional initializers in the tests (`settings.budget = {256, ...}`) stay valid.
  - `enum class core::CallKind { Lookup, Resolve, Narrate, Summarize, Review, Renarrate }`.

- [ ] **Step 1: Write the failing settings tests**

In `tests/core/SettingsTests.cpp`, replace `ValidSettingsJson()` with:

```cpp
// A valid settings document whose model paths are relative to the settings file's folder.
nlohmann::json ValidSettingsJson()
{
    return nlohmann::json::parse(R"({
        "chat_model": "models/chat.gguf",
        "embedding_model": "models/embed.gguf",
        "embedding_query_prefix": "search_query: ",
        "embedding_document_prefix": "search_document: ",
        "context_size": 16384,
        "gpu_layers": -1,
        "narration_check": true,
        "compact_transcript": true,
        "prose_turns": 2,
        "budget": {
            "resolve_max_tokens": 512, "narrate_max_tokens": 768, "review_max_tokens": 384,
            "resolve_thinking_tokens": 0, "narrate_thinking_tokens": 0, "review_thinking_tokens": 0,
            "narrate_allowance": 512, "tail_allowance": 2048,
            "engine_rules_cap": 1024, "world_cap": 4096, "summary_cap": 2048, "retrieval_cap": 768
        },
        "sampling": {
            "resolve": {"temperature": 0.3, "top_k": 40, "top_p": 0.9},
            "narrate": {"temperature": 0.8, "top_k": 40, "top_p": 0.95}
        }
    })");
}
```

Append after the test case "LoadSettings reads the thinking budgets":

```cpp
TEST_CASE("LoadSettings reads the narration review and compact transcript settings", "[settings]")
{
    nlohmann::json json = ValidSettingsJson();
    json["narration_check"] = false;
    json["prose_turns"] = 3;
    json["budget"]["review_thinking_tokens"] = MIN_THINKING_TOKENS;
    const Settings settings = LoadSettings(WriteSettings("settings_review", json));
    CHECK_FALSE(settings.narrationCheck);
    CHECK(settings.compactTranscript);
    CHECK(settings.proseTurns == 3);
    CHECK(settings.budget.reviewMaxTokens == 384);
    CHECK(settings.budget.reviewThinkingTokens == MIN_THINKING_TOKENS);

    SECTION("each key is required")
    {
        for (const std::string key : {"narration_check", "compact_transcript", "prose_turns"}) {
            nlohmann::json missing = ValidSettingsJson();
            missing.erase(key);
            CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_missing_" + key, missing)),
                              ContainsSubstring(key));
        }
        for (const std::string key : {"review_max_tokens", "review_thinking_tokens"}) {
            nlohmann::json missing = ValidSettingsJson();
            missing["budget"].erase(key);
            CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_missing_" + key, missing)),
                              ContainsSubstring(key));
        }
    }
    SECTION("prose_turns is at least 1")
    {
        json["prose_turns"] = 0;
        CHECK_THROWS_AS(LoadSettings(WriteSettings("settings_prose_turns_zero", json)), DataError);
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_prose_turns_zero", json)), ContainsSubstring("prose_turns"));
    }
    SECTION("review_thinking_tokens is 0 or at least the minimum")
    {
        json["budget"]["review_thinking_tokens"] = 10;
        CHECK_THROWS_AS(LoadSettings(WriteSettings("settings_review_thinking_small", json)), DataError);
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_thinking_small", json)),
                          ContainsSubstring("review_thinking_tokens") && ContainsSubstring("at least 64"));
    }
    SECTION("review_max_tokens is positive")
    {
        json["budget"]["review_max_tokens"] = 0;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_max_zero", json)),
                          ContainsSubstring("review_max_tokens"));
    }
    SECTION("the switches are true or false")
    {
        json["narration_check"] = "yes";
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_switch", json)),
                          ContainsSubstring("narration_check") && ContainsSubstring("true or false"));
    }
}
```

In the test case "SettingsToJson round-trips through LoadSettings", after `json["budget"]["narrate_thinking_tokens"] = 256;` add:

```cpp
    json["narration_check"] = false;
    json["prose_turns"] = 5;
    json["budget"]["review_max_tokens"] = 200;
    json["budget"]["review_thinking_tokens"] = 64;
```

and after `CHECK(copy.memoryMode == MemoryMode::Assisted);` add:

```cpp
    CHECK(copy.narrationCheck == original.narrationCheck);
    CHECK(copy.compactTranscript == original.compactTranscript);
    CHECK(copy.proseTurns == 5);
    CHECK(copy.budget.reviewMaxTokens == 200);
    CHECK(copy.budget.reviewThinkingTokens == 64);
```

In the test case "settings.example.json loads with the current loader", replace `CHECK_NOTHROW(LoadSettings(dir / "settings.json"));` with:

```cpp
    const Settings settings = LoadSettings(dir / "settings.json");
    // The example turns the narration review and the compact transcript on.
    CHECK(settings.narrationCheck);
    CHECK(settings.compactTranscript);
    CHECK(settings.proseTurns == 2);
    CHECK(settings.budget.reviewMaxTokens == 384);
    CHECK(settings.budget.reviewThinkingTokens == 0);
```

- [ ] **Step 2: Write the failing seed test**

Append to `tests/core/DiceTests.cpp`:

```cpp
TEST_CASE("SamplingSeed gives the review and renarrate calls their own seeds and keeps the others", "[dice]")
{
    // Review and Renarrate come after Summarize, so the older kinds keep the values that saved runs and replay
    // fixtures were sampled with.
    CHECK(static_cast<int>(CallKind::Lookup) == 0);
    CHECK(static_cast<int>(CallKind::Resolve) == 1);
    CHECK(static_cast<int>(CallKind::Narrate) == 2);
    CHECK(static_cast<int>(CallKind::Summarize) == 3);
    CHECK(static_cast<int>(CallKind::Review) == 4);
    CHECK(static_cast<int>(CallKind::Renarrate) == 5);

    std::set<std::uint32_t> seeds;
    for (const CallKind kind : {CallKind::Lookup, CallKind::Resolve, CallKind::Narrate, CallKind::Summarize,
                                CallKind::Review, CallKind::Renarrate}) {
        seeds.insert(SamplingSeed(42, 3, kind, 0));
    }
    CHECK(seeds.size() == 6);

    // A turn's second review adds 2^23 to the attempt (turn::REVIEW_RETRY_OFFSET): still a valid attempt, and a
    // seed apart from the first review's and from a regenerated turn's first review.
    const std::uint32_t retry = SamplingSeed(42, 3, CallKind::Review, 1 << 23);
    CHECK(retry != SamplingSeed(42, 3, CallKind::Review, 0));
    CHECK(retry != SamplingSeed(42, 3, CallKind::Review, 1));
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors: `narrationCheck`, `compactTranscript`, `proseTurns`, `reviewMaxTokens` and `reviewThinkingTokens` are not members, and `Review` / `Renarrate` are not members of `morpheus::core::CallKind`.

- [ ] **Step 4: Add the call kinds**

In `src/core/Dice.h`, replace the `CallKind` declaration and its comment with:

```cpp
// Which model call a sampling seed is for. New kinds are appended, so the existing ones keep their values (and every
// saved run and replay fixture its seeds).
enum class CallKind { Lookup, Resolve, Narrate, Summarize, Review, Renarrate };
```

- [ ] **Step 5: Add the settings fields**

In `src/core/Settings.h`, replace the last two fields of `BudgetSettings` (the comment line above them stays) with:

```cpp
    int resolveThinkingTokens = 0; // The resolve call's thinking block; 0 turns its thinking off.
    int narrateThinkingTokens = 0; // The narrate call's thinking block; 0 turns its thinking off.
    // The review call: its prompt is standalone and short, so neither is reserved in the resolve prompt's context.
    int reviewMaxTokens = 0;      // The review call's answer (its JSON).
    int reviewThinkingTokens = 0; // The review call's thinking block; 0 turns its thinking off.
```

Replace the `Settings` struct with:

```cpp
// settings.json: per-machine configuration. Not committed; settings.example.json documents it.
struct Settings {
    std::filesystem::path chatModel = {};      // Relative paths are resolved against the settings file's folder.
    std::filesystem::path embeddingModel = {};
    std::string embeddingQueryPrefix = {};     // Prepended to retrieval queries (model-specific).
    std::string embeddingDocumentPrefix = {};  // Prepended to embedded documents (model-specific).
    int contextSize = 0;
    int gpuLayers = 0;                         // -1 puts every layer on the GPU.
    BudgetSettings budget = {};
    SamplingSettings resolveSampling = {};
    SamplingSettings narrateSampling = {};
    std::optional<MemoryMode> memoryMode = {}; // Overrides the world's default memory_mode when set.
    // A review call after each narration checks it against the outcome; a contradiction is narrated once more.
    bool narrationCheck = false;
    // The review also writes the turn's digest, and older transcript turns show it instead of their narration.
    bool compactTranscript = false;
    int proseTurns = 2; // With compactTranscript: the newest turns that keep their narration in the transcript (>= 1).
};
```

Replace the `LoadSettings` comment with:

```cpp
// Loads and validates settings.json. Throws DataError naming the file and key at fault. Unknown keys are errors,
// both model files must exist, and the budget must leave room for the transcript (L3) and SETTINGS_TEMPLATE_MARGIN
// chat-template tokens; the review budgets are not part of that sum. memory_mode is optional; every other key is
// required.
```

- [ ] **Step 6: Load, validate and write the new keys**

In `src/core/Settings.cpp`:

Add `#include <limits>` to the includes.

After the `requireString` lambda, add:

```cpp
    auto requireBool = [&](const nlohmann::json& object, const char* key, const char* context) {
        const nlohmann::json& value = requireMember(object, key, context);
        if (!value.is_boolean()) {
            throw DataError(std::format("{}: {} '{}' must be true or false", where, context, key));
        }
        return value.get<bool>();
    };
```

Replace the top-level `checkKeys` call with:

```cpp
    checkKeys(json, "settings", {"chat_model", "embedding_model", "embedding_query_prefix", "embedding_document_prefix",
                                 "context_size", "gpu_layers", "budget", "sampling", "memory_mode", "narration_check",
                                 "compact_transcript", "prose_turns"});
```

After the `memory_mode` block (before `// Budget: every part positive and below the context size.`), add:

```cpp
    // The narration review, and the compact transcript that shows older turns as the review's digest.
    settings.narrationCheck = requireBool(json, "narration_check", "settings");
    settings.compactTranscript = requireBool(json, "compact_transcript", "settings");
    settings.proseTurns = requireInt(json, "prose_turns", "settings", 1, std::numeric_limits<int>::max());
```

Replace the budget `checkKeys` call with:

```cpp
    checkKeys(budget, "budget", {"resolve_max_tokens", "narrate_max_tokens", "review_max_tokens",
                                 "resolve_thinking_tokens", "narrate_thinking_tokens", "review_thinking_tokens",
                                 "narrate_allowance", "tail_allowance", "engine_rules_cap", "world_cap", "summary_cap",
                                 "retrieval_cap"});
```

After `settings.budget.narrateMaxTokens = requireInt(budget, "narrate_max_tokens", "budget", 1, contextSize);` add:

```cpp
    settings.budget.reviewMaxTokens = requireInt(budget, "review_max_tokens", "budget", 1, contextSize);
```

After `settings.budget.narrateThinkingTokens = requireThinking("narrate_thinking_tokens");` add:

```cpp
    settings.budget.reviewThinkingTokens = requireThinking("review_thinking_tokens");
```

Above `const long long fixed = ...`, add the comment line:

```cpp
    // The review call is standalone (its own short prompt), so its budgets are not reserved here.
```

In `SettingsToJson`, replace the `json` initializer's entries from `{"gpu_layers", settings.gpuLayers},` through the end of the `"budget"` entry with:

```cpp
        {"gpu_layers", settings.gpuLayers},
        {"narration_check", settings.narrationCheck},
        {"compact_transcript", settings.compactTranscript},
        {"prose_turns", settings.proseTurns},
        {"budget",
         {{"resolve_max_tokens", settings.budget.resolveMaxTokens},
          {"narrate_max_tokens", settings.budget.narrateMaxTokens},
          {"review_max_tokens", settings.budget.reviewMaxTokens},
          {"resolve_thinking_tokens", settings.budget.resolveThinkingTokens},
          {"narrate_thinking_tokens", settings.budget.narrateThinkingTokens},
          {"review_thinking_tokens", settings.budget.reviewThinkingTokens},
          {"narrate_allowance", settings.budget.narrateAllowance},
          {"tail_allowance", settings.budget.tailAllowance},
          {"engine_rules_cap", settings.budget.engineRulesCap},
          {"world_cap", settings.budget.worldCap},
          {"summary_cap", settings.budget.summaryCap},
          {"retrieval_cap", settings.budget.retrievalCap}}},
```

- [ ] **Step 7: Add the keys to settings.example.json**

In `settings.example.json`, replace

```json
  "gpu_layers": 24,
  "budget": {
    "resolve_max_tokens": 512,
    "narrate_max_tokens": 768,
    "resolve_thinking_tokens": 2048,
    "narrate_thinking_tokens": 1536,
```

with

```json
  "gpu_layers": 24,
  "narration_check": true,
  "compact_transcript": true,
  "prose_turns": 2,
  "budget": {
    "resolve_max_tokens": 512,
    "narrate_max_tokens": 768,
    "review_max_tokens": 384,
    "resolve_thinking_tokens": 2048,
    "narrate_thinking_tokens": 1536,
    "review_thinking_tokens": 0,
```

- [ ] **Step 8: Add the keys to this machine's settings.json (never staged)**

`D:\MorpheusEngine\settings.json` is gitignored and per machine; the app and `morpheus_play` refuse it without the new keys. Add, keeping every value it already has: after its `"gpu_layers"` line

```json
  "narration_check": true,
  "compact_transcript": true,
  "prose_turns": 2,
```

and in its `"budget"` object, after `"narrate_max_tokens"`, `"review_max_tokens": 384,` and after `"narrate_thinking_tokens"`, `"review_thinking_tokens": 0,`. Do not stage it.

- [ ] **Step 9: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[settings],[dice],[settings_edit]"`
Expected: all pass.

Then run the whole suite: `ctest --preset tests`
Expected: all pass. (The app tests copy `settings.example.json`; nothing reads the new keys yet.)

- [ ] **Step 10: Commit**

```bash
git add src/core/Settings.h src/core/Settings.cpp src/core/Dice.h settings.example.json tests/core/SettingsTests.cpp tests/core/DiceTests.cpp
git commit -m "Add the narration review and compact transcript settings" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

