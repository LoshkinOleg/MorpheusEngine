# Generation Controls (Sub-project G2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Three controls over model calls: a `min_p` sampling field, an explicit `callTag` on every call the turn layer makes (read by the replay and the app instead of guessing from grammar text), and the thinking block's markers as settings data (`thinking_format`: auto, qwen, none or custom texts).

**Architecture:** `min_p` runs from settings.json through `core::SamplingSettings` and `inference::Sampling` into both backends' samplers. `inference::GenerationSpec::callTag` is set by `Session`, `HostedSession` and `MemoryUpkeep`; `turn::CallKindOf(spec)` and `app::SeatLabelOf(spec)` read it. `core::ThinkingFormat` (five texts) and `core::ThinkingSetting` live in a new `core/Thinking.{h,cpp}` with the Qwen preset and `ResolveThinking`; settings.json's `thinking_format` fills `Settings::thinking`; both backends' configs take it and resolve it when they load, replacing the `THINKING_*` constants of `inference`.

**Tech Stack:** C++20, nlohmann::json, llama.cpp, cpp-httplib, Catch2, CMake presets (`tests`, `asan`, `dev`).

Spec: `docs/superpowers/specs/2026-11-16-generation-controls-design.md`.

## Global Constraints

- `min_p`: optional in each settings.json `sampling` entry, a number in [0, 1), absent = 0. Fault: `<file>: sampling '<call>': 'min_p' must be at least 0 and below 1, not <value>` (`<call>` is `resolve` or `narrate`). New struct fields go LAST (`core::SamplingSettings::minP` after `topP`; `inference::Sampling::minP` after `seed`) so positional initializers stay valid.
- In-process chain: top-k, top-p, min-p (`llama_sampler_init_min_p(minP, 1)`), temperature, draw; greedy stays greedy. llama-server: `samplers` = `["top_k", "top_p", "min_p", "temperature"]`, `min_p` = the value.
- Call tags (`GenerationSpec::callTag`, a `std::string`): `Session`: `lookup`, `resolve`, `narrate`, `renarrate`, `review`; `HostedSession`: `decide`, `resolve`, `speak`; `MemoryUpkeep`: `summarize`, `condense`.
- `turn::CallKindOf(const inference::GenerationSpec&)`: `lookup`->Lookup, `resolve`->Resolve, `narrate` and `renarrate`->Narrate (a renarrate's replies join the narrate replies, as today), `review`->Review, `summarize` and `condense`->Summarize; any other tag, the empty one included: `ContractError` "CallKindOf: no call kind for the call tag '<tag>'". `CallKindName` unchanged; replay fixtures unchanged.
- `app::SeatLabelOf(const inference::GenerationSpec&)`: `resolve`->Resolve, `narrate`->Narrate, `renarrate`->Rewrite, `lookup`->Lookup, `review`->Review, `summarize`->Summarize, `condense`->Condense; any other: `ContractError` "SeatLabelOf: no seat label for the call tag '<tag>'".
- `core::ThinkingFormat {open, skip, end, afterEnd, close}`; `core::QWEN_THINKING` = `{"<think>\n", "<think>\n\n</think>\n\n", "</think>", "\n\n", "\n\nConsidering the limited time, I have to give the solution based on the thinking directly now.\n</think>\n\n"}`; `core::ThinkingMode { Auto, Format, None }`; `core::ThinkingSetting { mode = Auto; format = {} }`.
- settings.json `thinking_format`: optional; `"auto"`, `"qwen"`, `"none"` or an object with exactly `open`, `skip`, `end`, `after_end`, `close` (strings). Faults (DataError): unknown preset `<file>: 'thinking_format' must be "auto", "qwen", "none" or an object with open, skip, end, after_end and close, got '<name>'`; empty `open` or `end`: `<file>: thinking_format '<key>' must not be empty`; `<file>: thinking_format 'close' must end with 'end' followed by 'after_end'`; with `"none"` a budget above 0: `<file>: thinking_format is "none": budget '<key>' must be 0`.
- `SettingsToJson` writes `thinking_format`: `"auto"`, `"none"`, `"qwen"` for a Format equal to `QWEN_THINKING`, else the object.
- In-process backend: the resolved format's `end` must be one token, else `InferenceError` "the thinking format's end marker '<end>' is not a single token of '<chat model path>'". llama-server sends `chat_template_kwargs {"enable_thinking": true}` whenever a format is resolved (as today when the template had the switch).
- Morpheus's own game keeps its behaviour: `"[turn]"`, `"[replay]"`, `"[context]"` stay green; the replay of every recorded fixture passes.
- CLAUDE.md rules: fail fast; comments that read top to bottom; a private helper only with more than one caller; class layout order; constants ALL_CAPS; members `_camelCase`; members initialized at declaration; ASCII only; tests first; no legacy paths kept.
- Catch2 cannot stringify `std::string_view` in this build: wrap one in `std::string(...)` inside a CHECK.
- A change to a struct used across files (Sampling, SamplingSettings, GenerationSpec, Settings, the backend configs) can leave stale objects: after such a change, build once with `cmake --build --preset tests --clean-first`; if a test you did not touch crashes (SIGSEGV or a "Debug Assertion Failed" dialog), clean-rebuild before changing code.
- Commit trailer `Co-Authored-By: Claude <model> <noreply@anthropic.com>` naming the implementing model. Never stage `settings.json`, `models/`, `imgui.ini`, `.superpowers/` or `docs/references/`.

Commands (Git Bash, repo root): `cmake --preset tests` after adding a file; `cmake --build --preset tests`; one tag `./build/tests/bin/Debug/morpheus_tests.exe "[settings]"`; suite `ctest --preset tests`. The `dev` preset (llama.cpp) is configured in `build/dev`: `cmake --build build/dev --config RelWithDebInfo --target morpheus_tests` builds `src/llama` and the tools. Foreground builds only, one at a time, with the 600000 ms timeout; leave nothing running.

---

### Task 1: min_p

**Files:**
- Modify: `src/core/Settings.h` (`SamplingSettings`), `src/core/Settings.cpp` (read, write), `src/inference/Llm.h` (`Sampling`), `src/turn/Session.cpp` (`SamplingFrom`), `src/llama/LlamaCppBackend.cpp` (the chain), `src/llamaserver/LlamaServerBackend.cpp` (the body), `src/app/model/SettingsEdit.cpp` (read, write), `src/app/ui/SettingsWindow.cpp` (the field), `settings.example.json`, `README.md`
- Test: `tests/core/SettingsTests.cpp`, `tests/turn/SamplingTests.cpp`, `tests/llamaserver/LlamaServerCompleteTests.cpp`, `tests/app/SettingsEditTests.cpp`

**Interfaces:**
- Produces: `core::SamplingSettings::minP` (`double`, default 0.0, last member); `inference::Sampling::minP` (`float`, default 0.0f, last member).

- [ ] **Step 1: Write the failing tests**

`tests/core/SettingsTests.cpp`, append:

```cpp
TEST_CASE("LoadSettings reads an optional min_p per call type", "[settings]")
{
    // Absent: 0, so existing settings files load as they did.
    const Settings plain = LoadSettings(WriteSettings("settings_min_p_absent", ValidSettingsJson()));
    CHECK(plain.resolveSampling.minP == 0.0);
    CHECK(plain.narrateSampling.minP == 0.0);

    nlohmann::json json = ValidSettingsJson();
    json["sampling"]["narrate"]["min_p"] = 0.05;
    const Settings set = LoadSettings(WriteSettings("settings_min_p_set", json));
    CHECK(set.narrateSampling.minP == 0.05);
    CHECK(set.resolveSampling.minP == 0.0);
    // It round-trips.
    CHECK(LoadSettings(WriteSettings("settings_min_p_copy", SettingsToJson(set))).narrateSampling.minP == 0.05);

    for (const double bad : {-0.1, 1.0, 1.5}) {
        INFO(bad);
        nlohmann::json wrong = ValidSettingsJson();
        wrong["sampling"]["resolve"]["min_p"] = bad;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_min_p_bad", wrong)),
                          ContainsSubstring(std::format("sampling 'resolve': 'min_p' must be at least 0 and below 1, "
                                                        "not {}",
                                                        bad)));
    }
}
```

(add `#include <format>`).

`tests/turn/SamplingTests.cpp`, append (add includes `turn/Session.h`, `core/Settings.h` if missing):

```cpp
TEST_CASE("SamplingFrom copies min_p", "[sampling]")
{
    core::SamplingSettings settings{0.7, 30, 0.9, 0.05};
    const inference::Sampling sampling = turn::SamplingFrom(settings, 99);
    CHECK(sampling.minP == 0.05f);
    CHECK(sampling.seed == 99u);
}
```

`tests/llamaserver/LlamaServerCompleteTests.cpp`, "LlamaServerBackend::Complete sends one streamed /completion request": the spec's sampling becomes `{0.5f, 20, 0.75f, 77, 0.05f}`; the samplers check becomes `CHECK(body.at("samplers") == nlohmann::json::array({"top_k", "top_p", "min_p", "temperature"}));` and the min_p check `CHECK(body.at("min_p").get<double>() == Catch::Approx(0.05));` (include `catch2/catch_approx.hpp`); update the comment above it to name min-p in the chain.

`tests/app/SettingsEditTests.cpp`: in the test that reads an edit, add `CHECK(edit.narrateSampling.minP == 0.0);`; add a case that saves an edit with `edit.resolveSampling.minP = 0.1` and checks the file's `sampling.resolve.min_p == 0.1` and the loaded settings' `resolveSampling.minP == 0.1` (follow the file's existing save test for the setup).

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: compile errors (`minP` is not a member).

- [ ] **Step 3: Implement**

`src/core/Settings.h`, `SamplingSettings`:

```cpp
struct SamplingSettings {
    double temperature = 0.0; // 0 means greedy decoding.
    int topK = 40;
    double topP = 0.95;
    double minP = 0.0;        // 0 means no min-p. Last, so positional initializers of the fields above stay valid.
};
```

`src/core/Settings.cpp`, `readSampling`: `checkKeys(object, callType, {"temperature", "top_k", "top_p", "min_p"});` and, after the top_p check:

```cpp
        // min_p is optional (0: none), and below 1, which would keep only the top token.
        if (object.contains("min_p")) {
            const nlohmann::json& minP = object.at("min_p");
            if (!minP.is_number() || minP.get<double>() < 0.0 || minP.get<double>() >= 1.0) {
                throw DataError(std::format("{}: sampling '{}': 'min_p' must be at least 0 and below 1, not {}", where,
                                            callType, minP.dump()));
            }
            result.minP = minP.get<double>();
        }
```

`SettingsToJson`'s `sampling` lambda adds `{"min_p", values.minP}`.

`src/inference/Llm.h`, `Sampling`: add after `seed`: `float minP = 0.0f; // 0 means no min-p. Last, so positional initializers stay valid.`

`src/turn/Session.cpp`, `SamplingFrom`: `sampling.minP = static_cast<float>(settings.minP);`

`src/llama/LlamaCppBackend.cpp`, the non-greedy chain: after the top-p line, `llama_sampler_chain_add(pChain.get(), llama_sampler_init_min_p(spec.sampling.minP, 1));`, and the comment names min-p.

`src/llamaserver/LlamaServerBackend.cpp`, the body: `{"samplers", nlohmann::json::array({"top_k", "top_p", "min_p", "temperature"})}` and `{"min_p", spec.sampling.minP}`; the comment above says the chain is top-k, top-p, min-p, temperature (min-p moves out of the neutral list).

`src/app/model/SettingsEdit.cpp`: `ReadSettingsEdit`'s `sampling` lambda reads `block.value("min_p", 0.0)` into the fourth member; `SaveSettingsEdit`'s `samplingJson` writes `{"min_p", sampling.minP}` after `top_p`.

`src/app/ui/SettingsWindow.cpp`, `samplingFields`: after Top P, `ImGui::InputDouble("Min P", &sampling.minP, 0.01, 0.05, "%.2f");`.

`tests/hosted/HostedTestSupport.h`, `HostedSettings()`: both samplings gain min_p 0.05 (`{0.3, 40, 0.9, 0.05}` and `{0.8, 40, 0.95, 0.05}`), so the live hosted and `[server]` runs, which start from these settings, sample with min_p (the fakes ignore sampling), and its comment says so.

`settings.example.json`: `"min_p": 0.0` in both sampling entries. `README.md`, the settings section's sampling description: name `min_p` (optional, 0 to below 1, default 0).

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests --clean-first && ctest --preset tests`
Expected: all pass. Then build the dev tests target (`cmake --build build/dev --config RelWithDebInfo --target morpheus_tests`): it compiles `src/llama`.

- [ ] **Step 5: Commit**

```bash
git add src/core/Settings.h src/core/Settings.cpp src/inference/Llm.h src/turn/Session.cpp src/llama/LlamaCppBackend.cpp src/llamaserver/LlamaServerBackend.cpp src/app/model/SettingsEdit.cpp src/app/ui/SettingsWindow.cpp settings.example.json README.md tests/hosted/HostedTestSupport.h tests/core/SettingsTests.cpp tests/turn/SamplingTests.cpp tests/llamaserver/LlamaServerCompleteTests.cpp tests/app/SettingsEditTests.cpp
git commit -m "Sampling: min_p, from settings.json to both backends' samplers"
```

---

### Task 2: The call tag

**Files:**
- Modify: `src/inference/Llm.h` (`GenerationSpec`), `src/turn/Session.cpp` (lookup, resolve, narrate/renarrate, review specs), `src/turn/HostedSession.cpp` (decide, resolve, speak specs), `src/turn/MemoryUpkeep.cpp` (fold, condense specs), `src/turn/Replay.h`, `src/turn/Replay.cpp` (`CallKindOf`, `RecordingBackend::Complete`), `src/app/model/ModelSeat.h`, `src/app/model/ModelSeat.cpp` (`SeatLabelOf`), `src/app/model/ModelSeatBackend.cpp` (its call), `src/turn/ResolveGrammar.h`, `src/turn/ResolveGrammar.cpp`, `src/grammar/Gbnf.h` (comments only)
- Test: `tests/turn/ReplayTests.cpp`, `tests/app/ModelSeatTests.cpp`, `tests/turn/CallTagTests.cpp` (new), `tests/hosted/HostedDecideTests.cpp`

**Interfaces:**
- Produces: `inference::GenerationSpec::callTag` (`std::string`, default empty); `core::CallKind turn::CallKindOf(const inference::GenerationSpec& spec)`; `app::SeatLabel app::SeatLabelOf(const inference::GenerationSpec& spec)`.

- [ ] **Step 1: Write the failing tests**

`tests/turn/ReplayTests.cpp`: the replay responder calls `turn::CallKindOf(spec)` (drop the prompt argument). Replace the test "CallKindOf tells the calls apart and CallKindName spells them" with:

```cpp
TEST_CASE("CallKindOf reads the call tag and CallKindName spells the kinds", "[replay]")
{
    auto kindOf = [](const std::string& tag) {
        inference::GenerationSpec spec;
        spec.callTag = tag;
        return turn::CallKindOf(spec);
    };
    CHECK(kindOf("lookup") == core::CallKind::Lookup);
    CHECK(kindOf("resolve") == core::CallKind::Resolve);
    CHECK(kindOf("narrate") == core::CallKind::Narrate);
    CHECK(kindOf("renarrate") == core::CallKind::Narrate); // Its replies join the narrate replies.
    CHECK(kindOf("review") == core::CallKind::Review);
    CHECK(kindOf("summarize") == core::CallKind::Summarize);
    CHECK(kindOf("condense") == core::CallKind::Summarize);
    // The replay and the app see only the owned game's calls: anything else is an engine bug.
    CHECK_THROWS_WITH(kindOf(""), "CallKindOf: no call kind for the call tag ''");
    CHECK_THROWS_WITH(kindOf("speak"), "CallKindOf: no call kind for the call tag 'speak'");
    CHECK_THROWS_WITH(kindOf("decide"), "CallKindOf: no call kind for the call tag 'decide'");

    // [keep the existing CallKindName checks of the old test here, unchanged]
}
```

(The bracketed line is plan shorthand: move the old test's `CallKindName` CHECKs into this one verbatim.)

`tests/app/ModelSeatTests.cpp`: replace "SeatLabelOf labels each call the way the session and folding make it" with:

```cpp
TEST_CASE("SeatLabelOf labels each call by its tag", "[model_seat]")
{
    auto labelOf = [](const std::string& tag) {
        inference::GenerationSpec spec;
        spec.callTag = tag;
        return app::SeatLabelOf(spec);
    };
    CHECK(labelOf("resolve") == app::SeatLabel::Resolve);
    CHECK(labelOf("narrate") == app::SeatLabel::Narrate);
    CHECK(labelOf("renarrate") == app::SeatLabel::Rewrite);
    CHECK(labelOf("lookup") == app::SeatLabel::Lookup);
    CHECK(labelOf("review") == app::SeatLabel::Review);
    CHECK(labelOf("summarize") == app::SeatLabel::Summarize);
    CHECK(labelOf("condense") == app::SeatLabel::Condense);
    CHECK_THROWS_WITH(labelOf(""), "SeatLabelOf: no seat label for the call tag ''");
    CHECK_THROWS_WITH(labelOf("speak"), "SeatLabelOf: no seat label for the call tag 'speak'");
}
```

Remove the includes that test no longer needs. Any other `SeatLabelOf(prompt, spec)` call in `tests/app/` becomes `SeatLabelOf(spec)` with a tagged spec.

Create `tests/turn/CallTagTests.cpp` (owned game): run turns of the mini world with a `test::ScriptedBackend` whose responder answers by `spec.callTag` and records every tag, with `narration_check` on so a review runs, and a review answer that reports a contradiction once so a renarrate runs; in assisted memory mode so a lookup runs; and settings with a small transcript budget (or enough turns) so a fold runs. Check the recorded tags contain each of `lookup`, `resolve`, `narrate`, `review`, `renarrate`, `summarize`, and that every call's tag is one of the owned tags. Model the setup on the existing `tests/turn/SessionTests.cpp` cases that make a review, a renarrate and a lookup (grep `RENARRATE_CORRECTION` and `Assisted` there), and on `tests/turn/FoldingTests.cpp` for a fold and a condense; reuse their settings and world setup rather than inventing new ones. If one test cannot reach a call kind cheaply, split it into two cases; every one of the seven owned tags (`condense` included) must be checked somewhere in this file.

`tests/hosted/HostedDecideTests.cpp`, the first case ("Decide chooses among the NPC participants ..."): add `CHECK(calls[0].spec.callTag == "decide"); CHECK(calls[1].spec.callTag == "resolve"); CHECK(calls[2].spec.callTag == "speak");`. In `tests/hosted/HostedPromptPackTests.cpp`'s first case, add `CHECK(backend.Calls().back().spec.callTag == "summarize");` after the fold, and in its condense case `CHECK(call.spec.callTag == "condense");` inside the condense branch.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: compile errors (`callTag` is not a member; `CallKindOf` and `SeatLabelOf` take two arguments).

- [ ] **Step 3: Implement**

`src/inference/Llm.h`, `GenerationSpec`, last member:

```cpp
    // What the call is, for the layers that tell calls apart (the replay, the app's labels); the backends ignore it.
    // The turn layer sets it on every call (generation-controls spec, section 2).
    std::string callTag = {};
```

Set the tag where each spec is built:
- `src/turn/Session.cpp`: `lookupSpec.callTag = "lookup";`, `resolveSpec.callTag = "resolve";`, `reviewSpec.callTag = "review";`; in the `narrate` lambda, `spec.callTag = kind == core::CallKind::Renarrate ? "renarrate" : "narrate";`.
- `src/turn/HostedSession.cpp`: `decideSpec.callTag = "decide";`, `resolveSpec.callTag = "resolve";`, `speakSpec.callTag = "speak";`.
- `src/turn/MemoryUpkeep.cpp`: the fold's `spec.callTag = "summarize";`, the condense's `spec.callTag = "condense";`.

`src/turn/Replay.h`: `core::CallKind CallKindOf(const inference::GenerationSpec& spec);` with the comment: "Which call a completion answers, read from its tag: lookup, resolve, narrate, review, summarize; a renarrate counts as narration and a condense as a summary (their replies join those kinds', in call order). Throws ContractError for any other tag: the replay and the app see only the owned game's calls."

`src/turn/Replay.cpp`, `CallKindOf`:

```cpp
core::CallKind CallKindOf(const inference::GenerationSpec& spec)
{
    // The tag the turn layer set; a renarrate's and a condense's replies join the narrate and summarize replies.
    const std::string& tag = spec.callTag;
    if (tag == "lookup") {
        return core::CallKind::Lookup;
    }
    if (tag == "resolve") {
        return core::CallKind::Resolve;
    }
    if (tag == "narrate" || tag == "renarrate") {
        return core::CallKind::Narrate;
    }
    if (tag == "review") {
        return core::CallKind::Review;
    }
    if (tag == "summarize" || tag == "condense") {
        return core::CallKind::Summarize;
    }
    throw core::ContractError(std::format("CallKindOf: no call kind for the call tag '{}'", tag));
}
```

(drop the `EngineText.h` and `ResolveGrammar.h` includes if nothing else in the file needs them; add `<format>`). `RecordingBackend::Complete` calls `CallKindOf(spec)`.

`src/app/model/ModelSeat.h`: `SeatLabel SeatLabelOf(const inference::GenerationSpec& spec);` with the comment "The label of a call, from its tag (the turn layer's): resolve, narrate, renarrate (a rewrite), lookup, review, summarize, condense. Throws ContractError for any other tag." `src/app/model/ModelSeat.cpp`:

```cpp
SeatLabel SeatLabelOf(const inference::GenerationSpec& spec)
{
    // The tag the turn layer set: each owned call has its own label.
    const std::string& tag = spec.callTag;
    if (tag == "resolve") {
        return SeatLabel::Resolve;
    }
    if (tag == "narrate") {
        return SeatLabel::Narrate;
    }
    if (tag == "renarrate") {
        return SeatLabel::Rewrite;
    }
    if (tag == "lookup") {
        return SeatLabel::Lookup;
    }
    if (tag == "review") {
        return SeatLabel::Review;
    }
    if (tag == "summarize") {
        return SeatLabel::Summarize;
    }
    if (tag == "condense") {
        return SeatLabel::Condense;
    }
    throw core::ContractError(std::format("SeatLabelOf: no seat label for the call tag '{}'", tag));
}
```

(drop includes it no longer needs). `src/app/model/ModelSeatBackend.cpp`: `SeatLabelOf(spec)`.

Comments: `src/turn/ResolveGrammar.h` (the lookup grammar's "Its first line, the root, is how turn::CallKindOf tells a lookup call"), `src/turn/ResolveGrammar.cpp` (lines ~178 and ~211) and `src/grammar/Gbnf.h` (~72): drop the clauses that say the root line exists for `CallKindOf`; the grammars themselves do not change.

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests --clean-first && ./build/tests/bin/Debug/morpheus_tests.exe "[replay]"`, then `ctest --preset tests`
Expected: all pass, the replay of every recorded fixture included (it now reads every owned call's tag: a missing tag would be a ContractError there).

- [ ] **Step 5: Commit**

```bash
git add src/inference/Llm.h src/turn/Session.cpp src/turn/HostedSession.cpp src/turn/MemoryUpkeep.cpp src/turn/Replay.h src/turn/Replay.cpp src/app/model/ModelSeat.h src/app/model/ModelSeat.cpp src/app/model/ModelSeatBackend.cpp src/turn/ResolveGrammar.h src/turn/ResolveGrammar.cpp src/grammar/Gbnf.h tests/turn/ReplayTests.cpp tests/app/ModelSeatTests.cpp tests/turn/CallTagTests.cpp tests/hosted/HostedDecideTests.cpp tests/hosted/HostedPromptPackTests.cpp
git commit -m "Call tags: every turn-layer call names itself; the replay and the app's labels read the tag"
```

---

### Task 3: Thinking formats in the settings

**Files:**
- Create: `src/core/Thinking.h`, `src/core/Thinking.cpp`
- Modify: `src/core/CMakeLists.txt` (add `Thinking.cpp`), `src/core/Settings.h` (`Settings::thinking`), `src/core/Settings.cpp` (read, check, write)
- Test: `tests/core/ThinkingTests.cpp` (new), `tests/core/SettingsTests.cpp`

**Interfaces:**
- Produces (`src/core/Thinking.h`, namespace `morpheus::core`):
  ```cpp
  struct ThinkingFormat {
      std::string open = {};     // The prompt's end when the call may think.
      std::string skip = {};     // The prompt's end when it does not (may be empty).
      std::string end = {};      // The end marker the model writes.
      std::string afterEnd = {}; // What follows the end marker before the answer.
      std::string close = {};    // What closes a block that reached its budget: a nudge, then end and afterEnd.
      bool operator==(const ThinkingFormat&) const = default;
  };
  enum class ThinkingMode { Auto, Format, None };
  struct ThinkingSetting {
      ThinkingMode mode = ThinkingMode::Auto;
      ThinkingFormat format = {}; // Used by ThinkingMode::Format.
      bool operator==(const ThinkingSetting&) const = default;
  };
  inline const ThinkingFormat QWEN_THINKING = {...}; // Global Constraints' texts
  // The format a backend uses: Auto is QWEN_THINKING when `chatTemplate` has the enable_thinking switch and none
  // otherwise; Format is its format; None is none.
  std::optional<ThinkingFormat> ResolveThinking(const ThinkingSetting& setting, std::string_view chatTemplate);
  ```
  `core::Settings::thinking` (`ThinkingSetting`, default Auto).

- [ ] **Step 1: Write the failing tests**

Create `tests/core/ThinkingTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "core/Thinking.h"

using namespace morpheus::core;

TEST_CASE("The Qwen preset is the texts Qwen's chat template writes", "[thinking]")
{
    CHECK(QWEN_THINKING.open == "<think>\n");
    CHECK(QWEN_THINKING.skip == "<think>\n\n</think>\n\n");
    CHECK(QWEN_THINKING.end == "</think>");
    CHECK(QWEN_THINKING.afterEnd == "\n\n");
    CHECK(QWEN_THINKING.close == "\n\nConsidering the limited time, I have to give the solution based on the thinking "
                                 "directly now.\n</think>\n\n");
    CHECK(QWEN_THINKING.close.ends_with(QWEN_THINKING.end + QWEN_THINKING.afterEnd));
}

TEST_CASE("ResolveThinking: auto reads the template's switch, a format is used as given, none is none", "[thinking]")
{
    const std::string withSwitch = "{%- if enable_thinking is defined %}...{%- endif %}";
    const std::string without = "{% for message in messages %}{{ message.content }}{% endfor %}";
    CHECK(ResolveThinking({}, withSwitch) == std::optional<ThinkingFormat>(QWEN_THINKING));
    CHECK(ResolveThinking({}, without) == std::nullopt);
    const ThinkingFormat custom = {"[T]", "", "[/T]", "\n", "Answer now.[/T]\n"};
    CHECK(ResolveThinking({ThinkingMode::Format, custom}, without) == std::optional<ThinkingFormat>(custom));
    CHECK(ResolveThinking({ThinkingMode::None, {}}, withSwitch) == std::nullopt);
}
```

`tests/core/SettingsTests.cpp`, append:

```cpp
TEST_CASE("LoadSettings reads thinking_format: a preset or the five texts", "[settings][thinking]")
{
    CHECK(LoadSettings(WriteSettings("settings_thinking_absent", ValidSettingsJson())).thinking == ThinkingSetting{});
    auto load = [](const std::string& name, const nlohmann::json& format) {
        nlohmann::json json = ValidSettingsJson();
        json["thinking_format"] = format;
        return LoadSettings(WriteSettings(name, json));
    };
    CHECK(load("settings_thinking_auto", "auto").thinking == ThinkingSetting{});
    CHECK(load("settings_thinking_qwen", "qwen").thinking == ThinkingSetting{ThinkingMode::Format, QWEN_THINKING});
    CHECK(load("settings_thinking_none", "none").thinking.mode == ThinkingMode::None);
    const nlohmann::json custom = {{"open", "[T]"}, {"skip", ""}, {"end", "[/T]"}, {"after_end", "\n"},
                                   {"close", "Answer now.[/T]\n"}};
    const Settings customSettings = load("settings_thinking_custom", custom);
    CHECK(customSettings.thinking ==
          ThinkingSetting{ThinkingMode::Format, {"[T]", "", "[/T]", "\n", "Answer now.[/T]\n"}});

    // Each form round-trips through SettingsToJson: "qwen" for the preset, the object for a custom format.
    CHECK(SettingsToJson(load("settings_thinking_qwen_json", "qwen")).at("thinking_format") == "qwen");
    CHECK(SettingsToJson(customSettings).at("thinking_format") == custom);
    CHECK(SettingsToJson(load("settings_thinking_none_json", "none")).at("thinking_format") == "none");
    CHECK(LoadSettings(WriteSettings("settings_thinking_copy", SettingsToJson(customSettings))).thinking ==
          customSettings.thinking);

    // The faults.
    CHECK_THROWS_WITH(load("settings_thinking_bad_name", "deepseek"),
                      ContainsSubstring("'thinking_format' must be \"auto\", \"qwen\", \"none\" or an object with open, "
                                        "skip, end, after_end and close, got 'deepseek'"));
    nlohmann::json noOpen = custom;
    noOpen["open"] = "";
    CHECK_THROWS_WITH(load("settings_thinking_no_open", noOpen), ContainsSubstring("thinking_format 'open' must not be empty"));
    nlohmann::json noEnd = custom;
    noEnd["end"] = "";
    CHECK_THROWS_WITH(load("settings_thinking_no_end", noEnd), ContainsSubstring("thinking_format 'end' must not be empty"));
    nlohmann::json badClose = custom;
    badClose["close"] = "Answer now.";
    CHECK_THROWS_WITH(load("settings_thinking_bad_close", badClose),
                      ContainsSubstring("thinking_format 'close' must end with 'end' followed by 'after_end'"));
    nlohmann::json extra = custom;
    extra["nudge"] = "x";
    CHECK_THROWS_WITH(load("settings_thinking_extra", extra), ContainsSubstring("unknown key 'nudge' in thinking_format"));
    nlohmann::json missing = custom;
    missing.erase("skip");
    CHECK_THROWS_WITH(load("settings_thinking_missing", missing), ContainsSubstring("thinking_format needs 'skip'"));

    // With no thinking, every thinking budget must be 0.
    nlohmann::json none = ValidSettingsJson();
    none["thinking_format"] = "none";
    none["budget"]["narrate_thinking_tokens"] = 128;
    CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_thinking_none_budget", none)),
                      ContainsSubstring("thinking_format is \"none\": budget 'narrate_thinking_tokens' must be 0"));
}
```

(add `#include "core/Thinking.h"`).

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --preset tests && cmake --build --preset tests`
Expected: `core/Thinking.h` not found.

- [ ] **Step 3: Implement**

`src/core/Thinking.h`: the Interfaces block above, with includes `<optional>`, `<string>`, `<string_view>`, a header comment explaining the block (as `src/inference/Llm.h`'s THINKING comment does today), and:

```cpp
inline const ThinkingFormat QWEN_THINKING = {
    "<think>\n",
    "<think>\n\n</think>\n\n",
    "</think>",
    "\n\n",
    "\n\nConsidering the limited time, I have to give the solution based on the thinking directly now.\n</think>\n\n",
};
```

`src/core/Thinking.cpp`:

```cpp
#include "core/Thinking.h"

namespace morpheus::core {

std::optional<ThinkingFormat> ResolveThinking(const ThinkingSetting& setting, std::string_view chatTemplate)
{
    switch (setting.mode) {
    case ThinkingMode::Auto:
        // The chat template's own enable_thinking switch marks a model that reasons in Qwen's block.
        return chatTemplate.find("enable_thinking") != std::string_view::npos ? std::optional(QWEN_THINKING)
                                                                              : std::nullopt;
    case ThinkingMode::Format:
        return setting.format;
    case ThinkingMode::None:
        return std::nullopt;
    }
    return std::nullopt; // Unreachable: every mode is handled above.
}

} // namespace morpheus::core
```

(If MSVC warns about the unreachable return under /WX, replace it with `throw ContractError("ResolveThinking: unhandled mode");` and include `core/Errors.h`.) Add `Thinking.cpp` to `src/core/CMakeLists.txt`.

`src/core/Settings.h`: include `core/Thinking.h`; `Settings` gains, after `narrateSampling`: `ThinkingSetting thinking = {}; // thinking_format: how the backends write the thinking block (Auto: as the template says).` The `LoadSettings` comment names `thinking_format` among the optional keys.

`src/core/Settings.cpp`: the top-level `checkKeys` allows `"thinking_format"`. After the budget block (before sampling):

```cpp
    // The thinking format (generation-controls spec, section 3): a preset name, or the five texts. Absent is "auto".
    if (json.contains("thinking_format")) {
        const nlohmann::json& format = json.at("thinking_format");
        if (format.is_string()) {
            const std::string name = format.get<std::string>();
            if (name == "qwen") {
                settings.thinking = {ThinkingMode::Format, QWEN_THINKING};
            } else if (name == "none") {
                settings.thinking = {ThinkingMode::None, {}};
            } else if (name != "auto") {
                throw DataError(std::format("{}: 'thinking_format' must be \"auto\", \"qwen\", \"none\" or an object "
                                            "with open, skip, end, after_end and close, got '{}'",
                                            where, name));
            }
        } else {
            checkKeys(format, "thinking_format", {"open", "skip", "end", "after_end", "close"});
            ThinkingFormat custom;
            custom.open = requireString(format, "open", "thinking_format");
            custom.skip = requireString(format, "skip", "thinking_format");
            custom.end = requireString(format, "end", "thinking_format");
            custom.afterEnd = requireString(format, "after_end", "thinking_format");
            custom.close = requireString(format, "close", "thinking_format");
            for (const auto& [key, text] : {std::pair{"open", &custom.open}, std::pair{"end", &custom.end}}) {
                if (text->empty()) {
                    throw DataError(std::format("{}: thinking_format '{}' must not be empty", where, key));
                }
            }
            // A closed block ends as the model's own does: the end marker, then what follows it.
            if (!custom.close.ends_with(custom.end + custom.afterEnd)) {
                throw DataError(std::format("{}: thinking_format 'close' must end with 'end' followed by 'after_end'",
                                            where));
            }
            settings.thinking = {ThinkingMode::Format, std::move(custom)};
        }
    }
    // With no thinking, no call may think.
    if (settings.thinking.mode == ThinkingMode::None) {
        const std::pair<const char*, int> budgets[] = {{"resolve_thinking_tokens", settings.budget.resolveThinkingTokens},
                                                       {"narrate_thinking_tokens", settings.budget.narrateThinkingTokens},
                                                       {"review_thinking_tokens", settings.budget.reviewThinkingTokens}};
        for (const auto& [key, value] : budgets) {
            if (value > 0) {
                throw DataError(std::format("{}: thinking_format is \"none\": budget '{}' must be 0", where, key));
            }
        }
    }
```

(`requireString`'s missing-key message is "thinking_format needs '<key>'", matching the test.) `SettingsToJson`:

```cpp
    // The thinking format as settings.json spells it: a preset name, or the five texts.
    if (settings.thinking.mode == ThinkingMode::Auto) {
        json["thinking_format"] = "auto";
    } else if (settings.thinking.mode == ThinkingMode::None) {
        json["thinking_format"] = "none";
    } else if (settings.thinking.format == QWEN_THINKING) {
        json["thinking_format"] = "qwen";
    } else {
        const ThinkingFormat& format = settings.thinking.format;
        json["thinking_format"] = {{"open", format.open}, {"skip", format.skip}, {"end", format.end},
                                   {"after_end", format.afterEnd}, {"close", format.close}};
    }
```

`settings.example.json`: `"thinking_format": "auto"` after `"prose_turns"`.

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests --clean-first && ./build/tests/bin/Debug/morpheus_tests.exe "[settings],[thinking]"`, then `ctest --preset tests`
Expected: all pass (the "settings.example.json loads" test included).

- [ ] **Step 5: Commit**

```bash
git add src/core/Thinking.h src/core/Thinking.cpp src/core/CMakeLists.txt src/core/Settings.h src/core/Settings.cpp settings.example.json tests/core/ThinkingTests.cpp tests/core/SettingsTests.cpp
git commit -m "Thinking formats as settings data: thinking_format (auto, qwen, none or the five texts)"
```

---

### Task 4: The backends use the thinking format

**Files:**
- Modify: `src/inference/Llm.h` (remove the `THINKING_*` constants and their comment), `src/llama/LlamaCppBackend.h`, `src/llama/LlamaCppBackend.cpp`, `src/llamaserver/LlamaServerBackend.h`, `src/llamaserver/LlamaServerBackend.cpp`, `src/app/main.cpp`, `tools/server/main.cpp`, `tools/probe/main.cpp`, `tools/play/main.cpp`, `src/app/model/EngineHost.cpp`, `README.md`, `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`
- Test: `tests/llamaserver/LlamaServerBackendTests.cpp`, `tests/llamaserver/LlamaServerCompleteTests.cpp`, `tests/app/EngineHostTests.cpp`, `tests/llama/*` that name the constants (grep)

**Interfaces:**
- Consumes: `core::ThinkingSetting`, `core::ThinkingFormat`, `core::QWEN_THINKING`, `core::ResolveThinking` (Task 3); `core::Settings::thinking`.
- Produces: `llama::LlamaConfig::thinking` and `llamaserver::LlamaServerConfig::thinking` (`core::ThinkingSetting`, default Auto).

- [ ] **Step 1: Write the failing tests**

Every test that names `inference::THINKING_OPEN_TEXT`, `THINKING_SKIP_TEXT`, `THINKING_END_TEXT`, `THINKING_AFTER_END_TEXT` or `CLOSE_THINKING_TEXT` uses `core::QWEN_THINKING.open`, `.skip`, `.end`, `.afterEnd`, `.close` instead (grep `tests/`; include `core/Thinking.h`).

`tests/llamaserver/LlamaServerBackendTests.cpp`, append:

```cpp
TEST_CASE("LlamaServerBackend writes the thinking block its setting names", "[llamaserver][thinking]")
{
    test::FakeLlamaServer embed;
    // A custom format on a template without the switch: the prompt ends with its skip text, and enable_thinking is sent.
    {
        test::FakeLlamaServer chat;
        llamaserver::LlamaServerConfig config = test::ConfigFor(chat, embed);
        config.thinking = {core::ThinkingMode::Format, {"[T]", "[T][/T]\n", "[/T]", "\n", "Answer now.[/T]\n"}};
        llamaserver::LlamaServerBackend backend(config);
        const std::size_t skipTokens = test::FakeLlamaServer::DefaultTokens("[T][/T]\n", false, true).size();
        CHECK(backend.CountPromptTokens(test::SERVER_PROMPT) ==
              static_cast<int>(test::ServerPromptTokens().size() + skipTokens));
        CHECK(chat.RequestsTo("/apply-template").back().body.at("chat_template_kwargs") ==
              nlohmann::json{{"enable_thinking", true}});
    }
    // "none" on a template with the switch: no block, and no enable_thinking.
    {
        test::FakeLlamaServer chat;
        chat.SetProps(test::ThinkingProps());
        llamaserver::LlamaServerConfig config = test::ConfigFor(chat, embed);
        config.thinking = {core::ThinkingMode::None, {}};
        llamaserver::LlamaServerBackend backend(config);
        CHECK(backend.CountPromptTokens(test::SERVER_PROMPT) == static_cast<int>(test::ServerPromptTokens().size()));
        CHECK_FALSE(chat.RequestsTo("/apply-template").back().body.contains("chat_template_kwargs"));
    }
}
```

`tests/llamaserver/LlamaServerCompleteTests.cpp`, append a case modelled on "LlamaServerBackend::Complete thinks in a first call, then answers in a second that extends it", with the custom format `{"[T]", "", "[/T]", "\n", "Answer now.[/T]\n"}` set in the config on a template without the switch: the first request's prompt ends with the tokens of `"[T]"`, its `stop` is `["[/T]"]`, and its `n_predict` is the budget less the tokens of `"Answer now.[/T]\n"`; copy that case's body and swap the format's texts in.

The live thinking tests (grep `thinks before it answers` in `tests/llama/` and `tests/llamaserver/LlamaServerLiveTests.cpp`): after the existing check with the default (Auto) setting, run the same call once more on a backend whose config has `thinking = {core::ThinkingMode::Format, core::QWEN_THINKING}`, with the same expectations (they skip without the models or servers; they must compile in `tests` and `dev`).

`tests/app/EngineHostTests.cpp`, in "EngineHost rebuilds the backend when the backend kind, a server URL or the slot changes" (or a new case beside it): a settings copy that differs only in `thinking` (`{core::ThinkingMode::None, {}}` with the budgets at 0) makes the host load a new backend (count the factory calls as that test does).

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: compile errors (`thinking` is not a config member; the `inference::THINKING_*` names still exist but the tests no longer use them, so the failure is the config).

- [ ] **Step 3: Implement**

`src/inference/Llm.h`: delete the five `THINKING_*`/`CLOSE_THINKING_TEXT` constants and their comment (the comment's explanation moves to `core/Thinking.h`, Task 3).

`src/llama/LlamaCppBackend.h`: `LlamaConfig` gains `core::ThinkingSetting thinking = {}; // settings.json's thinking_format; Auto reads the chat template.`; the class comment says the block's texts come from the resolved format (Auto: Qwen's when the template has the enable_thinking switch); `ThinkingMarkers`' member comments name the format's fields instead of the constants.

`src/llama/LlamaCppBackend.cpp`, the constructor's thinking block becomes:

```cpp
    // The thinking format: settings.json's, Auto reading the chat template's enable_thinking switch. Its end marker
    // must be a single token, or the block's end could not be recognised while generating; the other texts may be
    // any tokens.
    const char* pTemplate = llama_model_chat_template(_pChatModel.get(), nullptr);
    if (const std::optional<core::ThinkingFormat> format =
            core::ResolveThinking(_config.thinking, pTemplate != nullptr ? std::string_view(pTemplate) : std::string_view())) {
        const llama_vocab* pVocab = llama_model_get_vocab(_pChatModel.get());
        const std::vector<std::int32_t> end = Tokenize(pVocab, format->end, false, true);
        if (end.size() != 1) {
            throw InferenceError(std::format("the thinking format's end marker '{}' is not a single token of '{}'",
                                             format->end, core::PathText(_config.chatModel)));
        }
        ThinkingMarkers markers;
        markers.open = Tokenize(pVocab, format->open, false, true);
        markers.skip = Tokenize(pVocab, format->skip, false, true);
        markers.close = Tokenize(pVocab, format->close, false, true);
        markers.afterEnd = Tokenize(pVocab, format->afterEnd, false, false);
        markers.endToken = end.front();
        _thinking = std::move(markers);
    }
```

(`Tokenize` of an empty skip must give an empty vector; check it does, or skip the call when `format->skip` is empty.)

`src/llamaserver/LlamaServerBackend.h`: `LlamaServerConfig` gains `core::ThinkingSetting thinking = {};` (same comment); the class and `ThinkingMarkers` comments name the format's fields.

`src/llamaserver/LlamaServerBackend.cpp`: the `if (chatTemplate.find("enable_thinking") ...)` block becomes:

```cpp
    // The thinking format: settings.json's, Auto reading the chat template's enable_thinking switch. Its texts are
    // tokenized once as the in-process backend tokenizes them: special tokens parsed, the text after the end marker
    // as plain text.
    if (const std::optional<core::ThinkingFormat> format = core::ResolveThinking(_config.thinking, chatTemplate)) {
        ThinkingMarkers markers;
        markers.open = ServerTokens(format->open, false, true);
        markers.skip = format->skip.empty() ? std::vector<std::int32_t>() : ServerTokens(format->skip, false, true);
        markers.close = ServerTokens(format->close, false, true);
        markers.end = ServerTokens(format->end, false, true);
        markers.afterEnd = ServerTokens(format->afterEnd, false, false);
        markers.endText = format->end;
        _thinking = std::move(markers);
    }
```

`ThinkingMarkers` gains `std::string endText = {}; // The end marker's text: the thinking request's stop string.`, and the thinking request's `thinking["stop"]` uses `_thinking->endText` instead of `inference::THINKING_END_TEXT`. The `/apply-template` request keeps sending `chat_template_kwargs {"enable_thinking": true}` when `_thinking` is set. Remove every other use of the `inference::THINKING_*` names in both backends.

`src/app/main.cpp` (`MakeLlamaBackend`, `MakeServerBackend`), `tools/server/main.cpp`, `tools/probe/main.cpp`, `tools/play/main.cpp`: each config built from settings gets `config.thinking = settings.thinking;`.

`src/app/model/EngineHost.cpp`, `sameBackend`: add `&& settings.thinking == _settings.thinking` (the backend resolves the format when it loads); its comment names the thinking format.

`README.md`: the settings section describes `thinking_format` (its four forms, what each text is, the checks, the in-process single-token rule for `end`); the `morpheus_server` section recommends `resolve_thinking_tokens` and `narrate_thinking_tokens` 0 for hosted dialogue (a thinking block delays the first spoken sentence). The architecture spec's backend section: one sentence that the thinking block's texts are settings data (`thinking_format`, `core/Thinking.h`).

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests --clean-first && ./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver]"`, then `ctest --preset tests`; then `cmake --build build/dev --config RelWithDebInfo` (every dev target: the app, the tools and the llama tests compile).
Expected: all pass; the dev build is clean.

- [ ] **Step 5: Commit**

```bash
git add src/inference/Llm.h src/llama/LlamaCppBackend.h src/llama/LlamaCppBackend.cpp src/llamaserver/LlamaServerBackend.h src/llamaserver/LlamaServerBackend.cpp src/app/main.cpp tools/server/main.cpp tools/probe/main.cpp tools/play/main.cpp src/app/model/EngineHost.cpp README.md docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md tests/llamaserver/LlamaServerBackendTests.cpp tests/llamaserver/LlamaServerCompleteTests.cpp tests/app/EngineHostTests.cpp
git commit -m "Backends: the thinking block's texts from the settings' thinking format"
```

(Add any other test file you changed for the constants.)

---

## Finally (controller)

- `ctest --preset tests`; `cmake --build --preset asan` and `ctest --preset asan`.
- `cmake --build build/dev --config RelWithDebInfo` builds clean; the consumer check is ok.
- Live: `[llama]` in-process (incl. `[llama][hosted]` and the thinking test) and `[server]` over llama-server, with `min_p` 0.05 in the live test settings for one run; the thinking tests with `thinking_format` "qwen" as well as auto.
