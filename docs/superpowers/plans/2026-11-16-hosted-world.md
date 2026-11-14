# Hosted World (Sub-project D1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Morpheus runs a world it does not own: the host sends an action catalog, its observation's JSON Schema and optional lore once per run, and with each request the observation, events and who speaks; Morpheus assembles the prompt, proposes the speaker's action under a grammar built from the catalog and the actors present, validates it, streams the spoken line and keeps the run's memory, all in engine code (`src/turn` and below, no HTTP), while Morpheus's own game keeps its behaviour.

**Architecture:**
- **`grammar::Validate`** reads an extended subset for schemas that are only validated (numbers, booleans, optional keys, open objects, unbounded strings and arrays); `FromSchema` still refuses each. `grammar::CheckValidationSchema` checks such a schema without a value.
- **`store::RunStore`** moves to schema version 3: `run_meta.kind` (`owned` / `hosted`), and the tables `host_data`, `conversations`, `hosted_turns`, `conversation_events`. A version 2 file is upgraded in place on open. The hosted record types are plain structs in `core/HostedRecord.h`.
- **Shared memory upkeep (`turn::MemoryUpkeep`)**: `Session`'s fit loop, fold and condense move into one class that works on summaries and rendered transcript entries (`context::TranscriptEntry`: a turn number, its L3 messages, its fold messages and a one-line fact), so `Session` passes its turns and `HostedSession` its conversation's turns. The context module's prompt builder takes rendered entries.
- **`turn::HostedWorld`** parses and checks the host data, validates observations (a fixed core of actors, then the host's schema), builds the per-turn proposal schema and grammar (actor scopes, empty targets left out, allowed actions), and adjudicates the model's proposal.
- **Hosted prompt (`context::AssembleHostedPrompt`)**: L0 hosted system text, L1 the host's always-on lore, the participants, L2 run-wide summaries, L3 the conversation, then the tail (retrieved snippets, the observation as compact JSON, the new events, the player's line, the instruction); the same budgets and flags as the owned resolve prompt, through one shared layout helper. `EngineText` gains the default hosted texts.
- **`turn::HostedSession`**: opens (creates, reopens, replaces changed host data), manages conversations, plays a hosted turn (check, retrieve, resolve and adjudicate, speak streamed, commit) and folds through the shared upkeep (over budget during a conversation, everything left after `EndConversation` in `Maintain`).
- **Spec:** `docs/superpowers/specs/2026-11-16-hosted-world-design.md` (the single source of requirements); requirement R5 and section 5 of `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md`; architecture spec `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`.

**Tech Stack:** C++20 with MSVC (VS 2022), CMake 3.25+, nlohmann/json 3.12.0, SQLite (amalgamation), Catch2 v3, llama.cpp b9804 (`dev` preset, for the one `[llama]` test). No new dependency.

**Choices this plan makes where the spec is silent, ambiguous or cannot be followed literally (the owner may overrule any of them):**
- **The hosted record types live in `core`** (`core/HostedRecord.h`: `Participant`, `Conversation`, `ConversationEvent`, `Adjudication`, `HostedTurn`), because `store` stores them and must not depend on `turn`. The spec's `Adjudication` is `core::Adjudication`, and its `HostedTurn` is `core::HostedTurn`.
- **`HostedWorld` gains three public members the spec does not name:** `ProposalSchema(...)` (the schema `ProposalGrammar` writes and `Adjudicate` takes), `ActionIds()` (every catalog id, for a request without `allowedActions`) and the statics `ActorsOf(observation)` (the core actors of a stored observation, without the host's schema, so turns stored before a host data change still render) and `CheckEvents(events)`. `Lore()` returns the host's lore.
- **`HostedSession` gains a reader `Events(conversationId)`** (events recorded between turns) and `Meta()`, besides the spec's four readers.
- **Error field names** follow the JSON names D2 will expose: `conversation_id`, `speaker`, `participants/<i>`, `joined/<i>`, `left/<i>`, `allowed_actions/<i>`, `events/<i>/text`; the host data's own faults read `catalog at <pointer>`, `observation_schema: schema at <pointer>`, `lore at /<i>/<field>`, `host data at <pointer>`, and an observation's `observation at <pointer>` (one per fault, joined by `; `).
- **Events recorded between turns** are stored with the conversation's latest turn (`after_turn`, 0 before its first). The new events of a turn are those recorded since the conversation's last turn, then the request's own; in L3 a turn's entry shows them before its player text. Events recorded after a conversation's last turn are kept in `conversation_events` but not folded (a fold works on turns).
- **L3 is two messages per turn**, as the owned transcript is, so chat templates that require alternating roles accept it: a user message (the turn's events in parentheses, then `<player name>: <text>`; with neither, the cue `(<speaker> speaks.)`) and an assistant message (`<speaker name> [<action id>]: <line>`, with the params as compact JSON after the id when there are any: `Hulda [attack {"target":"0001B07C"}]: ...`; a refused action shows no bracket). A requested `none` shows `[none]`.
- **The participants layer shows the actor id:** `- Hulda (0001A6A4): <bio>`, or `- Aela (00000007)` without a bio, so the model can match a name to the ids of actor parameters.
- **`world_cap` bounds the host's always-on lore and the participants together** (`LayerTokens::world`); over it is a `DataError`. `engine_rules_cap` bounds the hosted system text (a `ContractError`, as for the owned rules).
- **Budgets:** the resolve call uses `resolve_max_tokens` and `resolve_thinking_tokens`; the speak call `narrate_max_tokens` and `narrate_thinking_tokens`; `narrate_allowance` covers the speak instruction, and an instruction over it (a very long speaker name) is a `DataError` naming `speaker` before any model call.
- **The default hosted texts use `{speaker}` and `{player}` only in the instructions and the cue**; the system text has no variable, so L0 never changes between turns.
- **Player text is cleaned as `Session` cleans input** (control characters other than newline and tab removed, trimmed); nothing left means no player text.
- **Conversation ids are `c1`, `c2`, ...** in creation order, made by the store.
- **`SetParticipants` applies `left` before `joined`**, so a leave and a rejoin in one call moves the actor to the end of the order; a conversation must keep at least one participant.
- **`Validate`'s extended subset drops llama.cpp's limits** (a validated `maxLength` or `maxItems` may exceed 1999 or 1000): they bound grammars only. `maxItems` must still be at least 1, an integer still needs both bounds, and an object still needs `properties` and `required`.
- **The fold of an ended conversation folds every remaining turn in one call when it fits** (`MemoryUpkeep::FoldAll`): `FoldOldest` takes how many turns to fold, and the fit loop passes half the transcript as before.
- **`Session::Maintain`'s loop moves into the upkeep too** (`MemoryUpkeep::Settle`), besides the fit loop, the fold and the condense the spec names: it is the same condense-or-fold loop without hits, and `HostedSession::Maintain` needs its sibling `FoldAll`.
- **"The request's events are added to the conversation before the turn"** is read as an order, not a write: they show in the turn's tail as new events, are stored in the committed turn record (`HostedTurn::events`), and render in L3 before the player's line. Storing them in `conversation_events` at step 0 would commit something on a cancel.
- **A conversation's folded mark (`conversations.folded_through`) says which of its turns are folded**, not the summaries' ranges: with conversations interleaved, run-wide summary ranges may overlap, and a condensed summary covers the first summary's start to the last one's end (by `from_turn`), as today.
- **`Maintain` condenses on the way while it folds an ended conversation.** With no ended conversation to fold, an over-cap summary is condensed by the next turn's fit loop (a condense prompt needs a conversation's system message).
- **Task 9's fixture tests pass as soon as they are written:** they pin Tasks 4-8 against the fixture files rather than drive new code.
- **Folded hosted turns are indexed as `Turn` history** (`MemoryIndex::SyncEntries`) under their run-wide number, embedded as their fact and shown as `Turn N: <fact>`.
- **`MemoryIndex::SyncLore` takes a lore list and a source name** (`world 'mini': lore.csv`, `host data: lore`); the world overload keeps its messages, except that the embed failure now names the source in front of `entry '<subject>' cannot be embedded`.
- **`TurnError` and the fold divisors move to `turn/MemoryUpkeep.h`**, which `turn/Session.h` includes, so every user of `turn::TurnError` still compiles unchanged; the fit loop's tail message names the caller's tail (`Session`'s text is byte-identical).
- **`Session`'s anonymous `Telemetry` becomes the public `turn::CallTelemetry`** in `turn/Session.h`, used by both sessions.
- **Tests that change besides the new ones** (none of them a `Session` test): `tests/grammar/SchemaTests.cpp` (eleven refusals become FromSchema-only, one `Validate` throw uses `null`), `tests/store/RunStoreTests.cpp` (version 3 in the version-1 test), `tests/turn/FoldingTests.cpp`, `tests/context/PromptBuilderTests.cpp` and `tests/app/ModelSeatTests.cpp` (rendered entries instead of `TurnRecord`s; no prompt text changes).
- **Fixtures:** `tests/hosted/fixtures/open.json` and `observation.json` arrive with `HostedWorld` (Task 4), the conversation fixtures with the scripted-conversation tests (Task 9).

## Global Constraints

- **Toolchain:** C++20 (`cxx_std_20`), MSVC through the `Visual Studio 17 2022` generator, x64, Windows 10. CMake >= 3.25. No new dependency.
- **Flags:** every MorpheusEngine target calls `morpheus_target_defaults()` (`/W4 /permissive- /Zc:preprocessor /utf-8`, warnings as errors at top level): a warning fails the build.
- **Exact names (from the spec, verbatim):**
  - `turn::HostedWorld` (`turn/HostedWorld.h`): constructed from `HostData`; `Observation Observe(const nlohmann::json& observation) const`; `std::string ProposalGrammar(const Observation&, const Participants&, const std::vector<std::string>& allowed, std::string_view speakerId) const`; `Adjudication Adjudicate(const nlohmann::json& proposalSchema, std::string_view modelText) const` returning `{ bool requested; std::string actionId; nlohmann::json params; std::vector<std::string> reasons; }`.
  - `Observation` holds the original JSON and the parsed actors (`id`, `name`, `isPlayer`, `bio`) in their order.
  - `turn::HostedSession` (`turn/HostedSession.h`): `static HostedSession Open(const std::filesystem::path& file, HostData data, core::Settings settings, std::uint64_t seed, inference::LlmBackend& backend)`; `std::string StartConversation(const std::vector<std::string>& participantIds, const nlohmann::json& observation)`; `void RecordEvents(std::string_view conversationId, const std::vector<nlohmann::json>& events)`; `void SetParticipants(std::string_view conversationId, const std::vector<std::string>& joined, const std::vector<std::string>& left, const nlohmann::json& observation)`; `std::optional<HostedTurn> RunTurn(const HostedTurnRequest& request, const HostedCallbacks& callbacks, std::stop_token stop)`; `void EndConversation(std::string_view conversationId)`; `bool Maintain(std::stop_token stop)`; readers `Conversation(id)`, `Turns(conversationId)`, `Summaries()`, `LastAssembly()`.
  - `HostData` holds the catalog, the observation schema (both JSON) and the lore (`core::LoreEntry` list). `HostedTurnRequest` holds `conversationId`, `speakerId`, an optional `playerText`, `events`, the `observation` and an optional `allowedActions`. `HostedCallbacks` holds `onAction` and `onToken`.
  - Catalog parameter types `{"type": "actor", "scope": "present" | "present_non_player" | "participant"}`, `{"type": "string", "maxLength": n}` (optional `minLength`), `{"type": "integer", "minimum": a, "maximum": b}`, `{"type": "enum", "values": [...]}`.
  - Observation core: `actors`, each with `id` (non-empty string), `name` (non-empty string), `is_player` (boolean), optional `bio` (string); ids unique; exactly one player. Events: JSON objects with a required `text` string.
  - Store: schema version 3; meta `kind` (owned or hosted); tables `conversations`, `hosted_turns`, `conversation_events`; a hosted run's world id is `hosted`.
  - `EngineText` variables `{speaker}` and `{player}`.
- **Dependencies flow downward:** `core` (new header only) <- `store` <- `context` <- `turn`; `grammar` <- `turn`. `context` uses `core/HostedRecord.h`. No module gains a library dependency.
- **No Skyrim knowledge in engine code:** every text and every name in `src/` stays generic. Host names (Hulda, Aela, Whiterun) appear only in `tests/` and in the fixtures.
- **Error policy:** engine bugs throw `core::ContractError` via `MORPHEUS_REQUIRE`; host data (catalog, observation schema, lore, observation, event, participant list, unknown speaker, conversation or allowed action, an owned run opened as hosted) throws `core::DataError` naming the field or JSON pointer; damaged saved data throws `store::StoreError` naming the file; a player text with no room in the tail is `turn::TurnError`; model mistakes are not exceptions (a refused action is recorded with its reasons and the turn goes on); backend failures stay `inference::InferenceError`; a cancel through `stop` returns nullopt and commits nothing. No fallbacks that hide a broken contract.
- **Coding rules (`CLAUDE.md`):** types and methods PascalCase, locals and parameters camelCase, members `_camelCase`, raw pointers `pName` / `_pName`; constants ALL_CAPS and grouped; members initialized at declaration (a member without a default constructor says `// Set by the constructor.`); class layout: nested types, public data, private data, public methods, private methods, helpers; comments let a method read top to bottom; a private helper only with more than one caller (local lambdas are fine); no legacy paths. ASCII only in source, comments, docs and fixtures.
- **Never name a local variable or parameter `grammar`** in a scope that uses the `grammar::` namespace.
- **Escape sequences:** implementers' file tools silently turn backslash-u and backslash-x escape text into raw bytes. No C++ literal and no fixture in this plan uses either. Where a test needs a backslash in a literal, it is built from the named constant `constexpr char BACKSLASH = '\\';` by concatenation.
- **Byte check (required before every commit):** run on every file the commit names; each count printed must be `0` (`grep -c` prints `file:count` per file and exits 1 when nothing matched, which is the expected outcome):

  ```bash
  LC_ALL=C grep -c $'[\x80-\xFF]' <files>
  LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' <files>
  ```

  A file with a non-zero count has a raw byte where an escape or a plain character was meant: rewrite that line.
- **Shell:** commands run from `D:\MorpheusEngine` in Git Bash:
  - `cmake --build --preset tests`, `ctest --preset tests`; one tag: `./build/tests/bin/Debug/morpheus_tests.exe "[tag]"`.
  - Before this plan `ctest --preset tests` runs 427 tests, all passing except the three `[server]` tests, which show as "Not Run (Skipped)". Each task states the new total as 427 plus the test cases added so far; skipped tests are not failures.
  - The `dev` tree is configured in `build/dev` (RelWithDebInfo): `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`.
  - `[llama]` tests: `export MORPHEUS_TEST_CHAT_MODEL="$PWD/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"; export MORPHEUS_TEST_EMBED_MODEL="$PWD/models/nomic-embed-text-v1.5.Q8_0.gguf"`, then `./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "<test names or tags>"`.
  - New test files and `CMakeLists.txt` changes are picked up by the next build (`CONFIGURE_DEPENDS`).
- **Never stage** `settings.json`, `models/`, `imgui.ini`, `.superpowers/` or `docs/references/`. Stage with the explicit paths each commit step names, nothing else (`git add -A` is forbidden).
- **Commits** are made on `master`. Every commit message ends with the line `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## File Structure

```
src/grammar/Schema.h, Schema.cpp                 modify: the extended Validate subset, CheckValidationSchema (Task 1)
tests/grammar/ValidateExtendedTests.cpp          create (Task 1)
tests/grammar/SchemaTests.cpp                    modify: refusals only FromSchema keeps (Task 1)
src/core/HostedRecord.h                          create: Participant, Conversation, ConversationEvent, Adjudication,
                                                 HostedTurn (Task 2)
src/store/RunStore.h, RunStore.cpp               modify: version 3, kind, hosted tables and methods, the upgrade (Task 2)
tests/store/HostedStoreTests.cpp                 create (Task 2)
tests/store/RunStoreTests.cpp                    modify: version 3 (Task 2)
src/context/Transcript.h, Transcript.cpp         modify: TranscriptEntry, TranscriptEntries (Task 3)
src/context/PromptBuilder.h, PromptBuilder.cpp   modify: rendered entries (Task 3); HostedInputs, AssembleHostedPrompt and
                                                 the shared layout (Task 5)
src/turn/MemoryUpkeep.h, MemoryUpkeep.cpp        create: TurnError, FitPass, MemoryUpkeep (Task 3)
src/turn/Folding.h, Folding.cpp                  modify: entries instead of TurnRecords (Task 3)
src/turn/Session.h, Session.cpp                  modify: calls MemoryUpkeep (Task 3); CallTelemetry public (Task 7)
src/turn/CMakeLists.txt                          modify: MemoryUpkeep.cpp (Task 3), HostedWorld.cpp (Task 4),
                                                 HostedSession.cpp (Task 6)
tests/turn/MemoryUpkeepTests.cpp                 create (Task 3)
tests/context/RenderTests.cpp, PromptBuilderTests.cpp, tests/turn/FoldingTests.cpp, tests/app/ModelSeatTests.cpp
                                                 modify: rendered entries (Task 3)
src/turn/HostedWorld.h, HostedWorld.cpp          create (Task 4)
tests/hosted/fixtures/open.json, observation.json  create (Task 4)
tests/hosted/HostedTestSupport.h                 create (Task 4); extended (Tasks 6, 7)
tests/hosted/HostedWorldTests.cpp                create (Task 4)
src/turn/EngineText.h                            modify: the default hosted texts (Task 5)
tests/context/HostedPromptTests.cpp              create (Task 5)
src/turn/Retrieval.h, Retrieval.cpp              modify: SyncLore over a lore list, SyncEntries (Task 6)
tests/turn/RetrievalTests.cpp                    modify: one new test (Task 6)
src/turn/HostedSession.h, HostedSession.cpp      create: Open, conversations, readers (Task 6); RunTurn (Task 7);
                                                 Maintain (Task 8)
tests/hosted/HostedSessionTests.cpp              create (Task 6)
tests/hosted/HostedTurnTests.cpp                 create (Task 7)
tests/hosted/HostedFoldingTests.cpp              create (Task 8)
tests/hosted/fixtures/conversation.json, radiant.json, invalid_requests.json   create (Task 9)
tests/hosted/HostedFixtureTests.cpp              create (Task 9)
tests/llama/LlamaHostedTests.cpp                 create: the [llama] hosted conversation (Task 9)
docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md, CLAUDE.md   modify (Task 10)
```

---

### Task 1: The extended `grammar::Validate` subset

`Validate` learns the keywords a schema that is only validated may use: numbers with optional bounds, booleans, properties outside `required` (optional), `additionalProperties: true` (extra keys allowed and unchecked), arrays without `maxItems` and strings without `maxLength`. `FromSchema` refuses every one of them with today's messages, because it must bound the grammar. `CheckValidationSchema` checks a schema against the extended subset without a value (`HostedWorld` checks the observation schema at open with it).

**Files:**
- Modify: `src/grammar/Schema.h`, `src/grammar/Schema.cpp`
- Create: `tests/grammar/ValidateExtendedTests.cpp`
- Modify: `tests/grammar/SchemaTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `std::vector<grammar::ValidationError> grammar::Validate(const nlohmann::json& value, const nlohmann::json& schema)` (same signature, extended subset); `void grammar::CheckValidationSchema(const nlohmann::json& schema)` in `grammar/Schema.h` (throws the `DataError` `Validate` would throw for `schema`, `"schema at <pointer>: <problem>"`). New value messages: `"expected a number, got X"`, `"expected a number from A to B, got X"`, `"expected a number of at least A, got X"`, `"expected a number of at most B, got X"`, `"expected a boolean, got X"`, `"expected an object"`, `"expected a string, got X"`, `"expected a string of at least N characters, got X"`, `"expected an array, got X"`, `"expected an array of at least N items, got X"`, `"has N items; expected at least M"`. New schema messages: `"'minimum' must be a number"`, `"'additionalProperties' must be true or false"`, `"'X' does not apply to a number"` / `"to a boolean"`.

- [ ] **Step 1: Write the failing tests**

Create `tests/grammar/ValidateExtendedTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "grammar/Schema.h"
#include "grammar/TestSchemas.h"

using namespace morpheus;
using namespace nlohmann::literals;

namespace {

// The one error Validate finds in `value` against `schema`.
grammar::ValidationError OnlyError(const nlohmann::json& value, const nlohmann::json& schema)
{
    const std::vector<grammar::ValidationError> errors = grammar::Validate(value, schema);
    INFO("value: " << value.dump());
    for (const grammar::ValidationError& error : errors) {
        UNSCOPED_INFO(error.path << ": " << error.message);
    }
    REQUIRE(errors.size() == 1);
    return errors[0];
}

// The message of the DataError Validate throws for `schema`.
std::string ValidateSchemaError(const nlohmann::json& schema)
{
    try {
        grammar::Validate(nlohmann::json(), schema);
    } catch (const core::DataError& error) {
        return error.what();
    }
    return "";
}

} // namespace

TEST_CASE("Validate accepts numbers within their optional bounds", "[schema]")
{
    const nlohmann::json any = R"({"type": "number"})"_json;
    CHECK(grammar::Validate(nlohmann::json(3), any).empty());
    CHECK(grammar::Validate(nlohmann::json(-2.5), any).empty());
    CHECK(grammar::Validate(nlohmann::json::parse("18446744073709551615"), any).empty());
    const grammar::ValidationError text = OnlyError(nlohmann::json("3"), any);
    CHECK(text.path.empty());
    CHECK(text.message == "expected a number, got '3'");
    CHECK(OnlyError(nlohmann::json(true), any).message == "expected a number, got true"); // A bool is not a number.

    const nlohmann::json hours = R"({"type": "number", "minimum": 0, "maximum": 23.5})"_json;
    CHECK(grammar::Validate(nlohmann::json(0), hours).empty());
    CHECK(grammar::Validate(nlohmann::json(23.5), hours).empty());
    CHECK(OnlyError(nlohmann::json(24), hours).message == "expected a number from 0 to 23.5, got 24");
    CHECK(OnlyError(nlohmann::json(-0.25), hours).message == "expected a number from 0 to 23.5, got -0.25");
    CHECK(OnlyError(nlohmann::json(1), R"({"type": "number", "minimum": 1.5})"_json).message ==
          "expected a number of at least 1.5, got 1");
    CHECK(OnlyError(nlohmann::json(0), R"({"type": "number", "maximum": -1})"_json).message ==
          "expected a number of at most -1, got 0");

    // A number's bounds are numbers, in order, and nothing else applies to it.
    CHECK(ValidateSchemaError(R"({"type": "number", "minimum": "0"})"_json) ==
          "schema at /minimum: 'minimum' must be a number");
    CHECK(ValidateSchemaError(R"({"type": "number", "minimum": 2, "maximum": 1})"_json) ==
          "schema at the root: 'minimum' 2 is above 'maximum' 1");
    CHECK(ValidateSchemaError(R"({"type": "number", "maxLength": 3})"_json) ==
          "schema at /maxLength: 'maxLength' does not apply to a number");
}

TEST_CASE("Validate accepts booleans", "[schema]")
{
    const nlohmann::json schema = R"({"type": "boolean"})"_json;
    CHECK(grammar::Validate(nlohmann::json(true), schema).empty());
    CHECK(grammar::Validate(nlohmann::json(false), schema).empty());
    CHECK(OnlyError(nlohmann::json(0), schema).message == "expected a boolean, got 0");
    CHECK(OnlyError(nlohmann::json("true"), schema).message == "expected a boolean, got 'true'");
    CHECK(ValidateSchemaError(R"({"type": "boolean", "minimum": 0})"_json) ==
          "schema at /minimum: 'minimum' does not apply to a boolean");
}

TEST_CASE("Validate treats properties outside required as optional", "[schema]")
{
    const nlohmann::json schema = R"({"type": "object", "required": ["id"], "properties": {
        "id": {"type": "string", "maxLength": 8},
        "bio": {"type": "string"},
        "age": {"type": "integer", "minimum": 0, "maximum": 200}}})"_json;
    CHECK(grammar::Validate(R"({"id": "a"})"_json, schema).empty());
    CHECK(grammar::Validate(R"({"id": "a", "bio": "b", "age": 3})"_json, schema).empty());

    // A present optional key is checked; a missing required one is an error; extra keys are not allowed.
    const grammar::ValidationError age = OnlyError(R"({"id": "a", "age": -1})"_json, schema);
    CHECK(age.path == "/age");
    CHECK(age.message == "expected an integer from 0 to 200, got -1");
    const grammar::ValidationError missing = OnlyError(R"({"bio": "b"})"_json, schema);
    CHECK(missing.path == "/id");
    CHECK(missing.message == "missing key 'id'");
    // The keys are listed required first, then the optional ones by name.
    const grammar::ValidationError extra = OnlyError(R"({"id": "a", "rank": 1})"_json, schema);
    CHECK(extra.path == "/rank");
    CHECK(extra.message == "unexpected key 'rank'; the keys are 'id', 'age', 'bio'");
    CHECK(OnlyError(nlohmann::json(3), schema).message == "expected an object with the keys 'id'");
    const nlohmann::json allOptional = R"({"type": "object", "required": [], "properties": {"a": {"type": "boolean"}}})"_json;
    CHECK(grammar::Validate(nlohmann::json::object(), allOptional).empty());
    CHECK(OnlyError(nlohmann::json(3), allOptional).message == "expected an object");

    // A oneOf branch still needs its discriminator, the first key of `required`.
    CHECK(ValidateSchemaError(R"({"oneOf": [{"type": "object", "properties": {"action": {"const": "x"}}, "required": []}]})"_json) ==
          "schema at /oneOf/0/required: a 'oneOf' branch needs a discriminator, the first key of 'required'");
}

TEST_CASE("Validate lets an open object hold extra keys, unchecked", "[schema]")
{
    const nlohmann::json schema = R"({"type": "object", "required": ["id"], "additionalProperties": true,
        "properties": {"id": {"type": "string", "maxLength": 8}}})"_json;
    CHECK(grammar::Validate(R"({"id": "a", "gender": "female", "equipment": {"body": "robes"}})"_json, schema).empty());
    const grammar::ValidationError id = OnlyError(R"({"id": 7, "extra": 1})"_json, schema);
    CHECK(id.path == "/id");
    CHECK(id.message == "expected a string of at most 8 characters, got 7");

    const nlohmann::json anything = R"({"type": "object", "properties": {}, "required": [], "additionalProperties": true})"_json;
    CHECK(grammar::Validate(R"({"anything": [1, 2]})"_json, anything).empty());
    CHECK(OnlyError(nlohmann::json::array(), anything).message == "expected an object");
    CHECK(ValidateSchemaError(R"({"type": "object", "properties": {}, "required": [], "additionalProperties": "yes"})"_json) ==
          "schema at /additionalProperties: 'additionalProperties' must be true or false");
}

TEST_CASE("Validate accepts arrays without maxItems and strings without maxLength", "[schema]")
{
    const nlohmann::json strings = R"({"type": "array", "items": {"type": "string"}})"_json;
    CHECK(grammar::Validate(nlohmann::json::array(), strings).empty());
    std::vector<std::string> many(1200, "x");
    many.push_back(std::string(2500, 'y'));
    CHECK(grammar::Validate(nlohmann::json(many), strings).empty());
    CHECK(OnlyError(nlohmann::json("x"), strings).message == "expected an array, got 'x'");
    const grammar::ValidationError item = OnlyError(R"(["a", 2])"_json, strings);
    CHECK(item.path == "/1");
    CHECK(item.message == "expected a string, got 2");

    const nlohmann::json atLeastTwo = R"({"type": "array", "items": {"const": 1}, "minItems": 2})"_json;
    CHECK(OnlyError(R"([1])"_json, atLeastTwo).message == "has 1 items; expected at least 2");
    CHECK(OnlyError(nlohmann::json("x"), atLeastTwo).message == "expected an array of at least 2 items, got 'x'");
    const nlohmann::json named = R"({"type": "string", "minLength": 1})"_json;
    CHECK(OnlyError(nlohmann::json(""), named).message == "shorter than 1 characters");
    CHECK(OnlyError(nlohmann::json(3), named).message == "expected a string of at least 1 characters, got 3");

    // llama.cpp's limits bound grammars only: a schema that is only validated may go past them.
    CHECK(grammar::Validate(nlohmann::json(std::string(2500, 'a')), R"({"type": "string", "maxLength": 3000})"_json).empty());
    CHECK(grammar::Validate(nlohmann::json(std::vector<int>(1500, 1)),
                            R"({"type": "array", "items": {"const": 1}, "maxItems": 5000})"_json)
              .empty());
}

TEST_CASE("FromSchema still refuses every schema only Validate accepts", "[schema]")
{
    const std::vector<std::string> validationOnly = {
        R"({"type": "number"})",
        R"({"type": "number", "minimum": 0, "maximum": 1})",
        R"({"type": "boolean"})",
        R"({"type": "object", "properties": {"a": {"const": 1}}, "required": []})",
        R"({"type": "object", "properties": {}, "required": [], "additionalProperties": true})",
        R"({"type": "array", "items": {"const": 1}})",
        R"({"type": "string"})",
        R"({"type": "string", "maxLength": 2000})",
        R"({"type": "array", "items": {"const": 1}, "maxItems": 1001})",
    };
    for (const std::string& text : validationOnly) {
        INFO(text);
        const nlohmann::json schema = nlohmann::json::parse(text);
        CHECK_THROWS_AS(grammar::FromSchema(schema), core::DataError);
        CHECK_NOTHROW(grammar::Validate(nlohmann::json(), schema));
    }
    // Today's messages, word for word.
    CHECK_THROWS_WITH(grammar::FromSchema(R"({"type": "boolean"})"_json), "schema at /type: the type 'boolean' is not supported");
    CHECK_THROWS_WITH(grammar::FromSchema(R"({"type": "string"})"_json), "schema at the root: a string needs 'maxLength'");
    CHECK_THROWS_WITH(grammar::FromSchema(R"({"type": "object", "properties": {}, "required": [], "additionalProperties": true})"_json),
                      "schema at /additionalProperties: 'additionalProperties' may only be false: extra keys are never allowed");
}

TEST_CASE("CheckValidationSchema accepts the extended subset and throws Validate's schema errors", "[schema]")
{
    CHECK_NOTHROW(grammar::CheckValidationSchema(test::CatalogSchema()));
    CHECK_NOTHROW(grammar::CheckValidationSchema(R"({"type": "object", "required": [], "additionalProperties": true,
        "properties": {"n": {"type": "number"}, "b": {"type": "boolean"}, "s": {"type": "string"}}})"_json));
    CHECK_THROWS_WITH(grammar::CheckValidationSchema(R"({"type": "string", "pattern": "a+"})"_json),
                      "schema at /pattern: the keyword 'pattern' is not supported");
    CHECK_THROWS_WITH(grammar::CheckValidationSchema(R"({"type": "null"})"_json),
                      "schema at /type: the type 'null' is not supported");
    CHECK_THROWS_WITH(grammar::CheckValidationSchema(R"({"type": "integer", "minimum": 0})"_json),
                      "schema at the root: an integer needs 'minimum' and 'maximum'");
}
```

In `tests/grammar/SchemaTests.cpp`, after the `SchemaError` helper (just before `// The one error Validate finds in `value` against `schema`.`), add:

```cpp
// The message of the DataError FromSchema throws for the schema `text`, after checking that Validate accepts it: the
// schema is in the extended subset of schemas that are only validated.
std::string GrammarOnlyError(const std::string& text)
{
    const nlohmann::json schema = nlohmann::json::parse(text);
    std::string fromSchema;
    try {
        grammar::FromSchema(schema);
    } catch (const core::DataError& error) {
        fromSchema = error.what();
    }
    INFO("schema: " << text);
    REQUIRE_FALSE(fromSchema.empty());
    CHECK_NOTHROW(grammar::Validate(nlohmann::json::object(), schema));
    return fromSchema;
}
```

Then, in the same file, replace `SchemaError(` with `GrammarOnlyError(` in exactly these eleven calls (each expected message stays as it is):

1. `SchemaError(R"({"type": "boolean"})")` (test "FromSchema refuses what is not a schema of the subset, naming the pointer")
2. `SchemaError(R"({"type": "number"})")` (same test)
3. `SchemaError(R"({"type": "object", "properties": {"a": {"const": 1}}, "required": []})")` (test "FromSchema refuses objects outside the subset")
4. `SchemaError(R"({"type": "object", "properties": {}, "required": [], "additionalProperties": true})")` (same test)
5. `SchemaError(R"({"type": "object", "properties": {"a/b~c": {"type": "string"}}, "required": ["a/b~c"]})")` (same test)
6. `SchemaError(R"({"type": "string"})")` (test "FromSchema refuses strings outside the subset")
7. `SchemaError(R"({"type": "string", "maxLength": 2000})")` (same test)
8. `SchemaError(R"({"type": "array", "items": {"const": 1}})")` (test "FromSchema refuses arrays outside the subset")
9. `SchemaError(R"({"type": "array", "items": {"type": "string"}, "maxItems": 3})")` (same test)
10. `SchemaError(R"({"type": "array", "items": {"const": 1}, "maxItems": 1001})")` (same test)
11. `SchemaError(schema.dump())` (test "FromSchema names the pointer of a mistake deep inside the schema")

And in the test "Validate throws the schema's DataError and never one for the value", replace

```cpp
    CHECK_THROWS_AS(grammar::Validate(nlohmann::json::object(), R"({"type": "boolean"})"_json), core::DataError);
```

with

```cpp
    CHECK_THROWS_AS(grammar::Validate(nlohmann::json::object(), R"({"type": "null"})"_json), core::DataError);
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile error `'CheckValidationSchema': is not a member of 'morpheus::grammar'` in `tests/grammar/ValidateExtendedTests.cpp`.

- [ ] **Step 3: Write the extended subset**

In `src/grammar/Schema.h`, replace the `Validate` comment and declaration (from `// Checks `value` against `schema`: exactly what FromSchema's grammar allows.` to `std::vector<ValidationError> Validate(const nlohmann::json& value, const nlohmann::json& schema);`) with:

```cpp
// Checks `value` against `schema`. The schema may use everything FromSchema supports, plus what only a validated
// schema may (the extended subset, for schemas such as a host's observation schema that never become a grammar):
//   {"type": "number"}                    any JSON number (not a bool), with optional "minimum" and "maximum" (any
//                                         numbers, a <= b), both included
//   {"type": "boolean"}
//   object properties not in "required"   optional: checked when present; they follow the required keys, by name
//   "additionalProperties": true          extra keys are allowed and not checked
//   {"type": "array"} without "maxItems"  optional "minItems"; "maxItems", when given, at least 1
//   {"type": "string"} without "maxLength"  optional "minLength"
// and llama.cpp's limits on "maxLength" and "maxItems" do not apply. What FromSchema's grammar allows is checked as
// it allows it: types (an integer is a JSON integer, not a float), enum and const, integer ranges, string lengths in
// characters (core::Utf8Length), required keys present and no extra keys unless the object is open, array sizes and
// each item. For a oneOf the discriminator's value picks the branch: a missing discriminator, or a value naming no
// branch, is one error at the discriminator's path; otherwise the errors are the chosen branch's, with full paths.
// Every error is reported, in document order: an object's keys in the schema's order (required keys in `required`
// order, then optional keys by name), each with its nested errors, then the keys the schema does not allow. An empty
// result means the value is valid. Never throws on the value: a mistake in the value is data. A schema outside the
// extended subset throws a DataError of FromSchema's form, "schema at <pointer>: <problem>".
// Two limits of a parsed value: the grammar's shared `char` rule admits a lone surrogate escape in the model's text,
// which strict JSON parsers such as nlohmann's refuse, so a host must treat a parse failure of the model's text as a
// model mistake before it calls Validate; and key order is enforced by the grammar, not by Validate, because a parsed
// value has lost it.
std::vector<ValidationError> Validate(const nlohmann::json& value, const nlohmann::json& schema);

// Checks that `schema` is in Validate's extended subset, without a value: throws the DataError Validate would throw
// for it, and returns when Validate accepts it. A host's validation-only schema is checked with it when it arrives,
// so an unsupported keyword fails then rather than at the first value.
void CheckValidationSchema(const nlohmann::json& schema);
```

Replace the whole of `src/grammar/Schema.cpp` with:

```cpp
#include "grammar/Schema.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Errors.h"
#include "core/JsonInt.h"
#include "core/Text.h"
#include "grammar/Gbnf.h"

namespace morpheus::grammar {

namespace {

// Keywords that do not change the values a schema allows: accepted anywhere and ignored.
constexpr std::array<std::string_view, 5> IGNORED_KEYWORDS = {"title", "description", "$comment", "$schema", "$id"};
// Every keyword of the subset; any other is refused by name.
constexpr std::array<std::string_view, 14> SUBSET_KEYWORDS = {
    "type",    "enum",    "const",     "oneOf",     "properties", "required", "additionalProperties",
    "minimum", "maximum", "minLength", "maxLength", "items",      "minItems", "maxItems"};
// The keywords that say what a value is; a schema has exactly one of them.
constexpr std::array<std::string_view, 4> KIND_KEYWORDS = {"type", "enum", "const", "oneOf"};
// A string's or an array's count with no upper limit: only a schema that is validated, never written as a grammar,
// has one.
constexpr int UNBOUNDED = -1;
// A number's absent bounds.
constexpr double NO_MINIMUM = -std::numeric_limits<double>::infinity();
constexpr double NO_MAXIMUM = std::numeric_limits<double>::infinity();

// What a schema is read for. A grammar must be bounded, so FromSchema reads the subset; Validate and
// CheckValidationSchema read the extended subset of schemas that are only validated.
enum class Use { Grammar, Validation };

// A schema checked against the subset, as FromSchema and Validate read it.
struct Node {
    enum class Kind { Object, Enum, Const, Integer, Number, Boolean, String, Array, OneOf };

    Kind kind = Kind::Object;
    // Object: its keys, the required ones in `required` order, then the optional ones (Validation only) by name.
    std::vector<std::string> keys = {};
    std::size_t requiredKeys = 0;            // Object: how many of `keys`, from the first, are required.
    bool open = false;                       // Object: "additionalProperties": true (Validation only).
    std::vector<Node> children = {};         // Object: one value schema per key; Array: the item schema;
                                             // OneOf: the branches, each an Object whose first key is the discriminator.
    std::vector<nlohmann::json> values = {}; // Enum: the allowed values (all strings or all integers); Const: the one.
    long long minimum = 0;                   // Integer: the bounds, both included.
    long long maximum = 0;
    double lowest = NO_MINIMUM;              // Number (Validation only): the bounds, both included.
    double highest = NO_MAXIMUM;
    int minCount = 0;                        // String: characters; Array: items.
    int maxCount = 0;                        // UNBOUNDED when there is no upper limit (Validation only).
};

// `key` as one reference token of a JSON pointer (RFC 6901): "~" is written "~0" and "/" is written "~1".
std::string PointerToken(std::string_view key)
{
    std::string token;
    for (const char c : key) {
        if (c == '~') {
            token += "~0";
        } else if (c == '/') {
            token += "~1";
        } else {
            token += c;
        }
    }
    return token;
}

// Equal as the grammar sees it: a string to a string, an integer to an integer (1.0 is not 1). Integers are compared by
// their text, as the grammar's Literal writes them: nlohmann's == would equate -1 with 18446744073709551615.
bool SameValue(const nlohmann::json& a, const nlohmann::json& b)
{
    if (a.is_string() && b.is_string()) {
        return a == b;
    }
    return a.is_number_integer() && b.is_number_integer() && a.dump() == b.dump();
}

// Reads `schema`, found at `pointer` inside the whole schema, into a Node, checking it against the subset `use` reads.
// Throws DataError naming the pointer of the mistake. Recursive: the schemas of an object's properties, an array's
// items and a oneOf's branches follow the same rules.
Node Compile(const nlohmann::json& schema, const std::string& pointer, Use use)
{
    auto error = [](const std::string& at, const std::string& problem) {
        return core::DataError(std::format("schema at {}: {}", at.empty() ? "the root" : at, problem));
    };
    auto contains = [](const auto& keywords, const std::string& key) {
        return std::find(keywords.begin(), keywords.end(), key) != keywords.end();
    };
    if (!schema.is_object()) {
        throw error(pointer, "a schema must be a JSON object");
    }

    // Keywords outside the subset are named first, so that "$ref" or "pattern" is reported as such.
    for (auto it = schema.begin(); it != schema.end(); ++it) {
        if (!contains(SUBSET_KEYWORDS, it.key()) && !contains(IGNORED_KEYWORDS, it.key())) {
            throw error(pointer + "/" + PointerToken(it.key()), std::format("the keyword '{}' is not supported", it.key()));
        }
    }
    // Exactly one keyword says what the value is.
    std::vector<std::string> kinds;
    for (const std::string_view keyword : KIND_KEYWORDS) {
        if (schema.contains(std::string(keyword))) {
            kinds.emplace_back(keyword);
        }
    }
    if (kinds.empty()) {
        throw error(pointer, "needs 'type', 'enum', 'const' or 'oneOf'");
    }
    if (kinds.size() > 1) {
        throw error(pointer, std::format("'{}' and '{}' cannot be combined", kinds[0], kinds[1]));
    }
    const std::string kind = kinds[0];

    // Only this kind's keywords, and the ignored ones, may appear.
    auto allowOnly = [&](std::string_view what, std::initializer_list<std::string_view> allowed) {
        for (auto it = schema.begin(); it != schema.end(); ++it) {
            if (!contains(allowed, it.key()) && !contains(IGNORED_KEYWORDS, it.key())) {
                throw error(pointer + "/" + PointerToken(it.key()), std::format("'{}' does not apply to {}", it.key(), what));
            }
        }
    };
    // A bound keyword: absent (nullopt), or a JSON integer from low to high.
    auto integer = [&](const char* keyword, long long low, long long high) -> std::optional<long long> {
        if (!schema.contains(keyword)) {
            return std::nullopt;
        }
        const std::optional<long long> value = core::JsonIntInRange(schema.at(keyword), low, high);
        if (!value) {
            throw error(pointer + "/" + keyword, std::format("'{}' must be an integer from {} to {}", keyword, low, high));
        }
        return value;
    };
    // A count keyword above `most`, the largest a grammar llama.cpp loads can hold (see GBNF_MAX_STRING_CHARS).
    auto beyondLlamaLimit = [&](const char* keyword, long long count, long long most) {
        return error(pointer + "/" + keyword,
                     std::format("'{}' {} is above {}, the most a grammar llama.cpp loads can hold", keyword, count, most));
    };
    constexpr long long LOWEST = std::numeric_limits<long long>::min();
    constexpr long long HIGHEST = std::numeric_limits<long long>::max();
    constexpr long long MOST_COUNT = std::numeric_limits<int>::max();
    const bool grammarUse = use == Use::Grammar;

    Node node;
    if (kind == "oneOf") {
        allowOnly("a oneOf", {"oneOf"});
        const nlohmann::json& branches = schema.at("oneOf");
        const std::string at = pointer + "/oneOf";
        if (!branches.is_array() || branches.empty()) {
            throw error(at, "'oneOf' must be a non-empty array");
        }
        node.kind = Node::Kind::OneOf;
        std::map<std::string, std::string> taken; // Discriminator value -> the branch that has it.
        for (std::size_t i = 0; i < branches.size(); ++i) {
            const nlohmann::json& branch = branches[i];
            const std::string branchAt = std::format("{}/{}", at, i);
            if (!branch.is_object() || !branch.contains("type") || branch.at("type") != "object") {
                throw error(branchAt, "a 'oneOf' branch must be an object schema");
            }
            Node compiled = Compile(branch, branchAt, use);
            // The discriminator: the first key of the branch's `required`, the same key in every branch, and a const
            // string in each that no other branch has.
            if (compiled.requiredKeys == 0) {
                throw error(branchAt + "/required", "a 'oneOf' branch needs a discriminator, the first key of 'required'");
            }
            const std::string& key = compiled.keys[0];
            if (i > 0 && key != node.children[0].keys[0]) {
                throw error(branchAt + "/required/0", std::format("the discriminator is '{}' here but '{}' in {}/0", key,
                                                                  node.children[0].keys[0], at));
            }
            const Node& tag = compiled.children[0];
            const std::string tagAt = branchAt + "/properties/" + PointerToken(key);
            if (tag.kind != Node::Kind::Const || !tag.values[0].is_string()) {
                throw error(tagAt, std::format("the discriminator '{}' must be a 'const' string", key));
            }
            const std::string value = tag.values[0].get<std::string>();
            if (const auto found = taken.find(value); found != taken.end()) {
                throw error(tagAt + "/const",
                            std::format("the discriminator value '{}' is repeated: {} has it too", value, found->second));
            }
            taken.emplace(value, branchAt);
            node.children.push_back(std::move(compiled));
        }
        return node;
    }

    if (kind == "enum") {
        allowOnly("an enum", {"enum"});
        const nlohmann::json& values = schema.at("enum");
        const std::string at = pointer + "/enum";
        if (!values.is_array() || values.empty()) {
            throw error(at, "'enum' must be a non-empty array");
        }
        // All strings or all integers (a bool is neither), each listed once.
        const bool strings = values[0].is_string();
        for (std::size_t i = 0; i < values.size(); ++i) {
            const nlohmann::json& value = values[i];
            const std::string valueAt = std::format("{}/{}", at, i);
            if (strings ? !value.is_string() : !value.is_number_integer()) {
                throw error(valueAt, "'enum' values must be all strings or all integers");
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (SameValue(values[j], value)) {
                    throw error(valueAt, std::format("{} is listed twice", value.dump()));
                }
            }
        }
        node.kind = Node::Kind::Enum;
        node.values.assign(values.begin(), values.end());
        return node;
    }

    if (kind == "const") {
        allowOnly("a const", {"const"});
        const nlohmann::json& value = schema.at("const");
        if (!value.is_string() && !value.is_number_integer()) {
            throw error(pointer + "/const", "'const' must be a string or an integer");
        }
        node.kind = Node::Kind::Const;
        node.values.push_back(value);
        return node;
    }

    // The rest is told by "type".
    const nlohmann::json& type = schema.at("type");
    if (!type.is_string()) {
        throw error(pointer + "/type", "'type' must be a string");
    }
    const std::string typeName = type.get<std::string>();

    if (typeName == "integer") {
        allowOnly("an integer", {"type", "minimum", "maximum"});
        const std::optional<long long> minimum = integer("minimum", LOWEST, HIGHEST);
        const std::optional<long long> maximum = integer("maximum", LOWEST, HIGHEST);
        if (!minimum || !maximum) {
            throw error(pointer, "an integer needs 'minimum' and 'maximum'");
        }
        if (*minimum > *maximum) {
            throw error(pointer, std::format("'minimum' {} is above 'maximum' {}", *minimum, *maximum));
        }
        node.kind = Node::Kind::Integer;
        node.minimum = *minimum;
        node.maximum = *maximum;
        return node;
    }

    // A number and a boolean are only validated: a grammar of them is not part of the subset.
    if (typeName == "number" && !grammarUse) {
        allowOnly("a number", {"type", "minimum", "maximum"});
        // A bound: absent, or any JSON number (a bool is not one).
        auto bound = [&](const char* keyword, double absent) {
            if (!schema.contains(keyword)) {
                return absent;
            }
            const nlohmann::json& value = schema.at(keyword);
            if (!value.is_number()) {
                throw error(pointer + "/" + keyword, std::format("'{}' must be a number", keyword));
            }
            return value.get<double>();
        };
        node.kind = Node::Kind::Number;
        node.lowest = bound("minimum", NO_MINIMUM);
        node.highest = bound("maximum", NO_MAXIMUM);
        if (node.lowest > node.highest) {
            throw error(pointer, std::format("'minimum' {} is above 'maximum' {}", node.lowest, node.highest));
        }
        return node;
    }
    if (typeName == "boolean" && !grammarUse) {
        allowOnly("a boolean", {"type"});
        node.kind = Node::Kind::Boolean;
        return node;
    }

    if (typeName == "string") {
        allowOnly("a string", {"type", "minLength", "maxLength"});
        const std::optional<long long> minLength = integer("minLength", 0, MOST_COUNT);
        const std::optional<long long> maxLength = integer("maxLength", 0, MOST_COUNT);
        // A grammar needs the upper limit, within llama.cpp's; a validated string may have none, or a larger one.
        if (!maxLength && grammarUse) {
            throw error(pointer, "a string needs 'maxLength'");
        }
        if (maxLength && grammarUse && *maxLength > GBNF_MAX_STRING_CHARS) {
            throw beyondLlamaLimit("maxLength", *maxLength, GBNF_MAX_STRING_CHARS);
        }
        if (maxLength && minLength.value_or(0) > *maxLength) {
            throw error(pointer, std::format("'minLength' {} is above 'maxLength' {}", *minLength, *maxLength));
        }
        node.kind = Node::Kind::String;
        node.minCount = static_cast<int>(minLength.value_or(0));
        node.maxCount = maxLength ? static_cast<int>(*maxLength) : UNBOUNDED;
        return node;
    }

    if (typeName == "array") {
        allowOnly("an array", {"type", "items", "minItems", "maxItems"});
        if (!schema.contains("items")) {
            throw error(pointer, "an array needs 'items'");
        }
        const std::optional<long long> minItems = integer("minItems", 0, MOST_COUNT);
        const std::optional<long long> maxItems = integer("maxItems", 0, MOST_COUNT);
        // As for a string: a grammar needs the upper limit within llama.cpp's; a validated array may have none.
        if (!maxItems && grammarUse) {
            throw error(pointer, "an array needs 'maxItems'");
        }
        if (maxItems && *maxItems < 1) {
            throw error(pointer + "/maxItems", "'maxItems' must be at least 1");
        }
        if (maxItems && grammarUse && *maxItems > GBNF_MAX_LIST_ITEMS) {
            throw beyondLlamaLimit("maxItems", *maxItems, GBNF_MAX_LIST_ITEMS);
        }
        if (maxItems && minItems.value_or(0) > *maxItems) {
            throw error(pointer, std::format("'minItems' {} is above 'maxItems' {}", *minItems, *maxItems));
        }
        node.kind = Node::Kind::Array;
        node.minCount = static_cast<int>(minItems.value_or(0));
        node.maxCount = maxItems ? static_cast<int>(*maxItems) : UNBOUNDED;
        node.children.push_back(Compile(schema.at("items"), pointer + "/items", use));
        return node;
    }

    if (typeName == "object") {
        allowOnly("an object", {"type", "properties", "required", "additionalProperties"});
        if (schema.contains("additionalProperties")) {
            const nlohmann::json& additional = schema.at("additionalProperties");
            // A grammar never allows extra keys; a validated object may be open.
            if (grammarUse && additional != false) {
                throw error(pointer + "/additionalProperties",
                            "'additionalProperties' may only be false: extra keys are never allowed");
            }
            if (!additional.is_boolean()) {
                throw error(pointer + "/additionalProperties", "'additionalProperties' must be true or false");
            }
            node.open = additional.get<bool>();
        }
        if (!schema.contains("properties") || !schema.at("properties").is_object()) {
            throw error(pointer, "an object needs 'properties', an object");
        }
        if (!schema.contains("required") || !schema.at("required").is_array()) {
            throw error(pointer, "an object needs 'required', an array");
        }
        const nlohmann::json& properties = schema.at("properties");
        const nlohmann::json& required = schema.at("required");
        node.kind = Node::Kind::Object;
        // The required keys in `required` order, which the grammar writes them in (JSON objects carry no order).
        for (std::size_t i = 0; i < required.size(); ++i) {
            const std::string at = std::format("{}/required/{}", pointer, i);
            if (!required[i].is_string()) {
                throw error(at, "'required' entries must be strings");
            }
            const std::string key = required[i].get<std::string>();
            if (!properties.contains(key)) {
                throw error(at, std::format("'{}' is required but not in 'properties'", key));
            }
            if (std::find(node.keys.begin(), node.keys.end(), key) != node.keys.end()) {
                throw error(at, std::format("'{}' is listed twice", key));
            }
            node.keys.push_back(key);
            node.children.push_back(Compile(properties.at(key), pointer + "/properties/" + PointerToken(key), use));
        }
        node.requiredKeys = node.keys.size();
        // Then every other property: optional, which only a validated object may have. nlohmann keeps an object's
        // keys sorted, so they follow by name.
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            if (std::find(node.keys.begin(), node.keys.end(), it.key()) != node.keys.end()) {
                continue;
            }
            const std::string at = pointer + "/properties/" + PointerToken(it.key());
            if (grammarUse) {
                throw error(at, std::format("'{}' must be in 'required': optional keys are not supported", it.key()));
            }
            node.keys.push_back(it.key());
            node.children.push_back(Compile(it.value(), at, use));
        }
        return node;
    }

    throw error(pointer + "/type", std::format("the type '{}' is not supported", typeName));
}

// The grammar expression for the values `node` allows. Recursive, like Compile. Each array's item expression becomes a
// rule of `builder`, items-0, items-1 and so on in the order they are added (inner arrays first), so that ListOf repeats
// a single reference: llama.cpp limits a repetition by the symbols its repeated group creates. Only a node compiled for
// Use::Grammar comes here, so every count is bounded and every key required.
std::string Expression(const Node& node, GrammarBuilder& builder, int& itemRules)
{
    switch (node.kind) {
    case Node::Kind::Object: {
        MORPHEUS_REQUIRE(node.requiredKeys == node.keys.size() && !node.open,
                         "FromSchema: an object with optional keys or extra keys reached the grammar writer");
        // An object without keys is the empty object (Object needs at least one field).
        if (node.keys.empty()) {
            return Literal("{}");
        }
        std::vector<ObjectField> fields;
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            fields.push_back({node.keys[i], Expression(node.children[i], builder, itemRules)});
        }
        return Object(fields);
    }
    case Node::Kind::Enum: {
        // Strings as JSON strings (Enum); integers as their decimal text.
        if (node.values[0].is_string()) {
            std::vector<std::string> strings;
            for (const nlohmann::json& value : node.values) {
                strings.push_back(value.get<std::string>());
            }
            return Enum(strings);
        }
        std::vector<std::string> numbers;
        for (const nlohmann::json& value : node.values) {
            numbers.push_back(Literal(value.dump()));
        }
        return Alternatives(numbers);
    }
    case Node::Kind::Const:
        return Literal(node.values[0].dump());
    case Node::Kind::Integer:
        return IntRange(node.minimum, node.maximum);
    case Node::Kind::Number:
    case Node::Kind::Boolean:
        throw core::ContractError("FromSchema: a number or boolean schema reached the grammar writer");
    case Node::Kind::String:
        MORPHEUS_REQUIRE(node.maxCount != UNBOUNDED, "FromSchema: an unbounded string reached the grammar writer");
        return JsonString(node.minCount, node.maxCount);
    case Node::Kind::Array: {
        MORPHEUS_REQUIRE(node.maxCount != UNBOUNDED, "FromSchema: an unbounded array reached the grammar writer");
        std::string item = Expression(node.children[0], builder, itemRules);
        const std::string name = builder.Rule(std::format("items-{}", itemRules++), std::move(item));
        return ListOf(name, node.minCount, node.maxCount);
    }
    case Node::Kind::OneOf: {
        std::vector<std::string> branches;
        for (const Node& branch : node.children) {
            branches.push_back(Expression(branch, builder, itemRules));
        }
        return Alternatives(branches);
    }
    }
    throw core::ContractError("FromSchema: unhandled schema kind");
}

// Adds to `errors` every way `value`, found at `path` inside the whole value, breaks `node`, in document order: an
// object's keys in the schema's order, then the keys it does not allow. Recursive, like Compile.
void CheckValue(const Node& node, const nlohmann::json& value, const std::string& path,
                std::vector<ValidationError>& errors)
{
    auto add = [&](const std::string& at, std::string message) { errors.push_back({at, std::move(message)}); };
    // A value as messages show it: a string in single quotes, anything else as compact JSON, both cut short.
    auto shown = [](const nlohmann::json& shownValue) {
        if (shownValue.is_string()) {
            const std::string& text = shownValue.get_ref<const std::string&>();
            const std::string cut = core::CutUtf8(text, core::SHORT_JSON_CHARS);
            return "'" + cut + (cut.size() < text.size() ? "...'" : "'");
        }
        // The replace handler keeps a string that is not UTF-8 from throwing: the value is data.
        std::string text = shownValue.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace);
        if (text.size() > core::SHORT_JSON_CHARS) {
            text.resize(core::SHORT_JSON_CHARS);
            text += "...";
        }
        return text;
    };
    // The allowed values as messages list them: JSON, comma-separated.
    auto listed = [](const std::vector<nlohmann::json>& listedValues) {
        std::string text;
        for (const nlohmann::json& listedValue : listedValues) {
            text += (text.empty() ? "" : ", ") + listedValue.dump();
        }
        return text;
    };
    // Keys as messages list them: 'a', 'b'.
    auto keyList = [](const std::vector<std::string>& keys, std::size_t count) {
        std::string text;
        for (std::size_t i = 0; i < count; ++i) {
            text += (text.empty() ? "'" : ", '") + keys[i] + "'";
        }
        return text;
    };
    auto same = [](const nlohmann::json& a, const nlohmann::json& b) { return SameValue(a, b); };

    switch (node.kind) {
    case Node::Kind::Object: {
        if (!value.is_object()) {
            // The shape expected: empty, the required keys, or any object when none is required.
            if (node.keys.empty() && !node.open) {
                add(path, "expected an empty object");
            } else if (node.requiredKeys == 0) {
                add(path, "expected an object");
            } else {
                add(path, "expected an object with the keys " + keyList(node.keys, node.requiredKeys));
            }
            break;
        }
        // The schema's keys first, in its order: a required key must be present; every present key is checked.
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            const std::string at = path + "/" + PointerToken(node.keys[i]);
            if (!value.contains(node.keys[i])) {
                if (i < node.requiredKeys) {
                    add(at, std::format("missing key '{}'", node.keys[i]));
                }
            } else {
                CheckValue(node.children[i], value.at(node.keys[i]), at, errors);
            }
        }
        // Then every key the schema does not allow, unless the object is open.
        if (node.open) {
            break;
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (std::find(node.keys.begin(), node.keys.end(), it.key()) == node.keys.end()) {
                add(path + "/" + PointerToken(it.key()),
                    std::format("unexpected key '{}'; {}", it.key(),
                                node.keys.empty() ? "the object must be empty"
                                                  : "the keys are " + keyList(node.keys, node.keys.size())));
            }
        }
        break;
    }
    case Node::Kind::Enum: {
        const bool allowed = std::any_of(node.values.begin(), node.values.end(),
                                         [&](const nlohmann::json& allowedValue) { return same(allowedValue, value); });
        if (!allowed) {
            add(path, std::format("{} is not one of: {}", shown(value), listed(node.values)));
        }
        break;
    }
    case Node::Kind::Const:
        if (!same(node.values[0], value)) {
            add(path, std::format("expected {}, got {}", node.values[0].dump(), shown(value)));
        }
        break;
    case Node::Kind::Integer:
        // JsonIntInRange refuses floats and bools, and unsigned values beyond long long.
        if (!core::JsonIntInRange(value, node.minimum, node.maximum)) {
            add(path, std::format("expected an integer from {} to {}, got {}", node.minimum, node.maximum, shown(value)));
        }
        break;
    case Node::Kind::Number: {
        // The bounds the message names: both, one, or none.
        const bool hasMinimum = node.lowest != NO_MINIMUM;
        const bool hasMaximum = node.highest != NO_MAXIMUM;
        std::string range;
        if (hasMinimum && hasMaximum) {
            range = std::format(" from {} to {}", node.lowest, node.highest);
        } else if (hasMinimum) {
            range = std::format(" of at least {}", node.lowest);
        } else if (hasMaximum) {
            range = std::format(" of at most {}", node.highest);
        }
        // is_number is false for a bool. Every JSON number compares as a double.
        if (!value.is_number() || value.get<double>() < node.lowest || value.get<double>() > node.highest) {
            add(path, std::format("expected a number{}, got {}", range, shown(value)));
        }
        break;
    }
    case Node::Kind::Boolean:
        if (!value.is_boolean()) {
            add(path, std::format("expected a boolean, got {}", shown(value)));
        }
        break;
    case Node::Kind::String: {
        const bool bounded = node.maxCount != UNBOUNDED;
        if (!value.is_string()) {
            // The lengths the message names, as the schema gives them.
            std::string lengths;
            if (bounded) {
                lengths = node.minCount == 0 ? std::format(" of at most {} characters", node.maxCount)
                                             : std::format(" of {} to {} characters", node.minCount, node.maxCount);
            } else if (node.minCount > 0) {
                lengths = std::format(" of at least {} characters", node.minCount);
            }
            add(path, std::format("expected a string{}, got {}", lengths, shown(value)));
            break;
        }
        // Lengths count characters (code points), as the grammar's `char` does for compact JSON text.
        const std::size_t length = core::Utf8Length(value.get_ref<const std::string&>());
        if (bounded && length > static_cast<std::size_t>(node.maxCount)) {
            add(path, std::format("longer than {} characters", node.maxCount));
        } else if (length < static_cast<std::size_t>(node.minCount)) {
            add(path, std::format("shorter than {} characters", node.minCount));
        }
        break;
    }
    case Node::Kind::Array: {
        const bool bounded = node.maxCount != UNBOUNDED;
        if (!value.is_array()) {
            if (bounded) {
                add(path, std::format("expected an array of {} to {} items, got {}", node.minCount, node.maxCount, shown(value)));
            } else if (node.minCount > 0) {
                add(path, std::format("expected an array of at least {} items, got {}", node.minCount, shown(value)));
            } else {
                add(path, std::format("expected an array, got {}", shown(value)));
            }
            break;
        }
        if (bounded && (value.size() < static_cast<std::size_t>(node.minCount) ||
                        value.size() > static_cast<std::size_t>(node.maxCount))) {
            add(path, std::format("has {} items; expected {} to {}", value.size(), node.minCount, node.maxCount));
        } else if (!bounded && value.size() < static_cast<std::size_t>(node.minCount)) {
            add(path, std::format("has {} items; expected at least {}", value.size(), node.minCount));
        }
        // Every item is checked, even past the limit.
        for (std::size_t i = 0; i < value.size(); ++i) {
            CheckValue(node.children[0], value[i], std::format("{}/{}", path, i), errors);
        }
        break;
    }
    case Node::Kind::OneOf: {
        const std::string& key = node.children[0].keys[0];
        std::vector<nlohmann::json> tags;
        for (const Node& branch : node.children) {
            tags.push_back(branch.children[0].values[0]);
        }
        if (!value.is_object()) {
            add(path, std::format("expected an object whose '{}' is one of: {}", key, listed(tags)));
            break;
        }
        const std::string at = path + "/" + PointerToken(key);
        if (!value.contains(key)) {
            add(at, std::format("missing key '{}'", key));
            break;
        }
        // The discriminator picks the branch; the value is then checked against that branch alone.
        const nlohmann::json& tag = value.at(key);
        const auto chosen = std::find_if(node.children.begin(), node.children.end(), [&](const Node& candidate) {
            return same(candidate.children[0].values[0], tag);
        });
        if (chosen == node.children.end()) {
            add(at, std::format("unknown {} {}; expected one of: {}", key, shown(tag), listed(tags)));
            break;
        }
        CheckValue(*chosen, value, path, errors);
        break;
    }
    }
}

} // namespace

std::string FromSchema(const nlohmann::json& schema)
{
    // Check the whole schema first, then write it: a bad schema never yields part of a grammar.
    const Node root = Compile(schema, "", Use::Grammar);
    GrammarBuilder builder;
    int itemRules = 0; // The item rules named so far.
    const std::string rootBody = Expression(root, builder, itemRules);
    return builder.Text(rootBody);
}

std::vector<ValidationError> Validate(const nlohmann::json& value, const nlohmann::json& schema)
{
    // A schema outside the extended subset is the host's data error.
    const Node root = Compile(schema, "", Use::Validation);
    std::vector<ValidationError> errors;
    CheckValue(root, value, "", errors);
    return errors;
}

void CheckValidationSchema(const nlohmann::json& schema)
{
    // Compiling is the whole check: it throws for anything outside the extended subset.
    Compile(schema, "", Use::Validation);
}

} // namespace morpheus::grammar
```

- [ ] **Step 4: Run the grammar tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[schema]"`
Expected: builds with no warnings; `All tests passed`.

- [ ] **Step 5: Run the whole fast suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 434` (427 + 7), the three `[server]` tests "Not Run (Skipped)".

- [ ] **Step 6: Byte check and commit**

```bash
LC_ALL=C grep -c $'[\x80-\xFF]' src/grammar/Schema.h src/grammar/Schema.cpp tests/grammar/ValidateExtendedTests.cpp tests/grammar/SchemaTests.cpp
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' src/grammar/Schema.h src/grammar/Schema.cpp tests/grammar/ValidateExtendedTests.cpp tests/grammar/SchemaTests.cpp
git add src/grammar/Schema.h src/grammar/Schema.cpp tests/grammar/ValidateExtendedTests.cpp tests/grammar/SchemaTests.cpp
git commit -m "grammar::Validate reads an extended subset for validation-only schemas; add CheckValidationSchema" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

### Task 2: RunStore schema version 3: run kind, hosted tables, the upgrade

The store learns hosted runs: `run_meta` gains `kind`; a hosted run keeps its host data (with the content hash in `world_hash`), its conversations, its turns and the events recorded between them. A version 2 file is upgraded in place on open; any other version is refused as today.

**Files:**
- Create: `src/core/HostedRecord.h`
- Modify: `src/store/RunStore.h`, `src/store/RunStore.cpp`
- Create: `tests/store/HostedStoreTests.cpp`
- Modify: `tests/store/RunStoreTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces (`core/HostedRecord.h`, namespace `morpheus::core`): `struct Participant { std::string id; std::string name; std::string bio; }`; `struct Conversation { std::string id; std::vector<Participant> participants; bool ended = false; int foldedThrough = 0; }`; `struct ConversationEvent { int afterTurn = 0; nlohmann::json event; }`; `struct Adjudication { bool requested = false; std::string actionId; nlohmann::json params = object; std::vector<std::string> reasons; }`; `struct HostedTurn { int number; std::string conversationId; std::string speakerId; std::vector<std::string> listenerIds; std::optional<std::string> playerText; std::vector<nlohmann::json> events; nlohmann::json observation; std::string resolveOutput; Adjudication adjudication; std::string line; nlohmann::json timings; }`.
- Produces (`store/RunStore.h`): `enum class store::RunKind { Owned, Hosted }`; `RunMeta::kind` (default `Owned`); `RunStore::SCHEMA_VERSION == 3`; `static RunStore CreateHosted(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& hostData)`; `nlohmann::json HostData()`; `void ReplaceHostData(const nlohmann::json& hostData, const std::string& hash)`; `std::string CreateConversation(const std::vector<core::Participant>& participants)` (ids `c1`, `c2`, ...); `std::optional<core::Conversation> Conversation(std::string_view id)`; `std::vector<core::Conversation> Conversations()` (creation order); `void SetParticipants(std::string_view id, const std::vector<core::Participant>& participants)`; `void EndConversation(std::string_view id)`; `void FoldConversation(std::string_view id, const core::Summary& summary)`; `void AddConversationEvents(std::string_view id, int afterTurn, const std::vector<nlohmann::json>& events)`; `std::vector<core::ConversationEvent> ConversationEvents(std::string_view id)`; `int LatestHostedTurn()`; `void CommitHostedTurn(const core::HostedTurn& record)`; `std::vector<core::HostedTurn> HostedTurns(std::string_view conversationId)`. Every hosted method throws `ContractError` on an owned run.

- [ ] **Step 1: Write the failing tests**

Create `tests/store/HostedStoreTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "core/TurnRecord.h"
#include "store/RunStore.h"
#include "store/Sqlite.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

// The schema of a version 2 run exactly as RunStore wrote it before version 3.
constexpr const char* V2_SCHEMA_SQL = R"sql(
CREATE TABLE run_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE turns (
    number    INTEGER PRIMARY KEY,
    attempt   INTEGER NOT NULL,
    input     TEXT NOT NULL,
    resolve   TEXT NOT NULL,
    checks    TEXT NOT NULL,
    applied   TEXT NOT NULL,
    rejected  TEXT NOT NULL,
    narration TEXT NOT NULL,
    digest    TEXT NOT NULL,
    timings   TEXT NOT NULL
);
CREATE TABLE state_snapshots (
    turn  INTEGER PRIMARY KEY,
    state TEXT NOT NULL
);
CREATE TABLE summaries (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    from_turn INTEGER NOT NULL,
    to_turn   INTEGER NOT NULL,
    text      TEXT NOT NULL
);
CREATE TABLE notes (
    id   INTEGER PRIMARY KEY AUTOINCREMENT,
    turn INTEGER NOT NULL,
    text TEXT NOT NULL
);
CREATE TABLE embeddings (
    kind         TEXT NOT NULL,
    ref_id       TEXT NOT NULL,
    model        TEXT NOT NULL,
    vector       BLOB NOT NULL,
    content_hash TEXT NOT NULL,
    PRIMARY KEY (kind, ref_id, model)
);
)sql";

store::RunMeta HostedMeta()
{
    store::RunMeta meta;
    meta.kind = store::RunKind::Hosted;
    meta.worldId = "hosted";
    meta.worldHash = "00000000000000aa";
    meta.modelId = "test-model";
    meta.rngSeed = 7;
    meta.settings = {{"context_size", 4096}};
    return meta;
}

nlohmann::json TestHostData()
{
    return nlohmann::json::parse(R"({"catalog": {"actions": [{"id": "none"}]},
        "observation_schema": {"type": "object", "properties": {}, "required": [], "additionalProperties": true},
        "lore": [{"subject": "inn", "data": "A warm inn.", "always_on": true}]})");
}

std::vector<core::Participant> TwoParticipants()
{
    return {{"npc-1", "Hulda", "Keeps the inn."}, {"player", "Aela", ""}};
}

core::HostedTurn TestHostedTurn(int number, const std::string& conversationId)
{
    core::HostedTurn turn;
    turn.number = number;
    turn.conversationId = conversationId;
    turn.speakerId = "npc-1";
    turn.listenerIds = {"player"};
    turn.playerText = "Follow me.";
    turn.events = {nlohmann::json{{"type", "ambient"}, {"text", "Rain starts."}}};
    turn.observation = {{"actors", nlohmann::json::array()}};
    turn.resolveOutput = R"({"action":"none","params":{}})";
    turn.adjudication = {true, "none", nlohmann::json::object(), {}};
    turn.line = std::format("Line {}.", number);
    turn.timings = {{"resolve", {{"prompt_tokens", 10}}}};
    return turn;
}

} // namespace

TEST_CASE("A hosted run stores its kind, host data, conversations, events and turns, and reopens with them",
          "[store][hosted]")
{
    const std::filesystem::path file = test::FreshTempDir("store_hosted") / "run.db";
    std::string id;
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        CHECK(store.Meta().kind == store::RunKind::Hosted);
        CHECK(store.LatestHostedTurn() == 0);
        id = store.CreateConversation(TwoParticipants());
        CHECK(id == "c1");
        CHECK(store.CreateConversation({{"npc-2", "Saadia", ""}}) == "c2");
        store.AddConversationEvents(id, 0, {nlohmann::json{{"text", "A door opens."}},
                                            nlohmann::json{{"type", "ambient"}, {"text", "Rain."}}});
        store.CommitHostedTurn(TestHostedTurn(1, id));
        core::HostedTurn quiet = TestHostedTurn(2, id);
        quiet.playerText.reset();
        quiet.adjudication = {false, "", nlohmann::json::object(), {"/intent: unexpected key 'intent'"}};
        store.CommitHostedTurn(quiet);
        store.AddConversationEvents(id, 2, {nlohmann::json{{"text", "Hulda leaves."}}});
    }

    store::RunStore reopened = store::RunStore::Open(file);
    CHECK(reopened.Meta().kind == store::RunKind::Hosted);
    CHECK(reopened.Meta().worldId == "hosted");
    CHECK(reopened.Meta().rngSeed == 7u);
    CHECK(reopened.HostData() == TestHostData());
    CHECK(reopened.LatestHostedTurn() == 2);
    CHECK(reopened.LatestTurn() == 0); // A hosted run plays no owned turns.

    const std::optional<core::Conversation> conversation = reopened.Conversation(id);
    REQUIRE(conversation);
    REQUIRE(conversation->participants.size() == 2);
    CHECK(conversation->participants[0].id == "npc-1");
    CHECK(conversation->participants[0].name == "Hulda");
    CHECK(conversation->participants[0].bio == "Keeps the inn.");
    CHECK(conversation->participants[1].bio.empty());
    CHECK_FALSE(conversation->ended);
    CHECK(conversation->foldedThrough == 0);
    CHECK_FALSE(reopened.Conversation("c9"));
    const std::vector<core::Conversation> all = reopened.Conversations();
    REQUIRE(all.size() == 2);
    CHECK(all[1].id == "c2");

    const std::vector<core::ConversationEvent> events = reopened.ConversationEvents(id);
    REQUIRE(events.size() == 3);
    CHECK(events[0].afterTurn == 0);
    CHECK(events[0].event == nlohmann::json{{"text", "A door opens."}});
    CHECK(events[1].event.at("type") == "ambient");
    CHECK(events[2].afterTurn == 2);
    CHECK(reopened.ConversationEvents("c2").empty());

    const std::vector<core::HostedTurn> turns = reopened.HostedTurns(id);
    REQUIRE(turns.size() == 2);
    const core::HostedTurn expected = TestHostedTurn(1, id);
    CHECK(turns[0].number == 1);
    CHECK(turns[0].conversationId == id);
    CHECK(turns[0].speakerId == "npc-1");
    CHECK(turns[0].listenerIds == std::vector<std::string>{"player"});
    CHECK(turns[0].playerText == std::optional<std::string>("Follow me."));
    CHECK(turns[0].events == expected.events);
    CHECK(turns[0].observation == expected.observation);
    CHECK(turns[0].resolveOutput == expected.resolveOutput);
    CHECK(turns[0].adjudication.requested);
    CHECK(turns[0].adjudication.actionId == "none");
    CHECK(turns[0].adjudication.params == nlohmann::json::object());
    CHECK(turns[0].line == "Line 1.");
    CHECK(turns[0].timings == expected.timings);
    CHECK_FALSE(turns[1].playerText);
    CHECK_FALSE(turns[1].adjudication.requested);
    CHECK(turns[1].adjudication.reasons == std::vector<std::string>{"/intent: unexpected key 'intent'"});
    CHECK(reopened.HostedTurns("c2").empty());
}

TEST_CASE("ReplaceHostData replaces the stored copy and the content hash", "[store][hosted]")
{
    const std::filesystem::path file = test::FreshTempDir("store_hosted_replace") / "run.db";
    nlohmann::json changed = TestHostData();
    changed["catalog"]["actions"].push_back(nlohmann::json{{"id", "follow"}});
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        store.ReplaceHostData(changed, "00000000000000bb");
        CHECK(store.Meta().worldHash == "00000000000000bb");
        CHECK(store.HostData() == changed);
    }
    store::RunStore reopened = store::RunStore::Open(file);
    CHECK(reopened.Meta().worldHash == "00000000000000bb");
    CHECK(reopened.HostData() == changed);
}

TEST_CASE("FoldConversation adds the summary and moves folded_through together; participants and the end are kept",
          "[store][hosted]")
{
    store::RunStore store =
        store::RunStore::CreateHosted(test::FreshTempDir("store_hosted_fold") / "run.db", HostedMeta(), TestHostData());
    const std::string id = store.CreateConversation(TwoParticipants());
    store.CommitHostedTurn(TestHostedTurn(1, id));
    store.CommitHostedTurn(TestHostedTurn(2, id));

    store.FoldConversation(id, {1, 2, "They met."});
    CHECK(store.Conversation(id)->foldedThrough == 2);
    const std::vector<core::Summary> summaries = store.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 2);
    CHECK(summaries[0].text == "They met.");
    CHECK_THROWS_AS(store.FoldConversation(id, {3, 2, "Backwards."}), core::ContractError);
    CHECK(store.Summaries().size() == 1);

    store.SetParticipants(id, {{"player", "Aela", ""}, {"npc-2", "Saadia", "Serves at the inn."}});
    const std::optional<core::Conversation> changed = store.Conversation(id);
    REQUIRE(changed);
    REQUIRE(changed->participants.size() == 2);
    CHECK(changed->participants[0].id == "player");
    CHECK(changed->participants[1].bio == "Serves at the inn.");
    CHECK_FALSE(changed->ended);
    store.EndConversation(id);
    CHECK(store.Conversation(id)->ended);
}

TEST_CASE("A version 2 run is upgraded in place to an owned version 3 run, its data unchanged", "[store][hosted]")
{
    const std::filesystem::path file = test::FreshTempDir("store_upgrade") / "run.db";
    {
        store::Database db = store::Database::Open(file, true);
        db.Exec(V2_SCHEMA_SQL);
        const std::vector<std::pair<std::string, std::string>> meta = {
            {"schema_version", "2"},  {"world_id", "mini"}, {"world_hash", "0123456789abcdef"},
            {"model_id", "old-model"}, {"rng_seed", "42"},  {"settings", R"({"context_size":8192})"},
            {"created_at", "2026-11-01T10:00:00Z"}};
        for (const auto& [key, value] : meta) {
            db.Prepare("INSERT INTO run_meta (key, value) VALUES (?1, ?2)").Bind(1, key).Bind(2, value).Run();
        }
        db.Exec(R"sql(
INSERT INTO state_snapshots (turn, state) VALUES (0, '{"player":{"type":"character","fields":{"health":10}}}');
INSERT INTO state_snapshots (turn, state) VALUES (1, '{"player":{"type":"character","fields":{"health":9}}}');
INSERT INTO turns (number, attempt, input, resolve, checks, applied, rejected, narration, digest, timings)
    VALUES (1, 0, 'I climb.', '{"intent":"climb"}', '[]', '[]', '[]', 'You climb.', 'The player climbed.', '{}');
INSERT INTO summaries (from_turn, to_turn, text) VALUES (1, 1, 'A climb.');
INSERT INTO notes (turn, text) VALUES (1, 'The rope is frayed.');
)sql");
    }

    {
        store::RunStore store = store::RunStore::Open(file);
        CHECK(store.Meta().kind == store::RunKind::Owned);
        CHECK(store.Meta().worldId == "mini");
        CHECK(store.Meta().worldHash == "0123456789abcdef");
        CHECK(store.Meta().rngSeed == 42u);
        CHECK(store.LatestTurn() == 1);
        const std::optional<core::TurnRecord> turn = store.Turn(1);
        REQUIRE(turn);
        CHECK(turn->input == "I climb.");
        CHECK(turn->narration == "You climb.");
        CHECK(turn->digest == "The player climbed.");
        CHECK(turn->notes == std::vector<std::string>{"The rope is frayed."});
        CHECK(store.StateAt(1).at("player").at("fields").at("health") == 9);
        const std::vector<core::Summary> summaries = store.Summaries();
        REQUIRE(summaries.size() == 1);
        CHECK(summaries[0].text == "A climb.");
    }

    // The file now says version 3 and owned, has the hosted tables, and opens again as it is.
    {
        store::Database db = store::Database::Open(file, false);
        store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
        REQUIRE(version.Step());
        CHECK(version.ColumnText(0) == "3");
        store::Statement kind = db.Prepare("SELECT value FROM run_meta WHERE key = 'kind'");
        REQUIRE(kind.Step());
        CHECK(kind.ColumnText(0) == "owned");
        store::Statement conversations = db.Prepare("SELECT COUNT(*) FROM conversations");
        REQUIRE(conversations.Step());
        CHECK(conversations.ColumnInt(0) == 0);
    }
    CHECK(store::RunStore::Open(file).LatestTurn() == 1);

    // A kind other than owned or hosted is damaged meta.
    {
        store::Database db = store::Database::Open(file, false);
        db.Exec("UPDATE run_meta SET value = 'alien' WHERE key = 'kind'");
    }
    CHECK_THROWS_AS(store::RunStore::Open(file), store::StoreError);
    CHECK_THROWS_WITH(store::RunStore::Open(file), ContainsSubstring("run_meta 'kind'"));
}

TEST_CASE("Hosted methods refuse an owned run, a turn out of order and an unknown conversation", "[store][hosted]")
{
    const std::filesystem::path dir = test::FreshTempDir("store_hosted_refuse");
    store::RunMeta owned = HostedMeta();
    owned.kind = store::RunKind::Owned;
    owned.worldId = "mini";

    // Each kind is created by its own method.
    CHECK_THROWS_AS(store::RunStore::Create(dir / "x.db", HostedMeta(), nlohmann::json::object()), core::ContractError);
    CHECK_THROWS_AS(store::RunStore::CreateHosted(dir / "y.db", owned, TestHostData()), core::ContractError);
    CHECK_FALSE(std::filesystem::exists(dir / "x.db"));
    CHECK_FALSE(std::filesystem::exists(dir / "y.db"));

    store::RunStore ownedStore = store::RunStore::Create(dir / "owned.db", owned, nlohmann::json::object());
    CHECK(ownedStore.Meta().kind == store::RunKind::Owned);
    CHECK_THROWS_AS(ownedStore.CreateConversation(TwoParticipants()), core::ContractError);
    CHECK_THROWS_AS(ownedStore.HostData(), core::ContractError);
    CHECK_THROWS_AS(ownedStore.LatestHostedTurn(), core::ContractError);

    store::RunStore hosted = store::RunStore::CreateHosted(dir / "hosted.db", HostedMeta(), TestHostData());
    const std::string id = hosted.CreateConversation(TwoParticipants());
    CHECK_THROWS_AS(hosted.CommitHostedTurn(TestHostedTurn(2, id)), core::ContractError);   // Not the next number.
    CHECK_THROWS_AS(hosted.CommitHostedTurn(TestHostedTurn(1, "c9")), core::ContractError); // No such conversation.
    CHECK_THROWS_AS(hosted.CreateConversation({}), core::ContractError);
    CHECK_THROWS_AS(hosted.EndConversation("c9"), core::ContractError);
    CHECK_THROWS_AS(hosted.SetParticipants("c9", TwoParticipants()), core::ContractError);
    CHECK_THROWS_AS(hosted.AddConversationEvents("c9", 0, {nlohmann::json{{"text", "x"}}}), core::ContractError);
    CHECK(hosted.LatestHostedTurn() == 0);
}
```

In `tests/store/RunStoreTests.cpp`, replace the whole test case `RunStore refuses a version-1 run, naming the file and both versions` with:

```cpp
TEST_CASE("RunStore refuses a version-1 run, naming the file and both versions", "[store]")
{
    CHECK(RunStore::SCHEMA_VERSION == 3);
    const std::filesystem::path file = FreshDbPath("store_version_1");
    {
        RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
    }
    {
        Database db = Database::Open(file, false);
        db.Exec("UPDATE run_meta SET value = '1' WHERE key = 'schema_version'");
    }
    CHECK_THROWS_AS(RunStore::Open(file), StoreError);
    CHECK_THROWS_WITH(RunStore::Open(file), ContainsSubstring("schema version 1 is not the supported version 3"));
    CHECK_THROWS_WITH(RunStore::Open(file), ContainsSubstring("run.db"));
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors, among them `Cannot open include file: 'core/HostedRecord.h'` in `tests/store/HostedStoreTests.cpp`.

- [ ] **Step 3: Add the hosted record types**

Create `src/core/HostedRecord.h`:

```cpp
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace morpheus::core {

// One participant of a hosted conversation, as it joined: the actor's id, with its name and bio from the observation
// it joined with. The participants layer of a hosted prompt shows them in join order.
struct Participant {
    std::string id = {};
    std::string name = {};
    std::string bio = {}; // Empty when the observation gave none.
};

// One conversation of a hosted run (hosted-world spec, section 5).
struct Conversation {
    std::string id = {};
    std::vector<Participant> participants = {}; // In join order.
    bool ended = false;
    int foldedThrough = 0; // This conversation's turns up to this number are folded into summaries; 0 when none are.
};

// An event recorded between turns (turn::HostedSession::RecordEvents): the host's JSON object, with its `text`.
struct ConversationEvent {
    int afterTurn = 0; // The conversation's latest turn when it was recorded; 0 before its first.
    nlohmann::json event = nlohmann::json::object();
};

// What became of the speaker's proposed action (turn::HostedWorld::Adjudicate). A requested action is the host's to
// carry out; its outcome comes back later as an event. A refused one has the parse or validation errors as reasons,
// and the speaker takes no action.
struct Adjudication {
    bool requested = false;
    std::string actionId = {};
    nlohmann::json params = nlohmann::json::object();
    std::vector<std::string> reasons = {};
};

// One committed hosted turn (hosted-world spec, section 2, step 4). Stored by store::RunStore; rendered into a hosted
// prompt's L3 by turn::HostedSession.
struct HostedTurn {
    int number = 0; // Run-wide, shared with summaries' turn ranges.
    std::string conversationId = {};
    std::string speakerId = {};
    std::vector<std::string> listenerIds = {};       // The other participants present, in join order.
    std::optional<std::string> playerText = {};      // Absent for a turn between NPCs.
    std::vector<nlohmann::json> events = {};         // The events this turn's request added, in order.
    nlohmann::json observation = nlohmann::json::object();
    std::string resolveOutput = {};                  // The resolve call's text exactly as the model wrote it.
    Adjudication adjudication = {};
    std::string line = {};                           // The spoken line, trimmed.
    nlohmann::json timings = nlohmann::json::object();
};

} // namespace morpheus::core
```

- [ ] **Step 4: Move the store to version 3**

Replace the whole of `src/store/RunStore.h` with:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/HostedRecord.h"
#include "core/TurnRecord.h"
#include "store/Sqlite.h"

namespace morpheus::store {

// Owned: Morpheus's own game, with its world, state and turns. Hosted: a world the host owns and sends as data
// (turn::HostedSession), with conversations and hosted turns.
enum class RunKind { Owned, Hosted };

struct RunMeta {
    std::string worldId = {};
    std::string worldHash = {}; // World content hash at run creation; a hosted run's host data hash, kept current.
    std::string modelId = {};
    std::uint64_t rngSeed = 0;
    nlohmann::json settings = nlohmann::json::object();
    std::string createdAt = {}; // ISO-8601 UTC, set by RunStore::Create.
    RunKind kind = RunKind::Owned;
};

enum class EmbeddingKind { Lore, Turn, Note };

// refId: lore subject for Lore, decimal turn number for Turn, "<turn>.<index>" for Note (index: the note's
// position among that turn's notes).
struct EmbeddingRow {
    EmbeddingKind kind = EmbeddingKind::Lore;
    std::string refId = {};
    std::string model = {};
    std::vector<float> vector = {};
    std::string contentHash = {}; // Hash of the exact text that was embedded. Callers re-embed when it differs
                                  // from the hash of the current text, so edited lore never keeps a stale vector.
};

// One run's SQLite database (saves/<world>/<run>.db). The only writer of run data.
// Tables: run_meta, turns, state_snapshots, summaries, notes, embeddings, and for hosted runs host_data,
// conversations, hosted_turns, conversation_events. Every file has every table; summaries and embeddings serve both
// kinds, and turn numbers run across the whole run.
class RunStore {
public:
    // 3: run_meta.kind and the hosted tables. 2: turns.digest (it also covers turns.attempt). A version 2 run is
    // upgraded in place on open; any other version is refused.
    static constexpr int SCHEMA_VERSION = 3;

private:
    Database _db;
    RunMeta _meta = {};
    std::filesystem::path _file = {}; // Only used to name the file in error messages.

public:
    // Creates a new owned run database holding the meta and the turn-0 snapshot. Throws StoreError if the file
    // exists, and ContractError unless meta.kind is Owned. If creation fails part way, the partial file is removed.
    static RunStore Create(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& initialState);

    // Creates a new hosted run database holding the meta and the host data (turn::HostDataToJson). Throws StoreError
    // if the file exists, and ContractError unless meta.kind is Hosted. If creation fails part way, the partial file
    // is removed.
    static RunStore CreateHosted(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& hostData);

    // Opens an existing run. A version 2 file is first upgraded in place to version 3 (the hosted tables are added
    // and it is marked owned, in one transaction; its data is not touched). Throws StoreError if it is missing or was
    // written with another schema version, or its meta (rng_seed, settings, kind) is damaged.
    static RunStore Open(const std::filesystem::path& file);

    const RunMeta& Meta() const;

    // Highest committed turn number, 0 before the first turn.
    int LatestTurn();

    // State after `turn` (0 = initial state). Throws ContractError if there is no snapshot for it
    // and StoreError if the stored snapshot is not valid JSON.
    nlohmann::json StateAt(int turn);

    // Appends turn LatestTurn()+1, the state it produced and the notes it wrote (record.notes, stored with
    // turn = record.number), in one transaction: either all of it commits or none of it does.
    // Throws ContractError if record.number is not LatestTurn()+1.
    void CommitTurn(const core::TurnRecord& record, const nlohmann::json& stateAfter);

    // nullopt when the turn does not exist. record.notes holds that turn's notes in the order they were written.
    // Throws StoreError if a stored column is not valid JSON or has the wrong shape, or a note has no matching
    // turn row.
    std::optional<core::TurnRecord> Turn(int number);

    // Turns in [fromTurn, toTurn], ascending, each with its notes. Throws StoreError if a stored column is not
    // valid JSON or has the wrong shape, or a note has no matching turn row.
    std::vector<core::TurnRecord> Turns(int fromTurn, int toTurn);

    // Undo: removes every turn, snapshot and note after `turn`, every turn and note embedding whose turn is after it,
    // and every summary that ends after it. Throws ContractError if turn < 0.
    void TruncateAfter(int turn);

    // Throws ContractError unless 1 <= fromTurn <= toTurn.
    void AddSummary(const core::Summary& summary);

    // Condensing: replaces every summary with `merged` in one transaction. Throws ContractError unless
    // 1 <= merged.fromTurn <= merged.toTurn.
    void ReplaceSummaries(const core::Summary& merged);

    // Ordered by fromTurn.
    std::vector<core::Summary> Summaries();

    // Every committed note, ordered by turn, then the order written. For context assembly in assisted mode.
    std::vector<core::Note> Notes();

    // Inserts or replaces the vector and content hash for (kind, refId, model).
    // Throws ContractError on an empty vector or an empty contentHash.
    void PutEmbedding(const EmbeddingRow& row);

    // Every embedding of one kind produced by one embedding model, with the content hash it was stored with.
    // Ordered by the number refId starts with, then refId.
    std::vector<EmbeddingRow> Embeddings(EmbeddingKind kind, const std::string& model);

    // Hosted runs only: every method below throws ContractError on an owned run.

    // The host data as stored. Throws StoreError when it is missing or not valid JSON.
    nlohmann::json HostData();

    // A mod update: replaces the stored host data and its content hash (run_meta world_hash, and Meta().worldHash)
    // together. Throws ContractError for an empty hash.
    void ReplaceHostData(const nlohmann::json& hostData, const std::string& hash);

    // Starts a conversation with `participants` in join order and returns its id: "c1", "c2", ... in creation order.
    // Throws ContractError for no participants.
    std::string CreateConversation(const std::vector<core::Participant>& participants);

    // nullopt when there is no conversation `id`. Throws StoreError when its participants column is damaged.
    std::optional<core::Conversation> Conversation(std::string_view id);

    // Every conversation, in creation order. Throws StoreError when a participants column is damaged.
    std::vector<core::Conversation> Conversations();

    // Replaces the participants of conversation `id`. Throws ContractError for an unknown conversation or none.
    void SetParticipants(std::string_view id, const std::vector<core::Participant>& participants);

    // Marks conversation `id` ended. Throws ContractError for an unknown conversation.
    void EndConversation(std::string_view id);

    // Adds `summary` and sets conversation `id`'s folded_through to summary.toTurn, in one transaction. Throws
    // ContractError for an unknown conversation or unless 1 <= fromTurn <= toTurn.
    void FoldConversation(std::string_view id, const core::Summary& summary);

    // Appends `events` to conversation `id`, in order, each with `afterTurn` (the conversation's latest turn, 0 before
    // its first), in one transaction. Throws ContractError for an unknown conversation.
    void AddConversationEvents(std::string_view id, int afterTurn, const std::vector<nlohmann::json>& events);

    // Conversation `id`'s events in the order they were recorded. Throws StoreError when one is not valid JSON.
    std::vector<core::ConversationEvent> ConversationEvents(std::string_view id);

    // Highest committed hosted turn number, 0 before the first.
    int LatestHostedTurn();

    // Appends hosted turn LatestHostedTurn()+1. Throws ContractError if record.number is not that, or its
    // conversation is unknown.
    void CommitHostedTurn(const core::HostedTurn& record);

    // Conversation `conversationId`'s turns, ascending. Throws StoreError if a stored column is not valid JSON or has
    // the wrong shape.
    std::vector<core::HostedTurn> HostedTurns(std::string_view conversationId);

private:
    RunStore(Database db, RunMeta meta, std::filesystem::path file);

    // Create and CreateHosted: makes the file, its whole schema and the meta rows, then runs `fill` in the same
    // transaction. A failure part way removes the file.
    static RunStore CreateWith(const std::filesystem::path& file, RunMeta meta,
                               const std::function<void(Database& db)>& fill);

    // Throws ContractError naming `method` unless this is a hosted run.
    void RequireHosted(const char* method) const;
};

} // namespace morpheus::store
```

In `src/store/RunStore.cpp`, make these changes.

1. Replace the include block

```cpp
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <map>
#include <utility>
```

with

```cpp
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <map>
#include <utility>
#include <vector>
```

2. After the closing `)sql";` of `SCHEMA_SQL` (and before `// Stored spelling of an embedding kind.`), insert:

```cpp

// The tables version 3 adds: a hosted run's host data, its conversations, its turns and the events recorded between
// them. Created in every file, so a version 2 file is upgraded by adding them.
constexpr const char* HOSTED_SCHEMA_SQL = R"sql(
CREATE TABLE host_data (
    id   INTEGER PRIMARY KEY CHECK (id = 1),
    data TEXT NOT NULL
);
CREATE TABLE conversations (
    id             TEXT PRIMARY KEY,
    position       INTEGER NOT NULL UNIQUE,
    participants   TEXT NOT NULL,
    ended          INTEGER NOT NULL,
    folded_through INTEGER NOT NULL
);
CREATE TABLE hosted_turns (
    number          INTEGER PRIMARY KEY,
    conversation_id TEXT NOT NULL,
    speaker_id      TEXT NOT NULL,
    listener_ids    TEXT NOT NULL,
    player_text     TEXT NOT NULL,
    events          TEXT NOT NULL,
    observation     TEXT NOT NULL,
    resolve_output  TEXT NOT NULL,
    adjudication    TEXT NOT NULL,
    line            TEXT NOT NULL,
    timings         TEXT NOT NULL
);
CREATE TABLE conversation_events (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    conversation_id TEXT NOT NULL,
    after_turn      INTEGER NOT NULL,
    event           TEXT NOT NULL
);
)sql";

// Stored spelling of a run kind, run_meta 'kind'.
const char* RunKindName(RunKind kind)
{
    switch (kind) {
    case RunKind::Owned:
        return "owned";
    case RunKind::Hosted:
        return "hosted";
    }
    throw morpheus::core::ContractError("RunKindName: unhandled RunKind");
}

// A conversation's participants as stored: [{"id": ..., "name": ..., "bio": ...}] in join order.
nlohmann::json ParticipantsToJson(const std::vector<core::Participant>& participants)
{
    nlohmann::json json = nlohmann::json::array();
    for (const core::Participant& participant : participants) {
        json.push_back({{"id", participant.id}, {"name", participant.name}, {"bio", participant.bio}});
    }
    return json;
}
```

3. Replace the whole function `RunStore RunStore::Create(...)` (from `RunStore RunStore::Create(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& initialState)` to its closing brace) with:

```cpp
RunStore RunStore::Create(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& initialState)
{
    MORPHEUS_REQUIRE(meta.kind == RunKind::Owned, "RunStore::Create: a hosted run is created with CreateHosted");
    // An owned run starts from its world's initial state.
    return CreateWith(file, std::move(meta), [&initialState](Database& db) {
        db.Prepare("INSERT INTO state_snapshots (turn, state) VALUES (0, ?1)").Bind(1, initialState.dump()).Run();
    });
}

RunStore RunStore::CreateHosted(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& hostData)
{
    MORPHEUS_REQUIRE(meta.kind == RunKind::Hosted, "RunStore::CreateHosted: an owned run is created with Create");
    // A hosted run has no state: it starts with the host's data.
    return CreateWith(file, std::move(meta), [&hostData](Database& db) {
        db.Prepare("INSERT INTO host_data (id, data) VALUES (1, ?1)").Bind(1, hostData.dump()).Run();
    });
}

RunStore RunStore::CreateWith(const std::filesystem::path& file, RunMeta meta,
                              const std::function<void(Database& db)>& fill)
{
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error) {
        throw StoreError(std::format("cannot check '{}': {}", core::PathText(file), error.message()));
    }
    if (exists) {
        throw StoreError(std::format("run database '{}' already exists", core::PathText(file)));
    }
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), error);
        if (error) {
            throw StoreError(
                std::format("cannot create the folder for '{}': {}", core::PathText(file), error.message()));
        }
    }
    meta.createdAt = std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                                 std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));

    // Opening creates the (empty) file, so any failure from here on must remove it again, or a retry would be
    // blocked by "already exists".
    try {
        // Schema, meta and the kind's first rows land together or not at all.
        Database db = Database::Open(file, true);
        db.Transaction([&] {
            db.Exec(SCHEMA_SQL);
            db.Exec(HOSTED_SCHEMA_SQL);

            const std::array<std::pair<const char*, std::string>, 8> metaRows = {{
                {"schema_version", std::to_string(SCHEMA_VERSION)},
                {"world_id", meta.worldId},
                {"world_hash", meta.worldHash},
                {"model_id", meta.modelId},
                {"rng_seed", std::to_string(meta.rngSeed)}, // Text: SQLite integers are signed 64-bit.
                {"settings", meta.settings.dump()},
                {"created_at", meta.createdAt},
                {"kind", RunKindName(meta.kind)},
            }};
            for (const auto& [key, value] : metaRows) {
                db.Prepare("INSERT INTO run_meta (key, value) VALUES (?1, ?2)").Bind(1, key).Bind(2, value).Run();
            }
            fill(db);
        });
        return RunStore(std::move(db), std::move(meta), file);
    } catch (...) {
        // `db` is already closed here (unwinding destroyed it), which Windows requires before deleting the file.
        // The non-throwing overload keeps the original error as the one that propagates.
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        throw;
    }
}
```

4. In `RunStore::Open`, replace

```cpp
    if (value("schema_version") != std::to_string(SCHEMA_VERSION)) {
        throw StoreError(std::format("'{}': schema version {} is not the supported version {}",
                                     core::PathText(file), value("schema_version"), SCHEMA_VERSION));
    }
```

with

```cpp
    // A version 2 run is upgraded in place: the hosted tables are added and it is marked owned, in one transaction.
    // Its data is not touched. Any other version is refused.
    if (value("schema_version") == "2") {
        db.Transaction([&] {
            db.Exec(HOSTED_SCHEMA_SQL);
            db.Prepare("INSERT INTO run_meta (key, value) VALUES ('kind', ?1)").Bind(1, RunKindName(RunKind::Owned)).Run();
            db.Prepare("UPDATE run_meta SET value = ?1 WHERE key = 'schema_version'")
                .Bind(1, std::to_string(SCHEMA_VERSION))
                .Run();
        });
        values["schema_version"] = std::to_string(SCHEMA_VERSION);
        values["kind"] = RunKindName(RunKind::Owned);
    }
    if (value("schema_version") != std::to_string(SCHEMA_VERSION)) {
        throw StoreError(std::format("'{}': schema version {} is not the supported version {}",
                                     core::PathText(file), value("schema_version"), SCHEMA_VERSION));
    }
```

and replace

```cpp
    meta.createdAt = value("created_at");
    return RunStore(std::move(db), std::move(meta), file);
}
```

with

```cpp
    meta.createdAt = value("created_at");

    const std::string& kindText = value("kind");
    if (kindText == RunKindName(RunKind::Owned)) {
        meta.kind = RunKind::Owned;
    } else if (kindText == RunKindName(RunKind::Hosted)) {
        meta.kind = RunKind::Hosted;
    } else {
        throw StoreError(std::format("'{}': run_meta 'kind' is '{}', not 'owned' or 'hosted'", core::PathText(file),
                                     kindText));
    }
    return RunStore(std::move(db), std::move(meta), file);
}
```

5. Before the constructor definition `RunStore::RunStore(Database db, RunMeta meta, std::filesystem::path file)` at the end of the file, insert the hosted methods:

```cpp
nlohmann::json RunStore::HostData()
{
    RequireHosted("HostData");
    Statement query = _db.Prepare("SELECT data FROM host_data WHERE id = 1");
    if (!query.Step()) {
        throw StoreError(std::format("'{}': the hosted run has no host data", core::PathText(_file)));
    }
    try {
        return nlohmann::json::parse(query.ColumnText(0));
    } catch (const nlohmann::json::parse_error& error) {
        throw StoreError(std::format("'{}': host data is not valid JSON: {}", core::PathText(_file), error.what()));
    }
}

void RunStore::ReplaceHostData(const nlohmann::json& hostData, const std::string& hash)
{
    RequireHosted("ReplaceHostData");
    MORPHEUS_REQUIRE(!hash.empty(), "ReplaceHostData: the content hash must not be empty");
    // The data and its hash change together, so a reopen never compares new data with an old hash.
    _db.Transaction([&] {
        _db.Prepare("UPDATE host_data SET data = ?1 WHERE id = 1").Bind(1, hostData.dump()).Run();
        _db.Prepare("UPDATE run_meta SET value = ?1 WHERE key = 'world_hash'").Bind(1, hash).Run();
    });
    _meta.worldHash = hash;
}

std::string RunStore::CreateConversation(const std::vector<core::Participant>& participants)
{
    RequireHosted("CreateConversation");
    MORPHEUS_REQUIRE(!participants.empty(), "CreateConversation: a conversation needs a participant");
    // Conversations are never deleted, so their count names the next one: c1, c2, ... The count's statement is
    // finished before the insert.
    std::int64_t position = 0;
    {
        Statement count = _db.Prepare("SELECT COUNT(*) FROM conversations");
        const bool hasRow = count.Step();
        MORPHEUS_REQUIRE(hasRow, "CreateConversation: aggregate query returned no row");
        position = count.ColumnInt(0) + 1;
    }
    const std::string id = std::format("c{}", position);
    _db.Prepare("INSERT INTO conversations (id, position, participants, ended, folded_through) "
                "VALUES (?1, ?2, ?3, 0, 0)")
        .Bind(1, id)
        .Bind(2, position)
        .Bind(3, ParticipantsToJson(participants).dump())
        .Run();
    return id;
}

std::optional<core::Conversation> RunStore::Conversation(std::string_view id)
{
    RequireHosted("Conversation");
    for (core::Conversation& conversation : Conversations()) {
        if (conversation.id == id) {
            return std::move(conversation);
        }
    }
    return std::nullopt;
}

std::vector<core::Conversation> RunStore::Conversations()
{
    RequireHosted("Conversations");
    Statement query =
        _db.Prepare("SELECT id, participants, ended, folded_through FROM conversations ORDER BY position");
    std::vector<core::Conversation> conversations;
    while (query.Step()) {
        core::Conversation conversation;
        conversation.id = query.ColumnText(0);
        // Damaged participants are a StoreError naming the file and the conversation.
        auto damaged = [&](const std::string& problem) {
            return StoreError(std::format("'{}': conversation '{}' column 'participants' {}", core::PathText(_file),
                                          conversation.id, problem));
        };
        nlohmann::json participants;
        try {
            participants = nlohmann::json::parse(query.ColumnText(1));
        } catch (const nlohmann::json::parse_error& error) {
            throw damaged(std::string("is not valid JSON: ") + error.what());
        }
        if (!participants.is_array()) {
            throw damaged("is not an array");
        }
        for (const nlohmann::json& participant : participants) {
            const bool whole = participant.is_object() && participant.contains("id") && participant.at("id").is_string() &&
                               participant.contains("name") && participant.at("name").is_string() &&
                               participant.contains("bio") && participant.at("bio").is_string();
            if (!whole) {
                throw damaged("holds a participant without a string id, name and bio");
            }
            conversation.participants.push_back({participant.at("id").get<std::string>(),
                                                 participant.at("name").get<std::string>(),
                                                 participant.at("bio").get<std::string>()});
        }
        conversation.ended = query.ColumnInt(2) != 0;
        conversation.foldedThrough = static_cast<int>(query.ColumnInt(3));
        conversations.push_back(std::move(conversation));
    }
    return conversations;
}

void RunStore::SetParticipants(std::string_view id, const std::vector<core::Participant>& participants)
{
    RequireHosted("SetParticipants");
    MORPHEUS_REQUIRE(Conversation(id).has_value(), "SetParticipants: no such conversation");
    MORPHEUS_REQUIRE(!participants.empty(), "SetParticipants: a conversation needs a participant");
    _db.Prepare("UPDATE conversations SET participants = ?1 WHERE id = ?2")
        .Bind(1, ParticipantsToJson(participants).dump())
        .Bind(2, id)
        .Run();
}

void RunStore::EndConversation(std::string_view id)
{
    RequireHosted("EndConversation");
    MORPHEUS_REQUIRE(Conversation(id).has_value(), "EndConversation: no such conversation");
    _db.Prepare("UPDATE conversations SET ended = 1 WHERE id = ?1").Bind(1, id).Run();
}

void RunStore::FoldConversation(std::string_view id, const core::Summary& summary)
{
    RequireHosted("FoldConversation");
    MORPHEUS_REQUIRE(summary.fromTurn >= 1 && summary.fromTurn <= summary.toTurn,
                     std::format("FoldConversation: bad turn range {}..{}", summary.fromTurn, summary.toTurn));
    MORPHEUS_REQUIRE(Conversation(id).has_value(), "FoldConversation: no such conversation");
    // The summary and the conversation's folded mark land together: a turn is never both folded and shown in L3.
    _db.Transaction([&] {
        _db.Prepare("INSERT INTO summaries (from_turn, to_turn, text) VALUES (?1, ?2, ?3)")
            .Bind(1, summary.fromTurn)
            .Bind(2, summary.toTurn)
            .Bind(3, summary.text)
            .Run();
        _db.Prepare("UPDATE conversations SET folded_through = ?1 WHERE id = ?2").Bind(1, summary.toTurn).Bind(2, id).Run();
    });
}

void RunStore::AddConversationEvents(std::string_view id, int afterTurn, const std::vector<nlohmann::json>& events)
{
    RequireHosted("AddConversationEvents");
    MORPHEUS_REQUIRE(Conversation(id).has_value(), "AddConversationEvents: no such conversation");
    _db.Transaction([&] {
        for (const nlohmann::json& event : events) {
            _db.Prepare("INSERT INTO conversation_events (conversation_id, after_turn, event) VALUES (?1, ?2, ?3)")
                .Bind(1, id)
                .Bind(2, afterTurn)
                .Bind(3, event.dump())
                .Run();
        }
    });
}

std::vector<core::ConversationEvent> RunStore::ConversationEvents(std::string_view id)
{
    RequireHosted("ConversationEvents");
    Statement query =
        _db.Prepare("SELECT after_turn, event FROM conversation_events WHERE conversation_id = ?1 ORDER BY id");
    query.Bind(1, id);
    std::vector<core::ConversationEvent> events;
    while (query.Step()) {
        core::ConversationEvent event;
        event.afterTurn = static_cast<int>(query.ColumnInt(0));
        try {
            event.event = nlohmann::json::parse(query.ColumnText(1));
        } catch (const nlohmann::json::parse_error& error) {
            throw StoreError(std::format("'{}': an event of conversation '{}' is not valid JSON: {}",
                                         core::PathText(_file), id, error.what()));
        }
        events.push_back(std::move(event));
    }
    return events;
}

int RunStore::LatestHostedTurn()
{
    RequireHosted("LatestHostedTurn");
    Statement query = _db.Prepare("SELECT COALESCE(MAX(number), 0) FROM hosted_turns");
    const bool hasRow = query.Step();
    MORPHEUS_REQUIRE(hasRow, "LatestHostedTurn: aggregate query returned no row");
    return static_cast<int>(query.ColumnInt(0));
}

void RunStore::CommitHostedTurn(const core::HostedTurn& record)
{
    RequireHosted("CommitHostedTurn");
    const int expected = LatestHostedTurn() + 1;
    MORPHEUS_REQUIRE(record.number == expected,
                     std::format("CommitHostedTurn: expected turn {}, got {}", expected, record.number));
    MORPHEUS_REQUIRE(Conversation(record.conversationId).has_value(), "CommitHostedTurn: no such conversation");

    // JSON columns. The replace handler keeps a reason quoting text that is not UTF-8 from throwing.
    const nlohmann::json listeners = record.listenerIds;
    const nlohmann::json playerText = record.playerText ? nlohmann::json(*record.playerText) : nlohmann::json(nullptr);
    nlohmann::json events = nlohmann::json::array();
    for (const nlohmann::json& event : record.events) {
        events.push_back(event);
    }
    const nlohmann::json adjudication = {{"requested", record.adjudication.requested},
                                         {"action_id", record.adjudication.actionId},
                                         {"params", record.adjudication.params},
                                         {"reasons", record.adjudication.reasons}};
    _db.Prepare("INSERT INTO hosted_turns (number, conversation_id, speaker_id, listener_ids, player_text, events, "
                "observation, resolve_output, adjudication, line, timings) "
                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)")
        .Bind(1, record.number)
        .Bind(2, record.conversationId)
        .Bind(3, record.speakerId)
        .Bind(4, listeners.dump())
        .Bind(5, playerText.dump())
        .Bind(6, events.dump())
        .Bind(7, record.observation.dump())
        .Bind(8, record.resolveOutput)
        .Bind(9, adjudication.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))
        .Bind(10, record.line)
        .Bind(11, record.timings.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))
        .Run();
}

std::vector<core::HostedTurn> RunStore::HostedTurns(std::string_view conversationId)
{
    RequireHosted("HostedTurns");
    Statement query = _db.Prepare("SELECT number, speaker_id, listener_ids, player_text, events, observation, "
                                  "resolve_output, adjudication, line, timings FROM hosted_turns "
                                  "WHERE conversation_id = ?1 ORDER BY number");
    query.Bind(1, conversationId);
    std::vector<core::HostedTurn> turns;
    while (query.Step()) {
        core::HostedTurn turn;
        turn.number = static_cast<int>(query.ColumnInt(0));
        turn.conversationId = std::string(conversationId);
        // Damaged saved data is a StoreError naming the file, the turn and the column.
        auto damaged = [&](const char* column, const std::string& problem) {
            return StoreError(std::format("'{}': hosted turn {} column '{}' {}", core::PathText(_file), turn.number,
                                          column, problem));
        };
        auto parseColumn = [&](int column, const char* name) {
            try {
                return nlohmann::json::parse(query.ColumnText(column));
            } catch (const nlohmann::json::parse_error& error) {
                throw damaged(name, std::string("is not valid JSON: ") + error.what());
            }
        };
        turn.speakerId = query.ColumnText(1);

        const nlohmann::json listeners = parseColumn(2, "listener_ids");
        if (!listeners.is_array()) {
            throw damaged("listener_ids", "is not an array");
        }
        for (const nlohmann::json& listener : listeners) {
            if (!listener.is_string()) {
                throw damaged("listener_ids", "holds a value that is not a string");
            }
            turn.listenerIds.push_back(listener.get<std::string>());
        }

        const nlohmann::json playerText = parseColumn(3, "player_text");
        if (playerText.is_string()) {
            turn.playerText = playerText.get<std::string>();
        } else if (!playerText.is_null()) {
            throw damaged("player_text", "is neither a string nor null");
        }

        const nlohmann::json events = parseColumn(4, "events");
        if (!events.is_array()) {
            throw damaged("events", "is not an array");
        }
        for (const nlohmann::json& event : events) {
            turn.events.push_back(event);
        }
        turn.observation = parseColumn(5, "observation");
        turn.resolveOutput = query.ColumnText(6);

        const nlohmann::json adjudication = parseColumn(7, "adjudication");
        const bool whole = adjudication.is_object() && adjudication.contains("requested") &&
                           adjudication.at("requested").is_boolean() && adjudication.contains("action_id") &&
                           adjudication.at("action_id").is_string() && adjudication.contains("params") &&
                           adjudication.contains("reasons") && adjudication.at("reasons").is_array();
        if (!whole) {
            throw damaged("adjudication", "is malformed");
        }
        turn.adjudication.requested = adjudication.at("requested").get<bool>();
        turn.adjudication.actionId = adjudication.at("action_id").get<std::string>();
        turn.adjudication.params = adjudication.at("params");
        for (const nlohmann::json& reason : adjudication.at("reasons")) {
            if (!reason.is_string()) {
                throw damaged("adjudication", "holds a reason that is not a string");
            }
            turn.adjudication.reasons.push_back(reason.get<std::string>());
        }

        turn.line = query.ColumnText(8);
        turn.timings = parseColumn(9, "timings");
        turns.push_back(std::move(turn));
    }
    return turns;
}

void RunStore::RequireHosted(const char* method) const
{
    MORPHEUS_REQUIRE(_meta.kind == RunKind::Hosted, std::format("RunStore::{}: this is an owned run", method));
}

```

- [ ] **Step 5: Run the store tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[store]"`
Expected: builds with no warnings; `All tests passed`.

- [ ] **Step 6: Run the whole fast suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 439` (427 + 7 + 5), the three `[server]` tests "Not Run (Skipped)". `Session` and the app open their runs unchanged: new owned runs are version 3 with kind `owned`.

- [ ] **Step 7: Byte check and commit**

```bash
LC_ALL=C grep -c $'[\x80-\xFF]' src/core/HostedRecord.h src/store/RunStore.h src/store/RunStore.cpp tests/store/HostedStoreTests.cpp tests/store/RunStoreTests.cpp
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' src/core/HostedRecord.h src/store/RunStore.h src/store/RunStore.cpp tests/store/HostedStoreTests.cpp tests/store/RunStoreTests.cpp
git add src/core/HostedRecord.h src/store/RunStore.h src/store/RunStore.cpp tests/store/HostedStoreTests.cpp tests/store/RunStoreTests.cpp
git commit -m "RunStore schema version 3: run kind, hosted tables and methods, in-place upgrade of version 2" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

### Task 3: The shared memory upkeep

`Session`'s fit loop (`AssembleFitting`), `FoldOldest` and `Condense` move into `turn::MemoryUpkeep`, which works on summaries and rendered transcript entries (`context::TranscriptEntry`), not on `core::TurnRecord`. The fold functions (`turn/Folding.h`) and the owned prompt builder take entries too. `Session` changes only by calling the upkeep; its prompts, calls and messages stay byte-identical, so every `Session` test passes unchanged. `FoldOldest` now takes how many entries to fold (the fit loop passes half the transcript as before), so `FoldAll` can fold an ended conversation in one call.

**Files:**
- Modify: `src/context/Transcript.h`, `src/context/Transcript.cpp`
- Modify: `src/context/PromptBuilder.h`, `src/context/PromptBuilder.cpp`
- Modify: `src/turn/Folding.h`, `src/turn/Folding.cpp`
- Create: `src/turn/MemoryUpkeep.h`, `src/turn/MemoryUpkeep.cpp`
- Modify: `src/turn/Session.h`, `src/turn/Session.cpp`, `src/turn/CMakeLists.txt`
- Create: `tests/turn/MemoryUpkeepTests.cpp`
- Modify: `tests/context/RenderTests.cpp`, `tests/context/PromptBuilderTests.cpp`, `tests/turn/FoldingTests.cpp`, `tests/app/ModelSeatTests.cpp`

**Interfaces:**
- Consumes: nothing from Tasks 1-2.
- Produces (`context/Transcript.h`): `struct context::TranscriptEntry { int turn = 0; std::vector<inference::Message> messages; std::vector<inference::Message> foldMessages; std::string fact; }`; `std::vector<TranscriptEntry> context::TranscriptEntries(const std::vector<core::TurnRecord>& turns, int proseTurns)`.
- Produces (`context/PromptBuilder.h`): `ContextInputs::transcript` is `std::vector<TranscriptEntry>`; `ContextInputs::proseTurns` is gone (the caller renders).
- Produces (`turn/Folding.h`): `SummarizePrompt(const inference::Message&, const std::vector<context::TranscriptEntry>&)`, `SummarizeFallback(const std::vector<context::TranscriptEntry>&, int)`, `SummarizeTurns(const inference::Message&, const std::vector<context::TranscriptEntry>&, const inference::GenerationSpec&, inference::LlmBackend&, std::stop_token)`; the condense functions are unchanged.
- Produces (`turn/MemoryUpkeep.h`): `class turn::TurnError` (moved from `Session.h`); `FOLD_SUMMARY_DIVISOR`, `CONDENSE_SUMMARY_DIVISOR` (moved); `struct turn::FitPass { context::Assembly assembly; std::vector<context::TranscriptEntry> transcript; }`; `class turn::MemoryUpkeep` with `using Assemble = std::function<FitPass(const std::vector<RetrievalHit>& hits)>`, `using StoreFold = std::function<void(const core::Summary& summary)>`, constructor `MemoryUpkeep(store::RunStore& store, inference::LlmBackend& backend, const core::Settings& settings, std::string tailName, int turn, int attempt, StoreFold storeFold)`, `std::optional<context::Assembly> Fit(std::vector<RetrievalHit>& hits, const Assemble& assemble, std::stop_token stop)`, `bool Settle(const Assemble& assemble, std::stop_token stop)`, `bool FoldAll(const Assemble& assemble, std::stop_token stop)`.
- Produces (`turn/Session.h`): `Session` keeps its public API; `turn/Session.h` includes `turn/MemoryUpkeep.h`.

- [ ] **Step 1: Write the failing tests**

Create `tests/turn/MemoryUpkeepTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "context/Transcript.h"
#include "core/Dice.h"
#include "core/Errors.h"
#include "core/Settings.h"
#include "fakes/ScriptedBackend.h"
#include "store/RunStore.h"
#include "turn/EngineText.h"
#include "turn/MemoryUpkeep.h"

using namespace morpheus;

namespace {

// Memory calls' replies: a condense call writes "Condensed.", a fold call "Folded.".
test::ScriptedBackend::Responder MemoryResponder()
{
    return [](const inference::Prompt& prompt, const inference::GenerationSpec&) {
        return prompt.messages.back().content == turn::CONDENSE_INSTRUCTION ? std::string("Condensed.")
                                                                             : std::string("Folded.");
    };
}

core::Settings UpkeepSettings()
{
    core::Settings settings;
    settings.budget = {64, 64, 64, 1024, 1024, 1024, 512, 256}; // tail_allowance 1024, summary_cap 512.
    settings.resolveSampling = {0.3, 40, 0.9};
    return settings;
}

// Turn `number` as an entry: L3 shows "Shown N.", a fold reads "Narration N.".
context::TranscriptEntry Entry(int number)
{
    context::TranscriptEntry entry;
    entry.turn = number;
    entry.messages = {{inference::Role::User, std::format("Input {}.", number)},
                      {inference::Role::Assistant, std::format("Shown {}.", number)}};
    entry.foldMessages = {{inference::Role::User, std::format("Input {}.", number)},
                          {inference::Role::Assistant, std::format("Narration {}.", number)}};
    entry.fact = std::format("Input {}.", number);
    return entry;
}

// `count` retrieval hits, h1 to hN.
std::vector<turn::RetrievalHit> Hits(int count)
{
    std::vector<turn::RetrievalHit> hits;
    for (int i = 1; i <= count; ++i) {
        hits.push_back({store::EmbeddingKind::Lore, std::format("h{}", i), std::format("hit {}", i), 1.0f, 1, {}});
    }
    return hits;
}

// A stand-in for a session: `entries` are its turns, and the unfolded ones follow the last summary's range. Its
// prompt is flagged by rules the test sets: the tail is over its allowance while more than `maxHits` hits are left
// (-1: always), the summary over its cap while more than `maxSummaries` summaries are stored, and the transcript over
// its budget while more than `maxEntries` entries are unfolded. Its StoreFold adds the summary and records it.
struct Harness {
    store::RunStore store; // Set by the constructor.
    std::vector<context::TranscriptEntry> entries = {};
    int maxHits = 100;
    std::size_t maxSummaries = 100;
    std::size_t maxEntries = 100;
    std::vector<core::Summary> folded = {};

    Harness(const std::string& name, int firstTurn, int lastTurn)
        : store(store::RunStore::Create(test::FreshTempDir(name) / "run.db", store::RunMeta{},
                                        nlohmann::json::object()))
    {
        for (int number = firstTurn; number <= lastTurn; ++number) {
            entries.push_back(Entry(number));
        }
    }

    turn::MemoryUpkeep Upkeep(test::ScriptedBackend& backend, const core::Settings& settings)
    {
        return turn::MemoryUpkeep(store, backend, settings, "the test tail", 9, 0, [this](const core::Summary& summary) {
            store.AddSummary(summary);
            folded.push_back(summary);
        });
    }

    turn::MemoryUpkeep::Assemble Assemble()
    {
        return [this](const std::vector<turn::RetrievalHit>& hits) {
            const std::vector<core::Summary> summaries = store.Summaries();
            const int foldedThrough = summaries.empty() ? 0 : summaries.back().toTurn;
            turn::FitPass pass;
            for (const context::TranscriptEntry& entry : entries) {
                if (entry.turn > foldedThrough) {
                    pass.transcript.push_back(entry);
                }
            }
            pass.assembly.prompt.messages = {{inference::Role::System, "SYSTEM"}, {inference::Role::User, "TAIL"}};
            pass.assembly.prompt.stableCount = 1;
            pass.assembly.tokens.tail = 7;
            pass.assembly.tailOverBudget = static_cast<int>(hits.size()) > maxHits;
            pass.assembly.summaryOverBudget = summaries.size() > maxSummaries;
            pass.assembly.transcriptOverBudget = pass.transcript.size() > maxEntries;
            return pass;
        };
    }
};

} // namespace

TEST_CASE("Fit sheds hits from the end while the tail is over its allowance, then names the caller's tail", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_tail", 1, 0);
    harness.maxHits = 1;
    std::vector<turn::RetrievalHit> hits = Hits(3);
    const std::optional<context::Assembly> fitted = harness.Upkeep(backend, settings).Fit(hits, harness.Assemble(), {});
    REQUIRE(fitted);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].refId == "h1"); // The last hits go first.
    CHECK(backend.Calls().empty());

    // A tail over its allowance with no hit left is a TurnError naming what the caller's tail holds.
    harness.maxHits = -1;
    std::vector<turn::RetrievalHit> more = Hits(2);
    CHECK_THROWS_AS(harness.Upkeep(backend, settings).Fit(more, harness.Assemble(), {}), turn::TurnError);
    CHECK(more.empty());
    CHECK_THROWS_WITH(harness.Upkeep(backend, settings).Fit(more, harness.Assemble(), {}),
                      "the test tail use 7 tokens, over tail_allowance 1024");
}

TEST_CASE("Fit folds the oldest half of the transcript and condenses an over-cap summary until the prompt fits",
          "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_fit", 1, 8);
    harness.maxEntries = 2;
    harness.maxSummaries = 1;
    std::vector<turn::RetrievalHit> hits;
    REQUIRE(harness.Upkeep(backend, settings).Fit(hits, harness.Assemble(), {}));

    // 8 unfolded: fold 1-4. 4 unfolded: fold 5-6. Two summaries over a cap of one: condense. 7 and 8 fit.
    REQUIRE(harness.folded.size() == 2);
    CHECK(harness.folded[0].fromTurn == 1);
    CHECK(harness.folded[0].toTurn == 4);
    CHECK(harness.folded[0].text == "Folded.");
    CHECK(harness.folded[1].fromTurn == 5);
    CHECK(harness.folded[1].toTurn == 6);
    const std::vector<core::Summary> summaries = harness.store.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 6);
    CHECK(summaries[0].text == "Condensed.");

    // The fold reads each entry's fold messages after the prompt's system message, with the fold's cap and seed.
    const std::vector<test::ScriptedBackend::Call>& calls = backend.Calls();
    REQUIRE(calls.size() == 3);
    REQUIRE(calls[0].prompt.messages.size() == 1 + 2 * 4 + 1);
    CHECK(calls[0].prompt.messages[0].content == "SYSTEM");
    CHECK(calls[0].prompt.messages[2].content == "Narration 1.");
    CHECK(calls[0].prompt.messages.back().content == turn::SUMMARIZE_INSTRUCTION);
    CHECK(calls[0].spec.maxTokens == 512 / turn::FOLD_SUMMARY_DIVISOR);
    CHECK(calls[0].spec.sampling.seed == core::SamplingSeed(0, 9, core::CallKind::Summarize, 0));
    CHECK(calls[2].prompt.messages.back().content == turn::CONDENSE_INSTRUCTION);
    CHECK(calls[2].spec.maxTokens == 512 / turn::CONDENSE_SUMMARY_DIVISOR);
}

TEST_CASE("Fit returns nothing when a memory call is cancelled, and stores no summary", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_cancel", 1, 4);
    harness.maxEntries = 2;
    std::stop_source stop;
    stop.request_stop();
    std::vector<turn::RetrievalHit> hits;
    CHECK_FALSE(harness.Upkeep(backend, settings).Fit(hits, harness.Assemble(), stop.get_token()));
    CHECK(harness.folded.empty());
    CHECK(harness.store.Summaries().empty());
}

TEST_CASE("Settle folds and condenses only while the transcript or the summary is over budget", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_settle", 1, 5);
    harness.maxEntries = 2;
    harness.maxHits = -1; // The tail is not Settle's to fix: between turns it is a placeholder.
    CHECK(harness.Upkeep(backend, settings).Settle(harness.Assemble(), {}));

    // 5 unfolded: fold 1-2. 3 unfolded: fold 3. The 2 left fit.
    REQUIRE(harness.folded.size() == 2);
    CHECK(harness.folded[0].fromTurn == 1);
    CHECK(harness.folded[0].toTurn == 2);
    CHECK(harness.folded[1].fromTurn == 3);
    CHECK(harness.folded[1].toTurn == 3);

    // With nothing over budget it makes no call.
    const std::size_t calls = backend.Calls().size();
    CHECK(harness.Upkeep(backend, settings).Settle(harness.Assemble(), {}));
    CHECK(backend.Calls().size() == calls);
}

TEST_CASE("FoldAll folds every unfolded entry in one call when it fits, condensing on the way", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_fold_all", 5, 8);
    harness.store.AddSummary({1, 2, "Earlier."});
    harness.store.AddSummary({3, 4, "Later."});
    harness.maxSummaries = 2;
    CHECK(harness.Upkeep(backend, settings).FoldAll(harness.Assemble(), {}));

    // 5-8 fold in one call; three summaries over a cap of two condense into one; nothing is left unfolded.
    REQUIRE(harness.folded.size() == 1);
    CHECK(harness.folded[0].fromTurn == 5);
    CHECK(harness.folded[0].toTurn == 8);
    const std::vector<core::Summary> summaries = harness.store.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 8);
    CHECK(backend.Calls().size() == 2);
}

TEST_CASE("A summary still over its cap right after a condense is a DataError", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_over_cap", 1, 0);
    harness.store.AddSummary({1, 1, "Long."});
    harness.maxSummaries = 0; // Even one summary is over the cap.
    CHECK_THROWS_AS(harness.Upkeep(backend, settings).Settle(harness.Assemble(), {}), core::DataError);
    CHECK_THROWS_WITH(harness.Upkeep(backend, settings).Settle(harness.Assemble(), {}),
                      "summary_cap 512 is too small for even a condensed summary (0 tokens)");
    CHECK(backend.Calls().size() == 2); // One condense per Settle, then the throw instead of a second.
}
```

In `tests/context/RenderTests.cpp`, append:

```cpp
TEST_CASE("TranscriptEntries renders each turn as its L3 messages, its fold messages and its fact", "[context]")
{
    std::vector<core::TurnRecord> turns = NumberedTurns(3);
    turns[0] = TurnWithMechanics();
    turns[0].number = 1;
    turns[0].digest = "The player fell.";

    // proseTurns 1: turns 1 and 2 are older and show their digest in L3; a fold reads every narration.
    const std::vector<context::TranscriptEntry> entries = context::TranscriptEntries(turns, 1);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].turn == 1);
    CHECK(entries[0].messages[1].content == "The player fell.");
    CHECK(entries[0].foldMessages[1].content == "You slip and hit the deck.");
    CHECK(entries[0].fact == "I climb the hull. " + context::RenderMechanicsLine(turns[0]));
    CHECK(entries[1].messages[1].content == "Digest 2.");
    CHECK(entries[1].foldMessages[1].content == "Narration 2.");
    CHECK(entries[1].fact == "Input 2."); // No mechanics: the input alone.
    CHECK(entries[2].messages[1].content == "Narration 3.");

    // The L3 messages are RenderTranscript's, two per turn, in order.
    const std::vector<inference::Message> flat = context::RenderTranscript(turns, 1);
    REQUIRE(flat.size() == 6);
    for (std::size_t i = 0; i < flat.size(); ++i) {
        CHECK(entries[i / 2].messages[i % 2].role == flat[i].role);
        CHECK(entries[i / 2].messages[i % 2].content == flat[i].content);
    }
    CHECK_THROWS_AS(context::TranscriptEntries(turns, 0), core::ContractError);
}
```

In `tests/context/PromptBuilderTests.cpp` (the prompts these tests build do not change; only how the transcript reaches the builder does):

1. Replace

   ```cpp
   #include "TestPaths.h"
   #include "context/PromptBuilder.h"
   ```

   with

   ```cpp
   #include <limits>
   #include <vector>

   #include "TestPaths.h"
   #include "context/PromptBuilder.h"
   #include "context/Transcript.h"
   ```

2. In `Inputs`, replace `    inputs.transcript = {turn};` with

   ```cpp
       inputs.transcript = context::TranscriptEntries({turn}, std::numeric_limits<int>::max());
   ```

3. In the section `transcript one token over its budget`, replace `        inputs.transcript.front().narration = std::string(400, 'x');` with

   ```cpp
           inputs.transcript.front().messages.back().content = std::string(400, 'x');
   ```

4. In the test `Thinking budgets are reserved with the generations they precede`, replace `    inputs.transcript.front().narration = std::string(1200, 'x');` with

   ```cpp
       inputs.transcript.front().messages.back().content = std::string(1200, 'x');
   ```

5. In the test `Digests shrink the transcript layer`, replace

   ```cpp
       inputs.transcript.clear();
       for (int number = 5; number <= 7; ++number) {
           core::TurnRecord turn;
           turn.number = number;
           turn.input = "I walk on.";
           turn.narration = std::string(400, 'n');
           turn.digest = "The player walked on.";
           inputs.transcript.push_back(turn);
       }
       const context::Assembly full = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend);
       inputs.proseTurns = 1;
       const context::Assembly compact = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend);
   ```

   with

   ```cpp
       std::vector<core::TurnRecord> turns;
       for (int number = 5; number <= 7; ++number) {
           core::TurnRecord turn;
           turn.number = number;
           turn.input = "I walk on.";
           turn.narration = std::string(400, 'n');
           turn.digest = "The player walked on.";
           turns.push_back(turn);
       }
       inputs.transcript = context::TranscriptEntries(turns, std::numeric_limits<int>::max());
       const context::Assembly full = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend);
       inputs.transcript = context::TranscriptEntries(turns, 1);
       const context::Assembly compact = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend);
   ```

   and in the same test replace the comment `// The default shows every narration.` with `// Every narration, as with compact_transcript off.`

In `tests/turn/FoldingTests.cpp` (the fold prompts these tests check do not change; the turns now reach the folding functions as rendered entries):

1. Replace

   ```cpp
   #include <optional>
   #include <stop_token>
   ```

   with

   ```cpp
   #include <limits>
   #include <optional>
   #include <stop_token>
   ```

2. After the function `BlankBackend` (before `} // namespace`), add:

   ```cpp

   // `turns` as the session renders them for L3 with every narration shown; a fold reads the same.
   std::vector<context::TranscriptEntry> Entries(const std::vector<core::TurnRecord>& turns)
   {
       return context::TranscriptEntries(turns, std::numeric_limits<int>::max());
   }
   ```

3. Replace `        system, {Turn(3, "I knock.", "The door creaks."), Turn(4, "I enter.", "It is dark.")}, spec, backend, {});` with `        system, Entries({Turn(3, "I knock.", "The door creaks."), Turn(4, "I enter.", "It is dark.")}), spec, backend, {});`
4. Replace `    CHECK_FALSE(turn::SummarizeTurns({inference::Role::System, "S"}, {Turn(1, "a", "b")}, {}, backend, stop.get_token()));` with `    CHECK_FALSE(turn::SummarizeTurns({inference::Role::System, "S"}, Entries({Turn(1, "a", "b")}), {}, backend, stop.get_token()));`
5. Replace `    CHECK_THROWS_AS(turn::SummarizeTurns(user, {Turn(1, "a", "b")}, {}, backend, {}), core::ContractError);` with `    CHECK_THROWS_AS(turn::SummarizeTurns(user, Entries({Turn(1, "a", "b")}), {}, backend, {}), core::ContractError);`
6. Replace `        turn::SummarizeTurns({inference::Role::System, "S"}, turns, inference::GenerationSpec{}, backend, {});` with `        turn::SummarizeTurns({inference::Role::System, "S"}, Entries(turns), inference::GenerationSpec{}, backend, {});`
7. Replace `        turn::SummarizeTurns({inference::Role::System, "S"}, {Turn(1, std::string(200, 'x'), "b")}, spec, backend, {});` with `        turn::SummarizeTurns({inference::Role::System, "S"}, Entries({Turn(1, std::string(200, 'x'), "b")}), spec, backend, {});`
8. In `The fold and condense prompts and fallbacks are the ones the folding calls use`, replace `    const std::optional<core::Summary> summary = turn::SummarizeTurns(system, turns, spec, backend, {});` with `    const std::optional<core::Summary> summary = turn::SummarizeTurns(system, Entries(turns), spec, backend, {});`, replace `    const inference::Prompt summarizePrompt = turn::SummarizePrompt(system, turns);` with `    const inference::Prompt summarizePrompt = turn::SummarizePrompt(system, Entries(turns));`, and replace `    const core::Summary summaryFallback = turn::SummarizeFallback(turns, spec.maxTokens);` with `    const core::Summary summaryFallback = turn::SummarizeFallback(Entries(turns), spec.maxTokens);`.
9. In `A fold sees the full narration even of turns with a digest`, replace

   ```cpp
       const inference::Prompt prompt =
           turn::SummarizePrompt({inference::Role::System, "story"}, {walked, Turn(2, "I wait.", "Time passes.")});
   ```

   with

   ```cpp
       // proseTurns 1: L3 would show turn 1's digest; the fold still reads its narration.
       const inference::Prompt prompt = turn::SummarizePrompt(
           {inference::Role::System, "story"},
           context::TranscriptEntries({walked, Turn(2, "I wait.", "Time passes.")}, 1));
   ```

In `tests/app/ModelSeatTests.cpp`:

1. Replace

   ```cpp
   #include <optional>
   #include <stop_token>
   ```

   with

   ```cpp
   #include <limits>
   #include <optional>
   #include <stop_token>
   ```

   and replace `#include "app/model/ModelSeat.h"` with

   ```cpp
   #include "app/model/ModelSeat.h"
   #include "context/Transcript.h"
   ```

2. Replace `    CHECK(app::SeatLabelOf(turn::SummarizePrompt(system, {played}), plain) == app::SeatLabel::Summarize);` with

   ```cpp
       CHECK(app::SeatLabelOf(turn::SummarizePrompt(system, context::TranscriptEntries({played}, std::numeric_limits<int>::max())),
                              plain) == app::SeatLabel::Summarize);
   ```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors, among them `Cannot open include file: 'turn/MemoryUpkeep.h'` in `tests/turn/MemoryUpkeepTests.cpp` and `'TranscriptEntries': is not a member of 'morpheus::context'`.

- [ ] **Step 3: Render turns as transcript entries**

In `src/context/Transcript.h`, before `// One applied change as the mechanics line and the resolution record show it`, insert:

```cpp
// One turn of a transcript layer (L3), rendered: what the prompt shows, what a fold reads, and the engine's one-line
// account of it for a summary written without a model. Both sessions render their turns into entries; the prompt
// builders and the shared memory upkeep (turn::MemoryUpkeep) work on entries only.
struct TranscriptEntry {
    int turn = 0;                                      // The turn's number, run-wide.
    std::vector<inference::Message> messages = {};     // As L3 shows it: a user and an assistant message.
    std::vector<inference::Message> foldMessages = {}; // As a fold call reads it: the turn in full.
    std::string fact = {};                             // One line; SummarizeFallback writes "Turn N: <fact>".
};

```

and at the end of the file, before `} // namespace morpheus::context`, insert:

```cpp

// Morpheus's own turns as entries: `messages` is the turn's pair from RenderTranscript(turns, proseTurns) (an older
// turn shows its digest), `foldMessages` its pair with every narration, and `fact` its input followed by its
// mechanics line when it has one. Throws ContractError for proseTurns < 1.
std::vector<TranscriptEntry> TranscriptEntries(const std::vector<core::TurnRecord>& turns, int proseTurns);
```

In `src/context/Transcript.cpp`, replace

```cpp
#include <cstddef>
#include <format>
#include <utility>
#include <vector>
```

with

```cpp
#include <cstddef>
#include <format>
#include <limits>
#include <utility>
#include <vector>
```

and before `} // namespace morpheus::context` at the end, insert:

```cpp

std::vector<TranscriptEntry> TranscriptEntries(const std::vector<core::TurnRecord>& turns, int proseTurns)
{
    // Two messages per turn in both views: as L3 shows it (older turns may show their digest) and in full, for a fold.
    const std::vector<inference::Message> shown = RenderTranscript(turns, proseTurns);
    const std::vector<inference::Message> full = RenderTranscript(turns, std::numeric_limits<int>::max());
    std::vector<TranscriptEntry> entries;
    entries.reserve(turns.size());
    for (std::size_t i = 0; i < turns.size(); ++i) {
        TranscriptEntry entry;
        entry.turn = turns[i].number;
        entry.messages = {shown[2 * i], shown[2 * i + 1]};
        entry.foldMessages = {full[2 * i], full[2 * i + 1]};
        // The engine's own account of the turn: its input and, when it has one, its mechanics line.
        const std::string mechanics = RenderMechanicsLine(turns[i]);
        entry.fact = mechanics.empty() ? turns[i].input : turns[i].input + " " + mechanics;
        entries.push_back(std::move(entry));
    }
    return entries;
}
```

In `src/context/PromptBuilder.h`, replace

```cpp
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Settings.h"
```

with

```cpp
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/Transcript.h"
#include "core/Settings.h"
```

and replace

```cpp
    std::vector<core::TurnRecord> transcript = {}; // L3: unfolded turns, oldest first.
    // L3: the newest turns that show their narration; older ones show their digest (context::RenderTranscript).
    // The default shows every narration.
    int proseTurns = std::numeric_limits<int>::max();
```

with

```cpp
    std::vector<TranscriptEntry> transcript = {};  // L3: unfolded turns, oldest first, rendered (TranscriptEntries).
```

In `src/context/PromptBuilder.cpp`, replace

```cpp
    for (inference::Message& message : RenderTranscript(inputs.transcript, inputs.proseTurns)) {
        tokens.transcript += backend.CountTokens(message.content);
        prompt.messages.push_back(std::move(message));
    }
```

with

```cpp
    for (const TranscriptEntry& entry : inputs.transcript) {
        for (const inference::Message& message : entry.messages) {
            tokens.transcript += backend.CountTokens(message.content);
            prompt.messages.push_back(message);
        }
    }
```

- [ ] **Step 4: Fold entries**

Replace the whole of `src/turn/Folding.h` with:

```cpp
#pragma once

#include <optional>
#include <stop_token>
#include <vector>

#include "context/Transcript.h"
#include "core/TurnRecord.h"
#include "inference/Llm.h"

namespace morpheus::turn {

// Rough bytes per token; the fallback only needs to stay near the summary's token cap.
constexpr int FALLBACK_BYTES_PER_TOKEN = 4;

// The fold call's prompt: `systemMessage` (the prompt's system message, L0 up to L2), each entry's foldMessages, then
// SUMMARIZE_INSTRUCTION. stableCount ends after the entries. The memory upkeep measures it before a fold.
inference::Prompt SummarizePrompt(const inference::Message& systemMessage,
                                  const std::vector<context::TranscriptEntry>& entries);

// The engine's summary of `entries`, written without a model: one "Turn N: <fact>" line per entry, cut to
// maxTokens * FALLBACK_BYTES_PER_TOKEN bytes. It covers the entries' range (the first's turn to the last's). Throws
// ContractError for no entries.
core::Summary SummarizeFallback(const std::vector<context::TranscriptEntry>& entries, int maxTokens);

// Summarizes `entries` (the oldest unfolded turns, in order) with one call on SummarizePrompt. That prompt starts like
// the turn's prompt, so the KV cache serves most of it. The summary covers the entries' range; its text is trimmed.
// When the model writes nothing, the summary is SummarizeFallback(entries, spec.maxTokens).
// Returns nullopt when `stop` cancels the call. Throws ContractError for no entries or a non-system message, and
// InferenceError from the backend (among others when the prompt does not fit the context: the caller measures it).
std::optional<core::Summary> SummarizeTurns(const inference::Message& systemMessage,
                                            const std::vector<context::TranscriptEntry>& entries,
                                            const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                            std::stop_token stop);

// The condense call's prompt: `systemMessage` (whose L2 holds the summaries), then CONDENSE_INSTRUCTION.
inference::Prompt CondensePrompt(const inference::Message& systemMessage);

// The engine's condensed summary, written without a model: one "Turns A-B: <text>" line per summary, cut to
// maxTokens * FALLBACK_BYTES_PER_TOKEN bytes. It covers every summary's range. Throws ContractError for none.
core::Summary CondenseFallback(const std::vector<core::Summary>& summaries, int maxTokens);

// Condenses every summary into one covering their whole range, with one call on CondensePrompt. When the model
// writes nothing, the summary is CondenseFallback(summaries, spec.maxTokens).
// Returns nullopt when cancelled. Throws ContractError for no summaries or a non-system message, and
// InferenceError from the backend.
std::optional<core::Summary> CondenseSummaries(const inference::Message& systemMessage,
                                               const std::vector<core::Summary>& summaries,
                                               const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                               std::stop_token stop);

} // namespace morpheus::turn
```

Replace the whole of `src/turn/Folding.cpp` with:

```cpp
#include "turn/Folding.h"

#include <cstddef>
#include <format>
#include <string>
#include <utility>

#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"

namespace morpheus::turn {

inference::Prompt SummarizePrompt(const inference::Message& systemMessage,
                                  const std::vector<context::TranscriptEntry>& entries)
{
    // The system message, then each turn in full (stable, so cached), then the instruction. Summaries are written
    // from the full narration, digests or not.
    inference::Prompt prompt;
    prompt.messages.push_back(systemMessage);
    for (const context::TranscriptEntry& entry : entries) {
        for (const inference::Message& message : entry.foldMessages) {
            prompt.messages.push_back(message);
        }
    }
    prompt.stableCount = prompt.messages.size();
    prompt.messages.push_back({inference::Role::User, std::string(SUMMARIZE_INSTRUCTION)});
    return prompt;
}

core::Summary SummarizeFallback(const std::vector<context::TranscriptEntry>& entries, int maxTokens)
{
    MORPHEUS_REQUIRE(!entries.empty() && maxTokens > 0, "SummarizeFallback: needs at least one entry and a token cap");
    // Each turn's fact: what the engine knows without a model.
    std::string text;
    for (const context::TranscriptEntry& entry : entries) {
        text += std::format("{}Turn {}: {}", text.empty() ? "" : "\n", entry.turn, entry.fact);
    }
    // Cut to about the summary's token cap.
    text = core::CutUtf8(text, static_cast<std::size_t>(maxTokens) * FALLBACK_BYTES_PER_TOKEN);
    return core::Summary{entries.front().turn, entries.back().turn, std::move(text)};
}

std::optional<core::Summary> SummarizeTurns(const inference::Message& systemMessage,
                                            const std::vector<context::TranscriptEntry>& entries,
                                            const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                            std::stop_token stop)
{
    MORPHEUS_REQUIRE(systemMessage.role == inference::Role::System && !entries.empty(),
                     "SummarizeTurns: needs the system message and at least one entry");
    const inference::Completion completion = backend.Complete(SummarizePrompt(systemMessage, entries), spec, {}, stop);
    if (completion.cancelled) {
        return std::nullopt;
    }

    // The model's summary, trimmed. When it wrote nothing, the engine writes a factual one so folding still
    // makes progress.
    std::string text = core::Trim(completion.text);
    if (text.empty()) {
        return SummarizeFallback(entries, spec.maxTokens);
    }
    return core::Summary{entries.front().turn, entries.back().turn, std::move(text)};
}

inference::Prompt CondensePrompt(const inference::Message& systemMessage)
{
    // The system message already holds every summary (L2), so only the instruction follows it.
    inference::Prompt prompt;
    prompt.messages.push_back(systemMessage);
    prompt.stableCount = 1;
    prompt.messages.push_back({inference::Role::User, std::string(CONDENSE_INSTRUCTION)});
    return prompt;
}

core::Summary CondenseFallback(const std::vector<core::Summary>& summaries, int maxTokens)
{
    MORPHEUS_REQUIRE(!summaries.empty() && maxTokens > 0, "CondenseFallback: needs at least one summary and a token cap");
    // Every summary's text under its range, cut to about the condensed summary's token cap.
    std::string text;
    for (const core::Summary& summary : summaries) {
        text += std::format("{}Turns {}-{}: {}", text.empty() ? "" : "\n", summary.fromTurn, summary.toTurn,
                            summary.text);
    }
    text = core::CutUtf8(text, static_cast<std::size_t>(maxTokens) * FALLBACK_BYTES_PER_TOKEN);
    return core::Summary{summaries.front().fromTurn, summaries.back().toTurn, std::move(text)};
}

std::optional<core::Summary> CondenseSummaries(const inference::Message& systemMessage,
                                               const std::vector<core::Summary>& summaries,
                                               const inference::GenerationSpec& spec, inference::LlmBackend& backend,
                                               std::stop_token stop)
{
    MORPHEUS_REQUIRE(systemMessage.role == inference::Role::System && !summaries.empty(),
                     "CondenseSummaries: needs the system message and at least one summary");
    const inference::Completion completion = backend.Complete(CondensePrompt(systemMessage), spec, {}, stop);
    if (completion.cancelled) {
        return std::nullopt;
    }

    // The model's summary, trimmed. When it wrote nothing, the engine writes one from the summaries it was given.
    std::string text = core::Trim(completion.text);
    if (text.empty()) {
        return CondenseFallback(summaries, spec.maxTokens);
    }
    return core::Summary{summaries.front().fromTurn, summaries.back().toTurn, std::move(text)};
}

} // namespace morpheus::turn
```

- [ ] **Step 5: Write the memory upkeep**

Create `src/turn/MemoryUpkeep.h`:

```cpp
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

#include "context/PromptBuilder.h"
#include "context/Transcript.h"
#include "core/Settings.h"
#include "core/TurnRecord.h"
#include "inference/Llm.h"
#include "store/RunStore.h"
#include "turn/Retrieval.h"

namespace morpheus::turn {

// A turn that cannot run as asked: nothing to undo or regenerate, an empty input, or an input too long for the
// tail allowance. Nothing changed; the message is for the player (in a hosted run, for the host).
class TurnError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

constexpr int FOLD_SUMMARY_DIVISOR = 4;     // A fold summary gets summary_cap / 4 tokens: several fit before a condense.
constexpr int CONDENSE_SUMMARY_DIVISOR = 2; // A condensed summary gets summary_cap / 2 tokens, so it fits its cap.

// One assembled prompt and the transcript entries its L3 shows, oldest first: what the upkeep measures and folds.
struct FitPass {
    context::Assembly assembly = {};
    std::vector<context::TranscriptEntry> transcript = {};
};

// The memory upkeep both sessions share (hosted-world spec, "Shared memory upkeep"): it keeps a prompt within its
// budgets by shedding retrieval hits, condensing the summaries and folding the oldest transcript entries into a new
// summary. It works on summaries and rendered entries only, so turn::Session passes its turns and
// turn::HostedSession its conversation's turns. Built for one turn's memory calls (their sampling seeds name the
// turn) and used at once: it holds the session's store, backend and settings by pointer.
class MemoryUpkeep {
public:
    // Assembles the prompt again for `hits`, from the store as it is now: the upkeep calls it after every fold,
    // condense and shed hit.
    using Assemble = std::function<FitPass(const std::vector<RetrievalHit>& hits)>;
    // Stores a fold's summary and makes the turns it covers searchable history: each session records what it folded
    // its own way.
    using StoreFold = std::function<void(const core::Summary& summary)>;

private:
    store::RunStore* _pStore = nullptr;         // Not owned; the session's.
    inference::LlmBackend* _pBackend = nullptr; // Not owned; the session's.
    const core::Settings* _pSettings = nullptr; // Not owned; the session's.
    std::string _tailName = {};                 // What the tail holds, for the TurnError when it is too long.
    int _turn = 0;                              // The turn these memory calls are for (their sampling seeds).
    int _attempt = 0;
    StoreFold _storeFold = {};

public:
    // `tailName` completes "<tailName> use N tokens, over tail_allowance M". Throws ContractError for an empty
    // storeFold.
    MemoryUpkeep(store::RunStore& store, inference::LlmBackend& backend, const core::Settings& settings,
                 std::string tailName, int turn, int attempt, StoreFold storeFold);

    // Assembles a prompt that fits, from the `hits` it may shed (from the end) and the store's summaries and
    // transcript. Every pass sheds a hit while the tail is over its allowance, condenses an over-cap summary, folds the
    // oldest half (at least one) of an over-budget or overflowing transcript, or sheds a hit as a last resort.
    // Returns the prompt, or nullopt when a memory call was cancelled (what it folded before stays). Throws TurnError
    // when the tail is over its allowance with no hit left or nothing fits at all, and DataError when a summary is
    // still over summary_cap right after a condense.
    std::optional<context::Assembly> Fit(std::vector<RetrievalHit>& hits, const Assemble& assemble,
                                         std::stop_token stop);

    // Between turns (Session::Maintain): condenses while the summary is over its cap and folds the oldest half while
    // the transcript is over its budget, with no hits. False when cancelled. Throws DataError like Fit.
    bool Settle(const Assemble& assemble, std::stop_token stop);

    // An ended conversation (HostedSession::Maintain): folds until no entry is left, every remaining entry in one
    // call when it fits, condensing whenever the summary is over its cap. False when cancelled. Throws DataError
    // like Fit.
    bool FoldAll(const Assemble& assemble, std::stop_token stop);

private:
    // Folds the oldest `wanted` entries of `transcript` into a new summary, or as many of them as fit one call in the
    // context. When not even one fits, the engine writes that entry's summary without a model call
    // (SummarizeFallback). Every call folds at least one entry and hands the summary to StoreFold. False when
    // cancelled.
    bool FoldOldest(const inference::Message& systemMessage, const std::vector<context::TranscriptEntry>& transcript,
                    std::size_t wanted, std::stop_token stop);

    // Replaces every summary with one condensed summary. When the condense call does not fit the context, the engine
    // writes it without a model call (CondenseFallback). False when cancelled.
    bool Condense(const inference::Message& systemMessage, std::stop_token stop);
};

} // namespace morpheus::turn
```

Create `src/turn/MemoryUpkeep.cpp`:

```cpp
#include "turn/MemoryUpkeep.h"

#include <algorithm>
#include <format>
#include <utility>

#include "core/Dice.h"
#include "core/Errors.h"
#include "turn/Folding.h"
#include "turn/Session.h"

namespace morpheus::turn {

namespace {

// Thrown when a summary is still over summary_cap right after a condense: condensing again would only repeat it.
core::DataError CondensedOverCap(int summaryCap, int summaryTokens)
{
    return core::DataError(std::format("summary_cap {} is too small for even a condensed summary ({} tokens)",
                                       summaryCap, summaryTokens));
}

} // namespace

MemoryUpkeep::MemoryUpkeep(store::RunStore& store, inference::LlmBackend& backend, const core::Settings& settings,
                           std::string tailName, int turn, int attempt, StoreFold storeFold)
    : _pStore(&store)
    , _pBackend(&backend)
    , _pSettings(&settings)
    , _tailName(std::move(tailName))
    , _turn(turn)
    , _attempt(attempt)
    , _storeFold(std::move(storeFold))
{
    MORPHEUS_REQUIRE(static_cast<bool>(_storeFold), "MemoryUpkeep: a fold needs somewhere to go");
}

std::optional<context::Assembly> MemoryUpkeep::Fit(std::vector<RetrievalHit>& hits, const Assemble& assemble,
                                                   std::stop_token stop)
{
    // Every pass sheds a hit, condenses the summary, or folds turns. Hits and the transcript only shrink, and a
    // summary still over its cap right after a condense throws, so the loop ends.
    bool condensed = false; // The summary was just condensed, with nothing folded into it since.
    for (;;) {
        FitPass pass = assemble(hits);
        const context::Assembly& assembly = pass.assembly;
        if (assembly.Fits()) {
            return std::move(pass.assembly);
        }
        if (assembly.tailOverBudget) {
            if (hits.empty()) {
                throw TurnError(std::format("{} use {} tokens, over tail_allowance {}", _tailName, assembly.tokens.tail,
                                            _pSettings->budget.tailAllowance));
            }
            hits.pop_back();
            continue;
        }
        if (assembly.summaryOverBudget) {
            if (condensed) {
                throw CondensedOverCap(_pSettings->budget.summaryCap, assembly.tokens.summary);
            }
            if (!Condense(assembly.prompt.messages.front(), stop)) {
                return std::nullopt;
            }
            condensed = true;
            continue;
        }
        // The transcript is over its budget, or the whole prompt overflows. contextOverflow is the authoritative fit
        // check: transcriptBudget leaves out a few template tokens that it counts.
        if (!pass.transcript.empty()) {
            const std::size_t half = std::max<std::size_t>(1, pass.transcript.size() / 2);
            if (!FoldOldest(assembly.prompt.messages.front(), pass.transcript, half, stop)) {
                return std::nullopt;
            }
            condensed = false;
            continue;
        }
        if (!hits.empty()) {
            hits.pop_back();
            continue;
        }
        throw TurnError("the prompt does not fit context_size even without transcript or retrieval");
    }
}

bool MemoryUpkeep::Settle(const Assemble& assemble, std::stop_token stop)
{
    // Every pass condenses or folds. A fold shrinks the transcript, and a summary still over its cap right after a
    // condense throws, so the loop ends.
    bool condensed = false; // The summary was just condensed, with nothing folded into it since.
    for (;;) {
        const FitPass pass = assemble({});
        if (pass.assembly.summaryOverBudget) {
            if (condensed) {
                throw CondensedOverCap(_pSettings->budget.summaryCap, pass.assembly.tokens.summary);
            }
            if (!Condense(pass.assembly.prompt.messages.front(), stop)) {
                return false;
            }
            condensed = true;
            continue;
        }
        if (pass.assembly.transcriptOverBudget && !pass.transcript.empty()) {
            const std::size_t half = std::max<std::size_t>(1, pass.transcript.size() / 2);
            if (!FoldOldest(pass.assembly.prompt.messages.front(), pass.transcript, half, stop)) {
                return false;
            }
            condensed = false;
            continue;
        }
        return true;
    }
}

bool MemoryUpkeep::FoldAll(const Assemble& assemble, std::stop_token stop)
{
    // Every pass condenses or folds at least one entry, so the transcript empties and the loop ends; a summary still
    // over its cap right after a condense throws.
    bool condensed = false; // The summary was just condensed, with nothing folded into it since.
    for (;;) {
        const FitPass pass = assemble({});
        if (pass.assembly.summaryOverBudget) {
            if (condensed) {
                throw CondensedOverCap(_pSettings->budget.summaryCap, pass.assembly.tokens.summary);
            }
            if (!Condense(pass.assembly.prompt.messages.front(), stop)) {
                return false;
            }
            condensed = true;
            continue;
        }
        if (!pass.transcript.empty()) {
            // All of it at once; FoldOldest folds the longest prefix that fits when that is too much for one call.
            if (!FoldOldest(pass.assembly.prompt.messages.front(), pass.transcript, pass.transcript.size(), stop)) {
                return false;
            }
            condensed = false;
            continue;
        }
        return true;
    }
}

bool MemoryUpkeep::FoldOldest(const inference::Message& systemMessage,
                              const std::vector<context::TranscriptEntry>& transcript, std::size_t wanted,
                              std::stop_token stop)
{
    MORPHEUS_REQUIRE(!transcript.empty() && wanted >= 1 && wanted <= transcript.size(),
                     "FoldOldest: needs 1 to transcript.size() entries to fold");
    inference::GenerationSpec spec;
    spec.maxTokens = std::max(1, _pSettings->budget.summaryCap / FOLD_SUMMARY_DIVISOR);
    spec.sampling = SamplingFrom(_pSettings->resolveSampling, core::SamplingSeed(_pStore->Meta().rngSeed, _turn,
                                                                                core::CallKind::Summarize, _attempt));

    // The oldest `count` entries of the transcript.
    auto oldest = [&transcript](std::size_t count) {
        return std::vector<context::TranscriptEntry>(transcript.begin(),
                                                     transcript.begin() + static_cast<std::ptrdiff_t>(count));
    };
    // Whether a fold call over the oldest `count` entries fits the context: its prompt plus the summary's max_tokens,
    // measured exactly as the backend measures a call. The transcript can be far larger than the context (a run
    // loaded with a smaller context_size, or turns put back by undoing past a condensed summary).
    auto fits = [&](std::size_t count) {
        const int promptTokens = _pBackend->CountPromptTokens(SummarizePrompt(systemMessage, oldest(count)));
        return spec.maxTokens <= _pBackend->ContextSize() - promptTokens;
    };

    // Fold the wanted entries, or the longest prefix of them that fits. More entries never cost fewer tokens, so a
    // binary search between "fits" and "does not fit" finds that prefix with few token counts.
    std::size_t count = wanted;
    if (!fits(wanted)) {
        std::size_t fitting = 0;      // Longest prefix known to fit; 0 means none is known to.
        std::size_t tooLong = wanted; // Shortest prefix known not to fit.
        while (tooLong - fitting > 1) {
            const std::size_t middle = fitting + (tooLong - fitting) / 2;
            if (fits(middle)) {
                fitting = middle;
            } else {
                tooLong = middle;
            }
        }
        count = fitting;
    }

    std::optional<core::Summary> summary;
    if (count == 0) {
        // Not even the oldest entry fits a call: the engine writes its summary, so folding always makes progress.
        summary = SummarizeFallback(oldest(1), spec.maxTokens);
    } else {
        summary = SummarizeTurns(systemMessage, oldest(count), spec, *_pBackend, stop);
        if (!summary) {
            return false;
        }
    }
    // The session stores it and makes the folded turns searchable history.
    _storeFold(*summary);
    return true;
}

bool MemoryUpkeep::Condense(const inference::Message& systemMessage, std::stop_token stop)
{
    inference::GenerationSpec spec;
    spec.maxTokens = std::max(1, _pSettings->budget.summaryCap / CONDENSE_SUMMARY_DIVISOR);
    spec.sampling = SamplingFrom(_pSettings->resolveSampling, core::SamplingSeed(_pStore->Meta().rngSeed, _turn,
                                                                                core::CallKind::Summarize, _attempt));
    const std::vector<core::Summary> summaries = _pStore->Summaries();

    // The condense prompt holds every summary in its system message. Summaries written under a larger summary_cap
    // can make it larger than the context: then the engine condenses them itself, so the loop still makes progress.
    const int promptTokens = _pBackend->CountPromptTokens(CondensePrompt(systemMessage));
    std::optional<core::Summary> merged;
    if (spec.maxTokens <= _pBackend->ContextSize() - promptTokens) {
        merged = CondenseSummaries(systemMessage, summaries, spec, *_pBackend, stop);
        if (!merged) {
            return false;
        }
    } else {
        merged = CondenseFallback(summaries, spec.maxTokens);
    }
    _pStore->ReplaceSummaries(*merged);
    return true;
}

} // namespace morpheus::turn
```

In `src/turn/CMakeLists.txt`, replace

```cmake
    Folding.cpp
    Replay.cpp
```

with

```cmake
    Folding.cpp
    MemoryUpkeep.cpp
    Replay.cpp
```

- [ ] **Step 6: Make Session call the upkeep**

Replace the whole of `src/turn/Session.h` with:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/PromptBuilder.h"
#include "core/MemoryMode.h"
#include "core/Settings.h"
#include "core/TurnRecord.h"
#include "core/World.h"
#include "inference/Llm.h"
#include "store/RunStore.h"
#include "turn/MemoryUpkeep.h"
#include "turn/Retrieval.h"

namespace morpheus::turn {

constexpr std::size_t RETRIEVAL_MAX_HITS = 6; // Retrieval hits in the tail (T2), before retrieval_cap applies.
constexpr std::size_t LOOKUP_MAX_HITS = 2;    // Hits each assisted-mode lookup adds.
constexpr int LOOKUP_MAX_TOKENS = 192;        // Generation cap for the lookup call.
// Added to a turn's attempt for the seed of its second review, so that seed never equals a regenerated turn's first
// review seed (attempt + 1). PlayTurn requires attempt < REVIEW_RETRY_OFFSET, which keeps the sum within
// SamplingSeed's limit of 2^24.
constexpr int REVIEW_RETRY_OFFSET = 1 << 23;

// The sampling for one call from its settings.json values and the call's seed (core::SamplingSeed).
inference::Sampling SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed);

struct TurnCallbacks {
    // After apply, before narrate: the record has its checks and applied and rejected changes, no narration yet.
    std::function<void(const core::TurnRecord&)> onResolved = {};
    // Every narration's pieces as they are written, the renarrate's included.
    inference::TokenCallback onNarrationToken = {};
    // The review found that the narration contradicts the outcome (`problems` are its reasons): the narration starts
    // over, and the new one streams through onNarrationToken. At most once per turn.
    std::function<void(const std::vector<std::string>& problems)> onNarrationRestarted = {};
};

// One open run: its world, settings, database and memory index, and the turn loop (architecture spec, "Turn flow").
// Every backend call happens inside these methods, on the caller's thread (the engine worker). Not thread-safe.
class Session {
private:
    core::World _world = {}; // Set by the constructor.
    core::Settings _settings = {};
    core::MemoryMode _mode = core::MemoryMode::Engine;
    store::RunStore _store; // Set by the constructor.
    inference::LlmBackend* _pBackend = nullptr; // Not owned; outlives the session.
    MemoryIndex _index;     // Set by the constructor.
    context::Assembly _lastAssembly = {};
    bool _worldChanged = false;

public:
    // Starts a new run in `file` from the world's initial state, with `seed` for dice and sampling, and indexes the
    // lore. Throws StoreError if the file exists, and DataError if lore cannot be embedded (then no file is left).
    static Session Start(core::World world, core::Settings settings, const std::filesystem::path& file,
                         std::uint64_t seed, inference::LlmBackend& backend);

    // Opens a run of `world`. Throws DataError if the run belongs to another world or its latest state no longer
    // validates against the world's schema. Edited world files set WorldChanged(); edited lore is re-embedded.
    static Session Load(core::World world, core::Settings settings, const std::filesystem::path& file,
                        inference::LlmBackend& backend);

    // Plays one turn: retrieve, (assisted mode: look up), resolve, apply, narrate, (narration_check or
    // compact_transcript: review, and at most one renarrate with its own review), commit. Control characters other
    // than newline and tab are stripped from `input` and it is trimmed. Returns the committed record, or nullopt
    // when `stop` cancelled the turn. Throws TurnError for an empty input or one that does not fit, DataError when
    // summary_cap is too small for even a condensed summary, and InferenceError from the backend. A cancelled or
    // failed turn commits no turn, but memory upkeep its fit loop finished before the stop (summaries and their
    // history embeddings) is kept: it only covers committed turns.
    std::optional<core::TurnRecord> RunTurn(std::string_view input, const TurnCallbacks& callbacks,
                                            std::stop_token stop);

    // Removes the latest turn, with its notes and any summary covering it, from the run and from retrieval.
    // Throws TurnError when there is none.
    void Undo();

    // Plays the latest turn's input again as a new attempt: same dice, newly sampled text. If it is cancelled or
    // fails, the previous turn is put back with the summaries that covered it. Throws TurnError when there is no
    // turn.
    std::optional<core::TurnRecord> Regenerate(const TurnCallbacks& callbacks, std::stop_token stop);

    // Between-turn upkeep: folds or condenses while the transcript or the summary is over budget, then indexes
    // folded turns and notes. Returns false if `stop` cancelled it (it can simply run again later). Throws DataError
    // when summary_cap is too small for even a condensed summary.
    bool Maintain(std::stop_token stop);

    const store::RunMeta& Meta() const;
    bool WorldChanged() const;
    nlohmann::json State();                  // After the latest turn.
    std::vector<core::TurnRecord> History(); // Every committed turn, oldest first.
    std::vector<core::Summary> Summaries();
    std::vector<core::Note> Notes(); // Every committed note, by turn, then the order written.
    const context::Assembly& LastAssembly() const; // The last resolve prompt, for the context inspector.

private:
    Session(core::World world, core::Settings settings, store::RunStore store, MemoryIndex index,
            inference::LlmBackend& backend);

    // RunTurn and Regenerate: plays turn LatestTurn()+1 for a cleaned input.
    std::optional<core::TurnRecord> PlayTurn(const std::string& input, int attempt, const TurnCallbacks& callbacks,
                                             std::stop_token stop);

    // One pass of the memory upkeep: the prompt for `hits` and the given tail parts, with the summaries and the
    // unfolded transcript read from the store as it is now, and the transcript entries it shows.
    FitPass Pass(const nlohmann::json& state, const std::vector<RetrievalHit>& hits, const std::string& input,
                 std::string_view instruction);

    // The shared memory upkeep for turn `turn`'s memory calls: a fold's summary joins the store, and the turns it
    // covers become searchable history.
    MemoryUpkeep Upkeep(int turn, int attempt);

    // The last turn covered by a summary, 0 when none.
    int FoldedThrough();
};

} // namespace morpheus::turn
```

In `src/turn/Session.cpp`:

1. Replace

   ```cpp
   #include "core/Dice.h"
   #include "core/Errors.h"
   #include "core/Text.h"
   #include "turn/EngineText.h"
   #include "turn/Folding.h"
   #include "turn/Resolve.h"
   ```

   with

   ```cpp
   #include "context/Transcript.h"
   #include "core/Dice.h"
   #include "core/Errors.h"
   #include "core/Text.h"
   #include "turn/EngineText.h"
   #include "turn/Resolve.h"
   ```

2. Delete the function `CondensedOverCap` from the anonymous namespace (its comment `// Thrown when a summary is still over summary_cap right after a condense: condensing again would only repeat it.`, the function and the blank line after it). It lives in `MemoryUpkeep.cpp` now.

3. Replace the whole body of `bool Session::Maintain(std::stop_token stop)` (from its opening brace to its closing brace) with:

   ```cpp
   {
       const int latest = _store.LatestTurn();
       const nlohmann::json state = _store.StateAt(latest);
       // The next input is unknown: a placeholder tail measures the stable layers. The turn checks the real tail.
       auto assemble = [&](const std::vector<RetrievalHit>&) {
           return Pass(state, {}, std::string(MAINTENANCE_INPUT), RESOLVE_INSTRUCTION);
       };
       if (!Upkeep(latest, 0).Settle(assemble, stop)) {
           return false;
       }
       _index.SyncHistory(_store, FoldedThrough(), *_pBackend);
       return true;
   }
   ```

4. In `PlayTurn`, replace

   ```cpp
           std::optional<context::Assembly> lookupAssembly =
               AssembleFitting(state, hits, input, LOOKUP_INSTRUCTION, turn, attempt, stop);
   ```

   with

   ```cpp
           std::optional<context::Assembly> lookupAssembly = Upkeep(turn, attempt).Fit(
               hits, [&](const std::vector<RetrievalHit>& kept) { return Pass(state, kept, input, LOOKUP_INSTRUCTION); },
               stop);
   ```

   and replace

   ```cpp
       std::optional<context::Assembly> assembly =
           AssembleFitting(state, hits, input, RESOLVE_INSTRUCTION, turn, attempt, stop);
   ```

   with

   ```cpp
       std::optional<context::Assembly> assembly = Upkeep(turn, attempt).Fit(
           hits, [&](const std::vector<RetrievalHit>& kept) { return Pass(state, kept, input, RESOLVE_INSTRUCTION); },
           stop);
   ```

5. Replace everything from `context::ContextInputs Session::Inputs(const nlohmann::json& state, const std::vector<RetrievalHit>& hits,` up to (not including) `int Session::FoldedThrough()` (the definitions of `Inputs`, `AssembleFitting`, `FoldOldest` and `Condense`) with:

   ```cpp
   FitPass Session::Pass(const nlohmann::json& state, const std::vector<RetrievalHit>& hits, const std::string& input,
                         std::string_view instruction)
   {
       context::ContextInputs inputs;
       inputs.engineRules = std::string(ENGINE_RULES);
       if (_mode == core::MemoryMode::Assisted) {
           inputs.engineRules += "\n\n" + std::string(ASSISTED_RULES);
       }
       inputs.summaries = _store.Summaries();
       // L3: the unfolded turns. With the compact transcript on, older turns show their digest; otherwise every turn
       // shows its narration. A fold always reads the full narration (TranscriptEntry::foldMessages).
       const int latest = _store.LatestTurn();
       const int firstUnfolded = FoldedThrough() + 1;
       if (firstUnfolded <= latest) {
           const int proseTurns = _settings.compactTranscript ? _settings.proseTurns : std::numeric_limits<int>::max();
           inputs.transcript = context::TranscriptEntries(_store.Turns(firstUnfolded, latest), proseTurns);
       }
       inputs.state = state;
       for (const RetrievalHit& hit : hits) {
           inputs.retrieved.push_back(hit.text);
       }
       inputs.playerInput = input;
       inputs.instruction = std::string(instruction);

       FitPass pass;
       pass.assembly = context::AssembleResolvePrompt(_world, inputs, _settings.budget, *_pBackend);
       pass.transcript = std::move(inputs.transcript);
       return pass;
   }

   MemoryUpkeep Session::Upkeep(int turn, int attempt)
   {
       // A fold's summary joins the store, and the turns it covers leave the transcript and become searchable history.
       return MemoryUpkeep(_store, *_pBackend, _settings, "the input and the state", turn, attempt,
                           [this](const core::Summary& summary) {
                               _store.AddSummary(summary);
                               _index.SyncHistory(_store, summary.toTurn, *_pBackend);
                           });
   }

   ```

Check: `grep -n "AssembleFitting\|FoldOldest\|Inputs(\|CondensedOverCap" src/turn/Session.h src/turn/Session.cpp` prints nothing.

- [ ] **Step 7: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[upkeep],[folding],[context],[session],[retrieval],[replay]"`
Expected: builds with no warnings; `All tests passed`. No `Session` test was changed.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 446` (427 + 7 + 5 + 7), the three `[server]` tests "Not Run (Skipped)".

- [ ] **Step 8: Build the dev targets**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings (the tools and the app use `Session`'s public API and `context::ContextInputs` without a transcript, both unchanged for them).

- [ ] **Step 9: Byte check and commit**

```bash
FILES="src/context/Transcript.h src/context/Transcript.cpp src/context/PromptBuilder.h src/context/PromptBuilder.cpp src/turn/Folding.h src/turn/Folding.cpp src/turn/MemoryUpkeep.h src/turn/MemoryUpkeep.cpp src/turn/Session.h src/turn/Session.cpp src/turn/CMakeLists.txt tests/turn/MemoryUpkeepTests.cpp tests/context/RenderTests.cpp tests/context/PromptBuilderTests.cpp tests/turn/FoldingTests.cpp tests/app/ModelSeatTests.cpp"
LC_ALL=C grep -c $'[\x80-\xFF]' $FILES
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES
git add $FILES
git commit -m "Move Session's fit loop, fold and condense into turn::MemoryUpkeep, working on rendered transcript entries" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

### Task 4: `turn::HostedWorld`: host data, observations, the proposal and its adjudication

R5's three operations over a run's host data. The constructor checks the catalog, the observation schema and the lore; `Observe` validates an observation (the fixed core of actors, then the host's schema); `ProposalSchema` / `ProposalGrammar` build the speaker's possible actions for one turn; `Adjudicate` marks the model's proposal requested or refused. The fixtures `open.json` and `observation.json` are written from section 5 of the requirements.

**Files:**
- Create: `src/turn/HostedWorld.h`, `src/turn/HostedWorld.cpp`
- Modify: `src/turn/CMakeLists.txt`
- Create: `tests/hosted/fixtures/open.json`, `tests/hosted/fixtures/observation.json`
- Create: `tests/hosted/HostedTestSupport.h`, `tests/hosted/HostedWorldTests.cpp`

**Interfaces:**
- Consumes: `grammar::Validate`, `grammar::CheckValidationSchema` (Task 1); `core::Participant`, `core::Adjudication` (Task 2).
- Produces (`turn/HostedWorld.h`): `struct turn::HostData { nlohmann::json catalog; nlohmann::json observationSchema; std::vector<core::LoreEntry> lore; }`; `nlohmann::json turn::HostDataToJson(const HostData&)`; `HostData turn::HostDataFromJson(const nlohmann::json&)`; `std::string turn::HostDataHash(const HostData&)`; `struct turn::Actor { std::string id; std::string name; bool isPlayer; std::string bio; }`; `struct turn::Observation { nlohmann::json json; std::vector<Actor> actors; const Actor* Find(std::string_view id) const; const Actor& Player() const; }`; `using turn::Participants = std::vector<core::Participant>`; `class turn::HostedWorld` with `explicit HostedWorld(const HostData& data)`, `const std::vector<core::LoreEntry>& Lore() const`, `std::vector<std::string> ActionIds() const`, `Observation Observe(const nlohmann::json& observation) const`, `static std::vector<Actor> ActorsOf(const nlohmann::json& observation)`, `static void CheckEvents(const std::vector<nlohmann::json>& events)`, `nlohmann::json ProposalSchema(const Observation&, const Participants&, const std::vector<std::string>& allowed, std::string_view speakerId) const`, `std::string ProposalGrammar(...)` (same parameters), `core::Adjudication Adjudicate(const nlohmann::json& proposalSchema, std::string_view modelText) const`.
- Produces (`tests/hosted/HostedTestSupport.h`, namespace `morpheus::test`): `HostedFixturesDir()`, `ReadFixture(std::string_view name)`, `FixtureHostData()`, `FixtureObservation()`, and the ids `HULDA` (`"0001A6A4"`), `SAADIA` (`"0001B07C"`), `AELA` (`"00000007"`, the player).

- [ ] **Step 1: Write the fixtures and the failing tests**

Create `tests/hosted/fixtures/open.json` (the action catalog of section 5.2, a JSON Schema for the observation of section 5.1 within the extended `Validate` subset, and lore):

```json
{
  "catalog": {
    "actions": [
      {"id": "none"},
      {"id": "follow", "params": {}},
      {"id": "wait", "params": {}},
      {"id": "attack", "params": {"target": {"type": "actor", "scope": "present_non_player"}}},
      {"id": "give_item", "params": {"item": {"type": "string", "maxLength": 48}}},
      {"id": "end_conversation"}
    ]
  },
  "observation_schema": {
    "type": "object",
    "required": ["game_time", "location", "actors"],
    "properties": {
      "game_time": {
        "type": "object",
        "required": ["days_passed", "hour"],
        "properties": {
          "days_passed": {"type": "number", "minimum": 0},
          "hour": {"type": "integer", "minimum": 0, "maximum": 23}
        }
      },
      "location": {"type": "string"},
      "weather": {"type": "string"},
      "actors": {
        "type": "array",
        "minItems": 1,
        "items": {
          "type": "object",
          "required": ["id", "name", "is_player"],
          "additionalProperties": true,
          "properties": {
            "id": {"type": "string", "minLength": 1},
            "name": {"type": "string", "minLength": 1},
            "is_player": {"type": "boolean"},
            "bio": {"type": "string"},
            "description": {"type": "string"},
            "gender": {"type": "string"},
            "race": {"type": "string"},
            "relationship_rank": {"type": "integer", "minimum": -4, "maximum": 4},
            "in_combat": {"type": "boolean"},
            "hostile": {"type": "boolean"}
          }
        }
      },
      "nearby": {
        "type": "array",
        "items": {
          "type": "object",
          "required": ["id", "name"],
          "additionalProperties": true,
          "properties": {
            "id": {"type": "string"},
            "name": {"type": "string"},
            "distance_m": {"type": "number", "minimum": 0}
          }
        }
      },
      "events": {"type": "array", "items": {"type": "string"}}
    }
  },
  "lore": [
    {"subject": "Whiterun", "data": "A city of the plains, ruled from its keep on the hill.", "always_on": true},
    {"subject": "The Bannered Mare", "data": "The inn of Whiterun, kept by Hulda.", "always_on": true},
    {"subject": "Hulda", "data": "Hulda has kept the Bannered Mare for years and hears every rumour in town.", "always_on": false},
    {"subject": "Saadia", "data": "Saadia came from across the desert and hides from men who hunt her.", "always_on": false}
  ]
}
```

Create `tests/hosted/fixtures/observation.json` (section 5.1's example, with Saadia among the actors):

```json
{
  "game_time": {"days_passed": 213.42, "hour": 14},
  "location": "Whiterun, The Bannered Mare",
  "weather": "clear",
  "actors": [
    {"id": "0001A6A4", "name": "Hulda", "is_player": false, "gender": "female", "race": "Nord",
     "relationship_rank": 1, "in_combat": false, "hostile": false, "voice_type": "FemaleEvenToned",
     "equipment": {"body": "Barkeeper's Clothes"}, "bio": "Hulda keeps the Bannered Mare."},
    {"id": "0001B07C", "name": "Saadia", "is_player": false, "gender": "female", "race": "Redguard",
     "relationship_rank": 0, "in_combat": false, "hostile": false, "bio": "Saadia serves at the inn."},
    {"id": "00000007", "name": "Aela", "is_player": true, "description": "A huntress."}
  ],
  "nearby": [{"id": "0001B07D", "name": "Mikael", "distance_m": 6.5}],
  "events": ["Aela picked up an Iron Sword.", "Hulda started following Aela."]
}
```

Create `tests/hosted/HostedTestSupport.h`:

```cpp
#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "turn/HostedWorld.h"

namespace morpheus::test {

// The actor ids of observation.json: Hulda and Saadia, and Aela, the player.
inline const std::string HULDA = "0001A6A4";
inline const std::string SAADIA = "0001B07C";
inline const std::string AELA = "00000007";

// tests/hosted/fixtures in the source tree: host data and requests written from section 5 of the integration
// requirements. The host's recorded fixtures can replace them later.
inline std::filesystem::path HostedFixturesDir()
{
    return RepoDir() / "tests" / "hosted" / "fixtures";
}

// The fixture `name`, parsed. A fixture that cannot be read is a broken test: it throws.
inline nlohmann::json ReadFixture(std::string_view name)
{
    std::ifstream stream(HostedFixturesDir() / name, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("ReadFixture: cannot open '" + std::string(name) + "'");
    }
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return nlohmann::json::parse(text);
}

// open.json: the host data a host sends when it opens a run.
inline turn::HostData FixtureHostData()
{
    return turn::HostDataFromJson(ReadFixture("open.json"));
}

// observation.json: Hulda and Saadia at the inn, and Aela, the player.
inline nlohmann::json FixtureObservation()
{
    return ReadFixture("observation.json");
}

} // namespace morpheus::test
```

Create `tests/hosted/HostedWorldTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "grammar/Check.h"
#include "grammar/Schema.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedWorld.h"

using namespace morpheus;
using namespace nlohmann::literals;
using Catch::Matchers::ContainsSubstring;

namespace {

constexpr char BACKSLASH = '\\';

// The fixture's conversation: Hulda, and Aela, the player.
turn::Participants HuldaAndAela()
{
    return {{test::HULDA, "Hulda", "Hulda keeps the Bannered Mare."}, {test::AELA, "Aela", ""}};
}

// The fixture host data with another catalog.
turn::HostData WithCatalog(const std::string& catalog)
{
    turn::HostData data = test::FixtureHostData();
    data.catalog = nlohmann::json::parse(catalog);
    return data;
}

// The fixture host data with a schema that lets any object through, so a test sees the core's faults alone.
turn::HostData WithOpenSchema()
{
    turn::HostData data = test::FixtureHostData();
    data.observationSchema = R"({"type": "object", "properties": {}, "required": [], "additionalProperties": true})"_json;
    return data;
}

// The fixture observation without Saadia: Hulda and Aela only.
nlohmann::json WithoutSaadia()
{
    nlohmann::json observation = test::FixtureObservation();
    nlohmann::json& actors = observation.at("actors");
    actors.erase(actors.begin() + 1);
    return observation;
}

// The message of the DataError HostedWorld throws for `data`, or "" when it accepts it.
std::string OpenError(const turn::HostData& data)
{
    try {
        const turn::HostedWorld world(data);
    } catch (const core::DataError& error) {
        return error.what();
    }
    return "";
}

// The message of the DataError Observe throws for `observation`, or "" when it accepts it.
std::string ObserveError(const turn::HostedWorld& world, const nlohmann::json& observation)
{
    try {
        world.Observe(observation);
    } catch (const core::DataError& error) {
        return error.what();
    }
    return "";
}

// The action ids a proposal schema offers, in order.
std::vector<std::string> OfferedIds(const nlohmann::json& schema)
{
    std::vector<std::string> ids;
    for (const nlohmann::json& branch : schema.at("oneOf")) {
        ids.push_back(branch.at("properties").at("action").at("const").get<std::string>());
    }
    return ids;
}

} // namespace

TEST_CASE("Host data reads and writes its JSON form and hashes its content", "[hosted]")
{
    const nlohmann::json fixture = test::ReadFixture("open.json");
    const turn::HostData data = turn::HostDataFromJson(fixture);
    CHECK(data.catalog == fixture.at("catalog"));
    CHECK(data.observationSchema == fixture.at("observation_schema"));
    REQUIRE(data.lore.size() == 4);
    CHECK(data.lore[0].subject == "Whiterun");
    CHECK(data.lore[0].alwaysOn);
    CHECK_FALSE(data.lore[2].alwaysOn);
    CHECK(turn::HostDataToJson(data) == fixture);

    // The hash follows the content.
    CHECK(turn::HostDataHash(data) == turn::HostDataHash(turn::HostDataFromJson(fixture)));
    CHECK(turn::HostDataHash(data).size() == 16);
    turn::HostData changed = data;
    changed.lore[2].data = "Hulda has kept the inn for twenty years.";
    CHECK(turn::HostDataHash(changed) != turn::HostDataHash(data));

    // Lore may be absent, and always_on defaults to false.
    CHECK(turn::HostDataFromJson(R"({"catalog": {}, "observation_schema": {}})"_json).lore.empty());
    CHECK_FALSE(turn::HostDataFromJson(
                    R"({"catalog": {}, "observation_schema": {}, "lore": [{"subject": "a", "data": "b"}]})"_json)
                    .lore[0]
                    .alwaysOn);

    // A wrong shape is a DataError naming the pointer.
    CHECK_THROWS_WITH(turn::HostDataFromJson(R"([])"_json), "host data at the root: host data must be a JSON object");
    CHECK_THROWS_WITH(turn::HostDataFromJson(R"({"observation_schema": {}})"_json),
                      "host data at /catalog: missing: the action catalog is required");
    CHECK_THROWS_WITH(turn::HostDataFromJson(R"({"catalog": {}})"_json),
                      "host data at /observation_schema: missing: the observation's JSON Schema is required");
    CHECK_THROWS_WITH(turn::HostDataFromJson(R"({"catalog": {}, "observation_schema": {}, "mods": []})"_json),
                      "host data at /mods: unknown key 'mods'");
    CHECK_THROWS_WITH(turn::HostDataFromJson(R"({"catalog": {}, "observation_schema": {}, "lore": {}})"_json),
                      "host data at /lore: 'lore' must be an array");
    CHECK_THROWS_WITH(
        turn::HostDataFromJson(R"({"catalog": {}, "observation_schema": {}, "lore": [{"subject": "a"}]})"_json),
        "host data at /lore/0/data: 'data' must be a string");
    CHECK_THROWS_WITH(turn::HostDataFromJson(
                          R"({"catalog": {}, "observation_schema": {}, "lore": [{"subject": "a", "data": "b", "always_on": "yes"}]})"_json),
                      "host data at /lore/0/always_on: 'always_on' must be true or false");
}

TEST_CASE("The catalog gives the proposal schema of section 3, and its grammar takes the right strings", "[hosted]")
{
    const turn::HostedWorld world(test::FixtureHostData());
    const turn::Observation observation = world.Observe(test::FixtureObservation());
    CHECK(world.ActionIds() ==
          std::vector<std::string>{"none", "follow", "wait", "attack", "give_item", "end_conversation"});

    // Hulda speaks: attack's present_non_player target is Saadia alone (never the speaker, never the player).
    const nlohmann::json schema = world.ProposalSchema(observation, HuldaAndAela(), world.ActionIds(), test::HULDA);
    const nlohmann::json expected = R"({"oneOf": [
        {"type": "object", "required": ["action", "params"], "properties": {"action": {"const": "none"},
            "params": {"type": "object", "properties": {}, "required": []}}},
        {"type": "object", "required": ["action", "params"], "properties": {"action": {"const": "follow"},
            "params": {"type": "object", "properties": {}, "required": []}}},
        {"type": "object", "required": ["action", "params"], "properties": {"action": {"const": "wait"},
            "params": {"type": "object", "properties": {}, "required": []}}},
        {"type": "object", "required": ["action", "params"], "properties": {"action": {"const": "attack"},
            "params": {"type": "object", "properties": {"target": {"enum": ["0001B07C"]}}, "required": ["target"]}}},
        {"type": "object", "required": ["action", "params"], "properties": {"action": {"const": "give_item"},
            "params": {"type": "object", "properties": {"item": {"type": "string", "maxLength": 48}}, "required": ["item"]}}},
        {"type": "object", "required": ["action", "params"], "properties": {"action": {"const": "end_conversation"},
            "params": {"type": "object", "properties": {}, "required": []}}}
    ]})"_json;
    CHECK(schema == expected);

    // The grammar is FromSchema's, and it takes the model's compact JSON only.
    const std::string gbnf = world.ProposalGrammar(observation, HuldaAndAela(), world.ActionIds(), test::HULDA);
    CHECK(gbnf == grammar::FromSchema(expected));
    CHECK(grammar::Check(gbnf, R"({"action":"follow","params":{}})").empty());
    CHECK(grammar::Check(gbnf, R"({"action":"attack","params":{"target":"0001B07C"}})").empty());
    CHECK(grammar::Check(gbnf, R"({"action":"give_item","params":{"item":"Iron Sword"}})").empty());
    CHECK_FALSE(grammar::Check(gbnf, R"({"action":"wait","params":{},"intent":"rest"})").empty());
    CHECK_FALSE(grammar::Check(gbnf, R"({"action":"attack","params":{"target":"0001A6A4"}})").empty());
    CHECK_FALSE(grammar::Check(gbnf, R"({"action":"fly","params":{}})").empty());
    CHECK_FALSE(grammar::Check(gbnf, R"({"action": "none", "params": {}})").empty());
    CHECK_FALSE(
        grammar::Check(gbnf, R"({"action":"give_item","params":{"item":")" + std::string(49, 'x') + R"("}})").empty());
}

TEST_CASE("Each actor scope offers its actors, never the speaker, each once", "[hosted]")
{
    const turn::HostedWorld world(WithCatalog(R"({"actions": [
        {"id": "greet", "params": {"whom": {"type": "actor", "scope": "present"}}},
        {"id": "attack", "params": {"target": {"type": "actor", "scope": "present_non_player"}}},
        {"id": "beckon", "params": {"companion": {"type": "actor", "scope": "participant"}}}]})"));
    const turn::Observation observation = world.Observe(test::FixtureObservation());
    // Hulda and Aela take part; Aela is listed twice to show the enum keeps each id once.
    const turn::Participants participants = {{test::HULDA, "Hulda", ""}, {test::AELA, "Aela", ""}, {test::AELA, "Aela", ""}};
    auto targets = [](const nlohmann::json& schema, std::size_t branch, const char* param) {
        return schema.at("oneOf")[branch].at("properties").at("params").at("properties").at(param).at("enum");
    };

    // Hulda speaks: present is Saadia and Aela, in observation order; present_non_player is Saadia; participant is
    // Aela.
    const nlohmann::json hulda = world.ProposalSchema(observation, participants, world.ActionIds(), test::HULDA);
    REQUIRE(hulda.at("oneOf").size() == 3);
    CHECK(targets(hulda, 0, "whom") == nlohmann::json::array({test::SAADIA, test::AELA}));
    CHECK(targets(hulda, 1, "target") == nlohmann::json::array({test::SAADIA}));
    CHECK(targets(hulda, 2, "companion") == nlohmann::json::array({test::AELA}));

    // Saadia speaks: Hulda is present, and a participant.
    const nlohmann::json saadia = world.ProposalSchema(observation, participants, world.ActionIds(), test::SAADIA);
    CHECK(targets(saadia, 0, "whom") == nlohmann::json::array({test::HULDA, test::AELA}));
    CHECK(targets(saadia, 1, "target") == nlohmann::json::array({test::HULDA}));
    CHECK(targets(saadia, 2, "companion") == nlohmann::json::array({test::HULDA, test::AELA}));
}

TEST_CASE("An action without a target this turn is left out, and allowedActions narrows the catalog", "[hosted]")
{
    const turn::HostedWorld world(test::FixtureHostData());
    const turn::Observation alone = world.Observe(WithoutSaadia());
    // Only Aela, the player, is there besides Hulda: attack's present_non_player target has no one.
    CHECK(OfferedIds(world.ProposalSchema(alone, HuldaAndAela(), world.ActionIds(), test::HULDA)) ==
          std::vector<std::string>{"none", "follow", "wait", "give_item", "end_conversation"});

    // allowedActions keeps the catalog's order and leaves the rest out.
    const turn::Observation observation = world.Observe(test::FixtureObservation());
    CHECK(OfferedIds(world.ProposalSchema(observation, HuldaAndAela(), {"attack", "follow"}, test::HULDA)) ==
          std::vector<std::string>{"follow", "attack"});

    // An unknown or repeated id, or nothing left to do, is the host's DataError naming the field.
    CHECK_THROWS_WITH(world.ProposalSchema(observation, HuldaAndAela(), {"follow", "fly"}, test::HULDA),
                      "allowed_actions/1: 'fly' is not an action of the catalog");
    CHECK_THROWS_WITH(world.ProposalSchema(observation, HuldaAndAela(), {"follow", "follow"}, test::HULDA),
                      "allowed_actions/1: 'follow' is listed twice");
    CHECK_THROWS_AS(world.ProposalSchema(alone, HuldaAndAela(), {"attack"}, test::HULDA), core::DataError);
    CHECK_THROWS_WITH(world.ProposalSchema(alone, HuldaAndAela(), {"attack"}, test::HULDA),
                      ContainsSubstring("allowed_actions: no allowed action"));
}

TEST_CASE("The catalog is checked at open, naming the pointer of its first fault", "[hosted]")
{
    CHECK(OpenError(test::FixtureHostData()).empty());
    CHECK(OpenError(WithCatalog(R"([])")) == "catalog at the root: the catalog must be a JSON object with 'actions'");
    CHECK(OpenError(WithCatalog(R"({"actions": [], "version": 2})")) ==
          "catalog at /version: 'version' does not apply to the catalog");
    CHECK(OpenError(WithCatalog(R"({"actions": []})")) == "catalog at /actions: 'actions' must be a non-empty array");
    CHECK(OpenError(WithCatalog(R"({"actions": ["none"]})")) == "catalog at /actions/0: an action must be a JSON object");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": ""}]})")) ==
          "catalog at /actions/0/id: an action needs 'id', a non-empty string");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "none"}, {"id": "none"}]})")) ==
          "catalog at /actions/1/id: the id 'none' is repeated: /actions/0 has it too");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "none", "label": "Nothing"}]})")) ==
          "catalog at /actions/0/label: 'label' does not apply to an action");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": []}]})")) ==
          "catalog at /actions/0/params: 'params' must be a JSON object");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": {"p": 3}}]})")) ==
          "catalog at /actions/0/params/p: a parameter must be a JSON object");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": {"p": {"type": "float"}}}]})")) ==
          "catalog at /actions/0/params/p/type: 'type' must be 'actor', 'string', 'integer' or 'enum'");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": {"p": {"type": "actor", "scope": "nearby"}}}]})")) ==
          "catalog at /actions/0/params/p/scope: 'scope' must be 'present', 'present_non_player' or 'participant'");
    CHECK(OpenError(WithCatalog(
              R"({"actions": [{"id": "x", "params": {"p": {"type": "actor", "scope": "present", "maxLength": 3}}}]})")) ==
          "catalog at /actions/0/params/p/maxLength: 'maxLength' does not apply to an actor parameter");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": {"p": {"type": "string"}}}]})")) ==
          "catalog at /actions/0/params/p/maxLength: a string parameter needs 'maxLength', an integer from 0 to 1999");
    CHECK(OpenError(WithCatalog(
              R"({"actions": [{"id": "x", "params": {"p": {"type": "string", "maxLength": 4, "minLength": 5}}}]})")) ==
          "catalog at /actions/0/params/p/minLength: 'minLength' must be an integer from 0 to 4 (the 'maxLength')");
    CHECK(OpenError(WithCatalog(
              R"({"actions": [{"id": "x", "params": {"p": {"type": "integer", "minimum": 5, "maximum": 3}}}]})")) ==
          "catalog at /actions/0/params/p: 'minimum' 5 is above 'maximum' 3");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": {"p": {"type": "integer", "maximum": 3}}}]})")) ==
          "catalog at /actions/0/params/p/minimum: an integer parameter needs 'minimum', an integer");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": {"p": {"type": "enum", "values": []}}}]})")) ==
          "catalog at /actions/0/params/p/values: 'values' must be a non-empty array of strings");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": {"p": {"type": "enum", "values": ["a", "a"]}}}]})")) ==
          "catalog at /actions/0/params/p/values/1: 'a' is listed twice");

    // Integer and enum parameters reach the proposal as their own schemas, every parameter required.
    const turn::HostedWorld paying(WithCatalog(R"({"actions": [{"id": "pay", "params": {
        "gold": {"type": "integer", "minimum": 1, "maximum": 100},
        "mood": {"type": "enum", "values": ["calm", "angry"]}}}]})"));
    const nlohmann::json pay =
        paying.ProposalSchema(paying.Observe(test::FixtureObservation()), HuldaAndAela(), {"pay"}, test::HULDA);
    const nlohmann::json& params = pay.at("oneOf")[0].at("properties").at("params");
    CHECK(params.at("properties").at("gold") == R"({"type": "integer", "minimum": 1, "maximum": 100})"_json);
    CHECK(params.at("properties").at("mood") == R"({"enum": ["calm", "angry"]})"_json);
    CHECK(params.at("required") == R"(["gold", "mood"])"_json);
}

TEST_CASE("The observation schema is checked at open against Validate's extended subset", "[hosted]")
{
    turn::HostData data = test::FixtureHostData();
    data.observationSchema =
        R"({"type": "object", "required": [], "properties": {"location": {"type": "string", "pattern": "^W"}}})"_json;
    CHECK(OpenError(data) ==
          "observation_schema: schema at /properties/location/pattern: the keyword 'pattern' is not supported");
    data.observationSchema = R"({"type": "object", "required": [], "properties": {"weather": {"type": "null"}}})"_json;
    CHECK(OpenError(data) == "observation_schema: schema at /properties/weather/type: the type 'null' is not supported");
    // Numbers, booleans, optional keys, open objects and unbounded strings and arrays are in the subset: the
    // fixture's schema uses each.
    data.observationSchema = test::FixtureHostData().observationSchema;
    CHECK(OpenError(data).empty());
}

TEST_CASE("The host's lore is checked at open", "[hosted]")
{
    turn::HostData data = test::FixtureHostData();
    data.lore[1].subject = "";
    CHECK(OpenError(data) == "lore at /1/subject: a lore entry needs a subject");
    data = test::FixtureHostData();
    data.lore[3].subject = "Whiterun";
    CHECK(OpenError(data) == "lore at /3/subject: the subject 'Whiterun' is repeated: /0 has it too");
    data = test::FixtureHostData();
    data.lore[0].data = "";
    CHECK(OpenError(data) == "lore at /0/data: a lore entry needs data");
    CHECK(turn::HostedWorld(test::FixtureHostData()).Lore().size() == 4);
}

TEST_CASE("Observe reads the actors and validates the rest against the host's schema", "[hosted]")
{
    const turn::HostedWorld world(test::FixtureHostData());
    const nlohmann::json json = test::FixtureObservation();
    const turn::Observation observation = world.Observe(json);
    CHECK(observation.json == json);
    REQUIRE(observation.actors.size() == 3);
    CHECK(observation.actors[0].id == test::HULDA);
    CHECK(observation.actors[0].name == "Hulda");
    CHECK_FALSE(observation.actors[0].isPlayer);
    CHECK(observation.actors[0].bio == "Hulda keeps the Bannered Mare.");
    CHECK(observation.actors[2].bio.empty()); // The player has a description, not a bio.
    CHECK(observation.Player().id == test::AELA);
    REQUIRE(observation.Find(test::SAADIA) != nullptr);
    CHECK(observation.Find(test::SAADIA)->name == "Saadia");
    CHECK(observation.Find("00000000") == nullptr);
    CHECK(turn::HostedWorld::ActorsOf(json).size() == 3);

    // The rest is the host's to describe: each field its schema refuses is a fault, all of them reported.
    nlohmann::json wrong = json;
    wrong["weather"] = 3;
    wrong["mood"] = "tense";
    const std::string message = ObserveError(world, wrong);
    CHECK_THAT(message, ContainsSubstring("observation at /weather: expected a string, got 3"));
    CHECK_THAT(message, ContainsSubstring("; observation at /mood: unexpected key 'mood'"));
}

TEST_CASE("Each fault of the observation core is a DataError naming its pointer", "[hosted]")
{
    const turn::HostedWorld world(WithOpenSchema());
    // The fixture observation with the field `key` of actor `index` set to `value`.
    auto withActor = [](std::size_t index, const char* key, const nlohmann::json& value) {
        nlohmann::json observation = test::FixtureObservation();
        observation.at("actors").at(index)[key] = value;
        return observation;
    };
    CHECK_THAT(ObserveError(world, nlohmann::json::array()),
               ContainsSubstring("observation at the root: the observation must be a JSON object"));
    CHECK(ObserveError(world, R"({"location": "inn"})"_json) ==
          "observation at /actors: the observation needs 'actors', an array");
    nlohmann::json notAnActor = test::FixtureObservation();
    notAnActor.at("actors").push_back(3);
    CHECK(ObserveError(world, notAnActor) == "observation at /actors/3: an actor must be a JSON object");
    CHECK(ObserveError(world, withActor(0, "id", "")) ==
          "observation at /actors/0/id: an actor needs 'id', a non-empty string");
    CHECK(ObserveError(world, withActor(1, "name", 7)) ==
          "observation at /actors/1/name: an actor needs 'name', a non-empty string");
    CHECK(ObserveError(world, withActor(1, "is_player", "no")) ==
          "observation at /actors/1/is_player: an actor needs 'is_player', true or false");
    CHECK(ObserveError(world, withActor(0, "bio", 5)) == "observation at /actors/0/bio: 'bio' must be a string");
    CHECK(ObserveError(world, withActor(2, "id", test::HULDA)) ==
          "observation at /actors/2/id: the id '0001A6A4' is repeated: /actors/0 has it too");
    CHECK(ObserveError(world, withActor(0, "is_player", true)) ==
          "observation at /actors: exactly one actor must be the player, not 2");
    CHECK(ObserveError(world, withActor(2, "is_player", false)) ==
          "observation at /actors: exactly one actor must be the player, not 0");

    // Every fault is reported, joined in order.
    nlohmann::json twoFaults = withActor(0, "id", "");
    twoFaults.at("actors").at(1)["bio"] = 5;
    CHECK(ObserveError(world, twoFaults) == "observation at /actors/0/id: an actor needs 'id', a non-empty string; "
                                            "observation at /actors/1/bio: 'bio' must be a string");
    CHECK_THROWS_AS(turn::HostedWorld::ActorsOf(twoFaults), core::DataError);
}

TEST_CASE("Adjudicate requests a valid action and refuses the model's mistakes with their reasons", "[hosted]")
{
    const turn::HostedWorld world(test::FixtureHostData());
    const nlohmann::json schema = world.ProposalSchema(world.Observe(test::FixtureObservation()), HuldaAndAela(),
                                                       world.ActionIds(), test::HULDA);

    const core::Adjudication attack = world.Adjudicate(schema, R"({"action":"attack","params":{"target":"0001B07C"}})");
    CHECK(attack.requested);
    CHECK(attack.actionId == "attack");
    CHECK(attack.params == R"({"target": "0001B07C"})"_json);
    CHECK(attack.reasons.empty());

    // The measured failure the grammar exists for: an "intent" key added to wait.
    const core::Adjudication intent = world.Adjudicate(schema, R"({"action":"wait","params":{},"intent":"rest"})");
    CHECK_FALSE(intent.requested);
    CHECK(intent.actionId.empty());
    CHECK(intent.reasons == std::vector<std::string>{"/intent: unexpected key 'intent'; the keys are 'action', 'params'"});

    const core::Adjudication fly = world.Adjudicate(schema, R"({"action":"fly","params":{}})");
    CHECK_FALSE(fly.requested);
    REQUIRE(fly.reasons.size() == 1);
    CHECK(fly.reasons[0].starts_with("/action: unknown action 'fly'; expected one of: "));

    // A target the turn does not offer: the speaker herself.
    const core::Adjudication self = world.Adjudicate(schema, R"({"action":"attack","params":{"target":"0001A6A4"}})");
    CHECK_FALSE(self.requested);
    CHECK(self.reasons == std::vector<std::string>{R"(/params/target: '0001A6A4' is not one of: "0001B07C")"});

    // Text that does not parse, among it a lone surrogate escape the grammar admits and the JSON parser refuses.
    const core::Adjudication cut = world.Adjudicate(schema, R"({"action":"fol)");
    CHECK_FALSE(cut.requested);
    REQUIRE(cut.reasons.size() == 1);
    CHECK(cut.reasons[0].starts_with("the output is not valid JSON: "));
    const std::string surrogate = std::string(R"({"action":"give_item","params":{"item":")") + BACKSLASH + "ud800\"}}";
    const core::Adjudication lone = world.Adjudicate(schema, surrogate);
    CHECK_FALSE(lone.requested);
    REQUIRE(lone.reasons.size() == 1);
    CHECK(lone.reasons[0].starts_with("the output is not valid JSON: "));
}

TEST_CASE("Events are JSON objects with a text string", "[hosted]")
{
    CHECK_NOTHROW(turn::HostedWorld::CheckEvents(
        {R"({"type": "action_result", "action": "follow", "actor": "0001A6A4", "succeeded": true, "text": "Hulda is now following Aela."})"_json,
         R"({"text": "Rain."})"_json}));
    CHECK_NOTHROW(turn::HostedWorld::CheckEvents({}));
    CHECK_THROWS_WITH(turn::HostedWorld::CheckEvents({R"({"text": "a"})"_json, R"("b")"_json}),
                      "events/1: an event must be a JSON object");
    CHECK_THROWS_WITH(turn::HostedWorld::CheckEvents({R"({"type": "ambient"})"_json}),
                      "events/0/text: an event needs 'text', a string");
    CHECK_THROWS_WITH(turn::HostedWorld::CheckEvents({R"({"text": 3})"_json}),
                      "events/0/text: an event needs 'text', a string");
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile error `Cannot open include file: 'turn/HostedWorld.h'` in `tests/hosted/HostedTestSupport.h`.

- [ ] **Step 3: Write HostedWorld**

Create `src/turn/HostedWorld.h`:

```cpp
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/HostedRecord.h"
#include "core/World.h"

namespace morpheus::turn {

// What the host sends once per run (hosted-world spec, section 1): its action catalog and the JSON Schema of its
// observation, both as the host wrote them, and its lore.
struct HostData {
    nlohmann::json catalog = nlohmann::json::object();
    nlohmann::json observationSchema = nlohmann::json::object();
    std::vector<core::LoreEntry> lore = {};
};

// HostData as a hosted run stores it and as the fixtures write it:
// {"catalog": ..., "observation_schema": ..., "lore": [{"subject": ..., "data": ..., "always_on": ...}]}.
nlohmann::json HostDataToJson(const HostData& data);

// Reads HostDataToJson's form; "lore" may be absent (no lore) and "always_on" defaults to false. Checks the shape only
// (HostedWorld checks the content): throws DataError "host data at <pointer>: <problem>".
HostData HostDataFromJson(const nlohmann::json& json);

// The content hash of `data` (core::ContentHash over HostDataToJson's text) that a hosted run stores: changed host
// data, such as a mod update, has another.
std::string HostDataHash(const HostData& data);

// One actor of an observation, as the engine reads it.
struct Actor {
    std::string id = {};
    std::string name = {};
    bool isPlayer = false;
    std::string bio = {}; // Empty when the observation gives none.
};

// An observation that passed HostedWorld::Observe: the host's JSON as sent, and its actors in their order.
struct Observation {
    nlohmann::json json = nlohmann::json::object();
    std::vector<Actor> actors = {};

    // The actor with `id`, or nullptr when it is not in the observation.
    const Actor* Find(std::string_view id) const;
    // The one actor that is the player. Throws ContractError for an observation without one (Observe refuses those).
    const Actor& Player() const;
};

// A conversation's participants, in join order.
using Participants = std::vector<core::Participant>;

// R5's three operations over the host data of one hosted run (hosted-world spec, section 1): Observe,
// ProposalGrammar and Adjudicate. It never simulates the world: an accepted action is requested, not applied.
class HostedWorld {
private:
    enum class ParamType { Actor, String, Integer, Enum };
    enum class Scope { Present, PresentNonPlayer, Participant };

    // One parameter of a catalog action.
    struct Param {
        std::string name = {};
        ParamType type = ParamType::String;
        Scope scope = Scope::Present;                    // Actor: whose ids a turn offers.
        nlohmann::json schema = nlohmann::json::object(); // String, Integer, Enum: the parameter's proposal schema.
    };

    // One catalog action, its parameters by name.
    struct Action {
        std::string id = {};
        std::vector<Param> params = {};
    };

    std::vector<Action> _actions = {}; // In catalog order.
    nlohmann::json _observationSchema = nlohmann::json::object();
    std::vector<core::LoreEntry> _lore = {};

public:
    // Checks the host data and keeps it. Throws DataError naming the first fault: "catalog at <pointer>: ...",
    // "observation_schema: schema at <pointer>: ..." (the schema must be in grammar::Validate's extended subset) or
    // "lore at /<index>/<field>: ..." (subjects non-empty and unique, data non-empty).
    explicit HostedWorld(const HostData& data);

    const std::vector<core::LoreEntry>& Lore() const;

    // Every action id of the catalog, in order: what a turn allows when the host names no allowed actions.
    std::vector<std::string> ActionIds() const;

    // Validates `observation` and returns it with its parsed core. First the core, which the engine needs whatever the
    // host's schema says: `actors`, an array of objects each with `id` and `name` (non-empty strings), `is_player`
    // (a boolean) and an optional `bio` (a string); ids unique; exactly one player. Then grammar::Validate against the
    // host's schema. Every fault of both is collected; any fault is a DataError
    // "observation at <pointer>: <problem>; observation at ...".
    Observation Observe(const nlohmann::json& observation) const;

    // The core actors of `observation`, read as Observe reads them but without the host's schema, so a stored
    // observation still reads after the host data changed. Throws Observe's DataError for a fault of the core.
    static std::vector<Actor> ActorsOf(const nlohmann::json& observation);

    // Events are JSON objects with a `text` string; their other fields are the host's. Throws DataError
    // "events/<i>: ..." or "events/<i>/text: ..." for the first that is not.
    static void CheckEvents(const std::vector<nlohmann::json>& events);

    // The JSON Schema of the speaker's possible actions this turn (hosted-world spec, section 3): a oneOf over the
    // allowed actions in catalog order, discriminated by "action", each branch {"action": <const id>, "params": {...}}
    // with every parameter required and no extra keys. An actor parameter is an enum of the actor ids its scope gives
    // (present: every actor of the observation; present_non_player: those but the player; participant: the
    // participants present), never the speaker, each once, in observation order; an action whose actor enum would be
    // empty is left out. Throws DataError "allowed_actions/<i>: ..." for an id not in the catalog or listed twice, and
    // "allowed_actions: ..." when no allowed action is left.
    nlohmann::json ProposalSchema(const Observation& observation, const Participants& participants,
                                  const std::vector<std::string>& allowed, std::string_view speakerId) const;

    // grammar::FromSchema of ProposalSchema(...): the compact JSON the resolve call may write.
    std::string ProposalGrammar(const Observation& observation, const Participants& participants,
                                const std::vector<std::string>& allowed, std::string_view speakerId) const;

    // What becomes of the model's proposal: parsed (a parse failure is a refusal: the grammar admits lone surrogate
    // escapes that the JSON parser refuses), then grammar::Validate against `proposalSchema`. A valid proposal is
    // requested with its action id and params; otherwise it is refused, and the reasons are the parse error or each
    // validation error as "<pointer>: <message>". Never throws for the model's text.
    core::Adjudication Adjudicate(const nlohmann::json& proposalSchema, std::string_view modelText) const;
};

} // namespace morpheus::turn
```

Create `src/turn/HostedWorld.cpp`:

```cpp
#include "turn/HostedWorld.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "core/ContentHash.h"
#include "core/Errors.h"
#include "core/JsonInt.h"
#include "grammar/Gbnf.h"
#include "grammar/Schema.h"

namespace morpheus::turn {

namespace {

// `key` as one reference token of a JSON pointer (RFC 6901): "~" is written "~0" and "/" is written "~1".
std::string PointerToken(std::string_view key)
{
    std::string token;
    for (const char c : key) {
        if (c == '~') {
            token += "~0";
        } else if (c == '/') {
            token += "~1";
        } else {
            token += c;
        }
    }
    return token;
}

// One fault of an observation: where it is ("" for the observation itself) and what is wrong.
struct Fault {
    std::string pointer = {};
    std::string problem = {};
};

// Reads the observation's core (its actors), adding a Fault for every mistake, and returns the actors read whole, in
// order. Observe and ActorsOf both read the core with it.
std::vector<Actor> ReadActors(const nlohmann::json& observation, std::vector<Fault>& faults)
{
    std::vector<Actor> actors;
    if (!observation.is_object()) {
        faults.push_back({"", "the observation must be a JSON object"});
        return actors;
    }
    if (!observation.contains("actors") || !observation.at("actors").is_array()) {
        faults.push_back({"/actors", "the observation needs 'actors', an array"});
        return actors;
    }
    const nlohmann::json& list = observation.at("actors");
    std::map<std::string, std::size_t> seen; // Actor id -> the index that has it first.
    int players = 0;                         // Actors whose is_player is true.
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string at = std::format("/actors/{}", i);
        const nlohmann::json& entry = list[i];
        if (!entry.is_object()) {
            faults.push_back({at, "an actor must be a JSON object"});
            continue;
        }
        // A required non-empty string field; "" with a fault when it is missing or wrong.
        auto text = [&](const char* key) {
            const bool good = entry.contains(key) && entry.at(key).is_string() &&
                              !entry.at(key).get_ref<const std::string&>().empty();
            if (!good) {
                faults.push_back({at + "/" + key, std::format("an actor needs '{}', a non-empty string", key)});
                return std::string();
            }
            return entry.at(key).get<std::string>();
        };
        Actor actor;
        actor.id = text("id");
        actor.name = text("name");
        bool whole = !actor.id.empty() && !actor.name.empty();
        if (!entry.contains("is_player") || !entry.at("is_player").is_boolean()) {
            faults.push_back({at + "/is_player", "an actor needs 'is_player', true or false"});
            whole = false;
        } else {
            actor.isPlayer = entry.at("is_player").get<bool>();
            players += actor.isPlayer ? 1 : 0;
        }
        if (entry.contains("bio")) {
            if (entry.at("bio").is_string()) {
                actor.bio = entry.at("bio").get<std::string>();
            } else {
                faults.push_back({at + "/bio", "'bio' must be a string"});
                whole = false;
            }
        }
        // Ids are unique: the engine names actors by them.
        if (!actor.id.empty()) {
            const auto [first, inserted] = seen.emplace(actor.id, i);
            if (!inserted) {
                faults.push_back({at + "/id", std::format("the id '{}' is repeated: /actors/{} has it too", actor.id,
                                                          first->second)});
                whole = false;
            }
        }
        if (whole) {
            actors.push_back(std::move(actor));
        }
    }
    if (players != 1) {
        faults.push_back({"/actors", std::format("exactly one actor must be the player, not {}", players)});
    }
    return actors;
}

// The DataError for an observation's faults: "observation at <pointer>: <problem>", joined by "; ".
core::DataError ObservationError(const std::vector<Fault>& faults)
{
    std::string message;
    for (const Fault& fault : faults) {
        message += std::format("{}observation at {}: {}", message.empty() ? "" : "; ",
                               fault.pointer.empty() ? "the root" : fault.pointer, fault.problem);
    }
    return core::DataError(message);
}

} // namespace

nlohmann::json HostDataToJson(const HostData& data)
{
    nlohmann::json lore = nlohmann::json::array();
    for (const core::LoreEntry& entry : data.lore) {
        lore.push_back({{"subject", entry.subject}, {"data", entry.data}, {"always_on", entry.alwaysOn}});
    }
    return {{"catalog", data.catalog}, {"observation_schema", data.observationSchema}, {"lore", std::move(lore)}};
}

HostData HostDataFromJson(const nlohmann::json& json)
{
    auto fault = [](const std::string& pointer, const std::string& problem) {
        return core::DataError(std::format("host data at {}: {}", pointer.empty() ? "the root" : pointer, problem));
    };
    if (!json.is_object()) {
        throw fault("", "host data must be a JSON object");
    }
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (it.key() != "catalog" && it.key() != "observation_schema" && it.key() != "lore") {
            throw fault("/" + PointerToken(it.key()), std::format("unknown key '{}'", it.key()));
        }
    }
    if (!json.contains("catalog")) {
        throw fault("/catalog", "missing: the action catalog is required");
    }
    if (!json.contains("observation_schema")) {
        throw fault("/observation_schema", "missing: the observation's JSON Schema is required");
    }
    HostData data;
    data.catalog = json.at("catalog");
    data.observationSchema = json.at("observation_schema");
    if (!json.contains("lore")) {
        return data;
    }

    // Each lore entry: a subject and data, both strings, and an optional always_on.
    const nlohmann::json& lore = json.at("lore");
    if (!lore.is_array()) {
        throw fault("/lore", "'lore' must be an array");
    }
    for (std::size_t i = 0; i < lore.size(); ++i) {
        const std::string at = std::format("/lore/{}", i);
        const nlohmann::json& entry = lore[i];
        if (!entry.is_object()) {
            throw fault(at, "a lore entry must be a JSON object");
        }
        for (auto it = entry.begin(); it != entry.end(); ++it) {
            if (it.key() != "subject" && it.key() != "data" && it.key() != "always_on") {
                throw fault(at + "/" + PointerToken(it.key()), std::format("unknown key '{}'", it.key()));
            }
        }
        if (!entry.contains("subject") || !entry.at("subject").is_string()) {
            throw fault(at + "/subject", "'subject' must be a string");
        }
        if (!entry.contains("data") || !entry.at("data").is_string()) {
            throw fault(at + "/data", "'data' must be a string");
        }
        if (entry.contains("always_on") && !entry.at("always_on").is_boolean()) {
            throw fault(at + "/always_on", "'always_on' must be true or false");
        }
        data.lore.push_back({entry.at("subject").get<std::string>(), entry.at("data").get<std::string>(),
                             entry.value("always_on", false)});
    }
    return data;
}

std::string HostDataHash(const HostData& data)
{
    // The replace handler keeps a string that is not UTF-8 from throwing; the text is the same for the same data.
    core::ContentHash hash;
    hash.Add(HostDataToJson(data).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    return hash.Hex();
}

const Actor* Observation::Find(std::string_view id) const
{
    const auto it = std::find_if(actors.begin(), actors.end(), [id](const Actor& actor) { return actor.id == id; });
    return it == actors.end() ? nullptr : &*it;
}

const Actor& Observation::Player() const
{
    const auto it = std::find_if(actors.begin(), actors.end(), [](const Actor& actor) { return actor.isPlayer; });
    MORPHEUS_REQUIRE(it != actors.end(), "Observation::Player: an observation that passed Observe has its player");
    return *it;
}

HostedWorld::HostedWorld(const HostData& data)
    : _observationSchema(data.observationSchema)
    , _lore(data.lore)
{
    // 1. The catalog: {"actions": [...]}, each action an id and its parameters.
    auto fault = [](const std::string& pointer, const std::string& problem) {
        return core::DataError(std::format("catalog at {}: {}", pointer.empty() ? "the root" : pointer, problem));
    };
    const nlohmann::json& catalog = data.catalog;
    if (!catalog.is_object()) {
        throw fault("", "the catalog must be a JSON object with 'actions'");
    }
    for (auto it = catalog.begin(); it != catalog.end(); ++it) {
        if (it.key() != "actions") {
            throw fault("/" + PointerToken(it.key()), std::format("'{}' does not apply to the catalog", it.key()));
        }
    }
    if (!catalog.contains("actions") || !catalog.at("actions").is_array() || catalog.at("actions").empty()) {
        throw fault("/actions", "'actions' must be a non-empty array");
    }
    const nlohmann::json& actions = catalog.at("actions");
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const std::string at = std::format("/actions/{}", i);
        const nlohmann::json& entry = actions[i];
        if (!entry.is_object()) {
            throw fault(at, "an action must be a JSON object");
        }
        for (auto it = entry.begin(); it != entry.end(); ++it) {
            if (it.key() != "id" && it.key() != "params") {
                throw fault(at + "/" + PointerToken(it.key()), std::format("'{}' does not apply to an action", it.key()));
            }
        }
        if (!entry.contains("id") || !entry.at("id").is_string() || entry.at("id").get_ref<const std::string&>().empty()) {
            throw fault(at + "/id", "an action needs 'id', a non-empty string");
        }
        Action action;
        action.id = entry.at("id").get<std::string>();
        for (std::size_t j = 0; j < _actions.size(); ++j) {
            if (_actions[j].id == action.id) {
                throw fault(at + "/id", std::format("the id '{}' is repeated: /actions/{} has it too", action.id, j));
            }
        }

        // Absent params: no parameters. Each parameter is checked against its type's keys.
        if (entry.contains("params")) {
            const nlohmann::json& params = entry.at("params");
            if (!params.is_object()) {
                throw fault(at + "/params", "'params' must be a JSON object");
            }
            for (auto it = params.begin(); it != params.end(); ++it) {
                const std::string paramAt = at + "/params/" + PointerToken(it.key());
                const nlohmann::json& definition = it.value();
                if (!definition.is_object()) {
                    throw fault(paramAt, "a parameter must be a JSON object");
                }
                Param param;
                param.name = it.key();
                const std::string typeName = definition.contains("type") && definition.at("type").is_string()
                                                 ? definition.at("type").get<std::string>()
                                                 : std::string();
                std::vector<std::string_view> keys; // The keys this type allows.
                std::string what;                   // The type as messages name it.
                if (typeName == "actor") {
                    param.type = ParamType::Actor;
                    keys = {"type", "scope"};
                    what = "an actor";
                } else if (typeName == "string") {
                    param.type = ParamType::String;
                    keys = {"type", "maxLength", "minLength"};
                    what = "a string";
                } else if (typeName == "integer") {
                    param.type = ParamType::Integer;
                    keys = {"type", "minimum", "maximum"};
                    what = "an integer";
                } else if (typeName == "enum") {
                    param.type = ParamType::Enum;
                    keys = {"type", "values"};
                    what = "an enum";
                } else {
                    throw fault(paramAt + "/type", "'type' must be 'actor', 'string', 'integer' or 'enum'");
                }
                for (auto key = definition.begin(); key != definition.end(); ++key) {
                    if (std::find(keys.begin(), keys.end(), key.key()) == keys.end()) {
                        throw fault(paramAt + "/" + PointerToken(key.key()),
                                    std::format("'{}' does not apply to {} parameter", key.key(), what));
                    }
                }

                switch (param.type) {
                case ParamType::Actor: {
                    // Whose ids a turn offers.
                    const std::string scope = definition.contains("scope") && definition.at("scope").is_string()
                                                  ? definition.at("scope").get<std::string>()
                                                  : std::string();
                    if (scope == "present") {
                        param.scope = Scope::Present;
                    } else if (scope == "present_non_player") {
                        param.scope = Scope::PresentNonPlayer;
                    } else if (scope == "participant") {
                        param.scope = Scope::Participant;
                    } else {
                        throw fault(paramAt + "/scope", "'scope' must be 'present', 'present_non_player' or 'participant'");
                    }
                    break;
                }
                case ParamType::String: {
                    // Bounded, so the proposal grammar is: maxLength within llama.cpp's limit, minLength up to it.
                    const std::optional<long long> maxLength =
                        definition.contains("maxLength")
                            ? core::JsonIntInRange(definition.at("maxLength"), 0, grammar::GBNF_MAX_STRING_CHARS)
                            : std::nullopt;
                    if (!maxLength) {
                        throw fault(paramAt + "/maxLength",
                                    std::format("a string parameter needs 'maxLength', an integer from 0 to {}",
                                                grammar::GBNF_MAX_STRING_CHARS));
                    }
                    param.schema = {{"type", "string"}, {"maxLength", *maxLength}};
                    if (definition.contains("minLength")) {
                        const std::optional<long long> minLength =
                            core::JsonIntInRange(definition.at("minLength"), 0, *maxLength);
                        if (!minLength) {
                            throw fault(paramAt + "/minLength",
                                        std::format("'minLength' must be an integer from 0 to {} (the 'maxLength')",
                                                    *maxLength));
                        }
                        param.schema["minLength"] = *minLength;
                    }
                    break;
                }
                case ParamType::Integer: {
                    // Both bounds, in order: the grammar writes the exact range.
                    auto bound = [&](const char* key) -> long long {
                        const std::optional<long long> value =
                            definition.contains(key)
                                ? core::JsonIntInRange(definition.at(key), std::numeric_limits<long long>::min(),
                                                       std::numeric_limits<long long>::max())
                                : std::nullopt;
                        if (!value) {
                            throw fault(paramAt + "/" + key,
                                        std::format("an integer parameter needs '{}', an integer", key));
                        }
                        return *value;
                    };
                    const long long minimum = bound("minimum");
                    const long long maximum = bound("maximum");
                    if (minimum > maximum) {
                        throw fault(paramAt, std::format("'minimum' {} is above 'maximum' {}", minimum, maximum));
                    }
                    param.schema = {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
                    break;
                }
                case ParamType::Enum: {
                    // Strings, each once.
                    const nlohmann::json values = definition.value("values", nlohmann::json());
                    const bool strings =
                        values.is_array() && !values.empty() &&
                        std::all_of(values.begin(), values.end(), [](const nlohmann::json& value) { return value.is_string(); });
                    if (!strings) {
                        throw fault(paramAt + "/values", "'values' must be a non-empty array of strings");
                    }
                    for (std::size_t k = 0; k < values.size(); ++k) {
                        for (std::size_t l = 0; l < k; ++l) {
                            if (values[l] == values[k]) {
                                throw fault(std::format("{}/values/{}", paramAt, k),
                                            std::format("'{}' is listed twice", values[k].get<std::string>()));
                            }
                        }
                    }
                    param.schema = {{"enum", values}};
                    break;
                }
                }
                action.params.push_back(std::move(param));
            }
        }
        _actions.push_back(std::move(action));
    }

    // 2. The observation schema: in Validate's extended subset, so an unsupported keyword fails here, not at a turn.
    try {
        grammar::CheckValidationSchema(data.observationSchema);
    } catch (const core::DataError& error) {
        throw core::DataError(std::format("observation_schema: {}", error.what()));
    }

    // 3. The lore: each entry has a subject, unique, and data.
    for (std::size_t i = 0; i < data.lore.size(); ++i) {
        const core::LoreEntry& entry = data.lore[i];
        if (entry.subject.empty()) {
            throw core::DataError(std::format("lore at /{}/subject: a lore entry needs a subject", i));
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (data.lore[j].subject == entry.subject) {
                throw core::DataError(std::format("lore at /{}/subject: the subject '{}' is repeated: /{} has it too",
                                                  i, entry.subject, j));
            }
        }
        if (entry.data.empty()) {
            throw core::DataError(std::format("lore at /{}/data: a lore entry needs data", i));
        }
    }
}

const std::vector<core::LoreEntry>& HostedWorld::Lore() const
{
    return _lore;
}

std::vector<std::string> HostedWorld::ActionIds() const
{
    std::vector<std::string> ids;
    for (const Action& action : _actions) {
        ids.push_back(action.id);
    }
    return ids;
}

Observation HostedWorld::Observe(const nlohmann::json& observation) const
{
    // The core first, then the host's schema; every fault of both is collected.
    std::vector<Fault> faults;
    std::vector<Actor> actors = ReadActors(observation, faults);
    for (const grammar::ValidationError& error : grammar::Validate(observation, _observationSchema)) {
        faults.push_back({error.path, error.message});
    }
    if (!faults.empty()) {
        throw ObservationError(faults);
    }
    return Observation{observation, std::move(actors)};
}

std::vector<Actor> HostedWorld::ActorsOf(const nlohmann::json& observation)
{
    std::vector<Fault> faults;
    std::vector<Actor> actors = ReadActors(observation, faults);
    if (!faults.empty()) {
        throw ObservationError(faults);
    }
    return actors;
}

void HostedWorld::CheckEvents(const std::vector<nlohmann::json>& events)
{
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (!events[i].is_object()) {
            throw core::DataError(std::format("events/{}: an event must be a JSON object", i));
        }
        if (!events[i].contains("text") || !events[i].at("text").is_string()) {
            throw core::DataError(std::format("events/{}/text: an event needs 'text', a string", i));
        }
    }
}

nlohmann::json HostedWorld::ProposalSchema(const Observation& observation, const Participants& participants,
                                           const std::vector<std::string>& allowed, std::string_view speakerId) const
{
    // Every allowed id names an action of the catalog, once.
    for (std::size_t i = 0; i < allowed.size(); ++i) {
        const bool known = std::any_of(_actions.begin(), _actions.end(),
                                       [&](const Action& action) { return action.id == allowed[i]; });
        if (!known) {
            throw core::DataError(std::format("allowed_actions/{}: '{}' is not an action of the catalog", i, allowed[i]));
        }
        if (std::find(allowed.begin(), allowed.begin() + static_cast<std::ptrdiff_t>(i), allowed[i]) !=
            allowed.begin() + static_cast<std::ptrdiff_t>(i)) {
            throw core::DataError(std::format("allowed_actions/{}: '{}' is listed twice", i, allowed[i]));
        }
    }

    // The actor ids a scope offers this turn, in observation order: never the speaker, each once.
    auto actorsIn = [&](Scope scope) {
        std::vector<std::string> ids;
        std::set<std::string> taken;
        for (const Actor& actor : observation.actors) {
            if (actor.id == speakerId) {
                continue;
            }
            const bool participates = std::any_of(participants.begin(), participants.end(),
                                                  [&](const core::Participant& p) { return p.id == actor.id; });
            const bool inScope = scope == Scope::Present || (scope == Scope::PresentNonPlayer && !actor.isPlayer) ||
                                 (scope == Scope::Participant && participates);
            if (inScope && taken.insert(actor.id).second) {
                ids.push_back(actor.id);
            }
        }
        return ids;
    };

    // One branch per allowed action the turn can offer, in catalog order.
    nlohmann::json branches = nlohmann::json::array();
    for (const Action& action : _actions) {
        if (std::find(allowed.begin(), allowed.end(), action.id) == allowed.end()) {
            continue;
        }
        nlohmann::json properties = nlohmann::json::object();
        nlohmann::json required = nlohmann::json::array();
        bool offered = true; // False when an actor parameter has no one to name: the action is left out.
        for (const Param& param : action.params) {
            nlohmann::json schema = param.schema;
            if (param.type == ParamType::Actor) {
                const std::vector<std::string> ids = actorsIn(param.scope);
                if (ids.empty()) {
                    offered = false;
                    break;
                }
                schema = {{"enum", ids}};
            }
            properties[param.name] = std::move(schema);
            required.push_back(param.name);
        }
        if (!offered) {
            continue;
        }
        const nlohmann::json paramsSchema = {{"type", "object"}, {"properties", properties}, {"required", required}};
        branches.push_back({{"type", "object"},
                            {"required", nlohmann::json::array({"action", "params"})},
                            {"properties", {{"action", {{"const", action.id}}}, {"params", paramsSchema}}}});
    }
    if (branches.empty()) {
        throw core::DataError("allowed_actions: no allowed action can be taken this turn: each needs an actor its "
                              "scope does not offer");
    }
    return {{"oneOf", std::move(branches)}};
}

std::string HostedWorld::ProposalGrammar(const Observation& observation, const Participants& participants,
                                         const std::vector<std::string>& allowed, std::string_view speakerId) const
{
    return grammar::FromSchema(ProposalSchema(observation, participants, allowed, speakerId));
}

core::Adjudication HostedWorld::Adjudicate(const nlohmann::json& proposalSchema, std::string_view modelText) const
{
    core::Adjudication adjudication;
    // A parse failure is the model's mistake: the grammar admits lone surrogate escapes that the parser refuses.
    nlohmann::json proposal;
    try {
        proposal = nlohmann::json::parse(modelText.begin(), modelText.end());
    } catch (const nlohmann::json::parse_error& error) {
        adjudication.reasons.push_back(std::format("the output is not valid JSON: {}", error.what()));
        return adjudication;
    }
    // Then the proposal schema of the turn: every mistake is a reason.
    for (const grammar::ValidationError& error : grammar::Validate(proposal, proposalSchema)) {
        adjudication.reasons.push_back(std::format("{}: {}", error.path.empty() ? "the output" : error.path, error.message));
    }
    if (!adjudication.reasons.empty()) {
        return adjudication;
    }
    adjudication.requested = true;
    adjudication.actionId = proposal.at("action").get<std::string>();
    adjudication.params = proposal.at("params");
    return adjudication;
}

} // namespace morpheus::turn
```

In `src/turn/CMakeLists.txt`, replace

```cmake
    Folding.cpp
    MemoryUpkeep.cpp
```

with

```cmake
    Folding.cpp
    HostedWorld.cpp
    MemoryUpkeep.cpp
```

- [ ] **Step 4: Run the hosted tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"`
Expected: builds with no warnings; `All tests passed` (the store's `[store][hosted]` tests and the eleven new ones).

- [ ] **Step 5: Run the whole fast suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 457` (446 + 11), the three `[server]` tests "Not Run (Skipped)".

- [ ] **Step 6: Byte check and commit**

```bash
FILES="src/turn/HostedWorld.h src/turn/HostedWorld.cpp src/turn/CMakeLists.txt tests/hosted/fixtures/open.json tests/hosted/fixtures/observation.json tests/hosted/HostedTestSupport.h tests/hosted/HostedWorldTests.cpp"
LC_ALL=C grep -c $'[\x80-\xFF]' $FILES
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES
git add $FILES
git commit -m "Add turn::HostedWorld: host data checks, observations, the proposal schema and grammar, Adjudicate" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

### Task 5: The hosted prompt layers and the default hosted texts

The context module builds a hosted prompt beside the owned one: one system message (L0 the hosted system text, L1 the host's always-on lore, the participants, L2 the run-wide summaries), the conversation's entries (L3), then the tail (retrieved snippets, the observation as compact JSON, the new events, the player's line, the instruction). Both builders lay out and measure their prompt through one shared helper, so the hosted prompt has exactly the owned prompt's budgets and flags and the shared upkeep acts on it unchanged. `EngineText` gains the default hosted texts.

**Files:**
- Modify: `src/context/PromptBuilder.h`, `src/context/PromptBuilder.cpp`
- Modify: `src/turn/EngineText.h`
- Create: `tests/context/HostedPromptTests.cpp`

**Interfaces:**
- Consumes: `context::TranscriptEntry` (Task 3); `core::Participant` (Task 2).
- Produces (`context/PromptBuilder.h`): `struct context::HostedInputs { std::string systemText; std::vector<core::LoreEntry> lore; std::vector<core::Participant> participants; std::vector<core::Summary> summaries; std::vector<TranscriptEntry> transcript; std::vector<std::string> retrieved; nlohmann::json observation; std::vector<std::string> events; std::string playerLine; std::string instruction; }`; `Assembly context::AssembleHostedPrompt(const HostedInputs& inputs, const core::BudgetSettings& budget, inference::LlmBackend& backend)`. The system message is `systemText + "\n\n" + ["# Lore\n" + "- <subject>: <data>\n"... + "\n"] + "# Participants\n" + "- <name> (<id>): <bio>\n" or "- <name> (<id>)\n"... + ["\n# Story so far\n" + "Turns A-B: <text>\n"...]`; the tail is `["# Relevant\n" + "- <snippet>\n"... + "\n"] + "# Observation\n<compact JSON>\n" + ["\n# New events\n" + "(<text>)\n"...] + ["\n# Player\n<playerLine>\n"] + "\n# Task\n<instruction>"`.
- Produces (`turn/EngineText.h`): `HOSTED_SYSTEM_TEXT` (no variable), `HOSTED_RESOLVE_INSTRUCTION` (`{speaker}`), `HOSTED_SPEAK_INSTRUCTION` (`{speaker}`, `{player}`), `HOSTED_TURN_CUE` (`"({speaker} speaks.)"`), all `inline constexpr std::string_view`.

- [ ] **Step 1: Write the failing tests**

Create `tests/context/HostedPromptTests.cpp`:

```cpp
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
    CHECK(turn::HOSTED_TURN_CUE == "({speaker} speaks.)");
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
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors, among them `'HostedInputs': is not a member of 'morpheus::context'` and `'HOSTED_SYSTEM_TEXT': is not a member of 'morpheus::turn'`.

- [ ] **Step 3: Write the default hosted texts**

In `src/turn/EngineText.h`, before `} // namespace morpheus::turn`, insert:

```cpp

// L0 of a hosted run (hosted-world spec, "Default hosted texts"): voicing characters in a world the host simulates,
// and the output rules. Generic: it names no game. It has no variable, so L0 never changes between turns. Sub-project
// F lets the host replace it.
inline constexpr std::string_view HOSTED_SYSTEM_TEXT = R"(# Engine
You voice the characters of a role-playing game that a host game runs. The host simulates the world: its places, its people, their items and the outcome of everything anyone does. You never decide outcomes. Each turn you speak for one character, the speaker, and choose the action they try.

Every turn has two steps.

Step 1, act. Reply with one JSON object and nothing else: "action" is one of the actions the turn allows, and "params" holds that action's parameters. A parameter that names an actor takes the actor's id from the observation. Choose what the speaker would do now. The host decides whether it happens; its outcome arrives later as an event.

Step 2, speak. Write only what the speaker says aloud: no name in front, no quotation marks, no narration and no description of actions. Stay in character and in the world. One to three sentences.

The conversation shows each line after the name of whoever said it, with the action the speaker tried in brackets. Events the host reports are in parentheses. The observation is the host's current view of the scene, as JSON.)";

// The task at the end of a hosted resolve prompt's tail. {speaker} and {player} are the speaker's and the player's
// names.
inline constexpr std::string_view HOSTED_RESOLVE_INSTRUCTION =
    "Step 1: choose the action {speaker} tries now. Reply with the JSON object only.";

// The user message the speak call appends after the resolve output; it must fit narrate_allowance.
inline constexpr std::string_view HOSTED_SPEAK_INSTRUCTION =
    "Step 2: write what {speaker} says now. Spoken words only, in character. {player} is the player's character.";

// L3's user message for a turn that had no events and no player text before the speaker's line.
inline constexpr std::string_view HOSTED_TURN_CUE = "({speaker} speaks.)";
```

- [ ] **Step 4: Write the hosted prompt builder**

Replace the whole of `src/context/PromptBuilder.h` with:

```cpp
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/Transcript.h"
#include "core/HostedRecord.h"
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
    std::vector<TranscriptEntry> transcript = {};  // L3: unfolded turns, oldest first, rendered (TranscriptEntries).
    nlohmann::json state = nullptr;                // T1: current state (validated).
    std::vector<std::string> retrieved = {};       // T2: formatted lore/history snippets.
    std::string playerInput = {};                  // T3
    std::string instruction = {};                  // T4: the resolve task instruction.
};

// What goes into one hosted resolve prompt, by layer (hosted-world spec, "The prompt"), most stable first, so a new
// turn adds to the end of the cached part.
struct HostedInputs {
    std::string systemText = {};                      // L0: the hosted system text.
    std::vector<core::LoreEntry> lore = {};           // L1: the host's lore; only the always-on entries are shown.
    std::vector<core::Participant> participants = {}; // The participants, in join order: name, id and bio.
    std::vector<core::Summary> summaries = {};        // L2: run-wide summaries of folded turns, oldest first.
    std::vector<TranscriptEntry> transcript = {};     // L3: this conversation's unfolded turns, rendered.
    std::vector<std::string> retrieved = {};          // Tail: lore and history snippets.
    nlohmann::json observation = nullptr;             // Tail: shown as compact JSON.
    std::vector<std::string> events = {};             // Tail: the new events' texts, in order.
    std::string playerLine = {};                      // Tail: "<player name>: <text>", or empty.
    std::string instruction = {};                     // Tail: the resolve instruction.
};

// Exact token counts. Layer counts are raw text (no template tokens), for the inspector; `total` is the only
// figure that includes chat-template tokens (the whole templated prompt).
struct LayerTokens {
    int engineRules = 0; // L0.
    int world = 0;       // L1; in a hosted prompt, the host's always-on lore and the participants.
    int summary = 0;
    int transcript = 0;
    int tail = 0;
    int total = 0;
};

struct Assembly {
    inference::Prompt prompt = {};
    LayerTokens tokens = {};
    int transcriptBudget = 0;          // Tokens L3 may add to the templated prompt, with the summary counted at most at summary_cap (always positive).
    int transcriptCost = 0;            // Tokens L3 adds to the templated prompt, template tokens included.
    bool transcriptOverBudget = false; // Fold old turns into the summary and assemble again.
    bool summaryOverBudget = false;    // The summary itself must be condensed.
    bool tailOverBudget = false;       // The tail (state or observation, retrieval, input and instruction) exceeds tail_allowance.
    // The full templated prompt plus both generations exceeds the context: shrink the tail or fold.
    bool contextOverflow = false;

    bool Fits() const
    {
        return !transcriptOverBudget && !summaryOverBudget && !tailOverBudget && !contextOverflow;
    }
};

// Builds the resolve prompt: one system message (L0 + L1 + L2), the transcript (L3), then one user message with the
// volatile tail (T1-T4). L1 is the world's rules, game reference (RenderWorldReference) and always-on lore.
// prompt.stableCount marks the end of L3. Throws ContractError if L0 exceeds engine_rules_cap (engine text too
// long) and DataError if L1 exceeds world_cap (the world is too big) or if the context leaves no room for the
// transcript after the system message (its summary counted at most at summary_cap), the tail allowance, the narrate
// allowance and both generations, thinking blocks included (folding cannot fix that). Other overruns, an over-cap
// summary among them, are flags for the turn layer to act on.
//
// The narrate call appends two messages that are not part of this prompt: the resolve output (assistant, up to
// resolveMaxTokens) and the resolution record + instruction (user). budget.narrateAllowance is reserved for the
// user message's text: transcriptBudget excludes it. contextOverflow counts it together with the template
// overhead of both messages (assistant and user), each approximated by the cost of an empty one-message prompt.
Assembly AssembleResolvePrompt(const core::World& world, const ContextInputs& inputs,
                               const core::BudgetSettings& budget, inference::LlmBackend& backend);

// Builds a hosted resolve prompt: one system message (L0, then L1 the always-on lore, then the participants as
// "- <name> (<id>): <bio>" or "- <name> (<id>)", then L2), the conversation's entries (L3), then one user message with
// the tail (retrieved snippets, the observation as compact JSON, the new events in parentheses, the player's line,
// the instruction). prompt.stableCount marks the end of L3. A join or a leave changes the prompt from the
// participants on; a new turn only adds entries before the tail. Budgets and flags are AssembleResolvePrompt's, with
// the speak call in the narrate call's place: tokens.world counts the lore and the participants together against
// world_cap (a DataError over it: the host's data); the system text over engine_rules_cap is a ContractError.
Assembly AssembleHostedPrompt(const HostedInputs& inputs, const core::BudgetSettings& budget,
                              inference::LlmBackend& backend);

// Turns a resolve prompt into the narrate prompt by appending the resolve output (assistant) and the resolution
// record with the narrate instruction (user). It only appends, so the whole resolve prompt stays cached.
// The appended user message must stay within budget.narrate_allowance; the turn layer checks this.
void ExtendForNarrate(inference::Prompt& prompt, const std::string& resolveOutput,
                      const std::string& resolutionAndInstruction);

} // namespace morpheus::context
```

Replace the whole of `src/context/PromptBuilder.cpp` with:

```cpp
#include "context/PromptBuilder.h"

#include <algorithm>
#include <format>
#include <utility>

#include "context/StateView.h"
#include "context/Transcript.h"
#include "context/WorldReference.h"
#include "core/Errors.h"

namespace morpheus::context {

namespace {

// Lays out a prompt into `assembly`: one system message with `systemText`, the messages of the transcript entries
// (the stable prefix ends after them), then one user message with `tail`. Then measures it against `budget`: the
// layer counts the caller has not set (tail, transcript, total), the transcript's budget and cost, and every flag. The
// caller has set tokens.summary: an over-cap summary is counted at most at its cap. Throws DataError when the context
// leaves no room for the transcript (folding cannot fix that).
void LayOut(Assembly& assembly, std::string systemText, const std::vector<TranscriptEntry>& transcript,
            std::string tail, const core::BudgetSettings& budget, inference::LlmBackend& backend)
{
    // The fixed part of the prompt: the system message and the tail. Counting it, with and without the tail,
    // gives the template-inclusive costs the transcript budget needs.
    inference::Prompt fixed;
    fixed.messages.push_back({inference::Role::System, std::move(systemText)});
    fixed.messages.push_back({inference::Role::User, std::move(tail)});

    // Exact counts with the model's tokenizer. Layer counts are raw text; only `total` includes template tokens.
    LayerTokens& tokens = assembly.tokens;
    tokens.tail = backend.CountTokens(fixed.messages.back().content);
    const int fixedTotal = backend.CountPromptTokens(fixed); // System message and tail, no transcript.
    inference::Message tailMessage = std::move(fixed.messages.back());
    fixed.messages.pop_back();
    const int systemCost = backend.CountPromptTokens(fixed); // The system message alone, templated.

    // Messages: stable prefix (system, transcript), then the tail.
    inference::Prompt& prompt = assembly.prompt;
    prompt.messages.push_back(std::move(fixed.messages.front()));
    for (const TranscriptEntry& entry : transcript) {
        for (const inference::Message& message : entry.messages) {
            tokens.transcript += backend.CountTokens(message.content);
            prompt.messages.push_back(message);
        }
    }
    prompt.stableCount = prompt.messages.size();
    prompt.messages.push_back(std::move(tailMessage));
    tokens.total = backend.CountPromptTokens(prompt);
    assembly.transcriptCost = tokens.total - fixedTotal;

    assembly.summaryOverBudget = tokens.summary > budget.summaryCap;
    assembly.tailOverBudget = tokens.tail > budget.tailAllowance;

    // L3 gets what the templated system message, the tail allowance, the narrate allowance and both generations
    // leave, with the summary counted at most at its cap: an over-cap summary is a flag (condense it), not a
    // settings error. A generation is a call's thinking block plus its answer. Reserving both thinking blocks is
    // conservative: the resolve call's block is not part of the narrate prompt, so the two never share a context.
    const int summaryExcess = std::max(0, tokens.summary - budget.summaryCap);
    assembly.transcriptBudget = backend.ContextSize() - budget.ResolveGenerationTokens() -
                                budget.NarrateGenerationTokens() - budget.narrateAllowance - budget.tailAllowance -
                                (systemCost - summaryExcess);
    if (assembly.transcriptBudget <= 0) {
        // Folding turns cannot fix this: the settings or the world leave L3 nothing, so it is not a flag.
        throw core::DataError(std::format("context_size {} leaves no room for the transcript after the system "
                                          "layers ({} tokens), the tail allowance, the narrate allowance and both "
                                          "generations",
                                          backend.ContextSize(), systemCost - summaryExcess));
    }
    assembly.transcriptOverBudget = assembly.transcriptCost > assembly.transcriptBudget;

    // Final safety check on the whole templated prompt, as seen by the narrate call: the prompt, the narrate
    // message (its template overhead plus the allowance for its text), the resolve output's assistant message
    // (template overhead only: its text is the resolve generation) and both generations. Each overhead is
    // approximated by the cost of an empty one-message prompt.
    inference::Prompt emptyUser;
    emptyUser.messages.push_back({inference::Role::User, ""});
    const int narrateMessageOverhead = backend.CountPromptTokens(emptyUser);
    inference::Prompt emptyAssistant;
    emptyAssistant.messages.push_back({inference::Role::Assistant, ""});
    const int resolveOutputOverhead = backend.CountPromptTokens(emptyAssistant);
    assembly.contextOverflow = tokens.total + budget.narrateAllowance + narrateMessageOverhead +
                                   resolveOutputOverhead + budget.ResolveGenerationTokens() +
                                   budget.NarrateGenerationTokens() >
                               backend.ContextSize();
}

} // namespace

Assembly AssembleResolvePrompt(const core::World& world, const ContextInputs& inputs,
                               const core::BudgetSettings& budget, inference::LlmBackend& backend)
{
    MORPHEUS_REQUIRE(!inputs.engineRules.empty() && !inputs.playerInput.empty() && !inputs.instruction.empty(),
                     "AssembleResolvePrompt: engine rules, player input and instruction are required");

    // L1: the world's rules, its game reference, then its always-on lore.
    std::string worldText = "# World\n" + world.rules;
    if (!worldText.ends_with('\n')) {
        worldText += '\n';
    }
    worldText += "\n" + RenderWorldReference(world);
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

    // The system layers' counts, then their caps: L0 is engine text (a bug if too long), L1 is world data (the
    // author must trim it).
    Assembly assembly;
    LayerTokens& tokens = assembly.tokens;
    tokens.engineRules = backend.CountTokens(inputs.engineRules);
    tokens.world = backend.CountTokens(worldText);
    tokens.summary = backend.CountTokens(summaryText);
    MORPHEUS_REQUIRE(tokens.engineRules <= budget.engineRulesCap,
                     std::format("engine rules use {} tokens, over engine_rules_cap {}", tokens.engineRules,
                                 budget.engineRulesCap));
    if (tokens.world > budget.worldCap) {
        throw core::DataError(std::format("world '{}': rules and always-on lore use {} tokens, over world_cap {}",
                                          world.id, tokens.world, budget.worldCap));
    }
    LayOut(assembly, inputs.engineRules + "\n\n" + worldText + summaryText, inputs.transcript, std::move(tail), budget,
           backend);
    return assembly;
}

Assembly AssembleHostedPrompt(const HostedInputs& inputs, const core::BudgetSettings& budget,
                              inference::LlmBackend& backend)
{
    MORPHEUS_REQUIRE(!inputs.systemText.empty() && !inputs.participants.empty() && !inputs.instruction.empty(),
                     "AssembleHostedPrompt: the system text, a participant and the instruction are required");

    // L1, then the participants: the host's always-on lore, then who takes part, in join order. A join or a leave
    // changes the prompt from the participants on; the lore before them stays cached.
    std::string lore;
    for (const core::LoreEntry& entry : inputs.lore) {
        if (entry.alwaysOn) {
            lore += std::format("- {}: {}\n", entry.subject, entry.data);
        }
    }
    std::string worldText = lore.empty() ? std::string() : "# Lore\n" + lore + "\n";
    worldText += "# Participants\n";
    for (const core::Participant& participant : inputs.participants) {
        worldText += participant.bio.empty()
                         ? std::format("- {} ({})\n", participant.name, participant.id)
                         : std::format("- {} ({}): {}\n", participant.name, participant.id, participant.bio);
    }

    // L2: run-wide summaries of folded turns, oldest first.
    std::string summaryText;
    if (!inputs.summaries.empty()) {
        summaryText = "# Story so far\n";
        for (const core::Summary& summary : inputs.summaries) {
            summaryText += std::format("Turns {}-{}: {}\n", summary.fromTurn, summary.toTurn, summary.text);
        }
    }

    // The tail, rebuilt every turn, so it goes last: retrieved snippets, the observation, the new events, the
    // player's line and the instruction. The replace handler keeps a string that is not UTF-8 from throwing: the
    // observation is the host's data.
    std::string tail;
    if (!inputs.retrieved.empty()) {
        tail += "# Relevant\n";
        for (const std::string& snippet : inputs.retrieved) {
            tail += "- " + snippet + "\n";
        }
        tail += "\n";
    }
    tail += "# Observation\n" + inputs.observation.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    if (!inputs.events.empty()) {
        tail += "\n# New events\n";
        for (const std::string& event : inputs.events) {
            tail += "(" + event + ")\n";
        }
    }
    if (!inputs.playerLine.empty()) {
        tail += "\n# Player\n" + inputs.playerLine + "\n";
    }
    tail += "\n# Task\n" + inputs.instruction;

    // The system layers' counts, then their caps: L0 is engine text (a bug if too long), the lore and the
    // participants are the host's data.
    Assembly assembly;
    LayerTokens& tokens = assembly.tokens;
    tokens.engineRules = backend.CountTokens(inputs.systemText);
    tokens.world = backend.CountTokens(worldText);
    tokens.summary = backend.CountTokens(summaryText);
    MORPHEUS_REQUIRE(tokens.engineRules <= budget.engineRulesCap,
                     std::format("the hosted system text uses {} tokens, over engine_rules_cap {}", tokens.engineRules,
                                 budget.engineRulesCap));
    if (tokens.world > budget.worldCap) {
        throw core::DataError(std::format("host lore and participants use {} tokens, over world_cap {}", tokens.world,
                                          budget.worldCap));
    }
    const std::string separator = summaryText.empty() ? std::string() : "\n";
    LayOut(assembly, inputs.systemText + "\n\n" + worldText + separator + summaryText, inputs.transcript,
           std::move(tail), budget, backend);
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

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[context]"`
Expected: builds with no warnings; `All tests passed` (the owned prompt tests unchanged, the six new ones).

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 463` (457 + 6), the three `[server]` tests "Not Run (Skipped)".

- [ ] **Step 6: Byte check and commit**

```bash
FILES="src/context/PromptBuilder.h src/context/PromptBuilder.cpp src/turn/EngineText.h tests/context/HostedPromptTests.cpp"
LC_ALL=C grep -c $'[\x80-\xFF]' $FILES
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES
git add $FILES
git commit -m "Add the hosted prompt layers (context::AssembleHostedPrompt) and the default hosted texts" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

### Task 6: `turn::HostedSession`: open, conversations, readers

One open hosted run. `Open` creates the run or reopens it (same host data as is; changed host data replaces the stored copy and re-embeds changed lore; an owned file is refused). Conversations start, take events between turns, change participants and end. Folded hosted turns are rendered as transcript entries and indexed for retrieval, which needs `MemoryIndex` to take a lore list and rendered entries. Turns come in Task 7.

**Files:**
- Modify: `src/turn/Retrieval.h`, `src/turn/Retrieval.cpp`
- Create: `src/turn/HostedSession.h`, `src/turn/HostedSession.cpp`
- Modify: `src/turn/CMakeLists.txt`
- Modify: `tests/hosted/HostedTestSupport.h`, `tests/turn/RetrievalTests.cpp`
- Create: `tests/hosted/HostedSessionTests.cpp`

**Interfaces:**
- Consumes: `store::RunStore` hosted methods, `store::RunKind`, `core::Conversation`, `core::HostedTurn`, `core::ConversationEvent` (Task 2); `context::TranscriptEntry` (Task 3); `turn::HostedWorld`, `HostData`, `HostDataToJson`, `HostDataHash`, `Actor`, `Observation` (Task 4); `HOSTED_TURN_CUE` (Task 5).
- Produces (`turn/Retrieval.h`): `void MemoryIndex::SyncLore(std::string_view source, const std::vector<core::LoreEntry>& lore, store::RunStore& store, inference::LlmBackend& backend)` (the world overload stays and calls it with `"world '<id>': lore.csv"`); `void MemoryIndex::SyncEntries(const std::vector<context::TranscriptEntry>& folded, store::RunStore& store, inference::LlmBackend& backend)`.
- Produces (`turn/HostedSession.h`): `constexpr std::string_view turn::HOSTED_WORLD_ID = "hosted"`; `class turn::HostedSession` with `static HostedSession Open(const std::filesystem::path& file, HostData data, core::Settings settings, std::uint64_t seed, inference::LlmBackend& backend)`, `std::string StartConversation(const std::vector<std::string>& participantIds, const nlohmann::json& observation)`, `void RecordEvents(std::string_view conversationId, const std::vector<nlohmann::json>& events)`, `void SetParticipants(std::string_view conversationId, const std::vector<std::string>& joined, const std::vector<std::string>& left, const nlohmann::json& observation)`, `void EndConversation(std::string_view conversationId)`, readers `const store::RunMeta& Meta() const`, `std::optional<core::Conversation> Conversation(std::string_view id)`, `std::vector<core::HostedTurn> Turns(std::string_view conversationId)`, `std::vector<core::ConversationEvent> Events(std::string_view conversationId)`, `std::vector<core::Summary> Summaries()`, `const context::Assembly& LastAssembly() const`; private `core::Conversation OpenConversation(std::string_view conversationId)`, `std::vector<context::TranscriptEntry> Entries(const core::Conversation& conversation, bool folded)`, `void SyncHistory()`.
- Produces (`tests/hosted/HostedTestSupport.h`): `core::Settings HostedSettings()`, `std::filesystem::path HostedRunFile(std::string_view name)`.

- [ ] **Step 1: Write the failing tests**

Replace the whole of `tests/hosted/HostedTestSupport.h` with:

```cpp
#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Settings.h"
#include "turn/HostedWorld.h"

namespace morpheus::test {

// The actor ids of observation.json: Hulda and Saadia, and Aela, the player.
inline const std::string HULDA = "0001A6A4";
inline const std::string SAADIA = "0001B07C";
inline const std::string AELA = "00000007";

// tests/hosted/fixtures in the source tree: host data and requests written from section 5 of the integration
// requirements. The host's recorded fixtures can replace them later.
inline std::filesystem::path HostedFixturesDir()
{
    return RepoDir() / "tests" / "hosted" / "fixtures";
}

// The fixture `name`, parsed. A fixture that cannot be read is a broken test: it throws.
inline nlohmann::json ReadFixture(std::string_view name)
{
    std::ifstream stream(HostedFixturesDir() / name, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("ReadFixture: cannot open '" + std::string(name) + "'");
    }
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return nlohmann::json::parse(text);
}

// open.json: the host data a host sends when it opens a run.
inline turn::HostData FixtureHostData()
{
    return turn::HostDataFromJson(ReadFixture("open.json"));
}

// observation.json: Hulda and Saadia at the inn, and Aela, the player.
inline nlohmann::json FixtureObservation()
{
    return ReadFixture("observation.json");
}

// Settings for hosted session tests: a context of 16384 with roomy budgets (resolve_max_tokens 64, narrate_max_tokens
// 256, narrate_allowance 128, tail_allowance 1024, engine_rules_cap 1024, world_cap 1024, summary_cap 512,
// retrieval_cap 256) and no thinking.
inline core::Settings HostedSettings()
{
    core::Settings settings;
    settings.chatModel = "models/chat.gguf"; // Only named in the run meta; never opened.
    settings.embeddingModel = "models/embed.gguf";
    settings.embeddingQueryPrefix = "search_query: ";
    settings.embeddingDocumentPrefix = "search_document: ";
    settings.contextSize = 16384;
    settings.budget = {64, 256, 128, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.3, 40, 0.9};
    settings.narrateSampling = {0.8, 40, 0.95};
    return settings;
}

// A run file in a fresh folder for test `name`.
inline std::filesystem::path HostedRunFile(std::string_view name)
{
    return FreshTempDir(name) / "run.db";
}

} // namespace morpheus::test
```

Create `tests/hosted/HostedSessionTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "store/RunStore.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using namespace nlohmann::literals;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;

namespace {

// The participant ids of conversation `id`, in join order.
std::vector<std::string> ParticipantIds(turn::HostedSession& session, const std::string& id)
{
    const std::optional<core::Conversation> conversation = session.Conversation(id);
    REQUIRE(conversation);
    std::vector<std::string> ids;
    for (const core::Participant& participant : conversation->participants) {
        ids.push_back(participant.id);
    }
    return ids;
}

} // namespace

TEST_CASE("Open creates a hosted run with its host data and indexes the retrievable lore", "[hosted][session]")
{
    test::ScriptedBackend backend(16384); // No model call is made: no responder.
    const std::filesystem::path file = test::HostedRunFile("hosted_open");
    const turn::HostData data = test::FixtureHostData();
    turn::HostedSession session = turn::HostedSession::Open(file, data, test::HostedSettings(), 42, backend);
    CHECK(session.Meta().kind == store::RunKind::Hosted);
    CHECK(session.Meta().worldId == "hosted");
    CHECK(session.Meta().worldHash == turn::HostDataHash(data));
    CHECK(session.Meta().rngSeed == 42u);
    CHECK(session.Meta().modelId == "chat");
    CHECK(backend.EmbeddedTexts() == 2); // Hulda and Saadia; Whiterun and the inn are always on.
    CHECK(session.Summaries().empty());
    CHECK(std::filesystem::exists(file));
}

TEST_CASE("Open with the same host data reopens the run as it is", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    const std::filesystem::path file = test::HostedRunFile("hosted_reopen");
    std::string id;
    {
        turn::HostedSession session =
            turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend);
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    }
    const int embedded = backend.EmbeddedTexts();
    turn::HostedSession reopened =
        turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 7, backend);
    CHECK(backend.EmbeddedTexts() == embedded); // The stored vectors are reused.
    CHECK(reopened.Meta().rngSeed == 42u);      // The run's seed, not the call's.
    CHECK(ParticipantIds(reopened, id) == std::vector<std::string>{test::HULDA, test::AELA});
}

TEST_CASE("Open with changed host data replaces the stored copy and re-embeds the changed lore", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    const std::filesystem::path file = test::HostedRunFile("hosted_changed");
    {
        turn::HostedSession session =
            turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend);
    }
    // A mod update: one lore entry and the catalog change.
    turn::HostData changed = test::FixtureHostData();
    changed.lore[2].data = "Hulda has kept the inn for twenty years.";
    changed.catalog["actions"].push_back(nlohmann::json{{"id", "trade"}});
    const int embedded = backend.EmbeddedTexts();
    {
        turn::HostedSession reopened = turn::HostedSession::Open(file, changed, test::HostedSettings(), 42, backend);
        CHECK(reopened.Meta().worldHash == turn::HostDataHash(changed));
        CHECK(backend.EmbeddedTexts() == embedded + 1); // Only Hulda's entry is embedded again.
    }
    store::RunStore stored = store::RunStore::Open(file);
    CHECK(stored.HostData() == turn::HostDataToJson(changed));
    CHECK(stored.Meta().worldHash == turn::HostDataHash(changed));
}

TEST_CASE("Open refuses a file that holds an owned run", "[hosted][session]")
{
    const std::filesystem::path file = test::HostedRunFile("hosted_owned");
    store::RunMeta meta;
    meta.worldId = "mini";
    {
        store::RunStore owned = store::RunStore::Create(file, meta, nlohmann::json::object());
    }
    test::ScriptedBackend backend(16384);
    CHECK_THROWS_MATCHES(turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend),
                         core::DataError,
                         MessageMatches(ContainsSubstring("is an owned run of world 'mini', not a hosted run")));
}

TEST_CASE("Open checks the host data before it touches the file, and leaves no file when the lore fails",
          "[hosted][session]")
{
    const std::filesystem::path file = test::HostedRunFile("hosted_bad_data");
    turn::HostData data = test::FixtureHostData();
    data.catalog = R"({"actions": []})"_json;
    test::ScriptedBackend backend(16384);
    CHECK_THROWS_WITH(turn::HostedSession::Open(file, data, test::HostedSettings(), 42, backend),
                      "catalog at /actions: 'actions' must be a non-empty array");
    CHECK_FALSE(std::filesystem::exists(file));

    // Lore the embedding model cannot take is the host's DataError, and the new file goes again.
    test::ScriptedBackend small(16384);
    small.SetEmbedLimit(20);
    CHECK_THROWS_MATCHES(turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, small),
                         core::DataError,
                         MessageMatches(ContainsSubstring("host data: lore entry 'Hulda' cannot be embedded")));
    CHECK_FALSE(std::filesystem::exists(file));
}

TEST_CASE("StartConversation stores the participants with their names and bios, in order", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_start"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const nlohmann::json observation = test::FixtureObservation();
    const std::string id = session.StartConversation({test::AELA, test::HULDA}, observation);
    CHECK(id == "c1");
    const std::optional<core::Conversation> conversation = session.Conversation(id);
    REQUIRE(conversation);
    REQUIRE(conversation->participants.size() == 2);
    CHECK(conversation->participants[0].id == test::AELA);
    CHECK(conversation->participants[0].name == "Aela");
    CHECK(conversation->participants[0].bio.empty());
    CHECK(conversation->participants[1].name == "Hulda");
    CHECK(conversation->participants[1].bio == "Hulda keeps the Bannered Mare.");
    CHECK_FALSE(conversation->ended);
    CHECK(session.StartConversation({test::SAADIA}, observation) == "c2");

    // Each fault is the host's DataError naming the field, and starts nothing.
    CHECK_THROWS_WITH(session.StartConversation({}, observation),
                      "participants: a conversation needs at least one participant");
    CHECK_THROWS_WITH(session.StartConversation({test::HULDA, "00000000"}, observation),
                      "participants/1: '00000000' is not an actor of the observation");
    CHECK_THROWS_WITH(session.StartConversation({test::HULDA, test::HULDA}, observation),
                      "participants/1: '0001A6A4' is listed twice");
    nlohmann::json broken = observation;
    broken.at("actors").at(0)["id"] = "";
    CHECK_THROWS_WITH(session.StartConversation({test::HULDA}, broken), ContainsSubstring("observation at /actors/0/id"));
    CHECK_FALSE(session.Conversation("c3"));
}

TEST_CASE("RecordEvents adds events to an open conversation, after its latest turn", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_events"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    session.RecordEvents(id, {R"({"type": "ambient", "text": "Rain starts."})"_json});
    session.RecordEvents(id, {R"({"text": "A door opens."})"_json, R"({"text": "Saadia looks up."})"_json});
    const std::vector<core::ConversationEvent> events = session.Events(id);
    REQUIRE(events.size() == 3);
    CHECK(events[0].afterTurn == 0); // Before the conversation's first turn.
    CHECK(events[0].event.at("type") == "ambient");
    CHECK(events[2].event.at("text") == "Saadia looks up.");

    CHECK_THROWS_WITH(session.RecordEvents(id, {R"({"type": "ambient"})"_json}),
                      "events/0/text: an event needs 'text', a string");
    CHECK_THROWS_WITH(session.RecordEvents("c9", {}), "conversation_id: there is no conversation 'c9'");
    CHECK(session.Events(id).size() == 3);
}

TEST_CASE("SetParticipants removes those who left, then adds those who joined at the end", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_participants"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const nlohmann::json observation = test::FixtureObservation();
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, observation);

    session.SetParticipants(id, {test::SAADIA}, {}, observation);
    CHECK(ParticipantIds(session, id) == std::vector<std::string>{test::HULDA, test::AELA, test::SAADIA});
    CHECK(session.Conversation(id)->participants[2].bio == "Saadia serves at the inn.");
    // A leave and a rejoin in one call moves the actor to the end.
    session.SetParticipants(id, {test::HULDA}, {test::HULDA}, observation);
    CHECK(ParticipantIds(session, id) == std::vector<std::string>{test::AELA, test::SAADIA, test::HULDA});
    session.SetParticipants(id, {}, {test::AELA}, observation);
    CHECK(ParticipantIds(session, id) == std::vector<std::string>{test::SAADIA, test::HULDA});

    // Each fault is the host's DataError naming the field, and changes nothing.
    CHECK_THROWS_WITH(session.SetParticipants(id, {test::SAADIA}, {}, observation),
                      "joined/0: '0001B07C' is already a participant of conversation 'c1'");
    CHECK_THROWS_WITH(session.SetParticipants(id, {"00000000"}, {}, observation),
                      "joined/0: '00000000' is not an actor of the observation");
    CHECK_THROWS_WITH(session.SetParticipants(id, {}, {test::AELA}, observation),
                      "left/0: '00000007' is not a participant of conversation 'c1'");
    CHECK_THROWS_WITH(session.SetParticipants(id, {}, {test::SAADIA, test::HULDA}, observation),
                      "left: conversation 'c1' would have no participant left");
    CHECK(ParticipantIds(session, id) == std::vector<std::string>{test::SAADIA, test::HULDA});
}

TEST_CASE("EndConversation ends a conversation once, and an ended conversation takes no more changes",
          "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_end"), test::FixtureHostData(),
                                                            test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    session.EndConversation(id);
    CHECK(session.Conversation(id)->ended);
    CHECK_THROWS_WITH(session.EndConversation(id), "conversation_id: conversation 'c1' has ended");
    CHECK_THROWS_WITH(session.RecordEvents(id, {R"({"text": "Late."})"_json}),
                      "conversation_id: conversation 'c1' has ended");
    CHECK_THROWS_WITH(session.SetParticipants(id, {test::SAADIA}, {}, test::FixtureObservation()),
                      "conversation_id: conversation 'c1' has ended");
    CHECK_THROWS_WITH(session.EndConversation("c9"), "conversation_id: there is no conversation 'c9'");
}
```

In `tests/turn/RetrievalTests.cpp`, replace `#include "TestPaths.h"` with

```cpp
#include "TestPaths.h"
#include "context/Transcript.h"
```

and append:

```cpp
TEST_CASE("Folded transcript entries are searchable history under their turn number", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_entries", world);
    context::TranscriptEntry entry;
    entry.turn = 4;
    entry.fact = "Aela: Where is the brass key? Hulda [none]: Under the bar.";
    test::ScriptedBackend backend(16384);
    turn::MemoryIndex index("search_document: ");
    index.SyncEntries({entry}, store, backend);

    const std::vector<turn::RetrievalHit> hits =
        index.Retrieve(turn::RetrievalQuery{"brass key"}, ALL_SOURCES, 1000, 6, backend);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].kind == store::EmbeddingKind::Turn);
    CHECK(hits[0].refId == "4");
    CHECK(hits[0].text == "Turn 4: Aela: Where is the brass key? Hulda [none]: Under the bar.");

    // The stored vector is reused, and an empty list leaves no history.
    const int embedded = backend.EmbeddedTexts();
    index.SyncEntries({entry}, store, backend);
    CHECK(backend.EmbeddedTexts() == embedded);
    index.SyncEntries({}, store, backend);
    CHECK(index.Retrieve(turn::RetrievalQuery{"brass key"}, ALL_SOURCES, 1000, 6, backend).empty());
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors, among them `Cannot open include file: 'turn/HostedSession.h'` and `'SyncEntries': is not a member of 'morpheus::turn::MemoryIndex'`.

- [ ] **Step 3: Let MemoryIndex take a lore list and rendered entries**

In `src/turn/Retrieval.h`, replace

```cpp
#include "core/World.h"
#include "inference/Llm.h"
```

with

```cpp
#include "context/Transcript.h"
#include "core/World.h"
#include "inference/Llm.h"
```

replace

```cpp
    // Indexes every lore entry that is not always on as "subject: data", embedding entries one at a time where
    // the stored vector is missing or stale. Throws DataError naming the subject when an entry cannot be embedded
    // (too long for the embedding model) or a subject appears twice.
    void SyncLore(const core::World& world, store::RunStore& store, inference::LlmBackend& backend);
```

with

```cpp
    // Indexes every entry of `lore` that is not always on as "subject: data", embedding entries one at a time where
    // the stored vector is missing or stale. `source` names the lore in errors ("world 'mini': lore.csv", "host data:
    // lore"). Throws DataError naming the source and the subject when an entry cannot be embedded (too long for the
    // embedding model) or a subject appears twice.
    void SyncLore(std::string_view source, const std::vector<core::LoreEntry>& lore, store::RunStore& store,
                  inference::LlmBackend& backend);

    // SyncLore of the world's lore.csv, named "world '<id>': lore.csv".
    void SyncLore(const core::World& world, store::RunStore& store, inference::LlmBackend& backend);
```

after the `SyncHistory` declaration (its line `    void SyncHistory(store::RunStore& store, int foldedThrough, inference::LlmBackend& backend);`), insert:

```cpp

    // Indexes `folded` (a hosted run's folded turns, as transcript entries) as searchable history: each embedded as
    // its fact cut to EMBED_TEXT_CHARS and shown as "Turn N: <fact>" with the fact cut to 2 * HISTORY_SNIPPET_CHARS,
    // embedding where missing or stale. Replaces every turn entry; notes are left as they are. Throws InferenceError
    // when the backend cannot embed a text (the engine cuts them, so that is a runtime failure).
    void SyncEntries(const std::vector<context::TranscriptEntry>& folded, store::RunStore& store,
                     inference::LlmBackend& backend);
```

and replace

```cpp
    // Vectors for `documents` of one kind, aligned with them: stored ones whose hash still matches, the rest
    // embedded one text per call and stored.
    std::vector<std::vector<float>> Vectors(store::EmbeddingKind kind, const std::vector<Document>& documents,
                                            store::RunStore& store, inference::LlmBackend& backend) const;
```

with

```cpp
    // Vectors for `documents` of one kind, aligned with them: stored ones whose hash still matches, the rest
    // embedded one text per call and stored. `loreSource` names lore in the DataError for an entry that cannot be
    // embedded; it is unused for other kinds.
    std::vector<std::vector<float>> Vectors(store::EmbeddingKind kind, const std::vector<Document>& documents,
                                            std::string_view loreSource, store::RunStore& store,
                                            inference::LlmBackend& backend) const;
```

In `src/turn/Retrieval.cpp`, replace the whole function `void MemoryIndex::SyncLore(const core::World& world, store::RunStore& store, inference::LlmBackend& backend)` (from its signature to its closing brace) with:

```cpp
void MemoryIndex::SyncLore(std::string_view source, const std::vector<core::LoreEntry>& lore, store::RunStore& store,
                           inference::LlmBackend& backend)
{
    std::vector<Document> documents;
    std::vector<Entry> entries;
    std::set<std::string> subjects;
    for (const core::LoreEntry& entry : lore) {
        if (entry.alwaysOn) {
            continue; // Always in L1.
        }
        if (!subjects.insert(entry.subject).second) {
            throw core::DataError(std::format("{} has two entries for subject '{}'", source, entry.subject));
        }
        const std::string text = entry.subject + ": " + entry.data;
        documents.push_back({entry.subject, text});
        entries.push_back({store::EmbeddingKind::Lore, entry.subject, Lowered(entry.subject), text, {}});
    }
    std::vector<std::vector<float>> vectors = Vectors(store::EmbeddingKind::Lore, documents, source, store, backend);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i].vector = std::move(vectors[i]);
    }
    Replace(store::EmbeddingKind::Lore, std::move(entries));
}

void MemoryIndex::SyncLore(const core::World& world, store::RunStore& store, inference::LlmBackend& backend)
{
    SyncLore(std::format("world '{}': lore.csv", world.id), world.lore, store, backend);
}
```

In `MemoryIndex::SyncHistory`, replace

```cpp
    std::vector<std::vector<float>> turnVectors = Vectors(store::EmbeddingKind::Turn, turnDocuments, store, backend);
```

with

```cpp
    std::vector<std::vector<float>> turnVectors = Vectors(store::EmbeddingKind::Turn, turnDocuments, "", store, backend);
```

and

```cpp
    std::vector<std::vector<float>> noteVectors = Vectors(store::EmbeddingKind::Note, noteDocuments, store, backend);
```

with

```cpp
    std::vector<std::vector<float>> noteVectors = Vectors(store::EmbeddingKind::Note, noteDocuments, "", store, backend);
```

After the closing brace of `MemoryIndex::SyncHistory`, insert:

```cpp

void MemoryIndex::SyncEntries(const std::vector<context::TranscriptEntry>& folded, store::RunStore& store,
                              inference::LlmBackend& backend)
{
    // Each folded turn under its run-wide number: its fact is what is embedded and, cut shorter, what is shown.
    std::vector<Document> documents;
    std::vector<Entry> entries;
    for (const context::TranscriptEntry& turn : folded) {
        const std::string refId = std::to_string(turn.turn);
        documents.push_back({refId, core::CutUtf8(turn.fact, EMBED_TEXT_CHARS)});
        entries.push_back({store::EmbeddingKind::Turn, refId, {},
                           std::format("Turn {}: {}", turn.turn, core::CutUtf8(turn.fact, 2 * HISTORY_SNIPPET_CHARS)),
                           {}});
    }
    std::vector<std::vector<float>> vectors = Vectors(store::EmbeddingKind::Turn, documents, "", store, backend);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i].vector = std::move(vectors[i]);
    }
    Replace(store::EmbeddingKind::Turn, std::move(entries));
}
```

Replace the signature line of `MemoryIndex::Vectors`

```cpp
std::vector<std::vector<float>> MemoryIndex::Vectors(store::EmbeddingKind kind, const std::vector<Document>& documents,
                                                     store::RunStore& store, inference::LlmBackend& backend) const
```

with

```cpp
std::vector<std::vector<float>> MemoryIndex::Vectors(store::EmbeddingKind kind, const std::vector<Document>& documents,
                                                     std::string_view loreSource, store::RunStore& store,
                                                     inference::LlmBackend& backend) const
```

and inside it replace

```cpp
                throw core::DataError(
                    std::format("lore.csv entry '{}' cannot be embedded: {}", document.refId, error.what()));
```

with

```cpp
                throw core::DataError(
                    std::format("{} entry '{}' cannot be embedded: {}", loreSource, document.refId, error.what()));
```

and the comment line above it, `        // A lore entry that cannot be embedded is a world problem (too long for the embedding model): a DataError`, with `        // A lore entry that cannot be embedded is a problem of its source (too long for the embedding model): a DataError`.

- [ ] **Step 4: Write HostedSession (without turns)**

Create `src/turn/HostedSession.h`:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/PromptBuilder.h"
#include "context/Transcript.h"
#include "core/HostedRecord.h"
#include "core/Settings.h"
#include "inference/Llm.h"
#include "store/RunStore.h"
#include "turn/HostedWorld.h"
#include "turn/Retrieval.h"

namespace morpheus::turn {

// Every hosted run's world id (store::RunMeta::worldId).
constexpr std::string_view HOSTED_WORLD_ID = "hosted";

// One open hosted run (hosted-world spec, section 1): the counterpart of turn::Session for a world the host owns and
// sends as data. Every backend call happens on the caller's thread. Not thread-safe.
class HostedSession {
private:
    HostedWorld _world; // Set by the constructor.
    core::Settings _settings = {};
    store::RunStore _store; // Set by the constructor.
    inference::LlmBackend* _pBackend = nullptr; // Not owned; outlives the session.
    MemoryIndex _index; // Set by the constructor.
    context::Assembly _lastAssembly = {};

public:
    // Creates the run in `file` when the file does not exist, with `seed` for sampling, and indexes the retrievable
    // lore; or reopens it: the same host data (equal content hash) reopens as it is, changed host data (a mod update)
    // replaces the stored copy, and changed lore is re-embedded. The host data is checked first (HostedWorld's
    // DataError). Throws DataError for a file that holds an owned run, StoreError for a damaged one, and DataError if
    // lore cannot be embedded (a new file is then removed).
    static HostedSession Open(const std::filesystem::path& file, HostData data, core::Settings settings,
                              std::uint64_t seed, inference::LlmBackend& backend);

    // Starts a conversation of `participantIds` (in this join order) and returns its id. Every participant must be an
    // actor of the observation, once; their names and bios are taken from it and stored. Throws DataError naming the
    // field ("participants", "participants/<i>", or the observation's pointer).
    std::string StartConversation(const std::vector<std::string>& participantIds, const nlohmann::json& observation);

    // Adds events to an open conversation without a turn, in order (HostedWorld::CheckEvents). They come before the
    // conversation's next turn. Throws DataError naming the field.
    void RecordEvents(std::string_view conversationId, const std::vector<nlohmann::json>& events);

    // Removes those who `left`, then adds those who `joined` at the end of the join order, with their names and bios
    // from the observation. A conversation keeps at least one participant. Throws DataError naming the field
    // ("left/<i>", "joined/<i>", "left", or the observation's pointer).
    void SetParticipants(std::string_view conversationId, const std::vector<std::string>& joined,
                         const std::vector<std::string>& left, const nlohmann::json& observation);

    // Marks the conversation ended; the next Maintain folds its remaining turns. Throws DataError for an unknown or
    // ended conversation.
    void EndConversation(std::string_view conversationId);

    const store::RunMeta& Meta() const;
    std::optional<core::Conversation> Conversation(std::string_view id);
    std::vector<core::HostedTurn> Turns(std::string_view conversationId);         // Oldest first.
    std::vector<core::ConversationEvent> Events(std::string_view conversationId); // Recorded between turns, in order.
    std::vector<core::Summary> Summaries();
    const context::Assembly& LastAssembly() const; // The last resolve prompt, for the inspector and the tests.

private:
    HostedSession(HostedWorld world, core::Settings settings, store::RunStore store, MemoryIndex index,
                  inference::LlmBackend& backend);

    // Conversation `conversationId`, which must exist and be open. Throws DataError "conversation_id: ..." otherwise.
    core::Conversation OpenConversation(std::string_view conversationId);

    // A conversation's turns as transcript entries, oldest first: the folded ones (folded == true) or those still in
    // L3. A turn's user message is the events before it (recorded since the conversation's previous turn, then its
    // request's own) in parentheses and the player's line, one per line; HOSTED_TURN_CUE when it has neither. Its
    // assistant message is "<speaker> [<action id> <params>]: <line>", without the params when there are none and
    // without the bracket for a refused action. Names come from the turn's own observation.
    std::vector<context::TranscriptEntry> Entries(const core::Conversation& conversation, bool folded);

    // Indexes every conversation's folded turns as searchable history.
    void SyncHistory();
};

} // namespace morpheus::turn
```

Create `src/turn/HostedSession.cpp`:

```cpp
#include "turn/HostedSession.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <system_error>
#include <utility>

#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"

namespace morpheus::turn {

namespace {

// The names of the host's lore in errors.
constexpr std::string_view HOST_LORE_SOURCE = "host data: lore";
// The variables the engine fills in the hosted texts.
constexpr std::string_view SPEAKER_VARIABLE = "{speaker}";
constexpr std::string_view PLAYER_VARIABLE = "{player}";

// `text` with every {speaker} and {player} replaced by the speaker's and the player's names.
std::string FillVariables(std::string_view text, std::string_view speaker, std::string_view player)
{
    std::string filled;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::string_view rest = text.substr(at);
        if (rest.starts_with(SPEAKER_VARIABLE)) {
            filled += speaker;
            at += SPEAKER_VARIABLE.size();
        } else if (rest.starts_with(PLAYER_VARIABLE)) {
            filled += player;
            at += PLAYER_VARIABLE.size();
        } else {
            filled += text[at];
            ++at;
        }
    }
    return filled;
}

} // namespace

HostedSession::HostedSession(HostedWorld world, core::Settings settings, store::RunStore store, MemoryIndex index,
                             inference::LlmBackend& backend)
    : _world(std::move(world))
    , _settings(std::move(settings))
    , _store(std::move(store))
    , _pBackend(&backend)
    , _index(std::move(index))
{
}

HostedSession HostedSession::Open(const std::filesystem::path& file, HostData data, core::Settings settings,
                                  std::uint64_t seed, inference::LlmBackend& backend)
{
    // The host data is checked before the file is touched: a fault is the host's DataError.
    HostedWorld world(data);
    const nlohmann::json stored = HostDataToJson(data);
    const std::string hash = HostDataHash(data);
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error) {
        throw store::StoreError(std::format("cannot check '{}': {}", core::PathText(file), error.message()));
    }

    if (!exists) {
        // A new run: its meta and host data, then the retrievable lore indexed.
        store::RunMeta meta;
        meta.kind = store::RunKind::Hosted;
        meta.worldId = std::string(HOSTED_WORLD_ID);
        meta.worldHash = hash;
        meta.modelId = core::PathText(settings.chatModel.stem());
        meta.rngSeed = seed;
        meta.settings = core::SettingsToJson(settings);
        store::RunStore store = store::RunStore::CreateHosted(file, meta, stored);
        MemoryIndex index(settings.embeddingDocumentPrefix);
        try {
            index.SyncLore(HOST_LORE_SOURCE, data.lore, store, backend);
        } catch (...) {
            // The run never started: close the database (a moved-from store holds no handle), then remove the file.
            { store::RunStore closing = std::move(store); }
            // A file that cannot be removed is deliberately secondary: the caller needs the error that stopped the open.
            std::error_code ignored;
            std::filesystem::remove(file, ignored);
            throw;
        }
        return HostedSession(std::move(world), std::move(settings), std::move(store), std::move(index), backend);
    }

    // An existing run, which must be hosted. Changed host data replaces the stored copy; SyncLore re-embeds only the
    // lore whose text changed (by its content hash), and the folded turns are indexed again.
    store::RunStore store = store::RunStore::Open(file);
    if (store.Meta().kind != store::RunKind::Hosted) {
        throw core::DataError(std::format("run '{}' is an owned run of world '{}', not a hosted run",
                                          core::PathText(file), store.Meta().worldId));
    }
    if (store.Meta().worldHash != hash) {
        store.ReplaceHostData(stored, hash);
    }
    MemoryIndex index(settings.embeddingDocumentPrefix);
    index.SyncLore(HOST_LORE_SOURCE, data.lore, store, backend);
    HostedSession session(std::move(world), std::move(settings), std::move(store), std::move(index), backend);
    session.SyncHistory();
    return session;
}

std::string HostedSession::StartConversation(const std::vector<std::string>& participantIds,
                                             const nlohmann::json& observation)
{
    const Observation observed = _world.Observe(observation);
    if (participantIds.empty()) {
        throw core::DataError("participants: a conversation needs at least one participant");
    }
    // Each participant is an actor of the observation, once; its name and bio come from there.
    std::vector<core::Participant> participants;
    for (std::size_t i = 0; i < participantIds.size(); ++i) {
        const Actor* pActor = observed.Find(participantIds[i]);
        if (pActor == nullptr) {
            throw core::DataError(
                std::format("participants/{}: '{}' is not an actor of the observation", i, participantIds[i]));
        }
        const bool repeated = std::any_of(participants.begin(), participants.end(),
                                          [pActor](const core::Participant& participant) { return participant.id == pActor->id; });
        if (repeated) {
            throw core::DataError(std::format("participants/{}: '{}' is listed twice", i, participantIds[i]));
        }
        participants.push_back({pActor->id, pActor->name, pActor->bio});
    }
    return _store.CreateConversation(participants);
}

void HostedSession::RecordEvents(std::string_view conversationId, const std::vector<nlohmann::json>& events)
{
    const core::Conversation conversation = OpenConversation(conversationId);
    HostedWorld::CheckEvents(events);
    // They follow the conversation's latest turn (0 before its first), so they come before its next.
    const std::vector<core::HostedTurn> played = _store.HostedTurns(conversation.id);
    _store.AddConversationEvents(conversation.id, played.empty() ? 0 : played.back().number, events);
}

void HostedSession::SetParticipants(std::string_view conversationId, const std::vector<std::string>& joined,
                                    const std::vector<std::string>& left, const nlohmann::json& observation)
{
    const core::Conversation conversation = OpenConversation(conversationId);
    const Observation observed = _world.Observe(observation);
    std::vector<core::Participant> participants = conversation.participants;
    // Where participant `id` is in the join order.
    auto position = [&participants](const std::string& id) {
        return std::find_if(participants.begin(), participants.end(),
                            [&id](const core::Participant& participant) { return participant.id == id; });
    };

    // Those who left go first, so a leave and a rejoin in one call moves the actor to the end.
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto it = position(left[i]);
        if (it == participants.end()) {
            throw core::DataError(std::format("left/{}: '{}' is not a participant of conversation '{}'", i, left[i],
                                              conversation.id));
        }
        participants.erase(it);
    }
    // Those who joined go to the end of the join order, with their names and bios from the observation.
    for (std::size_t i = 0; i < joined.size(); ++i) {
        if (position(joined[i]) != participants.end()) {
            throw core::DataError(std::format("joined/{}: '{}' is already a participant of conversation '{}'", i,
                                              joined[i], conversation.id));
        }
        const Actor* pActor = observed.Find(joined[i]);
        if (pActor == nullptr) {
            throw core::DataError(std::format("joined/{}: '{}' is not an actor of the observation", i, joined[i]));
        }
        participants.push_back({pActor->id, pActor->name, pActor->bio});
    }
    if (participants.empty()) {
        throw core::DataError(std::format("left: conversation '{}' would have no participant left", conversation.id));
    }
    _store.SetParticipants(conversation.id, participants);
}

void HostedSession::EndConversation(std::string_view conversationId)
{
    const core::Conversation conversation = OpenConversation(conversationId);
    _store.EndConversation(conversation.id);
}

const store::RunMeta& HostedSession::Meta() const
{
    return _store.Meta();
}

std::optional<core::Conversation> HostedSession::Conversation(std::string_view id)
{
    return _store.Conversation(id);
}

std::vector<core::HostedTurn> HostedSession::Turns(std::string_view conversationId)
{
    return _store.HostedTurns(conversationId);
}

std::vector<core::ConversationEvent> HostedSession::Events(std::string_view conversationId)
{
    return _store.ConversationEvents(conversationId);
}

std::vector<core::Summary> HostedSession::Summaries()
{
    return _store.Summaries();
}

const context::Assembly& HostedSession::LastAssembly() const
{
    return _lastAssembly;
}

core::Conversation HostedSession::OpenConversation(std::string_view conversationId)
{
    std::optional<core::Conversation> conversation = _store.Conversation(conversationId);
    if (!conversation) {
        throw core::DataError(std::format("conversation_id: there is no conversation '{}'", conversationId));
    }
    if (conversation->ended) {
        throw core::DataError(std::format("conversation_id: conversation '{}' has ended", conversationId));
    }
    return std::move(*conversation);
}

std::vector<context::TranscriptEntry> HostedSession::Entries(const core::Conversation& conversation, bool folded)
{
    const std::vector<core::ConversationEvent> recorded = _store.ConversationEvents(conversation.id);
    std::vector<context::TranscriptEntry> entries;
    int previous = 0; // The conversation's turn before the one rendered; 0 before its first.
    for (const core::HostedTurn& turn : _store.HostedTurns(conversation.id)) {
        if ((turn.number <= conversation.foldedThrough) == folded) {
            // Names from the turn's own observation, as it was when the turn was played.
            const std::vector<Actor> actors = HostedWorld::ActorsOf(turn.observation);
            const auto speaker = std::find_if(actors.begin(), actors.end(),
                                              [&turn](const Actor& actor) { return actor.id == turn.speakerId; });
            const auto player = std::find_if(actors.begin(), actors.end(), [](const Actor& actor) { return actor.isPlayer; });
            MORPHEUS_REQUIRE(speaker != actors.end() && player != actors.end(),
                             "HostedSession::Entries: a stored turn's speaker or player is not in its observation");

            // What came before the line: the events since the previous turn, the request's events, the player's line.
            std::vector<std::string> before;
            for (const core::ConversationEvent& event : recorded) {
                if (event.afterTurn == previous) {
                    before.push_back("(" + event.event.at("text").get<std::string>() + ")");
                }
            }
            for (const nlohmann::json& event : turn.events) {
                before.push_back("(" + event.at("text").get<std::string>() + ")");
            }
            if (turn.playerText) {
                before.push_back(player->name + ": " + *turn.playerText);
            }

            // The line under the speaker's name, with the requested action in brackets; a refused one shows none.
            std::string line = speaker->name;
            if (turn.adjudication.requested) {
                const nlohmann::json& params = turn.adjudication.params;
                line += params.empty() ? std::format(" [{}]", turn.adjudication.actionId)
                                       : std::format(" [{} {}]", turn.adjudication.actionId,
                                                     params.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            }
            line += ": " + turn.line;

            // The user message holds what came before, one per line, or the cue; the fact is all of it on one line.
            std::string user;
            std::string fact;
            for (const std::string& part : before) {
                user += (user.empty() ? "" : "\n") + part;
                fact += part + " ";
            }
            if (user.empty()) {
                user = FillVariables(HOSTED_TURN_CUE, speaker->name, player->name);
            }
            fact += line;

            context::TranscriptEntry entry;
            entry.turn = turn.number;
            entry.messages = {{inference::Role::User, std::move(user)}, {inference::Role::Assistant, line}};
            entry.foldMessages = entry.messages;
            entry.fact = std::move(fact);
            entries.push_back(std::move(entry));
        }
        previous = turn.number;
    }
    return entries;
}

void HostedSession::SyncHistory()
{
    // Every conversation's folded turns, rendered as entries, are the run's searchable history.
    std::vector<context::TranscriptEntry> folded;
    for (const core::Conversation& conversation : _store.Conversations()) {
        for (context::TranscriptEntry& entry : Entries(conversation, true)) {
            folded.push_back(std::move(entry));
        }
    }
    _index.SyncEntries(folded, _store, *_pBackend);
}

} // namespace morpheus::turn
```

(`FillVariables` has one caller until Task 7 gives it its second.)

In `src/turn/CMakeLists.txt`, replace

```cmake
    Folding.cpp
    HostedWorld.cpp
```

with

```cmake
    Folding.cpp
    HostedSession.cpp
    HostedWorld.cpp
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[session],[retrieval]"`
Expected: builds with no warnings; `All tests passed` (`Session`'s tests and the hosted session tests carry `[session]`).

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 473` (463 + 10), the three `[server]` tests "Not Run (Skipped)".

- [ ] **Step 6: Byte check and commit**

```bash
FILES="src/turn/Retrieval.h src/turn/Retrieval.cpp src/turn/HostedSession.h src/turn/HostedSession.cpp src/turn/CMakeLists.txt tests/hosted/HostedTestSupport.h tests/hosted/HostedSessionTests.cpp tests/turn/RetrievalTests.cpp"
LC_ALL=C grep -c $'[\x80-\xFF]' $FILES
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES
git add $FILES
git commit -m "Add turn::HostedSession: open, reopen and host data updates, conversations, events, participants" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

### Task 7: `HostedSession::RunTurn`

A hosted turn: check the request (nothing stored, no model call before it passes), retrieve from lore and the run's folded turns, resolve under the proposal grammar through the shared fit loop, adjudicate and report the action, speak streamed on a prompt that extends the resolve prompt, commit. A cancel at any step commits nothing. `Session`'s call telemetry becomes the public `turn::CallTelemetry` so both sessions write it the same way.

**Files:**
- Modify: `src/turn/Session.h`, `src/turn/Session.cpp`
- Modify: `src/turn/HostedSession.h`, `src/turn/HostedSession.cpp`
- Modify: `tests/hosted/HostedTestSupport.h`
- Create: `tests/hosted/HostedTurnTests.cpp`

**Interfaces:**
- Consumes: `MemoryUpkeep`, `FitPass`, `TurnError` (Task 3); `HostedWorld::ProposalSchema`, `ProposalGrammar`, `Adjudicate`, `CheckEvents`, `ActionIds`, `Observation` (Task 4); `context::AssembleHostedPrompt`, `HostedInputs`, `HOSTED_SYSTEM_TEXT`, `HOSTED_RESOLVE_INSTRUCTION`, `HOSTED_SPEAK_INSTRUCTION` (Task 5); `HostedSession` and its private `OpenConversation`, `Entries`, `SyncHistory` (Task 6).
- Produces (`turn/Session.h`): `nlohmann::json turn::CallTelemetry(const inference::Completion& completion, const inference::GenerationSpec& spec)`.
- Produces (`turn/HostedSession.h`): `struct turn::HostedTurnRequest { std::string conversationId; std::string speakerId; std::optional<std::string> playerText; std::vector<nlohmann::json> events; nlohmann::json observation; std::optional<std::vector<std::string>> allowedActions; }`; `struct turn::HostedCallbacks { std::function<void(const core::Adjudication&)> onAction; inference::TokenCallback onToken; }`; `std::optional<core::HostedTurn> HostedSession::RunTurn(const HostedTurnRequest& request, const HostedCallbacks& callbacks, std::stop_token stop)`; private `FitPass Pass(std::string_view conversationId, const std::vector<RetrievalHit>& hits, context::HostedInputs inputs)` and `MemoryUpkeep Upkeep(const std::string& conversationId, int turn)`.
- Produces (`tests/hosted/HostedTestSupport.h`): `struct test::HostedScript { std::vector<std::string> actions; std::vector<std::string> lines; ...; ScriptedBackend::Responder Responder(); }`; `turn::HostedTurnRequest test::Request(const std::string& conversationId, const std::string& speakerId, const std::string& playerText)`.

- [ ] **Step 1: Write the failing tests**

In `tests/hosted/HostedTestSupport.h`, replace

```cpp
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Settings.h"
#include "turn/HostedWorld.h"
```

with

```cpp
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/Settings.h"
#include "fakes/ScriptedBackend.h"
#include "turn/EngineText.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"
```

and before `} // namespace morpheus::test`, insert:

```cpp

// A scripted model for hosted turns. Resolve calls (they carry the proposal grammar) get `actions` in order, speak
// calls (no grammar) get `lines` in order, and fold and condense calls "Summary.". Running out of replies is a test
// bug. The script must outlive the backends that call it.
struct HostedScript {
    std::vector<std::string> actions = {};
    std::vector<std::string> lines = {};
    std::size_t acted = 0;
    std::size_t spoken = 0;

    ScriptedBackend::Responder Responder()
    {
        return [this](const inference::Prompt& prompt, const inference::GenerationSpec& spec) -> std::string {
            const std::string& last = prompt.messages.back().content;
            if (last == turn::SUMMARIZE_INSTRUCTION || last == turn::CONDENSE_INSTRUCTION) {
                return "Summary.";
            }
            if (!spec.grammar.empty()) {
                MORPHEUS_REQUIRE(acted < actions.size(), "HostedScript: no action left");
                return actions[acted++];
            }
            MORPHEUS_REQUIRE(spoken < lines.size(), "HostedScript: no line left");
            return lines[spoken++];
        };
    }
};

// A request in conversation `conversationId`: `speakerId` speaks after `playerText` (no player text when it is
// empty), in observation.json, with every action of the catalog allowed.
inline turn::HostedTurnRequest Request(const std::string& conversationId, const std::string& speakerId,
                                       const std::string& playerText)
{
    turn::HostedTurnRequest request;
    request.conversationId = conversationId;
    request.speakerId = speakerId;
    if (!playerText.empty()) {
        request.playerText = playerText;
    }
    request.observation = FixtureObservation();
    return request;
}
```

Create `tests/hosted/HostedTurnTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "grammar/Check.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"

using namespace morpheus;
using namespace nlohmann::literals;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;

namespace {

const std::string FOLLOW = R"({"action":"follow","params":{}})";
const std::string NONE = R"({"action":"none","params":{}})";

// A hosted run in a fresh file for test `name`, on `backend`.
turn::HostedSession OpenRun(const std::string& name, test::ScriptedBackend& backend)
{
    return turn::HostedSession::Open(test::HostedRunFile(name), test::FixtureHostData(), test::HostedSettings(), 42,
                                     backend);
}

} // namespace

TEST_CASE("A hosted turn resolves, adjudicates, speaks streamed and commits its record", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {FOLLOW};
    script.lines = {"Lead the way, then."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_turn", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());

    std::vector<std::string> order;
    std::string streamed;
    turn::HostedCallbacks callbacks;
    callbacks.onAction = [&](const core::Adjudication& adjudication) { order.push_back("action " + adjudication.actionId); };
    callbacks.onToken = [&](std::string_view piece) {
        if (order.back() != "token") {
            order.push_back("token");
        }
        streamed += piece;
    };
    const std::optional<core::HostedTurn> record =
        session.RunTurn(test::Request(id, test::HULDA, "Hulda, follow me."), callbacks, {});
    REQUIRE(record);
    CHECK(order == std::vector<std::string>{"action follow", "token"}); // The action before the line.
    CHECK(streamed == "Lead the way, then.");

    // The record of section 2, step 4.
    CHECK(record->number == 1);
    CHECK(record->conversationId == id);
    CHECK(record->speakerId == test::HULDA);
    CHECK(record->listenerIds == std::vector<std::string>{test::AELA}); // Saadia is present but takes no part.
    CHECK(record->playerText == std::optional<std::string>("Hulda, follow me."));
    CHECK(record->events.empty());
    CHECK(record->observation == test::FixtureObservation());
    CHECK(record->resolveOutput == FOLLOW);
    CHECK(record->adjudication.requested);
    CHECK(record->adjudication.actionId == "follow");
    CHECK(record->adjudication.params == nlohmann::json::object());
    CHECK(record->line == "Lead the way, then.");
    CHECK(record->timings.contains("resolve"));
    CHECK(record->timings.contains("speak"));
    CHECK(record->timings.contains("retrieval"));
    const std::vector<core::HostedTurn> turns = session.Turns(id);
    REQUIRE(turns.size() == 1);
    CHECK(turns[0].line == record->line);

    // The resolve call runs under the proposal grammar with resolve_max_tokens; the speak call extends its prompt with
    // the resolve output and the speak instruction, and has no grammar.
    const std::vector<test::ScriptedBackend::Call>& calls = backend.Calls();
    REQUIRE(calls.size() == 2);
    const turn::HostedWorld world(test::FixtureHostData());
    const turn::Participants participants = session.Conversation(id)->participants;
    CHECK(calls[0].spec.grammar == world.ProposalGrammar(world.Observe(test::FixtureObservation()), participants,
                                                         world.ActionIds(), test::HULDA));
    CHECK(calls[0].spec.maxTokens == 64);
    const std::string& tail = calls[0].prompt.messages.back().content;
    CHECK(tail.ends_with("# Task\nStep 1: choose the action Hulda tries now. Reply with the JSON object only."));
    CHECK_THAT(tail, ContainsSubstring("# Player\nAela: Hulda, follow me.\n"));
    const inference::Prompt& speak = calls[1].prompt;
    REQUIRE(speak.messages.size() == calls[0].prompt.messages.size() + 2);
    for (std::size_t i = 0; i < calls[0].prompt.messages.size(); ++i) {
        CHECK(speak.messages[i].content == calls[0].prompt.messages[i].content);
    }
    CHECK(speak.messages[speak.messages.size() - 2].role == inference::Role::Assistant);
    CHECK(speak.messages[speak.messages.size() - 2].content == FOLLOW);
    CHECK(speak.messages.back().content ==
          "Step 2: write what Hulda says now. Spoken words only, in character. Aela is the player's character.");
    CHECK(calls[1].spec.grammar.empty());
    CHECK(calls[1].spec.maxTokens == 256);
    CHECK(session.LastAssembly().prompt.messages.size() == calls[0].prompt.messages.size());
}

TEST_CASE("A refused action is kept with its reasons, and the turn speaks with no action", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"wait","params":{},"intent":"rest"})", NONE};
    script.lines = {"As you wish.", "Still here."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_refused", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());

    const std::optional<core::HostedTurn> refused = session.RunTurn(test::Request(id, test::HULDA, "Wait here."), {}, {});
    REQUIRE(refused);
    CHECK_FALSE(refused->adjudication.requested);
    CHECK(refused->adjudication.reasons ==
          std::vector<std::string>{"/intent: unexpected key 'intent'; the keys are 'action', 'params'"});
    CHECK(refused->resolveOutput == R"({"action":"wait","params":{},"intent":"rest"})");
    CHECK(refused->line == "As you wish.");
    CHECK(refused->timings.at("resolve").at("refused") == nlohmann::json(refused->adjudication.reasons));

    // In L3 the refused turn shows no bracket.
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Are you there?"), {}, {}));
    const inference::Prompt& prompt = session.LastAssembly().prompt;
    REQUIRE(prompt.stableCount == 3);
    CHECK(prompt.messages[1].content == "Aela: Wait here.");
    CHECK(prompt.messages[2].content == "Hulda: As you wish.");
}

TEST_CASE("A turn between NPCs needs no player text", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {NONE, FOLLOW, NONE};
    script.lines = {"Busy night.", "I will help you.", "Quiet again."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_npc", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA}, test::FixtureObservation());

    const std::optional<core::HostedTurn> first = session.RunTurn(test::Request(id, test::SAADIA, ""), {}, {});
    REQUIRE(first);
    CHECK_FALSE(first->playerText);
    CHECK(first->listenerIds == std::vector<std::string>{test::HULDA});
    CHECK_THAT(backend.Calls().at(0).prompt.messages.back().content, !ContainsSubstring("# Player"));

    // Hulda answers: Saadia's turn shows in L3 with the cue where a player's line would be.
    const std::optional<core::HostedTurn> second = session.RunTurn(test::Request(id, test::HULDA, ""), {}, {});
    REQUIRE(second);
    CHECK(second->listenerIds == std::vector<std::string>{test::SAADIA});
    const inference::Prompt& prompt = session.LastAssembly().prompt;
    REQUIRE(prompt.stableCount == 3);
    CHECK(prompt.messages[1].content == "(Saadia speaks.)");
    CHECK(prompt.messages[2].content == "Saadia [none]: Busy night.");

    // A player text of only spaces and control characters is no player text.
    const std::optional<core::HostedTurn> third = session.RunTurn(test::Request(id, test::SAADIA, " \t \n"), {}, {});
    REQUIRE(third);
    CHECK_FALSE(third->playerText);
}

TEST_CASE("The prompt is byte-identical across turns of one conversation up to the tail", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {FOLLOW, NONE, NONE};
    script.lines = {"Lead the way, then.", "Right behind you.", "Here we are."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_prefix", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());

    std::vector<inference::Prompt> prompts;
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hulda, follow me."), {}, {}));
    prompts.push_back(session.LastAssembly().prompt);
    session.RecordEvents(id, {R"({"type": "action_result", "action": "follow", "actor": "0001A6A4", "succeeded": true,
                                 "text": "Hulda is now following Aela."})"_json});
    turn::HostedTurnRequest second = test::Request(id, test::HULDA, "To the gate.");
    second.events = {R"({"type": "ambient", "text": "The door creaks."})"_json};
    REQUIRE(session.RunTurn(second, {}, {}));
    prompts.push_back(session.LastAssembly().prompt);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Stop here."), {}, {}));
    prompts.push_back(session.LastAssembly().prompt);

    // Each prompt's stable part (the system message and L3) is the start of the next one's, message for message: a
    // new turn only adds to the end of the cached part.
    for (std::size_t k = 0; k + 1 < prompts.size(); ++k) {
        INFO("turns " << k + 1 << " and " << k + 2);
        const inference::Prompt& before = prompts[k];
        const inference::Prompt& after = prompts[k + 1];
        REQUIRE(after.stableCount == before.stableCount + 2);
        for (std::size_t i = 0; i < before.stableCount; ++i) {
            CHECK(after.messages[i].role == before.messages[i].role);
            CHECK(after.messages[i].content == before.messages[i].content);
        }
    }

    // The second turn's tail holds both new events; the third's L3 shows them before the player's line.
    CHECK_THAT(prompts[1].messages.back().content,
               ContainsSubstring("# New events\n(Hulda is now following Aela.)\n(The door creaks.)\n"));
    const inference::Prompt& last = prompts[2];
    REQUIRE(last.stableCount == 5);
    CHECK(last.messages[1].content == "Aela: Hulda, follow me.");
    CHECK(last.messages[2].content == "Hulda [follow]: Lead the way, then.");
    CHECK(last.messages[3].content == "(Hulda is now following Aela.)\n(The door creaks.)\nAela: To the gate.");
    CHECK(last.messages[4].content == "Hulda [none]: Right behind you.");
    CHECK_THAT(last.messages.back().content, !ContainsSubstring("# New events"));
}

TEST_CASE("A join or a leave changes the prompt only from the participants layer on", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {FOLLOW, NONE, NONE};
    script.lines = {"Lead the way, then.", "Welcome, Saadia.", "Just us now."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_join", backend);
    const nlohmann::json observation = test::FixtureObservation();
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, observation);

    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hulda, follow me."), {}, {}));
    const inference::Prompt before = session.LastAssembly().prompt;
    session.SetParticipants(id, {test::SAADIA}, {}, observation);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Saadia, join us."), {}, {}));
    const inference::Prompt joined = session.LastAssembly().prompt;

    // Saadia joins at the end of the participants layer, which ends the system message here (no summary yet): the
    // first turn's system message is the start of the second's.
    const std::string& a = before.messages[0].content;
    const std::string& b = joined.messages[0].content;
    CHECK(b.starts_with(a));
    CHECK(b.substr(a.size()) == "- Saadia (0001B07C): Saadia serves at the inn.\n");
    CHECK(joined.messages[1].content == "Aela: Hulda, follow me.");

    // A leave changes the system message from the leaver's line on: L0, L1 and the participants before it stay.
    session.SetParticipants(id, {}, {test::AELA}, observation);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, ""), {}, {}));
    const std::string c = session.LastAssembly().prompt.messages[0].content;
    const std::size_t aelaLine = b.find("- Aela (00000007)\n");
    REQUIRE(aelaLine != std::string::npos);
    CHECK(c.substr(0, aelaLine) == b.substr(0, aelaLine));
    CHECK(c.substr(aelaLine) == "- Saadia (0001B07C): Saadia serves at the inn.\n");
}

TEST_CASE("A cancel during the resolve or the speak call commits nothing", "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {FOLLOW, FOLLOW, FOLLOW};
    script.lines = {"Lead the way.", "Lead the way."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_cancel", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, "Follow me.");
    request.events = {R"({"text": "Rain starts."})"_json};

    // Cancelled before the resolve call writes anything.
    std::stop_source early;
    early.request_stop();
    CHECK_FALSE(session.RunTurn(request, {}, early.get_token()));

    // Cancelled while the line streams: onAction asks for the stop, so the speak call ends at its first piece.
    std::stop_source late;
    bool acted = false;
    turn::HostedCallbacks callbacks;
    callbacks.onAction = [&](const core::Adjudication&) {
        acted = true;
        late.request_stop();
    };
    CHECK_FALSE(session.RunTurn(request, callbacks, late.get_token()));
    CHECK(acted);
    CHECK(session.Turns(id).empty());
    CHECK(session.Events(id).empty()); // The request's events were not kept either.

    // The same request then plays as turn 1, with its events.
    const std::optional<core::HostedTurn> record = session.RunTurn(request, {}, {});
    REQUIRE(record);
    CHECK(record->number == 1);
    CHECK(record->events == request.events);
}

TEST_CASE("Each malformed request is a DataError naming its field, before any model call", "[hosted][turn]")
{
    test::HostedScript script; // No reply: a model call would fail the test.
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_bad_requests", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    const std::string ended = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    session.EndConversation(ended);
    // RunTurn of `request` is a DataError whose message holds `field`.
    auto fails = [&](const turn::HostedTurnRequest& request, const std::string& field) {
        INFO(field);
        CHECK_THROWS_MATCHES(session.RunTurn(request, {}, {}), core::DataError, MessageMatches(ContainsSubstring(field)));
    };

    fails(test::Request("c9", test::HULDA, "Hi."), "conversation_id: there is no conversation 'c9'");
    fails(test::Request(ended, test::HULDA, "Hi."), "conversation_id: conversation 'c2' has ended");
    turn::HostedTurnRequest badObservation = test::Request(id, test::HULDA, "Hi.");
    badObservation.observation.at("actors").at(0)["id"] = "";
    fails(badObservation, "observation at /actors/0/id");
    fails(test::Request(id, test::SAADIA, "Hi."), "speaker: '0001B07C' is not a participant of conversation 'c1'");
    fails(test::Request(id, test::AELA, "Hi."), "speaker: '00000007' is the player");
    turn::HostedTurnRequest absent = test::Request(id, test::HULDA, "Hi.");
    nlohmann::json& actors = absent.observation.at("actors");
    actors.erase(actors.begin()); // Hulda is gone; Saadia and Aela stay.
    fails(absent, "speaker: '0001A6A4' is not an actor of the observation");
    turn::HostedTurnRequest unknownAction = test::Request(id, test::HULDA, "Hi.");
    unknownAction.allowedActions = std::vector<std::string>{"follow", "fly"};
    fails(unknownAction, "allowed_actions/1: 'fly' is not an action of the catalog");
    turn::HostedTurnRequest badEvent = test::Request(id, test::HULDA, "Hi.");
    badEvent.events = {R"({"type": "ambient"})"_json};
    fails(badEvent, "events/0/text: an event needs 'text', a string");

    CHECK(backend.Calls().empty());
    CHECK(session.Turns(id).empty());
}

TEST_CASE("A player text with no room in the tail allowance is a TurnError", "[hosted][turn]")
{
    test::HostedScript script;
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_long_text", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    const turn::HostedTurnRequest request = test::Request(id, test::HULDA, std::string(5000, 'x'));
    CHECK_THROWS_AS(session.RunTurn(request, {}, {}), turn::TurnError);
    CHECK_THROWS_WITH(session.RunTurn(request, {}, {}),
                      ContainsSubstring("the observation, the events and the player text use"));
    CHECK(backend.Calls().empty());
    CHECK(session.Turns(id).empty());
}

TEST_CASE("allowedActions narrows the grammar the resolve call runs under and the schema it is checked against",
          "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"attack","params":{"target":"0001B07C"}})", NONE};
    script.lines = {"Leave, now.", "Fine."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_allowed", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, "Throw her out.");
    request.allowedActions = std::vector<std::string>{"follow", "attack"};

    const std::optional<core::HostedTurn> record = session.RunTurn(request, {}, {});
    REQUIRE(record);
    CHECK(record->adjudication.requested);
    CHECK(record->adjudication.params == R"({"target": "0001B07C"})"_json);
    const std::string& gbnf = backend.Calls().at(0).spec.grammar;
    CHECK(grammar::Check(gbnf, FOLLOW).empty());
    CHECK(grammar::Check(gbnf, R"({"action":"attack","params":{"target":"0001B07C"}})").empty());
    CHECK_FALSE(grammar::Check(gbnf, NONE).empty());

    // An action the turn does not allow is refused even when the model writes it.
    const std::optional<core::HostedTurn> next = session.RunTurn(request, {}, {});
    REQUIRE(next);
    CHECK_FALSE(next->adjudication.requested);
    REQUIRE(next->adjudication.reasons.size() == 1);
    CHECK(next->adjudication.reasons[0].starts_with("/action: unknown action 'none'"));
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors, among them `'HostedTurnRequest': is not a member of 'morpheus::turn'` and `'RunTurn': is not a member of 'morpheus::turn::HostedSession'`.

- [ ] **Step 3: Make the call telemetry public**

In `src/turn/Session.h`, after the line `inference::Sampling SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed);`, insert:

```cpp

// One model call's numbers for a turn's telemetry (core::TurnRecord::timings, core::HostedTurn::timings). A call that
// was allowed to think also gets its thinking block: its numbers and its text, kept so the turn log can show why the
// model did what it did. The thinking text goes nowhere else: not into the narration, the transcript or a later
// prompt.
nlohmann::json CallTelemetry(const inference::Completion& completion, const inference::GenerationSpec& spec);
```

In `src/turn/Session.cpp`, delete the anonymous-namespace function `Telemetry` (its comment `// One model call's numbers for the turn telemetry (TurnRecord::timings).` with the two lines after it, the function, and the blank line after it). After the closing brace of `SamplingFrom`, insert:

```cpp

nlohmann::json CallTelemetry(const inference::Completion& completion, const inference::GenerationSpec& spec)
{
    nlohmann::json telemetry = {{"prompt_tokens", completion.promptTokens},  {"reused", completion.tokensReused},
                                {"evaluated", completion.tokensEvaluated},   {"generated", completion.tokensGenerated},
                                {"prompt_ms", completion.promptMs},          {"generate_ms", completion.generateMs},
                                {"stopped_at_limit", completion.stoppedAtLimit}};
    if (spec.thinkingMaxTokens > 0) {
        telemetry["thinking"] = {{"tokens", completion.thinkingTokens},
                                 {"ms", completion.thinkingMs},
                                 {"stopped_at_limit", completion.thinkingStoppedAtLimit},
                                 {"text", completion.thinking}};
    }
    return telemetry;
}
```

Then replace each of the five calls `Telemetry(` in `Session::PlayTurn` with `CallTelemetry(`: `Telemetry(looked, lookupSpec)`, `Telemetry(narrated, narrateSpec)`, `Telemetry(reviewed, spec)`, `Telemetry(renarrated, narrateSpec)` and `Telemetry(resolved, resolveSpec)`. Check: `grep -n "[^l]Telemetry(" src/turn/Session.cpp` prints nothing.

- [ ] **Step 4: Write RunTurn**

In `src/turn/HostedSession.h`, replace

```cpp
#include <cstdint>
#include <filesystem>
#include <optional>
```

with

```cpp
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
```

replace `#include "turn/HostedWorld.h"` with

```cpp
#include "turn/HostedWorld.h"
#include "turn/MemoryUpkeep.h"
```

after the line `constexpr std::string_view HOSTED_WORLD_ID = "hosted";`, insert:

```cpp

// One hosted turn as the host asks for it (hosted-world spec, section 1).
struct HostedTurnRequest {
    std::string conversationId = {};
    std::string speakerId = {};
    std::optional<std::string> playerText = {}; // Absent for a turn between NPCs.
    std::vector<nlohmann::json> events = {};    // Objects with a `text` string, added to the conversation before the turn.
    nlohmann::json observation = nlohmann::json::object();
    std::optional<std::vector<std::string>> allowedActions = {}; // Action ids; absent means every action of the catalog.
};

// What a hosted turn reports while it runs.
struct HostedCallbacks {
    // After the resolve call, before the line starts: what became of the speaker's proposed action.
    std::function<void(const core::Adjudication&)> onAction = {};
    // The line's pieces as they are written.
    inference::TokenCallback onToken = {};
};
```

after the `SetParticipants` declaration (its line `                         const std::vector<std::string>& left, const nlohmann::json& observation);`), insert:

```cpp

    // Plays one turn (hosted-world spec, section 2): checks the request, retrieves from lore and the run's folded
    // turns, resolves under the proposal grammar (the fit loop folding the conversation's oldest turns when L3 is
    // over its budget), adjudicates and reports the action through onAction, speaks on a prompt that extends the
    // resolve prompt (streamed through onToken), and commits the record. Player text is cleaned as Session cleans
    // input; nothing left means none. Returns the committed turn, or nullopt when `stop` cancelled it: then no turn is
    // committed and the request's events are not kept (folds the fit loop finished before the stop are, as in
    // Session). Throws DataError naming the field for a malformed request (before any model call), TurnError when
    // the tail does not fit tail_allowance, and InferenceError from the backend. Model mistakes are not exceptions: a
    // refused action is recorded with its reasons and the turn goes on with no action.
    std::optional<core::HostedTurn> RunTurn(const HostedTurnRequest& request, const HostedCallbacks& callbacks,
                                            std::stop_token stop);
```

and after the `SyncHistory` declaration (its line `    void SyncHistory();`), insert:

```cpp

    // One pass of the memory upkeep for a turn of conversation `conversationId`: the hosted prompt for `hits`, with
    // the tail parts `inputs` already holds (observation, events, player line, instruction) and the conversation, the
    // summaries and its unfolded entries read from the store as it is now; and the entries its L3 shows.
    FitPass Pass(std::string_view conversationId, const std::vector<RetrievalHit>& hits, context::HostedInputs inputs);

    // The shared memory upkeep for turn `turn` of conversation `conversationId`: a fold's summary joins the store with
    // the conversation's folded mark, and the folded turns become searchable history.
    MemoryUpkeep Upkeep(const std::string& conversationId, int turn);
```

In `src/turn/HostedSession.cpp`, replace

```cpp
#include <algorithm>
#include <cstddef>
#include <format>
#include <system_error>
#include <utility>

#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"
```

with

```cpp
#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <system_error>
#include <utility>

#include "core/Dice.h"
#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"
#include "turn/Session.h"
```

replace

```cpp
// The names of the host's lore in errors.
constexpr std::string_view HOST_LORE_SOURCE = "host data: lore";
```

with

```cpp
// The names of the host's lore in errors.
constexpr std::string_view HOST_LORE_SOURCE = "host data: lore";
// What a hosted tail holds, for the TurnError when it does not fit tail_allowance.
constexpr std::string_view HOSTED_TAIL_NAME = "the observation, the events and the player text";
// A hosted turn retrieves from the host's lore and the run's folded turns; a hosted run has no notes.
constexpr std::array<store::EmbeddingKind, 2> HOSTED_SOURCES = {store::EmbeddingKind::Lore, store::EmbeddingKind::Turn};
```

After the definition of `HostedSession::SetParticipants` (before `void HostedSession::EndConversation`), insert:

```cpp
std::optional<core::HostedTurn> HostedSession::RunTurn(const HostedTurnRequest& request,
                                                       const HostedCallbacks& callbacks, std::stop_token stop)
{
    // 0. Check the request: nothing is stored and no model call is made before it passes. The conversation exists and
    // is open, the observation passes Observe, the speaker takes part, is present and is not the player, every
    // allowed action is in the catalog with one left after the actor scopes, and every event has its text.
    const core::Conversation conversation = OpenConversation(request.conversationId);
    const Observation observation = _world.Observe(request.observation);
    const bool participates = std::any_of(conversation.participants.begin(), conversation.participants.end(),
                                          [&](const core::Participant& participant) { return participant.id == request.speakerId; });
    if (!participates) {
        throw core::DataError(std::format("speaker: '{}' is not a participant of conversation '{}'", request.speakerId,
                                          conversation.id));
    }
    const Actor* pSpeaker = observation.Find(request.speakerId);
    if (pSpeaker == nullptr) {
        throw core::DataError(std::format("speaker: '{}' is not an actor of the observation", request.speakerId));
    }
    if (pSpeaker->isPlayer) {
        throw core::DataError(
            std::format("speaker: '{}' is the player; the host speaks for the player", request.speakerId));
    }
    const std::vector<std::string> allowed = request.allowedActions.value_or(_world.ActionIds());
    const nlohmann::json proposalSchema =
        _world.ProposalSchema(observation, conversation.participants, allowed, request.speakerId);
    HostedWorld::CheckEvents(request.events);

    // Player text is cleaned as Session cleans input: control characters other than newline and tab go (they have no
    // place in a prompt), and it is trimmed. Nothing left means no player text.
    std::optional<std::string> playerText;
    if (request.playerText) {
        std::string kept;
        for (const char c : *request.playerText) {
            const auto byte = static_cast<unsigned char>(c);
            if ((byte >= 0x20 && byte != 0x7F) || c == '\n' || c == '\t') {
                kept += c;
            }
        }
        std::string cleaned = core::Trim(kept);
        if (!cleaned.empty()) {
            playerText = std::move(cleaned);
        }
    }

    // The instructions name the speaker and the player. The speak instruction must fit narrate_allowance, which the
    // prompt reserves for it; the names come from the host.
    const Actor& player = observation.Player();
    const std::string resolveInstruction = FillVariables(HOSTED_RESOLVE_INSTRUCTION, pSpeaker->name, player.name);
    const std::string speakMessage = FillVariables(HOSTED_SPEAK_INSTRUCTION, pSpeaker->name, player.name);
    const int speakTokens = _pBackend->CountTokens(speakMessage);
    if (speakTokens > _settings.budget.narrateAllowance) {
        throw core::DataError(std::format("speaker: the speak instruction for '{}' uses {} tokens, over "
                                          "narrate_allowance {}",
                                          pSpeaker->name, speakTokens, _settings.budget.narrateAllowance));
    }

    const int turn = _store.LatestHostedTurn() + 1; // Run-wide, shared with the summaries' ranges.
    const std::uint64_t seed = _store.Meta().rngSeed;
    const std::vector<core::HostedTurn> played = _store.HostedTurns(conversation.id);
    const int lastTurn = played.empty() ? 0 : played.back().number;

    // The new events: those recorded since the conversation's last turn, then the request's, in order.
    std::vector<std::string> newEvents;
    for (const core::ConversationEvent& recorded : _store.ConversationEvents(conversation.id)) {
        if (recorded.afterTurn == lastTurn) {
            newEvents.push_back(recorded.event.at("text").get<std::string>());
        }
    }
    for (const nlohmann::json& event : request.events) {
        newEvents.push_back(event.at("text").get<std::string>());
    }

    // 1. Retrieve from lore and the run's folded turns (this and earlier conversations), with the player text, the
    // speaker's name and the previous line.
    RetrievalQuery query;
    query.input = playerText.value_or("");
    query.scene = {pSpeaker->name};
    query.digest = played.empty() ? std::string() : played.back().line;
    std::vector<RetrievalHit> hits =
        _index.Retrieve(query, HOSTED_SOURCES, _settings.budget.retrievalCap, RETRIEVAL_MAX_HITS, *_pBackend);

    // 2. Resolve: a prompt that fits (the fit loop sheds hits, condenses and folds), under the proposal grammar.
    context::HostedInputs tail;
    tail.observation = observation.json;
    tail.events = newEvents;
    tail.playerLine = playerText ? player.name + ": " + *playerText : std::string();
    tail.instruction = resolveInstruction;
    std::optional<context::Assembly> assembly = Upkeep(conversation.id, turn).Fit(
        hits, [&](const std::vector<RetrievalHit>& kept) { return Pass(conversation.id, kept, tail); }, stop);
    if (!assembly) {
        return std::nullopt;
    }
    _lastAssembly = *assembly;
    inference::GenerationSpec resolveSpec;
    resolveSpec.grammar = _world.ProposalGrammar(observation, conversation.participants, allowed, request.speakerId);
    resolveSpec.maxTokens = _settings.budget.resolveMaxTokens;
    resolveSpec.thinkingMaxTokens = _settings.budget.resolveThinkingTokens;
    resolveSpec.sampling =
        SamplingFrom(_settings.resolveSampling, core::SamplingSeed(seed, turn, core::CallKind::Resolve, 0));
    const inference::Completion resolved = _pBackend->Complete(assembly->prompt, resolveSpec, {}, stop);
    if (resolved.cancelled) {
        return std::nullopt;
    }
    // A refused action is the model's mistake: its reasons are kept, and the speaker takes no action.
    const core::Adjudication adjudication = _world.Adjudicate(proposalSchema, resolved.text);
    if (callbacks.onAction) {
        callbacks.onAction(adjudication);
    }

    // 3. Speak: the resolve prompt, the resolve output as the model wrote it, and the speak instruction, so nearly all
    // of the prompt stays cached. No grammar; the line streams.
    inference::Prompt speakPrompt = assembly->prompt;
    context::ExtendForNarrate(speakPrompt, resolved.text, speakMessage);
    inference::GenerationSpec speakSpec;
    speakSpec.maxTokens = _settings.budget.narrateMaxTokens;
    speakSpec.thinkingMaxTokens = _settings.budget.narrateThinkingTokens;
    speakSpec.sampling =
        SamplingFrom(_settings.narrateSampling, core::SamplingSeed(seed, turn, core::CallKind::Narrate, 0));
    const inference::Completion spoken = _pBackend->Complete(speakPrompt, speakSpec, callbacks.onToken, stop);
    if (spoken.cancelled) {
        return std::nullopt;
    }

    // 4. Commit the record: the listeners are the other participants present.
    core::HostedTurn record;
    record.number = turn;
    record.conversationId = conversation.id;
    record.speakerId = request.speakerId;
    for (const core::Participant& participant : conversation.participants) {
        if (participant.id != request.speakerId && observation.Find(participant.id) != nullptr) {
            record.listenerIds.push_back(participant.id);
        }
    }
    record.playerText = playerText;
    record.events = request.events;
    record.observation = observation.json;
    record.resolveOutput = resolved.text;
    record.adjudication = adjudication;
    record.line = core::Trim(spoken.text);
    nlohmann::json timings = nlohmann::json::object();
    timings["resolve"] = CallTelemetry(resolved, resolveSpec);
    if (!adjudication.requested) {
        timings["resolve"]["refused"] = adjudication.reasons; // The log of why the speaker took no action.
    }
    timings["speak"] = CallTelemetry(spoken, speakSpec);
    timings["retrieval"] = nlohmann::json::array(); // The hits that made it into the prompt, and why they matched.
    for (const RetrievalHit& hit : hits) {
        timings["retrieval"].push_back({{"source", hit.kind == store::EmbeddingKind::Lore ? "lore" : "history"},
                                        {"ref", hit.refId},
                                        {"score", hit.score},
                                        {"reasons", hit.reasons}});
    }
    record.timings = std::move(timings);
    _store.CommitHostedTurn(record);
    return record;
}

```

After the definition of `HostedSession::SyncHistory` (at the end, before `} // namespace morpheus::turn`), insert:

```cpp

FitPass HostedSession::Pass(std::string_view conversationId, const std::vector<RetrievalHit>& hits,
                            context::HostedInputs inputs)
{
    // The conversation as the store holds it now: a fold since the last pass moved its folded mark.
    const std::optional<core::Conversation> conversation = _store.Conversation(conversationId);
    MORPHEUS_REQUIRE(conversation.has_value(), "HostedSession::Pass: the conversation is not in the store");
    inputs.systemText = std::string(HOSTED_SYSTEM_TEXT);
    inputs.lore = _world.Lore();
    inputs.participants = conversation->participants;
    inputs.summaries = _store.Summaries();
    inputs.transcript = Entries(*conversation, false);
    inputs.retrieved.clear();
    for (const RetrievalHit& hit : hits) {
        inputs.retrieved.push_back(hit.text);
    }

    FitPass pass;
    pass.assembly = context::AssembleHostedPrompt(inputs, _settings.budget, *_pBackend);
    pass.transcript = std::move(inputs.transcript);
    return pass;
}

MemoryUpkeep HostedSession::Upkeep(const std::string& conversationId, int turn)
{
    // A fold's summary joins the store with the conversation's folded mark, and the folded turns become history.
    return MemoryUpkeep(_store, *_pBackend, _settings, std::string(HOSTED_TAIL_NAME), turn, 0,
                        [this, conversationId](const core::Summary& summary) {
                            _store.FoldConversation(conversationId, summary);
                            SyncHistory();
                        });
}
```

`FillVariables` now has its second caller (`Entries` was the first); `Pass` and `Upkeep` get their second caller, `Maintain`, in Task 8.

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[hosted],[session]"`
Expected: builds with no warnings; `All tests passed`.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 482` (473 + 9), the three `[server]` tests "Not Run (Skipped)".

- [ ] **Step 6: Run the AddressSanitizer preset on the hosted tests**

Run: `cmake --preset asan && cmake --build --preset asan && ./build/asan/bin/Debug/morpheus_tests.exe "[hosted],[upkeep]"`
Expected: `All tests passed`, no AddressSanitizer report (the upkeep holds the session's members by pointer only while a turn runs).

- [ ] **Step 7: Byte check and commit**

```bash
FILES="src/turn/Session.h src/turn/Session.cpp src/turn/HostedSession.h src/turn/HostedSession.cpp tests/hosted/HostedTestSupport.h tests/hosted/HostedTurnTests.cpp"
LC_ALL=C grep -c $'[\x80-\xFF]' $FILES
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES
git add $FILES
git commit -m "HostedSession::RunTurn: check, retrieve, resolve and adjudicate, speak streamed, commit; CallTelemetry public" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

### Task 8: Folding in hosted runs: over budget, and `EndConversation` plus `Maintain`

When a conversation's L3 is over its budget, Task 7's fit loop already folds its oldest turns through the shared upkeep; this task pins that with a test and adds `Maintain`, which folds every remaining turn of each ended conversation (in one call when it fits), condensing on the way, so a later conversation sees the summary in L2 and retrieves the folded turns.

**Files:**
- Modify: `src/turn/HostedSession.h`, `src/turn/HostedSession.cpp`
- Create: `tests/hosted/HostedFoldingTests.cpp`

**Interfaces:**
- Consumes: `MemoryUpkeep::FoldAll` (Task 3); `HostedSession::Pass`, `Upkeep` (Task 7); `HostedTestSupport.h` (Tasks 4, 6, 7).
- Produces: `bool HostedSession::Maintain(std::stop_token stop)`.

- [ ] **Step 1: Write the failing tests**

Create `tests/hosted/HostedFoldingTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/HostedRecord.h"
#include "core/Settings.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// For a context of 2600 with lines of 2400 bytes. FakeBackend counts 1 token per 4 bytes: the hosted system message
// is about 350 tokens, so L3 gets about 1400 while each turn adds about 615. Two turns fit the conversation, three do
// not. The tail (the observation is about 190 tokens) stays well within its 600.
core::Settings FoldingSettings()
{
    core::Settings settings = test::HostedSettings();
    settings.budget = {64, 128, 64, 600, 1024, 1024, 512, 100};
    return settings;
}

} // namespace

TEST_CASE("A conversation over its transcript budget folds its oldest turns during a turn", "[hosted][folding]")
{
    test::HostedScript script;
    for (int i = 0; i < 6; ++i) {
        script.actions.push_back(NONE);
        script.lines.push_back(std::string(2400, 'w'));
    }
    test::ScriptedBackend backend(2600, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fold_budget"),
                                                            test::FixtureHostData(), FoldingSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    for (int i = 0; i < 6; ++i) {
        REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Go on."), {}, {}));
        CHECK(session.LastAssembly().Fits());
    }

    // The oldest turns went into run-wide summaries, and the conversation's folded mark moved with them.
    const std::vector<core::Summary> summaries = session.Summaries();
    REQUIRE_FALSE(summaries.empty());
    CHECK(summaries.front().fromTurn == 1);
    CHECK(session.Conversation(id)->foldedThrough >= 1);
    const inference::Prompt& prompt = session.LastAssembly().prompt;
    CHECK(prompt.stableCount < 1 + 2 * 5);
    CHECK_THAT(prompt.messages.front().content, ContainsSubstring("# Story so far\nTurns 1-"));
}

TEST_CASE("EndConversation and Maintain fold the rest, and a later conversation retrieves it", "[hosted][folding]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {"The silver key is under the bar.", "Nobody else knows about the silver key.", "Under the bar."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fold_end"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string first = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(first, test::HULDA, "Where did you hide the silver key?"), {}, {}));
    REQUIRE(session.RunTurn(test::Request(first, test::HULDA, "Does anyone else know?"), {}, {}));

    // While the conversation is open, Maintain leaves it alone.
    CHECK(session.Maintain({}));
    CHECK(session.Summaries().empty());

    // Ended, its remaining turns fold in one call.
    session.EndConversation(first);
    CHECK(session.Maintain({}));
    const std::vector<core::Summary> summaries = session.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 2);
    CHECK(summaries[0].text == "Summary.");
    CHECK(session.Conversation(first)->foldedThrough == 2);
    const std::size_t calls = backend.Calls().size();
    CHECK(session.Maintain({})); // Nothing is left to fold.
    CHECK(backend.Calls().size() == calls);

    // A later conversation sees the summary in L2, starts with an empty L3, and retrieves the folded turns.
    const std::string second = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(second, test::HULDA, "Remind me where the silver key is."), {}, {}));
    const inference::Prompt& prompt = session.LastAssembly().prompt;
    CHECK_THAT(prompt.messages.front().content, ContainsSubstring("# Story so far\nTurns 1-2: Summary.\n"));
    CHECK(prompt.stableCount == 1);
    CHECK_THAT(prompt.messages.back().content,
               ContainsSubstring("\n- Turn 1: Aela: Where did you hide the silver key? Hulda [none]: The silver key "
                                 "is under the bar.\n"));
}

TEST_CASE("A cancelled Maintain folds nothing and can simply run again", "[hosted][folding]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE};
    script.lines = {"One.", "Two."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fold_cancel"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "First."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Second."), {}, {}));
    session.EndConversation(id);

    std::stop_source stop;
    stop.request_stop();
    CHECK_FALSE(session.Maintain(stop.get_token()));
    CHECK(session.Summaries().empty());
    CHECK(session.Conversation(id)->foldedThrough == 0);

    CHECK(session.Maintain({}));
    REQUIRE(session.Summaries().size() == 1);
    CHECK(session.Conversation(id)->foldedThrough == 2);
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile error `'Maintain': is not a member of 'morpheus::turn::HostedSession'` in `tests/hosted/HostedFoldingTests.cpp`. (The first test needs no new code: Task 7's fit loop folds; it pins that behaviour.)

- [ ] **Step 3: Write Maintain**

In `src/turn/HostedSession.h`, after the `EndConversation` declaration (its line `    void EndConversation(std::string_view conversationId);`), insert:

```cpp

    // Between-turn upkeep, through the shared memory upkeep: folds every remaining turn of each ended conversation into
    // summaries (in one call when it fits), condensing summaries over their cap on the way, and indexes the folded
    // turns for retrieval. Returns false when `stop` cancelled it; it can simply run again. Throws DataError when
    // summary_cap is too small for even a condensed summary. Open conversations are left to their turns' fit loop.
    bool Maintain(std::stop_token stop);
```

In `src/turn/HostedSession.cpp`, replace

```cpp
// A hosted turn retrieves from the host's lore and the run's folded turns; a hosted run has no notes.
```

with

```cpp
// The task line of the placeholder tail Maintain measures an ended conversation's prompt with.
constexpr std::string_view MAINTENANCE_INSTRUCTION = "(the conversation's next turn)";
// A hosted turn retrieves from the host's lore and the run's folded turns; a hosted run has no notes.
```

and after the definition of `HostedSession::EndConversation`, insert:

```cpp

bool HostedSession::Maintain(std::stop_token stop)
{
    // Every ended conversation with turns still in L3 is folded whole. Its seeds name the run's latest turn.
    const int latest = _store.LatestHostedTurn();
    for (const core::Conversation& conversation : _store.Conversations()) {
        if (!conversation.ended) {
            continue;
        }
        const std::vector<core::HostedTurn> played = _store.HostedTurns(conversation.id);
        if (played.empty() || played.back().number <= conversation.foldedThrough) {
            continue;
        }
        // The prompt the conversation's next turn would start with: its last observation and a placeholder task stand
        // in for the tail, which no fold reads.
        context::HostedInputs tail;
        tail.observation = played.back().observation;
        tail.instruction = std::string(MAINTENANCE_INSTRUCTION);
        auto assemble = [&](const std::vector<RetrievalHit>&) { return Pass(conversation.id, {}, tail); };
        // Each fold's summary is stored and its turns indexed as it lands (Upkeep's StoreFold).
        if (!Upkeep(conversation.id, latest).FoldAll(assemble, stop)) {
            return false;
        }
    }
    return true;
}
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"`
Expected: builds with no warnings; `All tests passed`.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 485` (482 + 3), the three `[server]` tests "Not Run (Skipped)".

- [ ] **Step 5: Byte check and commit**

```bash
FILES="src/turn/HostedSession.h src/turn/HostedSession.cpp tests/hosted/HostedFoldingTests.cpp"
LC_ALL=C grep -c $'[\x80-\xFF]' $FILES
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES
git add $FILES
git commit -m "HostedSession::Maintain folds ended conversations through the shared upkeep; folding tests for hosted runs" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

### Task 9: Conversation fixtures, the scripted-conversation tests and the model-backed conversation

Fixture files written from section 5 of the requirements drive whole conversations with the fake backend: a scripted player conversation (events between and with turns, an action with parameters, a refused action, a turn without player text), a radiant conversation of two NPCs over six turns, and the requests that must fail with their `DataError`. The host's recorded fixtures can replace them later in the same format. One `[llama]` test plays a short hosted conversation on the real model.

**Files:**
- Create: `tests/hosted/fixtures/conversation.json`, `tests/hosted/fixtures/radiant.json`, `tests/hosted/fixtures/invalid_requests.json`
- Create: `tests/hosted/HostedFixtureTests.cpp`
- Create: `tests/llama/LlamaHostedTests.cpp`

**Interfaces:**
- Consumes: everything of Tasks 4-8 through `turn::HostedSession` and `tests/hosted/HostedTestSupport.h`; `test::SharedBackend()` (`tests/llama/LlamaTestModels.h`).
- Produces: the fixture format. A conversation fixture is `{"participants": [ids], "steps": [...]}`; a step is either `{"events": [event objects]}` (recorded between turns) or `{"turn": {...}, "model": {"resolve": text, "line": text}, "expect": {...}}`. A `turn` holds `speaker` and optionally `player_text`, `events`, `allowed_actions` and `observation` (default: `observation.json`). `expect` holds `requested`, `line`, `listeners`, and optionally `action` (`{"id", "params"}`, the turn result of section 5.3), `reasons`, `l3` (the contents of the turn's L3 messages, in order) and `new_events` (the texts the tail lists). The invalid-request fixture is `{"participants": [ids], "requests": [{"name", "conversation": "open" | "ended" | an id, "turn": {...}, "error": substring}]}`.

- [ ] **Step 1: Write the fixtures**

Create `tests/hosted/fixtures/conversation.json`:

```json
{
  "description": "Aela asks Hulda to follow; an action result arrives between turns and an ambient event with a turn; Hulda offers an attack with a target, a wait with an extra key is refused, and Hulda speaks on without player text. Written from section 5 of the integration requirements.",
  "participants": ["0001A6A4", "00000007"],
  "steps": [
    {
      "turn": {"speaker": "0001A6A4", "player_text": "Hulda, I need you to follow me."},
      "model": {"resolve": "{\"action\":\"follow\",\"params\":{}}", "line": "Lead the way, then."},
      "expect": {"requested": true, "action": {"id": "follow", "params": {}}, "line": "Lead the way, then.",
                 "listeners": ["00000007"], "l3": [], "new_events": []}
    },
    {
      "events": [{"type": "action_result", "action": "follow", "actor": "0001A6A4", "succeeded": true,
                  "text": "Hulda is now following Aela."}]
    },
    {
      "turn": {"speaker": "0001A6A4", "player_text": "Saadia keeps staring at us.",
               "events": [{"type": "ambient", "text": "Saadia glances at the door."}]},
      "model": {"resolve": "{\"action\":\"attack\",\"params\":{\"target\":\"0001B07C\"}}",
                "line": "Say the word and I will deal with her."},
      "expect": {"requested": true, "action": {"id": "attack", "params": {"target": "0001B07C"}},
                 "line": "Say the word and I will deal with her.", "listeners": ["00000007"],
                 "l3": ["Aela: Hulda, I need you to follow me.", "Hulda [follow]: Lead the way, then."],
                 "new_events": ["Hulda is now following Aela.", "Saadia glances at the door."]}
    },
    {
      "turn": {"speaker": "0001A6A4", "player_text": "No. Wait here for me.",
               "allowed_actions": ["none", "wait", "end_conversation"]},
      "model": {"resolve": "{\"action\":\"wait\",\"params\":{},\"intent\":\"rest\"}", "line": "As you wish."},
      "expect": {"requested": false,
                 "reasons": ["/intent: unexpected key 'intent'; the keys are 'action', 'params'"],
                 "line": "As you wish.", "listeners": ["00000007"],
                 "l3": ["Aela: Hulda, I need you to follow me.", "Hulda [follow]: Lead the way, then.",
                        "(Hulda is now following Aela.)\n(Saadia glances at the door.)\nAela: Saadia keeps staring at us.",
                        "Hulda [attack {\"target\":\"0001B07C\"}]: Say the word and I will deal with her."],
                 "new_events": []}
    },
    {
      "turn": {"speaker": "0001A6A4"},
      "model": {"resolve": "{\"action\":\"end_conversation\",\"params\":{}}", "line": "I will be here when you return."},
      "expect": {"requested": true, "action": {"id": "end_conversation", "params": {}},
                 "line": "I will be here when you return.", "listeners": ["00000007"],
                 "l3": ["Aela: Hulda, I need you to follow me.", "Hulda [follow]: Lead the way, then.",
                        "(Hulda is now following Aela.)\n(Saadia glances at the door.)\nAela: Saadia keeps staring at us.",
                        "Hulda [attack {\"target\":\"0001B07C\"}]: Say the word and I will deal with her.",
                        "Aela: No. Wait here for me.", "Hulda: As you wish."],
                 "new_events": []}
    }
  ]
}
```

Create `tests/hosted/fixtures/radiant.json`:

```json
{
  "description": "Hulda and Saadia talk for six turns with no player input (requirement R8's acceptance), alternating, each with a valid action.",
  "participants": ["0001A6A4", "0001B07C"],
  "steps": [
    {
      "turn": {"speaker": "0001B07C"},
      "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Another quiet night."},
      "expect": {"requested": true, "action": {"id": "none", "params": {}}, "line": "Another quiet night.",
                 "listeners": ["0001A6A4"], "l3": []}
    },
    {
      "turn": {"speaker": "0001A6A4"},
      "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Quiet suits me fine."},
      "expect": {"requested": true, "line": "Quiet suits me fine.", "listeners": ["0001B07C"]}
    },
    {
      "turn": {"speaker": "0001B07C"},
      "model": {"resolve": "{\"action\":\"give_item\",\"params\":{\"item\":\"Sweetroll\"}}", "line": "Here, have a sweetroll."},
      "expect": {"requested": true, "action": {"id": "give_item", "params": {"item": "Sweetroll"}},
                 "line": "Here, have a sweetroll.", "listeners": ["0001A6A4"]}
    },
    {
      "turn": {"speaker": "0001A6A4"},
      "model": {"resolve": "{\"action\":\"follow\",\"params\":{}}", "line": "Show me what you found."},
      "expect": {"requested": true, "line": "Show me what you found.", "listeners": ["0001B07C"]}
    },
    {
      "turn": {"speaker": "0001B07C"},
      "model": {"resolve": "{\"action\":\"wait\",\"params\":{}}", "line": "Wait, someone is coming."},
      "expect": {"requested": true, "line": "Wait, someone is coming.", "listeners": ["0001A6A4"]}
    },
    {
      "turn": {"speaker": "0001A6A4"},
      "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Then we wait together."},
      "expect": {"requested": true, "line": "Then we wait together.", "listeners": ["0001B07C"],
                 "l3": ["(Saadia speaks.)", "Saadia [none]: Another quiet night.",
                        "(Hulda speaks.)", "Hulda [none]: Quiet suits me fine.",
                        "(Saadia speaks.)", "Saadia [give_item {\"item\":\"Sweetroll\"}]: Here, have a sweetroll.",
                        "(Hulda speaks.)", "Hulda [follow]: Show me what you found.",
                        "(Saadia speaks.)", "Saadia [wait]: Wait, someone is coming."],
                 "new_events": []}
    }
  ]
}
```

Create `tests/hosted/fixtures/invalid_requests.json`:

```json
{
  "description": "Requests a host may send that Morpheus must refuse with a DataError naming the field, before any model call.",
  "participants": ["0001A6A4", "00000007"],
  "requests": [
    {"name": "unknown conversation", "conversation": "c9",
     "turn": {"speaker": "0001A6A4", "player_text": "Hello."},
     "error": "conversation_id: there is no conversation 'c9'"},
    {"name": "ended conversation", "conversation": "ended",
     "turn": {"speaker": "0001A6A4", "player_text": "Hello."},
     "error": "conversation_id: conversation 'c2' has ended"},
    {"name": "speaker who takes no part", "conversation": "open",
     "turn": {"speaker": "0001B07C", "player_text": "Hello."},
     "error": "speaker: '0001B07C' is not a participant of conversation 'c1'"},
    {"name": "the player as speaker", "conversation": "open",
     "turn": {"speaker": "00000007", "player_text": "Hello."},
     "error": "speaker: '00000007' is the player"},
    {"name": "speaker not present", "conversation": "open",
     "turn": {"speaker": "0001A6A4", "player_text": "Hello.",
              "observation": {"game_time": {"days_passed": 1, "hour": 9}, "location": "Whiterun",
                              "actors": [{"id": "00000007", "name": "Aela", "is_player": true}]}},
     "error": "speaker: '0001A6A4' is not an actor of the observation"},
    {"name": "unknown allowed action", "conversation": "open",
     "turn": {"speaker": "0001A6A4", "player_text": "Hello.", "allowed_actions": ["follow", "fly"]},
     "error": "allowed_actions/1: 'fly' is not an action of the catalog"},
    {"name": "event without text", "conversation": "open",
     "turn": {"speaker": "0001A6A4", "events": [{"type": "ambient"}]},
     "error": "events/0/text: an event needs 'text', a string"},
    {"name": "actor without an id", "conversation": "open",
     "turn": {"speaker": "0001A6A4",
              "observation": {"game_time": {"days_passed": 1, "hour": 9}, "location": "Whiterun",
                              "actors": [{"id": "", "name": "Hulda", "is_player": false},
                                         {"id": "00000007", "name": "Aela", "is_player": true}]}},
     "error": "observation at /actors/0/id"},
    {"name": "observation the host's schema refuses", "conversation": "open",
     "turn": {"speaker": "0001A6A4",
              "observation": {"game_time": {"days_passed": 1, "hour": 25}, "location": "Whiterun",
                              "actors": [{"id": "0001A6A4", "name": "Hulda", "is_player": false},
                                         {"id": "00000007", "name": "Aela", "is_player": true}]}},
     "error": "observation at /game_time/hour: expected an integer from 0 to 23, got 25"}
  ]
}
```

- [ ] **Step 2: Write the fixture tests and the model-backed test**

Create `tests/hosted/HostedFixtureTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;

namespace {

// The request a fixture's "turn" describes, in conversation `conversationId`. Without an "observation" it carries
// observation.json.
turn::HostedTurnRequest RequestFrom(const nlohmann::json& turnJson, const std::string& conversationId)
{
    turn::HostedTurnRequest request;
    request.conversationId = conversationId;
    request.speakerId = turnJson.at("speaker").get<std::string>();
    if (turnJson.contains("player_text")) {
        request.playerText = turnJson.at("player_text").get<std::string>();
    }
    if (turnJson.contains("events")) {
        request.events = turnJson.at("events").get<std::vector<nlohmann::json>>();
    }
    request.observation = turnJson.contains("observation") ? turnJson.at("observation") : test::FixtureObservation();
    if (turnJson.contains("allowed_actions")) {
        request.allowedActions = turnJson.at("allowed_actions").get<std::vector<std::string>>();
    }
    return request;
}

// Plays the conversation fixture `name` with the fake backend replying as its steps say, and checks each turn
// against its "expect": the record, the L3 its prompt showed and the new events its tail listed.
void PlayFixture(const std::string& name)
{
    const nlohmann::json fixture = test::ReadFixture(name);
    const nlohmann::json& steps = fixture.at("steps");
    test::HostedScript script;
    for (const nlohmann::json& step : steps) {
        if (step.contains("model")) {
            script.actions.push_back(step.at("model").at("resolve").get<std::string>());
            script.lines.push_back(step.at("model").at("line").get<std::string>());
        }
    }
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fixture_" + name),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation(fixture.at("participants").get<std::vector<std::string>>(),
                                                     test::FixtureObservation());

    int played = 0;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const nlohmann::json& step = steps[i];
        INFO(name << " step " << i);
        if (step.contains("events")) {
            session.RecordEvents(id, step.at("events").get<std::vector<nlohmann::json>>());
            continue;
        }
        const std::optional<core::HostedTurn> record = session.RunTurn(RequestFrom(step.at("turn"), id), {}, {});
        REQUIRE(record);
        ++played;

        // The record.
        const nlohmann::json& expect = step.at("expect");
        CHECK(record->number == played);
        CHECK(record->adjudication.requested == expect.at("requested").get<bool>());
        if (expect.contains("action")) {
            CHECK(record->adjudication.actionId == expect.at("action").at("id").get<std::string>());
            CHECK(record->adjudication.params == expect.at("action").at("params"));
        }
        if (expect.contains("reasons")) {
            CHECK(record->adjudication.reasons == expect.at("reasons").get<std::vector<std::string>>());
        }
        CHECK(record->line == expect.at("line").get<std::string>());
        CHECK(record->listenerIds == expect.at("listeners").get<std::vector<std::string>>());

        // The turn's prompt: its L3 messages, and the new events in its tail.
        const inference::Prompt& prompt = session.LastAssembly().prompt;
        if (expect.contains("l3")) {
            std::vector<std::string> l3;
            for (std::size_t k = 1; k < prompt.stableCount; ++k) {
                l3.push_back(prompt.messages[k].content);
            }
            CHECK(l3 == expect.at("l3").get<std::vector<std::string>>());
        }
        if (expect.contains("new_events")) {
            const std::vector<std::string> events = expect.at("new_events").get<std::vector<std::string>>();
            const std::string& tail = prompt.messages.back().content;
            if (events.empty()) {
                CHECK_THAT(tail, !ContainsSubstring("# New events"));
            } else {
                std::string block = "# New events\n";
                for (const std::string& event : events) {
                    block += "(" + event + ")\n";
                }
                CHECK_THAT(tail, ContainsSubstring(block));
            }
        }
    }
    CHECK(session.Turns(id).size() == static_cast<std::size_t>(played));
}

} // namespace

TEST_CASE("The scripted conversation fixture plays with the records and the L3 it expects", "[hosted][fixtures]")
{
    PlayFixture("conversation.json");
}

TEST_CASE("The radiant fixture: two NPCs talk for six turns with no player text", "[hosted][fixtures]")
{
    PlayFixture("radiant.json");
}

TEST_CASE("Each invalid request fixture is a DataError naming its field, before any model call", "[hosted][fixtures]")
{
    const nlohmann::json fixture = test::ReadFixture("invalid_requests.json");
    test::HostedScript script; // No reply: a model call would fail the test.
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fixture_invalid"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::vector<std::string> participants = fixture.at("participants").get<std::vector<std::string>>();
    const std::string open = session.StartConversation(participants, test::FixtureObservation());
    const std::string ended = session.StartConversation(participants, test::FixtureObservation());
    session.EndConversation(ended);

    for (const nlohmann::json& entry : fixture.at("requests")) {
        INFO(entry.at("name").get<std::string>());
        const std::string named = entry.at("conversation").get<std::string>();
        const std::string conversationId = named == "open" ? open : named == "ended" ? ended : named;
        CHECK_THROWS_MATCHES(session.RunTurn(RequestFrom(entry.at("turn"), conversationId), {}, {}), core::DataError,
                             MessageMatches(ContainsSubstring(entry.at("error").get<std::string>())));
    }
    CHECK(backend.Calls().empty());
    CHECK(session.Turns(open).empty());
}
```

Create `tests/llama/LlamaHostedTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/HostedRecord.h"
#include "core/Settings.h"
#include "hosted/HostedTestSupport.h"
#include "llama/LlamaTestModels.h"
#include "turn/HostedSession.h"

using namespace morpheus;

TEST_CASE("A hosted conversation on the real model: every action validates and every line is spoken", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    // Budgets for the shared backend's context of 4096, greedy sampling, no thinking.
    core::Settings settings = test::HostedSettings();
    settings.contextSize = backend.ContextSize();
    settings.budget = {96, 160, 128, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.0, 40, 0.95};
    settings.narrateSampling = {0.0, 40, 0.95};
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("llama_hosted"),
                                                            test::FixtureHostData(), settings, 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());

    const std::vector<std::string> playerTexts = {"Hulda, I need a room for the night.",
                                                  "Saadia keeps staring at us. Should I worry?",
                                                  "Then follow me, we are leaving."};
    for (const std::string& text : playerTexts) {
        INFO("player: " << text);
        std::string streamed;
        turn::HostedCallbacks callbacks;
        callbacks.onToken = [&streamed](std::string_view piece) { streamed += piece; };
        const std::optional<core::HostedTurn> record = session.RunTurn(test::Request(id, test::HULDA, text), callbacks, {});
        REQUIRE(record);
        INFO("resolve: " << record->resolveOutput << "; line: " << record->line);
        CHECK(record->adjudication.requested); // The grammar keeps the proposal valid.
        CHECK(record->adjudication.reasons.empty());
        CHECK_FALSE(record->line.empty());
        CHECK_FALSE(streamed.empty());
    }
    CHECK(session.Turns(id).size() == playerTexts.size());
}
```

- [ ] **Step 3: Run the fixture tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[fixtures]"`
Expected: builds with no warnings; `All tests passed` (3 test cases). These tests need no new engine code: they pin Tasks 4-8 against the fixtures. A failure here is a mismatch between a fixture and the engine's rendering; fix the engine, not the fixture, unless the fixture contradicts the spec.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 488` (485 + 3), the three `[server]` tests "Not Run (Skipped)". (`tests/llama` is not built in this preset.)

- [ ] **Step 4: Build the dev targets and run the model-backed test**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings.

Run, with the two model variables exported (Global Constraints): `./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "A hosted conversation on the real model: every action validates and every line is spoken"`
Expected: `All tests passed` (1 test case). If the variables are unset the test is skipped: export them and run again.

- [ ] **Step 5: Byte check and commit**

```bash
FILES="tests/hosted/fixtures/conversation.json tests/hosted/fixtures/radiant.json tests/hosted/fixtures/invalid_requests.json tests/hosted/HostedFixtureTests.cpp tests/llama/LlamaHostedTests.cpp"
LC_ALL=C grep -c $'[\x80-\xFF]' $FILES
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES
git add $FILES
git commit -m "Hosted conversation fixtures from the host's data contracts, the scripted and radiant conversation tests, and a model-backed hosted conversation" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

### Task 10: Documentation and full verification

The architecture spec gains a section on hosted runs and its tables follow store version 3; `CLAUDE.md`'s layout lines mention hosted runs and the fixtures. Then every preset is built and tested once more.

**Files:**
- Modify: `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: the names of Tasks 1-9.
- Produces: documentation only.

- [ ] **Step 1: Update the architecture spec**

In `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, make these replacements. Each "Replace" text appears once in the file; "with" is the new text, exactly.

1. Replace

   ```text
   | `store` | Per-run SQLite: turns, state snapshots, summaries, notes, embeddings | core |
   ```

   with

   ```text
   | `store` | Per-run SQLite: turns, state snapshots, summaries, notes, embeddings; for hosted runs the host data, conversations, hosted turns and the events between them | core |
   ```

2. Replace

   ```text
   | `context` | Layered prompt assembly, exact token budgets, cache-prefix tracking | core, inference |
   ```

   with

   ```text
   | `context` | Layered prompt assembly (the owned resolve prompt and the hosted prompt, on rendered transcript entries), exact token budgets, cache-prefix tracking | core, inference |
   ```

3. Replace

   ```text
   | `turn` | Turn orchestration (grammar, resolve and apply, retrieval, folding, session), the worker thread, replay recording | core, store, inference, context, grammar |
   ```

   with

   ```text
   | `turn` | Turn orchestration (grammar, resolve and apply, retrieval, folding and the shared memory upkeep, session), hosted runs (`HostedWorld`, `HostedSession`), the worker thread, replay recording | core, store, inference, context, grammar |
   ```

4. Replace

   ```text
   `Validate` checks a value against the same subset and returns its mistakes as data, a JSON pointer into the value and a message each; it compares integers by their written form, so a huge unsigned never equals a negative.
   ```

   with

   ```text
   `Validate` checks a value against the same subset, extended for schemas that are only validated (numbers with optional bounds, booleans, optional keys, open objects with `additionalProperties: true`, strings and arrays without an upper limit, and none of llama.cpp's limits; `FromSchema` still refuses each, and `CheckValidationSchema` checks such a schema without a value), and returns its mistakes as data, a JSON pointer into the value and a message each; it compares integers by their written form, so a huge unsigned never equals a negative.
   ```

5. Replace

   ```text
   | `run_meta` | world id, world content hash, model id, RNG seed, settings |
   ```

   with

   ```text
   | `run_meta` | world id, world content hash, model id, RNG seed, settings, kind (`owned` or `hosted`) |
   ```

6. Replace

   ```text
   | `embeddings` | kind, ref id, embedding model, content hash of the embedded text, vector |
   ```

   with

   ```text
   | `embeddings` | kind, ref id, embedding model, content hash of the embedded text, vector |
   | `host_data` | hosted runs: the host's action catalog, observation schema and lore as one JSON document (its hash is `run_meta`'s world hash) |
   | `conversations` | hosted runs: id (`c1`, `c2`, ...), participants with their names and bios in join order, ended, folded through |
   | `hosted_turns` | hosted runs: n (run-wide), conversation, speaker, listeners, player text, the request's events, observation, resolve output as written, adjudication, line, timings |
   | `conversation_events` | hosted runs: events recorded between turns, in order, with the conversation's turn they follow |
   ```

7. Replace

   ```text
   The schema version is 2 (the `digest` column; it also covers `attempt`). A run written with another version is refused, without migration.
   ```

   with

   ```text
   The schema version is 3 (`run_meta.kind` and the hosted tables; version 2 added the `digest` column and covers `attempt`). Every file has every table. A version 2 run is upgraded in place on open: the hosted tables are added and it is marked owned, in one transaction, its data untouched. A run written with any other version is refused.
   ```

8. Replace

   ```text
   ## UI (ImGui docking windows)
   ```

   with

   ```text
   ## Hosted runs

   Morpheus can run a world it does not own (design: `docs/superpowers/specs/2026-11-16-hosted-world-design.md`). The host sends once per run an action catalog, the JSON Schema of its observation and optional lore (`turn::HostData`), and with each request the observation, events and who speaks. Morpheus never simulates the world: an accepted action is requested, not applied, and its outcome comes back later as an event.

   - **`turn::HostedWorld`** checks the host data (a `DataError` naming the JSON pointer of the first fault) and holds R5's three operations. `Observe` validates an observation: first a fixed core the engine reads whatever the schema says (`actors`, each with a non-empty `id` and `name`, `is_player` and an optional `bio`; ids unique; exactly one player), then `grammar::Validate` against the host's schema, which must be in the extended subset; every fault is reported. `ProposalSchema` / `ProposalGrammar` build the speaker's possible actions for one turn: a `oneOf` over the allowed actions in catalog order, discriminated by `"action"`, each `{"action": <id>, "params": {...}}` with every parameter required and no extra keys; an actor parameter is an enum of the ids its scope gives (`present`, `present_non_player`, `participant`), never the speaker, and an action with no one to name is left out. `Adjudicate` parses the model's text (a parse failure is a refusal) and validates it: a valid action is requested, anything else refused with its reasons.
   - **`turn::HostedSession`** is one open hosted run in the same `RunStore` (kind `hosted`, world id `hosted`). `Open` creates the run or reopens it; changed host data (a mod update) replaces the stored copy and re-embeds changed lore; an owned file is refused. Conversations start with participants (their names and bios from the observation, in join order), take events between turns, gain and lose participants, and end. `RunTurn` checks the request (no model call before it passes), retrieves from lore and the run's folded turns, resolves under the proposal grammar, reports the adjudication through `onAction`, speaks on a prompt that extends the resolve prompt (streamed through `onToken`, no grammar), and commits the hosted turn record; a cancel commits nothing. There is no review, no dice, no state and no snapshot.
   - **The hosted prompt** (`context::AssembleHostedPrompt`), most stable first: L0 the hosted system text (`EngineText`, generic, no variable), L1 the host's always-on lore, the participants (`- <name> (<id>): <bio>`, in join order; a join or a leave changes the prompt from here on), L2 the run-wide summaries, L3 the conversation's unfolded turns as user/assistant pairs (the events in parentheses and the player's line, then `<speaker> [<action>]: <line>`; a refused action has no bracket), then the tail: retrieved snippets, the observation as compact JSON, the new events, the player's line and the resolve instruction. Budgets and flags are the owned prompt's; `world_cap` bounds the lore and the participants together. The prompt of a conversation's next turn starts with this turn's stable part, byte for byte.
   - **The shared memory upkeep** (`turn::MemoryUpkeep`) is `Session`'s fit loop, fold and condense, working on summaries and rendered transcript entries (`context::TranscriptEntry`: a turn number, its L3 messages, its fold messages and a one-line fact), so `Session` passes its turns and `HostedSession` its conversation's. During a conversation, an L3 over its budget folds its oldest turns into a run-wide summary; after `EndConversation`, `HostedSession::Maintain` folds every remaining turn (in one call when it fits). Folded turns of every conversation are retrievable history, under their run-wide turn numbers.

   ## UI (ImGui docking windows)
   ```

9. Replace

   ```text
   - **`context` tests:** layer assembly, budgets and folding triggers, using a fake tokenizer.
   ```

   with

   ```text
   - **`context` tests:** layer assembly, budgets and folding triggers, using a fake tokenizer; the hosted layers, their budgets and their stable prefix across turns and joins.
   - **Hosted tests:** `HostedWorld` (the proposal schema and its grammar, actor scopes, allowed actions, each fault of the host data and the observation core, `Adjudicate`), `HostedSession` (open, reopen and host data updates, conversations, turns, cancel, each request error, folding during a conversation and after its end) and the store's version 3 and upgrade, all with the fake backend. Fixtures in `tests/hosted/fixtures/` are written from the integration requirements' data contracts: the host data, an observation, a scripted conversation, a radiant conversation of two NPCs and the requests that must fail; the host's recorded fixtures can replace them. One `[llama]` test plays a hosted conversation on the real model.
   ```

- [ ] **Step 2: Update CLAUDE.md**

In `CLAUDE.md`:

1. Replace

   ```text
   - src/turn: turn orchestration: grammar, resolve and apply, retrieval, folding, session, worker, replay.
   ```

   with

   ```text
   - src/turn: turn orchestration: grammar, resolve and apply, retrieval, folding and the shared memory upkeep (`MemoryUpkeep`), session, worker, replay; hosted runs, a world the host owns and sends as data (`HostedWorld`, `HostedSession`).
   ```

2. Replace

   ```text
   - tests/: Catch2 tests (every tests/**/*.cpp is compiled in), tests/data: test worlds.
   ```

   with

   ```text
   - tests/: Catch2 tests (every tests/**/*.cpp is compiled in), tests/data: test worlds, tests/hosted/fixtures: hosted-run fixtures written from the host's data contracts.
   ```

- [ ] **Step 3: Check the documentation's bytes and names**

```bash
LC_ALL=C grep -c $'[\x80-\xFF]' docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md CLAUDE.md
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md CLAUDE.md
grep -n "schema version is 2\|without migration" docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md
```

Expected: every count printed is `0`; the last `grep` prints nothing.

- [ ] **Step 4: Run the whole fast suite and AddressSanitizer**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 488` (427 + 61), the three `[server]` tests "Not Run (Skipped)".

Run: `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
Expected: `0 tests failed out of 488`, the same three skipped, no AddressSanitizer report.

- [ ] **Step 5: Build every dev target and run the model-backed tests**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings.

Run, with the two model variables exported (Global Constraints): `./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "[llama]"`
Expected: `All tests passed`; the thinking tests and the `[server][llama]` test skipped as their variables say.

Run: `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 3`
Expected: exits with 0.

- [ ] **Step 6: Run the consumer check**

Run: `cmake -S cmake/consumer-check -B build/consumer-check && cmake --build build/consumer-check --config Debug && ./build/consumer-check/Debug/consumer_check.exe`
Expected: configures (no new library, no unwanted target), builds, and prints `consumer_check: ok (rolled ..., total ...)`.

- [ ] **Step 7: Commit**

```bash
git add docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md CLAUDE.md
git commit -m "Document hosted runs: HostedWorld, HostedSession, the shared memory upkeep, the hosted layers, store version 3" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git status --short
```

Expected: `git status --short` lists nothing of this plan (an untracked `.superpowers/` or the ignored `settings.json` are not staged and stay so).

---

## Self-review: spec coverage

| Spec requirement | Task |
|---|---|
| `HostedWorld` constructed from the host data; checks catalog, observation schema, lore; `DataError` naming the JSON pointer of the first fault | 4 |
| Observation schema within the extended `Validate` subset, checked at open (an unsupported keyword fails at open) | 1 (`CheckValidationSchema`), 4 |
| `Observe`: the core (`actors` with `id`, `name`, `is_player`, optional `bio`; unique ids; exactly one player), then `Validate`; every error collected; `DataError` naming each failing pointer | 4 |
| `Observation` holds the original JSON and the parsed actors in order | 4 |
| `ProposalGrammar(Observation, Participants, allowed, speakerId)` = `FromSchema` of the proposal schema | 4 |
| `Adjudicate`: parse (failure is a refusal), `Validate` against the turn's proposal schema, `{requested, actionId, params, reasons}` | 4 |
| `HostedSession::Open`: create; same host data reopens as is; changed host data replaces and re-embeds changed lore; owned file is a `DataError` | 6 |
| `StartConversation`: new id; participants are actors of the observation; bios stored in order | 6 |
| `RecordEvents` adds events to an open conversation without a turn | 6 |
| `SetParticipants`: joins at the end of the join order with bios; removes those who left | 6 |
| `RunTurn` returns the committed turn or nullopt on cancel | 7 |
| `EndConversation` marks it ended; the next `Maintain` folds the rest | 6, 8 |
| `Maintain`: folds ended conversations, condenses over-cap summaries, indexes folded turns; false when cancelled, can run again | 8 |
| Readers `Conversation(id)`, `Turns(conversationId)`, `Summaries()`, `LastAssembly()` | 6 |
| `HostData`, `HostedTurnRequest` (`conversationId`, `speakerId`, optional `playerText`, `events`, `observation`, optional `allowedActions`), `HostedCallbacks` (`onAction` after resolve before the line, `onToken`) | 4, 7 |
| Shared memory upkeep: fit loop, fold and condense move into one piece working on summaries and transcript entries; `Session` only calls it; every `Session` test unchanged | 3 |
| `context`: the prompt builder takes the hosted layer set; rendered entries instead of `TurnRecord`s, tests updated and saying so | 3, 5 |
| `grammar::Validate` extended; `FromSchema` refuses each | 1 |
| `EngineText`: hosted system text, resolve instruction, speak instruction, with `{speaker}` and `{player}`; generic | 5 |
| `RunStore` schema version 3 | 2 |
| Turn step 0: conversation exists and open; observation passes; speaker participant, present, not the player; allowed ids in the catalog and one action left; `DataError` naming the field; player text too long is `TurnError`; request events added before the turn in order | 7 (and 4 for `ProposalSchema`'s checks) |
| Step 1: retrieval with player text, speaker's name and previous line; lore and folded turns of the whole run | 6 (`SyncEntries`), 7, 8 (later conversation) |
| Step 2: resolve under `ProposalGrammar` with a small cap; `Adjudicate`; refused logged with reasons, the turn goes on; `onAction` fires | 7 |
| Step 3: speak on the resolve prompt plus the resolve output and the speak instruction; no grammar; streamed with its own cap | 7 |
| Step 4: the record (turn number run-wide, conversation, speaker, listeners, player text, request events, observation, resolve output as written, adjudication, line, timings) | 2 (types and storage), 7 |
| No review, dice, state or snapshot; thinking, sampling, context and budgets from `core::Settings`; a cancel commits nothing | 7 |
| The prompt layers L0 / L1 / participants / L2 / L3 / tail, most stable first; L3 shows the player text under the player's name, the line with the action in brackets (none when refused), events in parentheses | 5 (layout), 6 (`Entries`), 7, 9 |
| Folding: over-budget L3 folds through the shared upkeep; `EndConversation` + `Maintain` folds the rest and indexes it | 7 (fit loop), 8 |
| Catalog: `id` non-empty and unique; `params` may be absent; the four parameter types; anything else a `DataError` naming the pointer | 4 |
| Proposal schema: `oneOf` discriminated by `"action"`; each branch with every parameter required and no extra keys; actor enum of the scope's ids, deduplicated, never the speaker; an action with an empty actor enum left out | 4 |
| Turn result `{id, params}` (section 5.3) | 2 (`Adjudication`), 9 (fixtures' `action`) |
| Extended subset: number with optional min/max, boolean, optional properties, `additionalProperties: true`, arrays without `maxItems`, strings without `maxLength`; errors keep `Validate`'s form | 1 |
| Events: objects with a required `text`; other fields stored as they are; joins and leaves through `SetParticipants` | 4 (`CheckEvents`), 6, 7 |
| Storage: meta `kind`; hosted host data with its content hash; world id `hosted`; `conversations`, `hosted_turns`, `conversation_events`; shared summaries and embeddings; run-wide turn numbers | 2, 6 |
| Upgrade: a version 2 file upgraded in place (new tables, `kind` owned, version 3, data untouched); any other version refused | 2 |
| The run's file is the caller's choice | 6 (`Open` takes the path) |
| Errors: host data `DataError` naming the pointer; player text that does not fit `TurnError`; model mistakes not exceptions; backend failures `InferenceError`; cancel nullopt; engine bugs `ContractError` | 4, 6, 7 |
| Tests, `HostedWorld`: section 3 catalog schema and grammar; each scope; empty target left out; `allowedActions`; each core fault with its pointer; observation schema at open with an unsupported keyword; `Adjudicate` accepts and refuses `"intent"` on `wait`, an unknown action, a target not present, unparsable text | 4 |
| Tests, `Validate`: numbers, booleans, optional properties, open objects, unbounded arrays and strings accepted; `FromSchema` refuses each | 1 |
| Tests, `HostedSession` from fixture files in `tests/hosted/fixtures/`: open, reopen same, reopen changed; scripted conversation with events between turns, records and L3; NPC-to-NPC; prompt byte-identical up to the tail; a join changing the prompt only from the participants layer; folding over budget; end + `Maintain` and retrieval in a later conversation; a cancel; each request `DataError` | 4 (fixtures), 6, 7, 8, 9 |
| Tests, store: version 3 hosted round trip; version 2 upgraded in place, opening as owned with its data unchanged | 2 |
| Tests, `Session`: every existing test passes unchanged | 3 (and every later task's full run) |
| Model-backed (`[llama]`): one hosted conversation of a few turns; every action validates; every line non-empty | 9 |
| Docs: architecture spec section on hosted runs (`HostedWorld`, `HostedSession`, shared upkeep, hosted layers, store version 3); `CLAUDE.md` layout line for `src/turn` | 10 |
| Out of scope (D2 server, E per-speaker memory, F prompt pack, `"decide"`, per-turn review, call tag) | not touched |
