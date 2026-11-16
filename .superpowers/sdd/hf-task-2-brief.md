### Task 2: Lore scopes and conditions: the data

**Files:** `src/core/World.h` (and `World.cpp` for `Holds`), `src/core/HostedRecord.h`, `src/core/HostedRecord.cpp` (`ReadActors`), `src/turn/HostedWorld.cpp` (`HostDataToJson`, `HostDataFromJson`, the constructor's lore checks), `src/turn/HostedWorld.h` (comments); tests `tests/core/LoreConditionTests.cpp` (new), `tests/hosted/HostedWorldTests.cpp`.

- [ ] **Step 1: Tests first.**
  - `LoreCondition::Holds` (`[lore]`): `{"/quests/MG03", min 40}` holds for 40 and 55 and not for 30; `max` inclusive; `min` and `max` both; `equals` with a string, a number and an object; a missing pointer or a string where `min` is given does not hold.
  - Host data (`[hosted][lore]`): an entry with `scopes` and `requires` round-trips through `HostDataFromJson`/`HostDataToJson` exactly. Host data without them keeps the pinned hash `35856ce76ac0af2e` (the existing pin test). Faults name their pointer: `/lore/<i>/scopes` not an array of non-empty strings; `/lore/<i>/requires` not an object or with an unknown key; `/lore/<i>/requires/pointer` not a string or not a JSON pointer (the pointer check may live in HostedWorld like the clock's); no comparison; a non-number `min` or `max`; an always-on entry with scopes (`lore at /<i>/scopes: an always-on entry is in every speaker's prompt: scopes apply to retrieved entries`).
  - Observe: an actor's `knows` is read into `Actor::knows`; a non-array or an empty string is a fault at `/actors/<i>/knows` (`'knows' must be an array of non-empty strings`).
- [ ] **Step 2:** Implement per the spec's section 2 "Data" and Global Constraints. Use the existing fault and DataError message styles of each reader (`host data at <pointer>: ...` in `HostDataFromJson`, `lore at /<i>/...` in the HostedWorld constructor, `observation at ...` in Observe). `LoreCondition::Holds` reads the value with `nlohmann::json::json_pointer` (`contains` first); a pointer that is not well-formed is refused at open, so `Holds` may treat a parse failure as a ContractError.
- [ ] **Step 3:** `cmake --build --preset tests --clean-first && ctest --preset tests`. Commit: "Host lore: scopes and requires on entries, knows on actors; read, written and checked".

---

