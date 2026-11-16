### Task 1: The ingredients: the pack's `decide` text, the tail's candidates, the reserved id

**Files:**
- Modify: `src/turn/EngineText.h`, `src/turn/PromptPack.h`, `src/turn/PromptPack.cpp`, `src/context/PromptBuilder.h`, `src/context/PromptBuilder.cpp`, `src/turn/HostedWorld.h`, `src/turn/HostedWorld.cpp`
- Test: `tests/turn/PromptPackTests.cpp`, `tests/context/HostedPromptTests.cpp`, `tests/hosted/HostedWorldTests.cpp`, `tests/server/ApiPromptTests.cpp`

**Interfaces:**
- Produces: `turn::HOSTED_DECIDE_INSTRUCTION`; `turn::PromptPack::decide` (`std::string`); `PROMPT_TEXTS` now 7 entries (`decide` after `turn_cue`, variables `{"player", ""}`); `context::HostedInputs::candidates` (`std::vector<std::string>`, each `"<name> (<id>)"`); `inline constexpr std::string_view turn::DECIDE_SPEAKER = "decide";` in `src/turn/HostedWorld.h`.

- [ ] **Step 1: Write the failing tests**

`tests/turn/PromptPackTests.cpp`:
- In "A prompt pack defaults to the engine's hosted texts", add `CHECK(pack.decide == std::string(turn::HOSTED_DECIDE_INSTRUCTION));` after the `turnCue` check and `CHECK(&pack.Text("decide") == &pack.decide);` after the `turn_cue` Text check.
- In "A host's texts replace the defaults they name and leave the others", add `CHECK(pack.decide == std::string(turn::HOSTED_DECIDE_INSTRUCTION));`.
- In "A pack's faults name the text and the variable", every expected `the pack's texts are system, resolve, speak, turn_cue, summarize, condense` becomes `the pack's texts are system, resolve, speak, turn_cue, decide, summarize, condense`, and add:
  ```cpp
      CHECK(PackError({{"decide", "{speaker} or {actor}?"}}) ==
            "prompts/decide: unknown variable {speaker}; this text takes {player}");
  ```
- Append:
  ```cpp
  TEST_CASE("The default decide text asks for the next speaker", "[prompts][decide]")
  {
      CHECK(std::string(turn::HOSTED_DECIDE_INSTRUCTION) ==
            "Choose who speaks next: the character listed above who would most naturally speak now. Reply with the "
            "JSON object only.");
      CHECK(turn::PromptPack::FromHost({{"decide", "Who answers {player}?"}}).decide == "Who answers {player}?");
  }
  ```

`tests/server/ApiPromptTests.cpp`, "The defaults route answers every default text and the variables it takes": `prompts.size() == 6` becomes `7`, and the expected variables gain `{"decide", {"player"}}` (after `turn_cue`).

`tests/context/HostedPromptTests.cpp`, append:

```cpp
TEST_CASE("A hosted tail lists the speaker candidates before the task", "[context][hosted]")
{
    test::FakeBackend backend(8192);
    context::HostedInputs inputs = Inputs();
    inputs.candidates = {"Hulda (0001A6A4)", "Saadia (0001B07C)"};
    const context::Assembly assembly = context::AssembleHostedPrompt(inputs, RoomyBudget(), backend);
    CHECK(assembly.prompt.messages.back().content.ends_with(
        "# Player\nAela: Follow me.\n\n# Who may speak\n- Hulda (0001A6A4)\n- Saadia (0001B07C)\n\n# Task\nTASK"));
    // Without candidates the tail has no such section.
    CHECK_THAT(context::AssembleHostedPrompt(Inputs(), RoomyBudget(), backend).prompt.messages.back().content,
               !ContainsSubstring("# Who may speak"));
}
```

`tests/hosted/HostedWorldTests.cpp`, append:

```cpp
TEST_CASE("Observe refuses an actor whose id is the reserved 'decide'", "[hosted][decide]")
{
    const turn::HostedWorld world(WithOpenSchema());
    nlohmann::json observation = test::FixtureObservation();
    observation["actors"][1]["id"] = "decide";
    CHECK(ObserveError(world, observation) ==
          "observation at /actors/1/id: 'decide' is reserved: it asks the engine to choose the speaker");
    CHECK(std::string(turn::DECIDE_SPEAKER) == "decide");
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: compile errors: no `HOSTED_DECIDE_INSTRUCTION`, `PromptPack::decide`, `HostedInputs::candidates`, `DECIDE_SPEAKER`.

- [ ] **Step 3: Implement**

`src/turn/EngineText.h`, after `HOSTED_TURN_CUE`:

```cpp
// PromptPack's default `decide`: the task of a "decide" turn's decide call (decide-speaker spec, section 3), after the
// tail's list of who may speak. {player} is the player's name.
inline constexpr std::string_view HOSTED_DECIDE_INSTRUCTION =
    "Choose who speaks next: the character listed above who would most naturally speak now. Reply with the JSON "
    "object only.";
```

`src/turn/PromptPack.h`: `PROMPT_TEXTS` becomes `std::array<PromptText, 7>` with `{"decide", {"player", ""}},` after the `turn_cue` entry; the class gains, after `turnCue`:

```cpp
    std::string decide = std::string(HOSTED_DECIDE_INSTRUCTION);
```

`src/turn/PromptPack.cpp`: `MEMBERS` becomes `std::array<std::string PromptPack::*, 7>` with `&PromptPack::decide` after `&PromptPack::turnCue` (index-aligned with `PROMPT_TEXTS`).

`src/context/PromptBuilder.h`, `HostedInputs`, after `playerLine`:

```cpp
    std::vector<std::string> candidates = {};        // Tail: who may speak, "<name> (<id>)" each (a decide call).
```

and `AssembleHostedPrompt`'s comment gains "the speaker candidates under `# Who may speak` (a decide call)," before "the instruction".

`src/context/PromptBuilder.cpp`, `AssembleHostedPrompt`, between the player block and `tail += "\n# Task\n" + inputs.instruction;`:

```cpp
    // A decide call lists who may speak, before its task.
    if (!inputs.candidates.empty()) {
        tail += "\n# Who may speak\n";
        for (const std::string& candidate : inputs.candidates) {
            tail += "- " + candidate + "\n";
        }
    }
```

`src/turn/HostedWorld.h`, before `struct HostData`:

```cpp
// The speaker a turn request names to have the engine choose one of the conversation's NPCs (decide-speaker spec). No
// actor may take it as its id.
inline constexpr std::string_view DECIDE_SPEAKER = "decide";
```

and `Observe`'s comment gains: "An actor whose id is DECIDE_SPEAKER is a fault "'decide' is reserved: ..." at its id."

`src/turn/HostedWorld.cpp`, `Observe`, right after `core::ActorsCore read = core::ReadActors(observation);`:

```cpp
    // No actor may take the id that asks the engine to choose the speaker.
    if (observation.is_object() && observation.contains("actors") && observation.at("actors").is_array()) {
        const nlohmann::json& actors = observation.at("actors");
        for (std::size_t i = 0; i < actors.size(); ++i) {
            const bool reserved = actors[i].is_object() && actors[i].contains("id") && actors[i].at("id").is_string() &&
                                  actors[i].at("id").get_ref<const std::string&>() == DECIDE_SPEAKER;
            if (reserved) {
                read.faults.push_back({std::format("/actors/{}/id", i),
                                       "'decide' is reserved: it asks the engine to choose the speaker"});
            }
        }
    }
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[prompts]"`, then `ctest --preset tests`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/turn/EngineText.h src/turn/PromptPack.h src/turn/PromptPack.cpp src/context/PromptBuilder.h src/context/PromptBuilder.cpp src/turn/HostedWorld.h src/turn/HostedWorld.cpp tests/turn/PromptPackTests.cpp tests/context/HostedPromptTests.cpp tests/hosted/HostedWorldTests.cpp tests/server/ApiPromptTests.cpp
git commit -m "Decide ingredients: the pack's decide text, the tail's speaker candidates, the reserved actor id"
```

---

