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

