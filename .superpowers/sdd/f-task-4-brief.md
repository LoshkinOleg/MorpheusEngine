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

