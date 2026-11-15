# The Host's Prompt Pack (Sub-project F) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A hosted run's six model-facing texts (system, resolve, speak, turn cue, summarize, condense) can be replaced by the host per run as data sent at `open`, checked at open, with the engine's texts as defaults.

**Architecture:** A new `turn::PromptPack` holds the six texts (defaults from `EngineText`'s `HOSTED_*`), checks a host's texts (keys, emptiness, variables) and fills variables. `HostData` carries the host's texts by key; `HostedWorld` builds the run's pack with its other checks; `HostedSession` reads every hosted text from it and checks the system text against `engine_rules_cap` at open. `morpheus_server` passes the pack through `open` and answers the defaults on `GET /v1/prompts/default` (API 1.3.0).

**Tech Stack:** C++20, nlohmann::json, Catch2, CMake presets (`tests`, `asan`, `dev`).

Spec: `docs/superpowers/specs/2026-11-16-prompt-pack-design.md`.

## Global Constraints

- The pack's keys and variables, exactly: `system` (no variable), `resolve` `{speaker}` `{player}`, `speak` `{speaker}` `{player}`, `turn_cue` `{speaker}` `{player}`, `summarize` `{actor}`, `condense` `{actor}`.
- A variable is `{` followed by one or more of `a-z` and `_`, then `}`. Every other brace is literal text.
- Error texts, exactly: `prompts/<key>: unknown text; the pack's texts are system, resolve, speak, turn_cue, summarize, condense`; `prompts/<key>: empty`; `prompts/<key>: unknown variable {<name>}; this text takes {speaker}, {player}` (or `takes no variable`, or `takes {actor}`); `prompts/system: <n> tokens, over engine_rules_cap <cap>`; shape: `host data at /prompts: 'prompts' must be a JSON object of texts`, `host data at /prompts/<key>: a prompt text must be a string`.
- First fault reported: unknown keys first (in key order), then each text in the table's order (empty, then its variables in text order).
- `HostDataToJson` writes `prompts` only when the host sent at least one text, and only the texts it sent: a run without a pack keeps its pre-F hash.
- No fallback: a text that fails a check is never replaced by the default.
- API version `1.3.0`.
- Morpheus's own game is not touched: `"[turn]"`, `"[replay]"`, `"[context]"` stay green with no change except `tests/context/HostedPromptTests.cpp`'s default-text test (Task 4).
- CLAUDE.md coding rules: fail fast (`MORPHEUS_REQUIRE` for engine bugs, `core::DataError` for host data); method bodies commented to read top to bottom; a private helper only with more than one caller; class layout order nested types, public data, private data, public methods, private methods; constants ALL_CAPS; members `_camelCase`; initialize members at declaration; ASCII only in source and docs; tests written first.
- Commit trailer: `Co-Authored-By: Claude <model> <noreply@anthropic.com>` naming the implementing model. Never stage `settings.json`, `models/`, `imgui.ini` or `docs/references/`.

Commands (Git Bash, repo root): build `cmake --build --preset tests`; run one tag `./build/tests/bin/Debug/morpheus_tests.exe "[prompts]"`; whole suite `ctest --preset tests`. A new .cpp under `tests/` is compiled in automatically; a new .cpp under `src/turn/` must be added to `src/turn/CMakeLists.txt`. Re-run `cmake --preset tests` after adding a file.

---

### Task 1: `turn::PromptPack`

**Files:**
- Create: `src/turn/PromptPack.h`, `src/turn/PromptPack.cpp`
- Modify: `src/turn/CMakeLists.txt` (add `PromptPack.cpp` after `MemoryUpkeep.cpp`), `src/turn/EngineText.h` (comments only)
- Test: `tests/turn/PromptPackTests.cpp` (new)

**Interfaces:**
- Produces:
  - `struct turn::PromptVariables { std::string speaker; std::string player; std::string actor; }`
  - `struct turn::PromptText { std::string_view key; std::array<std::string_view, 2> variables; }`
  - `inline constexpr std::array<turn::PromptText, 6> turn::PROMPT_TEXTS`
  - `class turn::PromptPack` with public members `system`, `resolve`, `speak`, `turnCue`, `summarize`, `condense` (`std::string`, defaulted to `HOSTED_SYSTEM_TEXT`, `HOSTED_RESOLVE_INSTRUCTION`, `HOSTED_SPEAK_INSTRUCTION`, `HOSTED_TURN_CUE`, `HOSTED_SUMMARIZE_INSTRUCTION`, `HOSTED_CONDENSE_INSTRUCTION`), `static PromptPack FromHost(const std::map<std::string, std::string>& texts)`, `static std::string Fill(std::string_view text, const PromptVariables& variables)`, `const std::string& Text(std::string_view key) const`.

- [ ] **Step 1: Write the failing tests**

Create `tests/turn/PromptPackTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>

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
    CHECK(pack.system == turn::HOSTED_SYSTEM_TEXT);
    CHECK(pack.resolve == turn::HOSTED_RESOLVE_INSTRUCTION);
    CHECK(pack.speak == turn::HOSTED_SPEAK_INSTRUCTION);
    CHECK(pack.turnCue == turn::HOSTED_TURN_CUE);
    CHECK(pack.summarize == turn::HOSTED_SUMMARIZE_INSTRUCTION);
    CHECK(pack.condense == turn::HOSTED_CONDENSE_INSTRUCTION);

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
        INFO(text.key);
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
    CHECK(pack.system == turn::HOSTED_SYSTEM_TEXT);
    CHECK(pack.speak == turn::HOSTED_SPEAK_INSTRUCTION);
    CHECK(pack.turnCue == turn::HOSTED_TURN_CUE);
    CHECK(pack.summarize == turn::HOSTED_SUMMARIZE_INSTRUCTION);
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
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --preset tests && cmake --build --preset tests`
Expected: the build fails: `turn/PromptPack.h` not found.

- [ ] **Step 3: Write `src/turn/PromptPack.h`**

```cpp
#pragma once

#include <array>
#include <map>
#include <string>
#include <string_view>

#include "turn/EngineText.h"

namespace morpheus::turn {

// The names PromptPack::Fill puts in a hosted text. A variable a text names must have a non-empty value here.
struct PromptVariables {
    std::string speaker = {}; // {speaker}: the speaking NPC's name.
    std::string player = {};  // {player}: the player's character's name.
    std::string actor = {};   // {actor}: the NPC whose memory is folded or condensed.
};

// One text of the pack (prompt pack spec, section 1): its key in the host data and the variables it takes (at most
// two; an empty entry is unused).
struct PromptText {
    std::string_view key = {};
    std::array<std::string_view, 2> variables = {};
};

// The pack's texts in the spec's table order: the order FromHost checks them in and the defaults route lists them in.
inline constexpr std::array<PromptText, 6> PROMPT_TEXTS = {{
    {"system", {}},
    {"resolve", {"speaker", "player"}},
    {"speak", {"speaker", "player"}},
    {"turn_cue", {"speaker", "player"}},
    {"summarize", {"actor", ""}},
    {"condense", {"actor", ""}},
}};

// The texts a hosted run speaks to the model (prompt pack spec): the engine's defaults (EngineText's HOSTED_*), with the
// host's texts over them. The system text takes no variable, so L0 is the same for every turn and speaker of a run.
class PromptPack {
public:
    std::string system = std::string(HOSTED_SYSTEM_TEXT);
    std::string resolve = std::string(HOSTED_RESOLVE_INSTRUCTION);
    std::string speak = std::string(HOSTED_SPEAK_INSTRUCTION);
    std::string turnCue = std::string(HOSTED_TURN_CUE);
    std::string summarize = std::string(HOSTED_SUMMARIZE_INSTRUCTION);
    std::string condense = std::string(HOSTED_CONDENSE_INSTRUCTION);

    // The host's texts (HostData::prompts, by key) over the defaults. Throws DataError for the first fault: an unknown
    // key ("prompts/<key>: unknown text; ..."), checked in key order before any text; then, text by text in
    // PROMPT_TEXTS' order, an empty text ("prompts/<key>: empty") or a variable the text does not take
    // ("prompts/<key>: unknown variable {<name>}; this text takes ..."), in text order. A text that fails is never
    // replaced by its default.
    static PromptPack FromHost(const std::map<std::string, std::string>& texts);

    // `text` with every variable ({name}, name of a-z and _) replaced by its value in `variables`; other braces stay.
    // A variable that is not speaker, player or actor, or has an empty value, is a ContractError: FromHost refused the
    // first, and the engine always gives the names its texts take.
    static std::string Fill(std::string_view text, const PromptVariables& variables);

    // The text whose host-data key is `key` (one of PROMPT_TEXTS'); a ContractError for another key.
    const std::string& Text(std::string_view key) const;
};

} // namespace morpheus::turn
```

- [ ] **Step 4: Write `src/turn/PromptPack.cpp`**

```cpp
#include "turn/PromptPack.h"

#include <algorithm>
#include <cstddef>
#include <format>

#include "core/Errors.h"

namespace morpheus::turn {

namespace {

// The pack's members, in PROMPT_TEXTS' order: index i holds the text whose key is PROMPT_TEXTS[i].key.
constexpr std::array<std::string PromptPack::*, 6> MEMBERS = {&PromptPack::system,  &PromptPack::resolve,
                                                              &PromptPack::speak,   &PromptPack::turnCue,
                                                              &PromptPack::summarize, &PromptPack::condense};

// The length of the variable that starts at `at` in `text` ("{", one or more of a-z and _, "}"), or 0 when none does.
std::size_t VariableLength(std::string_view text, std::size_t at)
{
    if (text[at] != '{') {
        return 0;
    }
    std::size_t end = at + 1;
    while (end < text.size() && ((text[end] >= 'a' && text[end] <= 'z') || text[end] == '_')) {
        ++end;
    }
    // At least one name character, then the closing brace.
    return end > at + 1 && end < text.size() && text[end] == '}' ? end - at + 1 : 0;
}

} // namespace

PromptPack PromptPack::FromHost(const std::map<std::string, std::string>& texts)
{
    // 1. Every key the host sent names a text of the pack. The map gives the keys in order.
    for (const auto& [key, text] : texts) {
        const bool known = std::any_of(PROMPT_TEXTS.begin(), PROMPT_TEXTS.end(),
                                       [&key](const PromptText& entry) { return entry.key == key; });
        if (!known) {
            std::string keys;
            for (const PromptText& entry : PROMPT_TEXTS) {
                keys += std::format("{}{}", keys.empty() ? "" : ", ", entry.key);
            }
            throw core::DataError(std::format("prompts/{}: unknown text; the pack's texts are {}", key, keys));
        }
    }

    // 2. Each sent text, in the table's order: not empty, and naming only the variables it takes. It then replaces the
    // default.
    PromptPack pack;
    for (std::size_t i = 0; i < PROMPT_TEXTS.size(); ++i) {
        const PromptText& entry = PROMPT_TEXTS[i];
        const auto it = texts.find(std::string(entry.key));
        if (it == texts.end()) {
            continue;
        }
        const std::string& text = it->second;
        if (text.empty()) {
            throw core::DataError(std::format("prompts/{}: empty", entry.key));
        }
        for (std::size_t at = 0; at < text.size(); ++at) {
            const std::size_t length = VariableLength(text, at);
            if (length == 0) {
                continue;
            }
            const std::string_view name = std::string_view(text).substr(at + 1, length - 2);
            if (std::find(entry.variables.begin(), entry.variables.end(), name) == entry.variables.end()) {
                // What the text takes, as the message lists it.
                std::string takes;
                for (const std::string_view variable : entry.variables) {
                    if (!variable.empty()) {
                        takes += std::format("{}{{{}}}", takes.empty() ? "" : ", ", variable);
                    }
                }
                throw core::DataError(std::format("prompts/{}: unknown variable {{{}}}; this text takes {}", entry.key,
                                                  name, takes.empty() ? "no variable" : takes));
            }
            at += length - 1; // Past the variable; the loop's ++at steps over its closing brace.
        }
        pack.*MEMBERS[i] = text;
    }
    return pack;
}

std::string PromptPack::Fill(std::string_view text, const PromptVariables& variables)
{
    // Left to right over `text`: a variable becomes its value, anything else is copied. A value is never scanned, so a
    // name holding a variable is put in as written.
    std::string filled;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t length = VariableLength(text, at);
        if (length == 0) {
            filled += text[at];
            ++at;
            continue;
        }
        const std::string_view name = text.substr(at + 1, length - 2);
        const std::string* pValue = name == "speaker" ? &variables.speaker
                                    : name == "player" ? &variables.player
                                    : name == "actor"  ? &variables.actor
                                                       : nullptr;
        MORPHEUS_REQUIRE(pValue != nullptr, std::format("PromptPack::Fill: {{{}}} is not a variable the engine fills", name));
        MORPHEUS_REQUIRE(!pValue->empty(), std::format("PromptPack::Fill: no value for {{{}}}", name));
        filled += *pValue;
        at += length;
    }
    return filled;
}

const std::string& PromptPack::Text(std::string_view key) const
{
    // The member at the key's place in PROMPT_TEXTS.
    for (std::size_t i = 0; i < PROMPT_TEXTS.size(); ++i) {
        if (PROMPT_TEXTS[i].key == key) {
            return this->*MEMBERS[i];
        }
    }
    throw core::ContractError(std::format("PromptPack::Text: '{}' is not a text of the pack", key));
}

} // namespace morpheus::turn
```

Add `PromptPack.cpp` to `src/turn/CMakeLists.txt`'s source list, after `MemoryUpkeep.cpp`.

In `src/turn/EngineText.h`, update the hosted texts' comments: `HOSTED_SYSTEM_TEXT`'s "Sub-project F lets the host replace it." becomes "PromptPack's default `system`; a host's pack may replace it.", and add one line to each of the other five hosted constants' comments naming its pack key: "PromptPack's default `resolve`." / `speak` / `turn_cue` / `summarize` / `condense`. Text values unchanged.

- [ ] **Step 5: Run the tests**

Run: `cmake --preset tests && cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[prompts]"`
Expected: all `[prompts]` test cases pass.

- [ ] **Step 6: Commit**

```bash
git add src/turn/PromptPack.h src/turn/PromptPack.cpp src/turn/CMakeLists.txt src/turn/EngineText.h tests/turn/PromptPackTests.cpp
git commit -m "PromptPack: the hosted texts with a host's texts over them, checked, and the variable filler"
```

---

### Task 2: The pack in the host data and in `HostedWorld`

**Files:**
- Modify: `src/turn/HostedWorld.h`, `src/turn/HostedWorld.cpp` (`HostDataToJson`, `HostDataFromJson`, the constructor, a new `Prompts()`)
- Test: `tests/hosted/HostedWorldTests.cpp`

**Interfaces:**
- Consumes: `turn::PromptPack::FromHost` (Task 1).
- Produces: `turn::HostData::prompts` (`std::map<std::string, std::string>`, the host's texts by key); `const turn::PromptPack& turn::HostedWorld::Prompts() const`.

- [ ] **Step 1: Write the failing test**

Add `#include <map>` to `tests/hosted/HostedWorldTests.cpp`, and this test case after "Host data reads and writes its JSON form and hashes its content":

```cpp
TEST_CASE("Host data carries the host's prompt texts; without them it hashes as before packs", "[hosted][prompts]")
{
    // Without a pack the JSON form has no "prompts" and the hash is the one open.json had before packs existed.
    const turn::HostData plain = test::FixtureHostData();
    CHECK(plain.prompts.empty());
    CHECK_FALSE(turn::HostDataToJson(plain).contains("prompts"));
    CHECK(turn::HostDataHash(plain) == "35856ce76ac0af2e");
    // An empty pack is no pack.
    nlohmann::json emptyPack = test::ReadFixture("open.json");
    emptyPack["prompts"] = nlohmann::json::object();
    CHECK(turn::HostDataHash(turn::HostDataFromJson(emptyPack)) == turn::HostDataHash(plain));

    // A pack round-trips with only the texts sent, and changes the hash.
    nlohmann::json withPack = test::ReadFixture("open.json");
    withPack["prompts"] = {{"speak", "Talk, {speaker}."}, {"turn_cue", "({speaker} goes on.)"}};
    const turn::HostData data = turn::HostDataFromJson(withPack);
    CHECK(data.prompts ==
          std::map<std::string, std::string>{{"speak", "Talk, {speaker}."}, {"turn_cue", "({speaker} goes on.)"}});
    CHECK(turn::HostDataToJson(data) == withPack);
    CHECK(turn::HostDataHash(data) != turn::HostDataHash(plain));

    // A wrong shape names its pointer.
    nlohmann::json notObject = test::ReadFixture("open.json");
    notObject["prompts"] = "speak";
    CHECK_THROWS_WITH(turn::HostDataFromJson(notObject), "host data at /prompts: 'prompts' must be a JSON object of texts");
    nlohmann::json notString = test::ReadFixture("open.json");
    notString["prompts"] = {{"speak", 3}};
    CHECK_THROWS_WITH(turn::HostDataFromJson(notString), "host data at /prompts/speak: a prompt text must be a string");

    // HostedWorld checks the content and keeps the pack: the sent texts over the defaults.
    const turn::HostedWorld world(data);
    CHECK(world.Prompts().speak == "Talk, {speaker}.");
    CHECK(world.Prompts().turnCue == "({speaker} goes on.)");
    CHECK(world.Prompts().resolve == turn::HOSTED_RESOLVE_INSTRUCTION);
    CHECK(turn::HostedWorld(plain).Prompts().speak == turn::HOSTED_SPEAK_INSTRUCTION);
    turn::HostData bad = test::FixtureHostData();
    bad.prompts = {{"speak", "Talk to {location}."}};
    CHECK(OpenError(bad) == "prompts/speak: unknown variable {location}; this text takes {speaker}, {player}");
}
```

Before changing any code, run the existing suite's hash once to confirm the pinned value: the test does not compile yet (`prompts` is unknown), so in this step only, check the literal with a throwaway `CHECK(turn::HostDataHash(test::FixtureHostData()) == "35856ce76ac0af2e");` run on the unchanged code if you doubt it. If it fails on unchanged code, the plan's literal is wrong: use the value Catch prints, and say so in the report. Remove the throwaway check.

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build --preset tests`
Expected: compile error: `HostData` has no member `prompts`, `HostedWorld` has no member `Prompts`.

- [ ] **Step 3: Implement**

In `src/turn/HostedWorld.h`: add `#include <map>` and `#include "turn/PromptPack.h"`. In `HostData`, after `clock`:

```cpp
    // The host's prompt texts by pack key (prompt pack spec, section 1): any of system, resolve, speak, turn_cue,
    // summarize, condense, as sent. A key left out takes the engine's default (PromptPack). Empty: no pack.
    std::map<std::string, std::string> prompts = {};
```

Update the comments of `HostDataToJson` (`"prompts"` (an object of strings) is written only when the host data holds a text) and `HostDataFromJson` (`"prompts"` may be absent; when present it is an object whose values are strings; its keys and texts are HostedWorld's to check).

In `HostedWorld`'s private data, after `_hasClock`:

```cpp
    PromptPack _prompts = {}; // The host's texts over the engine's defaults.
```

Its constructor comment gains: `... or "host data at /clock: ..." (the clock must be a well-formed JSON pointer) or PromptPack::FromHost's "prompts/<key>: ..."`. After `HasClock()`'s declaration:

```cpp
    // The run's prompt pack: the host's texts over the engine's defaults, checked.
    const PromptPack& Prompts() const;
```

In `src/turn/HostedWorld.cpp`, `HostDataToJson`, after the clock:

```cpp
    // Only the texts the host sent, and no key at all without one: a run without a pack keeps the hash it had.
    if (!data.prompts.empty()) {
        json["prompts"] = data.prompts;
    }
```

`HostDataFromJson`: the unknown-key check also allows `"prompts"`; after the clock block, before `if (!json.contains("lore"))`:

```cpp
    // The host's prompt texts: an object of strings. Its keys and texts are PromptPack's to check (HostedWorld).
    if (json.contains("prompts")) {
        const nlohmann::json& prompts = json.at("prompts");
        if (!prompts.is_object()) {
            throw fault("/prompts", "'prompts' must be a JSON object of texts");
        }
        for (auto it = prompts.begin(); it != prompts.end(); ++it) {
            if (!it.value().is_string()) {
                throw fault("/prompts/" + grammar::PointerToken(it.key()), "a prompt text must be a string");
            }
            data.prompts[it.key()] = it.value().get<std::string>();
        }
    }
```

The constructor, after step 4 (the clock):

```cpp
    // 5. The prompt pack: the host's texts over the defaults, each checked (PromptPack::FromHost's DataError).
    _prompts = PromptPack::FromHost(data.prompts);
```

And after `HasClock()`'s definition:

```cpp
const PromptPack& HostedWorld::Prompts() const
{
    return _prompts;
}
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"` then `./build/tests/bin/Debug/morpheus_tests.exe "[prompts]"`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/turn/HostedWorld.h src/turn/HostedWorld.cpp tests/hosted/HostedWorldTests.cpp
git commit -m "Host data: the host's prompt texts, kept out of the JSON form and the hash when none are sent"
```

---

### Task 3: `HostedSession` speaks the run's pack

**Files:**
- Modify: `src/turn/HostedSession.h` (the `Open` comment), `src/turn/HostedSession.cpp`
- Test: `tests/hosted/HostedPromptPackTests.cpp` (new)

**Interfaces:**
- Consumes: `HostedWorld::Prompts()`, `PromptPack::Fill`, `PromptVariables` (Tasks 1-2).
- Produces: `HostedSession::Open` throws `DataError "prompts/system: <n> tokens, over engine_rules_cap <cap>"` before it touches the file.

- [ ] **Step 1: Write the failing tests**

Create `tests/hosted/HostedPromptPackTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "store/RunStore.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// A host's pack replacing all six texts; each marked "PACK" and naming its variables, so a test sees them filled.
std::map<std::string, std::string> CustomPack()
{
    return {{"system", "# Pack\nYou voice the people of a test scene. Act as step 1 asks, then speak as step 2 asks."},
            {"resolve", "PACK RESOLVE: what does {speaker} try while {player} waits?"},
            {"speak", "PACK SPEAK: {speaker} answers {player}."},
            {"turn_cue", "PACK CUE: {speaker} goes on."},
            {"summarize", "PACK SUMMARIZE: {actor} remembers."},
            {"condense", "PACK CONDENSE: {actor} shortens."}};
}

// open.json's host data with `pack`.
turn::HostData PackHostData(const std::map<std::string, std::string>& pack)
{
    turn::HostData data = test::FixtureHostData();
    data.prompts = pack;
    return data;
}

// A scripted model for CustomPack: a fold or a condense (its last message holds the pack's marker) answers "Summary.",
// a resolve call (it has a grammar) NONE, a speak call "Line <n>.".
test::ScriptedBackend::Responder PackResponder()
{
    return [spoken = 0](const inference::Prompt& prompt, const inference::GenerationSpec& spec) mutable -> std::string {
        const std::string& last = prompt.messages.back().content;
        if (last.starts_with("PACK SUMMARIZE") || last.find("PACK CONDENSE") != std::string::npos) {
            return "Summary.";
        }
        if (!spec.grammar.empty()) {
            return NONE;
        }
        return std::format("Line {}.", ++spoken);
    };
}

} // namespace

TEST_CASE("A host's pack replaces the system text, the instructions and the cue in every call", "[hosted][prompts]")
{
    test::ScriptedBackend backend(16384, PackResponder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_pack_texts"),
                                                            PackHostData(CustomPack()), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    // Hulda speaks first with no player text and no events (her entry shows the cue), then Aela speaks to her.
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, ""), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hello."), {}, {}));
    const std::vector<test::ScriptedBackend::Call>& calls = backend.Calls();
    REQUIRE(calls.size() == 4);

    // The second resolve prompt: the pack's system text first, the first turn under the pack's cue, the pack's
    // resolve instruction last, filled.
    const inference::Prompt& resolve = calls[2].prompt;
    CHECK(resolve.messages[0].content.starts_with(CustomPack().at("system")));
    CHECK_THAT(resolve.messages[0].content, !ContainsSubstring("You voice the characters of a role-playing game"));
    CHECK(resolve.messages[1].content == "PACK CUE: Hulda goes on.");
    CHECK(resolve.messages.back().content.ends_with("# Task\nPACK RESOLVE: what does Hulda try while Aela waits?"));
    // The speak call ends with the pack's speak text, filled.
    CHECK(calls[3].prompt.messages.back().content == "PACK SPEAK: Hulda answers Aela.");

    // The fold after the end: the pack's system text, and its summarize instruction filled.
    session.EndConversation(id);
    REQUIRE(session.Maintain({}));
    const inference::Prompt& fold = backend.Calls().back().prompt;
    CHECK(fold.messages[0].content.starts_with(CustomPack().at("system")));
    CHECK(fold.messages.back().content == "PACK SUMMARIZE: Hulda remembers.");
}

TEST_CASE("A host's pack gives the condense call its instruction", "[hosted][prompts]")
{
    test::ScriptedBackend backend(16384, PackResponder());
    core::Settings settings = test::HostedSettings();
    // A summary_cap of 11: Hulda's one memory told by Saadia ("# What Hulda remembers" and "- (told by Saadia)
    // Secret.", 13 tokens) is over it, a condensed "Summary." (9 tokens) is not.
    settings.budget.summaryCap = 11;
    const std::filesystem::path file = test::HostedRunFile("hosted_pack_condense");
    const turn::HostData data = PackHostData(CustomPack());
    std::string id;
    {
        turn::HostedSession session = turn::HostedSession::Open(file, data, settings, 42, backend);
        id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
        REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hello."), {}, {}));
    }
    // Saadia told Hulda a secret: a shared copy, written to the closed run's store as the share route would.
    {
        store::RunStore store = store::RunStore::Open(file);
        core::Summary told;
        told.fromTurn = 1;
        told.toTurn = 1;
        told.text = "Secret.";
        told.actorId = test::HULDA;
        told.conversationId = id;
        told.toldBy = test::SAADIA;
        store.AddActorSummary(told);
    }

    // Hulda's next turn condenses her memories under the pack's condense instruction.
    turn::HostedSession session = turn::HostedSession::Open(file, data, settings, 42, backend);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Again."), {}, {}));
    bool condensed = false;
    for (const test::ScriptedBackend::Call& call : backend.Calls()) {
        const std::string& last = call.prompt.messages.back().content;
        if (last.find("PACK CONDENSE") != std::string::npos) {
            condensed = true;
            CHECK(last.ends_with("- (told by Saadia) Secret.\n\nPACK CONDENSE: Hulda shortens."));
        }
    }
    CHECK(condensed);
}

TEST_CASE("Under a host's pack each turn's prompt keeps the previous prompt's stable part byte for byte",
          "[hosted][prompts]")
{
    test::ScriptedBackend backend(16384, PackResponder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_pack_prefix"),
                                                            PackHostData(CustomPack()), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    std::vector<inference::Prompt> prompts;
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}));
    prompts.push_back(session.LastAssembly().prompt);
    REQUIRE(session.RunTurn(test::Request(id, test::SAADIA, "How is business?"), {}, {}));
    prompts.push_back(session.LastAssembly().prompt);
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Goodbye."), {}, {}));
    prompts.push_back(session.LastAssembly().prompt);

    // R7's acceptance: up to the volatile tail, a prompt is the previous one's stable part plus the new entry.
    for (std::size_t t = 1; t < prompts.size(); ++t) {
        INFO("turn " << t + 1);
        const inference::Prompt& previous = prompts[t - 1];
        const inference::Prompt& next = prompts[t];
        CHECK(next.messages[0].content.starts_with(CustomPack().at("system")));
        REQUIRE(next.stableCount > previous.stableCount);
        for (std::size_t k = 0; k < previous.stableCount; ++k) {
            INFO("message " << k);
            CHECK(next.messages[k].role == previous.messages[k].role);
            CHECK(next.messages[k].content == previous.messages[k].content);
        }
    }
}

TEST_CASE("Open refuses a system text over engine_rules_cap before it touches the file", "[hosted][prompts]")
{
    test::ScriptedBackend backend(16384, PackResponder());
    core::Settings settings = test::HostedSettings();
    settings.budget.engineRulesCap = 32;
    // 500 characters: 125 tokens as the fake counts them.
    std::string longText;
    for (int i = 0; i < 100; ++i) {
        longText += "word ";
    }
    const std::filesystem::path file = test::HostedRunFile("hosted_pack_over_cap");
    CHECK_THROWS_WITH(turn::HostedSession::Open(file, PackHostData({{"system", longText}}), settings, 42, backend),
                      "prompts/system: 125 tokens, over engine_rules_cap 32");
    CHECK_FALSE(std::filesystem::exists(file));
    // A pack fault is refused before the file too.
    CHECK_THROWS_WITH(turn::HostedSession::Open(file, PackHostData({{"speak", "{who}"}}), test::HostedSettings(), 42,
                                                backend),
                      "prompts/speak: unknown variable {who}; this text takes {speaker}, {player}");
    CHECK_FALSE(std::filesystem::exists(file));
}

TEST_CASE("Reopening with a changed pack replaces the stored host data; the next prompt shows the new text",
          "[hosted][prompts]")
{
    test::ScriptedBackend backend(16384, PackResponder());
    const std::filesystem::path file = test::HostedRunFile("hosted_pack_reopen");
    std::string id;
    {
        turn::HostedSession session =
            turn::HostedSession::Open(file, PackHostData(CustomPack()), test::HostedSettings(), 42, backend);
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}));
    }
    std::map<std::string, std::string> pack = CustomPack();
    pack["resolve"] = "PACK RESOLVE TWO: {speaker} acts.";
    const turn::HostData changed = PackHostData(pack);
    turn::HostedSession session = turn::HostedSession::Open(file, changed, test::HostedSettings(), 42, backend);
    CHECK(session.Meta().worldHash == turn::HostDataHash(changed));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Again."), {}, {}));
    CHECK(session.LastAssembly().prompt.messages.back().content.ends_with("# Task\nPACK RESOLVE TWO: Hulda acts."));
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --preset tests && cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[hosted][prompts]"`
Expected: the first three cases fail (the prompts show the default texts), the over-cap case fails (no error: the default path ignores the host's system text), the reopen case fails on the tail.

- [ ] **Step 3: Implement**

In `src/turn/HostedSession.cpp`:

1. Include `"turn/PromptPack.h"`. Delete `SPEAKER_VARIABLE`, `PLAYER_VARIABLE`, `ACTOR_VARIABLE` and the `FillVariables` function with its comment.

2. `Open`: after the summary_cap/tail_allowance check and before `const nlohmann::json stored = HostDataToJson(data);`:

```cpp
    // The system text, the host's or the default, must fit engine_rules_cap: it is the host's data now, so over the
    // cap is its DataError, found before the file is touched.
    const int systemTokens = backend.CountTokens(world.Prompts().system);
    if (systemTokens > settings.budget.engineRulesCap) {
        throw core::DataError(std::format("prompts/system: {} tokens, over engine_rules_cap {}", systemTokens,
                                          settings.budget.engineRulesCap));
    }
```

3. `RunTurn`, where the instructions are filled:

```cpp
    // The instructions name the speaker and the player (the run's pack). The speak instruction must fit
    // narrate_allowance, which the prompt reserves for it; the names come from the host.
    const core::Actor& player = observation.Player();
    const PromptVariables names{.speaker = pSpeaker->name, .player = player.name};
    const std::string resolveInstruction = PromptPack::Fill(_world.Prompts().resolve, names);
    const std::string speakMessage = PromptPack::Fill(_world.Prompts().speak, names);
```

4. `Entries`, the cue:

```cpp
            if (user.empty()) {
                user = PromptPack::Fill(_world.Prompts().turnCue, {.speaker = speaker->name, .player = player->name});
            }
```

5. `Pass`: `inputs.systemText = _world.Prompts().system;`

6. `Upkeep`: replace the `fill` lambda and its comment with:

```cpp
    // {actor} in the pack's fold and condense texts is the actor's name.
    auto fill = [&name](std::string_view text) { return PromptPack::Fill(text, {.actor = name}); };
```

and `scope.foldInstruction = fill(_world.Prompts().summarize);`, `instruction = fill(_world.Prompts().condense)` in the condense lambda's capture.

In `src/turn/HostedSession.h`: `Open`'s comment, after the settings check sentence, gains: "Then the system text (the run's pack's) is counted with `backend`: over engine_rules_cap is a DataError "prompts/system: <n> tokens, over engine_rules_cap <cap>"." `Entries`' comment: "HOSTED_TURN_CUE" becomes "the pack's turn cue".

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"`, then `ctest --preset tests`
Expected: all pass, including the existing `{actor} the Bold` test (`HostedMemoryTests.cpp`). `Open` now makes one `CountTokens` call before it touches the file: with the llama-server backend that is a `/tokenize` request. A test that pins the requests `open` makes, or the message of an unreachable backend at open, may need its expectation updated; update it to the new first request and name each such test in the report (do not change `Open`'s order to avoid it).

- [ ] **Step 5: Commit**

```bash
git add src/turn/HostedSession.h src/turn/HostedSession.cpp tests/hosted/HostedPromptPackTests.cpp
git commit -m "HostedSession: every hosted text from the run's pack; a system text over engine_rules_cap fails the open"
```

---

### Task 4: Better default texts

**Files:**
- Modify: `src/turn/EngineText.h` (`HOSTED_SYSTEM_TEXT`, `HOSTED_SPEAK_INSTRUCTION`)
- Test: `tests/turn/PromptPackTests.cpp`, `tests/context/HostedPromptTests.cpp`, `tests/hosted/HostedTurnTests.cpp`

**Interfaces:** none new.

- [ ] **Step 1: Write the failing test**

Append to `tests/turn/PromptPackTests.cpp` (add `#include <string_view>`):

```cpp
TEST_CASE("The default texts do not invite naming the player and say most turns need no action", "[prompts]")
{
    CHECK(std::string_view(turn::HOSTED_SPEAK_INSTRUCTION) ==
          "Step 2: write what {speaker} says now. Spoken words only, in character.");
    CHECK(std::string_view(turn::HOSTED_SYSTEM_TEXT)
              .find("Choose what the speaker would do now. Most turns need no action: when the speaker would only talk, "
                    "choose the action that does nothing, if the turn allows one. The host decides whether it happens") !=
          std::string_view::npos);
}
```

Update the existing expectations to the new speak text:
- `tests/hosted/HostedTurnTests.cpp`, "A hosted turn resolves, adjudicates, speaks streamed and commits its record": the speak message check becomes `CHECK(speak.messages.back().content == "Step 2: write what Hulda says now. Spoken words only, in character.");`
- `tests/context/HostedPromptTests.cpp`, "The default hosted texts are generic, fit their cap and use only the engine's variables": delete the line `CHECK(turn::HOSTED_SPEAK_INSTRUCTION.find("{player}") != std::string_view::npos);` (the speak instruction no longer names the player; `[prompts]` checks every default's variables).

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[prompts],[hosted][turn]"`
Expected: the new case and the HostedTurnTests case fail.

- [ ] **Step 3: Change the defaults**

In `src/turn/EngineText.h`:
- `HOSTED_SPEAK_INSTRUCTION` becomes `"Step 2: write what {speaker} says now. Spoken words only, in character."`.
- In `HOSTED_SYSTEM_TEXT`'s "Step 1, act." paragraph, after `Choose what the speaker would do now.` insert ` Most turns need no action: when the speaker would only talk, choose the action that does nothing, if the turn allows one.` so the paragraph reads: `Step 1, act. Reply with one JSON object and nothing else: "action" is one of the actions the turn allows, and "params" holds that action's parameters. A parameter that names an actor takes the actor's id from the observation. Choose what the speaker would do now. Most turns need no action: when the speaker would only talk, choose the action that does nothing, if the turn allows one. The host decides whether it happens; its outcome arrives later as an event.`

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: the whole suite passes. If another test pins the old texts, update its expectation to the new text and name it in the report.

- [ ] **Step 5: Commit**

```bash
git add src/turn/EngineText.h tests/turn/PromptPackTests.cpp tests/context/HostedPromptTests.cpp tests/hosted/HostedTurnTests.cpp
git commit -m "Default hosted texts: the speak instruction stops naming the player; most turns need no action"
```

---

### Task 5: The server, the fixture and the docs

**Files:**
- Modify: `src/server/Api.h` (`API_VERSION`), `src/server/Api.cpp` (the defaults route)
- Modify: `tests/hosted/HostedTestSupport.h` (`PlayMemoryFixture`), `tests/server/ServerTestSupport.h` (`PlayMemoryFixtureOverHttp`)
- Create: `tests/hosted/fixtures/prompt_pack.json`, `tests/server/ApiPromptTests.cpp`
- Modify tests: `tests/hosted/HostedFixtureTests.cpp`, `tests/server/HttpMemoryTests.cpp`, `tests/llama/LlamaHostedTests.cpp`, every test pinning `"1.2.0"` (`tests/server/ApiMemoryTests.cpp`, `ApiRewindTests.cpp`, `ApiTests.cpp`, `ApiTurnTests.cpp`, `HttpServerTests.cpp`)
- Docs: `README.md`, `CHANGELOG.md`, `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`, `docs/superpowers/specs/2026-11-16-hosted-world-design.md`, `tests/hosted/fixtures/README.md`, `CLAUDE.md`

**Interfaces:**
- Consumes: `PROMPT_TEXTS`, `PromptPack::Text` (Task 1), `HostData::prompts` (Task 2).
- Produces: `GET /v1/prompts/default` -> `{"prompts": {<key>: <text>}, "variables": {<key>: [names]}, "api_version": "1.3.0"}`; a memory fixture's optional `"prompts"`.

- [ ] **Step 1: Write the failing tests**

Create `tests/server/ApiPromptTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/Api.h"
#include "server/ServerTestSupport.h"
#include "turn/PromptPack.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("The defaults route answers every default text and the variables it takes", "[hosted-server][api][prompts]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_prompts_default", script.Responder());
    const server::ApiResponse response = harness.Get("/v1/prompts/default");
    CHECK(response.status == 200);
    CHECK(response.body.at("api_version") == "1.3.0");
    const turn::PromptPack defaults;
    const nlohmann::json& prompts = response.body.at("prompts");
    CHECK(prompts.size() == 6);
    for (const turn::PromptText& text : turn::PROMPT_TEXTS) {
        INFO(text.key);
        CHECK(prompts.at(std::string(text.key)) == defaults.Text(text.key));
    }
    CHECK(response.body.at("variables") == nlohmann::json{{"system", nlohmann::json::array()},
                                                          {"resolve", {"speaker", "player"}},
                                                          {"speak", {"speaker", "player"}},
                                                          {"turn_cue", {"speaker", "player"}},
                                                          {"summarize", {"actor"}},
                                                          {"condense", {"actor"}}});
}

TEST_CASE("Open takes a pack: the next turn's prompt shows it; a bad pack is a 400 naming the text and the variable",
          "[hosted-server][api][prompts]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"none","params":{}})"};
    script.lines = {"Welcome."};
    test::ApiHarness harness("api_prompts_open", script.Responder());

    nlohmann::json bad = test::ReadFixture("open.json");
    bad["prompts"] = {{"speak", "Say it to {location}."}};
    const server::ApiResponse refused = harness.Post("/v1/runs/save_1/open", bad);
    CHECK(refused.status == 400);
    CHECK_THAT(refused.body.at("error").at("message").get<std::string>(),
               ContainsSubstring("prompts/speak: unknown variable {location}; this text takes {speaker}, {player}"));

    nlohmann::json open = test::ReadFixture("open.json");
    open["prompts"] = {{"system", "# Pack\nYou voice a test scene."}, {"resolve", "PACK: {speaker} acts."}};
    REQUIRE(harness.Post("/v1/runs/save_1/open", open).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    test::ReadAll(*harness.Post("/v1/runs/save_1/conversations/c1/turn",
                                {{"speaker", test::HULDA}, {"player_text", "Hi."}, {"observation", test::FixtureObservation()}})
                       .stream);
    // The run's backend is the last one made (the refused open may have made one before its host data failed).
    REQUIRE_FALSE(harness.backends.empty());
    const std::vector<test::ScriptedBackend::Call>& calls = harness.backends.back()->Calls();
    REQUIRE_FALSE(calls.empty());
    CHECK(calls[0].prompt.messages[0].content.starts_with("# Pack\nYou voice a test scene."));
    CHECK(calls[0].prompt.messages.back().content.ends_with("# Task\nPACK: Hulda acts."));
}
```

Create `tests/hosted/fixtures/prompt_pack.json`:

```json
{
  "description": "R7's acceptance: a host pack replaces the system text, the resolve instruction, the speak instruction and the turn cue; every resolve prompt shows the pack's texts and none of the replaced defaults. The fold and condense texts stay the engine's.",
  "prompts": {
    "system": "# Voices\nYou voice the people of a tavern scene that a host game runs. Step 1: reply with one JSON object choosing what the speaker tries; most turns need the action that does nothing. Step 2: write only the speaker's spoken words, one to three sentences, in character.",
    "resolve": "Step 1 for {speaker}: the JSON object only.",
    "speak": "Step 2: {speaker} speaks now. Spoken words only.",
    "turn_cue": "({speaker} goes on talking.)"
  },
  "observation": {
    "game_time": {"days_passed": 213.42, "hour": 14},
    "location": "Whiterun, The Bannered Mare",
    "actors": [
      {"id": "0001A6A4", "name": "Hulda", "is_player": false, "bio": "Owner of the Bannered Mare."},
      {"id": "0001B07C", "name": "Saadia", "is_player": false, "bio": "A Redguard working at the inn."},
      {"id": "00000007", "name": "Aela", "is_player": true}
    ]
  },
  "participants": ["0001A6A4", "0001B07C", "00000007"],
  "steps": [
    {"turn": {"speaker": "0001A6A4", "player_text": "Good evening."},
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Welcome, traveller."},
     "expect": {"prompt_contains": ["# Voices\nYou voice the people of a tavern scene", "Step 1 for Hulda: the JSON object only."],
                "prompt_absent": ["You voice the characters of a role-playing game", "Reply with the JSON object only."]}},
    {"turn": {"speaker": "0001A6A4"},
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "The stew is fresh today."}},
    {"turn": {"speaker": "0001B07C", "player_text": "And you, Saadia?"},
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "I keep the rooms clean."},
     "expect": {"prompt_contains": ["(Hulda goes on talking.)", "Step 1 for Saadia: the JSON object only."],
                "prompt_absent": ["(Hulda speaks.)", "You voice the characters of a role-playing game"]}},
    {"end": true},
    {"maintain": true},
    {"memories": {"actor": "0001A6A4", "contains": ["stew is fresh"]}}
  ]
}
```

Add the cases:
- `tests/hosted/HostedFixtureTests.cpp`:
  ```cpp
  TEST_CASE("The prompt-pack fixture: every resolve prompt shows the host's texts", "[hosted][fixtures][prompts]")
  {
      test::PlayMemoryFixture("prompt_pack.json");
  }
  ```
- `tests/server/HttpMemoryTests.cpp`:
  ```cpp
  TEST_CASE("The prompt-pack fixture plays over HTTP", "[hosted-server][http][prompts][fixtures]")
  {
      const nlohmann::json fixture = test::ReadFixture("prompt_pack.json");
      test::HostedScript script = test::ScriptOf(fixture);
      test::ApiHarness harness("http_prompt_pack_fixture", test::MemoryEchoResponder(script));
      server::HttpServer http(harness.api);
      http.Start(0);
      httplib::Client client("127.0.0.1", http.Port());
      test::PlayMemoryFixtureOverHttp(client, "r", fixture, true);
      harness.runs.StopAll();
      http.Stop();
  }
  ```
- `tests/llama/LlamaHostedTests.cpp`, after the reload case:
  ```cpp
  TEST_CASE("The prompt-pack fixture on the real model: the host's texts replace the defaults", "[llama][hosted][prompts]")
  {
      llama::LlamaCppBackend& backend = test::SharedBackend();
      // As the tests above: budgets that fit the shared backend's context of 4096, greedy sampling.
      core::Settings settings = test::HostedSettings();
      settings.contextSize = backend.ContextSize();
      settings.budget = {96, 160, 128, 1024, 1024, 1024, 512, 256};
      settings.resolveSampling = {0.0, 40, 0.95};
      settings.narrateSampling = {0.0, 40, 0.95};
      // The prompts' texts never depend on the model's words: every prompt_absent is checked.
      test::PlayMemoryFixture("prompt_pack.json", backend, settings, false);
  }
  ```

Change every `"1.2.0"` in `tests/server/*.cpp` to `"1.3.0"`.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --preset tests && cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[prompts]"`
Expected: the defaults route answers 404; the fixture cases fail (`PlayMemoryFixture` ignores `prompts`, so the default texts show); the version checks fail.

- [ ] **Step 3: Implement**

`src/server/Api.h`: `inline constexpr std::string_view API_VERSION = "1.3.0";`

`src/server/Api.cpp`: include `"turn/PromptPack.h"`; after the `health` branch:

```cpp
        } else if (get && route({"v1", "prompts", "default"})) {
            // The engine's default texts and the variables each takes, for a host writing its own pack.
            const turn::PromptPack defaults;
            nlohmann::json prompts = nlohmann::json::object();
            nlohmann::json variables = nlohmann::json::object();
            for (const turn::PromptText& text : turn::PROMPT_TEXTS) {
                const std::string key(text.key);
                prompts[key] = defaults.Text(text.key);
                variables[key] = nlohmann::json::array();
                for (const std::string_view name : text.variables) {
                    if (!name.empty()) {
                        variables[key].push_back(std::string(name));
                    }
                }
            }
            response.body = {{"prompts", std::move(prompts)}, {"variables", std::move(variables)}};
```

`tests/hosted/HostedTestSupport.h`, `PlayMemoryFixture`, after the clock (add `#include <map>`):

```cpp
    // A fixture with "prompts" sends that pack with the host data.
    if (fixture.contains("prompts")) {
        data.prompts = fixture.at("prompts").get<std::map<std::string, std::string>>();
    }
```

`tests/server/ServerTestSupport.h`, `PlayMemoryFixtureOverHttp`, after the clock:

```cpp
    // A fixture with "prompts" sends that pack with the host data.
    if (fixture.contains("prompts")) {
        hostData["prompts"] = fixture.at("prompts");
    }
```

and update both functions' comments ("A fixture with a "clock" ..." gains "and with "prompts", the host data's pack").

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: the whole suite passes.

- [ ] **Step 5: Docs**

- `CHANGELOG.md`, above `## API 1.2.0`:
  ```markdown
  ## API 1.3.0

  The host's prompt pack (design: `docs/superpowers/specs/2026-11-16-prompt-pack-design.md`). Only additions; two default texts changed.

  - The host data may carry `prompts`: any of `system`, `resolve`, `speak`, `turn_cue`, `summarize`, `condense`, each a
    text that replaces the engine's default for the run. Variables: `system` none; `resolve`, `speak`, `turn_cue`
    `{speaker}` and `{player}`; `summarize` and `condense` `{actor}`. An unknown key, an empty text, a variable the text
    does not take, or a system text over `engine_rules_cap` fails the `open` with 400 naming `prompts/<key>`.
  - `GET /v1/prompts/default` answers `{prompts, variables}`: the engine's default texts and the variables each takes.
  - The default speak instruction no longer names the player, and the default system text says most turns need no
    action.
  ```
- `README.md`, the `morpheus_server` route table: the `open` row's body becomes `{catalog, observation_schema, lore?, clock?, prompts?, seed?}`; add a row `| GET /v1/prompts/default | | {prompts, variables}: the default texts and their variables |`; after the Rewind paragraph add a **Prompt pack.** paragraph: what `prompts` holds (the six keys, what each text is, its variables), that a key left out keeps the engine's default, the `{name}` variable rule (other braces are literal), the errors at open, and that a changed pack is changed host data (the next prompt uses it). Link the spec.
- `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`: the `open` row gains `prompts?`; add the defaults route row ("added in 1.3.0 (F)"); the `api_version` line says 1.3.0; the CHANGELOG bullet in "Documentation" gains "API 1.3.0 (F, `2026-11-16-prompt-pack-design.md`) adds the host data's optional `prompts` and the defaults route."
- `docs/superpowers/specs/2026-11-16-hosted-world-design.md`, "Default hosted texts": replace "Sub-project F makes them replaceable by the host." with "Sub-project F made them replaceable by the host (`2026-11-16-prompt-pack-design.md`)."
- `tests/hosted/fixtures/README.md`: the memory fixtures heading lists `prompt_pack.json`; the format line gains `"prompts"`: "(optional) the host data's pack (`open.json`'s host data with these `prompts`)".
- `CLAUDE.md`, Layout, src/turn: "hosted runs, a world the host owns and sends as data (`HostedWorld`, `HostedSession`)" becomes "... (`HostedWorld`, `HostedSession`, and `PromptPack`, the host's texts over the engine's defaults)".

ASCII only; check with `python -c "import sys;d=open(sys.argv[1],'rb').read();print(sum(c>127 for c in d))" <file>` on every changed file (expected 0).

- [ ] **Step 6: Commit**

```bash
git add src/server/Api.h src/server/Api.cpp tests/hosted/HostedTestSupport.h tests/server/ServerTestSupport.h tests/hosted/fixtures/prompt_pack.json tests/server/ApiPromptTests.cpp tests/hosted/HostedFixtureTests.cpp tests/server/HttpMemoryTests.cpp tests/llama/LlamaHostedTests.cpp tests/server/ApiMemoryTests.cpp tests/server/ApiRewindTests.cpp tests/server/ApiTests.cpp tests/server/ApiTurnTests.cpp tests/server/HttpServerTests.cpp README.md CHANGELOG.md docs/superpowers/specs/2026-11-16-morpheus-server-design.md docs/superpowers/specs/2026-11-16-hosted-world-design.md tests/hosted/fixtures/README.md CLAUDE.md
git commit -m "Server: the pack through open, the defaults route, API 1.3.0; the prompt-pack fixture; docs"
```

---

## Finally (controller)

- `ctest --preset tests`; `cmake --build --preset asan` and `ctest --preset asan`.
- `"[turn]"`, `"[replay]"`, `"[context]"` pass (Morpheus's own game).
- `cmake --build --preset dev` builds clean; the consumer check (`cmake/consumer-check/`) is ok.
- Live, on the new defaults: `[llama][hosted]` in-process, and `[server]` over llama-server (README, "The [server] tests").
