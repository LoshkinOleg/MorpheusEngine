# Milestone 2: Inference and Context - Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run a local model in-process and build the prompts the engine will send it.
- `LlamaCppBackend`: llama.cpp in-process with Vulkan, prefix KV reuse, grammar-constrained sampling, streaming, cancel, embeddings, save/restore of the KV cache.
- The `context` module: the layered, cache-friendly prompt with exact token budgets.
- Turn-record types moved into `core`.
- A `morpheus_probe` tool that exercises all of it on a real model.

**Architecture:**
- **Modules.** Three new static libraries:
  - `morpheus_inference`: the `LlmBackend` interface and prompt types. No llama.cpp.
  - `morpheus_context`: prompt assembly over `core` and `inference`. No llama.cpp.
  - `morpheus_llama`: `LlamaCppBackend`. It is the only library that links llama.cpp.
- **Presets.** The fast `tests` and `asan` presets never build llama.cpp. Model-backed code is built and tested under `dev`.
- **Spec:** `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`.

**Tech Stack:** C++20 with MSVC (VS 2022). CMake 3.25+ with FetchContent. llama.cpp is already pinned at `960d628f46d17ea9c10aa944b061b2341afeb3b5` and is now built with `GGML_VULKAN`. nlohmann/json, SQLite and Catch2 are as in milestone 1.

**Decisions for this milestone (made with the product owner):**
- **Turn-record types move into `core`, typed.** `core/TurnRecord.h` owns `TurnRecord`, `Summary`, `Note` and the typed `CheckOutcome`, with JSON round-trip helpers. `store` only serializes them, and `context` renders from them. The turns table column `rolls` becomes `checks`. No runs exist yet, so `SCHEMA_VERSION` stays 1.
- **GPU backend: Vulkan.** The Vulkan SDK 1.4.350 is installed, the CUDA toolkit is not. The `dev` preset builds llama.cpp with `GGML_VULKAN=ON`.
- **Models:**
  - chat: Qwen3-4B-Instruct-2507 Q4_K_M
  - embeddings: nomic-embed-text-v1.5 Q8_0, with prefixes `search_query: ` and `search_document: `

  The owner downloads both into `models/` (gitignored). Model-backed tests skip unless `MORPHEUS_TEST_CHAT_MODEL` and `MORPHEUS_TEST_EMBED_MODEL` are set.
- **Prompt shape:** one system message (L0 + L1 + L2), then the transcript (L3) as user/assistant pairs, then one user message holding the volatile tail (T1-T4). `Prompt::stableCount` marks the end of L3.
  - Prefix reuse is token-level (the longest common prefix with the KV cache), so no per-layer template rendering is needed.
  - The narrate prompt is the resolve prompt plus two appended messages.
- **Grammar source:** the backend takes a ready GBNF string. Generating grammars from the state schema is milestone 3, along with the decision about llama.cpp's `common` json-schema converter.
- **A lone carriage return in a world file is a `DataError`.** CRLF is still normalized. This was the open choice from the hardening review.

**Roadmap:**
- **M3 (turn):** resolve/apply/narrate, retrieval, folding, the worker thread, and replay tests. It uses `Assembly` flags to decide when to fold.
- **M4 (app):** the ImGui UI.

## Global Constraints

- **Toolchain:** C++20 (`cxx_std_20`), MSVC through the `Visual Studio 17 2022` generator, x64, Windows 10. CMake >= 3.25.
- **Dependencies:** fetched with FetchContent at pinned SHAs. This milestone adds no new dependencies; llama.cpp stays at `960d628f46d17ea9c10aa944b061b2341afeb3b5`.
- **Flags:** every MorpheusEngine target calls `morpheus_target_defaults()` (`/W4 /WX /permissive- /Zc:preprocessor /utf-8`). Never apply these to fetched dependencies. llama.cpp is fetched `SYSTEM`, so its headers do not trigger `/WX`.
- **Namespaces and targets:**
  - `morpheus::core`, `morpheus::store`, `morpheus::inference`, `morpheus::context`, `morpheus::llama`.
  - Targets are `morpheus_<module>`, and includes are written as `"module/File.h"`.
- **Error policy:**
  - Engine bugs throw `core::ContractError` (via `MORPHEUS_REQUIRE`).
  - Bad world, settings or saved data throws `core::DataError` or `store::StoreError`, naming the file and field.
  - Model, runtime and llama.cpp failures throw `inference::InferenceError`.
  - Mistakes in model-proposed changes are values, never exceptions.
- **Module boundaries:**
  - Only `morpheus_llama` and `tools/probe` include `<llama.h>`.
  - `morpheus_inference` and `morpheus_context` must build and test under the `tests` preset, which has no llama.cpp.
- **Threading:** `LlamaCppBackend` is not thread-safe. One thread owns it.
- **Coding rules:** follow `CLAUDE.md`. Source comments and docs are ASCII only.
- **Shell:** commands run from the repository root in Git Bash. Never stage the untracked `dotnet/` folder.
- **Commit trailer:** end each commit message with `-m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"`, where `<Model>` is the implementing model's name.

## Model setup (owner action, needed only for model-backed tests and the probe)

Download into `models/` at the repo root. `models/` is gitignored by Task 3. File names are as published; check them on the model pages.
- `Qwen3-4B-Instruct-2507-Q4_K_M.gguf` from Hugging Face `unsloth/Qwen3-4B-Instruct-2507-GGUF` (about 2.5 GB).
- `nomic-embed-text-v1.5.Q8_0.gguf` from Hugging Face `nomic-ai/nomic-embed-text-v1.5-GGUF` (about 140 MB).

Then, for model-backed tests:

```bash
export MORPHEUS_TEST_CHAT_MODEL="$PWD/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
export MORPHEUS_TEST_EMBED_MODEL="$PWD/models/nomic-embed-text-v1.5.Q8_0.gguf"
```

## File Structure

```
CMakeLists.txt                      modify: MORPHEUS_BUILD_LLAMA / MORPHEUS_VULKAN, llama always-available, new subdirs
CMakePresets.json                   modify: tests/asan without llama, dev with Vulkan llama
.gitignore                          modify: models/, settings.json
settings.example.json               create: documented settings template
src/core/TurnRecord.h/.cpp          create: TurnRecord, CheckOutcome, Summary, Note + JSON round-trip
src/core/Settings.h/.cpp            create: settings.json loader
src/store/RunStore.h/.cpp           modify: use core turn types, typed columns, rolls -> checks
src/inference/Llm.h/.cpp            create: Role, Message, Prompt, GenerationSpec, Completion, LlmBackend, InferenceError
src/llama/LlamaCppBackend.h/.cpp    create: the llama.cpp backend
src/context/StateView.h/.cpp        create: compact state rendering (T1)
src/context/Transcript.h/.cpp       create: mechanics line + transcript messages (L3)
src/context/PromptBuilder.h/.cpp    create: AssembleResolvePrompt, ExtendForNarrate, budgets
tools/probe/main.cpp                create: morpheus_probe
tests/fakes/FakeBackend.h           create: deterministic LlmBackend for context tests
tests/llama/LlamaTestModels.h       create: env-gated shared backend for model-backed tests
tests/...                           new test files per task
```

---

### Task 1: Close the M1 hardening leftovers

**Files:**
- Modify: `src/core/World.cpp`, `src/core/StateSchema.cpp`, `src/core/JsonInt.h`, `src/core/Changes.cpp`, `src/store/RunStore.h`, `src/store/RunStore.cpp`
- Modify: `tests/core/WorldTests.cpp`, `tests/core/StateSchemaTests.cpp`, `tests/store/RunStoreTests.cpp`
- Move: the `MORPHEUS_REQUIRE` location test from `tests/TestPathsTests.cpp` to a new `tests/core/ErrorsTests.cpp`

**Interfaces:**
- Consumes: the existing M1 code.
- Produces: no new interfaces. Behavior changes:
  - A lone `\r` in a world file throws `DataError`.
  - A field with an unknown `type` reports the unknown type before any key complaint.

- [ ] **Step 1: Write the failing tests**

Add to `tests/core/WorldTests.cpp`, inside the "LoadWorld reports the file at fault" test case:

```cpp
    SECTION("lone carriage return")
    {
        const fs::path dir = CopyMiniWorld("world_lone_cr");
        morpheus::test::WriteFile(dir / "rules.md", "line one\rline two\n");
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("carriage return"));
    }
```

Add to `tests/core/StateSchemaTests.cpp`:

```cpp
TEST_CASE("An unknown field type is reported before its keys", "[schema]")
{
    CHECK_THROWS_WITH(StateSchema::FromJson(SchemaWithBadField({{"type", "itn"}, {"min", 0}, {"max", 5}})),
                      ContainsSubstring("unknown type 'itn'"));
}
```

Add to `tests/store/RunStoreTests.cpp`. This uses the same two-connection SHARED-lock technique as the existing "failed COMMIT" test in that file:

```cpp
TEST_CASE("A turn whose COMMIT fails leaves no notes behind", "[store]")
{
    const std::filesystem::path file = FreshDbPath("store_notes_rollback");
    RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());

    // A second connection holding a read transaction makes CommitTurn's COMMIT fail with SQLITE_BUSY.
    Database reader = Database::Open(file, false);
    reader.Exec("BEGIN; SELECT COUNT(*) FROM notes;");
    CHECK_THROWS_AS(store.CommitTurn(TestTurn(1, {"must not survive"}), StateWithHealth(9)), StoreError);
    reader.Exec("COMMIT");

    CHECK(store.LatestTurn() == 0);
    CHECK(store.Notes().empty());
}
```

If `Exec` with two statements does not keep the SHARED lock, copy whatever lock-holding approach the existing failed-COMMIT test uses.

Create `tests/core/ErrorsTests.cpp`:
- Move the `MORPHEUS_REQUIRE reports the source location` test case into it, unchanged except for the tag `[errors]`.
- Delete it from `tests/TestPathsTests.cpp`.
- If that leaves `tests/TestPathsTests.cpp` with no test cases, delete the file.

- [ ] **Step 2: Build and run. Expect failure.**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[world],[schema],[store]"`
Expected:
- The lone-CR section fails: no exception is thrown.
- The `itn` case fails: the message names a key, not the type.
- The rollback test passes or fails depending on the existing code. Either way, keep it as the regression guard.

- [ ] **Step 3: Implement**

1. **`src/core/World.cpp`, in the file read loop, right after CRLF pairs are converted to LF.** Reject any remaining `\r`:

```cpp
        // CRLF pairs are normalized above; a carriage return on its own means a malformed file.
        if (const std::size_t stray = text.find('\r'); stray != std::string::npos) {
            throw DataError(std::format("'{}': stray carriage return at byte {}", (dir / name).generic_string(), stray));
        }
```

   Use the loop's actual variable names for `text` and `name`. In `tests/core/WorldTests.cpp`, change `ReadLfText` so that it converts only CRLF pairs to LF, the same rule as `LoadWorld`; it must stop dropping every `\r`. Move `ReadLfText` and `ConvertWorldToCrlf` into the file's anonymous namespace.

2. **`src/core/World.cpp`.** Before the first use of `pCheckType`, add:

```cpp
    MORPHEUS_REQUIRE(pCheckType != nullptr, "LoadWorld: check entity type missing after ValidateState");
```

3. **`src/core/StateSchema.cpp` `FromJson`.** Validate `typeText` against the five type names before the unknown-key check of the field object. The error for an unknown type keeps its existing wording, which contains `unknown type '<name>'`. Build the allowed key set only after the type is known.

4. **`src/core/JsonInt.h`.** Extend the doc comment: "Throws ContractError if lo > hi (callers pass constants or validated bounds)."

5. **`src/core/Changes.cpp`.** Replace each remaining `std::numeric_limits<int>::max()` / `min()` in the counter branch with `INT_MAX_VALUE` / `INT_MIN_VALUE`.

6. **`tests/core/WorldTests.cpp`, the stat-not-int section.** Assert `ContainsSubstring("must be an int field")` instead of `"location"`. If the actual message differs, assert its exact distinguishing phrase, and keep the message naming the stat.

7. **`src/store/RunStore.h`.** For `Turn`/`Turns`, add "and StoreError if a note has no matching turn row". In `RunStore.cpp`, change the `CommitTurn` comment to "The turn, its resulting state and its notes land together or not at all."

- [ ] **Step 4: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/core src/store tests
git commit -m "Close M1 hardening leftovers" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Turn-record types move into core

**Files:**
- Create: `src/core/TurnRecord.h`, `src/core/TurnRecord.cpp`, `tests/core/TurnRecordTests.cpp`
- Modify: `src/core/CMakeLists.txt` (add `TurnRecord.cpp`), `src/store/RunStore.h`, `src/store/RunStore.cpp`, `tests/store/RunStoreTests.cpp`

**Interfaces:**
- Consumes: `CheckResult` (`core/Dice.h`); `Change`, `AppliedChange`, `RejectedChange`, `ChangeToJson`, `ParseChange` (`core/Changes.h`); `JsonIntInRange` (`core/JsonInt.h`).
- Produces (namespace `morpheus::core`):
  - `struct CheckOutcome { std::string stat; std::string difficulty; std::string stakes; CheckResult result; }`
  - `struct TurnRecord { int number; std::string input; nlohmann::json resolve; std::vector<CheckOutcome> checks; std::vector<AppliedChange> applied; std::vector<RejectedChange> rejected; std::string narration; nlohmann::json timings; std::vector<std::string> notes; }`
  - `struct Summary { int fromTurn; int toTurn; std::string text; }`
  - `struct Note { int turn; std::string text; }`
  - `CheckOutcomeToJson` / `CheckOutcomeFromJson`, `AppliedChangeToJson` / `AppliedChangeFromJson`, `RejectedChangeToJson` / `RejectedChangeFromJson`. Each FromJson throws `DataError` on a bad shape.
- `store` re-exports the types (`using core::TurnRecord;` and so on), so `store::TurnRecord` keeps working.

- [ ] **Step 1: Write the failing test `tests/core/TurnRecordTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Errors.h"
#include "core/TurnRecord.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("CheckOutcome round-trips through JSON", "[turnrecord]")
{
    CheckOutcome outcome;
    outcome.stat = "agility";
    outcome.difficulty = "hard";
    outcome.stakes = "fall vs reach the hatch";
    outcome.result.rolls = {9, 5};
    outcome.result.statValue = 3;
    outcome.result.total = 17;
    outcome.result.target = 16;
    outcome.result.success = true;

    const CheckOutcome back = CheckOutcomeFromJson(CheckOutcomeToJson(outcome));
    CHECK(back.stat == "agility");
    CHECK(back.difficulty == "hard");
    CHECK(back.stakes == "fall vs reach the hatch");
    CHECK(back.result.rolls == std::vector<int>{9, 5});
    CHECK(back.result.statValue == 3);
    CHECK(back.result.total == 17);
    CHECK(back.result.target == 16);
    CHECK(back.result.success);
}

TEST_CASE("Applied and rejected changes round-trip through JSON", "[turnrecord]")
{
    const AppliedChange applied{Change{ChangeOp::Add, "player.health", -3, "fell"}, "clamped to 0"};
    const AppliedChange appliedBack = AppliedChangeFromJson(AppliedChangeToJson(applied));
    CHECK(appliedBack.change.op == ChangeOp::Add);
    CHECK(appliedBack.change.path == "player.health");
    CHECK(appliedBack.change.value == -3);
    CHECK(appliedBack.change.reason == "fell");
    CHECK(appliedBack.note == "clamped to 0");

    const RejectedChange rejected{nlohmann::json{{"op", "explode"}}, "unknown op"};
    const RejectedChange rejectedBack = RejectedChangeFromJson(RejectedChangeToJson(rejected));
    CHECK(rejectedBack.raw == nlohmann::json{{"op", "explode"}});
    CHECK(rejectedBack.reason == "unknown op");
}

TEST_CASE("Malformed stored turn data is a DataError", "[turnrecord]")
{
    CHECK_THROWS_AS(CheckOutcomeFromJson(nlohmann::json::array()), DataError);
    CHECK_THROWS_WITH(CheckOutcomeFromJson(nlohmann::json::parse(
                          R"({"stat":"a","difficulty":"b","stakes":"c","rolls":[7],"stat_value":0,"total":7,"target":8})")),
                      ContainsSubstring("success"));
    CHECK_THROWS_AS(AppliedChangeFromJson(nlohmann::json::parse(R"({"change":{"path":"x"},"note":""})")), DataError);
    CHECK_THROWS_AS(RejectedChangeFromJson(nlohmann::json::parse(R"({"raw":1})")), DataError);
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'core/TurnRecord.h'`.

- [ ] **Step 3: Create `src/core/TurnRecord.h`**

```cpp
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Changes.h"
#include "core/Dice.h"

namespace morpheus::core {

// One resolved check: the stat and difficulty the model asked for, the stakes it named, and what the dice did.
struct CheckOutcome {
    std::string stat = {};
    std::string difficulty = {};
    std::string stakes = {};
    CheckResult result = {};
};

// Everything one committed turn produced. Stored by store::RunStore and rendered into the transcript (L3) by
// the context module.
struct TurnRecord {
    int number = 0;
    std::string input = {};
    nlohmann::json resolve = nlohmann::json::object(); // Resolve call output exactly as the model produced it.
    std::vector<CheckOutcome> checks = {};
    std::vector<AppliedChange> applied = {};
    std::vector<RejectedChange> rejected = {};
    std::string narration = {};
    nlohmann::json timings = nlohmann::json::object();
    std::vector<std::string> notes = {}; // Model-written notes (assisted memory mode), committed with the turn.
};

// A folded range of turns (story summary, layer L2).
struct Summary {
    int fromTurn = 0;
    int toTurn = 0;
    std::string text = {};
};

// A model-written memory from assisted memory mode. Notes exist only for committed turns.
struct Note {
    int turn = 0;
    std::string text = {};
};

// JSON forms used for storage. Each FromJson throws DataError naming the missing or malformed member.
nlohmann::json CheckOutcomeToJson(const CheckOutcome& outcome);
CheckOutcome CheckOutcomeFromJson(const nlohmann::json& json);
nlohmann::json AppliedChangeToJson(const AppliedChange& applied);
AppliedChange AppliedChangeFromJson(const nlohmann::json& json);
nlohmann::json RejectedChangeToJson(const RejectedChange& rejected);
RejectedChange RejectedChangeFromJson(const nlohmann::json& json);

} // namespace morpheus::core
```

- [ ] **Step 4: Create `src/core/TurnRecord.cpp`, and add it to `src/core/CMakeLists.txt` after `JsonInt.cpp`**

```cpp
#include "core/TurnRecord.h"

#include <format>
#include <limits>
#include <optional>

#include "core/Errors.h"
#include "core/JsonInt.h"

namespace morpheus::core {

nlohmann::json CheckOutcomeToJson(const CheckOutcome& outcome)
{
    return {
        {"stat", outcome.stat},
        {"difficulty", outcome.difficulty},
        {"stakes", outcome.stakes},
        {"rolls", outcome.result.rolls},
        {"stat_value", outcome.result.statValue},
        {"total", outcome.result.total},
        {"target", outcome.result.target},
        {"success", outcome.result.success},
    };
}

CheckOutcome CheckOutcomeFromJson(const nlohmann::json& json)
{
    // The only writer is CheckOutcomeToJson, so any other shape means the stored data is damaged.
    auto malformed = [&](const char* member) {
        return DataError(std::format("check outcome: missing or malformed '{}' in {}", member, json.dump()));
    };
    if (!json.is_object()) {
        throw malformed("object");
    }
    auto requireString = [&](const char* key) {
        if (!json.contains(key) || !json.at(key).is_string()) {
            throw malformed(key);
        }
        return json.at(key).get<std::string>();
    };
    auto requireInt = [&](const char* key, long long lo, long long hi) {
        const std::optional<long long> value = json.contains(key) ? JsonIntInRange(json.at(key), lo, hi) : std::nullopt;
        if (!value) {
            throw malformed(key);
        }
        return *value;
    };

    CheckOutcome outcome;
    outcome.stat = requireString("stat");
    outcome.difficulty = requireString("difficulty");
    outcome.stakes = requireString("stakes");

    if (!json.contains("rolls") || !json.at("rolls").is_array()) {
        throw malformed("rolls");
    }
    for (const nlohmann::json& roll : json.at("rolls")) {
        const std::optional<long long> face = JsonIntInRange(roll, 1, 1000);
        if (!face) {
            throw malformed("rolls");
        }
        outcome.result.rolls.push_back(static_cast<int>(*face));
    }

    constexpr long long INT_LO = std::numeric_limits<int>::min();
    constexpr long long INT_HI = std::numeric_limits<int>::max();
    outcome.result.statValue = static_cast<int>(requireInt("stat_value", INT_LO, INT_HI));
    outcome.result.total = requireInt("total", std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max());
    outcome.result.target = static_cast<int>(requireInt("target", INT_LO, INT_HI));

    if (!json.contains("success") || !json.at("success").is_boolean()) {
        throw malformed("success");
    }
    outcome.result.success = json.at("success").get<bool>();
    return outcome;
}

nlohmann::json AppliedChangeToJson(const AppliedChange& applied)
{
    return {{"change", ChangeToJson(applied.change)}, {"note", applied.note}};
}

AppliedChange AppliedChangeFromJson(const nlohmann::json& json)
{
    if (!json.is_object() || !json.contains("change") || !json.contains("note") || !json.at("note").is_string()) {
        throw DataError(std::format("applied change: malformed {}", json.dump()));
    }
    // The stored change was valid when applied, so a parse failure means damaged data.
    std::string error;
    const std::optional<Change> change = ParseChange(json.at("change"), error);
    if (!change) {
        throw DataError(std::format("applied change: {}", error));
    }
    return {*change, json.at("note").get<std::string>()};
}

nlohmann::json RejectedChangeToJson(const RejectedChange& rejected)
{
    return {{"raw", rejected.raw}, {"reason", rejected.reason}};
}

RejectedChange RejectedChangeFromJson(const nlohmann::json& json)
{
    if (!json.is_object() || !json.contains("raw") || !json.contains("reason") || !json.at("reason").is_string()) {
        throw DataError(std::format("rejected change: malformed {}", json.dump()));
    }
    return {json.at("raw"), json.at("reason").get<std::string>()};
}

} // namespace morpheus::core
```

- [ ] **Step 5: Switch `store` to the core types**

**`src/store/RunStore.h`:**
- Delete the `TurnRecord`, `Summary` and `Note` structs.
- Add `#include "core/TurnRecord.h"`.
- At the top of `namespace morpheus::store`, add:

```cpp
// Turn data types live in core; store re-exports them so callers can keep writing store::TurnRecord.
using core::Note;
using core::Summary;
using core::TurnRecord;
```

**`src/store/RunStore.cpp`:**

1. In `SCHEMA_SQL`, rename the `turns` column `rolls` to `checks`.

2. In `CommitTurn`, serialize the typed members, then bind them in place of the old JSON members. The column list becomes `(number, input, resolve, checks, applied, rejected, narration, timings)`:

```cpp
    // Typed turn data is stored as JSON arrays in its own columns.
    nlohmann::json checks = nlohmann::json::array();
    for (const core::CheckOutcome& check : record.checks) {
        checks.push_back(core::CheckOutcomeToJson(check));
    }
    nlohmann::json applied = nlohmann::json::array();
    for (const core::AppliedChange& change : record.applied) {
        applied.push_back(core::AppliedChangeToJson(change));
    }
    nlohmann::json rejected = nlohmann::json::array();
    for (const core::RejectedChange& change : record.rejected) {
        rejected.push_back(core::RejectedChangeToJson(change));
    }
```

   Bind `checks.dump()`, `applied.dump()` and `rejected.dump()` to `?4`, `?5` and `?6`.

3. In `Turns`, select `checks` instead of `rolls`. After `parseColumn`, convert the three arrays. A JSON value that is not an array, or an element whose FromJson throws `core::DataError`, becomes a `StoreError` with the same wording as the parse error ("'<file>': turn N column '<name>' ..."). Use one local generic lambda for the three conversions:

```cpp
        // A stored column that parses but has the wrong shape is damaged data too.
        auto convertColumn = [&](int column, const char* name, auto fromJson, auto& out) {
            const nlohmann::json array = parseColumn(column, name);
            try {
                if (!array.is_array()) {
                    throw core::DataError("not an array");
                }
                for (const nlohmann::json& element : array) {
                    out.push_back(fromJson(element));
                }
            } catch (const core::DataError& error) {
                throw StoreError(std::format("'{}': turn {} column '{}' is malformed: {}",
                                             _file.generic_string(), record.number, name, error.what()));
            }
        };
        convertColumn(3, "checks", core::CheckOutcomeFromJson, record.checks);
        convertColumn(4, "applied", core::AppliedChangeFromJson, record.applied);
        convertColumn(5, "rejected", core::RejectedChangeFromJson, record.rejected);
```

- [ ] **Step 6: Migrate `tests/store/RunStoreTests.cpp`**

Replace the body of `TestTurn` with typed data, keeping its signature `TurnRecord TestTurn(int number, std::vector<std::string> notes = {})`:

```cpp
    TurnRecord record;
    record.number = number;
    record.input = std::format("input {}", number);
    record.notes = std::move(notes);
    record.resolve = {{"intent", "climb"}};
    morpheus::core::CheckOutcome check;
    check.stat = "agility";
    check.difficulty = "hard";
    check.stakes = "fall vs reach the hatch";
    check.result.rolls = {14};
    check.result.total = 14;
    check.result.target = 16;
    check.result.success = false;
    record.checks = {check};
    record.applied = {morpheus::core::AppliedChange{
        morpheus::core::Change{morpheus::core::ChangeOp::Set, "player.location", "hatch", ""}, ""}};
    record.narration = std::format("narration {}", number);
    record.timings = {{"resolve_ms", 120}};
    return record;
```

Update the assertions that read the old JSON members. The test currently has:
- `CHECK(first->rolls.at(0).at("total") == 14);`
- `CHECK(first->applied.size() == 1);`
- `CHECK(first->rejected.empty());`

They become:

```cpp
    REQUIRE(first->checks.size() == 1);
    CHECK(first->checks.at(0).stat == "agility");
    CHECK(first->checks.at(0).result.total == 14);
    REQUIRE(first->applied.size() == 1);
    CHECK(first->applied.at(0).change.path == "player.location");
    CHECK(first->rejected.empty());
```

Add a test for a stored column that parses but has the wrong shape:

```cpp
TEST_CASE("A turn column with the wrong shape is a StoreError naming the column", "[store]")
{
    const std::filesystem::path file = FreshDbPath("store_bad_checks");
    {
        RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
        store.CommitTurn(TestTurn(1), StateWithHealth(9));
    }
    {
        Database db = Database::Open(file, false);
        db.Exec("UPDATE turns SET checks = '[{\"stat\":1}]' WHERE number = 1");
    }
    RunStore store = RunStore::Open(file);
    CHECK_THROWS_WITH(store.Turn(1), ContainsSubstring("column 'checks'"));
}
```

Any other test that used `TurnRecord`'s old JSON members gets the same mechanical change.

- [ ] **Step 7: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass, including `[turnrecord]`.

- [ ] **Step 8: Commit**

```bash
git add src/core src/store tests
git commit -m "Move typed turn records into core" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Settings

**Files:**
- Create: `src/core/Settings.h`, `src/core/Settings.cpp`, `tests/core/SettingsTests.cpp`, `settings.example.json`
- Modify: `src/core/CMakeLists.txt` (add `Settings.cpp`), `.gitignore`

**Interfaces:**
- Consumes: `JsonIntInRange`, `DataError`.
- Produces (namespace `morpheus::core`):
  - `struct SamplingSettings { double temperature; int topK; double topP; }`
  - `struct BudgetSettings { int resolveMaxTokens, narrateMaxTokens, tailAllowance, engineRulesCap, worldCap, summaryCap; }`
  - `struct Settings { std::filesystem::path chatModel, embeddingModel; std::string embeddingQueryPrefix, embeddingDocumentPrefix; int contextSize; int gpuLayers; BudgetSettings budget; SamplingSettings resolveSampling, narrateSampling; }`
  - `Settings LoadSettings(const std::filesystem::path& file)`

- [ ] **Step 1: Write the failing test `tests/core/SettingsTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/Settings.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

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
        "budget": {
            "resolve_max_tokens": 512, "narrate_max_tokens": 768, "tail_allowance": 2048,
            "engine_rules_cap": 1024, "world_cap": 4096, "summary_cap": 2048
        },
        "sampling": {
            "resolve": {"temperature": 0.3, "top_k": 40, "top_p": 0.9},
            "narrate": {"temperature": 0.8, "top_k": 40, "top_p": 0.95}
        }
    })");
}

// Writes `json` as settings.json into a fresh folder that also holds the two dummy model files.
fs::path WriteSettings(const std::string& testName, const nlohmann::json& json)
{
    const fs::path dir = morpheus::test::FreshTempDir(testName);
    fs::create_directories(dir / "models");
    morpheus::test::WriteFile(dir / "models" / "chat.gguf", "x");
    morpheus::test::WriteFile(dir / "models" / "embed.gguf", "x");
    morpheus::test::WriteFile(dir / "settings.json", json.dump(2));
    return dir / "settings.json";
}

} // namespace

TEST_CASE("LoadSettings reads every value and resolves model paths", "[settings]")
{
    const fs::path file = WriteSettings("settings_valid", ValidSettingsJson());
    const Settings settings = LoadSettings(file);

    CHECK(settings.chatModel == file.parent_path() / "models" / "chat.gguf");
    CHECK(settings.embeddingModel == file.parent_path() / "models" / "embed.gguf");
    CHECK(settings.embeddingQueryPrefix == "search_query: ");
    CHECK(settings.contextSize == 16384);
    CHECK(settings.gpuLayers == -1);
    CHECK(settings.budget.narrateMaxTokens == 768);
    CHECK(settings.budget.summaryCap == 2048);
    CHECK(settings.resolveSampling.temperature == 0.3);
    CHECK(settings.narrateSampling.topP == 0.95);
}

TEST_CASE("LoadSettings names what is wrong", "[settings]")
{
    SECTION("missing model file")
    {
        nlohmann::json json = ValidSettingsJson();
        json["chat_model"] = "models/missing.gguf";
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_missing_model", json)), ContainsSubstring("chat_model"));
    }
    SECTION("unknown key")
    {
        nlohmann::json json = ValidSettingsJson();
        json["contxt_size"] = 1;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_unknown_key", json)), ContainsSubstring("contxt_size"));
    }
    SECTION("unknown nested key")
    {
        nlohmann::json json = ValidSettingsJson();
        json["sampling"]["narrate"]["temprature"] = 1.0;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_unknown_nested", json)), ContainsSubstring("temprature"));
    }
    SECTION("budget leaves no room for the transcript")
    {
        nlohmann::json json = ValidSettingsJson();
        json["context_size"] = 8192;
        json["budget"]["world_cap"] = 6000;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_budget", json)), ContainsSubstring("transcript"));
    }
    SECTION("top_p of zero")
    {
        nlohmann::json json = ValidSettingsJson();
        json["sampling"]["resolve"]["top_p"] = 0.0;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_top_p", json)), ContainsSubstring("top_p"));
    }
    SECTION("temperature out of range")
    {
        nlohmann::json json = ValidSettingsJson();
        json["sampling"]["narrate"]["temperature"] = 3.0;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_temperature", json)), ContainsSubstring("temperature"));
    }
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'core/Settings.h'`.

- [ ] **Step 3: Create `src/core/Settings.h`**

```cpp
#pragma once

#include <filesystem>
#include <string>

namespace morpheus::core {

struct SamplingSettings {
    double temperature = 0.0; // 0 means greedy decoding.
    int topK = 40;
    double topP = 0.95;
};

// Token budget for one assembled prompt (architecture spec, "Context layout and KV cache").
struct BudgetSettings {
    int resolveMaxTokens = 0; // Generation reserved for the resolve call.
    int narrateMaxTokens = 0; // Generation reserved for the narrate call.
    int tailAllowance = 0;    // State view + retrieved snippets + player input + instruction (T1-T4).
    int engineRulesCap = 0;   // L0
    int worldCap = 0;         // L1: world rules + always-on lore.
    int summaryCap = 0;       // L2
};

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
};

// Loads and validates settings.json. Throws DataError naming the file and key at fault. Unknown keys are errors,
// both model files must exist, and the budget must leave room for the transcript (L3).
Settings LoadSettings(const std::filesystem::path& file);

} // namespace morpheus::core
```

- [ ] **Step 4: Create `src/core/Settings.cpp`, and add it to `src/core/CMakeLists.txt` after `JsonInt.cpp`**

```cpp
#include "core/Settings.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <initializer_list>
#include <optional>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/JsonInt.h"

namespace morpheus::core {

Settings LoadSettings(const std::filesystem::path& file)
{
    const std::string where = std::format("'{}'", file.generic_string());

    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw DataError(std::format("cannot read {}", where));
    }
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(stream);
    } catch (const nlohmann::json::parse_error& error) {
        throw DataError(std::format("{}: {}", where, error.what()));
    }

    // Typos must fail loudly instead of silently falling back to a default.
    auto checkKeys = [&](const nlohmann::json& object, const char* context, std::initializer_list<const char*> allowed) {
        if (!object.is_object()) {
            throw DataError(std::format("{}: {} must be an object", where, context));
        }
        for (auto it = object.begin(); it != object.end(); ++it) {
            const bool known = std::any_of(allowed.begin(), allowed.end(), [&](const char* key) { return it.key() == key; });
            if (!known) {
                throw DataError(std::format("{}: unknown key '{}' in {}", where, it.key(), context));
            }
        }
    };
    auto requireMember = [&](const nlohmann::json& object, const char* key, const char* context) -> const nlohmann::json& {
        if (!object.contains(key)) {
            throw DataError(std::format("{}: {} needs '{}'", where, context, key));
        }
        return object.at(key);
    };
    auto requireInt = [&](const nlohmann::json& object, const char* key, const char* context, long long lo, long long hi) {
        const std::optional<long long> value = JsonIntInRange(requireMember(object, key, context), lo, hi);
        if (!value) {
            throw DataError(std::format("{}: {} '{}' must be an integer in {}..{}", where, context, key, lo, hi));
        }
        return static_cast<int>(*value);
    };
    auto requireNumber = [&](const nlohmann::json& object, const char* key, const char* context, double lo, double hi) {
        const nlohmann::json& value = requireMember(object, key, context);
        if (!value.is_number() || value.get<double>() < lo || value.get<double>() > hi) {
            throw DataError(std::format("{}: {} '{}' must be a number in {}..{}", where, context, key, lo, hi));
        }
        return value.get<double>();
    };
    auto requireString = [&](const nlohmann::json& object, const char* key, const char* context) {
        const nlohmann::json& value = requireMember(object, key, context);
        if (!value.is_string()) {
            throw DataError(std::format("{}: {} '{}' must be a string", where, context, key));
        }
        return value.get<std::string>();
    };

    checkKeys(json, "settings", {"chat_model", "embedding_model", "embedding_query_prefix", "embedding_document_prefix",
                                 "context_size", "gpu_layers", "budget", "sampling"});

    Settings settings;

    // Model files: relative to the settings file's folder, and they must exist now rather than at first use.
    auto requireModel = [&](const char* key) {
        const std::string text = requireString(json, key, "settings");
        if (text.empty()) {
            throw DataError(std::format("{}: '{}' must not be empty", where, key));
        }
        // The JSON text is UTF-8; construct the path from char8_t so non-ASCII folders survive on Windows.
        std::filesystem::path path(reinterpret_cast<const char8_t*>(text.c_str()));
        if (path.is_relative()) {
            path = file.parent_path() / path;
        }
        if (!std::filesystem::is_regular_file(path)) {
            throw DataError(std::format("{}: '{}' file '{}' does not exist", where, key, path.generic_string()));
        }
        return path;
    };
    settings.chatModel = requireModel("chat_model");
    settings.embeddingModel = requireModel("embedding_model");
    settings.embeddingQueryPrefix = requireString(json, "embedding_query_prefix", "settings");
    settings.embeddingDocumentPrefix = requireString(json, "embedding_document_prefix", "settings");
    settings.contextSize = requireInt(json, "context_size", "settings", 512, 262144);
    settings.gpuLayers = requireInt(json, "gpu_layers", "settings", -1, 10000);

    // Budget: every part positive and below the context size.
    const nlohmann::json& budget = requireMember(json, "budget", "settings");
    checkKeys(budget, "budget", {"resolve_max_tokens", "narrate_max_tokens", "tail_allowance", "engine_rules_cap",
                                 "world_cap", "summary_cap"});
    const long long contextSize = settings.contextSize;
    settings.budget.resolveMaxTokens = requireInt(budget, "resolve_max_tokens", "budget", 1, contextSize);
    settings.budget.narrateMaxTokens = requireInt(budget, "narrate_max_tokens", "budget", 1, contextSize);
    settings.budget.tailAllowance = requireInt(budget, "tail_allowance", "budget", 1, contextSize);
    settings.budget.engineRulesCap = requireInt(budget, "engine_rules_cap", "budget", 1, contextSize);
    settings.budget.worldCap = requireInt(budget, "world_cap", "budget", 1, contextSize);
    settings.budget.summaryCap = requireInt(budget, "summary_cap", "budget", 1, contextSize);
    const long long fixed = static_cast<long long>(settings.budget.resolveMaxTokens) + settings.budget.narrateMaxTokens +
                            settings.budget.tailAllowance + settings.budget.engineRulesCap + settings.budget.worldCap +
                            settings.budget.summaryCap;
    if (fixed >= contextSize) {
        throw DataError(std::format("{}: budget uses {} of {} context tokens and leaves no room for the transcript",
                                    where, fixed, contextSize));
    }

    // Sampling per call type.
    const nlohmann::json& sampling = requireMember(json, "sampling", "settings");
    checkKeys(sampling, "sampling", {"resolve", "narrate"});
    auto readSampling = [&](const char* callType) {
        const nlohmann::json& object = requireMember(sampling, callType, "sampling");
        checkKeys(object, callType, {"temperature", "top_k", "top_p"});
        SamplingSettings result;
        result.temperature = requireNumber(object, "temperature", callType, 0.0, 2.0);
        result.topK = requireInt(object, "top_k", callType, 1, 1000);
        result.topP = requireNumber(object, "top_p", callType, 0.0, 1.0);
        if (result.topP <= 0.0) {
            throw DataError(std::format("{}: {} 'top_p' must be greater than 0", where, callType));
        }
        return result;
    };
    settings.resolveSampling = readSampling("resolve");
    settings.narrateSampling = readSampling("narrate");
    return settings;
}

} // namespace morpheus::core
```

- [ ] **Step 5: Create `settings.example.json` and update `.gitignore`**

`settings.example.json`, at the repo root:

```json
{
  "chat_model": "models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf",
  "embedding_model": "models/nomic-embed-text-v1.5.Q8_0.gguf",
  "embedding_query_prefix": "search_query: ",
  "embedding_document_prefix": "search_document: ",
  "context_size": 16384,
  "gpu_layers": -1,
  "budget": {
    "resolve_max_tokens": 512,
    "narrate_max_tokens": 768,
    "tail_allowance": 2048,
    "engine_rules_cap": 1024,
    "world_cap": 4096,
    "summary_cap": 2048
  },
  "sampling": {
    "resolve": { "temperature": 0.3, "top_k": 40, "top_p": 0.9 },
    "narrate": { "temperature": 0.8, "top_k": 40, "top_p": 0.95 }
  }
}
```

Append to `.gitignore`:

```
models/
settings.json
```

- [ ] **Step 6: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass, including `[settings]`.

- [ ] **Step 7: Commit**

```bash
git add src/core tests/core/SettingsTests.cpp settings.example.json .gitignore
git commit -m "Add settings.json loading" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Inference interface, Vulkan llama.cpp build, test fake

**Files:**
- Modify: `CMakeLists.txt`, `CMakePresets.json`, `tests/CMakeLists.txt`
- Create: `src/inference/CMakeLists.txt`, `src/inference/Llm.h`, `src/inference/Llm.cpp`, `tests/fakes/FakeBackend.h`, `tests/inference/LlmTests.cpp`

**Interfaces:**
- Consumes: `core::ContractError`.
- Produces (namespace `morpheus::inference`):
  - `enum class Role { System, User, Assistant }` and `std::string RoleName(Role)`, which returns `"system"`, `"user"` or `"assistant"`
  - `struct Message { Role role; std::string content; }`
  - `struct Prompt { std::vector<Message> messages; std::size_t stableCount; }`
  - `struct Sampling { float temperature; int topK; float topP; std::uint32_t seed; }`
  - `struct GenerationSpec { std::string grammar; int maxTokens; Sampling sampling; }`
  - `struct Completion { std::string text; int promptTokens, tokensReused, tokensEvaluated, tokensGenerated; bool stoppedAtLimit, cancelled; double promptMs, generateMs; }`
  - `enum class EmbedPurpose { Query, Document }`
  - `using TokenCallback = std::function<void(std::string_view)>`
  - `class InferenceError : std::runtime_error`
  - `class LlmBackend`, with pure virtual methods:
    - `int CountTokens(std::string_view)`
    - `int CountPromptTokens(const Prompt&)`
    - `int ContextSize() const`
    - `Completion Complete(const Prompt&, const GenerationSpec&, const TokenCallback&, std::stop_token)`
    - `std::vector<std::vector<float>> Embed(const std::vector<std::string>&, EmbedPurpose)`
    - `std::string EmbeddingModelId() const`
    - `std::vector<std::uint8_t> SaveState()`
    - `void RestoreState(std::span<const std::uint8_t>)`
  - Test double `morpheus::test::FakeBackend(int contextSize)`:
    - `CountTokens` = bytes / 4, rounded up.
    - `CountPromptTokens` = the sum, over messages, of the content's tokens + 4.
    - Every other method throws `ContractError`.
- CMake:
  - option `MORPHEUS_BUILD_LLAMA` (default ON) and option `MORPHEUS_VULKAN` (default ON);
  - targets `morpheus_inference` and (later) `morpheus_llama`.

- [ ] **Step 1: Write the failing test `tests/inference/LlmTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

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
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'fakes/FakeBackend.h'` (or `'inference/Llm.h'`).

- [ ] **Step 3: Create `src/inference/Llm.h`**

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace morpheus::inference {

// A model or runtime failure: a model file that will not load, a prompt that does not fit, a failed decode.
class InferenceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class Role { System, User, Assistant };

// Role spelling used by chat templates: "system", "user", "assistant".
std::string RoleName(Role role);

struct Message {
    Role role = Role::User;
    std::string content = {};
};

// A chat prompt. messages[0, stableCount) is the stable prefix (layers L0-L3); the rest is the volatile tail.
// The llama.cpp backend reuses the KV cache by longest common token prefix, so stableCount is informational for
// it; a cloud backend places its prompt-cache breakpoint there.
struct Prompt {
    std::vector<Message> messages = {};
    std::size_t stableCount = 0;
};

struct Sampling {
    float temperature = 0.0f; // <= 0 means greedy decoding.
    int topK = 40;
    float topP = 0.95f;
    std::uint32_t seed = 0;
};

struct GenerationSpec {
    std::string grammar = {}; // GBNF with a "root" rule; empty means unconstrained.
    int maxTokens = 256;
    Sampling sampling = {};
};

struct Completion {
    std::string text = {};
    int promptTokens = 0;     // Tokens in the templated prompt.
    int tokensReused = 0;     // Prompt tokens served from the KV cache.
    int tokensEvaluated = 0;  // Prompt tokens computed for this call.
    int tokensGenerated = 0;
    bool stoppedAtLimit = false; // Generation reached maxTokens before an end-of-generation token.
    bool cancelled = false;
    double promptMs = 0.0;
    double generateMs = 0.0;
};

enum class EmbedPurpose { Query, Document };

// Receives generated text as it streams. Pieces never split a UTF-8 character.
using TokenCallback = std::function<void(std::string_view piece)>;

// The engine's only view of a language model (architecture spec, "Inference interface").
class LlmBackend {
public:
    virtual ~LlmBackend() = default;

    // Tokens in raw text: no chat template, no special tokens.
    virtual int CountTokens(std::string_view text) = 0;

    // Tokens in the full templated prompt, exactly as Complete would evaluate it.
    virtual int CountPromptTokens(const Prompt& prompt) = 0;

    virtual int ContextSize() const = 0;

    // Generates the assistant's reply to `prompt`. Throws InferenceError if prompt + spec.maxTokens exceeds the
    // context. Cancellation through `stop` returns early with completion.cancelled set; it is not an error.
    virtual Completion Complete(const Prompt& prompt, const GenerationSpec& spec, const TokenCallback& onToken,
                                std::stop_token stop) = 0;

    // One L2-normalized vector per text, in order.
    virtual std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, EmbedPurpose purpose) = 0;

    // Identifies the embedding model, for store::EmbeddingRow::model.
    virtual std::string EmbeddingModelId() const = 0;

    // The chat KV cache and the tokens it holds, so the GPU can be handed to another model and the cache restored.
    virtual std::vector<std::uint8_t> SaveState() = 0;
    virtual void RestoreState(std::span<const std::uint8_t> state) = 0;
};

} // namespace morpheus::inference
```

- [ ] **Step 4: Create `src/inference/Llm.cpp` and `src/inference/CMakeLists.txt`**

`src/inference/Llm.cpp`:

```cpp
#include "inference/Llm.h"

#include "core/Errors.h"

namespace morpheus::inference {

std::string RoleName(Role role)
{
    switch (role) {
    case Role::System:
        return "system";
    case Role::User:
        return "user";
    case Role::Assistant:
        return "assistant";
    }
    throw core::ContractError("RoleName: unhandled Role");
}

} // namespace morpheus::inference
```

`src/inference/CMakeLists.txt`:

```cmake
add_library(morpheus_inference STATIC
    Llm.cpp
)
target_include_directories(morpheus_inference PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_inference PUBLIC morpheus_core)
morpheus_target_defaults(morpheus_inference)
```

- [ ] **Step 5: Create `tests/fakes/FakeBackend.h`**

```cpp
#pragma once

#include "core/Errors.h"
#include "inference/Llm.h"

namespace morpheus::test {

// Deterministic stand-in for context tests. One token per 4 bytes (rounded up), plus 4 template tokens per
// message. Generation, embeddings and state are not used by those tests and throw if called.
class FakeBackend final : public inference::LlmBackend {
private:
    int _contextSize = 0;

public:
    explicit FakeBackend(int contextSize)
        : _contextSize(contextSize)
    {
    }

    int CountTokens(std::string_view text) override
    {
        return static_cast<int>((text.size() + 3) / 4);
    }

    int CountPromptTokens(const inference::Prompt& prompt) override
    {
        int total = 0;
        for (const inference::Message& message : prompt.messages) {
            total += CountTokens(message.content) + 4;
        }
        return total;
    }

    int ContextSize() const override
    {
        return _contextSize;
    }

    inference::Completion Complete(const inference::Prompt&, const inference::GenerationSpec&,
                                   const inference::TokenCallback&, std::stop_token) override
    {
        throw core::ContractError("FakeBackend::Complete is not available");
    }

    std::vector<std::vector<float>> Embed(const std::vector<std::string>&, inference::EmbedPurpose) override
    {
        throw core::ContractError("FakeBackend::Embed is not available");
    }

    std::string EmbeddingModelId() const override
    {
        throw core::ContractError("FakeBackend::EmbeddingModelId is not available");
    }

    std::vector<std::uint8_t> SaveState() override
    {
        throw core::ContractError("FakeBackend::SaveState is not available");
    }

    void RestoreState(std::span<const std::uint8_t>) override
    {
        throw core::ContractError("FakeBackend::RestoreState is not available");
    }
};

} // namespace morpheus::test
```

- [ ] **Step 6: Restructure the CMake build**

In the root `CMakeLists.txt`:

1. Add, after the existing options:

```cmake
option(MORPHEUS_BUILD_LLAMA "Build the llama.cpp backend (fetches and builds llama.cpp)" ON)
option(MORPHEUS_VULKAN "Build llama.cpp with the Vulkan GPU backend (needs the Vulkan SDK)" ON)

if(MORPHEUS_BUILD_APP AND NOT MORPHEUS_BUILD_LLAMA)
    message(FATAL_ERROR "MORPHEUS_BUILD_APP needs MORPHEUS_BUILD_LLAMA.")
endif()
```

2. Move the `llama` FetchContent block out of `if(MORPHEUS_BUILD_APP)` into its own block, placed before it:

```cmake
if(MORPHEUS_BUILD_LLAMA)
    # GPU backend for llama.cpp. FORCE so that toggling MORPHEUS_VULKAN reconfigures the fetched llama.cpp.
    set(GGML_VULKAN ${MORPHEUS_VULKAN} CACHE BOOL "" FORCE)

    # llama
    FetchContent_Declare(
        llama                                                         # prefix for CMake variables
        GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git     # repo TODO: redirect to the new repo: ggml-org/llama.cpp , probably update the targeted commit?
        GIT_TAG        960d628f46d17ea9c10aa944b061b2341afeb3b5       # commit SHA to get, version b9804
        GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
        SYSTEM                                                        # Its headers are external: no /W4 /WX
    )
    FetchContent_MakeAvailable(llama)
endif()
```

   SDL3 and imgui stay inside `if(MORPHEUS_BUILD_APP)`.

3. Add `add_subdirectory(src/inference)` right after `add_subdirectory(src/store)`.

In `tests/CMakeLists.txt`, right after the `file(GLOB_RECURSE ...)` line, add:

```cmake
# Model-backed tests need llama.cpp; presets without it skip their sources entirely.
if(NOT MORPHEUS_BUILD_LLAMA)
    list(FILTER MORPHEUS_TEST_SOURCES EXCLUDE REGEX "/tests/llama/")
endif()
```

and add `morpheus_inference` to `morpheus_tests`'s `target_link_libraries` list.

In `CMakePresets.json`:
- Add `"MORPHEUS_BUILD_LLAMA": "ON"` and `"MORPHEUS_VULKAN": "ON"` to the `dev` preset's `cacheVariables`.
- Add `"MORPHEUS_BUILD_LLAMA": "OFF"` to the `tests` preset's `cacheVariables`. The `asan` preset inherits it.

- [ ] **Step 7: Build and run under `tests`. Expect a pass.**

Run: `cmake --preset tests && cmake --build --preset tests && ctest --preset tests`
Expected: configuration does not fetch llama.cpp, and all tests pass, including `[inference]`.

- [ ] **Step 8: Configure and build `dev` with Vulkan. Expect success.**

Run (long; use a 600000 ms timeout or a background run): `cmake --preset dev && cmake --build --preset dev`

Expected:
- llama.cpp configures with `GGML_VULKAN=ON`: the configure output mentions Vulkan and finds `glslc`.
- `build/dev/bin/Debug` contains `ggml-vulkan.dll` and `morpheus.exe`.

If Vulkan fails to configure, report the exact error. Do not switch to CPU on your own.

- [ ] **Step 9: Commit**

```bash
git add CMakeLists.txt CMakePresets.json tests src/inference
git commit -m "Add the inference interface and build llama.cpp with Vulkan" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

### Task 5: LlamaCppBackend

**Files:**
- Create: `src/llama/CMakeLists.txt`, `src/llama/LlamaCppBackend.h`, `src/llama/LlamaCppBackend.cpp`, `tests/llama/LlamaTestModels.h`, `tests/llama/LlamaCompletionTests.cpp`
- Modify: root `CMakeLists.txt` (`add_subdirectory(src/llama)` inside `if(MORPHEUS_BUILD_LLAMA)`), `tests/CMakeLists.txt` (link `morpheus_llama` when `MORPHEUS_BUILD_LLAMA`)

**Interfaces:**
- Consumes: everything in `inference/Llm.h` (Task 4).
- Produces (namespace `morpheus::llama`):
  - `struct LlamaConfig { std::filesystem::path chatModel, embeddingModel; std::string embeddingQueryPrefix, embeddingDocumentPrefix; int contextSize = 8192; int gpuLayers = -1; int embeddingContextSize = 2048; }`
  - `class LlamaCppBackend final : public inference::LlmBackend`, with `explicit LlamaCppBackend(LlamaConfig)`
- llama.cpp facts used, checked against `build/dev/_deps/llama-src/include/llama.h`:
  - `llama_memory_seq_rm(mem, seq, p0, p1)` returns false when a partial removal is unsupported.
  - `llama_chat_apply_template(tmpl, chat, n, add_ass, buf, len)` returns the needed length.
  - `llama_sampler_init_grammar(vocab, gbnf, "root")` returns NULL on a parse error.
  - `llama_sampler_sample` also accepts the token into the chain.
  - `llama_state_seq_get_size/get_data/set_data` (set_data returns 0 on failure).
  - `llama_get_embeddings_seq` returns NULL without pooling.

- [ ] **Step 1: Write the shared test fixture `tests/llama/LlamaTestModels.h`**

```cpp
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>
#include <string>

#include "llama/LlamaCppBackend.h"

namespace morpheus::test {

// Reads an environment variable; "" when unset. _dupenv_s avoids MSVC's getenv deprecation warning under /WX.
inline std::string EnvironmentVariable(const char* name)
{
#ifdef _MSC_VER
    char* pValue = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&pValue, &length, name) != 0 || pValue == nullptr) {
        return {};
    }
    std::string value(pValue);
    std::free(pValue);
    return value;
#else
    const char* pValue = std::getenv(name);
    return pValue != nullptr ? pValue : "";
#endif
}

// Model-backed tests need real GGUF files, named by environment variables:
//   MORPHEUS_TEST_CHAT_MODEL   chat model (Qwen3-4B-Instruct-2507-Q4_K_M.gguf)
//   MORPHEUS_TEST_EMBED_MODEL  embedding model (nomic-embed-text-v1.5.Q8_0.gguf)
// The calling test is skipped when either is unset. Loading is slow, so every test shares one backend.
inline llama::LlamaCppBackend& SharedBackend()
{
    const std::string chatModel = EnvironmentVariable("MORPHEUS_TEST_CHAT_MODEL");
    const std::string embedModel = EnvironmentVariable("MORPHEUS_TEST_EMBED_MODEL");
    if (chatModel.empty() || embedModel.empty()) {
        SKIP("set MORPHEUS_TEST_CHAT_MODEL and MORPHEUS_TEST_EMBED_MODEL to run model-backed tests");
    }
    static std::unique_ptr<llama::LlamaCppBackend> pBackend = [&] {
        llama::LlamaConfig config;
        config.chatModel = chatModel;
        config.embeddingModel = embedModel;
        config.embeddingQueryPrefix = "search_query: ";
        config.embeddingDocumentPrefix = "search_document: ";
        config.contextSize = 4096;
        config.gpuLayers = -1;
        return std::make_unique<llama::LlamaCppBackend>(config);
    }();
    return *pBackend;
}

} // namespace morpheus::test
```

- [ ] **Step 2: Write the failing test `tests/llama/LlamaCompletionTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <stop_token>
#include <string>

#include "core/Errors.h"
#include "llama/LlamaTestModels.h"

using namespace morpheus::inference;
using morpheus::test::SharedBackend;

namespace {

Prompt SimplePrompt(const std::string& question)
{
    Prompt prompt;
    prompt.messages = {{Role::System, "You are a terse assistant."}, {Role::User, question}};
    prompt.stableCount = 1;
    return prompt;
}

// Greedy decoding (temperature 0) keeps model-backed tests as repeatable as the hardware allows.
GenerationSpec Greedy(int maxTokens)
{
    GenerationSpec spec;
    spec.maxTokens = maxTokens;
    return spec;
}

} // namespace

TEST_CASE("LlamaCppBackend counts tokens", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    CHECK(backend.CountTokens("") == 0);
    const int sentence = backend.CountTokens("The crawler grinds across the dunes.");
    CHECK(sentence > 3);
    CHECK(sentence < 20);
    CHECK(backend.CountPromptTokens(SimplePrompt("Hi")) > backend.CountTokens("Hi"));
    CHECK(backend.ContextSize() == 4096);
}

TEST_CASE("LlamaCppBackend generates and streams text", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    std::string streamed;
    const Completion completion = backend.Complete(SimplePrompt("Say one short sentence about sand."), Greedy(24),
                                                   [&](std::string_view piece) { streamed += piece; }, {});
    CHECK_FALSE(completion.text.empty());
    CHECK(streamed == completion.text);
    CHECK(completion.tokensGenerated > 0);
    CHECK(completion.tokensGenerated <= 24);
    CHECK_FALSE(completion.cancelled);
}

TEST_CASE("LlamaCppBackend reuses the cached prompt prefix", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    const Prompt first = SimplePrompt("Name a color.");
    const Completion a = backend.Complete(first, Greedy(8), {}, {});

    // The same prompt again: everything but the last prompt token comes from the cache.
    const Completion b = backend.Complete(first, Greedy(8), {}, {});
    CHECK(b.tokensReused == b.promptTokens - 1);
    CHECK(b.tokensEvaluated == 1);

    // Appending messages (the narrate-call shape) keeps the whole earlier prompt cached.
    Prompt longer = first;
    longer.messages.push_back({Role::Assistant, a.text});
    longer.messages.push_back({Role::User, "Name another."});
    const Completion c = backend.Complete(longer, Greedy(8), {}, {});
    CHECK(c.tokensReused >= a.promptTokens);
}

TEST_CASE("LlamaCppBackend follows a GBNF grammar", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    GenerationSpec spec = Greedy(4);
    spec.grammar = R"(root ::= "yes" | "no")";
    const Completion completion = backend.Complete(SimplePrompt("Is sand dry? Answer yes or no."), spec, {}, {});
    CHECK((completion.text == "yes" || completion.text == "no"));
}

TEST_CASE("LlamaCppBackend returns early when cancelled", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    std::stop_source source;
    source.request_stop();
    const Completion completion = backend.Complete(SimplePrompt("Tell a long story."), Greedy(64), {}, source.get_token());
    CHECK(completion.cancelled);
    CHECK(completion.tokensGenerated == 0);
}

TEST_CASE("LlamaCppBackend rejects requests that cannot work", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    CHECK_THROWS_AS(backend.Complete(SimplePrompt("Hi"), Greedy(5000), {}, {}), InferenceError);

    GenerationSpec badGrammar = Greedy(4);
    badGrammar.grammar = "root ::= (";
    CHECK_THROWS_AS(backend.Complete(SimplePrompt("Hi"), badGrammar, {}, {}), morpheus::core::ContractError);
}
```

- [ ] **Step 3: Build under `dev`. Expect failure.**

Run: `cmake --build --preset dev`
Expected: FAIL with `error C1083: Cannot open include file: 'llama/LlamaCppBackend.h'`.

- [ ] **Step 4: Create `src/llama/LlamaCppBackend.h`**

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "inference/Llm.h"

struct llama_context;
struct llama_model;
struct llama_vocab;

namespace morpheus::llama {

struct LlamaConfig {
    std::filesystem::path chatModel = {};
    std::filesystem::path embeddingModel = {};
    std::string embeddingQueryPrefix = {};    // Prepended for EmbedPurpose::Query.
    std::string embeddingDocumentPrefix = {}; // Prepended for EmbedPurpose::Document.
    int contextSize = 8192;
    int gpuLayers = -1;                       // -1 puts every layer on the GPU.
    int embeddingContextSize = 2048;          // Longest text (in tokens) that can be embedded.
};

// llama.cpp in-process: a chat model for generation and token counting, plus a sentence-embedding model.
// The chat KV cache is reused by longest common token prefix across calls. Not thread-safe: one thread owns it.
class LlamaCppBackend final : public inference::LlmBackend {
public:
    struct ModelDeleter {
        void operator()(llama_model* pModel) const;
    };
    struct ContextDeleter {
        void operator()(llama_context* pContext) const;
    };

private:
    LlamaConfig _config = {};
    std::unique_ptr<llama_model, ModelDeleter> _pChatModel = nullptr;
    std::unique_ptr<llama_context, ContextDeleter> _pChatContext = nullptr;
    std::unique_ptr<llama_model, ModelDeleter> _pEmbedModel = nullptr;
    std::unique_ptr<llama_context, ContextDeleter> _pEmbedContext = nullptr;
    std::vector<std::int32_t> _cachedTokens = {}; // Exactly the tokens in the chat KV cache (sequence 0), in order.

public:
    // Loads both models and creates their contexts. Throws InferenceError if either fails.
    explicit LlamaCppBackend(LlamaConfig config);
    ~LlamaCppBackend() override;
    LlamaCppBackend(const LlamaCppBackend&) = delete;
    LlamaCppBackend& operator=(const LlamaCppBackend&) = delete;

    int CountTokens(std::string_view text) override;
    int CountPromptTokens(const inference::Prompt& prompt) override;
    int ContextSize() const override;
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
    std::string EmbeddingModelId() const override;
    std::vector<std::uint8_t> SaveState() override;
    void RestoreState(std::span<const std::uint8_t> state) override;

private:
    // The prompt rendered through the chat model's template, ending where the assistant's reply begins.
    std::string RenderPrompt(const inference::Prompt& prompt) const;

    // RenderPrompt tokenized the way Complete evaluates it.
    std::vector<std::int32_t> TokenizePrompt(const inference::Prompt& prompt) const;

    // Decodes `tokens` into the chat cache right after _cachedTokens, then appends them to _cachedTokens.
    // On failure the cache is cleared (it may hold part of the batch) and InferenceError is thrown.
    void DecodeChat(std::span<const std::int32_t> tokens, bool wantLogits);

    static std::vector<std::int32_t> Tokenize(const llama_vocab* pVocab, std::string_view text, bool addSpecial,
                                              bool parseSpecial);
};

} // namespace morpheus::llama
```

- [ ] **Step 5: Create `src/llama/LlamaCppBackend.cpp`**

```cpp
#include "llama/LlamaCppBackend.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>

#include <llama.h>

#include "core/Errors.h"

namespace morpheus::llama {

using inference::InferenceError;

namespace {

// llama.cpp and ggml log through one process-wide callback. Only warnings and errors reach stderr.
void LogFilter(ggml_log_level level, const char* pText, void* /*pUserData*/)
{
    if (level == GGML_LOG_LEVEL_WARN || level == GGML_LOG_LEVEL_ERROR) {
        std::fputs(pText, stderr);
    }
}

// Owns a llama_batch from llama_batch_init.
struct BatchGuard {
    llama_batch batch;

    explicit BatchGuard(std::size_t capacity)
        : batch(llama_batch_init(static_cast<int32_t>(capacity), 0, 1))
    {
    }
    ~BatchGuard()
    {
        llama_batch_free(batch);
    }
    BatchGuard(const BatchGuard&) = delete;
    BatchGuard& operator=(const BatchGuard&) = delete;
};

enum class Outputs { Last, All, None };

// Fills a batch with `tokens` for sequence 0, at positions startPosition onward.
void FillBatch(llama_batch& batch, std::span<const std::int32_t> tokens, std::size_t startPosition, Outputs outputs)
{
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        batch.token[i] = tokens[i];
        batch.pos[i] = static_cast<llama_pos>(startPosition + i);
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        const bool output = outputs == Outputs::All || (outputs == Outputs::Last && i + 1 == tokens.size());
        batch.logits[i] = static_cast<int8_t>(output ? 1 : 0);
    }
    batch.n_tokens = static_cast<int32_t>(tokens.size());
}

struct SamplerDeleter {
    void operator()(llama_sampler* pSampler) const
    {
        llama_sampler_free(pSampler);
    }
};

} // namespace

void LlamaCppBackend::ModelDeleter::operator()(llama_model* pModel) const
{
    llama_model_free(pModel);
}

void LlamaCppBackend::ContextDeleter::operator()(llama_context* pContext) const
{
    llama_free(pContext);
}

LlamaCppBackend::LlamaCppBackend(LlamaConfig config)
    : _config(std::move(config))
{
    MORPHEUS_REQUIRE(_config.contextSize > 0 && _config.embeddingContextSize > 0,
                     "LlamaCppBackend: context sizes must be positive");

    // llama.cpp's global state is initialised once per process and never torn down.
    static std::once_flag initOnce;
    std::call_once(initOnce, [] {
        llama_log_set(LogFilter, nullptr);
        llama_backend_init();
    });

    auto loadModel = [this](const std::filesystem::path& path) {
        llama_model_params params = llama_model_default_params();
        params.n_gpu_layers = _config.gpuLayers;
        llama_model* pModel = llama_model_load_from_file(path.string().c_str(), params);
        if (pModel == nullptr) {
            throw InferenceError(std::format("cannot load model '{}'", path.generic_string()));
        }
        return std::unique_ptr<llama_model, ModelDeleter>(pModel);
    };

    // Chat model: generation and token counting on sequence 0.
    _pChatModel = loadModel(_config.chatModel);
    {
        llama_context_params params = llama_context_default_params();
        params.n_ctx = static_cast<uint32_t>(_config.contextSize);
        params.n_seq_max = 1;
        _pChatContext.reset(llama_init_from_model(_pChatModel.get(), params));
        if (!_pChatContext) {
            throw InferenceError(std::format("cannot create a chat context of {} tokens", _config.contextSize));
        }
    }

    // Embedding model: pooled sentence embeddings. A whole text is one ubatch, as encoder models require.
    _pEmbedModel = loadModel(_config.embeddingModel);
    {
        llama_context_params params = llama_context_default_params();
        const auto size = static_cast<uint32_t>(_config.embeddingContextSize);
        params.n_ctx = size;
        params.n_batch = size;
        params.n_ubatch = size;
        params.n_seq_max = 1;
        params.embeddings = true;
        _pEmbedContext.reset(llama_init_from_model(_pEmbedModel.get(), params));
        if (!_pEmbedContext) {
            throw InferenceError("cannot create the embedding context");
        }
        if (llama_pooling_type(_pEmbedContext.get()) == LLAMA_POOLING_TYPE_NONE) {
            throw InferenceError(std::format("'{}' has no pooling; use a sentence-embedding model",
                                             _config.embeddingModel.generic_string()));
        }
    }
}

LlamaCppBackend::~LlamaCppBackend() = default;

int LlamaCppBackend::CountTokens(std::string_view text)
{
    return static_cast<int>(Tokenize(llama_model_get_vocab(_pChatModel.get()), text, false, false).size());
}

int LlamaCppBackend::CountPromptTokens(const inference::Prompt& prompt)
{
    return static_cast<int>(TokenizePrompt(prompt).size());
}

int LlamaCppBackend::ContextSize() const
{
    return static_cast<int>(llama_n_ctx(_pChatContext.get()));
}

inference::Completion LlamaCppBackend::Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                                const inference::TokenCallback& onToken, std::stop_token stop)
{
    MORPHEUS_REQUIRE(spec.maxTokens > 0, "Complete: maxTokens must be positive");
    using Clock = std::chrono::steady_clock;
    const Clock::time_point promptStart = Clock::now();
    const llama_vocab* pVocab = llama_model_get_vocab(_pChatModel.get());

    inference::Completion completion;
    const std::vector<std::int32_t> tokens = TokenizePrompt(prompt);
    completion.promptTokens = static_cast<int>(tokens.size());
    if (completion.promptTokens + spec.maxTokens > ContextSize()) {
        throw InferenceError(std::format("prompt ({} tokens) plus max_tokens ({}) exceeds the context size ({})",
                                         completion.promptTokens, spec.maxTokens, ContextSize()));
    }

    // Reuse the longest common prefix of the KV cache. The last prompt token is always evaluated, because
    // sampling needs its logits.
    std::size_t common = 0;
    while (common < _cachedTokens.size() && common < tokens.size() && _cachedTokens[common] == tokens[common]) {
        ++common;
    }
    if (common == tokens.size()) {
        --common;
    }
    llama_memory_t memory = llama_get_memory(_pChatContext.get());
    if (!llama_memory_seq_rm(memory, 0, static_cast<llama_pos>(common), -1)) {
        // Some model types cannot drop part of a sequence; start from an empty cache instead.
        llama_memory_clear(memory, true);
        common = 0;
    }
    _cachedTokens.resize(common);
    completion.tokensReused = static_cast<int>(common);

    // Evaluate the rest of the prompt in n_batch chunks; only the final chunk needs logits.
    const std::size_t batchSize = llama_n_batch(_pChatContext.get());
    for (std::size_t position = common; position < tokens.size(); position += batchSize) {
        if (stop.stop_requested()) {
            completion.cancelled = true;
            completion.tokensEvaluated = static_cast<int>(_cachedTokens.size() - common);
            return completion;
        }
        const std::size_t count = std::min(batchSize, tokens.size() - position);
        DecodeChat(std::span(tokens).subspan(position, count), position + count == tokens.size());
    }
    completion.tokensEvaluated = static_cast<int>(_cachedTokens.size() - common);
    const Clock::time_point generateStart = Clock::now();
    completion.promptMs = std::chrono::duration<double, std::milli>(generateStart - promptStart).count();

    // Sampler chain: the grammar first (it masks impossible tokens), then the sampling settings.
    std::unique_ptr<llama_sampler, SamplerDeleter> pSampler(llama_sampler_chain_init(llama_sampler_chain_default_params()));
    if (!spec.grammar.empty()) {
        llama_sampler* pGrammar = llama_sampler_init_grammar(pVocab, spec.grammar.c_str(), "root");
        MORPHEUS_REQUIRE(pGrammar != nullptr, "Complete: the GBNF grammar does not parse");
        llama_sampler_chain_add(pSampler.get(), pGrammar);
    }
    if (spec.sampling.temperature <= 0.0f) {
        llama_sampler_chain_add(pSampler.get(), llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(pSampler.get(), llama_sampler_init_top_k(spec.sampling.topK));
        llama_sampler_chain_add(pSampler.get(), llama_sampler_init_top_p(spec.sampling.topP, 1));
        llama_sampler_chain_add(pSampler.get(), llama_sampler_init_temp(spec.sampling.temperature));
        llama_sampler_chain_add(pSampler.get(), llama_sampler_init_dist(spec.sampling.seed));
    }

    // Length of the longest prefix of `text` that does not end inside a UTF-8 sequence, so streamed pieces never
    // split a character.
    auto completeUtf8Prefix = [](std::string_view text) -> std::size_t {
        for (std::size_t back = 1; back <= 4 && back <= text.size(); ++back) {
            const auto byte = static_cast<unsigned char>(text[text.size() - back]);
            if ((byte & 0xC0) == 0x80) {
                continue; // Continuation byte: keep looking for the lead byte.
            }
            const std::size_t length = byte < 0x80 ? 1 : (byte >> 5) == 0x6 ? 2 : (byte >> 4) == 0xE ? 3 : (byte >> 3) == 0x1E ? 4 : 1;
            return length <= back ? text.size() : text.size() - back;
        }
        return text.size(); // Only continuation bytes: malformed, emit as is.
    };

    // Generate. Every accepted token is decoded right away, so the cache always matches _cachedTokens.
    std::string pending; // Generated bytes not yet streamed (an incomplete UTF-8 character).
    for (int i = 0; i < spec.maxTokens; ++i) {
        if (stop.stop_requested()) {
            completion.cancelled = true;
            break;
        }
        const llama_token token = llama_sampler_sample(pSampler.get(), _pChatContext.get(), -1);
        if (llama_vocab_is_eog(pVocab, token)) {
            break;
        }

        char piece[256];
        const int32_t length = llama_token_to_piece(pVocab, token, piece, static_cast<int32_t>(sizeof(piece)), 0, false);
        MORPHEUS_REQUIRE(length >= 0, "Complete: a token piece is longer than 256 bytes");
        completion.text.append(piece, static_cast<std::size_t>(length));
        pending.append(piece, static_cast<std::size_t>(length));
        const std::size_t ready = completeUtf8Prefix(pending);
        if (ready > 0 && onToken) {
            onToken(std::string_view(pending).substr(0, ready));
        }
        pending.erase(0, ready);

        ++completion.tokensGenerated;
        const std::int32_t generated = token;
        DecodeChat(std::span(&generated, 1), true);
        if (i + 1 == spec.maxTokens) {
            completion.stoppedAtLimit = true;
        }
    }
    if (!pending.empty() && onToken) {
        onToken(pending);
    }
    completion.generateMs = std::chrono::duration<double, std::milli>(Clock::now() - generateStart).count();
    return completion;
}

std::vector<std::vector<float>> LlamaCppBackend::Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose)
{
    const llama_vocab* pVocab = llama_model_get_vocab(_pEmbedModel.get());
    const std::string& prefix = purpose == inference::EmbedPurpose::Query ? _config.embeddingQueryPrefix
                                                                          : _config.embeddingDocumentPrefix;
    const auto dimensions = static_cast<std::size_t>(llama_model_n_embd(_pEmbedModel.get()));
    const bool encoderOnly = llama_model_has_encoder(_pEmbedModel.get()) && !llama_model_has_decoder(_pEmbedModel.get());

    std::vector<std::vector<float>> vectors;
    vectors.reserve(texts.size());
    for (const std::string& text : texts) {
        const std::vector<std::int32_t> tokens = Tokenize(pVocab, prefix + text, true, false);
        if (tokens.empty() || tokens.size() > static_cast<std::size_t>(_config.embeddingContextSize)) {
            throw InferenceError(std::format("a text to embed has {} tokens; the embedding context holds {}",
                                             tokens.size(), _config.embeddingContextSize));
        }

        // Each text is embedded alone, from an empty cache (encoder-only models have no cache at all).
        if (llama_memory_t memory = llama_get_memory(_pEmbedContext.get()); memory != nullptr) {
            llama_memory_clear(memory, true);
        }
        BatchGuard guard(tokens.size());
        FillBatch(guard.batch, tokens, 0, Outputs::All);
        const int32_t result = encoderOnly ? llama_encode(_pEmbedContext.get(), guard.batch)
                                           : llama_decode(_pEmbedContext.get(), guard.batch);
        if (result != 0) {
            throw InferenceError(std::format("embedding failed with code {}", result));
        }

        const float* pEmbedding = llama_get_embeddings_seq(_pEmbedContext.get(), 0);
        if (pEmbedding == nullptr) {
            throw InferenceError("the embedding model returned no pooled embedding");
        }
        std::vector<float> vector(pEmbedding, pEmbedding + dimensions);

        // L2-normalize, so cosine similarity is a dot product.
        double norm = 0.0;
        for (const float value : vector) {
            norm += static_cast<double>(value) * value;
        }
        norm = std::sqrt(norm);
        if (norm == 0.0) {
            throw InferenceError("the embedding model returned a zero vector");
        }
        for (float& value : vector) {
            value = static_cast<float>(value / norm);
        }
        vectors.push_back(std::move(vector));
    }
    return vectors;
}

std::string LlamaCppBackend::EmbeddingModelId() const
{
    return _config.embeddingModel.stem().string();
}

std::vector<std::uint8_t> LlamaCppBackend::SaveState()
{
    // Layout: [token count: uint64][tokens: int32 each][llama.cpp sequence state].
    const std::size_t stateSize = llama_state_seq_get_size(_pChatContext.get(), 0);
    const std::uint64_t count = _cachedTokens.size();
    const std::size_t tokenBytes = _cachedTokens.size() * sizeof(std::int32_t);
    std::vector<std::uint8_t> blob(sizeof(count) + tokenBytes + stateSize);
    std::memcpy(blob.data(), &count, sizeof(count));
    if (tokenBytes > 0) {
        std::memcpy(blob.data() + sizeof(count), _cachedTokens.data(), tokenBytes);
    }
    const std::size_t written = llama_state_seq_get_data(_pChatContext.get(), blob.data() + sizeof(count) + tokenBytes, stateSize, 0);
    if (written != stateSize) {
        throw InferenceError("saving the KV cache state failed");
    }
    return blob;
}

void LlamaCppBackend::RestoreState(std::span<const std::uint8_t> state)
{
    // The blob comes from SaveState, so a malformed one is an engine bug.
    std::uint64_t count = 0;
    MORPHEUS_REQUIRE(state.size() >= sizeof(count), "RestoreState: state blob too small");
    std::memcpy(&count, state.data(), sizeof(count));
    const std::size_t tokenBytes = static_cast<std::size_t>(count) * sizeof(std::int32_t);
    MORPHEUS_REQUIRE(state.size() >= sizeof(count) + tokenBytes, "RestoreState: state blob truncated");
    std::vector<std::int32_t> tokens(static_cast<std::size_t>(count));
    if (tokenBytes > 0) {
        std::memcpy(tokens.data(), state.data() + sizeof(count), tokenBytes);
    }

    llama_memory_clear(llama_get_memory(_pChatContext.get()), true);
    _cachedTokens.clear();
    const std::size_t kvSize = state.size() - sizeof(count) - tokenBytes;
    if (llama_state_seq_set_data(_pChatContext.get(), state.data() + sizeof(count) + tokenBytes, kvSize, 0) == 0) {
        throw InferenceError("restoring the KV cache state failed");
    }
    _cachedTokens = std::move(tokens);
}

std::string LlamaCppBackend::RenderPrompt(const inference::Prompt& prompt) const
{
    MORPHEUS_REQUIRE(!prompt.messages.empty(), "RenderPrompt: the prompt has no messages");
    const char* pTemplate = llama_model_chat_template(_pChatModel.get(), nullptr);
    if (pTemplate == nullptr) {
        throw InferenceError("the chat model has no built-in chat template");
    }

    // llama_chat_message holds raw pointers: keep the role strings alive (reserve: no reallocation).
    std::vector<std::string> roles;
    roles.reserve(prompt.messages.size());
    std::vector<llama_chat_message> chat;
    chat.reserve(prompt.messages.size());
    std::size_t contentBytes = 0;
    for (const inference::Message& message : prompt.messages) {
        roles.push_back(inference::RoleName(message.role));
        chat.push_back({roles.back().c_str(), message.content.c_str()});
        contentBytes += message.content.size();
    }

    // add_ass: the rendered prompt ends where the assistant's reply begins. Retry once if the buffer was short.
    std::vector<char> buffer(2 * contentBytes + 1024);
    int32_t length = llama_chat_apply_template(pTemplate, chat.data(), chat.size(), true, buffer.data(),
                                               static_cast<int32_t>(buffer.size()));
    if (length < 0) {
        throw InferenceError("llama_chat_apply_template does not support this model's chat template");
    }
    if (static_cast<std::size_t>(length) > buffer.size()) {
        buffer.resize(static_cast<std::size_t>(length));
        length = llama_chat_apply_template(pTemplate, chat.data(), chat.size(), true, buffer.data(),
                                           static_cast<int32_t>(buffer.size()));
    }
    return std::string(buffer.data(), static_cast<std::size_t>(length));
}

std::vector<std::int32_t> LlamaCppBackend::TokenizePrompt(const inference::Prompt& prompt) const
{
    const llama_vocab* pVocab = llama_model_get_vocab(_pChatModel.get());
    // Special tokens written by the template are real control tokens (parse_special). BOS is added when the
    // model expects one; if the template already wrote it, the duplicate is dropped.
    const bool addBos = llama_vocab_get_add_bos(pVocab);
    std::vector<std::int32_t> tokens = Tokenize(pVocab, RenderPrompt(prompt), addBos, true);
    if (addBos && tokens.size() >= 2 && tokens[0] == llama_vocab_bos(pVocab) && tokens[1] == tokens[0]) {
        tokens.erase(tokens.begin());
    }
    MORPHEUS_REQUIRE(!tokens.empty(), "TokenizePrompt: the rendered prompt has no tokens");
    return tokens;
}

void LlamaCppBackend::DecodeChat(std::span<const std::int32_t> tokens, bool wantLogits)
{
    BatchGuard guard(tokens.size());
    FillBatch(guard.batch, tokens, _cachedTokens.size(), wantLogits ? Outputs::Last : Outputs::None);
    const int32_t result = llama_decode(_pChatContext.get(), guard.batch);
    if (result != 0) {
        // A failed decode can leave part of the batch in the cache: start from an empty cache next time.
        llama_memory_clear(llama_get_memory(_pChatContext.get()), true);
        _cachedTokens.clear();
        throw InferenceError(std::format("llama_decode failed with code {}", result));
    }
    _cachedTokens.insert(_cachedTokens.end(), tokens.begin(), tokens.end());
}

std::vector<std::int32_t> LlamaCppBackend::Tokenize(const llama_vocab* pVocab, std::string_view text, bool addSpecial,
                                                    bool parseSpecial)
{
    MORPHEUS_REQUIRE(text.size() <= static_cast<std::size_t>(std::numeric_limits<int32_t>::max()),
                     "Tokenize: text is too long");
    const auto length = static_cast<int32_t>(text.size());
    // The first call measures: it returns minus the number of tokens needed.
    const int32_t needed = llama_tokenize(pVocab, text.data(), length, nullptr, 0, addSpecial, parseSpecial);
    if (needed == std::numeric_limits<int32_t>::min()) {
        throw InferenceError("tokenization overflowed");
    }
    std::vector<std::int32_t> tokens(static_cast<std::size_t>(needed < 0 ? -needed : needed));
    const int32_t written = llama_tokenize(pVocab, text.data(), length, tokens.data(), static_cast<int32_t>(tokens.size()),
                                           addSpecial, parseSpecial);
    if (written < 0) {
        throw InferenceError("tokenization failed");
    }
    tokens.resize(static_cast<std::size_t>(written));
    return tokens;
}

} // namespace morpheus::llama
```

- [ ] **Step 6: Wire up CMake**

`src/llama/CMakeLists.txt`:

```cmake
add_library(morpheus_llama STATIC
    LlamaCppBackend.cpp
)
target_include_directories(morpheus_llama PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_llama
    PUBLIC morpheus_inference
    PRIVATE llama
)
morpheus_target_defaults(morpheus_llama)
```

In the root `CMakeLists.txt`, after `add_subdirectory(src/inference)`, add:

```cmake
if(MORPHEUS_BUILD_LLAMA)
    add_subdirectory(src/llama)
endif()
```

In `tests/CMakeLists.txt`, after `target_link_libraries(morpheus_tests ...)`, add:

```cmake
if(MORPHEUS_BUILD_LLAMA)
    target_link_libraries(morpheus_tests PRIVATE morpheus_llama)
endif()
```

- [ ] **Step 7: Build and run. Expect a pass, or a skip without models.**

Run: `cmake --build --preset dev && ./build/dev/bin/Debug/morpheus_tests.exe "[llama]"`

Expected:
- **Without the env vars:** every `[llama]` test is reported as skipped, and the build has no warnings.
- **With the env vars set** (see "Model setup"): all `[llama]` tests pass.
- If the models are not present on this machine, say so in the report; that is not a failure.

Then run `ctest --preset tests`. Expected: all pass (no llama.cpp in that preset).

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt src/llama tests
git commit -m "Add the in-process llama.cpp backend" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Embeddings and KV-cache save/restore tests

**Files:**
- Create: `tests/llama/LlamaEmbedStateTests.cpp`

**Interfaces:**
- Consumes: `LlamaCppBackend::Embed`, `EmbeddingModelId`, `SaveState`, `RestoreState` and `SharedBackend()` (Task 5).
- Produces: model-backed tests only. If they expose a bug, fix it in `src/llama/LlamaCppBackend.cpp`.

- [ ] **Step 1: Write the tests**

```cpp
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numeric>
#include <string>
#include <vector>

#include "llama/LlamaTestModels.h"

using namespace morpheus::inference;
using morpheus::test::SharedBackend;

namespace {

float Dot(const std::vector<float>& a, const std::vector<float>& b)
{
    return std::inner_product(a.begin(), a.end(), b.begin(), 0.0f);
}

Prompt SimplePrompt(const std::string& question)
{
    Prompt prompt;
    prompt.messages = {{Role::System, "You are a terse assistant."}, {Role::User, question}};
    prompt.stableCount = 1;
    return prompt;
}

} // namespace

TEST_CASE("Embeddings are normalized and rank related texts higher", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    const std::vector<std::vector<float>> vectors = backend.Embed(
        {"a tracked crawler crossing the desert", "a vehicle driving over sand dunes", "a recipe for tomato soup"},
        EmbedPurpose::Document);

    REQUIRE(vectors.size() == 3);
    REQUIRE_FALSE(vectors[0].empty());
    for (const std::vector<float>& vector : vectors) {
        CHECK(vector.size() == vectors[0].size());
        CHECK(Dot(vector, vector) == Catch::Approx(1.0f).margin(1e-4));
    }
    CHECK(Dot(vectors[0], vectors[1]) > Dot(vectors[0], vectors[2]));
    CHECK_FALSE(backend.EmbeddingModelId().empty());
}

TEST_CASE("Query and document prefixes give different embeddings", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    const std::vector<float> query = backend.Embed({"water supplies"}, EmbedPurpose::Query).at(0);
    const std::vector<float> document = backend.Embed({"water supplies"}, EmbedPurpose::Document).at(0);
    CHECK(Dot(query, document) < 0.9999f);
}

TEST_CASE("A saved KV cache restores prefix reuse", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    GenerationSpec spec;
    spec.maxTokens = 4;

    const Prompt original = SimplePrompt("Name a desert animal.");
    backend.Complete(original, spec, {}, {});
    const std::vector<std::uint8_t> saved = backend.SaveState();

    // A different prompt overwrites the cache...
    backend.Complete(SimplePrompt("Count to three."), spec, {}, {});

    // ...and restoring brings the original prefix back.
    backend.RestoreState(saved);
    const Completion again = backend.Complete(original, spec, {}, {});
    CHECK(again.tokensEvaluated == 1);
    CHECK(again.tokensReused == again.promptTokens - 1);
}
```

- [ ] **Step 2: Build and run**

Run: `cmake --build --preset dev && ./build/dev/bin/Debug/morpheus_tests.exe "[llama]"`

Expected:
- **With the models:** all `[llama]` tests pass.
- **Without them:** all are skipped.
- **On a failure:** fix the backend, and record the root cause in the report.

- [ ] **Step 3: Commit**

```bash
git add tests/llama src/llama
git commit -m "Test embeddings and KV-cache save/restore" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Context rendering: state view, mechanics line, transcript

**Files:**
- Create: `src/context/CMakeLists.txt`, `src/context/StateView.h`, `src/context/StateView.cpp`, `src/context/Transcript.h`, `src/context/Transcript.cpp`, `tests/context/RenderTests.cpp`
- Modify: root `CMakeLists.txt` (`add_subdirectory(src/context)` after `src/inference`), `tests/CMakeLists.txt` (link `morpheus_context`)

**Interfaces:**
- Consumes: `core::TurnRecord`, `core::CheckOutcome`, `core::AppliedChange`, `core::ChangeOp` (Task 2); `inference::Message` and `inference::Role` (Task 4).
- Produces (namespace `morpheus::context`):
  - `std::string RenderStateView(const nlohmann::json& state)`
  - `std::string RenderMechanicsLine(const core::TurnRecord& record)`
  - `std::vector<inference::Message> RenderTranscript(const std::vector<core::TurnRecord>& turns)`

- [ ] **Step 1: Write the failing test `tests/context/RenderTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "context/StateView.h"
#include "context/Transcript.h"

using namespace morpheus;

namespace {

core::TurnRecord TurnWithMechanics()
{
    core::TurnRecord record;
    record.number = 3;
    record.input = "I climb the hull.";
    record.narration = "You slip and hit the deck.";
    core::CheckOutcome check;
    check.stat = "agility";
    check.difficulty = "hard";
    check.result.total = 14;
    check.result.target = 16;
    check.result.success = false;
    record.checks = {check};
    record.applied = {
        {core::Change{core::ChangeOp::Set, "player.location", "deck", ""}, ""},
        {core::Change{core::ChangeOp::Add, "player.health", -3, ""}, "clamped to 0"},
        {core::Change{core::ChangeOp::Add, "player.conditions", "bruised", ""}, ""},
        {core::Change{core::ChangeOp::Remove, "player.inventory.rope", nullptr, ""}, ""},
        {core::Change{core::ChangeOp::Create, "npc:marta", nlohmann::json{{"type", "npc"}}, ""}, ""},
    };
    record.rejected = {{nlohmann::json{{"op", "explode"}}, "unknown op"}};
    return record;
}

} // namespace

TEST_CASE("RenderStateView prints one compact line per entity", "[context]")
{
    const nlohmann::json state = nlohmann::json::parse(R"({
        "player": {"type": "captain", "fields": {"health": 10, "location": "open desert", "conditions": [],
                                                  "inventory": {"pistol": 1, "compass": 1}}},
        "crawler": {"type": "crawler", "fields": {"crew": 20, "vehicles": {"sand_bike": 2}, "morale": "steady"}}
    })");
    CHECK(context::RenderStateView(state) ==
          "crawler (crawler): crew 20, morale \"steady\", vehicles {sand_bike 2}\n"
          "player (captain): conditions [], health 10, inventory {compass 1, pistol 1}, location \"open desert\"\n");
}

TEST_CASE("RenderMechanicsLine summarizes checks and applied changes", "[context]")
{
    CHECK(context::RenderMechanicsLine(TurnWithMechanics()) ==
          "[agility vs hard: 14/16 failure; player.location = \"deck\"; player.health -3 (clamped to 0); "
          "player.conditions +bruised; player.inventory.rope removed; npc:marta created]");

    core::TurnRecord quiet;
    quiet.input = "I wait.";
    CHECK(context::RenderMechanicsLine(quiet).empty());
}

TEST_CASE("RenderTranscript turns each turn into a user and an assistant message", "[context]")
{
    core::TurnRecord quiet;
    quiet.input = "I wait.";
    quiet.narration = "Nothing happens.";

    const std::vector<inference::Message> messages = context::RenderTranscript({TurnWithMechanics(), quiet});
    REQUIRE(messages.size() == 4);
    CHECK(messages[0].role == inference::Role::User);
    CHECK(messages[0].content == "I climb the hull.\n" + context::RenderMechanicsLine(TurnWithMechanics()));
    CHECK(messages[1].role == inference::Role::Assistant);
    CHECK(messages[1].content == "You slip and hit the deck.");
    CHECK(messages[2].content == "I wait."); // No mechanics: no extra line.
    CHECK(messages[3].content == "Nothing happens.");
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'context/StateView.h'`.

- [ ] **Step 3: Create the headers**

`src/context/StateView.h`:

```cpp
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace morpheus::context {

// The state view for the volatile tail (T1): one line per entity, e.g.
//   player (captain): conditions [], health 10, inventory {compass 1, pistol 1}, location "open desert"
// Entities and fields appear in name order (state JSON objects are sorted), so the text is deterministic.
// Numbers are bare, strings and enum values are quoted, list items are bare, counters print "item count".
// Throws ContractError for a state that is not shaped like validated state.
std::string RenderStateView(const nlohmann::json& state);

} // namespace morpheus::context
```

`src/context/Transcript.h`:

```cpp
#pragma once

#include <string>
#include <vector>

#include "core/TurnRecord.h"
#include "inference/Llm.h"

namespace morpheus::context {

// One line recording what the engine did in a turn, for the transcript (L3):
//   [agility vs hard: 14/16 failure; player.location = "deck"; player.health -3 (clamped to 0); npc:marta created]
// set: "path = value"; add: "path +N" / "path -N" / "path +item"; remove: "path -item" or "path removed";
// create: "id created"; an engine note follows in parentheses. Rejected changes are left out.
// Returns "" when the turn had no checks and no applied changes.
std::string RenderMechanicsLine(const core::TurnRecord& record);

// L3: each turn becomes a user message (the input, then the mechanics line on its own line when there is one)
// and an assistant message (the narration).
std::vector<inference::Message> RenderTranscript(const std::vector<core::TurnRecord>& turns);

} // namespace morpheus::context
```

- [ ] **Step 4: Create the implementations and CMake**

`src/context/StateView.cpp`:

```cpp
#include "context/StateView.h"

#include <format>

#include "core/Errors.h"

namespace morpheus::context {

std::string RenderStateView(const nlohmann::json& state)
{
    MORPHEUS_REQUIRE(state.is_object(), "RenderStateView: state must be an object");

    std::string text;
    for (auto entity = state.begin(); entity != state.end(); ++entity) {
        const nlohmann::json& body = entity.value();
        MORPHEUS_REQUIRE(body.is_object() && body.contains("type") && body.contains("fields"),
                         std::format("RenderStateView: entity '{}' has no type or fields", entity.key()));
        text += std::format("{} ({}):", entity.key(), body.at("type").get<std::string>());

        const nlohmann::json& fields = body.at("fields");
        bool firstField = true;
        for (auto field = fields.begin(); field != fields.end(); ++field) {
            text += firstField ? " " : ", ";
            firstField = false;
            text += field.key();
            text += ' ';

            const nlohmann::json& value = field.value();
            if (value.is_array()) {
                // List: bare items.
                text += '[';
                for (std::size_t i = 0; i < value.size(); ++i) {
                    text += (i == 0 ? "" : ", ") + value[i].get<std::string>();
                }
                text += ']';
            } else if (value.is_object()) {
                // Counter: "item count" pairs.
                text += '{';
                bool firstItem = true;
                for (auto item = value.begin(); item != value.end(); ++item) {
                    text += std::format("{}{} {}", firstItem ? "" : ", ", item.key(), item.value().dump());
                    firstItem = false;
                }
                text += '}';
            } else {
                text += value.dump(); // Numbers bare, strings quoted and escaped.
            }
        }
        text += '\n';
    }
    return text;
}

} // namespace morpheus::context
```

`src/context/Transcript.cpp`:

```cpp
#include "context/Transcript.h"

#include <format>

namespace morpheus::context {

std::string RenderMechanicsLine(const core::TurnRecord& record)
{
    std::vector<std::string> parts;
    for (const core::CheckOutcome& check : record.checks) {
        parts.push_back(std::format("{} vs {}: {}/{} {}", check.stat, check.difficulty, check.result.total,
                                    check.result.target, check.result.success ? "success" : "failure"));
    }
    for (const core::AppliedChange& applied : record.applied) {
        const core::Change& change = applied.change;
        std::string part;
        switch (change.op) {
        case core::ChangeOp::Set:
            part = std::format("{} = {}", change.path, change.value.dump());
            break;
        case core::ChangeOp::Add:
            // Numeric add (int field or counter item) or a list item.
            part = change.value.is_number_integer() ? std::format("{} {:+}", change.path, change.value.get<long long>())
                                                    : std::format("{} +{}", change.path, change.value.get<std::string>());
            break;
        case core::ChangeOp::Remove:
            // A list item names what was removed; a counter item is removed whole.
            part = change.value.is_string() ? std::format("{} -{}", change.path, change.value.get<std::string>())
                                            : std::format("{} removed", change.path);
            break;
        case core::ChangeOp::Create:
            part = std::format("{} created", change.path);
            break;
        }
        if (!applied.note.empty()) {
            part += std::format(" ({})", applied.note);
        }
        parts.push_back(std::move(part));
    }

    if (parts.empty()) {
        return {};
    }
    std::string line = "[";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        line += (i == 0 ? "" : "; ") + parts[i];
    }
    return line + "]";
}

std::vector<inference::Message> RenderTranscript(const std::vector<core::TurnRecord>& turns)
{
    std::vector<inference::Message> messages;
    messages.reserve(turns.size() * 2);
    for (const core::TurnRecord& turn : turns) {
        const std::string mechanics = RenderMechanicsLine(turn);
        messages.push_back({inference::Role::User, mechanics.empty() ? turn.input : turn.input + "\n" + mechanics});
        messages.push_back({inference::Role::Assistant, turn.narration});
    }
    return messages;
}

} // namespace morpheus::context
```

`src/context/CMakeLists.txt` (PromptBuilder.cpp is added in Task 8):

```cmake
add_library(morpheus_context STATIC
    StateView.cpp
    Transcript.cpp
)
target_include_directories(morpheus_context PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_context PUBLIC morpheus_core morpheus_inference)
morpheus_target_defaults(morpheus_context)
```

Then:
- Add `add_subdirectory(src/context)` after `add_subdirectory(src/inference)` in the root `CMakeLists.txt`.
- Add `morpheus_context` to `morpheus_tests`'s `target_link_libraries` list.

- [ ] **Step 5: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass, including `[context]`.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/context tests
git commit -m "Render state view, mechanics lines and transcript" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Prompt assembly and budgets

**Files:**
- Create: `src/context/PromptBuilder.h`, `src/context/PromptBuilder.cpp`, `tests/context/PromptBuilderTests.cpp`
- Modify: `src/context/CMakeLists.txt` (add `PromptBuilder.cpp`)

**Interfaces:**
- Consumes:
  - `RenderStateView`, `RenderTranscript` (Task 7)
  - `core::World`, `core::LoreEntry`, `core::Summary`, `core::TurnRecord`, `core::BudgetSettings`
  - `inference::LlmBackend`
  - `morpheus::test::FakeBackend` (tests)
- Produces (namespace `morpheus::context`):
  - `struct ContextInputs { std::string engineRules; std::vector<core::Summary> summaries; std::vector<core::TurnRecord> transcript; nlohmann::json state; std::vector<std::string> retrieved; std::string playerInput; std::string instruction; }`
  - `struct LayerTokens { int engineRules, world, summary, transcript, tail, total; }`
  - `struct Assembly`, holding:
    - `inference::Prompt prompt`, `LayerTokens tokens`, `int transcriptBudget`
    - the flags `transcriptOverBudget`, `summaryOverBudget`, `tailOverBudget` and `contextOverflow`
    - `bool Fits() const`
  - `Assembly AssembleResolvePrompt(const core::World&, const ContextInputs&, const core::BudgetSettings&, inference::LlmBackend&)`
  - `void ExtendForNarrate(inference::Prompt&, const std::string& resolveOutput, const std::string& resolutionAndInstruction)`
- Prompt layout (spec, "Context layout and KV cache"):
  - **System message:** `engineRules + "\n\n# World\n" + rules`. If any lore is always-on, add `"# Lore\n- subject: data\n..."`. If any summaries exist, add `"# Story so far\nTurns a-b: text\n..."`.
  - **Transcript:** the messages from L3.
  - **Final user message:** `"# Current state\n" + view`. If there are retrieved snippets, add `"\n# Relevant\n- ...\n"`. Then `"\n# Player\n" + input + "\n\n# Task\n" + instruction`.

- [ ] **Step 1: Write the failing test `tests/context/PromptBuilderTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestPaths.h"
#include "context/PromptBuilder.h"
#include "core/Errors.h"
#include "core/World.h"
#include "fakes/FakeBackend.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

core::World MiniWorld()
{
    return core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
}

core::BudgetSettings RoomyBudget()
{
    return {64, 64, 512, 256, 512, 256};
}

context::ContextInputs Inputs(const core::World& world)
{
    context::ContextInputs inputs;
    inputs.engineRules = "ENGINE RULES";
    inputs.summaries = {{1, 4, "The keeper lit the lamp."}};
    core::TurnRecord turn;
    turn.number = 5;
    turn.input = "I knock.";
    turn.narration = "The door creaks.";
    inputs.transcript = {turn};
    inputs.state = world.initialState;
    inputs.retrieved = {"storm: Storms arrive at dusk."};
    inputs.playerInput = "I go inside.";
    inputs.instruction = "RESOLVE INSTRUCTION";
    return inputs;
}

} // namespace

TEST_CASE("AssembleResolvePrompt lays out the layers", "[context]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(8192);
    const context::Assembly assembly = context::AssembleResolvePrompt(world, Inputs(world), RoomyBudget(), backend);
    const inference::Prompt& prompt = assembly.prompt;

    REQUIRE(prompt.messages.size() == 4);
    CHECK(prompt.stableCount == 3);

    const std::string& system = prompt.messages[0].content;
    CHECK(prompt.messages[0].role == inference::Role::System);
    CHECK(system.starts_with("ENGINE RULES\n\n# World\n"));
    CHECK_THAT(system, ContainsSubstring("tiny test world"));
    CHECK_THAT(system, ContainsSubstring("# Lore\n- lighthouse: A tall lighthouse, painted red.\n"));
    CHECK_THAT(system, !ContainsSubstring("keeper:")); // Not always-on.
    CHECK_THAT(system, ContainsSubstring("# Story so far\nTurns 1-4: The keeper lit the lamp.\n"));

    CHECK(prompt.messages[1].content == "I knock.");
    CHECK(prompt.messages[2].content == "The door creaks.");

    const std::string& tail = prompt.messages[3].content;
    CHECK(prompt.messages[3].role == inference::Role::User);
    CHECK(tail.starts_with("# Current state\nplayer (character):"));
    CHECK_THAT(tail, ContainsSubstring("\n# Relevant\n- storm: Storms arrive at dusk.\n"));
    CHECK_THAT(tail, ContainsSubstring("\n# Player\nI go inside.\n\n# Task\nRESOLVE INSTRUCTION"));
}

TEST_CASE("AssembleResolvePrompt leaves out empty optional layers", "[context]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(8192);
    context::ContextInputs inputs = Inputs(world);
    inputs.summaries.clear();
    inputs.retrieved.clear();
    inputs.transcript.clear();

    const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend);
    REQUIRE(assembly.prompt.messages.size() == 2);
    CHECK(assembly.prompt.stableCount == 1);
    CHECK_THAT(assembly.prompt.messages[0].content, !ContainsSubstring("# Story so far"));
    CHECK_THAT(assembly.prompt.messages[1].content, !ContainsSubstring("# Relevant"));
}

TEST_CASE("AssembleResolvePrompt counts tokens per layer", "[context]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(8192);
    const context::ContextInputs inputs = Inputs(world);
    const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend);

    CHECK(assembly.tokens.engineRules == backend.CountTokens("ENGINE RULES"));
    CHECK(assembly.tokens.transcript == backend.CountTokens("I knock.") + backend.CountTokens("The door creaks."));
    CHECK(assembly.tokens.tail == backend.CountTokens(assembly.prompt.messages[3].content));
    CHECK(assembly.tokens.total == backend.CountPromptTokens(assembly.prompt));
    CHECK(assembly.Fits());
}

TEST_CASE("AssembleResolvePrompt reports budget overruns", "[context]")
{
    const core::World world = MiniWorld();

    SECTION("transcript over its budget")
    {
        test::FakeBackend tight(64 + 64 + 512 + 60); // Room for the fixed layers, almost none for L3.
        context::ContextInputs inputs = Inputs(world);
        inputs.transcript.front().narration = std::string(400, 'x');
        const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), tight);
        CHECK(assembly.transcriptOverBudget);
        CHECK_FALSE(assembly.Fits());
    }
    SECTION("tail over its allowance")
    {
        test::FakeBackend backend(8192);
        context::ContextInputs inputs = Inputs(world);
        inputs.playerInput = std::string(4000, 'y');
        CHECK(context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend).tailOverBudget);
    }
    SECTION("summary over its cap")
    {
        test::FakeBackend backend(8192);
        context::ContextInputs inputs = Inputs(world);
        inputs.summaries.front().text = std::string(2000, 's');
        CHECK(context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend).summaryOverBudget);
    }
    SECTION("world over its cap is a data error")
    {
        test::FakeBackend backend(8192);
        core::BudgetSettings budget = RoomyBudget();
        budget.worldCap = 2;
        CHECK_THROWS_WITH(context::AssembleResolvePrompt(world, Inputs(world), budget, backend), ContainsSubstring("world_cap"));
    }
    SECTION("engine rules over their cap are an engine bug")
    {
        test::FakeBackend backend(8192);
        core::BudgetSettings budget = RoomyBudget();
        budget.engineRulesCap = 1;
        CHECK_THROWS_AS(context::AssembleResolvePrompt(world, Inputs(world), budget, backend), core::ContractError);
    }
}

TEST_CASE("ExtendForNarrate only appends", "[context]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(8192);
    const context::Assembly assembly = context::AssembleResolvePrompt(world, Inputs(world), RoomyBudget(), backend);

    inference::Prompt narrate = assembly.prompt;
    context::ExtendForNarrate(narrate, "{\"intent\":\"enter\"}", "RESOLUTION + NARRATE INSTRUCTION");

    REQUIRE(narrate.messages.size() == assembly.prompt.messages.size() + 2);
    CHECK(narrate.stableCount == assembly.prompt.stableCount);
    for (std::size_t i = 0; i < assembly.prompt.messages.size(); ++i) {
        CHECK(narrate.messages[i].content == assembly.prompt.messages[i].content);
    }
    CHECK(narrate.messages[4].role == inference::Role::Assistant);
    CHECK(narrate.messages[4].content == "{\"intent\":\"enter\"}");
    CHECK(narrate.messages[5].role == inference::Role::User);
    CHECK(narrate.messages[5].content == "RESOLUTION + NARRATE INSTRUCTION");
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'context/PromptBuilder.h'`.

- [ ] **Step 3: Create `src/context/PromptBuilder.h`**

```cpp
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Settings.h"
#include "core/TurnRecord.h"
#include "core/World.h"
#include "inference/Llm.h"

namespace morpheus::context {

// What goes into one resolve prompt, by layer (architecture spec, "Context layout and KV cache"). The world
// (L1) is passed separately.
struct ContextInputs {
    std::string engineRules = {};                  // L0: engine rules and the output format.
    std::vector<core::Summary> summaries = {};     // L2: folded turns, oldest first.
    std::vector<core::TurnRecord> transcript = {}; // L3: unfolded turns, oldest first.
    nlohmann::json state = nullptr;                // T1: current state (validated).
    std::vector<std::string> retrieved = {};       // T2: formatted lore/history snippets.
    std::string playerInput = {};                  // T3
    std::string instruction = {};                  // T4: the resolve task instruction.
};

// Exact token counts. Layer counts are raw text (no template tokens); `total` is the whole templated prompt.
struct LayerTokens {
    int engineRules = 0;
    int world = 0;
    int summary = 0;
    int transcript = 0;
    int tail = 0;
    int total = 0;
};

struct Assembly {
    inference::Prompt prompt = {};
    LayerTokens tokens = {};
    int transcriptBudget = 0;          // Tokens L3 may use under this budget.
    bool transcriptOverBudget = false; // Fold old turns into the summary and assemble again.
    bool summaryOverBudget = false;    // The summary itself must be condensed.
    bool tailOverBudget = false;       // State, retrieval, input and instruction exceed tail_allowance.
    bool contextOverflow = false;      // The templated prompt plus both generations exceeds the context.

    bool Fits() const
    {
        return !transcriptOverBudget && !summaryOverBudget && !tailOverBudget && !contextOverflow;
    }
};

// Builds the resolve prompt: one system message (L0 + L1 + L2), the transcript (L3), then one user message with
// the volatile tail (T1-T4). prompt.stableCount marks the end of L3. Throws ContractError if L0 exceeds
// engine_rules_cap (engine text too long) and DataError if L1 exceeds world_cap (the world is too big). Other
// overruns are flags for the turn layer to act on.
Assembly AssembleResolvePrompt(const core::World& world, const ContextInputs& inputs,
                               const core::BudgetSettings& budget, inference::LlmBackend& backend);

// Turns a resolve prompt into the narrate prompt by appending the resolve output (assistant) and the resolution
// record with the narrate instruction (user). It only appends, so the whole resolve prompt stays cached.
void ExtendForNarrate(inference::Prompt& prompt, const std::string& resolveOutput,
                      const std::string& resolutionAndInstruction);

} // namespace morpheus::context
```

- [ ] **Step 4: Create `src/context/PromptBuilder.cpp`, and add it to `src/context/CMakeLists.txt`**

```cpp
#include "context/PromptBuilder.h"

#include <algorithm>
#include <format>

#include "context/StateView.h"
#include "context/Transcript.h"
#include "core/Errors.h"

namespace morpheus::context {

Assembly AssembleResolvePrompt(const core::World& world, const ContextInputs& inputs,
                               const core::BudgetSettings& budget, inference::LlmBackend& backend)
{
    MORPHEUS_REQUIRE(!inputs.engineRules.empty() && !inputs.playerInput.empty() && !inputs.instruction.empty(),
                     "AssembleResolvePrompt: engine rules, player input and instruction are required");

    // L1: the world's rules, then its always-on lore.
    std::string worldText = "# World\n" + world.rules;
    if (!worldText.ends_with('\n')) {
        worldText += '\n';
    }
    std::string lore;
    for (const core::LoreEntry& entry : world.lore) {
        if (entry.alwaysOn) {
            lore += std::format("- {}: {}\n", entry.subject, entry.data);
        }
    }
    if (!lore.empty()) {
        worldText += "# Lore\n" + lore;
    }

    // L2: folded summaries, oldest first.
    std::string summaryText;
    if (!inputs.summaries.empty()) {
        summaryText = "# Story so far\n";
        for (const core::Summary& summary : inputs.summaries) {
            summaryText += std::format("Turns {}-{}: {}\n", summary.fromTurn, summary.toTurn, summary.text);
        }
    }

    // T1-T4: rebuilt every call, so it goes last.
    std::string tail = "# Current state\n" + RenderStateView(inputs.state);
    if (!inputs.retrieved.empty()) {
        tail += "\n# Relevant\n";
        for (const std::string& snippet : inputs.retrieved) {
            tail += "- " + snippet + "\n";
        }
    }
    tail += "\n# Player\n" + inputs.playerInput + "\n\n# Task\n" + inputs.instruction;

    // Messages: stable prefix (system, transcript), then the tail.
    Assembly assembly;
    inference::Prompt& prompt = assembly.prompt;
    prompt.messages.push_back({inference::Role::System, inputs.engineRules + "\n\n" + worldText + summaryText});
    for (inference::Message& message : RenderTranscript(inputs.transcript)) {
        prompt.messages.push_back(std::move(message));
    }
    prompt.stableCount = prompt.messages.size();
    prompt.messages.push_back({inference::Role::User, tail});

    // Exact counts with the model's tokenizer.
    LayerTokens& tokens = assembly.tokens;
    tokens.engineRules = backend.CountTokens(inputs.engineRules);
    tokens.world = backend.CountTokens(worldText);
    tokens.summary = backend.CountTokens(summaryText);
    for (std::size_t i = 1; i < prompt.stableCount; ++i) {
        tokens.transcript += backend.CountTokens(prompt.messages[i].content);
    }
    tokens.tail = backend.CountTokens(tail);
    tokens.total = backend.CountPromptTokens(prompt);

    // Fixed layers: L0 is engine text (a bug if too long), L1 is world data (the author must trim it).
    MORPHEUS_REQUIRE(tokens.engineRules <= budget.engineRulesCap,
                     std::format("engine rules use {} tokens, over engine_rules_cap {}", tokens.engineRules,
                                 budget.engineRulesCap));
    if (tokens.world > budget.worldCap) {
        throw core::DataError(std::format("world '{}': rules and always-on lore use {} tokens, over world_cap {}",
                                          world.id, tokens.world, budget.worldCap));
    }
    assembly.summaryOverBudget = tokens.summary > budget.summaryCap;
    assembly.tailOverBudget = tokens.tail > budget.tailAllowance;

    // L3 gets what the fixed layers and the reserved generations leave.
    assembly.transcriptBudget = backend.ContextSize() - budget.resolveMaxTokens - budget.narrateMaxTokens -
                                budget.tailAllowance - tokens.engineRules - tokens.world -
                                std::min(tokens.summary, budget.summaryCap);
    assembly.transcriptOverBudget = tokens.transcript > assembly.transcriptBudget;

    // Template tokens are not in the layer counts; this catches them overflowing the context.
    assembly.contextOverflow =
        tokens.total + budget.resolveMaxTokens + budget.narrateMaxTokens > backend.ContextSize();
    return assembly;
}

void ExtendForNarrate(inference::Prompt& prompt, const std::string& resolveOutput,
                      const std::string& resolutionAndInstruction)
{
    MORPHEUS_REQUIRE(!prompt.messages.empty() && prompt.messages.back().role == inference::Role::User,
                     "ExtendForNarrate: expects a resolve prompt ending with the user tail");
    prompt.messages.push_back({inference::Role::Assistant, resolveOutput});
    prompt.messages.push_back({inference::Role::User, resolutionAndInstruction});
}

} // namespace morpheus::context
```

Add `PromptBuilder.cpp` to the `add_library(morpheus_context ...)` source list.

- [ ] **Step 5: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass. If the mini world's rules or lore make a budget section's numbers land differently, adjust only that section's sizes so that it exercises the intended branch, and say so in the report.

- [ ] **Step 6: Commit**

```bash
git add src/context tests/context
git commit -m "Assemble layered prompts with exact token budgets" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Probe tool, docs, milestone verification

**Files:**
- Create: `tools/probe/CMakeLists.txt`, `tools/probe/main.cpp`
- Modify: root `CMakeLists.txt` (`add_subdirectory(tools/probe)` inside `if(MORPHEUS_BUILD_LLAMA)`), `CLAUDE.md`, `README.md`, `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`

**Interfaces:**
- Consumes: `LoadSettings`, `LoadWorld`, `LlamaCppBackend`, `AssembleResolvePrompt`.
- Produces: the `morpheus_probe` executable (dev preset only).

- [ ] **Step 1: Create `tools/probe/main.cpp`**

```cpp
// morpheus_probe: loads settings.json and a world, assembles a resolve-style prompt, and streams two
// completions, printing token usage and KV-cache reuse. A manual check that the model, the Vulkan backend and
// prefix caching work on this machine.
//
// Usage: morpheus_probe <settings.json> <world folder> "<player input>"

#include <exception>
#include <format>
#include <iostream>
#include <string_view>

#include "context/PromptBuilder.h"
#include "core/Settings.h"
#include "core/World.h"
#include "llama/LlamaCppBackend.h"

using namespace morpheus;

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::cerr << "usage: morpheus_probe <settings.json> <world folder> \"<player input>\"\n";
        return 2;
    }
    try {
        const core::Settings settings = core::LoadSettings(argv[1]);
        const core::World world = core::LoadWorld(argv[2]);

        llama::LlamaConfig config;
        config.chatModel = settings.chatModel;
        config.embeddingModel = settings.embeddingModel;
        config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
        config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
        config.contextSize = settings.contextSize;
        config.gpuLayers = settings.gpuLayers;
        llama::LlamaCppBackend backend(config);

        // A plain-prose stand-in for the resolve call: the real engine rules and grammar arrive in milestone 3.
        context::ContextInputs inputs;
        inputs.engineRules = "You are the game master of a solo text RPG. Reply in plain prose.";
        inputs.state = world.initialState;
        inputs.playerInput = argv[3];
        inputs.instruction = "Narrate what happens next in two or three sentences.";
        const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, settings.budget, backend);
        std::cout << std::format("prompt: {} tokens (engine rules {}, world {}, tail {}); fits: {}\n",
                                 assembly.tokens.total, assembly.tokens.engineRules, assembly.tokens.world,
                                 assembly.tokens.tail, assembly.Fits() ? "yes" : "no");

        inference::GenerationSpec spec;
        spec.maxTokens = settings.budget.narrateMaxTokens;
        spec.sampling.temperature = static_cast<float>(settings.narrateSampling.temperature);
        spec.sampling.topK = settings.narrateSampling.topK;
        spec.sampling.topP = static_cast<float>(settings.narrateSampling.topP);
        spec.sampling.seed = 1234;

        // The second run should reuse almost the whole prompt from the KV cache.
        for (int run = 1; run <= 2; ++run) {
            std::cout << "\n--- run " << run << " ---\n";
            const inference::Completion completion = backend.Complete(
                assembly.prompt, spec, [](std::string_view piece) { std::cout << piece << std::flush; }, {});
            std::cout << std::format("\n[prompt {} tokens: {} reused, {} evaluated in {:.0f} ms; {} generated in {:.0f} ms]\n",
                                     completion.promptTokens, completion.tokensReused, completion.tokensEvaluated,
                                     completion.promptMs, completion.tokensGenerated, completion.generateMs);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
```

`tools/probe/CMakeLists.txt`:

```cmake
add_executable(morpheus_probe main.cpp)
target_link_libraries(morpheus_probe PRIVATE morpheus_context morpheus_llama)
morpheus_target_defaults(morpheus_probe)
```

In the root `CMakeLists.txt`, extend the `if(MORPHEUS_BUILD_LLAMA)` block that adds `src/llama` with `add_subdirectory(tools/probe)`. Because `src/context` must exist before the probe, that block has to come after `add_subdirectory(src/context)`; move it there if needed.

- [ ] **Step 2: Update the docs**

**`CLAUDE.md`:**
- **Layout:** add
  - `src/inference` (the LlmBackend interface, no llama.cpp)
  - `src/context` (prompt assembly)
  - `src/llama` (llama.cpp backend, dev preset only)
  - `tools/probe`
  - `models/` and `settings.json` (both gitignored; see `settings.example.json`)
- **Build:**
  - `tests` and `asan` build without llama.cpp;
  - `dev` builds llama.cpp with Vulkan;
  - model-backed tests are tagged `[llama]` and need `MORPHEUS_TEST_CHAT_MODEL` and `MORPHEUS_TEST_EMBED_MODEL`.

**`README.md`:**
- **Prerequisites:** add the Vulkan SDK (for the `dev` preset).
- **Presets table:** update it.
- **Models:** add a short "Models and settings" section listing the two model downloads, how to copy `settings.example.json` to `settings.json`, and how to run the probe:

  ```
  ./build/dev/bin/Debug/morpheus_probe.exe settings.json game_projects/sandcrawler "I climb onto the crawler's roof."
  ```

**Architecture spec:**
- In the `turns` table row, `rolls` becomes `checks` ("typed check outcomes").
- In "Inference interface", note:
  - `save_state`/`restore_state` carry the cached token list;
  - the llama.cpp backend is built with Vulkan;
  - grammar text is supplied by the caller (generated from the schema in M3).
- In the module table, `core` also holds settings and turn-record types.

- [ ] **Step 3: Verify the milestone**

Run each preset:
1. `cmake --preset tests && cmake --build --preset tests && ctest --preset tests`. Expected: all pass, and llama.cpp is not configured.
2. `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`. Expected: all pass, with no AddressSanitizer report.
3. `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`. Expected:
   - all non-model tests pass;
   - `[llama]` tests are skipped, or pass when the model env vars are set;
   - `morpheus_probe.exe` is built.

If the model files exist under `models/` and a `settings.json` exists, also run the probe command from the README and record:
- the token line from each run;
- that run 2 reports `reused` = prompt tokens - 1.

If they do not exist, say so; that is not a failure.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt tools/probe CLAUDE.md README.md docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md
git commit -m "Add morpheus_probe and document milestone 2" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```
