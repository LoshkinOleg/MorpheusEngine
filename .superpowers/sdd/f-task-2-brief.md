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

